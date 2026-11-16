#include "../../../kernel/sched/sched.h"
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
#include <linux/sched/mm.h>
#include <linux/mman.h>
#include <linux/sizes.h>
#include <linux/signal.h>
#include <linux/platform_device.h>
#include <asm/host_ops.h>
#include <asm/syscalls.h>
#include <asm/syscalls_32.h>
#include <asm/cpu.h>
#include <asm/sched.h>
#include <asm/wasi_sock.h>
#include <asm/window.h>

#ifdef CONFIG_ARCH_HAS_SYSCALL_WRAPPER
/* one type for every entry (asm/syscall_wrapper.h) */
typedef long (*syscall_handler_t)(const long *args);

static long __lkl_sys_ni_syscall(const long *args)
{
	return sys_ni_syscall();
}

/* indirect, so the table's aliases (asm/syscalls.h) expand before pasting */
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
	long ret, args[6];
	syscall_handler_t handler;

	if (no < 0 || no >= __NR_syscalls)
		return -ENOSYS;
	handler = (syscall_handler_t)syscall_table[no];
	/* AF_INET(6) sockets, local or wasi:sockets' (drivers/wasi_sock.c) */
	if (no == __NR_socket)
		params[0] = wasi_sock_family(params[0], params[1]);
	/* (a process in a window: its mappings' addresses the kernel's) */
	lkl_user_addrs(no, params, args);

#ifdef CONFIG_ARCH_HAS_SYSCALL_WRAPPER
	ret = handler(args);
#else
	ret = handler(args[0], args[1], args[2], args[3], args[4], args[5]);
#endif
	ret = lkl_user_result(no, ret);

	task_work_run();

	return ret;
}


#define CLONE_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_THREAD |	\
		     CLONE_SIGHAND | SIGCHLD)

static int host_task_id;
/*
 * The task host calls are made as when a host thread has none of its own,
 * and the thread group of each host thread's: a kernel thread (kthreadd's
 * child, not init's), its signals ignored, so exited host tasks are reaped.
 */
static struct task_struct *host0;

