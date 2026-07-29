/*
 * sys_event.c - Event Flag System Services
 *
 * VMS event flags are single-bit semaphores used for synchronisation.
 * Each process has 128 event flags in 4 clusters of 32:
 *   Cluster 0 (flags 0-31):   local
 *   Cluster 1 (flags 32-63):  local
 *   Cluster 2 (flags 64-95):  common (shared between processes)
 *   Cluster 3 (flags 96-127): common
 *
 * Local flags (0-63) are genuinely process-private by VMS design and stay
 * on the PCB's pthread mutex/condition variable -- no kernel round trip
 * needed or wanted for those.
 *
 * Common flags (64-127) route exclusively through the kernel module (the
 * vms.ko VMS executive, reached via /dev/vms ioctl through the vms_kif_*
 * wrappers in libvmssys -- see src/kernel/vms_eflag.c's vms_common_ef_lock
 * / vms_common_ef_list), matching the sys_lock.c pattern: there is no
 * userspace common-cluster table and no PCB fallback. A process must
 * associate via sys$ascefc before any common-range SETEF/CLREF/WAITFR/
 * WFLOR/WFLAND/READEF resolves -- the kernel enforces this and returns
 * SS$_UNASEFC otherwise (see ef_kstat_to_ss below). Docker containers
 * have no /dev/vms, so common-flag operations return SS$_NOSUCHDEV
 * there -- that is accepted, by design, not a bug (CLAUDE.md Rule 9).
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "starlet.h"
#include "vms/pcb.h"
#include "vms_kif.h"

/* Total event flags supported (4 clusters x 32) */
#define EF_TOTAL 128

/* First EFN in the common (kernel-shared) range. Flags below this are
 * process-local and never leave the PCB. */
#define EF_COMMON_BASE 64

/*
 * Lazily open /dev/vms for this thread. vms_kif_open() is idempotent (it
 * no-ops if the thread-local fd is already open), so it is safe to call
 * on every common-cluster operation. Returns 0 if the kernel device is
 * available, -1 otherwise (e.g. Docker mode, which has no /dev/vms).
 *
 * Mirrors src/libvms/syssvc/sys_lock.c's ensure_kif_open() -- duplicated
 * rather than shared because it is a two-line static helper and the two
 * files have no common private header to hang it on.
 */
static int ensure_kif_open(void)
{
    return vms_kif_open() >= 0 ? 0 : -1;
}

/*
 * ef_kstat_to_ss - Translate a kernel event-flag status code (raw
 * SS__xxx values from src/kernel/vms_internal.h) into its public
 * ssdef.h SS$_xxx constant, at the boundary where a kernel-module
 * status crosses into the public sys$setef/.../sys$ascefc contract.
 *
 * Same rationale as sys_lock.c's kstat_to_ss: the kernel module uses a
 * compact internal numbering scheme that does not match the public
 * ssdef.h values for most of these codes. The magic numbers on the left
 * are the raw kernel SS__xxx values (kept as literals rather than an
 * #include, since vms_internal.h pulls in kernel-only headers and
 * cannot be built into glibc userspace code):
 *
 *   kernel SS__NORMAL  (1)  == public SS$_NORMAL  (1)  -- passthrough
 *   kernel SS__WASSET  (9)  == public SS$_WASSET  (9)  -- passthrough
 *   kernel SS__WASCLR  (5)  != public SS$_WASCLR  (1)  -- mapped below
 *   kernel SS__ILLEFC  (44) != public SS$_ILLEFC  (2260) -- mapped below
 *   kernel SS__UNASEFC (48) != public SS$_UNASEFC (2280) -- mapped below
 *
 * NOTE (flagged, not silently resolved -- see unresolved_constraints in
 * this item's return): the kernel's SS__INSFMEM is also numerically 44
 * (src/kernel/vms_internal.h defines it as 0x2C), colliding with
 * SS__ILLEFC. vms_ioctl_ascefc() returns SS__INSFMEM only on a kzalloc
 * failure (GFP_ATOMIC, out-of-memory on cluster creation) -- an
 * essentially untestable path -- so that failure would be misreported
 * as SS$_ILLEFC here. This is a pre-existing kernel-side numbering
 * collision (not introduced by this wiring) and is out of scope for a
 * userspace-only item; flagged for the kernel owner.
 *
 * Anything not listed here passes through unchanged, so an unexpected
 * kernel status is never silently mapped to the wrong public constant.
 */
static uint32_t ef_kstat_to_ss(uint32_t k)
{
    switch (k) {
    case 5:  return SS$_WASCLR;   /* kernel SS__WASCLR */
    case 44: return SS$_ILLEFC;   /* kernel SS__ILLEFC (see note above) */
    case 48: return SS$_UNASEFC;  /* kernel SS__UNASEFC */
    default: return k;
    }
}

