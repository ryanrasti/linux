/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_ARCHRANDOM_H
#define _ASM_LKL_ARCHRANDOM_H

#include <asm/host_ops.h>

/*
 * The host's random source (e.g. crypto.getRandomValues), as other arches
 * have the CPU's: it seeds the RNG at boot (random.trust_cpu), so getrandom()
 * doesn't wait for entropy we can't collect (no timing jitter here).
 */
static inline size_t __must_check arch_get_random_longs(unsigned long *v, size_t max_longs)
{
	return 0;
}

static inline size_t __must_check arch_get_random_seed_longs(unsigned long *v, size_t max_longs)
{
	if (!lkl_ops->random || lkl_ops->random(v, max_longs * sizeof(*v)))
		return 0;
	return max_longs;
}

#endif
