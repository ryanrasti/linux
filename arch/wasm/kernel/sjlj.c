// SPDX-License-Identifier: GPL-2.0
/*
 * Runtime for LLVM's setjmp/longjmp lowering to wasm exceptions
 * (-mllvm -wasm-enable-sjlj), for CONFIG_WASM_JSPI's exec restart. Built
 * without the lowering (Makefile), which declares these itself. The layout
 * is wasi-libc's.
 */
#include <linux/types.h>

struct jmp_buf_impl {
	void *func_invocation_id;
	u32 label;
	struct {
		void *env;
		int val;
	} arg;
};

void __wasm_setjmp(void *env, u32 label, void *func_invocation_id)
{
	struct jmp_buf_impl *jb = env;

	jb->func_invocation_id = func_invocation_id;
	jb->label = label;
}

u32 __wasm_setjmp_test(void *env, void *func_invocation_id)
{
	struct jmp_buf_impl *jb = env;

	return jb->func_invocation_id == func_invocation_id ? jb->label : 0;
}
