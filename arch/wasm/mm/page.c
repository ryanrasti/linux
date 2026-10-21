#include <linux/mm.h>
#include <linux/panic.h>
#include <linux/spinlock.h>

static DEFINE_RAW_SPINLOCK(memory_grow_lock);

/* desired: kernel pages (pfns); wasm memory grows in WASM_PAGE_SIZE pages */
static void materialize_memory(unsigned long desired_pfns)
{
	unsigned long desired = DIV_ROUND_UP_ULL((u64)desired_pfns << PAGE_SHIFT, WASM_PAGE_SIZE);
	unsigned long flags, current_pages;
	long previous;

	raw_spin_lock_irqsave(&memory_grow_lock, flags);
	current_pages = __builtin_wasm_memory_size(0);
	if (current_pages < desired) {
		/*
		 * Each grow of a shared memory is a host call (and an mprotect):
		 * grow ahead, a few MiB at a time, rather than page by page.
		 */
		unsigned long grow = max(desired - current_pages, 64UL);

		previous = __builtin_wasm_memory_grow(0, grow);
		if (previous < 0)
			previous = __builtin_wasm_memory_grow(0, desired - current_pages);
		if (previous < 0)
			panic("wasm: failed to grow kernel memory from %lu to %lu pages",
			      current_pages, desired);
	}
	raw_spin_unlock_irqrestore(&memory_grow_lock, flags);
}

void __init arch_memblock_materialize(phys_addr_t base, phys_addr_t size)
{
	materialize_memory(PFN_UP(base + size));
}

void arch_alloc_page(struct page *page, int order)
{
	materialize_memory(page_to_pfn(page) + (1UL << order));
}

