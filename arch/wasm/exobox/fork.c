// SPDX-License-Identifier: GPL-2.0-only
/*
 * fork: clone without a function or CLONE_VM returns twice, as on other
 * architectures. A program's wasm stack can't be copied, so the program is
 * Asyncify-instrumented and saves it into its memory: the clone call asks
 * the host to start unwinding and returns at once (no syscall entry or
 * exit: nothing may run in the program meanwhile, a signal handler
 * included); the program unwinds out of its entry, back to
 * wasm_user_enter, which makes the child, its memory a copy of the
 * parent's (the saved stack in it); then both rewind into the same clone
 * call, which returns the child's pid in the parent and 0 in the child.
 */
#include <asm/exobox.h>
#include <asm/wasm_imports.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/syscalls.h>

bool exobox_syscall_enter(long nr, const unsigned long args[6], long *ret)
{
	struct thread_info *ti = current_thread_info();

	/* the end of a rewind: the clone call returning, a second time */
	if (ti->rewinding) {
		ti->rewinding = 0;
		wasm_user_stop_rewind();
		*ret = ti->rewind_ret;
		return true;
	}

	/* clone(fn, fn_arg, flags, ptid, ctid, tls) with neither: fork */
	if (nr != __NR_clone || args[0] || (args[2] & CLONE_VM))
		return false;
	/*
	 * The copy is of one memory at one moment: not while another task
	 * shares it (as a thread), which could run before the child does.
	 */
	if (!wasm_user_can_fork() || atomic_read(&current->mm->mm_users) > 1) {
		*ret = -ENOSYS;
		return true;
	}
	ti->forking = 1;
	ti->fork_flags = args[2];
	ti->fork_ptid = args[3];
	ti->fork_ctid = args[4];
	ti->fork_tls = args[5];
	wasm_user_start_unwind();
	*ret = 0;	/* (unused: the program unwinds past it) */
	return true;
}

int exobox_fork_child(void *unused)
{
	struct thread_info *ti = current_thread_info();

	/* its program, a copy of the parent's, rewinds to return 0 from clone */
	ti->rewinding = 1;
	ti->rewind_ret = 0;
	wasm_user_start_rewind();
	return 0;
}

static long exobox_fork(struct thread_info *ti)
{
	unsigned long flags = ti->fork_flags;
	struct kernel_clone_args args = {
		.flags = lower_32_bits(flags) & ~CSIGNAL,
		.exit_signal = lower_32_bits(flags) & CSIGNAL,
		.pidfd = (int __user *)ti->fork_ptid,
		.parent_tid = (int __user *)ti->fork_ptid,
		.child_tid = (int __user *)ti->fork_ctid,
		.tls = ti->fork_tls,
		.fn = exobox_fork_child,
	};

	return kernel_clone(&args);
}

bool exobox_fork_unwound(void)
{
	struct thread_info *ti = current_thread_info();

	if (!ti->forking)
		return false;
	ti->forking = 0;
	wasm_user_stop_unwind();
	/* (the host copies the memory now: copy_thread's WASM_USER_MEMORY_COPY) */
	ti->rewind_ret = exobox_fork(ti);
	ti->rewinding = 1;
	wasm_user_start_rewind();
	return true;
}
