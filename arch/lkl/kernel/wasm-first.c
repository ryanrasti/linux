// SPDX-License-Identifier: GPL-2.0
/*
 * Linked before every other object, in place of the linker script wasm-ld
 * doesn't have: it lays out data segments in the order their section names
 * first appear among its inputs, so the zero-size markers here put the
 * kernel's own sections in vmlinux.lds's order and define the bounds it
 * would (include/asm-generic/vmlinux.lds.h: INIT_CALLS, CON_INITCALL,
 * INIT_SETUP, SCHED_DATA). Each end is a section of its own, right after the
 * last it bounds.
 *
 * Markers must be zero-size: a dummy obs_kernel_param, say, would match
 * every boot parameter. And a section's entries are 4-byte aligned, so no
 * padding comes between one and the next.
 */
#include <linux/init.h>

#define MARK_AS(type, sym, sec)	type sym[0] __section(sec) __used
#define MARK(sym, sec)		MARK_AS(initcall_entry_t, sym, sec)
/* (pins a section's place, defining nothing anyone uses) */
#define PLACE(sec)		MARK(__UNIQUE_ID(place), sec)

MARK(__initcall_start, ".initcallearly.init");
#define LEVEL(level)							\
	MARK(__initcall##level##_start, ".initcall" #level ".init");	\
	PLACE(".initcall" #level "s.init")
LEVEL(0);
LEVEL(1);
LEVEL(2);
LEVEL(3);
LEVEL(4);
LEVEL(5);
LEVEL(rootfs);
LEVEL(6);
LEVEL(7);
MARK(__initcall_end, ".initcall_end.init");

MARK(__con_initcall_start, ".con_initcall.init");
MARK(__con_initcall_end, ".con_initcall_end.init");

MARK_AS(const struct obs_kernel_param, __setup_start, ".init.setup");
MARK_AS(const struct obs_kernel_param, __setup_end, ".init.setup_end");

/* (sched_init checks the classes' order) */
MARK(__sched_class_highest, "__stop_sched_class");
PLACE("__dl_sched_class");
PLACE("__rt_sched_class");
PLACE("__fair_sched_class");
PLACE("__ext_sched_class");
PLACE("__idle_sched_class");
MARK(__sched_class_lowest, "__sched_class_lowest");

