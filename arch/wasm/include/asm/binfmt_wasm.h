/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _WASM_BINFMT_WASM_H
#define _WASM_BINFMT_WASM_H

struct file;

/*
 * Send a file's bytes to the host, which compiles them (binfmt_wasm.c): an
 * exec's program, or a library of one. 0, or a negative errno.
 */
int wasm_stream_file(struct file *file);

#endif
