/*
 * sys_lock.c - Lock Manager System Services ($ENQ, $DEQ)
 *
 * Routes exclusively through the kernel lock manager (the vms.ko module's
 * distributed lock manager, reached via /dev/vms ioctl through the
 * vms_kif_enq/deq/convert wrappers in libvmssys). There is no userspace
 * lock table and no POSIX flock() fallback -- the kernel module is the
 * single authoritative lock manager, matching the cluster-interop design
 * (docs/design-cluster-node.md).
 *
 * There is no absent-executive case here. The executive is INTEGRAL: PID 1
 * refuses to bring the system up unless /dev/vms is open, and holds it open
 * for the life of the system (src/ovmx_init/ovmx_init.c, executive_attach).
 * So by the time any image calls $ENQ, a reachable executive is guaranteed.
 * This file used to return SS$_NOSUCHDEV when /dev/vms would not open; that
 * was itself a fiction -- it encoded "OVMX runs, minus the executive", and
 * no such system exists. On OpenVMS the executive IS the operating system.
 * SS$_NOSUCHDEV keeps its real meaning elsewhere (a caller named a VMS
 * device that does not exist), which is a genuinely different condition.
 *
 * Lock modes (VMS compatible), from starlet.h:
 *   LCK$K_NLMODE (0) - Null lock (no access)
 *   LCK$K_CRMODE (1) - Concurrent Read
 *   LCK$K_CWMODE (2) - Concurrent Write
 *   LCK$K_PRMODE (3) - Protected Read
 *   LCK$K_PWMODE (4) - Protected Write
 *   LCK$K_EXMODE (5) - Exclusive
 */

/*
 * OVMX service register (rd vms-d89) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * WHICH PROOF THESE CITE, AND WHY IT MOVED (vms-ecf). Both
 * tests/qemu/test_syssvc_lock.c and tests/qemu/test_syssvc_lock_status.c drive
 * these three PUBLIC entry points across a real fork() against a real
 * /dev/vms, and the first is still the mutual-exclusion proof -- read it for
 * the NOQUEUE denial and the cross-process release. What decided the proof=
 * pointer is the register's price: it is paid only where a mutation of THIS
 * FILE is known to redden an assertion in the cited proof, and the three
 * kstat_to_ss() defects in tests/qemu/facility_defects.sh redden
 * test_syssvc_lock_status.c. No mutation of sys_lock.c's own code is currently
 * known to redden test_syssvc_lock.c, so citing it would have been a claim
 * this file cannot back.
 *
 * OVMX-EXECUTIVE: sys$enq (vms-ci.7) proof=tests/qemu/test_syssvc_lock_status.c -- there
 *     is no userspace lock table and no flock() fallback; the grant decision, the lock
 *     id and the value block all come back from the kernel lock manager.
 * OVMX-EXECUTIVE: sys$enqw (vms-ci.7) proof=tests/qemu/test_syssvc_lock_status.c -- the
 *     same request as $ENQ with the wait taken in the executive.
 * OVMX-EXECUTIVE: sys$deq (vms-ci.7) proof=tests/qemu/test_syssvc_lock_status.c -- one-line
 *     pass-through to vms_kif_deq.
 *
 * kstat_to_ss() below translates the kernel's status numbering into the public
 * ssdef.h values. That is a translation of the executive's answer, not a
 * substitute for it: it changes how the answer is spelled, never what it says.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "starlet.h"
#include "vms_kif.h"

/* Lock Status Block (VMS-compatible layout) */
struct lksb {
    uint16_t lksb$w_status;
    uint16_t lksb$w_reserved;
    uint32_t lksb$l_lkid;
    char     lksb$b_valblk[16];  /* Lock value block */
};

/*
 * There is no bind step here any more, and its absence is the fix, not an
 * omission (vms-9fc).
 *
 * This file used to call a local bind_to_executive() that called
 * vms_kif_open() -- and ONLY vms_kif_open(). It never registered, so the
 * executive rejected every $ENQ and $DEQ ioctl this file issued with -ESRCH.
 * That is worse than a local bug: the executive-retrofit design named this
 * file as "the one facility already wired to /dev/vms" and told every other
 * implementer to copy it, so the omission was propagated by design review.
 *
 * The sequence now lives once, in src/libvmssys/vms_kif.c's kif_bind(), so
 * every facility that reaches the executive inherits a registered process
 * instead of each caller re-remembering a step that was already forgotten
 * four times over. $ENQ/$DEQ simply call vms_kif_enq()/vms_kif_deq().
 */

