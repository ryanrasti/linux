/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_WASI_H
#define _ASM_LKL_WASI_H

/*
 * WASI 0.2's imports, which the kernel's drivers call themselves (it is
 * linked into the program: a library OS, a component), as the canonical
 * ABI lowers them: handles are i32s, results written to a return area,
 * lists the module's cabi_realloc's (the host's mem_alloc: freed with
 * lkl_ops->mem_free). The names wit-bindgen gives them; the version the
 * module's world's (the host's wit/host.wit, which names every interface
 * the kernel imports).
 */
#include <linux/types.h>

#define WASI_IMPORT(iface, name) \
	__attribute__((import_module(iface "@0.2.12"), import_name(name)))
#define WASI_STREAMS(name) WASI_IMPORT("wasi:io/streams", name)

/* stream-error: last-operation-failed (an error resource), or closed */
struct wasi_stream_error {
	u8 closed;
	u32 error;
};

/* result<_, stream-error> */
struct wasi_ret_stream {
	u8 is_err;
	struct wasi_stream_error err;
};

/* result<list<u8>, stream-error> */
struct wasi_ret_list {
	u8 is_err;
	union {
		struct {
			u8 *ptr;
			u32 len;
		} ok;
		struct wasi_stream_error err;
	};
};

/* result<u64, stream-error> */
struct wasi_ret_u64 {
	u8 is_err;
	union {
		u64 ok;
		struct wasi_stream_error err;
	};
};

WASI_STREAMS("[method]input-stream.read")
void __wasm_import_wasi_io_streams_method_input_stream_read(s32, s64, void *);
WASI_STREAMS("[method]input-stream.subscribe")
s32 __wasm_import_wasi_io_streams_method_input_stream_subscribe(s32);
WASI_STREAMS("[method]output-stream.check-write")
void __wasm_import_wasi_io_streams_method_output_stream_check_write(s32, void *);
WASI_STREAMS("[method]output-stream.write")
void __wasm_import_wasi_io_streams_method_output_stream_write(s32, const void *, u32, void *);
WASI_STREAMS("[method]output-stream.flush")
void __wasm_import_wasi_io_streams_method_output_stream_flush(s32, void *);
WASI_STREAMS("[method]output-stream.subscribe")
s32 __wasm_import_wasi_io_streams_method_output_stream_subscribe(s32);
WASI_STREAMS("[resource-drop]input-stream")
void __wasm_import_wasi_io_streams_input_stream_drop(s32);
WASI_STREAMS("[resource-drop]output-stream")
void __wasm_import_wasi_io_streams_output_stream_drop(s32);
WASI_IMPORT("wasi:io/poll", "[method]pollable.block")
void __wasm_import_wasi_io_poll_method_pollable_block(s32);
WASI_IMPORT("wasi:io/poll", "[resource-drop]pollable")
void __wasm_import_wasi_io_poll_pollable_drop(s32);
WASI_IMPORT("wasi:io/error", "[resource-drop]error")
void __wasm_import_wasi_io_error_error_drop(s32);

/*
 * the drivers' WASI handles dropped, each got again at its next use (the
 * console's streams and pollables, the sockets' network): a boot done
 * ahead of time, the host's snapshot, holds none of the instance's.
 * lkl_wasi_release is the host's call (the kernel's thread then idle).
 */
void lkl_wasi_release(void);
void wasi_sock_network_drop(void);

/* a stream's failure, its error resource dropped */
static inline void wasi_stream_failed(const struct wasi_stream_error *e)
{
	if (!e->closed)
		__wasm_import_wasi_io_error_error_drop(e->error);
}

#endif
