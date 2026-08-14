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
 *   1. exec_arena_alloc / exec_arena_free (exec_kbackend.h §10) -- the ALLOCATION
 *      of the one read-only-publishable arena. On NetBSD the arena is
 *      physically-backed, WIRED kernel memory from uvm_km_alloc(kernel_map, ...,
 *      UVM_KMF_WIRED | UVM_KMF_ZERO): a zeroed, page-aligned kernel VA whose pages
 *      are real RAM that never pages out, so d_mmap below can resolve each page to
 *      a physical frame. (The Linux backend uses vmalloc_user for the same role.)
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

/*
 * The two facility entry points this glue calls, DEFINED in the shared
 * src/kernel-core/vms_lnm.c (which this TU cannot include a header of without
 * dragging in the rbtree). Declared by hand, byte-matching vms_lnm.c's
 * signatures; a mismatch is a link-time error.
 */
void  *vms_lnm_arena_base(void);
size_t vms_lnm_arena_size(void);

/*
 * exec_arena_alloc - allocate the ONE read-only-publishable arena (exec_kbackend.h
 * §10). Wired, page-aligned, zeroed kernel memory whose pages d_mmap can resolve
 * to physical frames.
 *
 * SIZE TRACKING. The facility frees with only the pointer (the Linux vfree
 * contract), but uvm_km_free needs the extent -- so we over-allocate one leading
 * page, stash the whole extent there, and return the still-page-aligned arena page
 * above it. The leading page is never mapped to userspace: d_mmap addresses the
 * arena base (this returned pointer), PAGE_SIZE above the extent header.
 */
void *
exec_arena_alloc(size_t n)
{
	vsize_t total = round_page(n) + PAGE_SIZE;   /* +1 page: the extent header */
	vaddr_t va = uvm_km_alloc(kernel_map, total, 0,
	                          UVM_KMF_WIRED | UVM_KMF_ZERO);
	if (va == 0)
		return NULL;
	*(vsize_t *)va = total;                      /* remember the extent for free */
	return (void *)(va + PAGE_SIZE);             /* page-aligned, zeroed arena */
}

void
exec_arena_free(void *arena)
{
	vaddr_t va;
	vsize_t total;

	if (arena == NULL)
		return;
	va = (vaddr_t)arena - PAGE_SIZE;
	total = *(vsize_t *)va;
	uvm_km_free(kernel_map, va, total, UVM_KMF_WIRED);
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

	va = (vaddr_t)base + (vaddr_t)off;
	if (!pmap_extract(pmap_kernel(), va, &pa))
		return (paddr_t)-1;
	return atop(pa);
}
