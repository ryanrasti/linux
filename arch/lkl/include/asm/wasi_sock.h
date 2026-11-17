/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_WASI_SOCK_H
#define _ASM_LKL_WASI_SOCK_H

/* socket()'s family for userspace: wasi:sockets' (drivers/wasi_sock.c) */
#ifdef CONFIG_LKL_WASI_SOCK
int wasi_sock_family(int family, int type);
#else
static inline int wasi_sock_family(int family, int type) { return family; }
#endif

#endif
