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
struct pt_regs;

#ifdef CONFIG_EXOBOX
#include <linux/jiffies.h>

/*
 * At a syscall's entry, before anything else (fork's rewind): true if it's
 * handled here, and returns *ret (exobox/syscall.c).
 */
bool exobox_syscall_enter(long nr, const unsigned long args[6], long *ret);
/*
 * A syscall past its entry work (seccomp, ptrace, audit), its exit work to
 * come: its arguments ours to change; true if it's handled here, its result
 * in regs->syscall_return (exobox/syscall.c).
 */
bool exobox_syscall(struct pt_regs *regs);

/*
 * a syscall's preemption point, past a time slice or when the host asks (its
 * work in progress): the host's event loop turns (preempt.c)
 */
extern unsigned long exobox_slice_end;
extern int exobox_host_turn;
void exobox_preempt(void);
static inline void exobox_syscall_preempt(void)
{
	if (unlikely(time_after_eq(jiffies, exobox_slice_end) ||
		     READ_ONCE(exobox_host_turn)))
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

/* exec failed before its point of no return: what the host made, let go of (exec.c) */
void exobox_exec_cancel(void);
/*
 * exec of an ELF program whose code is wasm (exobox/elf.c): before the point of no return, its state into *state;
 * after it (the new memory the current one), its segments placed and its
 * interpreter instantiated, auxv's extra entries into aux (at most 14
 * words); or before it, cancelled
 */
struct linux_binprm;
int exobox_elf_exec(struct linux_binprm *bprm, u32 max_pages, void **state);
int exobox_elf_load(void *state, unsigned long *aux);
void exobox_elf_cancel(void *state);

/*
 * A process's memory, its mappings and break (mm.c): kept per mm
 * (mm->context.exobox), a fork's copied as its mm is (init_new_context),
 * written back as the process exits (exit_mm, its memory still the
 * process's; exit_mmap), let go of with the mm.
 */
struct mm_struct;
int exobox_mm_init(struct mm_struct *mm);
void exobox_mm_exiting(struct mm_struct *mm);
void exobox_mm_exit(struct mm_struct *mm);
void exobox_mm_destroy(struct mm_struct *mm);
#else
#include <asm/wasm_imports.h>
#include <linux/errno.h>
static inline bool exobox_syscall_enter(long nr, const unsigned long args[6], long *ret) { return false; }
static inline bool exobox_syscall(struct pt_regs *regs) { return false; }
static inline void exobox_syscall_preempt(void) {}
static inline bool exobox_fork_unwound(void) { return false; }
static inline bool exobox_is_fork(int (*fn)(void *)) { return false; }
static inline void exobox_exec_cancel(void) { wasm_user_compile_abort(); }
struct linux_binprm;
static inline int exobox_elf_exec(struct linux_binprm *bprm, u32 max_pages, void **state) { return -ENOEXEC; }
static inline int exobox_elf_load(void *state, unsigned long *aux) { return -ENOEXEC; }
static inline void exobox_elf_cancel(void *state) {}
struct mm_struct;
static inline int exobox_mm_init(struct mm_struct *mm) { return 0; }
static inline void exobox_mm_exiting(struct mm_struct *mm) {}
static inline void exobox_mm_exit(struct mm_struct *mm) {}
static inline void exobox_mm_destroy(struct mm_struct *mm) {}
#endif

#endif
