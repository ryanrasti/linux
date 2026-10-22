// SPDX-License-Identifier: GPL-2.0-only
/*
 * exec of a dynamically linked program: its libraries (wasm modules with a
 * dylink.0 section), which the host links with it. The host can't read the
 * machine's files, so the kernel sends them, as it sends the program: the
 * host decides which path to try next (the needed libraries breadth first,
 * each in its requester's run paths, then /lib and /usr/lib), the kernel
 * opens it and streams it, and says whether it couldn't.
 *
 * And the exec cache (CONFIG_EXOBOX_EXEC_CACHE): a file the host has
 * compiled before isn't read or sent again.
 */
#include "exobox.h"
#include <asm/wasm_imports.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/slab.h>

/*
 * exec, after the program: the path of the next library to send it, in
 * buf (>0), or 0 (no more), or a negative errno. err: why the last path
 * couldn't be sent (0: it was), for the host to try the next place.
 */
int exobox_import(user, exec_lib)(char *buf, u32 size, int err);
/*
 * 0: the host has key's module compiled, and its memory ready (of at most
 * maximum_memory_pages); 1: compile it (compile_begin...), and cache it
 */
int exobox_import(user, compile_cached)(const char *key, u32 key_len, u32 maximum_memory_pages);

int exobox_exec_libraries(void)
{
	char *path = kmalloc(PATH_MAX, GFP_KERNEL);
	int err = 0, ret;

	if (!path)
		return -ENOMEM;
	while ((ret = wasm_user_exec_lib(path, PATH_MAX, err)) > 0) {
		struct file *file;

		path[PATH_MAX - 1] = 0;
		file = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
		if (IS_ERR(file)) {
			err = PTR_ERR(file);
			continue;
		}
		err = wasm_stream_file(file);
		fput(file);
	}
	kfree(path);
	return ret;
}

#ifdef CONFIG_EXOBOX_EXEC_CACHE
int exobox_exec_cached(struct file *file, loff_t size, u32 max_memory_pages)
{
	/*
	 * The host may have this file compiled already (same device, inode,
	 * size and mtime): then nothing is read or sent, and it has the module
	 * and its memory ready, as compile_end would.
	 */
	struct inode *inode = file_inode(file);
	struct timespec64 mtime = inode_get_mtime(inode);
	char key[96];
	int len = snprintf(key, sizeof(key), "%u:%llu:%lld:%lld.%ld",
			   inode->i_sb->s_dev, (unsigned long long)inode->i_ino, size,
			   (long long)mtime.tv_sec, mtime.tv_nsec);

	/* (1: not cached; compile_end caches it under this key) */
	return wasm_user_compile_cached(key, len, max_memory_pages);
}
#endif