static int new_host_task(struct task_struct **task, int new_tg_leader)
{
	pid_t pid;

	switch_to_host_task(host0);

	/*
	 * A new thread group is a process of its own, as init is: a user
	 * mode thread (a kernel thread, PF_KTHREAD, can't exec a program),
	 * its files and fs a copy of host0's and its signals' actions the
	 * defaults, so its children can be waited for.
	 */
	if (new_tg_leader)
		pid = user_mode_thread(host_task_stub, NULL, SIGCHLD);
	else
		pid = kernel_thread(host_task_stub, NULL, NULL, CLONE_FLAGS);
	if (pid < 0)
		return pid;

	rcu_read_lock();
	*task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();

	if (new_tg_leader) {
		spin_lock_irq(&(*task)->sighand->siglock);
		flush_signal_handlers(*task, 1);
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

/*
 * Whether a syscall a signal interrupted runs again (an -ERESTART* ret;
 * any other is its result): if not, it ends with -EINTR. ka: the handler
 * about to run, or NULL.
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
 * A syscall's return to the program, as an arch's (arch_do_signal_or_restart)
 * but without a frame: a pending signal's handler, a function of the
 * program's, is called by the host on the task's own stack, and when it
 * returns, sigreturn's work is done here (the mask restored; then more
 * signals, or the syscall again, unless the host says the handler moved
 * the program on). A fatal signal exits the task in get_signal(), which
 * doesn't return (__switch_to(): task_exit).
 */
static long syscall_exit_signals(long no, long *params, long ret)
{
	struct task_struct *task = current;
	struct ksignal ksig;
	siginfo_t info;
	sigset_t mask;
	bool restart, si;

	for (;;) {
		if (!signal_pending(task) || !get_signal(&ksig)) {
			restart = syscall_restarts(&ret, NULL);
			restore_saved_sigmask();
			if (!restart)
				return ret;
		} else {
			restart = syscall_restarts(&ret, &ksig.ka);
			si = ksig.ka.sa.sa_flags & SA_SIGINFO;
			if (si)
				copy_siginfo_to_external(&info, &ksig.info);
			mask = *sigmask_to_save();
			signal_setup_done(0, &ksig, 0);

			/* in the program */
			lkl_cpu_put();
			if (!lkl_ops->signal_handler((unsigned long)ksig.ka.sa.sa_handler,
						     ksig.sig, si ? &info : NULL,
						     &mask, restart) && restart) {
				restart = false;
				ret = -EINTR;
			}
			if (lkl_cpu_get() < 0)
				return -EINTR;	/* (the kernel halted) */
			switch_to_host_task(task);
			/* sigreturn */
			set_current_blocked(&mask);
			if (!restart)
				continue;
		}
		if (ret == -ERESTART_RESTARTBLOCK)
			no = __NR_restart_syscall;
		ret = run_syscall(no, params);
	}
}

long lkl_syscall(long no, long *params)
{
	struct task_struct *task = host0;
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

	/* (an exec's old memory, which the task ran on until it returned) */
	if (task_thread_info(task)->exec_old_mm) {
		mmput(task_thread_info(task)->exec_old_mm);
		task_thread_info(task)->exec_old_mm = NULL;
	}

	ret = run_syscall(no, params);

	if (no == __NR_reboot) {
		thread_sched_jb();
		return ret;
	}

	/* a process's (a kernel thread's signals are the kernel's own) */
	if (!(task->flags & PF_KTHREAD) && lkl_ops->signal_handler)
		ret = syscall_exit_signals(no, params, ret);

out:
	lkl_cpu_put();

	return ret;
}

/*
 * A thread of a host task's process (clone with CLONE_VM, on a stack of its
 * own) runs on a new host thread. It waits there like any new thread until
 * the kernel first schedules it, then becomes the thread's host task, leaves
 * the kernel, and returns to the host, which starts it at the function its
 * libc left on that stack.
 */
static void vm_thread_bootstrap(void *arg)
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
	lkl_cpu_put();
}

int copy_user_thread(struct task_struct *p,
		     const struct kernel_clone_args *args)
{
	struct thread_info *ti = task_thread_info(p);

	if (!lkl_ops->vm_thread_create || !lkl_ops->tls_set ||
	    !test_thread_flag(TIF_HOST_THREAD))
		return -ENOSYS;
	/*
	 * a copy of a process's memory (fork): its window's copy (copy_mm's,
	 * nommu_dup_mmap), a host thread that resumes the parent's user code
	 * in it; else a thread on a stack given
	 */
	if (!(args->flags & CLONE_VM)) {
		if (!lkl_ops->fork_thread_create || !p->mm || !p->mm->context.win)
			return -EINVAL;
		clear_ti_thread_flag(ti, TIF_HOST_THREAD);
		clear_ti_thread_flag(ti, TIF_SCHED_JB);
		ti->tid = lkl_ops->fork_thread_create(vm_thread_bootstrap, ti,
						      p->mm->context.base);
		return ti->tid ? 0 : -ENOMEM;
	}
	if (!args->stack)
		return -EINVAL;

	/* a plain thread, which kill_thread() can end, until it first runs */
	clear_ti_thread_flag(ti, TIF_HOST_THREAD);
	clear_ti_thread_flag(ti, TIF_SCHED_JB);
	ti->tid = lkl_ops->vm_thread_create(vm_thread_bootstrap, ti, args->stack);

	return ti->tid ? 0 : -ENOMEM;
}

static struct task_struct *idle_host_task;

/*
 * Whether a task other than current (and the idle host task, which only
 * stands for host tasks being in user mode) is runnable: then a host task
 * leaving the kernel lets it run (lkl_cpu_put). Counting a queued idle host
 * task made a host task that kept making syscalls switch to it and back at
 * every one.
 */
bool others_runnable(void)
{
	unsigned int n = this_rq()->nr_running;

	if (idle_host_task && current != idle_host_task &&
	    task_on_rq_queued(idle_host_task))
		n--;
	return n > 1;
}

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

static DECLARE_COMPLETION(host0_made);
static int host0_ret;

/*
 * host0: a kernel thread that becomes a host task (its host thread ends,
 * as init's does in lkl_run_init), the idle host task a thread of its group
 */
static int host0_init(void *unused)
{
	/* its files its own (host tasks' descriptors), not the kernel threads' */
	host0_ret = ksys_unshare(CLONE_FILES);
	if (!host0_ret &&
	    kernel_thread(idle_host_task_loop, NULL, NULL, CLONE_FLAGS) < 0)
		host0_ret = -ENOMEM;
	set_thread_flag(TIF_HOST_THREAD);
	host0 = current;
	complete(&host0_made);
	/* asleep until a host thread takes it (switch_to_host_task) */
	thread_sched_jb();
	lkl_ops->thread_exit();
	return 0;
}

/*
 * Called by init (PID 1) in its exec of /init, which then stays the task
 * of the thread that started the kernel (syscalls_init_thread): the first
 * program's.
 */
int syscalls_init(void)
{
	struct task_struct *t;

	if (lkl_ops->tls_alloc) {
		task_key = lkl_ops->tls_alloc(del_host_task);
		if (!task_key)
			return -1;
	}

	t = kthread_run(host0_init, NULL, "host0");
	if (!IS_ERR(t)) {
		wait_for_completion(&host0_made);
		if (!host0_ret) {
			set_thread_flag(TIF_HOST_THREAD);
			return 0;
		}
	}
	if (lkl_ops->tls_free)
		lkl_ops->tls_free(task_key);
	return -1;
}

/* the thread that started the kernel: init its host task */
void syscalls_init_thread(void)
{
	if (lkl_ops->tls_set)
		lkl_ops->tls_set(task_key, current);
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


/*
 * A process's memory past its mappings: its break, a mapping of its own
 * (the program's data is the module's, not the process's), which brk grows
 * over and shrinks back from; and its main thread's stack, a mapping the
 * host runs the program on, its argv, envp and auxv at the top as Linux
 * lays them out (so musl's pthread_getattr_np finds the stack by mremap's
 * probe, as natively). Their pages are backed as they're written (do_mmap
 * zeroes only what isn't zero). Returns the stack's top, or an error.
 */
#define LKL_BRK_SIZE (1 << 20)

unsigned long lkl_process_mem(void)
{
	struct mm_struct *mm = current->mm;
	unsigned long addr, stack;

	addr = vm_mmap(NULL, 0, LKL_BRK_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (IS_ERR_VALUE(addr))
		return addr;
	mm->start_brk = mm->brk = addr;
	mm->context.end_brk = addr + LKL_BRK_SIZE;

	/* (RLIMIT_STACK's, 8 MiB by default, within reason) */
	stack = PAGE_ALIGN(clamp_t(unsigned long, rlimit(RLIMIT_STACK),
				   SZ_128K, SZ_64M));
	addr = vm_mmap(NULL, 0, stack, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, 0);
	if (IS_ERR_VALUE(addr))
		return addr;
	mm->start_stack = addr + stack;
	return addr + stack;
}

/*
 * A process's memory: an mm of its own (one made from host0, a kernel
 * thread, has none until here), so its mappings are its own (nommu's mmap)
 * and freed with it, and a break. exec replaces it, as a native process's.
 */
static unsigned long new_process_mm(void)
{
	struct task_struct *tsk = current;
	struct mm_struct *mm, *active_mm;

	if (tsk->mm)
		return -EEXIST;
	mm = mm_alloc();
	if (!mm)
		return -ENOMEM;
	task_lock(tsk);
	active_mm = tsk->active_mm;
	tsk->active_mm = mm;
	tsk->mm = mm;
	task_unlock(tsk);
	if (active_mm)
		mmdrop_lazy_tlb(active_mm);
	return lkl_process_mem();
}

/*
 * The task is made in lkl_syscall routine: here its memory. Returns its
 * stack's top (lkl_process_mem), or an error.
 */
SYSCALL_DEFINE0(new_thread_group_leader)
{
	if (current->tgid == current->pid)
		return new_process_mm();
	else
		return -EINVAL;
}
