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
#include <linux/platform_device.h>
#include <linux/tick.h>
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

	if (new_tg_leader)
		flags &= ~CLONE_THREAD;

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

	/* init's exit would panic; its end is the host's (lkl_syscall) */
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

long lkl_syscall(long no, long *params)
{
	struct task_struct *task = init_host_task;
	long ret;

	ret = lkl_cpu_get();
	if (ret < 0)
		return ret;

	if (lkl_ops->tls_get) {
		task = lkl_ops->tls_get(task_key);
		if (!task) {
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

	/*
	 * init's end, its exit_group or its last thread's exit, is the host's,
	 * with its wait status: do_exit() would panic. (host0 is the other
	 * live thread init always has.) The call returns the status instead;
	 * the host ends then.
	 */
	if (is_global_init(task) &&
	    (no == __NR_exit_group ||
	     (no == __NR_exit && atomic_read(&task->signal->live) == 2))) {
		ret = (params[0] & 0xff) << 8;
		goto out;
	}

	ret = run_syscall(no, params);

	if (no == __NR_reboot) {
		thread_sched_jb();
		return ret;
	}

out:
	lkl_cpu_put();

	return ret;
}

static struct task_struct *idle_host_task;

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
