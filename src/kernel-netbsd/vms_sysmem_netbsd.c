/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_sysmem_netbsd.c -- the dedicated uvm-only TU for the VAX SHOW MEMORY
 * "Physical Memory Usage" section (rd vms-a3cd). ovmx_sysmem_bytes() reads the
 * system-wide physical page totals the executive reports to $GETSYI / SHOW
 * MEMORY: total managed memory and current free memory, in bytes.
 *
 * WHY A DEDICATED TU (identical rationale to vms_acct_rss_netbsd.c /
 * vms_lnm_arena_netbsd.c). The page accounting lives behind
 * <uvm/uvm_extern.h> (struct uvmexp, uvm_availmem()), and <uvm/uvm_extern.h>
 * transitively pulls <sys/rbtree.h>, whose rb_left/rb_right MACROS collide with
 * OVMX's intrusive exec_rbtree_netbsd.h (the header every OTHER executive TU
 * includes via vms_internal.h). This file therefore includes uvm but NOT
 * vms_internal.h / exec_kbackend.h / exec_rbtree.h; it declares by hand the
 * single entry point it exports (also prototyped in exec_kbackend_netbsd.h) so
 * it needs none of the executive headers.
 *
 * WHY uvm_availmem(true) AND NOT uvmexp.free. On NetBSD 10 the free-page count
 * is a lazily-synced per-CPU counter; the kernel's OWN vmstat/sysctl path reads
 * it through uvm_availmem(true) (uvm_meter.c: `u.free = uvm_availmem(true)' and
 * `uvmexp.free = (int)uvm_availmem(true)'), whose `true' argument forces
 * cpu_count_sync() before cpu_count_get(CPU_COUNT_FREEPAGES). Reading the raw
 * uvmexp.free field would risk a stale snapshot -- the same
 * maintained-accessor-not-raw-field discipline as vm_resident_count() in the
 * rss TU (kern_proc.c reads the pmap resident count, never vm_rssize).
 * uvmexp.npages (total managed pages) is set once at boot and read directly.
 * Both are converted to bytes with the kernel's own PAGE_SIZE, so the KIF ABI
 * carries an arch-neutral byte count -- no VMS/host page-size skew crosses the
 * wire.
 *
 * INV-6 / Rule 8 (clean-room). Nothing here fabricates state: the figures are
 * the executive's real uvm counters. Fields VMS also shows but for which OVMX
 * has no faithful uvm source (the Modified page list, VIO cache, pool,
 * paging-file usage) are NOT sourced here -- the renderer honestly OMITS those
 * sections rather than print a fabricated 0. If uvm is not yet up (npages <= 0)
 * this returns 0 so the caller omits the Physical Memory section too, never a
 * fake zero. OVMX glue over the PUBLIC NetBSD uvm(9) KPIs; no NetBSD or VSI
 * source is copied.
 */

#include <sys/param.h>       /* PAGE_SIZE */
#include <sys/types.h>
#include <uvm/uvm_extern.h>  /* struct uvmexp uvmexp; int uvm_availmem(bool) */

/* Also prototyped in exec_kbackend_netbsd.h (the caller); declared here by hand
 * too so this TU needs none of the rbtree-carrying executive headers. */
int ovmx_sysmem_bytes(uint64_t *total_bytes, uint64_t *free_bytes);

int
ovmx_sysmem_bytes(uint64_t *total_bytes, uint64_t *free_bytes)
{
	int npages = uvmexp.npages;
	int freepg;

	if (npages <= 0)
		return 0;   /* uvm not up yet -> honest omission, never a fake 0 */

	freepg = uvm_availmem(true);   /* maintained accessor; syncs stale counters */
	if (freepg < 0)
		freepg = 0;

	*total_bytes = (uint64_t)npages * (uint64_t)PAGE_SIZE;
	*free_bytes  = (uint64_t)freepg * (uint64_t)PAGE_SIZE;
	return 1;
}
