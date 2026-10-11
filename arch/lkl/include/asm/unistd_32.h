#include <asm/bitsperlong.h>

#ifndef __SYSCALL
#define __SYSCALL(x, y)
#endif

#if __BITS_PER_LONG == 32
__SYSCALL(__NR3264_truncate, sys_32_truncate64)
__SYSCALL(__NR3264_ftruncate, sys_32_ftruncate64)

#ifdef CONFIG_MMU
__SYSCALL(__NR3264_mmap, sys_32_mmap)
#endif

__SYSCALL(__NR_pread64, sys_32_pread64)
__SYSCALL(__NR_pwrite64, sys_32_pwrite64)

__SYSCALL(__NR_readahead, sys_32_readahead)
#ifdef __ARCH_WANT_SYNC_FILE_RANGE2
__SYSCALL(__NR_sync_file_range2, sys_32_sync_file_range2)
#else
__SYSCALL(__NR_sync_file_range, sys_32_sync_file_range)
#endif
/* mm/fadvise.c */
__SYSCALL(__NR3264_fadvise64, sys_32_fadvise64_64)
__SYSCALL(__NR_fallocate, sys_32_fallocate)

#endif
