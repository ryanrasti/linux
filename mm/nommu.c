// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/nommu.c
 *
 *  Replacement code for mm functions to support CPU's that don't
 *  have any form of memory management unit (thus no virtual memory).
 *
 *  See Documentation/admin-guide/mm/nommu-mmap.rst
 *
 *  Copyright (c) 2004-2008 David Howells <dhowells@redhat.com>
 *  Copyright (c) 2000-2003 David McCullough <davidm@snapgear.com>
 *  Copyright (c) 2000-2001 D Jeff Dionne <jeff@uClinux.org>
 *  Copyright (c) 2002      Greg Ungerer <gerg@snapgear.com>
 *  Copyright (c) 2007-2010 Paul Mundt <lethal@linux-sh.org>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/export.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/mman.h>
#include <linux/swap.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/backing-dev.h>
#include <linux/compiler.h>
#include <linux/mount.h>
#include <linux/personality.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/audit.h>
#include <linux/printk.h>

#include <linux/uaccess.h>
#include <linux/uio.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include <asm/mmu_context.h>
#include "internal.h"

/*
 * a process whose memory is a window of the module's (LKL on WASI, a blob's
 * program: arch/lkl/mm/window.c): its mappings made there, not by the page
 * allocator, and copied at fork
 */
#ifdef CONFIG_LKL_WASI
#include <asm/window.h>
#include <asm/host_ops.h>
#define mm_window(mm) ((mm)->context.win)
#else
#define mm_window(mm) NULL
#define lkl_window_alloc(mm, len) NULL
#define lkl_window_release(from, to) false
#define lkl_in_window(addr) false
#define lkl_window_exit(mm) do { } while (0)
#endif

void *high_memory;
EXPORT_SYMBOL(high_memory);
struct page *mem_map;
unsigned long max_mapnr;
EXPORT_SYMBOL(max_mapnr);
unsigned long highest_memmap_pfn;
int sysctl_nr_trim_pages = CONFIG_NOMMU_INITIAL_TRIM_EXCESS;
int heap_stack_gap = 0;

atomic_long_t mmap_pages_allocated;

EXPORT_SYMBOL(mem_map);

/* list of mapped, potentially shareable regions */
static struct kmem_cache *vm_region_jar;
struct rb_root nommu_region_tree = RB_ROOT;
DECLARE_RWSEM(nommu_region_sem);

const struct vm_operations_struct generic_file_vm_ops = {
};

/*
 * Return the total memory allocated for this pointer, not
 * just what the caller asked for.
 *
 * Doesn't have to be accurate, i.e. may have races.
 */
unsigned int kobjsize(const void *objp)
{
	struct page *page;

	/*
	 * If the object we have should not have ksize performed on it,
	 * return size of 0
	 */
	if (!objp || !virt_addr_valid(objp))
		return 0;

	page = virt_to_head_page(objp);

	/*
	 * If the allocator sets PageSlab, we know the pointer came from
	 * kmalloc().
	 */
	if (PageSlab(page))
		return ksize(objp);

	/*
	 * If it's not a compound page, see if we have a matching VMA
	 * region. This test is intentionally done in reverse order,
	 * so if there's no VMA, we still fall through and hand back
	 * PAGE_SIZE for 0-order pages.
	 */
	if (!PageCompound(page)) {
		struct vm_area_struct *vma;

		vma = find_vma(current->mm, (unsigned long)objp);
		if (vma)
			return vma->vm_end - vma->vm_start;
	}

	/*
	 * The ksize() function is only guaranteed to work for pointers
	 * returned by kmalloc(). So handle arbitrary pointers here.
	 */
	return page_size(page);
}

void vfree(const void *addr)
{
	kfree(addr);
}
EXPORT_SYMBOL(vfree);

void *__vmalloc_noprof(unsigned long size, gfp_t gfp_mask)
{
	/*
	 *  You can't specify __GFP_HIGHMEM with kmalloc() since kmalloc()
	 * returns only a logical address.
	 */
	return kmalloc_noprof(size, (gfp_mask | __GFP_COMP) & ~__GFP_HIGHMEM);
}
EXPORT_SYMBOL(__vmalloc_noprof);

void *vrealloc_noprof(const void *p, size_t size, gfp_t flags)
{
	return krealloc_noprof(p, size, (flags | __GFP_COMP) & ~__GFP_HIGHMEM);
}

void *__vmalloc_node_range_noprof(unsigned long size, unsigned long align,
		unsigned long start, unsigned long end, gfp_t gfp_mask,
		pgprot_t prot, unsigned long vm_flags, int node,
		const void *caller)
{
	return __vmalloc_noprof(size, gfp_mask);
}

void *__vmalloc_node_noprof(unsigned long size, unsigned long align, gfp_t gfp_mask,
		int node, const void *caller)
{
	return __vmalloc_noprof(size, gfp_mask);
}

static void *__vmalloc_user_flags(unsigned long size, gfp_t flags)
{
	void *ret;

	ret = __vmalloc(size, flags);
	if (ret) {
		struct vm_area_struct *vma;

		mmap_write_lock(current->mm);
		vma = find_vma(current->mm, (unsigned long)ret);
		if (vma)
			vm_flags_set(vma, VM_USERMAP);
		mmap_write_unlock(current->mm);
	}

	return ret;
}

void *vmalloc_user_noprof(unsigned long size)
{
	return __vmalloc_user_flags(size, GFP_KERNEL | __GFP_ZERO);
}
EXPORT_SYMBOL(vmalloc_user_noprof);

struct page *vmalloc_to_page(const void *addr)
{
	return virt_to_page(addr);
}
EXPORT_SYMBOL(vmalloc_to_page);

unsigned long vmalloc_to_pfn(const void *addr)
{
	return page_to_pfn(virt_to_page(addr));
}
EXPORT_SYMBOL(vmalloc_to_pfn);

long vread_iter(struct iov_iter *iter, const char *addr, size_t count)
{
	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	return copy_to_iter(addr, count, iter);
}

/*
 *	vmalloc  -  allocate virtually contiguous memory
 *
 *	@size:		allocation size
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
void *vmalloc_noprof(unsigned long size)
{
	return __vmalloc_noprof(size, GFP_KERNEL);
}
EXPORT_SYMBOL(vmalloc_noprof);

void *vmalloc_huge_noprof(unsigned long size, gfp_t gfp_mask) __weak __alias(__vmalloc_noprof);

/*
 *	vzalloc - allocate virtually contiguous memory with zero fill
 *
 *	@size:		allocation size
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *	The memory allocated is set to zero.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
void *vzalloc_noprof(unsigned long size)
{
	return __vmalloc_noprof(size, GFP_KERNEL | __GFP_ZERO);
}
EXPORT_SYMBOL(vzalloc_noprof);

/**
 * vmalloc_node - allocate memory on a specific node
 * @size:	allocation size
 * @node:	numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 */
void *vmalloc_node_noprof(unsigned long size, int node)
{
	return vmalloc_noprof(size);
}
EXPORT_SYMBOL(vmalloc_node_noprof);

/**
 * vzalloc_node - allocate memory on a specific node with zero fill
 * @size:	allocation size
 * @node:	numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * The memory allocated is set to zero.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 */
void *vzalloc_node_noprof(unsigned long size, int node)
{
	return vzalloc_noprof(size);
}
EXPORT_SYMBOL(vzalloc_node_noprof);

/**
 * vmalloc_32  -  allocate virtually contiguous memory (32bit addressable)
 *	@size:		allocation size
 *
 *	Allocate enough 32bit PA addressable pages to cover @size from the
 *	page level allocator and map them into contiguous kernel virtual space.
 */
void *vmalloc_32_noprof(unsigned long size)
{
	return __vmalloc_noprof(size, GFP_KERNEL);
}
EXPORT_SYMBOL(vmalloc_32_noprof);

/**
 * vmalloc_32_user - allocate zeroed virtually contiguous 32bit memory
 *	@size:		allocation size
 *
 * The resulting memory area is 32bit addressable and zeroed so it can be
 * mapped to userspace without leaking data.
 *
 * VM_USERMAP is set on the corresponding VMA so that subsequent calls to
 * remap_vmalloc_range() are permissible.
 */
void *vmalloc_32_user_noprof(unsigned long size)
{
	/*
	 * We'll have to sort out the ZONE_DMA bits for 64-bit,
	 * but for now this can simply use vmalloc_user() directly.
	 */
	return vmalloc_user_noprof(size);
}
EXPORT_SYMBOL(vmalloc_32_user_noprof);

void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot)
{
	BUG();
	return NULL;
}
EXPORT_SYMBOL(vmap);

void vunmap(const void *addr)
{
	BUG();
}
EXPORT_SYMBOL(vunmap);

void *vm_map_ram(struct page **pages, unsigned int count, int node)
{
	BUG();
	return NULL;
}
EXPORT_SYMBOL(vm_map_ram);

void vm_unmap_ram(const void *mem, unsigned int count)
{
	BUG();
}
EXPORT_SYMBOL(vm_unmap_ram);

void vm_unmap_aliases(void)
{
}
EXPORT_SYMBOL_GPL(vm_unmap_aliases);

void free_vm_area(struct vm_struct *area)
{
	BUG();
}
EXPORT_SYMBOL_GPL(free_vm_area);

int vm_insert_page(struct vm_area_struct *vma, unsigned long addr,
		   struct page *page)
{
	return -EINVAL;
}
EXPORT_SYMBOL(vm_insert_page);

int vm_insert_pages(struct vm_area_struct *vma, unsigned long addr,
			struct page **pages, unsigned long *num)
{
	return -EINVAL;
}
EXPORT_SYMBOL(vm_insert_pages);

int vm_map_pages(struct vm_area_struct *vma, struct page **pages,
			unsigned long num)
{
	return -EINVAL;
}
EXPORT_SYMBOL(vm_map_pages);

int vm_map_pages_zero(struct vm_area_struct *vma, struct page **pages,
				unsigned long num)
{
	return -EINVAL;
}
EXPORT_SYMBOL(vm_map_pages_zero);

