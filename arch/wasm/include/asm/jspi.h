/* SPDX-License-Identifier: GPL-2.0-only */
/* CONFIG_WASM_JSPI: every task a coroutine on one event loop (kernel/jspi.c) */
#ifndef _WASM_JSPI_H
#define _WASM_JSPI_H

#include <asm/wasm_imports.h>
#include <linux/types.h>

struct task_struct;
struct mm_struct;

#ifdef CONFIG_WASM_JSPI
/*
 * A program's functions the kernel calls, by their types: through the
 * kernel's table, its slots user.entry's and user.func's (call_indirect
 * checks the type: a wrong one traps).
 */
typedef void (*wasm_user_start_t)(void);		/* _start */
typedef int (*wasm_user_thread_t)(u32 arg);		/* a clone child's (musl's clone_entry) */
typedef void (*wasm_user_handler_t)(int sig);		/* a signal handler */
typedef void (*wasm_user_trampoline_t)(u32 handler, u32 sig);	/* an SA_SIGINFO handler's trampoline */

static inline wasm_user_start_t wasm_user_start(void)
{
	return (wasm_user_start_t)(uintptr_t)wasm_user_entry();
}
static inline wasm_user_thread_t wasm_user_thread(uintptr_t fn)
{
	return (wasm_user_thread_t)(uintptr_t)wasm_user_func(fn);
}
static inline wasm_user_handler_t wasm_user_handler(uintptr_t fn)
{
	return (wasm_user_handler_t)(uintptr_t)wasm_user_func(fn);
}
static inline wasm_user_trampoline_t wasm_user_trampoline(uintptr_t fn)
{
	return (wasm_user_trampoline_t)(uintptr_t)wasm_user_func(fn);
}
#endif

/* run the task's program, through the kernel's own table, until it returns */
void wasm_user_enter(void);
/* exec's restart, the host's export user_restart: the task's new program */
void wasm_user_restart(void);
/* access_remote_vm's: another process's memory, copied by the host */
int wasm_jspi_access_remote_vm(struct mm_struct *mm, unsigned long addr, void *buf,
			       int len, unsigned int gup_flags);

#endif