/*
 * sys$setef - Set an event flag.
 *
 * Sets the specified event flag bit. If any threads are waiting
 * on this flag (via $WAITFR, $WFLOR, $WFLAND), they are woken.
 *
 * Returns:
 *   SS$_WASSET - Flag was already set before this call
 *   SS$_WASCLR - Flag was clear before this call (now set)
 *   SS$_ILLEFC - Event flag number out of range
 */
uint32_t sys$setef(uint32_t efn) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    if (efn >= EF_COMMON_BASE) {
        if (ensure_kif_open() < 0) return SS$_NOSUCHDEV;
        return ef_kstat_to_ss(vms_kif_setef(efn));
    }

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);
    int bit  = (int)(efn % 32);
    uint32_t mask = (uint32_t)1 << bit;

    pthread_mutex_lock(&pcb->ef_lock);
    uint32_t was_set = (pcb->ef_clusters[cidx] & mask) ? 1 : 0;
    pcb->ef_clusters[cidx] |= mask;
    pthread_cond_broadcast(&pcb->ef_cond);
    pthread_mutex_unlock(&pcb->ef_lock);

    return was_set ? SS$_WASSET : SS$_WASCLR;
}

/*
 * sys$clref - Clear an event flag.
 *
 * Returns:
 *   SS$_WASSET - Flag was set before clearing
 *   SS$_WASCLR - Flag was already clear
 *   SS$_ILLEFC - Event flag number out of range
 */
uint32_t sys$clref(uint32_t efn) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    if (efn >= EF_COMMON_BASE) {
        if (ensure_kif_open() < 0) return SS$_NOSUCHDEV;
        return ef_kstat_to_ss(vms_kif_clref(efn));
    }

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);
    int bit  = (int)(efn % 32);
    uint32_t mask = (uint32_t)1 << bit;

    pthread_mutex_lock(&pcb->ef_lock);
    uint32_t was_set = (pcb->ef_clusters[cidx] & mask) ? 1 : 0;
    pcb->ef_clusters[cidx] &= ~mask;
    pthread_mutex_unlock(&pcb->ef_lock);

    return was_set ? SS$_WASSET : SS$_WASCLR;
}

/*
 * sys$waitfr - Wait for a single event flag to be set.
 *
 * If the flag is already set, returns immediately.
 * Otherwise blocks until another thread/AST sets the flag.
 *
 * Returns:
 *   SS$_NORMAL - Flag is set
 *   SS$_ILLEFC - Event flag number out of range
 */
uint32_t sys$waitfr(uint32_t efn) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    if (efn >= EF_COMMON_BASE) {
        if (ensure_kif_open() < 0) return SS$_NOSUCHDEV;
        return ef_kstat_to_ss(vms_kif_waitfr(efn));
    }

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);
    int bit  = (int)(efn % 32);
    uint32_t mask = (uint32_t)1 << bit;

    pthread_mutex_lock(&pcb->ef_lock);
    while (!(pcb->ef_clusters[cidx] & mask))
        pthread_cond_wait(&pcb->ef_cond, &pcb->ef_lock);
    pthread_mutex_unlock(&pcb->ef_lock);

    return SS$_NORMAL;
}

/*
 * sys$wflor - Wait for any flags in a mask to be set (logical OR).
 *
 * efn selects the cluster (efn / 32 gives cluster index).
 * mask is the 32-bit pattern of flags within that cluster to wait on.
 * Returns as soon as ANY of the masked flags become set.
 */
uint32_t sys$wflor(uint32_t efn, uint32_t mask) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    if (efn >= EF_COMMON_BASE) {
        if (ensure_kif_open() < 0) return SS$_NOSUCHDEV;
        return ef_kstat_to_ss(vms_kif_wflor(efn, mask));
    }

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);

    pthread_mutex_lock(&pcb->ef_lock);
    while ((pcb->ef_clusters[cidx] & mask) == 0)
        pthread_cond_wait(&pcb->ef_cond, &pcb->ef_lock);
    pthread_mutex_unlock(&pcb->ef_lock);

    return SS$_NORMAL;
}

/*
 * sys$wfland - Wait for all flags in a mask to be set (logical AND).
 *
 * Same cluster selection as $WFLOR, but waits until ALL masked
 * flags are set simultaneously.
 */
uint32_t sys$wfland(uint32_t efn, uint32_t mask) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    if (efn >= EF_COMMON_BASE) {
        if (ensure_kif_open() < 0) return SS$_NOSUCHDEV;
        return ef_kstat_to_ss(vms_kif_wfland(efn, mask));
    }

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);

    pthread_mutex_lock(&pcb->ef_lock);
    while ((pcb->ef_clusters[cidx] & mask) != mask)
        pthread_cond_wait(&pcb->ef_cond, &pcb->ef_lock);
    pthread_mutex_unlock(&pcb->ef_lock);

    return SS$_NORMAL;
}

