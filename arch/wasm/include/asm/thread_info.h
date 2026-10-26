#ifndef _WASM_THREAD_INFO_H
#define _WASM_THREAD_INFO_H

#include <asm/exobox_thread.h>
#include <asm/page.h>
#include <linux/types.h>

struct kernel_siginfo;
struct mm_struct;

/*
 * A remote wake changes the value parked workers wait on, so publishing a
 * request cannot race between their mailbox check and atomic.wait().
 */
#define WASM_CPU_PARKED		(-1)
#define WASM_CPU_REMOTE_WAKE	(-2)

/* THREAD_SIZE is the size of the task_struct + kernel stack (asserted in
 * setup): 128 KiB, whatever the page size (2 pages of 64 KiB, 32 of 4 KiB);
 * a task_struct is *way* less. */
#define THREAD_SIZE_ORDER (17 - PAGE_SHIFT)
#define THREAD_SIZE (PAGE_SIZE << THREAD_SIZE_ORDER)
#define THREAD_SHIFT (PAGE_SHIFT + THREAD_SIZE_ORDER)

struct thread_info {
	unsigned long flags;
	unsigned long syscall_work; /* SYSCALL_WORK_ flags */
	int preempt_count;
	int cpu; // this is for the kernel
	atomic_t running_cpu; // negative means unscheduled
	/* The mm represented by this worker's local JS user context. */
	struct mm_struct *context_mm;
	unsigned long tp_value;
	const struct kernel_siginfo *active_siginfo;
	/* Counts execs that replaced this task's user instance. */
	unsigned int exec_count;
#ifdef CONFIG_WASM_JSPI
	/* copy_thread's bootstrap args, until the task first runs (exit_thread) */
	void *bootstrap;
	/* a clone child's entry (user table index, 0: _start) and argument */
	u32 clone_fn, clone_arg;
	/* what the program was entered at (wasm_user_enter), and with what */
	u32 entry_fn, entry_arg;
	/* a fork child: rewinding into its parent's entry_fn, not starting anew */
	int rewinding_entry;
	/* the SA_SIGINFO handler's copy_siginfo: what it gave (-EINVAL: none yet) */
	int siginfo_copied;
#endif
#ifdef CONFIG_EXOBOX
	struct exobox_thread_info exobox;	/* (arch/wasm/exobox/) */
#endif
};

#ifndef current_thread_info
#include <asm/current.h>
#define current_thread_info() ((struct thread_info *)current)
#endif

#define INIT_THREAD_INFO(tsk)                        \
	{                                            \
		.flags = 0,                          \
		.preempt_count = INIT_PREEMPT_COUNT, \
		.cpu = 0,                            \
		.running_cpu = ATOMIC_INIT(0),       \
		.context_mm = NULL,                  \
		.tp_value = U32_MAX,                 \
		.active_siginfo = NULL,              \
	}

#include <asm-generic/thread_info_tif.h>

#endif