/*
 * zero [addr, addr + len), page by page, writing only the pages that aren't
 * zero already: memory the host never wrote stays untouched (lazily backed:
 * a wasm memory's pages are resident only once written)
 */
static void clear_nonzero(void *addr, unsigned long len)
{
	while (len) {
		unsigned long n = min(len, PAGE_SIZE - offset_in_page(addr));

		if (memchr_inv(addr, 0, n))
			memset(addr, 0, n);
		addr += n;
		len -= n;
	}
}

/*
 * whether a page the page allocator just gave is still as the host gave it:
 * zeros never written (an arch with a lazily backed pool may know)
 */
bool __weak arch_page_fresh(struct page *page)
{
	return false;
}

/*
 * zero a new mapping's pages, just allocated: those not fresh
 * (arch_page_fresh) as clear_nonzero does, the fresh left untouched
 */
static void clear_new_pages(void *addr, unsigned long len)
{
	/* (a window's: zeroed as it was allocated) */
	if (lkl_in_window((unsigned long)addr))
		return;
	for (; len; addr += PAGE_SIZE, len -= min(len, PAGE_SIZE))
		if (!arch_page_fresh(virt_to_page(addr)))
			clear_nonzero(addr, min(len, PAGE_SIZE));
}

/*
 *  sys_brk() for the most part doesn't need the global kernel
 *  lock, except when an application is doing something nasty
 *  like trying to un-brk an area that has already been mapped
 *  to a regular file.  in this case, the unmapping will need
 *  to invoke file system routines that need the global lock.
 */
SYSCALL_DEFINE1(brk, unsigned long, brk)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;

	if (brk < mm->start_brk || brk > mm->context.end_brk)
		return mm->brk;

	if (mm->brk == brk)
		return mm->brk;

	/*
	 * Always allow shrinking brk
	 */
	if (brk <= mm->brk) {
		mm->brk = brk;
		return brk;
	}

	/*
	 * Growing: over the break's own mapping, as far as it's still mapped
	 * (munmap or mremap may have cut it), zeroed as a native break's new
	 * memory is (a shrink left what was there).
	 */
	if (mmap_write_lock_killable(mm))
		return mm->brk;
	vma = vma_lookup(mm, mm->start_brk);
	if (vma && brk <= vma->vm_end && mm->brk >= vma->vm_start) {
		clear_nonzero((void *)mm->brk, brk - mm->brk);
		flush_icache_user_range(mm->brk, brk);
		mm->brk = brk;
	}
	mmap_write_unlock(mm);
	return mm->brk;
}

/*
 * initialise the percpu counter for VM and region record slabs
 */
void __init mmap_init(void)
{
	int ret;

	ret = percpu_counter_init(&vm_committed_as, 0, GFP_KERNEL);
	VM_BUG_ON(ret);
	vm_region_jar = KMEM_CACHE(vm_region, SLAB_PANIC|SLAB_ACCOUNT);
}

/*
 * validate the region tree
 * - the caller must hold the region lock
 */
#ifdef CONFIG_DEBUG_NOMMU_REGIONS
static noinline void validate_nommu_regions(void)
{
	struct vm_region *region, *last;
	struct rb_node *p, *lastp;

	lastp = rb_first(&nommu_region_tree);
	if (!lastp)
		return;

	last = rb_entry(lastp, struct vm_region, vm_rb);
	BUG_ON(last->vm_end <= last->vm_start);
	BUG_ON(last->vm_top < last->vm_end);

	while ((p = rb_next(lastp))) {
		region = rb_entry(p, struct vm_region, vm_rb);
		last = rb_entry(lastp, struct vm_region, vm_rb);

		BUG_ON(region->vm_end <= region->vm_start);
		BUG_ON(region->vm_top < region->vm_end);
		BUG_ON(region->vm_start < last->vm_top);

		lastp = p;
	}
}
#else
static void validate_nommu_regions(void)
{
}
#endif

/*
 * add a region into the global tree
 */
static void add_nommu_region(struct vm_region *region)
{
	struct vm_region *pregion;
	struct rb_node **p, *parent;

	validate_nommu_regions();

	parent = NULL;
	p = &nommu_region_tree.rb_node;
	while (*p) {
		parent = *p;
		pregion = rb_entry(parent, struct vm_region, vm_rb);
		if (region->vm_start < pregion->vm_start)
			p = &(*p)->rb_left;
		else if (region->vm_start > pregion->vm_start)
			p = &(*p)->rb_right;
		else if (pregion == region)
			return;
		else
			BUG();
	}

	rb_link_node(&region->vm_rb, parent, p);
	rb_insert_color(&region->vm_rb, &nommu_region_tree);

	validate_nommu_regions();
}

/*
 * delete a region from the global tree
 */
static void delete_nommu_region(struct vm_region *region)
{
	BUG_ON(!nommu_region_tree.rb_node);

	validate_nommu_regions();
	rb_erase(&region->vm_rb, &nommu_region_tree);
	validate_nommu_regions();
}

/*
 * free a contiguous series of pages
 */
static void free_page_series(unsigned long from, unsigned long to)
{
	if (lkl_window_release(from, to)) {
		atomic_long_sub((to - from) >> PAGE_SHIFT, &mmap_pages_allocated);
		return;
	}
	for (; from < to; from += PAGE_SIZE) {
		struct page *page = virt_to_page((void *)from);

		atomic_long_dec(&mmap_pages_allocated);
		put_page(page);
	}
}

/*
 * release a reference to a region
 * - the caller must hold the region semaphore for writing, which this releases
 * - the region may not have been added to the tree yet, in which case vm_top
 *   will equal vm_start
 */
static void __put_nommu_region(struct vm_region *region)
	__releases(nommu_region_sem)
{
	BUG_ON(!nommu_region_tree.rb_node);

	if (--region->vm_usage == 0) {
		if (region->vm_top > region->vm_start)
			delete_nommu_region(region);
		up_write(&nommu_region_sem);

		if (region->vm_file)
			fput(region->vm_file);

		/* IO memory and memory shared directly out of the pagecache
		 * from ramfs/tmpfs mustn't be released here */
		if (region->vm_flags & VM_MAPPED_COPY)
			free_page_series(region->vm_start, region->vm_top);
		kmem_cache_free(vm_region_jar, region);
	} else {
		up_write(&nommu_region_sem);
	}
}

/*
 * release a reference to a region
 */
static void put_nommu_region(struct vm_region *region)
{
	down_write(&nommu_region_sem);
	__put_nommu_region(region);
}

static void setup_vma_to_mm(struct vm_area_struct *vma, struct mm_struct *mm)
{
	vma->vm_mm = mm;

	/* add the VMA to the mapping */
	if (vma->vm_file) {
		struct address_space *mapping = vma->vm_file->f_mapping;

		i_mmap_lock_write(mapping);
		flush_dcache_mmap_lock(mapping);
		vma_interval_tree_insert(vma, &mapping->i_mmap);
		flush_dcache_mmap_unlock(mapping);
		i_mmap_unlock_write(mapping);
	}
}

/* remove the VMA from its file's mapping (setup_vma_to_mm adds it again) */
static void vma_unlink_file(struct vm_area_struct *vma)
{
	if (vma->vm_file) {
		struct address_space *mapping;
		mapping = vma->vm_file->f_mapping;

		i_mmap_lock_write(mapping);
		flush_dcache_mmap_lock(mapping);
		vma_interval_tree_remove(vma, &mapping->i_mmap);
		flush_dcache_mmap_unlock(mapping);
		i_mmap_unlock_write(mapping);
	}
}

static void cleanup_vma_from_mm(struct vm_area_struct *vma)
{
	vma->vm_mm->map_count--;
	vma_unlink_file(vma);
}

/*
 * delete a VMA from its owning mm_struct and address space
 */
static int delete_vma_from_mm(struct vm_area_struct *vma)
{
	VMA_ITERATOR(vmi, vma->vm_mm, vma->vm_start);

	vma_iter_config(&vmi, vma->vm_start, vma->vm_end);
	if (vma_iter_prealloc(&vmi, NULL)) {
		pr_warn("Allocation of vma tree for process %d failed\n",
		       current->pid);
		return -ENOMEM;
	}
	cleanup_vma_from_mm(vma);

	/* remove from the MM's tree and list */
	vma_iter_clear(&vmi);
	return 0;
}
/*
 * destroy a VMA record
 */
static void drop_snapshot(struct vm_area_struct *vma);

static void delete_vma(struct mm_struct *mm, struct vm_area_struct *vma)
{
	drop_snapshot(vma);
	vma_close(vma);
	if (vma->vm_file)
		fput(vma->vm_file);
	put_nommu_region(vma->vm_region);
	vm_area_free(vma);
}

struct vm_area_struct *find_vma_intersection(struct mm_struct *mm,
					     unsigned long start_addr,
					     unsigned long end_addr)
{
	unsigned long index = start_addr;

	mmap_assert_locked(mm);
	return mt_find(&mm->mm_mt, &index, end_addr - 1);
}
EXPORT_SYMBOL(find_vma_intersection);

/*
 * look up the first VMA in which addr resides, NULL if none
 * - should be called with mm->mmap_lock at least held readlocked
 */
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	VMA_ITERATOR(vmi, mm, addr);

	return vma_iter_load(&vmi);
}
EXPORT_SYMBOL(find_vma);

/*
 * At least xtensa ends up having protection faults even with no
 * MMU.. No stack expansion, at least.
 */
struct vm_area_struct *lock_mm_and_find_vma(struct mm_struct *mm,
			unsigned long addr, struct pt_regs *regs)
{
	struct vm_area_struct *vma;

	mmap_read_lock(mm);
	vma = vma_lookup(mm, addr);
	if (!vma)
		mmap_read_unlock(mm);
	return vma;
}

/*
 * expand a stack to a given address
 * - not supported under NOMMU conditions
 */
int expand_stack_locked(struct vm_area_struct *vma, unsigned long addr)
{
	return -ENOMEM;
}

