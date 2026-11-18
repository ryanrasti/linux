#include <linux/stat.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/net.h>
#include <linux/task_work.h>
#include <linux/syscalls.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/sched/signal.h>
#include <linux/sched/stat.h>
#include <linux/platform_device.h>
#include <linux/fdtable.h>
#include <linux/tick.h>
#include <linux/sched/jobctl.h>
#include <linux/sched/task.h>
#include <linux/posix-timers.h>
#include <linux/rseq.h>
#include <linux/binfmts.h>
#include <asm/host_ops.h>
#include <asm/syscalls.h>
#include <asm/syscalls_32.h>
#include <asm/cpu.h>
#include <asm/sched.h>

static asmlinkage long sys_virtio_mmio_device_add(long base, long size,
						  unsigned int irq);

static asmlinkage long sys_new_thread_group_leader(void);

static asmlinkage long sys_new_thread(void);

static asmlinkage long sys_attach_thread(pid_t tid);

static asmlinkage long sys_new_exec(const char __user *name);

/* hostfd.c */
asmlinkage long sys_host_fd(int fd, unsigned int flags);
/* memblk.c */
asmlinkage long sys_memblk_add(unsigned long addr, unsigned long len);

typedef long (*syscall_handler_t)(long arg1, ...);

#undef __SYSCALL
#define __SYSCALL(nr, sym)[nr] = sym,

void *syscall_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] =  sys_ni_syscall,
#include <asm/unistd.h>

#if __BITS_PER_LONG == 32
#include <asm/unistd_32.h>
#endif
};

static long run_syscall(long no, long *params)
{
	long ret;
	syscall_handler_t handler;

	/* the number is the caller's: check it before it indexes the table */
	if (no < 0 || no >= __NR_syscalls)
		return -ENOSYS;
	handler = (syscall_handler_t)syscall_table[no];

	ret = handler(params[0], params[1], params[2], params[3], params[4],
		      params[5]);

	task_work_run();

	return ret;
}


#define CLONE_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_THREAD |	\
		     CLONE_SIGHAND | SIGCHLD)

static int host_task_id;
/*
 * init (PID 1): the task of the thread that started the kernel, which
 * makes the first program's syscalls, and of every call when the host has
 * no thread local storage.
 */
static struct task_struct *init_host_task;
/*
 * A thread of init's process that no host thread runs: the task host tasks
 * are cloned from (new_host_task), so that a host thread's task shares
 * init's files without borrowing init itself, whose own thread may be
 * waiting in a syscall.
 */
static struct task_struct *host0;

/*
 * A host task cloned from current (a user mode thread: a kernel thread,
 * PF_KTHREAD, isn't a program's), not yet woken: the scheduler can't run
 * it until a host thread takes it, so it stays TASK_NEW until then
 * (switch_to_host_task wakes it).
 */
static struct task_struct *clone_host_task(unsigned long flags)
{
	struct kernel_clone_args args = {
		.flags = (flags | CLONE_VM | CLONE_UNTRACED) & ~CSIGNAL,
		.exit_signal = flags & CSIGNAL,
		.fn = host_task_stub,
	};

	return copy_process(NULL, 0, NUMA_NO_NODE, &args);
}

/*
 * A host thread's task when its first call isn't attach_thread: a thread
 * of init's process (cloned from host0), or for new_thread_group_leader a
 * process of its own, init's child.
 */
static int new_host_task(struct task_struct **task, int new_tg_leader)
{
	unsigned long flags = CLONE_FLAGS;

	switch_to_host_task(host0);

	/* a process: its signal actions its own, a copy of init's */
	if (new_tg_leader)
		flags &= ~(CLONE_THREAD | CLONE_SIGHAND);

	*task = clone_host_task(flags);
	if (IS_ERR(*task))
		return PTR_ERR(*task);

	host_task_id++;

	snprintf((*task)->comm, sizeof((*task)->comm), "host%d", host_task_id);

	return 0;
}

/*
 * attach_thread(tid) as a host thread's first call: its task is tid, a
 * thread new_thread made and no host thread has taken yet.
 */