/*
 * kstat_to_ss - Translate a kernel lock-manager status code into its
 * public ssdef.h SS$_xxx constant.
 *
 * The kernel module (src/kernel/vms_internal.h, SS__xxx) uses a compact
 * internal numbering scheme that does NOT match the public ssdef.h
 * values for most lock-manager codes (e.g. "not queued" is 40 in the
 * kernel; ssdef.h's SS$_NOTQUEUED is a different value -- see ssdef.h
 * itself, not a number recited here to drift out of sync with it). This
 * is the boundary where a raw kernel status crosses into the public
 * sys$enq/sys$enqw/sys$deq contract -- translate here, once, at the
 * point a status is stored
 * into the caller's LKSB or returned. Internal control flow in do_enq
 * (the wait-loop's success-bit and granted-mode checks) must keep using
 * the raw kernel value; only the value that actually leaves this file
 * gets mapped.
 *
 * The magic numbers on the left are the raw kernel SS__xxx values from
 * src/kernel/vms_internal.h (kept as literals rather than an #include,
 * since that header pulls in kernel-only headers and cannot be built
 * into glibc userspace code). Anything not listed here -- including
 * SS__NORMAL(1)/SS__BADPARAM(0x14)/SS__ACCVIO(0xC), which already
 * share the same numeric value in both schemes -- passes through
 * unchanged, so an unexpected kernel status is never silently mapped
 * to the wrong public constant.
 */
static uint32_t kstat_to_ss(uint32_t k)
{
    switch (k) {
    case 40:  return SS$_NOTQUEUED;    /* kernel SS__NOTQUEUED */
    case 100: return SS$_DEADLOCK;     /* kernel SS__DEADLOCK */
    case 108: return SS$_IVLOCKID;     /* kernel SS__IVLOCKID */
    case 112: return SS$_SUBLOCKS;     /* kernel SS__SUBLOCKS */
    case 116: return SS$_CVTUNGRANT;   /* kernel SS__CANCELGRANT -- same
                                         * concept as ssdef.h's "convert
                                         * ungrantable": a queued
                                         * conversion could not be
                                         * granted (deadlock avoidance) */
    case 120: return SS$_VALNOTVALID;  /* kernel SS__VALNOTVALID */
    default:  return k;
    }
}

/*
 * lckflags_to_kernel - Translate public OpenVMS LCK$M_* flag bits (real
 * $LCKDEF values, see starlet.h) into the kernel lock manager's internal
 * LCK_M_* bitmask (src/kernel/vms_ioctl.h) before crossing the /dev/vms
 * ioctl boundary. The public and kernel bit layouts are different (e.g.
 * public LCK$M_VALBLK is 0x01 but kernel LCK_M_VALBLK is 0x08); this keeps
 * the kernel's private numbering from leaking into the public $ENQ contract.
 * Only the flags the kernel lock manager actually implements are mapped;
 * the remaining real-VMS flags have no kernel equivalent and are dropped.
 */
static uint32_t lckflags_to_kernel(uint32_t vms_flags)
{
    uint32_t k = 0;
    if (vms_flags & LCK$M_VALBLK)  k |= LCK_M_VALBLK;
    if (vms_flags & LCK$M_CONVERT) k |= LCK_M_CONVERT;
    if (vms_flags & LCK$M_NOQUEUE) k |= LCK_M_NOQUEUE;
    if (vms_flags & LCK$M_SYSTEM)  k |= LCK_M_SYSTEM;
    return k;
}

/*
 * do_enq - Shared core for sys$enq / sys$enqw.
 *
 * Maps the VMS $ENQ parameter set onto vms_kif_enq (fresh lock request)
 * or vms_kif_convert (when LCK$M_CONVERT is set and the caller's LKSB
 * already holds a lock ID), then fills in the caller's LKSB.
 *
 * When `wait` is set (sys$enqw), the LCK_M_SYNC flag asks the kernel lock
 * manager to block in-kernel until the request is granted (or a deadlock is
 * detected) and to return the final status directly -- so a queued sync
 * request never returns until it is actually granted at the requested mode.
 * $ENQ (wait=0) returns immediately with whatever the kernel reports
 * (granted or queued); on a later grant the kernel delivers a completion AST
 * if astadr was supplied.
 */
static uint32_t do_enq(uint32_t efn, uint32_t lkmode, struct lksb *lksb,
                       uint32_t flags, const struct dsc$descriptor_s *resnam,
                       uint32_t parid, void (*astadr)(uint32_t), uint32_t astprm,
                       void (*blkastadr)(uint32_t), int wait)
{
    if (!lksb)
        return SS$_BADPARAM;

    uint8_t valblk[16];
    memcpy(valblk, lksb->lksb$b_valblk, sizeof(valblk));

    uint32_t lkid = 0;
    uint32_t status;

    /* Public LCK$M_* semantics are tested on `flags` (real-VMS bits); the
     * kernel ioctls receive the translated `kflags`. */
    uint32_t kflags = lckflags_to_kernel(flags);

    /*
     * $ENQW's synchronous "wait" is realized by asking the kernel lock
     * manager to BLOCK in-kernel until the request is granted (or a
     * deadlock is detected), via the LCK_M_SYNC flag. This replaces the
     * former userspace GETLKI poll loop, which busy-polled forever and
     * could not observe a deadlock that formed after the request was
     * queued. LCK_M_SYNC is inert for LCK$M_NOQUEUE (the kernel never
     * queues in that case), so it is safe to set unconditionally for wait.
     */
    if (wait)
        kflags |= LCK_M_SYNC;

    if ((flags & LCK$M_CONVERT) && lksb->lksb$l_lkid != 0) {
        lkid = lksb->lksb$l_lkid;
        status = vms_kif_convert(lkid, lkmode, kflags,
                                  (uint64_t)(uintptr_t)blkastadr, valblk);
    } else {
        char name[32] = "";
        if (resnam && resnam->dsc$a_pointer)
            dsc$strncpy(name, resnam, sizeof(name));

        status = vms_kif_enq(efn, lkmode, kflags, name, parid,
                              (uint64_t)(uintptr_t)astadr, astprm,
                              (uint64_t)(uintptr_t)blkastadr,
                              &lkid, valblk);
    }

    /* Translate raw kernel status -> public ssdef.h constant exactly once,
     * at the boundary where it is stored/returned. The kernel has already
     * blocked (for a queued sync request) and reports the final granted /
     * deadlock status directly -- no userspace wait loop remains. */
    uint32_t pub_status = kstat_to_ss(status);

    lksb->lksb$w_status = (uint16_t)pub_status;
    lksb->lksb$l_lkid = lkid;
    memcpy(lksb->lksb$b_valblk, valblk, sizeof(valblk));

    return pub_status;
}

