/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_HOSTSOCK_H
#define _ASM_LKL_HOSTSOCK_H

/* socket()'s family for userspace: the host's sockets' (drivers/hostsock.c) */
#ifdef CONFIG_LKL_HOSTSOCK
int hostsock_family(int family, int type);
bool hostsock_sol_socket(const long *params, long *ret);
#else
static inline int hostsock_family(int family, int type) { return family; }
static inline bool hostsock_sol_socket(const long *params, long *ret) { return false; }
#endif

#endif