static int attach_host_task(struct task_struct **task, pid_t tid)
{
	struct task_struct *p;

	rcu_read_lock();
	p = find_task_by_pid_ns(tid, &init_pid_ns);
	rcu_read_unlock();
	if (!p)
		return -ESRCH;
	/* the CPU is held: nothing can take or start it before we do */
	if (READ_ONCE(p->__state) != TASK_NEW ||
	    !test_tsk_thread_flag(p, TIF_HOST_THREAD) ||
	    task_thread_info(p)->tid)
		return -EINVAL;

	*task = p;
	return 0;
}

static void exit_task(void)
{
	do_exit(0);
}

static void del_host_task(void *arg)
{
	struct task_struct *task = (struct task_struct *)arg;
	struct thread_info *ti = task_thread_info(task);

	/* init ends by its program's exit, not its first thread's */
	if (task == init_host_task)
		return;

	if (lkl_cpu_get() < 0)
		return;

	switch_to_host_task(task);
	host_task_id--;
	set_ti_thread_flag(ti, TIF_SCHED_JB);
	lkl_ops->jmp_buf_set(&ti->sched_jb, exit_task);
}

static struct lkl_tls_key *task_key;

/*
 * init's process has ended (its last thread but host0 exited): host0 is
 * left, only for the halt
 */
static bool init_ended;

/*
 * What a host task's exit (prev, TASK_DEAD, in __switch_to) tells the host:
 * if it was its process's last thread, the process's pid and wait status
 * (the group's exit code if an exit_group or a fatal signal ended it).
 * host0 isn't counted: it's a thread of init's process that no host thread
 * runs, so init's process ends with its other threads.
 */
void host_task_exit_status(struct task_struct *task, int *pid, int *status)
{
	struct signal_struct *sig = task->signal;

	*pid = 0;
	*status = 0;
	if (atomic_read(&sig->live) != (sig == host0->signal ? 1 : 0))
		return;
	*pid = task_tgid_nr(task);
	*status = sig->flags & SIGNAL_GROUP_EXIT ? sig->group_exit_code :
						   task->exit_code;
	if (sig == host0->signal)
		init_ended = true;
}

/*
 * The exited task's host thread, the CPU released: its task is gone (freed
 * once reaped), so its TLS forgets it, and the host ends the thread.
 */
void __noreturn host_task_exited(int pid, int status)
{
	if (lkl_ops->tls_set)
		lkl_ops->tls_set(task_key, NULL);
	if (lkl_ops->task_exit)
		lkl_ops->task_exit(pid, status);
	lkl_ops->thread_exit();
	for (;;)
		;
}

/*
 * init's process ends with this call, its exit_group or its last thread's
 * exit (host0 is the other live thread it always has): host0 drops its
 * files first, so that the exit closes them. host0 holds a reference, as
 * every host task cloned from it shares init's files.
 */
static void init_ending(struct task_struct *task, long no)
{
	struct files_struct *files = host0->files;

	if (task->signal != host0->signal || !files ||
	    !(no == __NR_exit_group ||
	      (no == __NR_exit && atomic_read(&task->signal->live) == 2)))
		return;
	task_lock(host0);
	host0->files = NULL;
	task_unlock(host0);
	put_files_struct(files);
}

/*
 * A fatal signal (another thread's exit_group, a SIGKILL) pending at a
 * syscall's start ends the task there, before the call runs: the signal
 * came while the task was outside the kernel. (At its end, signal
 * delivery does it.) Not host0, which the halt borrows once init's process
 * has ended.
 */
static void exit_if_killed(struct task_struct *task)
{
	if (task != host0 && fatal_signal_pending(task))
		do_group_exit(SIGKILL);
}

/*
 * Whether a syscall a signal interrupted runs again (its -ERESTART*
 * result; any other is its result), as an arch's signal delivery decides:
 * if not, it ends with -EINTR. ka: the handler about to run, or NULL.
 */
static bool syscall_restarts(long *ret, struct k_sigaction *ka)
{
	switch (*ret) {
	case -ERESTARTNOHAND:
	case -ERESTART_RESTARTBLOCK:
		if (!ka)
			return true;
		break;
	case -ERESTARTSYS:
		if (!ka || ka->sa.sa_flags & SA_RESTART)
			return true;
		break;
	case -ERESTARTNOINTR:
		return true;
	default:
		return false;
	}
	*ret = -EINTR;
	return false;
}

