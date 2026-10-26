// SPDX-License-Identifier: GPL-2.0-only
/*
 * System calls with 64-bit arguments, as a 32-bit wasm program passes
 * them: each in two arguments, low word first (musl's __SYSCALL_LL_E).
 * The syscall wrapper (asm/syscall_wrapper.h) takes one argument a
 * register, so the generic definitions would see a loff_t's low word
 * alone, and every argument after it shifted (fallocate's len: the
 * offset's high word, so EINVAL). As compat's are (fs/read_write.c,
 * arg_u32p), in syscall.c's table in their place.
 */
#include <linux/fs.h>
#include <linux/syscalls.h>
#include <linux/fcntl.h>

#define arg_u64(lo, hi) ((u64)(hi) << 32 | (u32)(lo))

SYSCALL_DEFINE6(wasm_fallocate, int, fd, int, mode, u32, off_lo, u32, off_hi,
		u32, len_lo, u32, len_hi)
{
	return ksys_fallocate(fd, mode, arg_u64(off_lo, off_hi), arg_u64(len_lo, len_hi));
}

SYSCALL_DEFINE6(wasm_fadvise64_64, int, fd, u32, off_lo, u32, off_hi,
		u32, len_lo, u32, len_hi, int, advice)
{
	return ksys_fadvise64_64(fd, arg_u64(off_lo, off_hi), arg_u64(len_lo, len_hi), advice);
}

SYSCALL_DEFINE6(wasm_sync_file_range2, int, fd, unsigned int, flags,
		u32, off_lo, u32, off_hi, u32, nbytes_lo, u32, nbytes_hi)
{
	return ksys_sync_file_range(fd, arg_u64(off_lo, off_hi), arg_u64(nbytes_lo, nbytes_hi), flags);
}

SYSCALL_DEFINE4(wasm_readahead, int, fd, u32, off_lo, u32, off_hi, size_t, count)
{
	return ksys_readahead(fd, arg_u64(off_lo, off_hi), count);
}

SYSCALL_DEFINE5(wasm_pread64, unsigned int, fd, char __user *, buf, size_t, count,
		u32, pos_lo, u32, pos_hi)
{
	return ksys_pread64(fd, buf, count, arg_u64(pos_lo, pos_hi));
}

SYSCALL_DEFINE5(wasm_pwrite64, unsigned int, fd, const char __user *, buf, size_t, count,
		u32, pos_lo, u32, pos_hi)
{
	return ksys_pwrite64(fd, buf, count, arg_u64(pos_lo, pos_hi));
}

SYSCALL_DEFINE3(wasm_truncate64, const char __user *, path, u32, len_lo, u32, len_hi)
{
	return ksys_truncate(path, arg_u64(len_lo, len_hi));
}

SYSCALL_DEFINE3(wasm_ftruncate64, unsigned int, fd, u32, len_lo, u32, len_hi)
{
	return ksys_ftruncate(fd, arg_u64(len_lo, len_hi), FTRUNCATE_LFS);
}