struct vm_area_struct *expand_stack(struct mm_struct *mm, unsigned long addr)
{
	mmap_read_unlock(mm);
	return NULL;
}

/*
 * determine whether a mapping should be permitted and, if so, what sort of
 * mapping we're capable of supporting
 */
static int validate_mmap_request(struct file *file,
				 unsigned long addr,
				 unsigned long len,
				 unsigned long prot,
				 unsigned long flags,
				 unsigned long pgoff,
				 unsigned long *_capabilities)
{
	unsigned long capabilities, rlen;
	int ret;

	/* do the simple checks first */
	if (flags & MAP_FIXED)
		return -EINVAL;

	if ((flags & MAP_TYPE) != MAP_PRIVATE &&
	    (flags & MAP_TYPE) != MAP_SHARED)
		return -EINVAL;

	if (!len)
		return -EINVAL;

	/* Careful about overflows.. */
	rlen = PAGE_ALIGN(len);
	if (!rlen || rlen > TASK_SIZE)
		return -ENOMEM;

	/* offset overflow? */
	if ((pgoff + (rlen >> PAGE_SHIFT)) < pgoff)
		return -EOVERFLOW;

	if (file) {
		/* files must support mmap */
		if (!file->f_op->mmap)
			return -ENODEV;

		/* work out if what we've got could possibly be shared
		 * - we support chardevs that provide their own "memory"
		 * - we support files/blockdevs that are memory backed
		 */
		if (file->f_op->mmap_capabilities) {
			capabilities = file->f_op->mmap_capabilities(file);
		} else {
			/* no explicit capabilities set, so assume some
			 * defaults */
			switch (file_inode(file)->i_mode & S_IFMT) {
			case S_IFREG:
			case S_IFBLK:
				capabilities = NOMMU_MAP_COPY;
				break;

			case S_IFCHR:
				capabilities =
					NOMMU_MAP_DIRECT |
					NOMMU_MAP_READ |
					NOMMU_MAP_WRITE;
				break;

			default:
				return -EINVAL;
			}
		}

		/* eliminate any capabilities that we can't support on this
		 * device */
		if (!file->f_op->get_unmapped_area)
			capabilities &= ~NOMMU_MAP_DIRECT;
		if (!(file->f_mode & FMODE_CAN_READ))
			capabilities &= ~NOMMU_MAP_COPY;

		/* The file shall have been opened with read permission. */
		if (!(file->f_mode & FMODE_READ))
			return -EACCES;

		if (flags & MAP_SHARED) {
			/* do checks for writing, appending and locking */
			if ((prot & PROT_WRITE) &&
			    !(file->f_mode & FMODE_WRITE))
				return -EACCES;

			if (IS_APPEND(file_inode(file)) &&
			    (file->f_mode & FMODE_WRITE))
				return -EACCES;

			/*
			 * a file that can't be mapped directly is mapped as a
			 * copy, written back to it (nommu_write_back)
			 */
			if (!(capabilities & (NOMMU_MAP_DIRECT | NOMMU_MAP_COPY)))
				return -ENODEV;
		} else {
			/* we're going to read the file into private memory we
			 * allocate */
			if (!(capabilities & NOMMU_MAP_COPY))
				return -ENODEV;

			/*
			 * a private mapping is always a copy of its own, never
			 * the backing device's memory: so it can be split,
			 * moved and dropped as anonymous memory is
			 */
			capabilities &= ~NOMMU_MAP_DIRECT;
		}

		if (capabilities & NOMMU_MAP_DIRECT) {
			if (((prot & PROT_READ)  && !(capabilities & NOMMU_MAP_READ))  ||
			    ((prot & PROT_WRITE) && !(capabilities & NOMMU_MAP_WRITE)) ||
			    ((prot & PROT_EXEC)  && !(capabilities & NOMMU_MAP_EXEC))
			    ) {
				capabilities &= ~NOMMU_MAP_DIRECT;
				if ((flags & MAP_SHARED) &&
				    !(capabilities & NOMMU_MAP_COPY)) {
					pr_warn("MAP_SHARED not completely supported on !MMU\n");
					return -EINVAL;
				}
			}
		}

		/* handle executable mappings and implied executable
		 * mappings */
		if (path_noexec(&file->f_path)) {
			if (prot & PROT_EXEC)
				return -EPERM;
		} else if ((prot & PROT_READ) && !(prot & PROT_EXEC)) {
			/* handle implication of PROT_EXEC by PROT_READ */
			if (current->personality & READ_IMPLIES_EXEC) {
				if (capabilities & NOMMU_MAP_EXEC)
					prot |= PROT_EXEC;
			}
		} else if ((prot & PROT_READ) &&
			 (prot & PROT_EXEC) &&
			 !(capabilities & NOMMU_MAP_EXEC)
			 ) {
			/* backing file is not executable, try to copy */
			capabilities &= ~NOMMU_MAP_DIRECT;
		}
	} else {
		/* anonymous mappings are always memory backed and can be
		 * privately mapped
		 */
		capabilities = NOMMU_MAP_COPY;

		/* handle PROT_EXEC implication by PROT_READ */
		if ((prot & PROT_READ) &&
		    (current->personality & READ_IMPLIES_EXEC))
			prot |= PROT_EXEC;
	}

	/* allow the security API to have its say */
	ret = security_mmap_addr(addr);
	if (ret < 0)
		return ret;

	/* looks okay */
	*_capabilities = capabilities;
	return 0;
}

/*
 * we've determined that we can make the mapping, now translate what we
 * now know into VMA flags
 */
static unsigned long determine_vm_flags(struct file *file,
					unsigned long prot,
					unsigned long flags,
					unsigned long capabilities)
{
	unsigned long vm_flags;

	vm_flags = calc_vm_prot_bits(prot, 0) | calc_vm_flag_bits(file, flags);

	if (!file) {
		/*
		 * MAP_ANONYMOUS. MAP_SHARED is mapped to MAP_PRIVATE, because
		 * there is no fork(); but VM_SHARED, so MADV_DONTNEED keeps it.
		 */
		vm_flags |= VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;
		if (flags & MAP_SHARED)
			vm_flags |= VM_SHARED;
	} else if (flags & MAP_PRIVATE) {
		/* MAP_PRIVATE file mapping: a copy (validate_mmap_request) */
		vm_flags |= VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;
	} else if (capabilities & NOMMU_MAP_DIRECT) {
		/* MAP_SHARED file mapping, of the file's own memory */
		vm_flags |= VM_SHARED | VM_MAYSHARE |
			    (capabilities & NOMMU_VMFLAGS);
	} else {
		/*
		 * MAP_SHARED file mapping, a copy written back to the file
		 * (nommu_write_back): shared with no other mapping
		 */
		vm_flags |= VM_SHARED | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;
	}

	return vm_flags;
}

/*
 * set up a shared mapping on a file (the driver or filesystem provides and
 * pins the storage)
 */
static int do_mmap_shared_file(struct vm_area_struct *vma)
{
	int ret;

	ret = mmap_file(vma->vm_file, vma);
	if (ret == 0) {
		vma->vm_region->vm_top = vma->vm_region->vm_end;
		return 0;
	}
	if (ret != -ENOSYS)
		return ret;

	/* getting -ENOSYS indicates that direct mmap isn't possible (as
	 * opposed to tried but failed) so we can only give a suitable error as
	 * it's not possible to make a private copy if MAP_SHARED was given */
	return -ENODEV;
}

/*
 * set up a private mapping or an anonymous shared mapping
 */
static int do_mmap_private(struct vm_area_struct *vma,
			   struct vm_region *region,
			   unsigned long len,
			   unsigned long capabilities)
{
	unsigned long total, point;
	void *base;
	int ret, order;

	/*
	 * Invoke the file's mapping function so that it can keep track of
	 * shared mappings on devices or memory. VM_MAYOVERLAY will be set if
	 * it may attempt to share, which will make is_nommu_shared_mapping()
	 * happy.
	 */
	if (capabilities & NOMMU_MAP_DIRECT) {
		ret = mmap_file(vma->vm_file, vma);
		/* shouldn't return success if we're not sharing */
		if (WARN_ON_ONCE(!is_nommu_shared_mapping(vma->vm_flags)))
			ret = -ENOSYS;
		if (ret == 0) {
			vma->vm_region->vm_top = vma->vm_region->vm_end;
			return 0;
		}
		if (ret != -ENOSYS)
			return ret;

		/* getting an ENOSYS error indicates that direct mmap isn't
		 * possible (as opposed to tried but failed) so we'll try to
		 * make a private copy of the data and map that instead */
	}


	/* allocate some memory to hold the mapping
	 * - note that this may not return a page-aligned address if the object
	 *   we're allocating is smaller than a page
	 */
	order = get_order(len);
	total = 1 << order;
	point = len >> PAGE_SHIFT;

	/*
	 * we don't want to allocate a power-of-2 sized page set; but a
	 * mapping mremap moved (VM_MAPPED_COPY already) keeps it, to grow
	 * into in place
	 */
	if (sysctl_nr_trim_pages && total - point >= sysctl_nr_trim_pages &&
	    !(vma->vm_flags & VM_MAPPED_COPY))
		total = point;

	if (mm_window(current->mm))
		base = lkl_window_alloc(current->mm, total << PAGE_SHIFT);
	else
		base = alloc_pages_exact(total << PAGE_SHIFT, GFP_KERNEL);
	if (!base)
		goto enomem;

	atomic_long_add(total, &mmap_pages_allocated);

	vm_flags_set(vma, VM_MAPPED_COPY);
	region->vm_flags = vma->vm_flags;
	region->vm_start = (unsigned long) base;
	region->vm_end   = region->vm_start + len;
	region->vm_top   = region->vm_start + (total << PAGE_SHIFT);

	vma->vm_start = region->vm_start;
	vma->vm_end   = region->vm_start + len;

	if (vma->vm_file) {
		/* read the contents of a file into the copy */
		loff_t fpos;

		fpos = vma->vm_pgoff;
		fpos <<= PAGE_SHIFT;

		ret = kernel_read(vma->vm_file, base, len, &fpos);
		if (ret < 0)
			goto error_free;

		/* clear the last little bit */
		if (ret < len)
			memset(base + ret, 0, len - ret);

	} else {
		vma_set_anonymous(vma);
	}

	return 0;

error_free:
	free_page_series(region->vm_start, region->vm_top);
	region->vm_start = vma->vm_start = 0;
	region->vm_end   = vma->vm_end = 0;
	region->vm_top   = 0;
	return ret;

enomem:
	pr_err("Allocation of length %lu from process %d (%s) failed\n",
	       len, current->pid, current->comm);
	show_mem();
	return -ENOMEM;
}