/*
 * A caught signal's handler, a function of the program's, run by the host
 * (signal_handler) on the task's host thread with the CPU released; then
 * sigreturn's work: the mask the host hands back (the ucontext's
 * uc_sigmask) and the alternate stack (uc_stack) restored. Without the op
 * the handler is skipped, as if it had returned at once. Returns whether
 * the interrupted call still restarts (the host's say), or -1 if the
 * kernel halted meanwhile (the CPU not taken back).
 */
static int run_signal_handler(struct task_struct *task, struct ksignal *ksig,
			      bool restart)
{
	struct lkl_signal s;
	siginfo_t info;
	sigset_t mask = *sigmask_to_save();
	stack_t stack;

	copy_siginfo_to_external(&info, &ksig->info);
	__save_altstack((stack_t __user *)&stack, 0);
	/* the handler's mask, SS_AUTODISARM's disarm */
	signal_setup_done(0, ksig, 0);

	s = (struct lkl_signal){
		.sig = ksig->sig,
		.restart = restart,
		.handler = (unsigned long)ksig->ka.sa.sa_handler,
		.flags = ksig->ka.sa.sa_flags,
		.info = &info,
		.mask = &mask,
		.stack = &stack,
	};
	if (lkl_ops->signal_handler) {
		lkl_cpu_put();
		if (!lkl_ops->signal_handler(&s))
			restart = false;
		if (lkl_cpu_get() < 0)
			return -1;
		switch_to_host_task(task);
	}

	/* sigreturn */
	sigdelsetmask(&mask, sigmask(SIGKILL) | sigmask(SIGSTOP));
	set_current_blocked(&mask);
	restore_altstack((const stack_t __user *)&stack);
	return restart;
}

/*
 * A syscall's return to the program, as an arch's (arch_do_signal_or_restart)
 * but without a signal frame: the task's pending signals are taken one at a
 * time (get_signal: the default actions, a fatal one ending the task there,
 * reported by task_exit; a stop stops it), a caught one's handler run by
 * the host (run_signal_handler), and the call restarted or ended with
 * -EINTR as SA_RESTART and its result say. Returns the call's result; false
 * in *cpu if the kernel halted while a handler ran.
 */
static long syscall_return(struct task_struct *task, long no, long *params,
			   long ret, bool *cpu)
{
	struct ksignal ksig;
	int restart;

	for (;;) {
		if (!signal_pending(task) || !get_signal(&ksig)) {
			restart = syscall_restarts(&ret, NULL);
			restore_saved_sigmask();
			if (!restart)
				return ret;
		} else {
			restart = syscall_restarts(&ret, &ksig.ka);
			restart = run_signal_handler(task, &ksig, restart);
			if (restart < 0) {
				*cpu = false;
				return -EINTR;
			}
			if (!restart) {
				if (ret <= -ERESTARTSYS && ret >= -ERESTART_RESTARTBLOCK)
					ret = -EINTR;
				continue;
			}
		}
		if (ret == -ERESTART_RESTARTBLOCK)
			no = __NR_restart_syscall;
		ret = run_syscall(no, params);
	}
}

long lkl_syscall(long no, long *params)
{
	struct task_struct *task = init_ended ? host0 : init_host_task;
	long ret;

	ret = lkl_cpu_get();
	if (ret < 0)
		return ret;

	if (lkl_ops->tls_get) {
		task = lkl_ops->tls_get(task_key);
		if (!task && init_ended) {
			/* no process to join: only the halt, as host0 */
			ret = -ESRCH;
			if (no != __NR_reboot)
				goto out;
			task = host0;
		} else if (!task) {
			if (no == __NR_attach_thread)
				ret = attach_host_task(&task, params[0]);
			else
				ret = new_host_task(&task,
						    no == __NR_new_thread_group_leader);
			if (ret)
				goto out;
			lkl_ops->tls_set(task_key, task);
		}
	}

	switch_to_host_task(task);

	exit_if_killed(task);
	init_ending(task, no);

	ret = run_syscall(no, params);

	if (no == __NR_reboot) {
		thread_sched_jb();
		return ret;
	}

	if (task != host0) {
		bool cpu = true;

		ret = syscall_return(task, no, params, ret, &cpu);
		if (!cpu)
			return ret;
	}

out:
	lkl_cpu_put();

	return ret;
}

