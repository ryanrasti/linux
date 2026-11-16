/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_MMU_CONTEXT_H
#define _ASM_LKL_MMU_CONTEXT_H

#ifdef CONFIG_MMU
static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next,
		      struct task_struct *tsk) {
	// No-op for LKL as it doesn't support multiple user-mode address spaces.
}

/*  Generic hooks for arch_dup_mmap and arch_exit_mmap  */
#include <asm-generic/mm_hooks.h>
#include <asm-generic/mmu_context.h>

#elif defined(CONFIG_LKL_WASI)
/*
 * A process's memory a window of the module's (arch/lkl/mm/window.c): a
 * new mm has none until exec's (lkl_window_exec), a fork's child a copy
 * of its parent's, the window freed with its mappings.
 */
#define init_new_context init_new_context
static inline int init_new_context(struct task_struct *tsk,
				   struct mm_struct *mm)
{
	mm->context.base = 0;
	mm->context.win = NULL;
	return 0;
}

int arch_dup_mmap(struct mm_struct *oldmm, struct mm_struct *mm);
void arch_exit_mmap(struct mm_struct *mm);

static inline bool arch_vma_access_permitted(struct vm_area_struct *vma,
		bool write, bool execute, bool foreign)
{
	return true;
}

static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next,
			     struct task_struct *tsk)
{
}

#include <asm-generic/mmu_context.h>
#else
#include <asm-generic/nommu_context.h>
#endif

#endif /* _ASM_LKL_MMU_CONTEXT_H */
