/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_UACCESS_H
#define _ASM_LKL_UACCESS_H

#ifdef CONFIG_LKL_WASI
/*
 * a user address, the kernel's: the current process's plus its base (its
 * memory a window: asm/mmu.h), else the same
 */
unsigned long lkl_user_base(void);
#define __uaccess_addr(p) ((__typeof__(p))((unsigned long)(p) + lkl_user_base()))
#endif

#include <asm-generic/uaccess.h>

#endif
