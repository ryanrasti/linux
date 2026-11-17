/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LKL_WASI_LIBC_STDLIB_H
#define _LKL_WASI_LIBC_STDLIB_H

/*
 * libc's, for wit-bindgen's C (wasi_bindings.c): the module's one
 * allocator, its host layer's, whose cabi_realloc gives the lists and
 * strings the kernel gets (and copies its own), lkl_ops->mem_free freeing
 * them
 */
#include <linux/types.h>
#include <asm/host_ops.h>

void *cabi_realloc(void *ptr, size_t old_size, size_t align, size_t new_size);

static inline void free(void *ptr)
{
	lkl_ops->mem_free(ptr);
}

#endif
