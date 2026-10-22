/* SPDX-License-Identifier: GPL-2.0-only */
/* exobox's host interface (arch/wasm/exobox/): its hooks in arch/wasm */
#ifndef _WASM_EXOBOX_H
#define _WASM_EXOBOX_H

#include <linux/types.h>

/*
 * at a syscall's entry, its arguments ours to change: true if it's handled
 * here, and returns *ret (syscall.c)
 */
bool exobox_syscall_enter(long nr, unsigned long args[6], long *ret);
/* ...fork's part: its rewind's end, and its start (fork.c) */
bool exobox_fork_enter(long nr, const unsigned long args[6], long *ret);
/* after the program's entry returned: true if it unwound for fork (now rewound) */
bool exobox_fork_unwound(void);
/* a fork child's kernel_clone fn */
int exobox_fork_child(void *unused);

struct file;
/* send a file's bytes to the host to compile (binfmt_wasm.c) */
int wasm_stream_file(struct file *file);
/* exec: a dynamically linked program's libraries, after it (exec.c) */
int exobox_exec_libraries(void);

/* socket()'s family for userspace: the host's sockets' (hostsock.c) */
#ifdef CONFIG_EXOBOX_HOSTSOCK
int hostsock_family(int family, int type);
bool hostsock_sol_socket(const long *params, long *ret);
#else
static inline int hostsock_family(int family, int type) { return family; }
static inline bool hostsock_sol_socket(const long *params, long *ret) { return false; }
#endif

#endif
