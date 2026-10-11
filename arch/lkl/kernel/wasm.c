// SPDX-License-Identifier: GPL-2.0
/*
 * Symbols that the linker script provides on other hosts. wasm-ld has no
 * linker scripts, so define them here.
 *
 * Address ranges that only describe the image layout (text, rodata, init,
 * bss, ...) are left empty: both bounds point at the same byte, so "is this
 * address in X" checks are false and range walks do nothing. The same goes
 * for tables that are not wired up yet (exception table, tracing).
 */
#include <linux/compiler.h>

static char wasm_empty_range[1];

#define EMPTY_RANGE(start, end)						\
	extern char start[1] __alias(wasm_empty_range);			\
	extern char end[1] __alias(wasm_empty_range)

EMPTY_RANGE(_stext, _etext);
EMPTY_RANGE(_sinittext, _einittext);
EMPTY_RANGE(__init_begin, __init_end);
EMPTY_RANGE(_sdata, _edata);
EMPTY_RANGE(__start_rodata, __end_rodata);
EMPTY_RANGE(__bss_start, __bss_stop);
EMPTY_RANGE(__irqentry_text_start, __irqentry_text_end);
EMPTY_RANGE(__softirqentry_text_start, __softirqentry_text_end);
EMPTY_RANGE(__start___ex_table, __stop___ex_table);
EMPTY_RANGE(__start_notes, __stop_notes);
EMPTY_RANGE(__start___modver, __stop___modver);
EMPTY_RANGE(__start_ftrace_events, __stop_ftrace_events);
EMPTY_RANGE(__start_ftrace_eval_maps, __stop_ftrace_eval_maps);
EMPTY_RANGE(__start___trace_bprintk_fmt, __stop___trace_bprintk_fmt);
EMPTY_RANGE(__start___tracepoint_str, __stop___tracepoint_str);
