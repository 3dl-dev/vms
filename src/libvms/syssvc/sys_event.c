/*
 * sys_event.c - Event Flag System Services
 *
 * VMS event flags are single-bit semaphores. Each process sees 128 of them
 * in 4 clusters of 32:
 *   Cluster 0 (flags 0-31):   local
 *   Cluster 1 (flags 32-63):  local
 *   Cluster 2 (flags 64-95):  common -- named, SHARED between processes
 *   Cluster 3 (flags 96-127): common -- named, SHARED between processes
 *
 * EVERY ONE OF THEM LIVES IN THE EXECUTIVE (src/kernel/vms_eflag.c, reached
 * through /dev/vms by the vms_kif_* wrappers in libvmssys). This file holds
 * no flag state of its own. It is a translation layer and nothing else --
 * VMS descriptors in, /dev/vms ioctls out, executive statuses back.
 *
 * WHAT THIS FILE USED TO BE, AND WHY THAT MATTERED (vms-2a8)
 *
 * It implemented all 128 flags in per-process memory: struct vms_pcb's
 * ef_clusters[] guarded by a pthread mutex and condvar. Every event-flag
 * entry point of the kernel-interface client -- vms_kif_setef, clref,
 * waitfr, wflor, wfland, readef, ascefc, dacefc -- had ZERO callers
 * product-wide, while src/kernel/vms_eflag.c sat there implementing common
 * clusters correctly on a module-global list that nothing ever asked for.
 * sys$ascefc was `return SS$_NORMAL;` with a TODO and no side effect at
 * all: a process asked to join a named common cluster, was told it had
 * succeeded, and shared nothing with anybody.
 *
 * Under CLAUDE.md Rule 11 that is a facade -- a system facility living in
 * per-process memory, reporting success while sharing nothing -- and under
 * Rule 10 the $ASCEFC stub was the illegal third answer, a plausible-looking
 * handler for a condition VMS never faces. The decisive test is
 * A-writes/B-reads and it lives in tests/qemu/test_syssvc_ef_mproc.c: one
 * process sets a common flag, ANOTHER process must see it. Single-process
 * coverage cannot see the difference, which is exactly how this survived --
 * tests/qemu/test_kmod_eflag.c passed 11/11 against a deliberately
 * per-process executive.
 *
 * TWO RULES FOR ANYONE EDITING THIS FILE
 *
 * 1. NO PER-PROCESS FALLBACK, EVER. Not when /dev/vms cannot be opened, not
 *    "just for local flags", not "to keep the unit tests running". The
 *    executive is INTEGRAL: PID 1 refuses to bring the system up without it
 *    (src/ovmx_init/ovmx_init.c) so its absence is unreachable, and
 *    vms_kif's kif_bind() completes open->register itself before every
 *    ioctl. A caller that reaches a sys$ entry point here has an executive.
 *
 * 2. LOCAL CLUSTERS ARE PER-PROCESS ON VMS TOO -- but that is the
 *    EXECUTIVE's per-process state (proc->ef.local[] in vms_eflag.c, keyed
 *    by the thread-group id), not this image's. Routing flags 0-63 to the
 *    executive is not a bug to be "optimised away" into local memory: it is
 *    what makes a wait scheduler-visible, and what lets the executive set a
 *    flag on I/O completion. Do not add a userspace shortcut for them.
 */

