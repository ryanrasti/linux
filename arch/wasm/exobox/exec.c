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
 * compiled before isn't read or sent again. And exec by reference
 * (CONFIG_EXOBOX_EXEC_REF): a file that names a module by its content's
 * hash (a custom section exobox.ref) runs the host's module of that hash,
 * if it has one, with nothing read past the name.
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
/*
 * 0: the host has the module ref names (an exobox.ref section's contents),
 * compiled, and its memory ready (of at most maximum_memory_pages), as
 * compile_end would; 1: it hasn't (send the file); or a negative errno
 */
int exobox_import(user, exec_ref)(const u8 *ref, u32 ref_len, u32 maximum_memory_pages);

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

#ifdef CONFIG_EXOBOX_EXEC_REF
/* a reference's longest contents (the host's format: "sha256:" and 64 hex digits) */
#define EXEC_REF_MAX 128
#define EXEC_REF_NAME "exobox.ref"
/* the custom sections looked at, from the module's start */
#define EXEC_REF_SECTIONS 4

/* a LEB128 u32 at buf[*at], within len bytes; false if it isn't one */
static bool read_leb_u32(const u8 *buf, u32 len, u32 *at, u32 *value)
{
	u32 v = 0;

	for (int shift = 0; shift < 35 && *at < len; shift += 7) {
		u8 b = buf[(*at)++];

		v |= (u32)(b & 0x7f) << shift;
		if (!(b & 0x80)) {
			*value = v;
			return true;
		}
	}
	return false;
}

/*
 * A module that names another by reference: a custom section exobox.ref
 * among its first sections (after dylink.0, which comes first by the tool
 * conventions), whose contents the host resolves (its format is the
 * host's: packages/exobox's machine.ts). A stub is such a module with
 * nothing but custom sections: no code of its own.
 *
 * 0: the host has the module (nothing more to send); 1: send the file (no
 * reference, or the host hasn't it and the file is a module of its own);
 * -ENOEXEC: a stub whose module the host hasn't; or a negative errno.
 */
static int exobox_exec_ref(struct file *file, loff_t size, u32 max_memory_pages)
{
	/* a section's start: id, size, name's length, name and contents */
	u8 buf[1 + 5 + 5 + sizeof(EXEC_REF_NAME) - 1 + EXEC_REF_MAX];
	bool named = false;
	loff_t at = 8;

	for (int i = 0; i < EXEC_REF_SECTIONS && at < size; i++) {
		loff_t pos = at;
		ssize_t n = kernel_read(file, buf, sizeof(buf), &pos);
		u32 p = 1, section, body, name_len, len;
		int ret;

		if (n < 0)
			return n;
		/* not a custom section: the module's own (if it isn't a stub) */
		if (n < 2 || buf[0] != 0)
			break;
		if (!read_leb_u32(buf, n, &p, &section) || section > size - at - p)
			return 1; /* (not a module: compile_end says so) */
		at += p + section;
		body = p;
		if (!read_leb_u32(buf, n, &p, &name_len) || name_len > section - (p - body))
			return 1;
		if (name_len != sizeof(EXEC_REF_NAME) - 1 || p + name_len > n ||
		    memcmp(buf + p, EXEC_REF_NAME, name_len))
			continue;
		len = section - (p - body) - name_len;
		if (named || len > EXEC_REF_MAX || p + name_len + len > n)
			return 1;
		named = true;
		ret = wasm_user_exec_ref(buf + p + name_len, len, max_memory_pages);
		if (ret <= 0)
			return ret;
		/* (the host hasn't it: on, to see whether the file is a stub) */
	}
	if (!named)
		return 1;
	/* a stub (custom sections alone) whose module the host hasn't */
	return at >= size ? -ENOEXEC : 1;
}
#else
static inline int exobox_exec_ref(struct file *file, loff_t size, u32 max_memory_pages) { return 1; }
#endif

#if defined(CONFIG_EXOBOX_EXEC_CACHE) || defined(CONFIG_EXOBOX_EXEC_REF)
int exobox_exec_cached(struct file *file, loff_t size, u32 max_memory_pages)
{
	int ret = exobox_exec_ref(file, size, max_memory_pages);

	if (ret <= 0)
		return ret;
#ifdef CONFIG_EXOBOX_EXEC_CACHE
	{
		/*
		 * The host may have this file compiled already (same device,
		 * inode, size and mtime): then nothing is read or sent, and it
		 * has the module and its memory ready, as compile_end would.
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
#else
	return 1;
#endif
}
#endif
