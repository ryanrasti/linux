// SPDX-License-Identifier: GPL-2.0
/*
 * Executing a program linked into the blob module. An executable in the
 * image is a stub: a magic and the program's index. The host starts that
 * program's entry on the task's return to user mode (lkl_ops->exec_prog),
 * with the new argv and envp, on the new memory's stack, in the one shared
 * memory (no instance of its own: a library OS on WASI).
 */
#include <linux/binfmts.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/personality.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <asm/host_ops.h>
#include <asm/syscalls.h>
#include <asm/window.h>

#define WASI_PROG_MAGIC "\0wasiprg"	/* 8 bytes */

/* argv then envp, NUL-terminated, from bprm->p up (as exec.c's exec_args) */
static char *wasi_exec_args(struct linux_binprm *bprm, unsigned long *len)
{
	unsigned long top = PAGE_SIZE * MAX_ARG_PAGES - sizeof(void *);
	unsigned long pos;
	char *args = kvmalloc(top - bprm->p, GFP_KERNEL);

	if (!args)
		return NULL;
	*len = top - bprm->p;
	for (pos = bprm->p; pos < top; ) {
		unsigned long off = pos % PAGE_SIZE;
		unsigned long n = min(PAGE_SIZE - off, top - pos);

		memcpy(args + pos - bprm->p,
		       page_address(bprm->page[pos / PAGE_SIZE]) + off, n);
		pos += n;
	}
	return args;
}

static int load_wasi_prog(struct linux_binprm *bprm)
{
	unsigned int index;
	unsigned long len, stack;
	char *args;
	int ret;

	/* (a program's task only: a kernel thread has no program to replace) */
	if (memcmp(bprm->buf, WASI_PROG_MAGIC, 8) || !lkl_ops->exec_prog ||
	    (current->flags & PF_KTHREAD))
		return -ENOEXEC;
	memcpy(&index, bprm->buf + 8, 4);

	ret = lkl_window_exec_room();
	if (ret)
		return ret;
	args = wasi_exec_args(bprm, &len);
	if (!args)
		return -ENOMEM;
	/*
	 * The old memory goes here (begin_new_exec's exec_mmap), its window
	 * back to the host before the new one is taken, so an exec needs one
	 * window, not two (its old one may be the new: a fork's child's). The
	 * task doesn't run on its old stack again: the host unwinds its old
	 * frames on the kernel stack, then starts the program on the new.
	 */
	ret = begin_new_exec(bprm);
	if (!ret) {
		set_personality(PER_LINUX);
		setup_new_exec(bprm);
		/*
		 * (the new mm a window of the module's: its program's data and
		 * mappings, its break and stack among them; the program run on
		 * that)
		 */
		ret = lkl_window_exec(current->mm, index);
		stack = ret ? ret : lkl_process_mem();
		ret = IS_ERR_VALUE(stack) ? stack :
			lkl_ops->exec_prog(index, args, len, bprm->argc,
					   bprm->envc, stack);
	}
	kvfree(args);
	if (ret)
		return ret;
	finalize_exec(bprm);
	return 0;
}

static struct linux_binfmt wasi_prog_format = {
	.module		= THIS_MODULE,
	.load_binary	= load_wasi_prog,
};

static int __init wasi_prog_init(void)
{
	register_binfmt(&wasi_prog_format);
	return 0;
}
core_initcall(wasi_prog_init);
