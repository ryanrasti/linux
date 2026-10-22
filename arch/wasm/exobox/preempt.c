// SPDX-License-Identifier: GPL-2.0-only
/*
 * A preemption point at syscalls. Under JSPI the CPU is the host's event
 * loop, and wasm can't be interrupted: a task runs until it suspends. A
 * process whose syscalls all return at once (a server answering from
 * memory, a loop polling) never suspends, so the loop never turns: the
 * host's I/O completions and interrupts (console input, disks, sockets)
 * and its other timers wait until the process blocks or ends.
 *
 * The kernel's own side needs nothing here: wasm_syscall polls the timer
 * deadline at each entry (wasm_timer_check), so the tick, timers and their
 * signals and wakeups happen on time, and syscall exit schedules when the
 * tick or a wakeup asked to (need_resched).
 *
 * So once a time slice, at a syscall's entry, the event loop turns: as
 * cpu_relax does, with interrupts off across the yield, so that what the
 * host completed meanwhile is handled as the interrupts are restored. Then
 * whoever that woke may run before the syscall does (cond_resched): the
 * task is in the kernel, interrupts on, holding nothing, as at any
 * cond_resched point, and from userspace it's as if it had been preempted
 * just before the syscall.
 *
 * The slice is measured by jiffies, a load and a compare per syscall. They
 * are as current as the host's clock to the tick: the timer check above
 * runs the tick at the first syscall past it. A slice is at least 4 ms
 * and at least a tick (10 ms at HZ=100): the loop turns at most once a
 * tick. It's measured from the last turn here, not from the loop's turns
 * when the CPU idled: after an idle, the first syscall turns once more.
 */
#include <asm/exobox.h>
#include <asm/wasm_imports.h>
#include <linux/irqflags.h>
#include <linux/jiffies.h>
#include <linux/sched.h>

#define SLICE_MS 4
/* (rounded up: at least a jiffy) */
#define SLICE_JIFFIES DIV_ROUND_UP(SLICE_MS * HZ, MSEC_PER_SEC)

/* the first slice is from boot (jiffies start at INITIAL_JIFFIES, not 0) */
unsigned long exobox_slice_end = INITIAL_JIFFIES + SLICE_JIFFIES;

void exobox_preempt(void)
{
	unsigned long flags;

	exobox_slice_end = jiffies + SLICE_JIFFIES;
	local_irq_save(flags);
	wasm_kernel_jspi_yield();
	local_irq_restore(flags);
	cond_resched();
}