/*
 * whether a mapping is memory of its own, the only one of its region: it can
 * be split, cut, grown and moved as anonymous memory is (an anonymous or
 * private file mapping, or a shared file mapping's copy)
 */
static bool vma_is_own_copy(struct vm_area_struct *vma)
{
	return (vma->vm_flags & VM_MAPPED_COPY) && vma->vm_region->vm_usage == 1;
}

/* a shared file mapping that's a copy: written back to the file */
static bool vma_is_shared_copy(struct vm_area_struct *vma)
{
	return vma->vm_file && (vma->vm_flags & VM_SHARED) &&
	       (vma->vm_flags & VM_MAPPED_COPY);
}

/*
 * [addr, addr + len) of a file mapping: the file's bytes at pos, zeros past
 * its end
 */
static int read_file_into(struct file *file, unsigned long addr,
			  unsigned long len, loff_t pos)
{
	ssize_t ret = kernel_read(file, (void *)addr, len, &pos);

	if (ret < 0)
		return ret;
	if (ret < len)
		memset((void *)addr + ret, 0, len - ret);
	return 0;
}

/*
 * A shared file mapping that's a copy (its file has no memory to map
 * directly), written back: what of [start, end) is within the file, at the
 * mapping's offset, by its file (an O_APPEND one's too: where it was
 * mapped). At msync, munmap, mremap, and exit or exec (exit_mmap); the
 * file's readers see the mapping's writes then, and a write by another
 * meanwhile is overwritten. Only once the mapping was writable.
 */
static int write_back_range(struct vm_area_struct *vma, unsigned long start,
			    unsigned long end)
{
	struct file *file = vma->vm_file;
	loff_t pos, size;
	struct kiocb kiocb;
	struct iov_iter iter;
	struct kvec kv;
	ssize_t ret = 0;

	if (!vma_is_shared_copy(vma) || !(vma->vm_flags & VM_WRITE) ||
	    !(file->f_mode & FMODE_WRITE) || !file->f_op->write_iter)
		return 0;
	start = max(start, vma->vm_start);
	end = min(end, vma->vm_end);
	pos = ((loff_t)vma->vm_pgoff << PAGE_SHIFT) + (start - vma->vm_start);
	size = i_size_read(file_inode(file));
	if (start >= end || pos >= size)
		return 0;
	end = start + min_t(loff_t, end - start, size - pos);
	file_start_write(file);
	while (start < end) {
		kv.iov_base = (void *)start;
		kv.iov_len = end - start;
		iov_iter_kvec(&iter, ITER_SOURCE, &kv, 1, kv.iov_len);
		init_sync_kiocb(&kiocb, file);
		kiocb.ki_flags &= ~IOCB_APPEND;
		kiocb.ki_pos = pos;
		ret = file->f_op->write_iter(&kiocb, &iter);
		if (ret <= 0)
			break;
		start += ret;
		pos += ret;
	}
	file_end_write(file);
	return ret < 0 ? ret : 0;
}

/*
 * A fork's child's copy of a shared file mapping (in a window: LKL on WASI)
 * has a snapshot of it as the fork made it (vm_private_data): its parent's
 * bytes, which the parent writes back. Only the pages the child changed
 * since are its to write back (as natively, where both see the one page),
 * and the snapshot then follows them.
 */
static int nommu_write_back(struct vm_area_struct *vma, unsigned long start,
			    unsigned long end)
{
	char *snap = vma->vm_private_data;
	unsigned long p, run = 0;
	int ret = 0, r;

	if (!snap || !vma_is_shared_copy(vma))
		return write_back_range(vma, start, end);
	start = max(start, vma->vm_start) & PAGE_MASK;
	end = min(end, vma->vm_end);
	for (p = start; p < end; p += PAGE_SIZE) {
		unsigned long n = min(end - p, PAGE_SIZE);
		char *s = snap + (p - vma->vm_start);

		if (memcmp((void *)p, s, n)) {
			memcpy(s, (void *)p, n);
			if (!run)
				run = p;
			continue;
		}
		if (run) {
			r = write_back_range(vma, run, p);
			ret = ret ?: r;
			run = 0;
		}
	}
	if (run) {
		r = write_back_range(vma, run, end);
		ret = ret ?: r;
	}
	return ret;
}

/* (its snapshot dropped: whole again, its write-back all of it) */
static void drop_snapshot(struct vm_area_struct *vma)
{
	if (vma->vm_private_data && vma_is_shared_copy(vma)) {
		kvfree(vma->vm_private_data);
		vma->vm_private_data = NULL;
	}
}

/*
 * MAP_FIXED: only over memory of the process's own already there (as
 * InnoDB commits the buffer pool it reserved): it is the new mapping then,
 * zeroed or the file's bytes read in. A shared file mapping can't be made
 * so, nor one anywhere else (no MMU to place it).
 */
static unsigned long do_mmap_fixed(struct file *file, unsigned long addr,
				   unsigned long len, unsigned long prot,
				   unsigned long flags, unsigned long pgoff)
{
	struct vm_area_struct *vma;
	int ret;

	len = PAGE_ALIGN(len);
	if (offset_in_page(addr) || !len || addr + len < addr)
		return -EINVAL;
	if (file && (flags & MAP_TYPE) != MAP_PRIVATE)
		return -EINVAL;
	if (file && !(file->f_mode & FMODE_READ))
		return -EACCES;
	vma = vma_lookup(current->mm, addr);
	if (!vma || addr + len > vma->vm_end || !vma_is_own_copy(vma) ||
	    vma_is_shared_copy(vma))
		return -EINVAL;
	if (file) {
		ret = read_file_into(file, addr, len, (loff_t)pgoff << PAGE_SHIFT);
		if (ret)
			return ret;
	} else {
		clear_nonzero((void *)addr, len);
	}
	vm_flags_set(vma, calc_vm_prot_bits(prot, 0));
	return addr;
}

/*
 * handle mapping creation for uClinux
 */
unsigned long do_mmap(struct file *file,
			unsigned long addr,
			unsigned long len,
			unsigned long prot,
			unsigned long flags,
			vm_flags_t vm_flags,
			unsigned long pgoff,
			unsigned long *populate,
			struct list_head *uf)
{
	struct vm_area_struct *vma;
	struct vm_region *region;
	struct rb_node *rb;
	unsigned long capabilities, result;
	int ret;
	VMA_ITERATOR(vmi, current->mm, 0);

	*populate = 0;

	/* MAP_SHARED_VALIDATE: no flag past the legacy ones (MAP_SYNC: no DAX) */
	if ((flags & MAP_TYPE) == MAP_SHARED_VALIDATE) {
		if (flags & ~LEGACY_MAP_MASK)
			return -EOPNOTSUPP;
		flags = (flags & ~MAP_TYPE) | MAP_SHARED;
	}

	if (flags & MAP_FIXED)
		return do_mmap_fixed(file, addr, len, prot, flags, pgoff);

	/* decide whether we should attempt the mapping, and if so what sort of
	 * mapping */
	ret = validate_mmap_request(file, addr, len, prot, flags, pgoff,
				    &capabilities);
	if (ret < 0)
		return ret;

	/* we ignore the address hint */
	addr = 0;
	len = PAGE_ALIGN(len);

	/*
	 * a process in a window: its mappings all in it (a fork copies the
	 * window), so a shared file mapping is a copy written back (as where
	 * the file can't be mapped directly), never the file's own memory
	 */
	if (mm_window(current->mm) && file && (capabilities & NOMMU_MAP_DIRECT)) {
		if (!(capabilities & NOMMU_MAP_COPY))
			return -ENODEV;
		capabilities &= ~NOMMU_MAP_DIRECT;
	}

	/* we've determined that we can make the mapping, now translate what we
	 * now know into VMA flags */
	vm_flags |= determine_vm_flags(file, prot, flags, capabilities);


	/* we're going to need to record the mapping */
	region = kmem_cache_zalloc(vm_region_jar, GFP_KERNEL);
	if (!region)
		goto error_getting_region;

	vma = vm_area_alloc(current->mm);
	if (!vma)
		goto error_getting_vma;

	region->vm_usage = 1;
	region->vm_flags = vm_flags;
	region->vm_pgoff = pgoff;

	vm_flags_init(vma, vm_flags);
	vma->vm_pgoff = pgoff;

	if (file) {
		region->vm_file = get_file(file);
		vma->vm_file = get_file(file);
	}

	down_write(&nommu_region_sem);

