#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <asm/host_ops.h>
#include <asm/cpu.h>
#include <asm/sched.h>
#include <asm/switch_to.h>

static int init_ti(struct thread_info *ti)
{
	ti->sched_sem = lkl_ops->sem_alloc(0);
	if (!ti->sched_sem)
		return -ENOMEM;

	ti->dead = false;
	ti->prev_sched = NULL;
	ti->tid = 0;
	ti->exec_old_mm = NULL;

	return 0;
}

unsigned long *arch_alloc_thread_stack_node(struct task_struct *task, int node)
{
	struct thread_info *ti;

	ti = kmalloc(sizeof(*ti), GFP_KERNEL);
	if (!ti)
		return NULL;

	if (init_ti(ti)) {
		kfree(ti);
		return NULL;
	}
	ti->task = task;


	return (unsigned long *)ti;
}

/*
 * The only new tasks created are kernel threads that have a predefined starting
 * point thus no stack copy is required.
 */
void setup_thread_stack(struct task_struct *p, struct task_struct *org)
{
	struct thread_info *ti = task_thread_info(p);
	struct thread_info *org_ti = task_thread_info(org);

	ti->flags = org_ti->flags;
	ti->preempt_count = org_ti->preempt_count;
}

static void kill_thread(struct thread_info *ti)
{
	if (!test_ti_thread_flag(ti, TIF_HOST_THREAD)) {
		ti->dead = true;
		lkl_ops->sem_up(ti->sched_sem);
		lkl_ops->thread_join(ti->tid);
	}
	lkl_ops->sem_free(ti->sched_sem);

}

void arch_free_thread_stack(struct task_struct *tsk)
{
	struct thread_info *ti = task_thread_info(tsk);

	kill_thread(ti);
	kfree(ti);
}

/*
 * A task's exit (do_exit's, after its mm's): an exec's old memory it had
 * left, which its next syscall would have released (wasi_exec.c): a task
 * killed at the exec's return, or by its failure past the point of no
 * return, has none. And PID 1's, the system's end.
 */
void exit_thread(struct task_struct *tsk)
{
	struct thread_info *ti = task_thread_info(tsk);

	if (ti->exec_old_mm) {
		mmput(ti->exec_old_mm);
		ti->exec_old_mm = NULL;
	}

	/*
	 * PID 1's end, its last thread's (exit_group, exit, or a fatal
	 * signal: a failed exec's SIGSEGV), is the host's, with its wait
	 * status, once its memory is torn down (exit_mm: a shared mapping's
	 * copy written back) and its files closed: as a container runtime
	 * ends a container whose init died, rather than Linux's panic
	 */
	if (IS_ENABLED(CONFIG_ARCH_INIT_EXIT_ENDS_SYSTEM) && tsk == current &&
	    is_global_init(tsk) && (tsk->flags & PF_EXITING) &&
	    !atomic_read(&tsk->signal->live)) {
		struct signal_struct *sig = tsk->signal;
		int status = sig->flags & SIGNAL_GROUP_EXIT ?
			     sig->group_exit_code : tsk->exit_code;

		if (lkl_ops->task_exit)
			lkl_ops->task_exit(1, status);
		panic("Attempted to kill init! exitcode=0x%08x\n", status);
	}
}

struct thread_info *_current_thread_info = &init_thread_union.thread_info;

/*
 * schedule() expects the return of this function to be the task that we
 * switched away from. Returning prev is not going to work because we are
 * actually going to return the previous taks that was scheduled before the
 * task we are going to wake up, and not the current task, e.g.:
 *
 * swapper -> init: saved prev on swapper stack is swapper
 * init -> ksoftirqd0: saved prev on init stack is init
 * ksoftirqd0 -> swapper: returned prev is swapper
 */
static struct task_struct *abs_prev = &init_task;

struct task_struct *__switch_to(struct task_struct *prev,
				struct task_struct *next)
{
	struct thread_info *_prev = task_thread_info(prev);
	struct thread_info *_next = task_thread_info(next);
	unsigned long _prev_flags = _prev->flags;
	struct lkl_jmp_buf _prev_jb;
	bool exited = false;
	int pid = 0, status = 0;

	_current_thread_info = task_thread_info(next);
	_next->prev_sched = prev;
	abs_prev = prev;

	BUG_ON(!_next->tid);
	lkl_cpu_change_owner(_next->tid);

