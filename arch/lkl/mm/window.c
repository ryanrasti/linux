// SPDX-License-Identifier: GPL-2.0
/*
 * A blob's processes, each in a window of the module's memory (the host's:
 * host_ops' window_*), so that one can be copied anywhere as a whole (a
 * fork's child): its program's data and its mappings, which nommu's mmap
 * makes in the window's free ranges here rather than from the page
 * allocator. The process's code adds its window's base to every address it
 * uses (the host runs a program's relative copy where the base isn't 0), and
 * the kernel does too: a user address the kernel reaches is it plus the base
 * (uaccess: lkl_user_base), and the calls on a process's mappings translate
 * theirs (lkl_user_addrs: mmap and its relatives take and give user
 * addresses, the mappings themselves being at their kernel addresses).
 *
 * A window's memory is the host's, fresh (zeros never written) past the
 * high-water mark of what its mappings used: a new mapping there needn't be
 * zeroed, nor read, so it stays unbacked until written.
 */
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/mman.h>
#include <asm/host_ops.h>
#include <asm/window.h>
#include <asm/unistd.h>

struct extent {
	struct list_head node;
	unsigned long start, end;
};

struct lkl_window {
	unsigned long lo, hi;	/* its mappings' range (kernel addresses) */
	/*
	 * how far it's dirty: what a process before wrote (the host's mark)
	 * and what its mappings gave back; past it, zeros (a mapping given
	 * out there needn't be zeroed: memory never written, which costs the
	 * host a fault even to read)
	 */
	unsigned long used;
	struct list_head free;	/* free ranges, in order */
	struct list_head node;	/* on windows */
};

static LIST_HEAD(windows);
static DEFINE_SPINLOCK(windows_lock);

unsigned long lkl_user_base(void)
{
	struct mm_struct *mm = current->mm;

	return mm ? mm->context.base : 0;
}

static struct lkl_window *window_new(unsigned long lo, unsigned long hi)
{
	struct lkl_window *w = kzalloc(sizeof(*w), GFP_KERNEL);
	struct extent *e = kmalloc(sizeof(*e), GFP_KERNEL);

	if (!w || !e) {
		kfree(w);
		kfree(e);
		return NULL;
	}
	w->lo = w->used = lo;
	w->hi = hi;
	INIT_LIST_HEAD(&w->free);
	e->start = lo;
	e->end = hi;
	list_add(&e->node, &w->free);
	spin_lock(&windows_lock);
	list_add(&w->node, &windows);
	spin_unlock(&windows_lock);
	return w;
}

static void window_del(struct lkl_window *w)
{
	struct extent *e, *n;

	spin_lock(&windows_lock);
	list_del(&w->node);
	spin_unlock(&windows_lock);
	list_for_each_entry_safe(e, n, &w->free, node)
		kfree(e);
	kfree(w);
}

/*
 * The memory of the process exec'ing program index: a window, the mm's (a
 * new one: begin_new_exec's). 0, or -errno; no window op (a module without
 * programs), 0 and the mm as nommu's.
 */
int lkl_window_exec(struct mm_struct *mm, unsigned int index)
{
	unsigned long lo, hi, dirty;
	long base;

	if (!lkl_ops->window_exec)
		return 0;
	base = lkl_ops->window_exec(index, &lo, &hi, &dirty);
	if (IS_ERR_VALUE(base))
		return base;
	mm->context.base = base;
	mm->context.win = window_new(lo + base, hi + base);
	if (!mm->context.win) {
		lkl_ops->window_free(base, dirty);
		mm->context.base = 0;
		return -ENOMEM;
	}
	mm->context.win->used = max(lo, dirty) + base;
	return 0;
}


/* whether a page is zeros (64-bit words, 8 at a time: memchr_inv's bytes are slow) */
static bool page_zero(const void *p)
{
	const u64 *w = p, *end = p + PAGE_SIZE;
	u64 or = 0;

	for (; w < end; w += 8)
		or |= w[0] | w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7];
	return !or;
}

/*
 * len bytes (pages) of the mm's window, zeroed: from its highest free
 * range (most often fresh memory, past what was given back), else the first that
 * fits (memory given back, which a break shrunk by mremap may grow over
 * again: taken last); NULL if none fit
 */
void *lkl_window_alloc(struct mm_struct *mm, unsigned long len)
{
	struct lkl_window *w = mm->context.win;
	struct extent *e = list_empty(&w->free) ? NULL :
			   list_last_entry(&w->free, struct extent, node);
	unsigned long p;

	if (!e || e->end - e->start < len) {
		list_for_each_entry(e, &w->free, node)
			if (e->end - e->start >= len)
				break;
		if (list_entry_is_head(e, &w->free, node))
			return NULL;
	}
	p = e->start;
	e->start += len;
	if (e->start == e->end) {
		list_del(&e->node);
		kfree(e);
	}
	/*
	 * (past the dirty mark: zeros already; before it, memory given out
	 * before, most likely backed already: zeroed outright, the host's
	 * memset a memory.fill, faster than any check)
	 */
	if (p < w->used)
		memset((void *)p, 0, min(p + len, w->used) - p);
	return (void *)p;
}

static struct lkl_window *window_of(unsigned long addr)
{
	struct lkl_window *w;

	spin_lock(&windows_lock);
	list_for_each_entry(w, &windows, node) {
		if (addr >= w->lo && addr < w->hi) {
			spin_unlock(&windows_lock);
			return w;
		}
	}
	spin_unlock(&windows_lock);
	return NULL;
}

