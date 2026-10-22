/* SPDX-License-Identifier: GPL-2.0-only */
/* CONFIG_WASM_JSPI: every task a coroutine on one event loop (kernel/jspi.c) */
#ifndef _WASM_JSPI_H
#define _WASM_JSPI_H

struct task_struct;

/* __switch_to's: suspend from's coroutine, to's runs; returns the task switched from */
struct task_struct *wasm_jspi_switch_to(struct task_struct *from,
					struct task_struct *to, int cpu);
/* run the task's program, through the kernel's own table, until it returns */
void wasm_user_enter(void);
/* exec's restart, the host's export user_restart: the task's new program */
void wasm_user_restart(void);

#endif
