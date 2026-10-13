/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_MMU_H
#define _ASM_LKL_MMU_H

/* asm-generic/mmu.h's, and the domain the process's memory is (exec_domain) */
typedef struct {
	unsigned long end_brk;
	unsigned long domain;
} mm_context_t;

#endif
