/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_ARCHRANDOM_H
#define _ASM_LKL_ARCHRANDOM_H

#include <linux/types.h>

/*
 * WASI's random source (wasi:random's, which the kernel calls itself), as
 * other arches have the CPU's: it seeds the RNG at boot (random.trust_cpu),
 * so getrandom() doesn't wait for entropy we can't collect (no timing
 * jitter here).
 */
static inline size_t __must_check arch_get_random_longs(unsigned long *v, size_t max_longs)
{
	return 0;
}

#ifdef CONFIG_LKL_WASI
u64 __wasm_import_wasi_random_random_get_random_u64(void)
	__attribute__((import_module("wasi:random/random@0.2.12"), import_name("get-random-u64")));

static inline size_t __must_check arch_get_random_seed_longs(unsigned long *v, size_t max_longs)
{
	size_t i;

	for (i = 0; i < max_longs; i++)
		v[i] = __wasm_import_wasi_random_random_get_random_u64();
	return max_longs;
}
#else
static inline size_t __must_check arch_get_random_seed_longs(unsigned long *v, size_t max_longs)
{
	return 0;
}
#endif

#endif
