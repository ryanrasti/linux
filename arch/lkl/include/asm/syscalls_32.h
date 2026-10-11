#ifndef _ASM_SYSCALLS_32_H
#define _ASM_SYSCALLS_32_H

#include <linux/compiler.h>
#include <linux/linkage.h>
#include <linux/types.h>
#include <linux/signal.h>

#if __BITS_PER_LONG == 32 && !defined(CONFIG_ARCH_HAS_SYSCALL_WRAPPER)

/* kernel/syscalls_32.c */
asmlinkage long sys_32_truncate64(const char __user *, unsigned long, unsigned long);
asmlinkage long sys_32_ftruncate64(unsigned int, unsigned long, unsigned long);

#ifdef CONFIG_MMU
struct mmap_arg_struct32;
asmlinkage long sys_32_mmap(struct mmap_arg_struct32 __user *);
#endif

asmlinkage long sys_32_pread64(unsigned int, char __user *, u32, u32, u32);
asmlinkage long sys_32_pwrite64(unsigned int, const char __user *, u32, u32, u32);
asmlinkage long sys_32_fadvise64_64(int, __u32, __u32, __u32, __u32, int);
asmlinkage long sys_32_readahead(int, unsigned int, unsigned int, size_t);
asmlinkage long sys_32_sync_file_range(int, unsigned int, unsigned int,
				       unsigned int, unsigned int, unsigned int);
asmlinkage long sys_32_sync_file_range2(int, unsigned int,
					unsigned int, unsigned int,
					unsigned int, unsigned int);
asmlinkage long sys_32_fallocate(int, int, unsigned int,
				 unsigned int, unsigned int, unsigned int);

#endif /* __BITS_PER_LONG */

#endif /* _ASM_SYSCALLS_32_H */