/*
 * A fork's child, in the child's host process: its thread waits like any
 * new thread until the kernel first schedules the task, which is then the
 * thread's host task, and leaves the kernel as the clone's return in the
 * child (0, signals delivered); the host resumes the program there. If the
 * clone failed after the host's fork (copy_process undoing it), the thread
 * is woken dead (kill_thread) and ends.
 */
static void fork_bootstrap(void *arg)
{
	struct thread_info *ti = arg;
	struct task_struct *task = ti->task;
	bool cpu = true;

	lkl_ops->sem_down(ti->sched_sem);
	if (ti->dead) {
		lkl_ops->thread_exit();
		return;
	}

	set_ti_thread_flag(ti, TIF_HOST_THREAD);
	if (lkl_ops->tls_set)
		lkl_ops->tls_set(task_key, task);
	schedule_tail(ti->prev_sched);
	/*
	 * CLONE_CHILD_SETTID's write is the first schedule's: switch_to_host_task
	 * runs schedule_tail at every switch back to a host task, which would
	 * write the tid again for the task's life, into memory that, after an
	 * exec in the same address space, is the new program's (tier A: a
	 * version table in ld.so's heap, "undefined symbol ..., version
	 * GLIBC_2.38").
	 */
	task->set_child_tid = NULL;

	syscall_return(task, __NR_clone, NULL, 0, &cpu);
	if (cpu)
		lkl_cpu_put();
}

/*
 * copy_thread() for a program's clone, which makes a process (fork, vfork,
 * posix_spawn's clone; a thread is new_thread's): the host forks its
 * process, on the parent's host thread, the CPU held, and the child's
 * first thread runs fork_bootstrap. Until it does, it's a plain thread
 * (not TIF_HOST_THREAD), which kill_thread can end. Everything else is
 * clone's as usual: the child task, its files, signals, its parent, the
 * wait for a vfork, CLONE_*_SETTID.
 */
int copy_fork_thread(struct task_struct *p,
		     const struct kernel_clone_args *args)
{
	struct thread_info *ti = task_thread_info(p);

	if (args->flags & CLONE_THREAD)
		return -EINVAL;
	if (!lkl_ops->fork || !test_thread_flag(TIF_HOST_THREAD))
		return -ENOSYS;

	clear_ti_thread_flag(ti, TIF_HOST_THREAD);
	clear_ti_thread_flag(ti, TIF_SCHED_JB);
	ti->tid = lkl_ops->fork(fork_bootstrap, ti);

	return ti->tid ? 0 : -EAGAIN;
}

static struct task_struct *idle_host_task;

/*
 * Whether a task other than current is runnable, for a host task leaving
 * the kernel to let it run (lkl_cpu_put). Not counted: the idle host task,
 * which only stands for host tasks being outside the kernel, and the fair
 * server, a deadline entity counted in nr_running while it's queued. With
 * either counted, a host task making syscalls switched away and back at
 * every one (boot.c: 3 switches per getpid; 2.5x slower on Valkey).
 */
bool others_runnable(void)
{
	return nr_running_tasks(current != idle_host_task ?
				idle_host_task : NULL) > 1;
}

/*
 * called from idle, don't failed, don't block; false until the idle host
 * task exists (until boot is done)
 */
bool wakeup_idle_host_task(void)
{
	if (!idle_host_task)
		return false;
	if (!need_resched())
		wake_up_process(idle_host_task);
	return true;
}

/*
 * The kernel is idle while the CPU is free with the idle host task current:
 * the idle task's loop switched to it and ended, and its tick's idle
 * (NO_HZ_IDLE) with it, so the tick ran at HZ while nothing ran, waking the
 * host HZ times a second. The idle host task's idle is now the kernel's:
 * the tick stops until the next timer, as the idle loop's would, is
 * re-evaluated at each interrupt's end, and restarts when the CPU is taken.
 */
