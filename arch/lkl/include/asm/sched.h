#ifndef _ASM_LKL_SCHED_H
#define _ASM_LKL_SCHED_H

#include <linux/sched.h>
#include <asm/kasan.h>
#include <uapi/asm/host_ops.h>

static inline void thread_sched_jb(void)
{
	if (test_ti_thread_flag(current_thread_info(), TIF_HOST_THREAD)) {
		set_ti_thread_flag(current_thread_info(), TIF_SCHED_JB);
		set_current_state(TASK_UNINTERRUPTIBLE);
		lkl_ops->jmp_buf_set(&current_thread_info()->sched_jb,
				     schedule);
		/*
		 * The previous call to setjmp/longjmp won't unwind the stack
		 * and, as a result, shadow memory will remain poisoned.
		 */
		kasan_unpoison_stack();
	} else {
		lkl_bug("thread_sched_jb() can be used only for host task");
	}
}

void switch_to_host_task(struct task_struct *);
int host_task_stub(void *unused);
void host_task_exit_status(struct task_struct *task, int *pid, int *status);
void __noreturn host_task_exited(int pid, int status);

#endif /*  _ASM_LKL_SCHED_H */