/*
 * sys$enqw - Enqueue lock request and wait (synchronous).
 *
 * Blocks in-kernel (via the LCK_M_SYNC flag -- see do_enq) until the
 * requested lock mode is actually granted, unless LCK$M_NOQUEUE is specified.
 *
 * Parameters:
 *   efn      - Event flag set on completion
 *   lkmode   - Lock mode (LCK$K_xxx)
 *   lksb     - Lock status block
 *   flags    - Lock flags (LCK$M_xxx)
 *   resnam   - Resource name descriptor
 *   parid    - Parent lock ID (currently unused by the kernel manager)
 *   astadr   - Completion AST routine
 *   astprm   - AST parameter
 *   blkastadr- Blocking AST routine
 *   acmode   - Access mode (unused; the kernel manager does not
 *              segregate lock namespaces by access mode)
 *   rsdm_id  - Resource domain ID (unused; single-domain kernel manager)
 */
uint32_t sys$enqw(uint32_t efn, uint32_t lkmode, void *lksb_ptr,
                  uint32_t flags, const struct dsc$descriptor_s *resnam,
                  uint32_t parid, void (*astadr)(uint32_t), uint32_t astprm,
                  void (*blkastadr)(uint32_t), uint32_t acmode,
                  uint32_t rsdm_id) {
    (void)acmode; (void)rsdm_id;

    uint32_t status = do_enq(efn, lkmode, (struct lksb *)lksb_ptr, flags,
                              resnam, parid, astadr, astprm, blkastadr, 1);

    if (efn > 0)
        sys$setef(efn);

    return status;
}

/*
 * sys$enq - Enqueue lock request (asynchronous).
 *
 * Returns immediately with whatever the kernel lock manager reports:
 * granted, queued (SS$_NORMAL with the lock still at NL until a later
 * DEQ/CONVERT elsewhere frees the resource -- see do_enq's comment on
 * the missing completion AST), or SS$_NOTQUEUED if LCK$M_NOQUEUE was
 * specified and the mode is incompatible.
 */
uint32_t sys$enq(uint32_t efn, uint32_t lkmode, void *lksb_ptr,
                 uint32_t flags, const struct dsc$descriptor_s *resnam,
                 uint32_t parid, void (*astadr)(uint32_t), uint32_t astprm,
                 void (*blkastadr)(uint32_t), uint32_t acmode,
                 uint32_t rsdm_id) {
    (void)acmode; (void)rsdm_id;

    uint32_t status = do_enq(efn, lkmode, (struct lksb *)lksb_ptr, flags,
                              resnam, parid, astadr, astprm, blkastadr, 0);

    if (efn > 0 && (status & 1))
        sys$setef(efn);

    return status;
}

/*
 * sys$deq - Dequeue (release) a lock.
 *
 * Releases the lock identified by lkid via the kernel lock manager.
 * If LCK$M_VALBLK is set in flags and valblk is non-NULL, the supplied
 * value block replaces the resource's persistent value block (VMS
 * semantics: $DEQ's valblk argument is input-only -- it is not filled
 * in with the prior value).
 *
 * Parameters:
 *   lkid   - Lock ID (from $ENQ/$ENQW's lksb)
 *   valblk - Optional 16-byte value block to store in the resource
 *   acmode - Access mode (unused; see sys$enqw)
 *   flags  - Dequeue flags (LCK$M_xxx)
 */
uint32_t sys$deq(uint32_t lkid, void *valblk, uint32_t acmode,
                 uint32_t flags) {
    (void)acmode;

    /* Translate public flags to the kernel bitmask at the boundary (same as
     * $ENQ). NOTE: real OpenVMS $DEQ has its own flag namespace (LCK$M_DEQALL
     * /CANCEL/INVVALBLK) distinct from the $ENQ flags defined in starlet.h;
     * the kernel deq currently only acts on the VALBLK bit, so only that is
     * meaningful here. Full $DEQ flag support is tracked separately. */
    return kstat_to_ss(vms_kif_deq(lkid, (uint8_t *)valblk,
                                   lckflags_to_kernel(flags)));
}