static bool host_idle;

/* the CPU released with the idle host task current: the kernel is idle */
void idle_host_task_idle(void)
{
	unsigned long flags;

	if (!idle_host_task || current != idle_host_task || irqs_disabled())
		return;
	if (!host_idle) {
		host_idle = true;
		tick_nohz_idle_enter();
	}
	local_irq_save(flags);
	tick_nohz_idle_stop_tick();
	local_irq_restore(flags);
}

/*
 * An interrupt while idle: jiffies brought up to date (irq_enter does it
 * only for the idle task)
 */
void idle_host_task_irq_enter(void)
{
	if (host_idle)
		tick_irq_enter();
}

/* an interrupt's end, idle: the tick stopped again, to the next timer */
void idle_host_task_irq_exit(void)
{
	if (host_idle && !need_resched()) {
		tick_nohz_irq_exit();
		tick_nohz_idle_stop_tick();
	}
}

/* the CPU taken from the idle kernel: by a host thread, or to schedule */
void idle_host_task_busy(void)
{
	if (!host_idle)
		return;
	host_idle = false;
	tick_nohz_idle_exit();
}

static int idle_host_task_loop(void *unused)
{
	struct thread_info *ti = task_thread_info(current);

	snprintf(current->comm, sizeof(current->comm), "idle_host_task");
	set_thread_flag(TIF_HOST_THREAD);
	idle_host_task = current;

	for (;;) {
		lkl_cpu_put();
		lkl_ops->sem_down(ti->sched_sem);
		if (idle_host_task == NULL) {
			lkl_ops->thread_exit();
			return 0;
		}
		schedule_tail(ti->prev_sched);
	}
}

static DECLARE_COMPLETION(host0_parked);

/* host0's own host thread: park the task, then end, leaving it to others */
static int host0_park(void *unused)
{
	snprintf(current->comm, sizeof(current->comm), "host0");
	set_thread_flag(TIF_HOST_THREAD);
	host0 = current;
	complete(&host0_parked);
	thread_sched_jb();
	lkl_ops->thread_exit();
	return 0;
}

/*
 * Called by init in its exec of /init (lkl_run_init), whose task is then
 * the one of the thread that started the kernel (syscalls_init_thread).
 * init is the first program's process: its signal actions stay the
 * defaults, and the idle host task is a kernel thread, not one of its.
 */
int syscalls_init(void)
{
	struct task_struct *idle;

	set_thread_flag(TIF_HOST_THREAD);
	init_host_task = current;
	/*
	 * The program is init, but a program as any other: a signal whose
	 * default action is fatal ends it (abort, a SIGTERM), as it would
	 * outside the sandbox. (init's end is the host's: task_exit(1, ...).)
	 */
	current->signal->flags &= ~SIGNAL_UNKILLABLE;

	if (lkl_ops->tls_alloc) {
		task_key = lkl_ops->tls_alloc(del_host_task);
		if (!task_key)
			return -1;
	}

	idle = kthread_run(idle_host_task_loop, NULL, "idle_host_task");
	if (IS_ERR(idle) || user_mode_thread(host0_park, NULL, CLONE_FLAGS) < 0) {
		if (lkl_ops->tls_free)
			lkl_ops->tls_free(task_key);
		return -1;
	}
	wait_for_completion(&host0_parked);

	return 0;
}

/* the thread that started the kernel: its task is init */
void syscalls_init_thread(void)
{
	if (lkl_ops->tls_set)
		lkl_ops->tls_set(task_key, init_host_task);
}

void syscalls_cleanup(void)
{
	if (idle_host_task) {
		struct thread_info *ti = task_thread_info(idle_host_task);

		idle_host_task = NULL;
		lkl_ops->sem_up(ti->sched_sem);
		lkl_ops->thread_join(ti->tid);
	}

	if (lkl_ops->tls_free)
		lkl_ops->tls_free(task_key);
}

