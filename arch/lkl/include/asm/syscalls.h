#ifndef _ASM_LKL_SYSCALLS_H
#define _ASM_LKL_SYSCALLS_H

int syscalls_init(void);
void syscalls_init_thread(void);
void syscalls_cleanup(void);
long lkl_syscall(long no, long *params);
/* syscall no as current, whatever task that is (tsi.c's lkl_net_syscall) */
long run_syscall_current(long no, long *params);
bool wakeup_idle_host_task(void);
bool others_runnable(void);
void idle_host_task_idle(void);
void idle_host_task_irq_enter(void);
void idle_host_task_irq_exit(void);
void idle_host_task_busy(void);
long lkl_sys_halt(void);
struct kernel_clone_args;
int copy_fork_thread(struct task_struct *p,
		     const struct kernel_clone_args *args);

/*
 * tsi.c: the handler run_syscall runs for syscall no, given the table's:
 * the program's AF_INET sockets are impersonated (CONFIG_LKL_TSI).
 */
#ifdef CONFIG_LKL_TSI
void *tsi_handler(long no, long *params, void *handler);
#else
static inline void *tsi_handler(long no, long *params, void *handler)
{
	return handler;
}
#endif

#define sys_mmap sys_mmap_pgoff
#define sys_mmap2 sys_mmap_pgoff
#define sys_vfork sys_ni_syscall
#define sys_rt_sigreturn sys_ni_syscall

#include <asm-generic/syscalls.h>

#endif /* _ASM_LKL_SYSCALLS_H */