	/* if we want to share, we need to check for regions created by other
	 * mmap() calls that overlap with our proposed mapping
	 * - we can only share with a superset match on most regular files
	 * - shared mappings on character devices and memory backed files are
	 *   permitted to overlap inexactly as far as we are concerned for in
	 *   these cases, sharing is handled in the driver or filesystem rather
	 *   than here
	 */
	if (is_nommu_shared_mapping(vm_flags)) {
		struct vm_region *pregion;
		unsigned long pglen, rpglen, pgend, rpgend, start;

		pglen = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
		pgend = pgoff + pglen;

		for (rb = rb_first(&nommu_region_tree); rb; rb = rb_next(rb)) {
			pregion = rb_entry(rb, struct vm_region, vm_rb);

			if (!is_nommu_shared_mapping(pregion->vm_flags))
				continue;

			/* search for overlapping mappings on the same file */
			if (file_inode(pregion->vm_file) !=
			    file_inode(file))
				continue;

			if (pregion->vm_pgoff >= pgend)
				continue;

			rpglen = pregion->vm_end - pregion->vm_start;
			rpglen = (rpglen + PAGE_SIZE - 1) >> PAGE_SHIFT;
			rpgend = pregion->vm_pgoff + rpglen;
			if (pgoff >= rpgend)
				continue;

			/* handle inexactly overlapping matches between
			 * mappings */
			if ((pregion->vm_pgoff != pgoff || rpglen != pglen) &&
			    !(pgoff >= pregion->vm_pgoff && pgend <= rpgend)) {
				/* new mapping is not a subset of the region */
				if (!(capabilities & NOMMU_MAP_DIRECT))
					goto sharing_violation;
				continue;
			}

			/* we've found a region we can share */
			pregion->vm_usage++;
			vma->vm_region = pregion;
			start = pregion->vm_start;
			start += (pgoff - pregion->vm_pgoff) << PAGE_SHIFT;
			vma->vm_start = start;
			vma->vm_end = start + len;

			if (pregion->vm_flags & VM_MAPPED_COPY)
				vm_flags_set(vma, VM_MAPPED_COPY);
			else {
				ret = do_mmap_shared_file(vma);
				if (ret < 0) {
					vma->vm_region = NULL;
					vma->vm_start = 0;
					vma->vm_end = 0;
					pregion->vm_usage--;
					pregion = NULL;
					goto error_just_free;
				}
			}
			fput(region->vm_file);
			kmem_cache_free(vm_region_jar, region);
			region = pregion;
			result = start;
			goto share;
		}

		/* obtain the address at which to make a shared mapping
		 * - this is the hook for quasi-memory character devices to
		 *   tell us the location of a shared mapping
		 */
		if (capabilities & NOMMU_MAP_DIRECT) {
			addr = file->f_op->get_unmapped_area(file, addr, len,
							     pgoff, flags);
			if (IS_ERR_VALUE(addr)) {
				ret = addr;
				if (ret != -ENOSYS)
					goto error_just_free;

				/* the driver refused to tell us where to site
				 * the mapping so we'll have to attempt to copy
				 * it */
				ret = -ENODEV;
				if (!(capabilities & NOMMU_MAP_COPY))
					goto error_just_free;

				/* (a shared mapping: a copy, written back) */
				capabilities &= ~NOMMU_MAP_DIRECT;
				vm_flags_clear(vma, VM_MAYSHARE);
				region->vm_flags = vma->vm_flags;
			} else {
				vma->vm_start = region->vm_start = addr;
				vma->vm_end = region->vm_end = addr + len;
			}
		}
	}

	vma->vm_region = region;

	/* set up the mapping
	 * - the region is filled in if NOMMU_MAP_DIRECT is still set
	 */
	if (file && vma->vm_flags & VM_MAYSHARE)
		ret = do_mmap_shared_file(vma);
	else
		ret = do_mmap_private(vma, region, len, capabilities);
	if (ret < 0)
		goto error_just_free;
	add_nommu_region(region);

	/* clear anonymous mappings that don't ask for uninitialized data */
	if (!vma->vm_file &&
	    (!IS_ENABLED(CONFIG_MMAP_ALLOW_UNINITIALIZED) ||
	     !(flags & MAP_UNINITIALIZED)))
		clear_new_pages((void *)region->vm_start,
				region->vm_end - region->vm_start);

	/* okay... we have a mapping; now we have to register it */
	result = vma->vm_start;

	current->mm->total_vm += len >> PAGE_SHIFT;

share:
	BUG_ON(!vma->vm_region);
	vma_iter_config(&vmi, vma->vm_start, vma->vm_end);
	if (vma_iter_prealloc(&vmi, vma))
		goto error_just_free;

	setup_vma_to_mm(vma, current->mm);
	current->mm->map_count++;
	/* add the VMA to the tree */
	vma_iter_store(&vmi, vma);

	/* we flush the region from the icache only when the first executable
	 * mapping of it is made  */
	if (vma->vm_flags & VM_EXEC && !region->vm_icache_flushed) {
		flush_icache_user_range(region->vm_start, region->vm_end);
		region->vm_icache_flushed = true;
	}

	up_write(&nommu_region_sem);

	return result;

error_just_free:
	up_write(&nommu_region_sem);
error:
	vma_iter_free(&vmi);
	if (region->vm_file)
		fput(region->vm_file);
	kmem_cache_free(vm_region_jar, region);
	if (vma->vm_file)
		fput(vma->vm_file);
	vm_area_free(vma);
	return ret;

sharing_violation:
	up_write(&nommu_region_sem);
	pr_warn("Attempt to share mismatched mappings\n");
	ret = -EINVAL;
	goto error;

error_getting_vma:
	kmem_cache_free(vm_region_jar, region);
	pr_warn("Allocation of vma for %lu byte allocation from process %d failed\n",
			len, current->pid);
	show_mem();
	return -ENOMEM;

error_getting_region:
	pr_warn("Allocation of vm region for %lu byte allocation from process %d failed\n",
			len, current->pid);
	show_mem();
	return -ENOMEM;
}

unsigned long ksys_mmap_pgoff(unsigned long addr, unsigned long len,
			      unsigned long prot, unsigned long flags,
			      unsigned long fd, unsigned long pgoff)
{
	struct file *file = NULL;
	unsigned long retval = -EBADF;

	audit_mmap_fd(fd, flags);
	if (!(flags & MAP_ANONYMOUS)) {
		file = fget(fd);
		if (!file)
			goto out;
	}

	retval = vm_mmap_pgoff(file, addr, len, prot, flags, pgoff);

	if (file)
		fput(file);
out:
	return retval;
}

SYSCALL_DEFINE6(mmap_pgoff, unsigned long, addr, unsigned long, len,
		unsigned long, prot, unsigned long, flags,
		unsigned long, fd, unsigned long, pgoff)
{
	return ksys_mmap_pgoff(addr, len, prot, flags, fd, pgoff);
}

#ifdef __ARCH_WANT_SYS_OLD_MMAP
struct mmap_arg_struct {
	unsigned long addr;
	unsigned long len;
	unsigned long prot;
	unsigned long flags;
	unsigned long fd;
	unsigned long offset;
};

SYSCALL_DEFINE1(old_mmap, struct mmap_arg_struct __user *, arg)
{
	struct mmap_arg_struct a;

	if (copy_from_user(&a, arg, sizeof(a)))
		return -EFAULT;
	if (offset_in_page(a.offset))
		return -EINVAL;

	return ksys_mmap_pgoff(a.addr, a.len, a.prot, a.flags, a.fd,
			       a.offset >> PAGE_SHIFT);
}
#endif /* __ARCH_WANT_SYS_OLD_MMAP */

/*
 * split a vma into two pieces at address 'addr', a new vma is allocated either
 * for the first part or the tail.
 */
