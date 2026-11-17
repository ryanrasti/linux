/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_PAGE_H
#define _ASM_LKL_PAGE_H

#ifndef CONFIG_MMU
#define ARCH_PFN_OFFSET	(memory_start >> PAGE_SHIFT)
#include <asm/page-nommu.h>
#else // CONFIG_MMU
#include <asm/page-mmu.h>
#endif // CONFIG_MMU

#ifndef __ASSEMBLY__
void free_mem(void);
void bootmem_init(unsigned long mem_size);
#ifndef CONFIG_MMU
/* (a page freed is no longer fresh: arch_page_fresh, mm/bootmem.c) */
#define HAVE_ARCH_FREE_PAGE
struct page;
void arch_free_page(struct page *page, int order);
#endif
#endif

#undef PAGE_OFFSET
#define PAGE_OFFSET memory_start

#endif /* _ASM_LKL_PAGE_H */