/*
 * sys$readef - Read event flag state.
 *
 * Reads the current state of a 32-flag cluster and optionally
 * returns it in *state. Also indicates whether the specific
 * flag efn is set or clear.
 *
 * Returns:
 *   SS$_WASSET - The specific flag is set
 *   SS$_WASCLR - The specific flag is clear
 *   SS$_ILLEFC - Event flag number out of range
 */
uint32_t sys$readef(uint32_t efn, uint32_t *state) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    if (efn >= EF_COMMON_BASE) {
        if (ensure_kif_open() < 0) return SS$_NOSUCHDEV;
        uint32_t kstate = 0;
        uint32_t kstatus = vms_kif_readef(efn, &kstate);
        if (state) *state = kstate;
        return ef_kstat_to_ss(kstatus);
    }

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);
    int bit  = (int)(efn % 32);
    uint32_t mask = (uint32_t)1 << bit;

    pthread_mutex_lock(&pcb->ef_lock);
    if (state) {
        *state = pcb->ef_clusters[cidx];
    }
    uint32_t was_set = (pcb->ef_clusters[cidx] & mask) ? 1 : 0;
    pthread_mutex_unlock(&pcb->ef_lock);

    return was_set ? SS$_WASSET : SS$_WASCLR;
}

/*
 * sys$synch - Synchronize with async system service completion.
 *
 * Waits for the event flag to be set, then checks the IOSB status.
 * This is the standard VMS pattern for waiting on async services:
 *   status = sys$qio(..., efn, ..., iosb, ...);
 *   if (status & 1) status = sys$synch(efn, iosb);
 *   if (status & 1) status = iosb[0];
 *
 * Parameters:
 *   efn  - Event flag number to wait on
 *   iosb - Pointer to I/O Status Block (first word is status)
 *          If NULL, just waits for the event flag.
 *
 * Returns:
 *   The IOSB status word if iosb is provided.
 *   SS$_NORMAL if iosb is NULL and the wait succeeded.
 *   Error from sys$waitfr on failure.
 */
uint32_t sys$synch(uint32_t efn, void *iosb) {
    uint32_t status = sys$waitfr(efn);
    if (!(status & 1)) return status;

    if (iosb) {
        /* Return the status word from the IOSB (first 16-bit word,
         * but VMS convention zero-extends to 32 bits) */
        return (uint32_t)(*(uint16_t *)iosb);
    }
    return SS$_NORMAL;
}

/* --- Common event flag cluster --- */

/*
 * sys$ascefc - Associate Common Event Flag Cluster.
 *
 * Routes to the kernel module's named common-cluster table
 * (src/kernel/vms_eflag.c: vms_common_ef_lock / vms_common_ef_list) via
 * vms_kif_ascefc -- this is the executive-backed association that makes
 * SETEF/WAITFR/etc. on efn 64-127 visible across processes that
 * associate the same cluster name. No userspace fallback: absent
 * /dev/vms, this fails honestly with SS$_NOSUCHDEV (CLAUDE.md Rule 9).
 */
uint32_t sys$ascefc(uint32_t efn, const struct dsc$descriptor_s *name,
                    uint32_t prot, uint32_t perm) {
    if (efn != EF_COMMON_BASE && efn != 96) return SS$_ILLEFC;
    if (ensure_kif_open() < 0) return SS$_NOSUCHDEV;

    char cname[32] = "";
    if (name && name->dsc$a_pointer)
        dsc$strncpy(cname, name, sizeof(cname));

    return ef_kstat_to_ss(vms_kif_ascefc(efn, cname, prot, perm));
}

/*
 * sys$dacefc - Disassociate from Common Event Flag Cluster.
 */
uint32_t sys$dacefc(uint32_t efn) {
    if (efn != EF_COMMON_BASE && efn != 96) return SS$_ILLEFC;
    if (ensure_kif_open() < 0) return SS$_NOSUCHDEV;

    return ef_kstat_to_ss(vms_kif_dacefc(efn));
}

/*
 * sys$dlcefc - Delete Common Event Flag Cluster (stub).
 *
 * NOT wired: vms.ko has no DLCEFC ioctl (the kernel-side cluster list
 * only supports associate/disassociate with refcounting -- a cluster is
 * freed automatically when its last associate disassociates, unless
 * marked permanent). Deleting a *permanent* named cluster outright has
 * no kernel entry point to call, so this stays a stub rather than
 * silently faking success against a nonexistent kernel facility. Out of
 * scope for this item (ioctls 0x20-0x27 only); tracked as an
 * unresolved_constraints entry in this item's return.
 */
uint32_t sys$dlcefc(const struct dsc$descriptor_s *name) {
    (void)name;
    return SS$_NORMAL;
}