static int split_vma(struct vma_iterator *vmi, struct vm_area_struct *vma,
		     unsigned long addr, int new_below)
{
	struct vm_area_struct *new;
	struct vm_region *region;
	unsigned long npages;
	struct mm_struct *mm;

	/* we're only permitted to split memory of the mapping's own (the
	 * only usage of its region: anonymous memory, a file's copy) */
	if (!vma_is_own_copy(vma))
		return -ENOMEM;
	/* (a fork's snapshot: what's changed written back, then whole) */
	if (vma->vm_private_data && vma_is_shared_copy(vma)) {
		nommu_write_back(vma, vma->vm_start, vma->vm_end);
		drop_snapshot(vma);
	}

	mm = vma->vm_mm;
	if (mm->map_count >= sysctl_max_map_count)
		return -ENOMEM;

	region = kmem_cache_alloc(vm_region_jar, GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	new = vm_area_dup(vma);
	if (!new)
		goto err_vma_dup;

	/* most fields are the same, copy all, and then fixup */
	*region = *vma->vm_region;
	new->vm_region = region;

	npages = (addr - vma->vm_start) >> PAGE_SHIFT;

	if (new_below) {
		region->vm_top = region->vm_end = new->vm_end = addr;
	} else {
		region->vm_start = new->vm_start = addr;
		region->vm_pgoff = new->vm_pgoff += npages;
	}

	vma_iter_config(vmi, new->vm_start, new->vm_end);
	if (vma_iter_prealloc(vmi, vma)) {
		pr_warn("Allocation of vma tree for process %d failed\n",
			current->pid);
		goto err_vmi_preallocate;
	}

	/* (a file's copy: the new mapping and region each hold the file) */
	if (region->vm_file) {
		get_file(region->vm_file);
		get_file(new->vm_file);
	}
	if (new->vm_ops && new->vm_ops->open)
		new->vm_ops->open(new);

	vma_unlink_file(vma);
	down_write(&nommu_region_sem);
	delete_nommu_region(vma->vm_region);
	if (new_below) {
		vma->vm_region->vm_start = vma->vm_start = addr;
		vma->vm_region->vm_pgoff = vma->vm_pgoff += npages;
	} else {
		vma->vm_region->vm_end = vma->vm_end = addr;
		vma->vm_region->vm_top = addr;
	}
	add_nommu_region(vma->vm_region);
	add_nommu_region(new->vm_region);
	up_write(&nommu_region_sem);

	setup_vma_to_mm(vma, mm);
	setup_vma_to_mm(new, mm);
	vma_iter_store(vmi, new);
	mm->map_count++;
	return 0;

err_vmi_preallocate:
	vm_area_free(new);
err_vma_dup:
	kmem_cache_free(vm_region_jar, region);
	return -ENOMEM;
}

/*
 * shrink a VMA by removing the specified chunk from either the beginning or
 * the end
 */
static int vmi_shrink_vma(struct vma_iterator *vmi,
		      struct vm_area_struct *vma,
		      unsigned long from, unsigned long to)
{
	struct vm_region *region;

	/* (written back already: munmap_vma_range) */
	drop_snapshot(vma);
	/* adjust the VMA's pointers, which may reposition it in the MM's tree
	 * and list */
	vma_unlink_file(vma);
	if (from > vma->vm_start) {
		if (vma_iter_clear_gfp(vmi, from, vma->vm_end, GFP_KERNEL))
			goto enomem;
		vma->vm_end = from;
	} else {
		if (vma_iter_clear_gfp(vmi, vma->vm_start, to, GFP_KERNEL))
			goto enomem;
		vma->vm_start = to;
		vma->vm_pgoff += (to - from) >> PAGE_SHIFT;
	}
	setup_vma_to_mm(vma, vma->vm_mm);

	/* cut the backing region down to size */
	region = vma->vm_region;
	BUG_ON(region->vm_usage != 1);

	down_write(&nommu_region_sem);
	delete_nommu_region(region);
	if (from > region->vm_start) {
		to = region->vm_top;
		region->vm_top = region->vm_end = from;
	} else {
		region->vm_start = to;
		region->vm_pgoff = vma->vm_pgoff;
	}
	add_nommu_region(region);
	up_write(&nommu_region_sem);

	free_page_series(from, to);
	return 0;

enomem:
	setup_vma_to_mm(vma, vma->vm_mm);
	return -ENOMEM;
}

/*
 * release [start, end) of a mapping: all of it, or a part of memory of its
 * own (vma_is_own_copy); a shared copy's part written back first
 */
static int munmap_vma_range(struct mm_struct *mm, struct vm_area_struct *vma,
			    unsigned long start, unsigned long end)
{
	VMA_ITERATOR(vmi, mm, start);
	int ret;

	nommu_write_back(vma, start, end);
	if (start == vma->vm_start && end == vma->vm_end) {
		vma_iter_set(&vmi, vma->vm_start);
		if (delete_vma_from_mm(vma))
			return -ENOMEM;
		delete_vma(mm, vma);
		return 0;
	}
	/* a file's own memory (a shared mapping of it) can't be cut */
	if (!vma_is_own_copy(vma))
		return -EINVAL;
	vma_iter_set(&vmi, vma->vm_start);
	if (vma_find(&vmi, vma->vm_end) != vma)
		return -EINVAL;
	if (start != vma->vm_start && end != vma->vm_end) {
		ret = split_vma(&vmi, vma, start, 1);
		if (ret < 0)
			return ret;
	}
	return vmi_shrink_vma(&vmi, vma, start, end);
}

/*
 * release the mappings in [start, start + len), as Linux's munmap: what
 * isn't mapped is no matter, a mapping partly in it cut (but not a file's
 * own memory: a shared mapping of it, which NOMMU conditions can only
 * release whole)
 */
int do_munmap(struct mm_struct *mm, unsigned long start, size_t len, struct list_head *uf)
{
	struct vm_area_struct *vma;
	unsigned long end;
	int ret;

	if (offset_in_page(start) || start > TASK_SIZE || len > TASK_SIZE - start)
		return -EINVAL;
	len = PAGE_ALIGN(len);
	if (len == 0)
		return -EINVAL;

	end = start + len;
	while ((vma = find_vma_intersection(mm, start, end))) {
		ret = munmap_vma_range(mm, vma, max(start, vma->vm_start),
				       min(end, vma->vm_end));
		if (ret)
			return ret;
	}
	return 0;
}

int vm_munmap(unsigned long addr, size_t len)
{
	struct mm_struct *mm = current->mm;
	int ret;

	mmap_write_lock(mm);
	ret = do_munmap(mm, addr, len, NULL);
	mmap_write_unlock(mm);
	return ret;
}
EXPORT_SYMBOL(vm_munmap);

SYSCALL_DEFINE2(munmap, unsigned long, addr, size_t, len)
{
	return vm_munmap(addr, len);
}

/*
 * release all the mappings made in a process's VM space
 */
void exit_mmap(struct mm_struct *mm)
{
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;

	if (!mm)
		return;

	mm->total_vm = 0;

	/*
	 * Lock the mm to avoid assert complaining even though this is the only
	 * user of the mm
	 */
	mmap_write_lock(mm);
	for_each_vma(vmi, vma) {
		nommu_write_back(vma, vma->vm_start, vma->vm_end);
		cleanup_vma_from_mm(vma);
		delete_vma(mm, vma);
		cond_resched();
	}
	__mt_destroy(&mm->mm_mt);
	mmap_write_unlock(mm);
	lkl_window_exit(mm);
}

#ifdef CONFIG_LKL_WASI
/*
 * A fork's child's mappings (mm, a copy of oldmm's struct, its own maple
 * tree empty): a process in a window gets a window of its own
 * (lkl_window_fork), and a copy of each of its parent's mappings there, at
 * the same user address (its kernel address offset by the bases'
 * difference), the same contents: memory of its own, as the parent's is
 * (anonymous memory, a file's private or shared copy). Else nothing, as
 * nommu has it (vfork's child shares its parent's memory).
 */
int nommu_dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm)
{
	VMA_ITERATOR(vmi, oldmm, 0);
	struct vm_area_struct *vma, *new;
	struct vm_region *region, *old;
	unsigned long sp = 0, from, to;
	long delta;
	int ret;

	if (!mm_window(oldmm)) {
		mm->context.win = NULL;
		mm->context.base = 0;
		return 0;
	}
	/*
	 * (the parent's mappings, and its window's free ranges, as they are:
	 * another of its threads may map or unmap meanwhile, as the copy
	 * allocates; the child's not linked in yet, as dup_mmap's)
	 */
	mmap_write_lock(oldmm);
	mmap_write_lock_nested(mm, SINGLE_DEPTH_NESTING);
	ret = lkl_window_fork(mm, oldmm);
	if (ret)
		goto out;
	delta = mm->context.base - oldmm->context.base;
	if (lkl_ops->user_sp && current->mm == oldmm)
		sp = lkl_ops->user_sp() + oldmm->context.base;
	mm->start_brk += delta;
	mm->brk += delta;
	mm->context.end_brk += delta;
	mm->start_stack += delta;
	mm->total_vm = oldmm->total_vm;
	mm->map_count = 0;

	for_each_vma(vmi, vma) {
		VMA_ITERATOR(nvmi, mm, 0);

		old = vma->vm_region;
		ret = -ENOMEM;
		region = kmem_cache_alloc(vm_region_jar, GFP_KERNEL);
		if (!region)
			break;
		new = vm_area_dup(vma);
		if (!new) {
			kmem_cache_free(vm_region_jar, region);
			break;
		}
		*region = *old;
		region->vm_usage = 1;
		region->vm_start += delta;
		region->vm_end += delta;
		region->vm_top += delta;
		if (region->vm_file)
			get_file(region->vm_file);
		if (new->vm_file)
			get_file(new->vm_file);
		new->vm_region = region;
		new->vm_start += delta;
		new->vm_end += delta;
		new->vm_private_data = NULL;
		if (vma_is_shared_copy(vma) && (vma->vm_flags & VM_WRITE)) {
			new->vm_private_data = kvmalloc(vma->vm_end - vma->vm_start, GFP_KERNEL);
			if (new->vm_private_data)
				memcpy(new->vm_private_data, (void *)vma->vm_start,
				       vma->vm_end - vma->vm_start);
		}
		/*
		 * (the forking thread's stack: below its stack pointer, dead
		 * frames, left out; memory never written, which still costs the
		 * host a fault to read)
		 */
		from = old->vm_start;
		to = old->vm_end;
		if (sp > old->vm_start && sp < old->vm_end)
			from = sp & PAGE_MASK;
		/*
		 * (the break's mapping: past the break, not the program's,
		 * zeroed as brk grows over it again; mostly never written,
		 * which a fault would back just to read)
		 */
		if (oldmm->start_brk >= old->vm_start && oldmm->start_brk < old->vm_end)
			to = min(to, PAGE_ALIGN(oldmm->brk));
		if (to > from)
			lkl_window_copy(mm, (void *)region->vm_start + (from - old->vm_start),
					(void *)from, to - from);

		vma_iter_config(&nvmi, new->vm_start, new->vm_end);
		if (vma_iter_prealloc(&nvmi, new)) {
			/* (not in the tree: the window goes whole with the mm) */
			region->vm_top = region->vm_start;
			delete_vma(mm, new);
			break;
		}
		down_write(&nommu_region_sem);
		add_nommu_region(region);
		up_write(&nommu_region_sem);
		setup_vma_to_mm(new, mm);
		vma_iter_store(&nvmi, new);
		mm->map_count++;
		ret = 0;
	}
out:
	mmap_write_unlock(mm);
	mmap_write_unlock(oldmm);
	return ret;
}
#endif

/*
 * grow a mapping in place, over what its region holds past its end (a
 * power-of-2 allocation's rest: one mremap moved keeps it), zeroed or the
 * file's bytes read in
 */
static int mremap_grow_in_place(struct mm_struct *mm, struct vm_area_struct *vma,
				unsigned long new_end)
{
	struct vm_region *region = vma->vm_region;
	unsigned long old_end = vma->vm_end;
	VMA_ITERATOR(vmi, mm, vma->vm_start);
	int ret;

	if (!vma_is_own_copy(vma) || region->vm_end != old_end ||
	    new_end > region->vm_top)
		return -ENOMEM;
	if (vma->vm_private_data && vma_is_shared_copy(vma)) {
		nommu_write_back(vma, vma->vm_start, vma->vm_end);
		drop_snapshot(vma);
	}
	if (vma->vm_file)
		ret = read_file_into(vma->vm_file, old_end, new_end - old_end,
				     ((loff_t)vma->vm_pgoff << PAGE_SHIFT) +
				     (old_end - vma->vm_start));
	else
		ret = (clear_new_pages((void *)old_end, new_end - old_end), 0);
	if (ret)
		return ret;
	vma_iter_config(&vmi, vma->vm_start, new_end);
	if (vma_iter_prealloc(&vmi, vma))
		return -ENOMEM;
	vma_unlink_file(vma);
	down_write(&nommu_region_sem);
	region->vm_end = new_end;
	up_write(&nommu_region_sem);
	vma->vm_end = new_end;
	vma_iter_store(&vmi, vma);
	setup_vma_to_mm(vma, mm);
	mm->total_vm += (new_end - old_end) >> PAGE_SHIFT;
	return 0;
}

