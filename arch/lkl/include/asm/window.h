/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_WINDOW_H
#define _ASM_LKL_WINDOW_H

/* a process's memory a window of the module's (arch/lkl/mm/window.c) */
struct mm_struct;

#ifndef CONFIG_LKL_WASI
#include <linux/string.h>
static inline void lkl_user_addrs(long no, const long *params, long *args)
{
	memcpy(args, params, 6 * sizeof(long));
}
#define lkl_user_result(no, ret) (ret)
#define lkl_window_exec(mm, index) 0
#define lkl_window_exec_room() 0
#else

int lkl_window_exec_room(void);
int lkl_window_exec(struct mm_struct *mm, unsigned int index);
int lkl_window_fork(struct mm_struct *mm, struct mm_struct *oldmm);
void lkl_window_exit(struct mm_struct *mm);
void *lkl_window_alloc(struct mm_struct *mm, unsigned long len);
bool lkl_window_release(unsigned long from, unsigned long to);
bool lkl_in_window(unsigned long addr);
void lkl_window_copy(struct mm_struct *mm, void *to, const void *from, unsigned long len);
void lkl_user_addrs(long no, const long *params, long *args);
long lkl_user_result(long no, long ret);
#endif

#endif