	if (test_bit(TIF_SCHED_JB, &_prev_flags)) {
		/* Atomic. Must be done before wakeup next */
		clear_ti_thread_flag(_prev, TIF_SCHED_JB);
		_prev_jb = _prev->sched_jb;
	} else if (test_bit(TIF_HOST_THREAD, &_prev_flags) &&
		   READ_ONCE(prev->__state) == TASK_DEAD && lkl_ops->task_exit) {
		/*
		 * a host task exited in a syscall: its host thread ends too,
		 * and if it was its process's last thread, the process (its
		 * wait status: the group's exit code if one ended it all)
		 */
		struct signal_struct *sig = prev->signal;

		exited = true;
		if (!atomic_read(&sig->live)) {
			pid = task_tgid_nr(prev);
			status = sig->flags & SIGNAL_GROUP_EXIT ?
				 sig->group_exit_code : prev->exit_code;
		}
	}

	lkl_ops->sem_up(_next->sched_sem);
	if (test_bit(TIF_SCHED_JB, &_prev_flags)) {
		lkl_ops->jmp_buf_longjmp(&_prev_jb, 1);
	} else if (exited) {
		lkl_ops->task_exit(pid, status);
	} else {
		lkl_ops->sem_down(_prev->sched_sem);
	}

	if (_prev->dead) {
		kasan_unpoison_stack();
		lkl_ops->thread_exit();
	}

	return abs_prev;
}

int host_task_stub(void *unused)
{
	return 0;
}

void switch_to_host_task(struct task_struct *task)
{
	if (WARN_ON(!test_tsk_thread_flag(task, TIF_HOST_THREAD)))
		return;

	task_thread_info(task)->tid = lkl_ops->thread_self();

	if (current == task)
		return;

	wake_up_process(task);
	thread_sched_jb();
	lkl_ops->sem_down(task_thread_info(task)->sched_sem);
	schedule_tail(abs_prev);
}

struct thread_bootstrap_arg {
	struct thread_info *ti;
	int (*f)(void *);
	void *arg;
};

static void thread_bootstrap(void *_tba)
{
	struct thread_bootstrap_arg *tba = (struct thread_bootstrap_arg *)_tba;
	struct thread_info *ti = tba->ti;
	int (*f)(void *) = tba->f;
	void *arg = tba->arg;

	lkl_ops->sem_down(ti->sched_sem);
	kfree(tba);
	if (ti->prev_sched)
		schedule_tail(ti->prev_sched);

	f(arg);
	do_exit(0);
}

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	unsigned long esp = (unsigned long)args->fn;
	struct thread_info *ti = task_thread_info(p);
	struct thread_bootstrap_arg *tba;

	if ((int (*)(void *))esp == host_task_stub) {
		set_ti_thread_flag(ti, TIF_HOST_THREAD);
		return 0;
	}

	/* clone from userspace */
	if (!args->fn)
		return copy_user_thread(p, args);

	tba = kmalloc(sizeof(*tba), GFP_KERNEL);
	if (!tba)
		return -ENOMEM;

	tba->f = (int (*)(void *))esp;
	tba->arg = (void *)args->fn_arg;
	tba->ti = ti;

	ti->tid = lkl_ops->thread_create(thread_bootstrap, tba);
	if (!ti->tid) {
		kfree(tba);
		return -ENOMEM;
	}

	return 0;
}

/**
 * This is called before the kernel initializes, so no kernel calls (including
 * printk) can't be made yet.
 */
void threads_init(void)
{
	int ret;
	struct thread_info *ti = &init_thread_union.thread_info;

	ret = init_ti(ti);
	if (ret < 0)
		lkl_printf("lkl: failed to allocate init schedule semaphore\n");

	ti->tid = lkl_ops->thread_self();
}

void threads_cleanup(void)
{
	struct task_struct *p, *t;

	for_each_process_thread(p, t) {
		struct thread_info *ti = task_thread_info(t);

		if (t->pid != 1 && !test_ti_thread_flag(ti, TIF_HOST_THREAD))
			WARN(!(t->flags & PF_KTHREAD),
			     "non kernel thread task %s\n", t->comm);
		WARN(READ_ONCE(t->__state) == TASK_RUNNING,
		     "thread %s still running while halting\n", t->comm);

		kill_thread(ti);
	}

	lkl_ops->sem_free(init_thread_union.thread_info.sched_sem);
}
