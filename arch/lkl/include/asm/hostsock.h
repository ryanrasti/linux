/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_HOSTSOCK_H
#define _ASM_LKL_HOSTSOCK_H

/* socket()'s family for userspace: the host's sockets' (drivers/hostsock.c) */
#ifdef CONFIG_LKL_HOSTSOCK
int hostsock_family(int family, int type);
#else
static inline int hostsock_family(int family, int type) { return family; }
#endif

#endif
