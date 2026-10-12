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
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/platform_device.h>
#include <asm/host_ops.h>
#include <asm/syscalls.h>
#include <asm/syscalls_32.h>
#include <asm/cpu.h>
#include <asm/sched.h>

#ifdef CONFIG_ARCH_HAS_SYSCALL_WRAPPER
typedef long (*syscall_handler_t)(const long *args);

static long __lkl_sys_ni_syscall(const long *args)
{
	return sys_ni_syscall();
}

/* indirect so table aliases in asm/syscalls.h expand before pasting */
#define __LKL_SYM(sym) __lkl_##sym

#undef __SYSCALL
#define __SYSCALL(nr, sym) long __LKL_SYM(sym)(const long *args);
#include <asm/unistd.h>
#if __BITS_PER_LONG == 32
#include <asm/unistd_32.h>
#endif

#undef __SYSCALL
#define __SYSCALL(nr, sym)[nr] = __LKL_SYM(sym),
#define __LKL_SYS_NI __lkl_sys_ni_syscall
#else
static asmlinkage long sys_virtio_mmio_device_add(long base, long size,
						  unsigned int irq);

static asmlinkage long sys_new_thread_group_leader(void);

typedef long (*syscall_handler_t)(long arg1, ...);

#undef __SYSCALL
#define __SYSCALL(nr, sym)[nr] = sym,
#define __LKL_SYS_NI sys_ni_syscall
#endif

void *syscall_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] =  __LKL_SYS_NI,
#include <asm/unistd.h>

#if __BITS_PER_LONG == 32
#include <asm/unistd_32.h>
#endif
};

static long run_syscall(long no, long *params)
{
	long ret;
	syscall_handler_t handler = (syscall_handler_t)syscall_table[no];

	if (no < 0 || no >= __NR_syscalls)
		return -ENOSYS;

#ifdef CONFIG_ARCH_HAS_SYSCALL_WRAPPER
	ret = handler(params);
#else
	ret = handler(params[0], params[1], params[2], params[3], params[4],
		      params[5]);
#endif

	task_work_run();

	return ret;
}

/*
 * A syscall by a host task, with what a real arch does on the way back to
 * user mode: signal delivery and syscall restart. The task may exit in it
 * (exit, a fatal signal); its final schedule() then comes back to jb (see
 * __switch_to()).
 */
struct host_syscall {
	long no;		/* NO_SYSCALL: only deliver signals */
	long *params;
	long ret;
	bool restart;		/* run the syscall again */
	bool handler;		/* run ksig's handler, then restore saved_mask */
	struct ksignal ksig;
	sigset_t saved_mask;
	struct lkl_jmp_buf jb;
};

#define NO_SYSCALL -1

struct lkl_jmp_buf *host_syscall_jb(struct host_syscall *sc)
{
	return &sc->jb;
}

/* ka: the handler about to run, if any */
static void prepare_restart(struct host_syscall *sc, struct k_sigaction *ka)
{
	switch (sc->ret) {
	case -ERESTARTNOHAND:
	case -ERESTART_RESTARTBLOCK:
		if (ka) {
			sc->ret = -EINTR;
			return;
		}
		break;
	case -ERESTARTSYS:
		if (ka && !(ka->sa.sa_flags & SA_RESTART)) {
			sc->ret = -EINTR;
			return;
		}
		break;
	case -ERESTARTNOINTR:
		break;
	default:
		return;
	}
	if (sc->ret == -ERESTART_RESTARTBLOCK)
		sc->no = __NR_restart_syscall;
	sc->restart = true;
}

static void host_syscall_body(void)
{
	struct host_syscall *sc = current_thread_info()->syscall;
	bool syscall = sc->no != NO_SYSCALL;

	if (syscall)
		sc->ret = run_syscall(sc->no, sc->params);
	sc->restart = false;
	sc->handler = false;

	if (!test_thread_flag(TIF_USER_TASK) || !lkl_ops->signal_handler)
		return;
	/* may not return: a fatal signal exits the task */
	if (!signal_pending(current) || !get_signal(&sc->ksig)) {
		if (syscall)
			prepare_restart(sc, NULL);
		restore_saved_sigmask();
		return;
	}
	if (syscall)
		prepare_restart(sc, &sc->ksig.ka);
	sc->saved_mask = *sigmask_to_save();
	sc->handler = true;
	signal_setup_done(0, &sc->ksig, 0);
}

#define CLONE_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_THREAD |	\
		     CLONE_SIGHAND | SIGCHLD)

static int host_task_id;
static struct task_struct *host0;

static int new_host_task(struct task_struct **task, int new_tg_leader)
{
	pid_t pid;
	unsigned long flags = CLONE_FLAGS;

	switch_to_host_task(host0);

	/*
	 * A new thread group is a separate process: it also gets its own
	 * signal handlers (a copy of host0's), so the handlers it installs
	 * don't change how host0 and the other host tasks see signals.
	 */
	if (new_tg_leader)
		flags &= ~(CLONE_THREAD | CLONE_SIGHAND);

	pid = kernel_thread(host_task_stub, NULL, NULL, flags);
	if (pid < 0)
		return pid;

	rcu_read_lock();
	*task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();

	/*
	 * host0 ignores SIGCHLD so that exited host tasks are reaped; a new
	 * process starts with the default, so that its own children can be
	 * waited for.
	 */
	if (new_tg_leader) {
		set_tsk_thread_flag(*task, TIF_USER_TASK);
		spin_lock_irq(&(*task)->sighand->siglock);
		(*task)->sighand->action[SIGCHLD - 1].sa.sa_handler = SIG_DFL;
		spin_unlock_irq(&(*task)->sighand->siglock);
	}

	host_task_id++;

	snprintf((*task)->comm, sizeof((*task)->comm), "host%d", host_task_id);

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

	if (lkl_cpu_get() < 0)
		return;

	switch_to_host_task(task);
	host_task_id--;
	set_ti_thread_flag(ti, TIF_SCHED_JB);
	lkl_ops->jmp_buf_set(&ti->sched_jb, exit_task);
}

