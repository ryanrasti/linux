/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Waiting on a word, and waking its waiters: a task's switch, a new task's
 * wait for a cpu, the idle wait, cpu_relax, delays. memory.atomic.wait and
 * notify, each task's worker a thread of its own; or, with CONFIG_WASM_JSPI
 * (every task a coroutine on one thread, where a wait would block them
 * all), the host's kernel.wait32, wait64 and notify, which park the
 * coroutine on the word and unpark it. The same semantics either way: a
 * wait returns 0 woken, 1 the word not the value expected, 2 timed out
 * (timeout_ns; negative: none); a short timeout (0: cpu_relax's, the
 * preemption point's) is a turn of the host's event loop.
 */
#ifndef _WASM_WASM_WAIT_H
#define _WASM_WASM_WAIT_H

#include <asm/globals.h>
#include <asm/wasm_imports.h>
#include <linux/compiler.h>
#include <linux/types.h>

#ifdef CONFIG_WASM_JSPI
/* (wasm_imports.h's wasm_import is #undef'd after it) */
__attribute__((import_module("kernel"), import_name("wait32")))
int wasm_kernel_wait32(int *addr, int expected, long long timeout_ns);
__attribute__((import_module("kernel"), import_name("wait64")))
int wasm_kernel_wait64(long long *addr, long long expected, long long timeout_ns);
__attribute__((import_module("kernel"), import_name("notify")))
int wasm_kernel_notify(int *addr, int count);

/*
 * (every task shares the instance's globals: the stack pointer is the last
 * task's when this one runs on, so it's kept across)
 */
#define WASM_PARKED(call)				\
	({						\
		void *__sp = get_stack_pointer();	\
		int __ret = (call);			\
		set_stack_pointer(__sp);		\
		__ret;					\
	})

static __always_inline int wasm_wait32(int *addr, int expected, long long timeout_ns)
{
	return WASM_PARKED(wasm_kernel_wait32(addr, expected, timeout_ns));
}
static __always_inline int wasm_wait64(long long *addr, long long expected, long long timeout_ns)
{
	return WASM_PARKED(wasm_kernel_wait64(addr, expected, timeout_ns));
}
static __always_inline int wasm_notify(int *addr, int count)
{
	return wasm_kernel_notify(addr, count);
}
#else
static __always_inline int wasm_wait32(int *addr, int expected, long long timeout_ns)
{
	return __builtin_wasm_memory_atomic_wait32(addr, expected, timeout_ns);
}
static __always_inline int wasm_wait64(long long *addr, long long expected, long long timeout_ns)
{
	return __builtin_wasm_memory_atomic_wait64(addr, expected, timeout_ns);
}
static __always_inline int wasm_notify(int *addr, int count)
{
	return __builtin_wasm_memory_atomic_notify(addr, count);
}
#endif

#endif
