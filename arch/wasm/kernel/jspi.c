// SPDX-License-Identifier: GPL-2.0-only
/*
 * CONFIG_WASM_JSPI: one CPU, and every task a coroutine on the host's event
 * loop, one kernel instance for them all. A task blocks by suspending
 * through JavaScript Promise Integration (kernel.jspi_*), not by waiting on
 * an atomic in a worker of its own: here, the task switch (__switch_to's)
 * and the entry into user code, through the kernel's own function table.
 */
#include <asm/exobox.h>
#include <asm/globals.h>
#include <asm/jspi.h>
#include <asm/wasm_imports.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>

struct task_struct *wasm_jspi_switch_to(struct task_struct *from,
					struct task_struct *to, int cpu)
{
	struct thread_info *from_info = task_thread_info(from);
	struct thread_info *to_info = task_thread_info(to);
	struct task_struct *prev;

	/*
	 * One CPU, one event loop: hand it to the next task's coroutine and
	 * suspend this one until a switch back to it resumes it.
	 */
	BUG_ON(atomic_read(&to_info->running_cpu) >= 0);
	atomic_set(&to_info->running_cpu, cpu);
	if (wasm_get_thread_done()) {
		/* (one instance for every task: the next one isn't done) */
		wasm_set_thread_done(0);
		wasm_kernel_jspi_exit(to);
		/* (a trap: no cleanup in a program's frames below can catch it) */
		__builtin_trap();
	}
	{
		/*
		 * Every task runs on the same instance, so its globals are the
		 * last task's when it resumes: its stack pointer (a wasm local
		 * here, kept on its own JSPI stack), cpu and task are its own.
		 */
		void *sp = get_stack_pointer();

		wasm_kernel_jspi_switch(from, to);
		set_stack_pointer(sp);
	}
	cpu = atomic_read(&from_info->running_cpu);
	BUG_ON(cpu < 0 || cpu >= nr_cpu_ids || cpu != from_info->cpu);
	set_current_cpu(cpu);
	prev = get_current_task_on(cpu);
	set_current_task(from);
	return prev;
}

/*
 * Enter user code through the kernel's own function table: no JavaScript
 * frame between a syscall and the task's suspension. An exec from a syscall
 * leaves the old program by a trap, and the host runs the task again at
 * user_restart (below), into the new one.
 */
void wasm_user_enter(void)
{
	struct thread_info *ti = current_thread_info();

	ti->user_entered = 1;
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
			/* (musl's clone_entry: int (void *)) */
			((int (*)(u32))(uintptr_t)wasm_user_func(ti->entry_fn))(ti->entry_arg);
		else
			((void (*)(void))(uintptr_t)wasm_user_entry())();
	/* (it returned because it unwound for fork: on into the rewind) */
	} while (exobox_fork_unwound());
	ti->user_entered = 0;
}

static void noinline_for_stack __noreturn user_restart_inner(void)
{
	wasm_user_enter();
	/* (its entry returned without exiting) */
	local_irq_enable();
	do_exit(SIGSEGV);
}

/* exec's restart (wasm_syscall): the task's new program, on its kernel stack from the top */
__attribute__((export_name("user_restart"))) void wasm_user_restart(void)
{
	set_stack_pointer(task_pt_regs(current) - 1);
	user_restart_inner();
}
