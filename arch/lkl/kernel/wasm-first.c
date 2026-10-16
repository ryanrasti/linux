// SPDX-License-Identifier: GPL-2.0
/*
 * Linked before every other object (scripts/build-lkl.sh in linux-workers),
 * in place of the linker script wasm-ld doesn't have: it lays out data
 * segments in the order their section names first appear among its inputs,
 * so the zero-size markers here put the kernel's own sections in
 * vmlinux.lds's order and define the bounds it would (include/asm-generic/
 * vmlinux.lds.h: INIT_CALLS, CON_INITCALL, INIT_SETUP, SCHED_DATA). Each
 * end is a section of its own, right after the last it bounds.
 *
 * Markers must be zero-size: a dummy obs_kernel_param, say, would match
 * every boot parameter. And a section's entries are 4-byte aligned, so no
 * padding comes between one and the next.
 */
#include <linux/init.h>
#include <linux/array_size.h>
#include <asm/host_ops.h>
#include <asm/setup.h>
#include <linux/bug.h>

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

/* the first of a range's bad, or NULL: none */
static const char *__init layout_wrong(void)
{
	initcall_entry_t *bounds[] = {
		__initcall_start, __initcall0_start, __initcall1_start,
		__initcall2_start, __initcall3_start, __initcall4_start,
		__initcall5_start, __initcallrootfs_start, __initcall6_start,
		__initcall7_start, __initcall_end,
	};
	const char *setup = (const char *)__setup_start;
	const char *setup_end = (const char *)__setup_end;
	const struct obs_kernel_param *p;
	initcall_entry_t *fn;

	for (int i = 1; i < ARRAY_SIZE(bounds); i++)
		if (bounds[i] < bounds[i - 1])
			return "initcall levels out of order";
	for (fn = __initcall_start; fn < __initcall_end; fn++)
		if (!initcall_from_entry(fn))
			return "an initcall that isn't one";
	if (__con_initcall_end < __con_initcall_start)
		return "console initcalls' bounds";
	for (fn = __con_initcall_start; fn < __con_initcall_end; fn++)
		if (!initcall_from_entry(fn))
			return "a console initcall that isn't one";
	if (setup_end < setup || (setup_end - setup) % sizeof(*p))
		return "__setup's bounds";
	for (p = __setup_start; p < __setup_end; p++)
		if (!p->str || !p->setup_func)
			return "a __setup entry that isn't one";
	return NULL;
}

/*
 * What nothing else checks: the initcall levels' order, and that each
 * range holds only what it should (an entry of another section, or padding,
 * would be called, or parsed, as one). From setup_arch, before the first of
 * them is used (the boot parameters; sched_init checks the classes' order).
 */
void __init wasm_layout_check(void)
{
	const char *wrong = layout_wrong();

	if (!wrong)
		return;
	lkl_printf("lkl: the kernel's sections are out of place (%s): "
		   "wasm-first.o must be linked first (scripts/build-lkl.sh)\n",
		   wrong);
	lkl_ops->panic();
}
