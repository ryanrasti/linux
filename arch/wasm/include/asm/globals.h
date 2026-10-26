#ifndef _WASM_GLOBALS_H
#define _WASM_GLOBALS_H

#include <linux/types.h>

__asm__(".globaltype __stack_pointer, i32\n");
static inline void set_stack_pointer(void *ptr)
{
	__asm__ volatile("local.get %0\n"
			 "global.set __stack_pointer" ::"r"(ptr));
}
static inline void *get_stack_pointer(void)
{
	void* ptr;
	__asm__ volatile("global.get __stack_pointer\n"
			 "local.set %0"
			 : "=r"(ptr));
	return ptr;
}

/*
 * fn(arg) at the top of task's kernel stack: for an entry the host runs on
 * a fresh coroutine (a task's start, exec's restart, a trap's fault), where
 * the stack pointer is whatever the last task left. The stack pointer can't
 * move under a frame, so the caller keeps none (no locals), and fn is
 * noinline_for_stack (its frame laid out on the new stack). (task_pt_regs:
 * asm/processor.h, at the use.)
 */
#define wasm_call_on_task_stack(task, fn, arg)			\
	do {							\
		set_stack_pointer(task_pt_regs(task) - 1);	\
		fn(arg);					\
	} while (0)


void set_current_cpu(int cpu);
int get_current_cpu(void);

struct task_struct;
void set_current_task(struct task_struct *task);
struct task_struct *get_current_task(void);
struct task_struct *get_current_task_on(int cpu);

void set_irq_enabled(u32 flags);
u32 get_irq_enabled(void);

void wasm_set_thread_done(int done);
int wasm_get_thread_done(void);

#endif
