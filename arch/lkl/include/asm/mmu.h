/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_MMU_H
#define _ASM_LKL_MMU_H

#ifdef CONFIG_MMU
#include <asm-generic/mmu.h>
#else
struct lkl_window;

typedef struct {
	unsigned long end_brk;
	/*
	 * A process whose memory is a window of the module's (a blob's
	 * program: arch/lkl/mm/window.c): its addresses are its user
	 * addresses plus base (a fork's copy runs anywhere: its program's
	 * code adds the base too), its mappings made in the window. base 0
	 * and no window: memory as nommu's, anywhere in the kernel's.
	 */
	unsigned long base;
	struct lkl_window *win;
} mm_context_t;
#endif

#endif
