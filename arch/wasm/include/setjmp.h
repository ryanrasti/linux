/* SPDX-License-Identifier: GPL-2.0 */
/*
 * setjmp/longjmp for CONFIG_WASM_JSPI, lowered by LLVM to wasm exceptions
 * (-mllvm -wasm-enable-sjlj); the runtime is kernel/sjlj.c and sjlj_asm.S.
 */
#ifndef _WASM_SETJMP_H
#define _WASM_SETJMP_H

typedef unsigned long long jmp_buf[4];

int setjmp(void *env) __attribute__((returns_twice));
void longjmp(void *env, int val) __attribute__((noreturn));

#endif