/*
 * resize a mapping, as Linux's mremap: shrunk, what's past its new end
 * unmapped; grown in place (mremap_grow_in_place), or else moved
 * (MREMAP_MAYMOVE): a new mapping (anonymous memory, the file's copy, or a
 * shared mapping of the file again), the old one's contents copied into it
 * (a shared mapping's are the file's: a copy's written back first), the old
 * one unmapped. MREMAP_FIXED is not supported under NOMMU conditions.
 */
static unsigned long do_mremap(unsigned long addr,
			unsigned long old_len, unsigned long new_len,
			unsigned long flags, unsigned long new_addr)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long populate, prot, map_flags, ret;
	struct file *file;

	/* insanity checks first */
	old_len = PAGE_ALIGN(old_len);
	new_len = PAGE_ALIGN(new_len);
	if (flags & ~(MREMAP_FIXED | MREMAP_MAYMOVE))
		return -EINVAL;
	if (old_len == 0 || new_len == 0)
		return (unsigned long) -EINVAL;

	if (offset_in_page(addr))
		return -EINVAL;

	if (flags & MREMAP_FIXED && new_addr != addr)
		return (unsigned long) -EINVAL;

	vma = vma_lookup(mm, addr);
	if (!vma || old_len > vma->vm_end - addr)
		return (unsigned long) -EFAULT;

	if (new_len == old_len)
		return addr;
	if (new_len < old_len) {
		ret = do_munmap(mm, addr + new_len, old_len - new_len, NULL);
		return ret ? ret : addr;
	}

	if (addr + old_len == vma->vm_end &&
	    !mremap_grow_in_place(mm, vma, addr + new_len))
		return addr;
	if (!(flags & MREMAP_MAYMOVE))
		return (unsigned long) -ENOMEM;

	file = vma->vm_file;
	prot = (vma->vm_flags & VM_READ ? PROT_READ : 0) |
	       (vma->vm_flags & VM_WRITE ? PROT_WRITE : 0) |
	       (vma->vm_flags & VM_EXEC ? PROT_EXEC : 0);
	map_flags = vma->vm_flags & VM_SHARED ? MAP_SHARED : MAP_PRIVATE;
	if (!file)
		map_flags |= MAP_ANONYMOUS;
	nommu_write_back(vma, addr, addr + old_len);
	/* (VM_MAPPED_COPY: a power-of-2 allocation kept whole, to grow into) */
	ret = do_mmap(file, 0, new_len, prot, map_flags,
		      vma->vm_flags & VM_SHARED ? 0 : VM_MAPPED_COPY,
		      vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT),
		      &populate, NULL);
	if (IS_ERR_VALUE(ret))
		return ret;
	if (!(vma->vm_flags & VM_SHARED) || !file) {
		memcpy((void *)ret, (void *)addr, old_len);
	} else {
		/* (a shared copy written since this one was written back: none) */
		struct vm_area_struct *new = vma_lookup(mm, ret);

		if (vma->vm_flags & VM_WRITE)
			vm_flags_set(new, VM_WRITE);
	}
	do_munmap(mm, addr, old_len, NULL);
	return ret;
}

SYSCALL_DEFINE5(mremap, unsigned long, addr, unsigned long, old_len,
		unsigned long, new_len, unsigned long, flags,
		unsigned long, new_addr)
{
	unsigned long ret;

	mmap_write_lock(current->mm);
	ret = do_mremap(addr, old_len, new_len, flags, new_addr);
	mmap_write_unlock(current->mm);
	return ret;
}

/*
 * The other calls on a process's mappings, as Linux's (their checks and
 * errors), on what NOMMU conditions have: protections recorded, not
 * enforced; every page resident and locked already.
 */

/*
 * each mapping's part of [start, end) visited: then -ENOMEM if some of the
 * range isn't mapped (the rest visited still), or the first visit's error
 */
static int walk_vmas(struct mm_struct *mm, unsigned long start, unsigned long end,
		     unsigned long arg,
		     int (*visit)(struct vm_area_struct *vma, unsigned long start,
				  unsigned long end, unsigned long arg))
{
	struct vm_area_struct *vma;
	int unmapped = 0, ret;

	while (start < end) {
		vma = find_vma_intersection(mm, start, end);
		if (!vma)
			return -ENOMEM;
		if (vma->vm_start > start)
			unmapped = -ENOMEM;
		start = max(start, vma->vm_start);
		ret = visit(vma, start, min(end, vma->vm_end), arg);
		if (ret)
			return ret;
		start = vma->vm_end;
	}
	return unmapped;
}

/*
 * the whole mapping takes the protection (no MMU enforces it); a shared
 * copy written back as long as it was ever writable
 */
static int mprotect_vma(struct vm_area_struct *vma, unsigned long start,
			unsigned long end, unsigned long prot)
{
	vm_flags_t keep = vma_is_shared_copy(vma) ? VM_WRITE : 0;

	if ((prot & PROT_WRITE) && vma->vm_file && (vma->vm_flags & VM_SHARED) &&
	    !(vma->vm_file->f_mode & FMODE_WRITE))
		return -EACCES;
	vm_flags_reset(vma, (vma->vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC)) |
			    (vma->vm_flags & keep) | calc_vm_prot_bits(prot, 0));
	return 0;
}

SYSCALL_DEFINE3(mprotect, unsigned long, start, size_t, len,
		unsigned long, prot)
{
	struct mm_struct *mm = current->mm;
	unsigned long end;
	int ret;

	if (offset_in_page(start))
		return -EINVAL;
	if (!len)
		return 0;
	len = PAGE_ALIGN(len);
	end = start + len;
	if (end <= start)
		return -ENOMEM;
	if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_SEM |
		     PROT_GROWSDOWN | PROT_GROWSUP))
		return -EINVAL;
	if (mmap_write_lock_killable(mm))
		return -EINTR;
	ret = walk_vmas(mm, start, end, prot, mprotect_vma);
	mmap_write_unlock(mm);
	return ret;
}

/* a shared copy written back; MS_SYNC: a shared mapping's file synced */
static int msync_vma(struct vm_area_struct *vma, unsigned long start,
		     unsigned long end, unsigned long flags)
{
	struct file *file = vma->vm_file;
	loff_t pos;
	int ret = nommu_write_back(vma, start, end);

	if (ret || !(flags & MS_SYNC) || !file || !(vma->vm_flags & VM_SHARED))
		return ret;
	pos = ((loff_t)vma->vm_pgoff << PAGE_SHIFT) + (start - vma->vm_start);
	return vfs_fsync_range(file, pos, pos + (end - start) - 1, 1);
}

SYSCALL_DEFINE3(msync, unsigned long, start, size_t, len, int, flags)
{
	struct mm_struct *mm = current->mm;
	unsigned long end;
	int ret;

	if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC))
		return -EINVAL;
	if (offset_in_page(start))
		return -EINVAL;
	if ((flags & MS_ASYNC) && (flags & MS_SYNC))
		return -EINVAL;
	len = PAGE_ALIGN(len);
	end = start + len;
	if (end < start)
		return -ENOMEM;
	if (end == start)
		return 0;
	if (mmap_read_lock_killable(mm))
		return -EINTR;
	ret = walk_vmas(mm, start, end, flags, msync_vma);
	mmap_read_unlock(mm);
	return ret;
}

static bool madvise_behavior_valid(int behavior)
{
	switch (behavior) {
	case MADV_DOFORK:
	case MADV_DONTFORK:
	case MADV_NORMAL:
	case MADV_SEQUENTIAL:
	case MADV_RANDOM:
	case MADV_REMOVE:
	case MADV_WILLNEED:
	case MADV_DONTNEED:
	case MADV_DONTNEED_LOCKED:
	case MADV_FREE:
	case MADV_COLD:
	case MADV_PAGEOUT:
	case MADV_POPULATE_READ:
	case MADV_POPULATE_WRITE:
	case MADV_DONTDUMP:
	case MADV_DODUMP:
	case MADV_WIPEONFORK:
	case MADV_KEEPONFORK:
		return true;
	default:
		return false;
	}
}

/*
 * MADV_DONTNEED: private memory as a fresh mapping's again (zeroed, or the
 * file's bytes); a shared mapping's kept, as Linux's
 */
static int madvise_vma(struct vm_area_struct *vma, unsigned long start,
		       unsigned long end, unsigned long behavior)
{
	if (behavior != MADV_DONTNEED && behavior != MADV_DONTNEED_LOCKED)
		return 0;
	if (vma->vm_flags & VM_SHARED)
		return 0;
	if (vma->vm_file)
		return read_file_into(vma->vm_file, start, end - start,
				      ((loff_t)vma->vm_pgoff << PAGE_SHIFT) +
				      (start - vma->vm_start));
	clear_nonzero((void *)start, end - start);
	return 0;
}

SYSCALL_DEFINE3(madvise, unsigned long, start, size_t, len_in, int, behavior)
{
	struct mm_struct *mm = current->mm;
	unsigned long end, len;
	int ret;

	if (!madvise_behavior_valid(behavior))
		return -EINVAL;
	if (!PAGE_ALIGNED(start))
		return -EINVAL;
	len = PAGE_ALIGN(len_in);
	/* Check to see whether len was rounded up from small -ve to zero */
	if (len_in && !len)
		return -EINVAL;
	end = start + len;
	if (end < start)
		return -EINVAL;
	if (end == start)
		return 0;
	if (mmap_read_lock_killable(mm))
		return -EINTR;
	ret = walk_vmas(mm, start, end, behavior, madvise_vma);
	mmap_read_unlock(mm);
	return ret;
}

