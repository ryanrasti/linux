// SPDX-License-Identifier: GPL-2.0-only
/*
 * For the host's debugging: every task's state, as SysRq-T prints it, and
 * the syscall each is in (wasm has no stack to walk: the arguments say
 * what it waits on).
 */
#include <linux/ptrace.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>

__attribute__((export_name("debug_tasks"))) void exobox_debug_tasks(void)
{
	struct task_struct *g, *t;

	rcu_read_lock();
	for_each_process_thread(g, t) {
		struct pt_regs *regs = task_pt_regs(t);

		if (t->flags & PF_KTHREAD)
			continue;
		pr_info("task %d (tgid %d, %s) state %c: syscall %ld (%lx, %lx, %lx, %lx)\n",
			t->pid, t->tgid, t->comm, task_state_to_char(t), regs->syscall_nr,
			regs->syscall_args[0], regs->syscall_args[1], regs->syscall_args[2],
			regs->syscall_args[3]);
	}
	rcu_read_unlock();
}
