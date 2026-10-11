/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _ASM_LKL_POSIX_TYPES_H
#define _ASM_LKL_POSIX_TYPES_H

#ifdef __wasm32__
/* match the wasm32 C ABI, where size_t is unsigned long */
typedef unsigned long	__kernel_size_t;
typedef long		__kernel_ssize_t;
typedef long		__kernel_ptrdiff_t;
#define __kernel_size_t __kernel_size_t
#endif

#include <asm-generic/posix_types.h>

#endif /* _ASM_LKL_POSIX_TYPES_H */