/*
 * OVMX service register (rd vms-d89) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * The pass-throughs below are each named by the A-writes/B-reads proof they
 * cite: one process sets, clears or associates, ANOTHER process reads it back
 * through the public sys$ API. That proof is the price of the exemption --
 * reaching vms_kif_* is necessary and nowhere near sufficient, and an ignored
 * call satisfies it.
 *
 * OVMX-EXECUTIVE: sys$setef (vms-2a8) proof=tests/qemu/test_syssvc_ef_mproc.c -- one-line
 *     pass-through to vms_kif_setef; the flag and its wakeups are the executive's.
 * OVMX-EXECUTIVE: sys$clref (vms-2a8) proof=tests/qemu/test_syssvc_ef_mproc.c -- one-line
 *     pass-through to vms_kif_clref.
 * OVMX-EXECUTIVE: sys$waitfr (vms-2a8) proof=tests/qemu/test_syssvc_ef_mproc.c -- the wait
 *     is a kernel wait queue; the waiter child is woken by another process's $SETEF.
 * OVMX-EXECUTIVE: sys$readef (vms-2a8) proof=tests/qemu/test_syssvc_ef_mproc.c -- the whole
 *     cluster word comes back from the executive; no flag state exists in this file.
 * OVMX-EXECUTIVE: sys$ascefc (vms-2a8) proof=tests/qemu/test_syssvc_ef_mproc.c -- the named
 *     cluster list is node-wide in the executive; the name is only marshalled here.
 * OVMX-EXECUTIVE: sys$dacefc (vms-2a8) proof=tests/qemu/test_syssvc_ef_mproc.c -- one-line
 *     pass-through to vms_kif_dacefc.
 * OVMX-EXECUTIVE: sys$dlcefc (vms-2a8) proof=tests/qemu/test_syssvc_ef_mproc.c -- one-line
 *     pass-through to vms_kif_dlcefc.
 *
 * $WFLOR and $WFLAND are the same shape as $WAITFR and route the same way, but
 * NO test under tests/qemu names either of them. Measured, and re-runnable:
 *     grep -rn 'sys[$]wflor\|sys[$]wfland' tests/
 * matches only the read-only tier1 corpus examples -- nothing OVMX itself runs
 * calls either service. So they
 * cannot buy the full exemption, and they do not pretend to: what is missing is
 * the proof, and that is what their local half says.
 *
 * OVMX-PARTIAL: sys$wflor (vms-2a8) -- exec: the wait and every flag it tests are
 *     the executive's; this is a one-line pass-through to vms_kif_wflor.
 * OVMX-LOCAL: sys$wflor -- nothing of the answer is computed here, but no
 *     A-writes/B-reads test names this service, so full residency is UNPROVEN.
 * OVMX-PARTIAL: sys$wfland (vms-2a8) -- exec: same one-line pass-through, to
 *     vms_kif_wfland.
 * OVMX-LOCAL: sys$wfland -- same gap: no test under tests/qemu names it.
 *
 * OVMX-PARTIAL: sys$synch (vms-2a8) -- exec: the wait, inherited whole from
 *     $WAITFR, including the guarantee that it cannot return success on a clear flag.
 * OVMX-LOCAL: sys$synch -- the status it RETURNS is read out of the caller's own
 *     IOSB, in this process's memory. The executive never sees that word.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "starlet.h"
#include "descrip.h"
#include "vms_kif.h"

/*
 * NO EF_TOTAL RANGE CHECK IN THIS FILE, DELIBERATELY.
 *
 * The executive owns the flags, so the executive decides what is an event
 * flag: it answers SS$_ILLEFC for a number that is not one (>= 128) and
 * SS$_UNASEFC for a common flag whose cluster this process has not
 * associated with. A pre-check here would be this layer fabricating its own
 * answer about executive-owned state -- the exact shape Rule 11's corollary
 * forbids -- and it would shadow UNASEFC, which is a different condition
 * with a different meaning to the caller.
 */

/*
 * clusnam - copy a VMS string descriptor into a C string for the executive.
 * Shared by $ASCEFC and $DLCEFC, which name the same clusters.
 *
 * NO VALIDATION, AND NO INVENTED STATUS FOR A BAD NAME. The public docs
 * quoted in docs/oracle/vax73-event-flags.md give the legal name lengths
 * ("1 to 15 bytes") but the online help carries no Condition Values section
 * for $ASCEFC, so what VMS RETURNS for a zero-length or over-long cluster
 * name is not pinned. Rule 10: when the VMS behaviour cannot be found, you
 * are not authorized to invent one -- so this does not manufacture an
 * SS$_IVLOGNAM (or any other plausible-looking status) it cannot back up.
 * It follows the in-tree descriptor convention instead (sys_lock.c's
 * resnam, sys_logical.c's lognam): tolerate a null descriptor, hand the
 * bytes to the executive, and let the executive answer. Raised as an open
 * question rather than settled here.
 */
static void clusnam(const struct dsc$descriptor_s *name, char *buf, size_t buflen)
{
    buf[0] = '\0';
    if (name && name->dsc$a_pointer)
        dsc$strncpy(buf, name, buflen);
}

/*
 * sys$setef - Set an event flag.
 *
 * Sets the flag in the executive and wakes every process waiting on it --
 * for a common cluster, that includes processes other than this one.
 *
 * Returns:
 *   SS$_WASSET  - Flag was already set before this call
 *   SS$_WASCLR  - Flag was clear before this call (now set). NOTE this is
 *                 numerically SS$_NORMAL: SS$_WASCLR is 1 on VMS
 *                 (docs/oracle/vax73-event-flags.md). Test for SS$_WASSET
 *                 to discriminate; do not expect a distinct clear value.
 *   SS$_UNASEFC - Common flag, but this process has not $ASCEFC'd its cluster
 *   SS$_ILLEFC  - Not an event flag number
 */
uint32_t sys$setef(uint32_t efn) {
    return vms_kif_setef(efn);
}

/*
 * sys$clref - Clear an event flag.
 *
 * Returns:
 *   SS$_WASSET  - Flag was set before clearing
 *   SS$_WASCLR  - Flag was already clear
 *   SS$_UNASEFC - Common flag whose cluster is not associated
 *   SS$_ILLEFC  - Not an event flag number
 */
uint32_t sys$clref(uint32_t efn) {
    return vms_kif_clref(efn);
}