SYSCALL_DEFINE3(virtio_mmio_device_add, long, base, long, size, unsigned int,
		irq)
{
	struct platform_device *pdev;
	int ret;

	struct resource res[] = {
		[0] = {
		       .start = base,
		       .end = base + size - 1,
		       .flags = IORESOURCE_MEM,
		       },
		[1] = {
		       .start = irq,
		       .end = irq,
		       .flags = IORESOURCE_IRQ,
		       },
	};

	pdev = platform_device_alloc("virtio-mmio", PLATFORM_DEVID_AUTO);
	if (!pdev) {
		dev_err(&pdev->dev, "%s: Unable to device alloc for virtio-mmio\n", __func__);
		return -ENOMEM;
	}

	ret = platform_device_add_resources(pdev, res, ARRAY_SIZE(res));
	if (ret) {
		dev_err(&pdev->dev, "%s: Unable to add resources for %s%d\n", __func__, pdev->name, pdev->id);
		goto exit_device_put;
	}

	ret = platform_device_add(pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "%s: Unable to add %s%d\n", __func__, pdev->name, pdev->id);
		goto exit_release_pdev;
	}

	return pdev->id;

exit_release_pdev:
	platform_device_del(pdev);
exit_device_put:
	platform_device_put(pdev);

	return ret;
}


SYSCALL_DEFINE0(new_thread_group_leader)
{
	// No-op here -- actual handling is done in lkl_syscall routine.
	if (current->tgid == current->pid)
		return 0;
	else
		return -1;
}

/*
 * A thread of the caller's process, as pthread_create's clone makes one
 * (sharing its memory, files, fs, signal actions and semaphore undos), for
 * another host thread to run: that thread's first call is attach_thread
 * with the tid this returns. Until then the thread doesn't run, but it is
 * one of the process's (signals to it wait). Made here, as the caller, it
 * can join any process, init's or not; a host thread can't clone a
 * process's thread itself, having no task of that process to clone from.
 */
SYSCALL_DEFINE0(new_thread)
{
	struct task_struct *p;

	p = clone_host_task(CLONE_FLAGS | CLONE_SYSVSEM);
	if (IS_ERR(p))
		return PTR_ERR(p);

	return task_pid_nr(p);
}

/*
 * Attaching is done in lkl_syscall, as a host thread's first call; here the
 * thread has a task already, which is tid or not.
 */
SYSCALL_DEFINE1(attach_thread, pid_t, tid)
{
	return task_pid_nr(current) == tid ? 0 : -EBUSY;
}

/*
 * An exec's other threads end (de_thread's work, which is static in
 * fs/exec.c): each is killed and waited for, the leader until it is a
 * zombie if the caller isn't it. Unlike de_thread, the caller doesn't take
 * the leader's pid (its tid stays its own: getpid is still the process's),
 * and host0 isn't one of them: in init's process it is the thread no host
 * thread runs, parked for good (a SIGKILL couldn't wake it, and the exec
 * would wait forever). A thread running outside the kernel dies at its
 * next syscall: the exec waits for it.
 */
static int exec_single_thread(struct task_struct *me)
{
	struct signal_struct *sig = me->signal;
	spinlock_t *lock = &me->sighand->siglock;
	struct task_struct *leader = me->group_leader, *t;
	int count = 0;

	spin_lock_irq(lock);
	if ((sig->flags & SIGNAL_GROUP_EXIT) || sig->group_exec_task) {
		spin_unlock_irq(lock);
		return -EAGAIN;
	}
	sig->group_exec_task = me;
	sig->group_stop_count = 0;
	for_other_threads(me, t) {
		if (t == host0)
			continue;
		task_clear_jobctl_pending(t, JOBCTL_PENDING_MASK);
		/*
		 * (a zombie counts, as zap_other_threads' does: it is waited
		 * for until released; a thread new_thread made that no host
		 * thread has taken yet doesn't: it can't run to die until
		 * then, and dies at attach_thread)
		 */
		if (t != leader && READ_ONCE(t->__state) != TASK_NEW)
			count++;
		if (t->exit_state)
			continue;
		sigaddset(&t->pending.signal, SIGKILL);
		signal_wake_up(t, 1);
	}

	sig->notify_count = count;
	while (sig->notify_count) {
		__set_current_state(TASK_KILLABLE);
		spin_unlock_irq(lock);
		schedule();
		if (__fatal_signal_pending(me))
			goto killed;
		spin_lock_irq(lock);
	}
	spin_unlock_irq(lock);

	/* exit_notify() wakes us when the leader is a zombie */
	while (leader != me) {
		write_lock_irq(&tasklist_lock);
		sig->notify_count = -1;
		if (leader->exit_state)
			break;
		__set_current_state(TASK_KILLABLE);
		write_unlock_irq(&tasklist_lock);
		schedule();
		if (__fatal_signal_pending(me))
			goto killed;
	}
	if (leader != me)
		write_unlock_irq(&tasklist_lock);

	sig->group_exec_task = NULL;
	sig->notify_count = 0;
	return 0;

killed:
	read_lock(&tasklist_lock);
	sig->group_exec_task = NULL;
	sig->notify_count = 0;
	read_unlock(&tasklist_lock);
	return -EAGAIN;
}

