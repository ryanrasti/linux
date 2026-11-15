// SPDX-License-Identifier: GPL-2.0
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/bitmap.h>
#include <linux/sizes.h>
#include <asm/host_ops.h>

unsigned long memory_start, memory_end;
static void *_memory_start;
static unsigned long mem_size;

#ifndef CONFIG_MMU
/*
 * The pool's fresh pages: those the page allocator hasn't freed since
 * boot (a bit each), past the part the boot allocator (memblock) may use.
 * They still hold the zeros the host's memory gave them: a new mapping
 * needn't zero them, nor read them (a lazily backed memory, as wasm's,
 * would back what's read). A page is fresh until it's freed: whoever has
 * it then may have written it.
 */
static unsigned long *fresh;
static unsigned long fresh_pfn, fresh_pages;

bool arch_page_fresh(struct page *page)
{
	unsigned long i = page_to_pfn(page) - fresh_pfn;

	return fresh && page_to_pfn(page) >= fresh_pfn && i < fresh_pages &&
	       test_bit(i, fresh);
}

void arch_free_page(struct page *page, int order)
{
	unsigned long pfn = page_to_pfn(page), end = pfn + (1UL << order);

	if (!fresh || end <= fresh_pfn || pfn >= fresh_pfn + fresh_pages)
		return;
	pfn = max(pfn, fresh_pfn);
	end = min(end, fresh_pfn + fresh_pages);
	bitmap_clear(fresh, pfn - fresh_pfn, end - pfn);
}

/*
 * the boot allocator's part, at the pool's start: enough for the memory
 * map (a struct page each page) and the rest it allocates; the fresh pages
 * past it
 */
static void __init fresh_init(void)
{
	unsigned long boot = min(mem_size, PAGE_ALIGN((mem_size >> 6) + SZ_4M));

	memblock_set_current_limit(__pa(memory_start + boot));
	fresh_pfn = virt_to_pfn((void *)(memory_start + boot));
	fresh_pages = (mem_size - boot) >> PAGE_SHIFT;
	if (fresh_pages)
		fresh = memblock_alloc(BITS_TO_LONGS(fresh_pages) * sizeof(long),
				       SMP_CACHE_BYTES);
}
#endif

void *empty_zero_page;

void __init bootmem_init(unsigned long mem_sz)
{
	unsigned long zones_max_pfn[MAX_NR_ZONES] = {0, };

	mem_size = mem_sz;

#ifdef CONFIG_MMU
	mem_size = PAGE_ALIGN(mem_size);
	lkl_ops->shmem_init(mem_size);
	void *lkl_va_base = (void *)CONFIG_LKL_MEMORY_START;
	enum lkl_prot shmem_prot = LKL_PROT_READ | LKL_PROT_WRITE;

	_memory_start = lkl_ops->shmem_mmap(lkl_va_base, 0, mem_size, shmem_prot);
#else
	if (lkl_ops->page_alloc) {
		mem_size = PAGE_ALIGN(mem_size);
		_memory_start = lkl_ops->page_alloc(mem_size);
	} else {
		_memory_start = lkl_ops->mem_alloc(mem_size);
	}
#endif
	memory_start = (unsigned long)_memory_start;
	BUG_ON(!memory_start);
	memory_end = memory_start + mem_size;

	if (PAGE_ALIGN(memory_start) != memory_start) {
		mem_size -= PAGE_ALIGN(memory_start) - memory_start;
		memory_start = PAGE_ALIGN(memory_start);
		mem_size = (mem_size / PAGE_SIZE) * PAGE_SIZE;
	}
	/*
	 * its end at a max-order block's boundary, when it spans several: the
	 * memory map spans whole blocks, and is zeroed (resident) at boot
	 */
	if (mem_size > 2 * (PAGE_SIZE << MAX_PAGE_ORDER)) {
		memory_end = ALIGN_DOWN(memory_start + mem_size,
					PAGE_SIZE << MAX_PAGE_ORDER);
		mem_size = memory_end - memory_start;
	}
	pr_info("memblock address range: 0x%lx - 0x%lx\n", memory_start,
		memory_start+mem_size);
	/*
	 * Give all the memory to the bootmap allocator, tell it to put the
	 * boot mem_map at the start of memory.
	 */
	memblock_add(__pa(memory_start), mem_size);
#ifndef CONFIG_MMU
	fresh_init();
#endif

	max_pfn = max_low_pfn = virt_to_pfn((void *)memory_end);
	min_low_pfn = virt_to_pfn((void *)memory_start);
	set_max_mapnr(max_pfn - ARCH_PFN_OFFSET);

	empty_zero_page = memblock_alloc(PAGE_SIZE, PAGE_SIZE);
	memset(empty_zero_page, 0, PAGE_SIZE);

	zones_max_pfn[ZONE_NORMAL] = max_low_pfn;
	free_area_init(zones_max_pfn);
}

void __init mem_init(void)
{
	memblock_free_all();
#ifndef CONFIG_MMU
	/* (freeing them to the page allocator cleared them) */
	if (fresh)
		bitmap_fill(fresh, fresh_pages);
#endif
}

/*
 * In our case __init memory is not part of the page allocator so there is
 * nothing to free.
 */
void free_initmem(void)
{
}

void free_mem(void)
{
	if (lkl_ops->page_free)
		lkl_ops->page_free(_memory_start, mem_size);
	else
		lkl_ops->mem_free(_memory_start);
}
