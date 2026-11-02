/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_lnm_arena_netbsd.c - the NetBSD host-mm glue for the executive-resident
 * logical-name arena (rd vms-72da, epic vms-8e8; docs/design-netbsd-executive-core.md
 * §2, docs/design-logical-name-placement.md).
 *
 * WHAT THIS IS. Two seams the substrate-agnostic logical-name facility
 * (src/kernel-core/vms_lnm.c) needs from its host, both fundamentally coupled to
 * the NetBSD virtual-memory subsystem, quarantined HERE in the rind:
 *
 *   1. exec_arena_alloc / exec_arena_grow / exec_arena_free (exec_kbackend.h
 *      §10) -- the ALLOCATION of the one read-only-publishable arena. On NetBSD
 *      the arena's kernel VA is reserved whole (uvm_km_alloc UVM_KMF_VAONLY) and
 *      backed by wired, zeroed pages only as far as exec_arena_grow is asked to
 *      (rd vms-c01): the facility backs the entries it uses, not its 1.2 MB
 *      capacity, which on a 16 MB VAX was memory the boot did not have. A backed
 *      page is real RAM that never pages out, so d_mmap below can resolve it to a
 *      physical frame. (The Linux backend uses vmalloc_user, backed whole.)
 *
 *   2. vms_mmap -- the cdevsw d_mmap that PUBLISHES the arena read-only into a
 *      process. For the requested page offset it resolves the arena's kernel VA to
 *      a physical frame with pmap_extract() and returns that frame number
 *      (atop(pa)); the device pager reconstructs the frame via pmap_phys_address()
 *      and enters it into the process at the mmap(2) protection. Userspace maps
 *      PROT_READ only (kif_xport_mmap); this handler REFUSES any write intent, so
 *      the MMU -- not a convention -- is what stops a process corrupting the
 *      system logical-name table (the direct analogue of VMS protecting system
 *      space by processor access mode, design §2.4). This is the standard NetBSD
 *      idiom for exposing kernel memory through the d_mmap(dev,off,prot) seam (the
 *      /dev/mem precedent). It is the MMAP-TIME MAPPING the Linux vms_module.c
 *      vms_lnm_mmap does with remap_vmalloc_range; on NetBSD it stays here.
 *
 * WHY A DEDICATED TU (and not exec_kbackend_netbsd.h / vms_netbsd.c). The uvm KPIs
 * above live behind <uvm/uvm_extern.h>, which transitively pulls <sys/rbtree.h>,
 * whose rb_left/rb_right MACROS collide with OVMX's intrusive exec_rbtree_netbsd.h
 * (the DLM's lock-ID red-black tree) -- a header every OTHER executive TU includes
 * via vms_internal.h. This file therefore deliberately includes uvm but NOT
 * vms_internal.h / exec_kbackend.h / exec_rbtree.h: it is the one TU where the uvm
 * headers are safe. It declares by hand the two facility entry points it calls
 * (vms_lnm_arena_base/_size, defined in the shared src/kernel-core/vms_lnm.c) so
 * it needs none of the executive headers.
 *
 * INV-6 / Rule 9 / Rule 11. Nothing here fabricates state: exec_arena_alloc either
 * returns real wired kernel memory or NULL (out of memory), and vms_mmap either
 * maps the real arena page or fails (paddr_t)-1. A failed map is what
 * vms_kif_lnm_arena() reads as "executive absent" and turns into an honest
 * SS$_NOSUCHDEV, never a per-process fake table.
 *
 * Clean-room (CLAUDE.md Rule 8): OVMX glue over the PUBLIC NetBSD uvm/pmap KPIs.
 * No NetBSD or VSI source is copied.
 */

#include <sys/param.h>       /* round_page, PAGE_SIZE, PGSHIFT */
#include <sys/types.h>       /* vaddr_t, vsize_t, paddr_t, dev_t, off_t, size_t */
#include <sys/systm.h>
#include <uvm/uvm_extern.h>  /* uvm_km_alloc/free, kernel_map, UVM_KMF_*, atop */
#include <uvm/uvm_pmap.h>    /* pmap_extract, pmap_kernel; VM_PROT_* via uvm_prot.h */
#include <uvm/uvm_page.h>    /* uvm_pagealloc/free, VM_PAGE_TO_PHYS, PHYS_TO_VM_PAGE */

/*
 * The two facility entry points this glue calls, DEFINED in the shared
 * src/kernel-core/vms_lnm.c (which this TU cannot include a header of without
 * dragging in the rbtree). Declared by hand, byte-matching vms_lnm.c's
 * signatures; a mismatch is a link-time error.
 */
void  *vms_lnm_arena_base(void);
size_t vms_lnm_arena_size(void);

/*
 * The one arena's kernel VA + extent, and how much of it is backed. This is the
 * SOLE arena in the system (allocated once at vms_lnm_init, freed once at
 * vms_lnm_cleanup), so file-static state is the natural place for it. The arena
 * base is EXACTLY uvm_km_alloc's page-aligned return (no offset), so d_mmap
 * resolves the same page the executive writes.
 *
 * arena_zero: one zeroed page d_mmap names for an offset not yet backed.
 * mmap(2) asks d_mmap for every page of the range it maps, so every page of the
 * extent must have an answer; a reader stops at max_entries and so never
 * touches one of these pages, but if one ever did it would read zeros (no
 * entry in use, which is what an unbacked slot is), and exec_arena_grow takes
 * the zero page back from every process before it backs anything, so no
 * process can keep reading zeros where a name now is.
 */
static vaddr_t arena_kva;
static vsize_t arena_extent;
static vsize_t arena_backed;
static struct vm_page *arena_zero;

/*
 * exec_arena_alloc - reserve the ONE read-only-publishable arena (exec_kbackend.h
 * §10): kernel VA for all n bytes, no page behind any of it yet. The facility
 * backs what it uses with exec_arena_grow before it writes there.
 */
void *
exec_arena_alloc(size_t n)
{
	arena_extent = round_page(n);
	arena_backed = 0;
	arena_zero = uvm_pagealloc(NULL, 0, NULL, UVM_PGA_ZERO);
	if (arena_zero == NULL) {
		arena_extent = 0;
		return NULL;
	}
	arena_kva = uvm_km_alloc(kernel_map, arena_extent, 0, UVM_KMF_VAONLY);
	if (arena_kva == 0) {
		uvm_pagefree(arena_zero);
		arena_zero = NULL;
		arena_extent = 0;
		return NULL;
	}
	return (void *)arena_kva;
}

/*
 * exec_arena_grow - back the arena with wired, zeroed pages up to round_page(need)
 * (never past the extent) and return the bytes backed. Never sleeps: the
 * facility holds its exec_lock. Stops short, and says so, when no page is free.
 */
size_t
exec_arena_grow(void *arena, size_t need)
{
	vsize_t want = round_page(need);
	int grew = 0;

	if (arena == NULL || (vaddr_t)arena != arena_kva)
		return 0;
	if (want > arena_extent)
		want = arena_extent;
	while (arena_backed < want) {
		struct vm_page *pg = uvm_pagealloc(NULL, 0, NULL, UVM_PGA_ZERO);

		if (pg == NULL)
			break;
		if (!grew) {
			pmap_page_protect(arena_zero, VM_PROT_NONE);
			grew = 1;
		}
		pmap_kenter_pa(arena_kva + arena_backed, VM_PAGE_TO_PHYS(pg),
		    VM_PROT_READ | VM_PROT_WRITE, 0);
		arena_backed += PAGE_SIZE;
	}
	if (grew)
		pmap_update(pmap_kernel());
	return arena_backed;
}

void
exec_arena_free(void *arena)
{
	vsize_t off;

	if (arena == NULL || arena_kva == 0)
		return;
	for (off = 0; off < arena_backed; off += PAGE_SIZE) {
		paddr_t pa;

		if (pmap_extract(pmap_kernel(), arena_kva + off, &pa)) {
			struct vm_page *pg = PHYS_TO_VM_PAGE(pa);

			pmap_kremove(arena_kva + off, PAGE_SIZE);
			if (pg != NULL)
				uvm_pagefree(pg);
		}
	}
	pmap_update(pmap_kernel());
	uvm_km_free(kernel_map, arena_kva, arena_extent, UVM_KMF_VAONLY);
	pmap_page_protect(arena_zero, VM_PROT_NONE);
	uvm_pagefree(arena_zero);
	arena_zero = NULL;
	arena_kva = 0;
	arena_extent = 0;
	arena_backed = 0;
}

/*
 * vms_lnm_arena_selftest - prove, at module load, that the arena the executive
 * WROTE is the one d_mmap will PUBLISH: pmap_extract the arena base (the exact
 * resolution d_mmap does per page) and report it alongside the magic the
 * executive stamped via the kernel VA (vms_lnm_init). If pmap_extract fails, or
 * the magic is not 'LNMA', the userspace mmap roundtrip cannot work and every
 * SYS$SYSTEM translation will miss -- this line makes that visible on the console
 * instead of surfacing only as a downstream %OVMX-F-EXECINIT. Prints once; it is
 * a substrate self-check, not scaffolding (Rule 7: prove the seam, don't assume).
 */
void
vms_lnm_arena_selftest(void)
{
	void *base = vms_lnm_arena_base();
	paddr_t pa = 0;
	int ok;

	if (base == NULL) {
		printf("vms: lnm arena selftest: NO ARENA (alloc failed)\n");
		return;
	}
	ok = pmap_extract(pmap_kernel(), (vaddr_t)base, &pa) ? 1 : 0;
	printf("vms: lnm arena kva=%p size=%zu PAGE_SIZE=%u "
	       "pmap_extract=%d pa=0x%lx atop(pa)=0x%lx magic@kva=0x%08x\n",
	       base, vms_lnm_arena_size(), (unsigned)PAGE_SIZE,
	       ok, (unsigned long)pa, (unsigned long)atop(pa),
	       *(volatile uint32_t *)base);
}

/*
 * vms_mmap - the cdevsw d_mmap (referenced by vms_netbsd.c's vms_cdevsw). Resolves
 * one page of the read-only arena to its physical frame number, refusing any write
 * intent and any offset outside the arena.
 */
paddr_t
vms_mmap(dev_t self, off_t off, int prot)
{
	void *base = vms_lnm_arena_base();
	vaddr_t va;
	paddr_t pa;

	(void)self;

	/* No arena (vms_lnm_init has not run / failed): honest failure. */
	if (base == NULL)
		return (paddr_t)-1;
	/* Read-only, now and forever: reject any write intent. */
	if (prot & VM_PROT_WRITE)
		return (paddr_t)-1;
	/* Only the arena, only from its start, only within its extent. */
	if (off < 0 || (vsize_t)off >= round_page(vms_lnm_arena_size()))
		return (paddr_t)-1;

	/* Not backed yet (rd vms-c01): the zero page -- see arena_zero. */
	if ((vsize_t)off >= arena_backed)
		return atop(VM_PAGE_TO_PHYS(arena_zero));

	va = (vaddr_t)base + (vaddr_t)off;
	if (!pmap_extract(pmap_kernel(), va, &pa))
		return (paddr_t)-1;
	return atop(pa);
}