/* every page mapped is resident: a 1 for each */
SYSCALL_DEFINE3(mincore, unsigned long, start, size_t, len,
		unsigned char __user *, vec)
{
	struct mm_struct *mm = current->mm;
	unsigned long pages, end, i;
	static const unsigned char ones[64] = {
		[0 ... 63] = 1
	};
	int ret = 0;

	if (offset_in_page(start))
		return -EINVAL;
	pages = (len >> PAGE_SHIFT) + (offset_in_page(len) != 0);
	end = start + (pages << PAGE_SHIFT);
	if (end < start)
		return -ENOMEM;
	if (!access_ok(vec, pages))
		return -EFAULT;
	if (mmap_read_lock_killable(mm))
		return -EINTR;
	for (i = 0; i < pages && !ret; ) {
		unsigned long addr = start + (i << PAGE_SHIFT);
		struct vm_area_struct *vma = vma_lookup(mm, addr);
		unsigned long n;

		if (!vma) {
			ret = -ENOMEM;
			break;
		}
		n = min(pages - i, (vma->vm_end - addr) >> PAGE_SHIFT);
		while (n && !ret) {
			unsigned long k = min(n, sizeof(ones));

			if (copy_to_user(vec + i, ones, k))
				ret = -EFAULT;
			i += k;
			n -= k;
		}
	}
	mmap_read_unlock(mm);
	return ret;
}

static int lock_vma(struct vm_area_struct *vma, unsigned long start,
		    unsigned long end, unsigned long unused)
{
	return 0;
}

/* [start, start + len) all mapped (it's locked already) */
static int lock_range(unsigned long start, size_t len)
{
	struct mm_struct *mm = current->mm;
	unsigned long end;
	int ret;

	len = PAGE_ALIGN(len + offset_in_page(start));
	start &= PAGE_MASK;
	end = start + len;
	if (end < start)
		return -EINVAL;
	if (end == start)
		return 0;
	if (mmap_read_lock_killable(mm))
		return -EINTR;
	ret = walk_vmas(mm, start, end, 0, lock_vma);
	mmap_read_unlock(mm);
	return ret;
}

SYSCALL_DEFINE2(mlock, unsigned long, start, size_t, len)
{
	return lock_range(start, len);
}

SYSCALL_DEFINE3(mlock2, unsigned long, start, size_t, len, int, flags)
{
	if (flags & ~MLOCK_ONFAULT)
		return -EINVAL;
	return lock_range(start, len);
}

SYSCALL_DEFINE2(munlock, unsigned long, start, size_t, len)
{
	return lock_range(start, len);
}

SYSCALL_DEFINE1(mlockall, int, flags)
{
	if (!flags || (flags & ~(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)) ||
	    flags == MCL_ONFAULT)
		return -EINVAL;
	return 0;
}

SYSCALL_DEFINE0(munlockall)
{
	return 0;
}

int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
		unsigned long pfn, unsigned long size, pgprot_t prot)
{
	if (addr != (pfn << PAGE_SHIFT))
		return -EINVAL;

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	return 0;
}
EXPORT_SYMBOL(remap_pfn_range);

int vm_iomap_memory(struct vm_area_struct *vma, phys_addr_t start, unsigned long len)
{
	unsigned long pfn = start >> PAGE_SHIFT;
	unsigned long vm_len = vma->vm_end - vma->vm_start;

	pfn += vma->vm_pgoff;
	return io_remap_pfn_range(vma, vma->vm_start, pfn, vm_len, vma->vm_page_prot);
}
EXPORT_SYMBOL(vm_iomap_memory);

int remap_vmalloc_range(struct vm_area_struct *vma, void *addr,
			unsigned long pgoff)
{
	unsigned int size = vma->vm_end - vma->vm_start;

	if (!(vma->vm_flags & VM_USERMAP))
		return -EINVAL;

	vma->vm_start = (unsigned long)(addr + (pgoff << PAGE_SHIFT));
	vma->vm_end = vma->vm_start + size;

	return 0;
}
EXPORT_SYMBOL(remap_vmalloc_range);

vm_fault_t filemap_fault(struct vm_fault *vmf)
{
	BUG();
	return 0;
}
EXPORT_SYMBOL(filemap_fault);

vm_fault_t filemap_map_pages(struct vm_fault *vmf,
		pgoff_t start_pgoff, pgoff_t end_pgoff)
{
	BUG();
	return 0;
}
EXPORT_SYMBOL(filemap_map_pages);

static int __access_remote_vm(struct mm_struct *mm, unsigned long addr,
			      void *buf, int len, unsigned int gup_flags)
{
	struct vm_area_struct *vma;
	int write = gup_flags & FOLL_WRITE;

	if (mmap_read_lock_killable(mm))
		return 0;

	/* the access must start within one of the target process's mappings */
	vma = find_vma(mm, addr);
	if (vma) {
		/* don't overrun this mapping */
		if (addr + len >= vma->vm_end)
			len = vma->vm_end - addr;

		/* only read or write mappings where it is permitted */
		if (write && vma->vm_flags & VM_MAYWRITE)
			copy_to_user_page(vma, NULL, addr,
					 (void *) addr, buf, len);
		else if (!write && vma->vm_flags & VM_MAYREAD)
			copy_from_user_page(vma, NULL, addr,
					    buf, (void *) addr, len);
		else
			len = 0;
	} else {
		len = 0;
	}

	mmap_read_unlock(mm);

	return len;
}

/**
 * access_remote_vm - access another process' address space
 * @mm:		the mm_struct of the target address space
 * @addr:	start address to access
 * @buf:	source or destination buffer
 * @len:	number of bytes to transfer
 * @gup_flags:	flags modifying lookup behaviour
 *
 * The caller must hold a reference on @mm.
 */
int access_remote_vm(struct mm_struct *mm, unsigned long addr,
		void *buf, int len, unsigned int gup_flags)
{
	return __access_remote_vm(mm, addr, buf, len, gup_flags);
}

/*
 * Access another process' address space.
 * - source/target buffer must be kernel space
 */
int access_process_vm(struct task_struct *tsk, unsigned long addr, void *buf, int len,
		unsigned int gup_flags)
{
	struct mm_struct *mm;

	if (addr + len < addr)
		return 0;

	mm = get_task_mm(tsk);
	if (!mm)
		return 0;

	len = __access_remote_vm(mm, addr, buf, len, gup_flags);

	mmput(mm);
	return len;
}
EXPORT_SYMBOL_GPL(access_process_vm);

/**
 * nommu_shrink_inode_mappings - Shrink the shared mappings on an inode
 * @inode: The inode to check
 * @size: The current filesize of the inode
 * @newsize: The proposed filesize of the inode
 *
 * Check the shared mappings on an inode on behalf of a shrinking truncate to
 * make sure that any outstanding VMAs aren't broken and then shrink the
 * vm_regions that extend beyond so that do_mmap() doesn't
 * automatically grant mappings that are too large.
 */
int nommu_shrink_inode_mappings(struct inode *inode, size_t size,
				size_t newsize)
{
	struct vm_area_struct *vma;
	struct vm_region *region;
	pgoff_t low, high;
	size_t r_size, r_top;

	low = newsize >> PAGE_SHIFT;
	high = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;

	down_write(&nommu_region_sem);
	i_mmap_lock_read(inode->i_mapping);

	/* search for VMAs that fall within the dead zone */
	vma_interval_tree_foreach(vma, &inode->i_mapping->i_mmap, low, high) {
		/* found one - only interested if it's shared out of the page
		 * cache (not a copy) */
		if ((vma->vm_flags & VM_SHARED) &&
		    !(vma->vm_flags & VM_MAPPED_COPY)) {
			i_mmap_unlock_read(inode->i_mapping);
			up_write(&nommu_region_sem);
			return -ETXTBSY; /* not quite true, but near enough */
		}
	}

	/* reduce any regions that overlap the dead zone - if in existence,
	 * these will be pointed to by VMAs that don't overlap the dead zone
	 *
	 * we don't check for any regions that start beyond the EOF as there
	 * shouldn't be any
	 */
	vma_interval_tree_foreach(vma, &inode->i_mapping->i_mmap, 0, ULONG_MAX) {
		if (!(vma->vm_flags & VM_SHARED) ||
		    (vma->vm_flags & VM_MAPPED_COPY))
			continue;

		region = vma->vm_region;
		r_size = region->vm_top - region->vm_start;
		r_top = (region->vm_pgoff << PAGE_SHIFT) + r_size;

		if (r_top > newsize) {
			region->vm_top -= r_top - newsize;
			if (region->vm_end > region->vm_top)
				region->vm_end = region->vm_top;
		}
	}

	i_mmap_unlock_read(inode->i_mapping);
	up_write(&nommu_region_sem);
	return 0;
}

/*
 * Initialise sysctl_user_reserve_kbytes.
 *
 * This is intended to prevent a user from starting a single memory hogging
 * process, such that they cannot recover (kill the hog) in OVERCOMMIT_NEVER
 * mode.
 *
 * The default value is min(3% of free memory, 128MB)
 * 128MB is enough to recover with sshd/login, bash, and top/kill.
 */
static int __meminit init_user_reserve(void)
{
	unsigned long free_kbytes;

	free_kbytes = K(global_zone_page_state(NR_FREE_PAGES));

	sysctl_user_reserve_kbytes = min(free_kbytes / 32, 1UL << 17);
	return 0;
}
subsys_initcall(init_user_reserve);

/*
 * Initialise sysctl_admin_reserve_kbytes.
 *
 * The purpose of sysctl_admin_reserve_kbytes is to allow the sys admin
 * to log in and kill a memory hogging process.
 *
 * Systems with more than 256MB will reserve 8MB, enough to recover
 * with sshd, bash, and top in OVERCOMMIT_GUESS. Smaller systems will
 * only reserve 3% of free pages by default.
 */
static int __meminit init_admin_reserve(void)
{
	unsigned long free_kbytes;

	free_kbytes = K(global_zone_page_state(NR_FREE_PAGES));

	sysctl_admin_reserve_kbytes = min(free_kbytes / 32, 1UL << 13);
	return 0;
}
subsys_initcall(init_admin_reserve);
