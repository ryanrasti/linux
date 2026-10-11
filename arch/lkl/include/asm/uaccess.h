/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_UACCESS_H
#define _ASM_LKL_UACCESS_H

#ifndef CONFIG_UACCESS_MEMCPY
/*
 * User memory may live outside the kernel's address space, e.g. in a
 * separate wasm memory per process, so user copies go through the host.
 * Without the copy host ops, user and kernel memory are one, as with
 * UACCESS_MEMCPY.
 */
#include <linux/string.h>
#include <linux/unaligned.h>
#include <asm/host_ops.h>

static inline __must_check unsigned long
raw_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	if (lkl_ops->copy_from_user)
		return lkl_ops->copy_from_user(to, (const void __force *)from, n);
	memcpy(to, (const void __force *)from, n);
	return 0;
}

static inline __must_check unsigned long
raw_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	if (lkl_ops->copy_to_user)
		return lkl_ops->copy_to_user((void __force *)to, from, n);
	memcpy((void __force *)to, from, n);
	return 0;
}
#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER

static inline __must_check unsigned long
__clear_user(void __user *to, unsigned long n)
{
	static const char zeros[256];

	while (n) {
		unsigned long len = min(n, sizeof(zeros));

		if (raw_copy_to_user(to, zeros, len))
			return n;
		to += len;
		n -= len;
	}
	return 0;
}
#define __clear_user __clear_user

/* kernel memory is always directly accessible; never route it to the host */
#define __get_kernel_nofault(dst, src, type, err_label)			\
do {									\
	*((type *)dst) = get_unaligned((type *)(src));			\
	if (0)								\
		goto err_label;						\
} while (0)

#define __put_kernel_nofault(dst, src, type, err_label)			\
do {									\
	put_unaligned(*((type *)src), (type *)(dst));			\
	if (0)								\
		goto err_label;						\
} while (0)
#endif /* !CONFIG_UACCESS_MEMCPY */

#include <asm-generic/uaccess.h>

#endif /* _ASM_LKL_UACCESS_H */
