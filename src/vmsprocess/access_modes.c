/*
 * access_modes.c - VMS Access Mode and Privilege API
 *
 * VMS defines four hierarchical access modes:
 *   0  Kernel     - Most privileged; runs the kernel itself
 *   1  Executive  - File system, RMS, etc.
 *   2  Supervisor - CLI, command language interpreter
 *   3  User       - Normal user programs
 *
 * Lower numbers are more privileged.
 *
 * THE EXECUTIVE OWNS THIS STATE. Every function here is a thin call into
 * vms.ko through the vms_exec_* gateway (src/libvmssys/vms_exec.c); the PCB
 * fields are only a cache, refreshed from the executive after each call.
 *
 * This file previously implemented the whole model in userspace: mode
 * transitions were checked against pcb->cur_privs, and priv_set() wrote
 * pcb->cur_privs directly. That made it a SECOND ENTRY POINT to the same
 * privilege state that $SETPRV manages -- so a privilege denied through
 * $SETPRV could be taken here instead. Both doors now lead to the same
 * kernel-enforced check.
 *
 * NO SILENT FALLBACK (CLAUDE.md rule 9): with no /dev/vms there is no
 * executive, so priv_check() answers "not held", priv_get_all() answers
 * "none", and access_mode_set() refuses. Absence denies; it never grants.
 */

#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#include "ssdef.h"
#include "vms/pcb.h"
#include "prvdef.h"
#include "vms_exec.h"
#include "vms_kif.h"    /* pulls src/kernel/vms_ioctl.h: the executive's bits */

/*
 * ANTI-DRIFT GATE. The executive (vms.ko) and userspace must agree on which
 * bit means which privilege, or an enforcement check silently guards the
 * wrong thing. They did not agree: the kernel had SETPRV at bit 5 -- the
 * DETACH bit -- so "may this process set any privilege?" was answering a
 * different question entirely, and the unprivileged default set was LOG_IO
 * and GROUP rather than TMPMBX and NETMBX.
 *
 * This is the only translation unit that sees both headers, so it is where
 * the two are pinned together. Editing either side alone fails the build.
 * Both sides are oracle-pinned; see the PROVENANCE note in
 * src/kernel/vms_ioctl.h for the SDA/SYSDEF.STB observation.
 */
_Static_assert(PRV_M_CMKRNL == PRV$M_CMKRNL,
               "kernel/userspace disagree on the CMKRNL privilege bit");
_Static_assert(PRV_M_CMEXEC == PRV$M_CMEXEC,
               "kernel/userspace disagree on the CMEXEC privilege bit");
_Static_assert(PRV_M_SETPRV == PRV$M_SETPRV,
               "kernel/userspace disagree on the SETPRV privilege bit");
_Static_assert(PRV_M_TMPMBX == PRV$M_TMPMBX,
               "kernel/userspace disagree on the TMPMBX privilege bit");
_Static_assert(PRV_M_NETMBX == PRV$M_NETMBX,
               "kernel/userspace disagree on the NETMBX privilege bit");

/*
 * Same gate for the status values. vms_exec.h has to spell them as literals
 * (libvmssys is freestanding and cannot include ssdef.h), and independent
 * copies of a status value is precisely how SS$_NOSUCHDEV came to be 2680
 * in three places when the system itself reports 2312 (2680 is RMTPATH).
 */
_Static_assert(VMS_EXEC_SS_NORMAL     == SS$_NORMAL,
               "vms_exec.h / ssdef.h disagree on SS$_NORMAL");
_Static_assert(VMS_EXEC_SS_NOPRIV     == SS$_NOPRIV,
               "vms_exec.h / ssdef.h disagree on SS$_NOPRIV");
_Static_assert(VMS_EXEC_SS_BADPARAM   == SS$_BADPARAM,
               "vms_exec.h / ssdef.h disagree on SS$_BADPARAM");
_Static_assert(VMS_EXEC_SS_NOTALLPRIV == SS$_NOTALLPRIV,
               "vms_exec.h / ssdef.h disagree on SS$_NOTALLPRIV");
_Static_assert(VMS_EXEC_SS_NOSUCHDEV  == SS$_NOSUCHDEV,
               "vms_exec.h / ssdef.h disagree on SS$_NOSUCHDEV");

/* ------------------------------------------------------------------ */
/* Access mode constants                                              */
/* ------------------------------------------------------------------ */
#define PSL$C_KERNEL     0
#define PSL$C_EXEC       1
#define PSL$C_SUPER      2
#define PSL$C_USER       3

/*
 * Refresh the PCB's cached copy of the executive's answer, so existing
 * readers of pcb->cur_privs / pcb->current_mode never drift from it.
 */
static void cache_from_executive(void)
{
    struct vms_pcb *pcb = vms_pcb_get();
    uint64_t cur = 0, perm = 0;
    uint8_t mode = PSL$C_USER;

    (void)vms_exec_getprv(&cur, &perm, &mode);

    if (!pcb)
        return;

    pthread_mutex_lock(&pcb->priv_lock);
    pcb->cur_privs = cur;
    pcb->perm_privs = perm;
    pthread_mutex_unlock(&pcb->priv_lock);
    pcb->current_mode = mode;
}

/* ------------------------------------------------------------------ */
/* Access mode API                                                    */
/* ------------------------------------------------------------------ */

uint8_t access_mode_get(void)
{
    uint8_t mode = PSL$C_USER;

    /* vms_exec_getprv reports USER (least privileged) when there is no
     * executive, so an unreachable executive can never look like kernel
     * mode to a caller that is deciding whether to permit something. */
    (void)vms_exec_getprv(NULL, NULL, &mode);
    return mode;
}

uint32_t access_mode_set(uint8_t mode)
{
    uint32_t status;

    if (mode > PSL$C_USER)
        return SS$_BADPARAM;

    /* The executive decides: a transition to a more privileged mode needs
     * CMKRNL/CMEXEC, and it is the kernel's copy of those privileges that
     * is consulted, not one this process wrote. */
    status = vms_exec_setmode(mode);
    cache_from_executive();
    return status;
}

int access_mode_check(uint8_t required)
{
    return access_mode_get() <= required;
}

/* ------------------------------------------------------------------ */
/* Privilege API                                                      */
/* ------------------------------------------------------------------ */

int priv_check(uint64_t priv)
{
    /* SS$_NORMAL only when the executive confirms every requested bit is
     * held. SS$_NOPRIV and SS$_NOSUCHDEV both mean "no". */
    return vms_exec_chkpriv(priv) == SS$_NORMAL ? 1 : 0;
}

uint64_t priv_set(uint64_t mask, int enable)
{
    uint64_t prev = 0;

    (void)vms_exec_setprv(mask, enable ? 1 : 0, 0, &prev);
    cache_from_executive();
    return prev;
}

uint64_t priv_get_all(void)
{
    uint64_t cur = 0;

    (void)vms_exec_getprv(&cur, NULL, NULL);
    return cur;
}

void priv_init(uint64_t initial_privs)
{
    /*
     * `initial_privs` is a REQUEST. The executive clamps it to what this
     * process's credentials authorize and the PCB caches the result -- this
     * is no longer "write the caller's mask into the PCB and call it done".
     */
    (void)vms_exec_attach((uint32_t)getpid(), initial_privs, NULL);
    cache_from_executive();
}
