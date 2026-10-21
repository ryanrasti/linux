// SPDX-License-Identifier: GPL-2.0-only
/*
 * A trap in a program (unreachable, an out-of-bounds access) unwinds
 * through the kernel's frames to the task's coroutine, which the host
 * catches: it then runs this, as the task, on a coroutine of its own, with
 * the signal Linux would kill it by (SIGILL for unreachable, SIGSEGV for an
 * out-of-bounds access, SIGFPE for a division by zero). A trap in user mode
 * kills the process, every thread of it; one in the kernel is a bug.
 */
#include <asm/globals.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/kernel.h>
#include <linux/ptrace.h>

static void noinline_for_stack __noreturn user_fault_inner(int sig)
{
	if (!current_pt_regs()->user_mode)
		panic("trap in the kernel (task %s, pid %d)", current->comm, current->pid);
	local_irq_enable();
	do_group_exit(sig);
}

__attribute__((export_name("user_fault"))) void exobox_user_fault(int sig)
{
	/* the task's kernel stack, from its top again (its frames were lost) */
	set_stack_pointer(task_pt_regs(current) - 1);
	user_fault_inner(sig);
}