/*
 * sys$waitfr - Wait for a single event flag to be set.
 *
 * Blocks in the executive on a kernel wait queue, so the process is
 * genuinely asleep to the scheduler and can be woken by ANOTHER process's
 * $SETEF on a common flag. Returns immediately if the flag is already set.
 *
 * IT CANNOT RETURN SS$_NORMAL WHILE THE FLAG IS CLEAR, and that is a
 * guarantee, not an aspiration -- $SYNCH below depends on it. A host signal
 * does not end the wait: the executive abandons the ioctl without writing a
 * status and libvmssys' kif_wait_call() re-enters it. See the INTERRUPTED
 * WAITS note in src/kernel/vms_eflag.c and the oracle transcript in
 * docs/oracle/vax73-event-flags.md §4 -- VMS has no "your wait was
 * interrupted" condition value, so under Rule 10 that condition is made
 * unreachable rather than given a plausible-looking status.
 *
 * Returns:
 *   SS$_NORMAL  - Flag is set
 *   SS$_UNASEFC - Common flag whose cluster is not associated
 *   SS$_ILLEFC  - Not an event flag number
 */
uint32_t sys$waitfr(uint32_t efn) {
    return vms_kif_waitfr(efn);
}

/*
 * sys$wflor - Wait for any flag in a mask to be set (logical OR).
 *
 * efn is the number of ANY event flag in the cluster to wait on -- that is
 * how VMS identifies the cluster (HELP SYSTEM_SERVICES $WFLOR Arguments on
 * the reference lab V7.3; see docs/oracle/vax73-event-flags.md). mask is
 * the 32-bit pattern of flags within that cluster. Returns as soon as ANY
 * of the masked flags is set.
 */
uint32_t sys$wflor(uint32_t efn, uint32_t mask) {
    return vms_kif_wflor(efn, mask);
}

/*
 * sys$wfland - Wait for all flags in a mask to be set (logical AND).
 *
 * Same cluster selection as $WFLOR; waits until ALL masked flags are set.
 */
uint32_t sys$wfland(uint32_t efn, uint32_t mask) {
    return vms_kif_wfland(efn, mask);
}

/*
 * sys$readef - Read event flag state.
 *
 * Returns the whole 32-flag cluster in *state and reports whether the
 * specific flag efn is set. For a common cluster the state read back is the
 * state every associated process sees.
 *
 * Returns:
 *   SS$_WASSET  - The specific flag is set
 *   SS$_WASCLR  - The specific flag is clear
 *   SS$_UNASEFC - Common flag whose cluster is not associated
 *   SS$_ILLEFC  - Not an event flag number
 */
uint32_t sys$readef(uint32_t efn, uint32_t *state) {
    return vms_kif_readef(efn, state);
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
 * THIS ROUTINE READS THE IOSB ONLY BECAUSE $WAITFR CANNOT LIE ABOUT THE
 * FLAG. It has no way to tell a real completion from a spurious wake, so
 * every guarantee it makes about the IOSB is inherited from $WAITFR's:
 * $WAITFR returns success only when the flag really is set, therefore the
 * I/O really did complete, therefore the IOSB really was filled. When
 * $WAITFR reported SS$_NORMAL for a signal-interrupted wait (fixed in
 * vms-2a8 round 2) this handed the caller a status word the I/O never
 * wrote, on the documented VMS async pattern and on the boot path.
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

/* --- Common event flag clusters --- */

/*
 * sys$ascefc - Associate a named common event flag cluster with this process.
 *
 * The executive keeps common clusters on a node-wide list keyed by name, so
 * two processes that associate with the same name share the same 32 flags.
 * If no cluster of that name exists it is created.
 *
 * efn names the cluster to attach it to: any flag number 64-95 selects
 * common cluster 2, any number 96-127 selects cluster 3 (HELP
 * SYSTEM_SERVICES $ASCEFC Arguments, quoted in
 * docs/oracle/vax73-event-flags.md).
 *
 * prot and perm are passed through to the executive unchanged.
 *
 * Returns SS$_ILLEFC if efn is not a common flag number, and otherwise
 * whatever the executive reports for the association.
 */
uint32_t sys$ascefc(uint32_t efn, const struct dsc$descriptor_s *name,
                    uint32_t prot, uint32_t perm) {
    char buf[32];

    clusnam(name, buf, sizeof(buf));

    return vms_kif_ascefc(efn, buf, prot, perm);
}

/*
 * sys$dacefc - Disassociate this process from a common event flag cluster.
 *
 * A temporary cluster is deleted when its last associated process
 * disassociates; a permanent one survives until $DLCEFC.
 *
 * Returns SS$_UNASEFC if this process is not associated with that cluster,
 * SS$_ILLEFC if efn is not a common flag number.
 */
uint32_t sys$dacefc(uint32_t efn) {
    return vms_kif_dacefc(efn);
}

/*
 * sys$dlcefc - Mark a permanent common event flag cluster for deletion.
 *
 * "Marks a permanent common event flag cluster for deletion" (HELP
 * SYSTEM_SERVICES $DLCEFC on the reference lab V7.3). Processes associated
 * with it keep working; the cluster goes away when the last one
 * disassociates. Named, not numbered -- the caller need never have
 * associated with it.
 *
 * Returns SS$_UNASEFC if the executive has no cluster of that name.
 */
uint32_t sys$dlcefc(const struct dsc$descriptor_s *name) {
    char buf[32];

    clusnam(name, buf, sizeof(buf));

    return vms_kif_dlcefc(buf);
}
