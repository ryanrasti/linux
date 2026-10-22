/* SPDX-License-Identifier: GPL-2.0-only */
/* between arch/wasm/exobox/'s parts (its hooks in arch/wasm: asm/exobox.h) */
#ifndef _WASM_EXOBOX_EXOBOX_H
#define _WASM_EXOBOX_EXOBOX_H

#include <asm/exobox.h>

/* a host import of ours (his wasm_imports.h's wasm_import is #undef'd after it) */
#define exobox_import(ns, name) \
	__attribute__((import_module(#ns), import_name(#name))) \
	wasm_##ns##_##name

/* fork's part of a syscall's entry: its rewind's end, and its start (fork.c) */
bool exobox_fork_enter(long nr, const unsigned long args[6], long *ret);

/* socket()'s family for userspace: the host's sockets' (hostsock.c) */
#ifdef CONFIG_EXOBOX_HOSTSOCK
int hostsock_family(int family, int type);
bool hostsock_sol_socket(const long *params, long *ret);
#else
static inline int hostsock_family(int family, int type) { return family; }
static inline bool hostsock_sol_socket(const long *params, long *ret) { return false; }
#endif

#endif
