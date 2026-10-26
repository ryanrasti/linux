/* SPDX-License-Identifier: GPL-2.0-only */
/* exobox's per-task state (arch/wasm/exobox/), in struct thread_info */
#ifndef _WASM_EXOBOX_THREAD_H
#define _WASM_EXOBOX_THREAD_H

struct exobox_thread_info {
	/* fork: the program is unwinding for it (exobox/fork.c) */
	int forking;
	unsigned long fork_flags, fork_ptid, fork_ctid, fork_tls;
	/* ...or rewinding into the clone call, which then returns rewind_ret */
	int rewinding;
	long rewind_ret;
};

#endif
