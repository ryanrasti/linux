// SPDX-License-Identifier: GPL-2.0
/*
 * 128-bit multiply libcall. clang supports __int128 on wasm32 but there is
 * no compiler-rt for the kernel to link against.
 */
#include <linux/types.h>

typedef unsigned __int128 u128;

__int128 __multi3(__int128 a, __int128 b);

static u128 mul_64x64(u64 a, u64 b)
{
	u64 a0 = (u32)a, a1 = a >> 32, b0 = (u32)b, b1 = b >> 32;
	u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	u64 mid = (p00 >> 32) + (u32)p01 + (u32)p10;
	u64 lo = (mid << 32) | (u32)p00;
	u64 hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);

	return ((u128)hi << 64) | lo;
}

__int128 __multi3(__int128 a, __int128 b)
{
	u64 al = a, ah = (u128)a >> 64, bl = b, bh = (u128)b >> 64;

	return mul_64x64(al, bl) + ((u128)(al * bh + ah * bl) << 64);
}
