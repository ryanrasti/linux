// SPDX-License-Identifier: GPL-2.0
/*
 * Executing wasm programs. The host runs each program as a wasm instance
 * with its own memory; the kernel does the generic exec work and hands the
 * host the program (the file's contents, which the host keeps under a key
 * that identifies them) and the new argv and envp.
 */
#include <linux/binfmts.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/personality.h>
#include <linux/slab.h>
#include <asm/host_ops.h>

#define CHUNK (64 * 1024)

static int load_wasm_binary(struct linux_binprm *bprm);

static struct linux_binfmt wasm_format = {
	.module		= THIS_MODULE,
	.load_binary	= load_wasm_binary,
};

/* the file's identity and version, as the key of the host's program cache */
static void program_key(struct file *file, char *key, size_t size)
{
	struct inode *inode = file_inode(file);
	struct timespec64 ctime = inode_get_ctime(inode);

	snprintf(key, size, "%u:%lu:%lld:%lld.%09ld", inode->i_sb->s_dev,
		 inode->i_ino, i_size_read(inode), (long long)ctime.tv_sec,
		 ctime.tv_nsec);
}

/* make sure the host has the program, sending it the file if not */
static int load_program(struct file *file, const char *key)
{
	loff_t size = i_size_read(file_inode(file)), pos = 0;
	void *buf;
	int ret;

	ret = lkl_ops->exec_load(key, NULL, 0, 0, size);
	if (ret <= 0)
		return ret;

	buf = kmalloc(CHUNK, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	while (pos < size) {
		loff_t off = pos;
		ssize_t n = kernel_read(file, buf, min_t(loff_t, CHUNK, size - pos), &pos);

		if (n <= 0) {
			ret = n ? n : -EIO;
			break;
		}
		ret = lkl_ops->exec_load(key, buf, off, n, size);
		if (ret < 0)
			break;
	}
	kfree(buf);
	return ret;
}

/*
 * The argv and envp strings, in order: on !MMU they are in bprm's pages, from
 * bprm->p up (argv, then envp, then the filename).
 */
static char *exec_args(struct linux_binprm *bprm, unsigned long *len)
{
	unsigned long top = PAGE_SIZE * MAX_ARG_PAGES - sizeof(void *);
	unsigned long pos;
	char *args;

	*len = top - bprm->p;
	args = kvmalloc(*len, GFP_KERNEL);
	if (!args)
		return NULL;
	for (pos = bprm->p; pos < top; ) {
		unsigned long off = pos % PAGE_SIZE;
		unsigned long n = min(PAGE_SIZE - off, top - pos);

		memcpy(args + pos - bprm->p,
		       page_address(bprm->page[pos / PAGE_SIZE]) + off, n);
		pos += n;
	}
	return args;
}

static int load_wasm_binary(struct linux_binprm *bprm)
{
	static const char magic[] = "\0asm\1\0\0";	/* + final \0 */
	unsigned long len;
	char key[80], *args;
	int ret;

	if (memcmp(bprm->buf, magic, sizeof(magic)) || !lkl_ops->exec ||
	    !test_thread_flag(TIF_USER_TASK))
		return -ENOEXEC;

	program_key(bprm->file, key, sizeof(key));
	ret = load_program(bprm->file, key);
	if (ret)
		return ret;
	args = exec_args(bprm, &len);
	if (!args)
		return -ENOMEM;

	/* failing from here on kills the task (see bprm_execve()) */
	ret = begin_new_exec(bprm);
	if (!ret) {
		set_personality(PER_LINUX);
		setup_new_exec(bprm);
		ret = lkl_ops->exec(key, args, len, bprm->argc, bprm->envc);
	}
	kvfree(args);
	if (ret)
		return ret;
	set_binfmt(&wasm_format);
	finalize_exec(bprm);
	return 0;
}

static int __init wasm_binfmt_init(void)
{
	register_binfmt(&wasm_format);
	return 0;
}
core_initcall(wasm_binfmt_init);
