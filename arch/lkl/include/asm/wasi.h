/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_WASI_H
#define _ASM_LKL_WASI_H

/*
 * WASI 0.2's imports, which the kernel's drivers call themselves (it is
 * linked into the program: a library OS, a component): wit-bindgen's C
 * bindings (asm/wasi_bindings.h, drivers/wasi_bindings.c), generated from
 * the module's WIT, its kernel world (arch/lkl/scripts/wasi-bindgen.sh).
 * The lists and strings they give are the module's cabi_realloc's (the
 * host's mem_alloc), freed by their _free helpers (lkl_ops->mem_free).
 */
#include <asm/wasi_bindings.h>

/*
 * the drivers' WASI handles dropped, each got again at its next use (the
 * console's streams and pollables, the sockets' network): a boot done
 * ahead of time, the host's snapshot, holds none of the instance's.
 * lkl_wasi_release is the host's call (the kernel's thread then idle).
 */
void lkl_wasi_release(void);
void wasi_sock_network_drop(void);

/* a stream's failure, its error resource dropped */
static inline void wasi_stream_failed(wasi_io_streams_stream_error_t *e)
{
	if (e->tag == WASI_IO_STREAMS_STREAM_ERROR_LAST_OPERATION_FAILED)
		wasi_io_error_error_drop_own(e->val.last_operation_failed);
}

#endif
