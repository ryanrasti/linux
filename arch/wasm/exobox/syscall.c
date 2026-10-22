// SPDX-License-Identifier: GPL-2.0-only
/*
 * At a syscall's entry (wasm_syscall), before anything else: fork's rewind
 * and start (fork.c), and userspace's inet sockets made the host's
 * (hostsock.c: socket() redirected to its families, and getsockopt's
 * SO_DOMAIN answered for them).
 */
#include "exobox.h"
#include <linux/syscalls.h>

bool exobox_syscall_enter(long nr, unsigned long args[6], long *ret)
{
	if (exobox_fork_enter(nr, args, ret))
		return true;
	if (nr == __NR_socket)
		args[0] = hostsock_family(args[0], args[1]);
	else if (nr == __NR_getsockopt && hostsock_sol_socket((const long *)args, ret))
		return true;
	return false;
}