/* whether [from, to) is a window's (its mm's mappings): then released to it */
bool lkl_window_release(unsigned long from, unsigned long to)
{
	struct lkl_window *w = from < to ? window_of(from) : NULL;
	struct extent *e, *prev = NULL, *n;

	if (!w)
		return false;
	if (to > w->used)
		w->used = to;
	list_for_each_entry(e, &w->free, node) {
		if (e->start >= to)
			break;
		prev = e;
	}
	/* (merged with its neighbours: prev before it, the next after) */
	if (prev && prev->end == from) {
		prev->end = to;
		n = list_next_entry(prev, node);
		if (!list_entry_is_head(n, &w->free, node) && n->start == to) {
			prev->end = n->end;
			list_del(&n->node);
			kfree(n);
		}
		return true;
	}
	n = prev ? list_next_entry(prev, node) : list_first_entry(&w->free, struct extent, node);
	if (!list_entry_is_head(n, &w->free, node) && n->start == to) {
		n->start = from;
		return true;
	}
	e = kmalloc(sizeof(*e), GFP_KERNEL | __GFP_NOFAIL);
	e->start = from;
	e->end = to;
	list_add(&e->node, prev ? &prev->node : &w->free);
	return true;
}


/*
 * a mapping's copy, a fork's, into mm's window (its pages, page-aligned):
 * page by page, only what isn't zero (memory never written, which reads as
 * zeros, stays unbacked); a page of the window past its dirty mark is zeros
 * already
 */
void lkl_window_copy(struct mm_struct *mm, void *to, const void *from, unsigned long len)
{
	unsigned long dirty = mm->context.win->used;

	for (; len >= PAGE_SIZE; to += PAGE_SIZE, from += PAGE_SIZE, len -= PAGE_SIZE) {
		if (!page_zero(from))
			memcpy(to, from, PAGE_SIZE);
		else if ((unsigned long)to < dirty && !page_zero(to))
			memset(to, 0, PAGE_SIZE);
	}
	if (len)
		memcpy(to, from, len);
}

/* whether addr is in a window (its memory zeroed as it's allocated) */
bool lkl_in_window(unsigned long addr)
{
	return window_of(addr) != NULL;
}

/*
 * a fork's child's memory (mm, a copy of oldmm's struct): a window of its
 * own, the program's data copied by the host, its free ranges the
 * parent's; the parent's mappings are copied into it after (nommu's
 * dup_mmap), offset by the bases' difference
 */
int lkl_window_fork(struct mm_struct *mm, struct mm_struct *oldmm)
{
	struct lkl_window *old = oldmm->context.win, *w;
	unsigned long dirty, hi, need = old->hi;
	struct extent *e, *c;
	long base, delta;

	mm->context.win = NULL;
	mm->context.base = 0;
	if (!old || !lkl_ops->window_fork)
		return -EINVAL;
	/* (what the parent's mappings reach: its last free range's start) */
	if (!list_empty(&old->free)) {
		e = list_last_entry(&old->free, struct extent, node);
		if (e->end == old->hi)
			need = e->start;
	}
	base = lkl_ops->window_fork(oldmm->context.base, need - oldmm->context.base,
				    &dirty, &hi);
	if (IS_ERR_VALUE(base))
		return base;
	delta = base - oldmm->context.base;
	/* (the child's window maybe smaller: its free ranges cut at its end) */
	w = window_new(old->lo + delta, hi + base);
	if (!w) {
		lkl_ops->window_free(base, old->lo - oldmm->context.base);
		return -ENOMEM;
	}
	/* (its free ranges the parent's) */
	c = list_first_entry(&w->free, struct extent, node);
	list_del(&c->node);
	kfree(c);
	list_for_each_entry(e, &old->free, node) {
		c = kmalloc(sizeof(*c), GFP_KERNEL);
		if (!c) {
			mm->context.win = w;
			mm->context.base = base;
			return -ENOMEM;
		}
		c->start = e->start + delta;
		c->end = min(e->end + delta, w->hi);
		if (c->start >= c->end) {
			kfree(c);
			continue;
		}
		list_add_tail(&c->node, &w->free);
	}
	/*
	 * (dirty as the host's mark has it: the parent's free ranges are its
	 * own memory's, the child's window never written there)
	 */
	w->used = max(w->lo, dirty + base);
	mm->context.win = w;
	mm->context.base = base;
	return 0;
}

/* the mm's end (exit_mmap): its window back to the host */
void lkl_window_exit(struct mm_struct *mm)
{
	struct lkl_window *w = mm->context.win;

	if (!w)
		return;
	mm->context.win = NULL;
	lkl_ops->window_free(mm->context.base, w->used - mm->context.base);
	window_del(w);
}

/*
 * A system call's arguments that are a process's addresses of its mappings
 * (mmap and its relatives), as the kernel's: the user's plus its base (into
 * args, a copy: a restarted call starts again from the user's); and a
 * result that's one, the user's again.
 */
void lkl_user_addrs(long no, const long *params, long *args)
{
	unsigned long base = lkl_user_base();

	memcpy(args, params, 6 * sizeof(long));
	if (!base)
		return;
	switch (no) {
	case __NR_mremap:
		if (args[3] & MREMAP_FIXED)
			args[4] += base;
		fallthrough;
	case __NR3264_mmap:
	case __NR_brk:
		if (args[0])
			args[0] += base;
		break;
	case __NR_munmap:
	case __NR_mprotect:
	case __NR_msync:
	case __NR_mlock:
	case __NR_mlock2:
	case __NR_munlock:
	case __NR_mincore:
	case __NR_madvise:
		args[0] += base;
		break;
	}
}

long lkl_user_result(long no, long ret)
{
	unsigned long base = lkl_user_base();

	if (!base)
		return ret;
	switch (no) {
	case __NR_brk:
		return ret - base;
	case __NR_mremap:
	case __NR3264_mmap:
		return IS_ERR_VALUE(ret) ? ret : ret - base;
	}
	return ret;
}
