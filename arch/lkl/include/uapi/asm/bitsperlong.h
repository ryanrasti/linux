#ifndef _ASM_UAPI_LKL_BITSPERLONG_H
#define _ASM_UAPI_LKL_BITSPERLONG_H

#include <asm/config.h>

#if defined(LKL_CONFIG_64BIT)
#define __BITS_PER_LONG 64
#else
#define __BITS_PER_LONG 32
#endif

/* (wasm32: as riscv32, no stat64 calls: statx; see asm/unistd.h) */
#ifndef LKL_CONFIG_WASM
#define __ARCH_WANT_STAT64
#endif

#ifndef __BITS_PER_LONG_LONG
#define __BITS_PER_LONG_LONG 64
#endif

#endif /* _ASM_UAPI_LKL_BITSPERLONG_H */