static struct lkl_tls_key *task_key;

/* the task exited in the syscall; the CPU has gone to the next task */
static void host_task_exited(struct task_struct *task)
{
	int status = task->exit_code;

	lkl_ops->tls_set(task_key, NULL);
	if (lkl_ops->task_exit)
		lkl_ops->task_exit(status);
}

long lkl_syscall(long no, long *params)
{
	struct task_struct *task = host0;
	struct host_syscall sc = { .no = no, .params = params };
	long ret;

	ret = lkl_cpu_get();
	if (ret < 0)
		return ret;

	if (lkl_ops->tls_get) {
		task = lkl_ops->tls_get(task_key);
		if (!task) {
			ret = new_host_task(&task, no == __NR_new_thread_group_leader);
			if (ret)
				goto out;
			lkl_ops->tls_set(task_key, task);
		}
	}

	switch_to_host_task(task);

	if (no == __NR_reboot) {
		ret = run_syscall(no, params);
		thread_sched_jb();
		return ret;
	}

	for (;;) {
		task_thread_info(task)->syscall = &sc;
		lkl_ops->jmp_buf_set(&sc.jb, host_syscall_body);
		if (READ_ONCE(task->__state) == TASK_DEAD) {
			host_task_exited(task);
			return 0;
		}
		task_thread_info(task)->syscall = NULL;

		if (sc.handler) {
			struct k_sigaction *ka = &sc.ksig.ka;

			/* in user mode */
			lkl_cpu_put();
			lkl_ops->signal_handler((unsigned long)ka->sa.sa_handler,
						sc.ksig.sig,
						!!(ka->sa.sa_flags & SA_SIGINFO));
			ret = lkl_cpu_get();
			if (ret < 0)
				return ret;
			switch_to_host_task(task);

			/* sigreturn; then more signals, or the syscall again */
			set_current_blocked(&sc.saved_mask);
			if (!sc.restart)
				sc.no = NO_SYSCALL;
			continue;
		}
		if (!sc.restart)
			break;
	}
	ret = sc.ret;

out:
	lkl_cpu_put();

	return ret;
}

/*
 * A task cloned from userspace (fork) runs on a new host thread. It waits
 * there like any new thread until the kernel first schedules it, then
 * becomes the thread's host task, leaves the kernel, and returns to the host,
 * which resumes the child's user context.
 */
static void user_thread_bootstrap(void *arg)
{
	struct thread_info *ti = arg;

	lkl_ops->sem_down(ti->sched_sem);
	/* copy_process() failed after copy_thread(); see kill_thread() */
	if (ti->dead)
		lkl_ops->thread_exit();

	set_ti_thread_flag(ti, TIF_HOST_THREAD);
	lkl_ops->tls_set(task_key, ti->task);
	if (ti->prev_sched)
		schedule_tail(ti->prev_sched);
	/* nobody joins host threads; the task exits when the thread does */
	lkl_ops->thread_detach();
	lkl_cpu_put();
}

int copy_user_thread(struct task_struct *p,
		     const struct kernel_clone_args *args)
{
	struct thread_info *ti = task_thread_info(p);

	if (!lkl_ops->user_thread_create || !lkl_ops->tls_set ||
	    !test_thread_flag(TIF_HOST_THREAD))
		return -ENOSYS;
	/* each process has its own memory; nothing can share it yet */
	if (args->flags & CLONE_VM)
		return -EINVAL;

	/* a plain thread, which kill_thread() can end, until it first runs */
	clear_ti_thread_flag(ti, TIF_HOST_THREAD);
	clear_ti_thread_flag(ti, TIF_SCHED_JB);
	ti->tid = lkl_ops->user_thread_create(user_thread_bootstrap, ti);

	return ti->tid ? 0 : -ENOMEM;
}

static struct task_struct *idle_host_task;

/*
 * called from idle, don't failed, don't block; returns false if there is no
 * idle host task yet
 */
bool wakeup_idle_host_task(void)
{
	if (!idle_host_task)
		return false;
	if (!need_resched())
		wake_up_process(idle_host_task);
	return true;
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

int syscalls_init(void)
{
	snprintf(current->comm, sizeof(current->comm), "host0");
	set_thread_flag(TIF_HOST_THREAD);
	host0 = current;

	// Reap zombie host tasks spawned as new thread group leaders via
	// new_thread_group_leader syscall (otherwise the dead task resources
	// such as `struct task_struct` won't be reclaimed by kernel).
	// TODO: currently LKL doesn't support thread signal handling in
	// user-space, thus, it's safe to ignore this signal here. However,
	// if user-space signal handling is implemented in future we might want
	// to remove the line of code below to let user-space handle SIGCHLD.
	kernel_sigaction(SIGCHLD, SIG_IGN);

	if (lkl_ops->tls_alloc) {
		task_key = lkl_ops->tls_alloc(del_host_task);
		if (!task_key)
			return -1;
	}

	if (kernel_thread(idle_host_task_loop, NULL, NULL, CLONE_FLAGS) < 0) {
		if (lkl_ops->tls_free)
			lkl_ops->tls_free(task_key);
		return -1;
	}

	return 0;
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
