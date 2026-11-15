/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_ARCHRANDOM_H
#define _ASM_LKL_ARCHRANDOM_H

#include <linux/types.h>

/*
 * WASI's random source (random_get, which the kernel calls itself), as
 * other arches have the CPU's: it seeds the RNG at boot (random.trust_cpu),
 * so getrandom() doesn't wait for entropy we can't collect (no timing
 * jitter here).
 */
static inline size_t __must_check arch_get_random_longs(unsigned long *v, size_t max_longs)
{
	return 0;
}

#ifdef CONFIG_LKL_WASI
u16 wasi_random_get(void *buf, u32 len)
	__attribute__((import_module("wasi_snapshot_preview1"), import_name("random_get")));

static inline size_t __must_check arch_get_random_seed_longs(unsigned long *v, size_t max_longs)
{
	return wasi_random_get(v, max_longs * sizeof(*v)) ? 0 : max_longs;
}
#else
static inline size_t __must_check arch_get_random_seed_longs(unsigned long *v, size_t max_longs)
{
	return 0;
}
#endif

#endif
