// SPDX-License-Identifier: GPL-2.0-only
/*
 * CONFIG_WASM_JSPI: one CPU, and every task a coroutine on the host's event
 * loop, one kernel instance for them all. A task blocks by suspending
 * through JavaScript Promise Integration, its waits the host's
 * (asm/wasm_wait.h), not an atomic's in a worker of its own: here, the entry
 * into user code, through the kernel's own function table.
 */
#include <asm/exobox.h>
#include <asm/globals.h>
#include <asm/jspi.h>
#include <asm/wasm_imports.h>
#include <linux/mm.h>
#include <linux/ptrace.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>

/*
 * Enter user code through the kernel's own function table: no JavaScript
 * frame between a syscall and the task's suspension. An exec from a syscall
 * leaves the old program as the syscall export returns (user.unwind: under
 * JSPI, the host has the kernel trap out of it), and the host runs the task
 * again at user_restart (below), into the new one.
 */
void wasm_user_enter(void)
{
	struct thread_info *ti = current_thread_info();

	/*
	 * What the program is entered at: a clone child's function, else
	 * _start (after exec too); kept, for a fork's rewind into the same
	 * frames (a fork child inherits it with the rest of thread_info).
	 */
	if (ti->clone_fn) {
		ti->entry_fn = ti->clone_fn;
		ti->entry_arg = ti->clone_arg;
		ti->clone_fn = 0;
	} else if (!ti->rewinding_entry) {
		ti->entry_fn = 0;
	}
	ti->rewinding_entry = 0;
	do {
		/* (a trap from here on is the program's: see exobox/fault.c) */
		current_pt_regs()->user_mode = 1;
		if (ti->entry_fn)
			wasm_user_thread(ti->entry_fn)(ti->entry_arg);
		else
			wasm_user_start()();
	/* (it returned because it unwound for fork: on into the rewind) */
	} while (exobox_fork_unwound());
}

static void noinline_for_stack __noreturn user_restart_inner(void *unused)
{
	/*
	 * (an exec from a signal handler left call_signal_handler without its
	 * restoring this: the info it points at was on the stack now reset)
	 */
	current_thread_info()->active_siginfo = NULL;
	wasm_user_enter();
	/* (its entry returned without exiting) */
	local_irq_enable();
	do_exit(SIGSEGV);
}

/* exec's restart (after user.unwind): the task's new program, on its kernel stack from the top */
__attribute__((export_name("user_restart"))) void wasm_user_restart(void)
{
	wasm_call_on_task_stack(current, user_restart_inner, NULL);
}

/*
 * For the host, which runs every task's coroutine and parks them in their
 * waits (asm/wasm_wait.h): which task holds the cpu, as __switch_to and
 * task_entry record it (globals.c's; one cpu), and so whose program the
 * user imports act on.
 */
extern struct task_struct *current_tasks[NR_CPUS];

__attribute__((export_name("current_task_word"))) struct task_struct **wasm_current_task_word(void)
{
	return &current_tasks[0];
}

/*
 * Another process's memory (/proc/<pid>/cmdline, environ, ptrace): every
 * process's is the host's, in this one isolate, so it copies from a task
 * of the mm's directly. (remote_vm.c asks the owner's worker instead, which
 * here can't run while this task waits.)
 */
int wasm_jspi_access_remote_vm(struct mm_struct *mm, unsigned long addr, void *buf,
			       int len, unsigned int gup_flags)
{
	struct task_struct *p, *t;
	int ret = 0;

	if (len <= 0)
		return 0;
	rcu_read_lock();
	for_each_process_thread(p, t) {
		/*
		 * (and its program instantiated: exec sets mm before the host
		 * has the new memory, context_mm after, as remote_vm.c's guard)
		 */
		if (t->mm == mm && READ_ONCE(task_thread_info(t)->context_mm) == mm) {
			ret = wasm_kernel_jspi_access_task(t, addr, buf, len,
							   !!(gup_flags & FOLL_WRITE));
			goto out;
		}
	}
out:
	rcu_read_unlock();
	return ret;
}
