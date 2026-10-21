/* SPDX-License-Identifier: GPL-2.0 */
/* longjmp for kernel/sjlj.c: throw __c_longjmp with &jb->arg */
	.tagtype __c_longjmp i32
	.globl __c_longjmp
__c_longjmp:

	.globl __wasm_longjmp
	.type __wasm_longjmp,@function
__wasm_longjmp:
	.functype __wasm_longjmp (i32, i32) -> ()
	local.get 0
	local.get 0
	i32.store 8
	local.get 0
	local.get 1
	i32.const 1
	local.get 1
	i32.select
	i32.store 12
	local.get 0
	i32.const 8
	i32.add
	throw __c_longjmp
	unreachable	# (LLVM 19 asm: throw does not end the stack)
	end_function
