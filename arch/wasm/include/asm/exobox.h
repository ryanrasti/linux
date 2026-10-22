/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * exobox's host interface (CONFIG_EXOBOX, arch/wasm/exobox/): its hooks in
 * arch/wasm, each a no-op without it, so his files call them unconditionally.
 */
#ifndef _WASM_EXOBOX_H
#define _WASM_EXOBOX_H

#include <linux/compiler.h>
#include <linux/types.h>

struct file;

/* send a file's bytes to the host to compile (binfmt_wasm.c) */
int wasm_stream_file(struct file *file);

#ifdef CONFIG_EXOBOX
#include <linux/jiffies.h>

/*
 * At a syscall's entry, its arguments ours to change: true if it's handled
 * here, and returns *ret (exobox/syscall.c).
 */
bool exobox_syscall_enter(long nr, unsigned long args[6], long *ret);

/* a syscall's preemption point, past a time slice: the host's event loop turns (preempt.c) */
extern unsigned long exobox_slice_end;
void exobox_preempt(void);
static inline void exobox_syscall_preempt(void)
{
	if (unlikely(time_after_eq(jiffies, exobox_slice_end)))
		exobox_preempt();
}

/* after the program's entry returned: true if it unwound for fork (now rewound) (fork.c) */
bool exobox_fork_unwound(void);
/* a fork child's kernel_clone fn: its memory a copy of its parent's */
int exobox_fork_child(void *unused);
static inline bool exobox_is_fork(int (*fn)(void *))
{
	return fn == exobox_fork_child;
}

/* exec: a dynamically linked program's libraries, after it (exec.c) */
int exobox_exec_libraries(void);
#else
static inline bool exobox_syscall_enter(long nr, unsigned long args[6], long *ret) { return false; }
static inline void exobox_syscall_preempt(void) {}
static inline bool exobox_fork_unwound(void) { return false; }
static inline bool exobox_is_fork(int (*fn)(void *)) { return false; }
static inline int exobox_exec_libraries(void) { return 0; }
#endif

#ifdef CONFIG_EXOBOX_EXEC_CACHE
/*
 * exec: whether the host has this file compiled already (exec.c): 0 (then
 * nothing is sent), 1 (send it, and the host caches it), or a negative errno
 */
int exobox_exec_cached(struct file *file, loff_t size, u32 max_memory_pages);
#else
static inline int exobox_exec_cached(struct file *file, loff_t size, u32 max_memory_pages) { return 1; }
#endif

#endif