/*
 * The files table made the exec's own, as unshare_files() does, but in
 * init's process host0, a thread of it, keeps sharing it: dropped for the
 * unshare (else, holding a reference, it made it copy the table and kept
 * the old one, the pre-exec fds open for good: a pipe's reader never saw
 * EOF), then given the table again.
 */
static int exec_unshare_files(struct task_struct *me)
{
	struct files_struct *files = me->files;
	bool host0_shares = host0->files == files;
	int ret;

	if (host0_shares) {
		task_lock(host0);
		host0->files = NULL;
		task_unlock(host0);
		put_files_struct(files);
	}
	ret = unshare_files();
	if (host0_shares) {
		files = me->files;
		atomic_inc(&files->count);
		task_lock(host0);
		host0->files = files;
		task_unlock(host0);
	}
	return ret;
}

/*
 * new_exec(name): an exec's kernel half, the host having loaded the new
 * program (its memory is the host's, so exec_mmap and the binfmt are the
 * host's too). Of begin_new_exec() and bprm_execve(), what a program can
 * observe of a task that isn't its memory: the other threads killed (the
 * process single-threaded), the files unshared and close-on-exec ones
 * closed, the user pointers the kernel keeps for the task dropped
 * (set_child_tid, clear_child_tid, the robust futex list, rseq) and a
 * vfork parent released (exec_mm_release), the POSIX timers deleted, the
 * alternate signal stack cleared, caught signals' actions reset to the
 * default (ignored ones stay ignored), the name changed, self_exec_id
 * moved on. Kept, as Linux keeps them: the mask and pending signals, the
 * itimers and alarm, the pid, the cwd and the rest. Not done: credentials
 * (no setuid file), the personality, dumpability.
 */
SYSCALL_DEFINE1(new_exec, const char __user *, name)
{
	struct task_struct *me = current;
	char *path;
	long ret;

	/* the name as exec's: the path's last part */
	path = strndup_user(name, PATH_MAX);
	if (IS_ERR(path))
		return PTR_ERR(path);

	ret = exec_single_thread(me);
	if (!ret)
		ret = exec_unshare_files(me);
	if (ret) {
		kfree(path);
		return ret;
	}

	/*
	 * The old image's user pointers. clear_child_tid is dropped, not
	 * written: Linux writes 0 there if the old memory is still another
	 * task's (mm_users > 1), which here it always is (every host task
	 * shares init's stub mm), and the address may be the new program's
	 * by now. exec_mm_release walks the robust futex list (with FUTEX:
	 * in the old image, so before the host replaces it) and completes a
	 * vfork parent's wait.
	 */
	me->set_child_tid = NULL;
	me->clear_child_tid = NULL;
	exec_mm_release(me, me->mm);
	rseq_execve(me);

#ifdef CONFIG_POSIX_TIMERS
	spin_lock_irq(&me->sighand->siglock);
	posix_cpu_timers_exit(me);
	spin_unlock_irq(&me->sighand->siglock);
	exit_itimers(me);
	flush_itimer_signals();
#endif

	do_close_on_exec(me->files);
	me->sas_ss_sp = me->sas_ss_size = 0;
	__set_task_comm(me, kbasename(path), true);
	kfree(path);
	WRITE_ONCE(me->self_exec_id, me->self_exec_id + 1);
	flush_signal_handlers(me, 0);

	return 0;
}
