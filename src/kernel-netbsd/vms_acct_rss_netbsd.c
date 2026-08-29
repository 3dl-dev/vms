/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_acct_rss_netbsd.c -- the dedicated uvm-only TU for the VAX SHOW SYSTEM
 * "Pages" / SHOW WORKING_SET resident-set read (rd vms-601). ovmx_task_rss_pages()
 * reads a pinned proc's current resident page count for the accounting snapshot
 * exec_task_pin() builds in exec_kbackend_netbsd.h.
 *
 * WHY A DEDICATED TU (and not exec_kbackend_netbsd.h / vms_netbsd.c). The resident
 * count lives behind <uvm/uvm_extern.h> (vm_resident_count) + <uvm/uvm_pmap.h>
 * (pmap_resident_count), and <uvm/uvm_extern.h> transitively pulls <sys/rbtree.h>,
 * whose rb_left/rb_right MACROS collide with OVMX's intrusive exec_rbtree_netbsd.h
 * (the DLM's lock-ID red-black tree) -- a header every OTHER executive TU includes
 * via vms_internal.h. This file therefore deliberately includes uvm but NOT
 * vms_internal.h / exec_kbackend.h / exec_rbtree.h; it declares by hand the single
 * entry point it exports (also prototyped in exec_kbackend_netbsd.h) so it needs
 * none of the executive headers. Mirrors vms_lnm_arena_netbsd.c exactly.
 *
 * WHY vm_resident_count() AND NOT vm->vm_rssize. The kernel's own ps/kinfo path
 * (kern_proc.c fill_kproc/fill_kproc2) reads the current RSS via
 * vm_resident_count(vm) == pmap_resident_count(vm->vm_map.pmap), NOT the raw
 * vm_rssize field -- vm_rssize is not maintained on __HAVE_NO_PMAP_STATS pmaps,
 * whereas the pmap resident-count IS the authoritative, portable figure (on VAX,
 * pmap.h defines it as pm_stats.resident_count). Reading the raw field would risk
 * a stale/zero count on some arches; this mirrors the kernel's real accessor.
 *
 * INV-6 / Rule 8 (clean-room). Nothing here fabricates state: a proc with an
 * address space reports its REAL resident count; a proc with none (kernel thread /
 * mid-exit) reports "no value" (return 0) so the caller honestly OMITS the "Pages"
 * column rather than printing a fabricated 0. OVMX glue over the PUBLIC NetBSD
 * proc(9)/uvm/pmap KPIs; no NetBSD or VSI source is copied.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/proc.h>        /* struct proc, p_vmspace, p_lock */
#include <sys/mutex.h>       /* mutex_owned (KASSERT the p_lock contract) */
#include <uvm/uvm_extern.h>  /* struct vmspace, vm_map, vm_resident_count() */
#include <uvm/uvm_pmap.h>    /* pmap_resident_count (VAX: pm_stats.resident_count) */

/* Also prototyped in exec_kbackend_netbsd.h (the caller); declared here by hand
 * too so this TU needs none of the rbtree-carrying executive headers. */
int ovmx_task_rss_pages(struct proc *p, uint64_t *pages_out);

int
ovmx_task_rss_pages(struct proc *p, uint64_t *pages_out)
{
	struct vmspace *vm;

	KASSERT(p == NULL || mutex_owned(p->p_lock));

	if (p == NULL || (vm = p->p_vmspace) == NULL)
		return 0;   /* no address space -> honest omission, never a fake 0 */
	*pages_out = (uint64_t)vm_resident_count(vm);
	return 1;
}
