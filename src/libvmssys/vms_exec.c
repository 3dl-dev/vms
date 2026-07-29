/*
 * vms_exec.c - Executive privilege / access-mode gateway (userspace side)
 *
 * The single userspace door to the executive's privilege state. Everything
 * that used to keep privileges in a per-process variable ($SETPRV's PCB
 * fields, the access_modes.c privilege API) now routes through here, so the
 * authoritative answer to "what privileges does this process hold?" comes
 * from vms.ko via /dev/vms and not from a value the process wrote itself.
 *
 * NO SILENT FALLBACK (CLAUDE.md rule 9): when /dev/vms is absent there is no
 * executive and therefore no authority, so every entry point fails with
 * SS$_NOSUCHDEV and reports ZERO privileges. Absence denies; it never grants.
 * This mirrors src/libvms/syssvc/sys_lock.c, which returns SS$_NOSUCHDEV for
 * $ENQ/$DEQ off the kernel target.
 *
 * ---------------------------------------------------------------------------
 * PROVENANCE of the status values used here (clean-room, CLAUDE.md rule 8):
 * observed on the reference lab (~/vax/cluster, OpenVMS VAX V7.3, node VAX1)
 * by scanning F$MESSAGE across the SYSTEM-facility code range from a DCL
 * command procedure:
 *     F$MESSAGE(36)   -> %SYSTEM-F-NOPRIV,      insufficient privilege or
 *                                               object protection violation
 *     F$MESSAGE(1664) -> %SYSTEM-W-NOTALLPRIV,  not all requested privileges
 *                                               authorized
 *     F$MESSAGE(2312) -> %SYSTEM-W-NOSUCHDEV,   no such device available
 * They are spelled as literals rather than #include "ssdef.h" because
 * libvmssys is freestanding and must not pull in the libvms headers; the
 * values are kept identical to ssdef.h (see the matching comments there).
 *
 * The kernel module's internal SS__ numbering happens to agree with the
 * public ssdef.h values for every status this interface can return
 * (NORMAL=1, BADPARAM=20, NOPRIV=36, NOTALLPRIV=1664), so unlike the lock
 * manager (sys_lock.c kstat_to_ss) no translation table is needed. A status
 * the kernel returns that is NOT one of those is passed through unchanged
 * rather than being mapped onto a plausible-looking constant.
 * ---------------------------------------------------------------------------
 */

#include "vms_exec.h"
#include "vms_kif.h"

/* Kernel status for "this pid is already registered" (SS$_DUPNAM). */
#define KSTAT_DUPNAM    0x0000001C

/*
 * Per-thread attach state. The kernel keys its per-process record on the
 * calling task, so each thread performs its own REGISTER.
 */
static __thread int thread_attached;

/*
 * Process-wide granted ceiling, established by the FIRST successful attach.
 * Later threads attach with this ceiling instead of a fresh request, so a
 * thread cannot ask the executive for more than the process was granted.
 * Plain statics are sufficient: the first attach happens during single-
 * threaded process initialization (image activation / login), before any
 * thread that could race it exists.
 */
static int      process_ceiling_valid;
static uint64_t process_ceiling;

int vms_exec_attached(void)
{
    return thread_attached;
}

uint32_t vms_exec_attach(uint32_t vms_pid, uint64_t request, uint64_t *granted)
{
    uint32_t status;
    uint64_t cur = 0, perm = 0;

    if (granted)
        *granted = 0;

    if (vms_kif_open() < 0)
        return VMS_EXEC_SS_NOSUCHDEV;   /* no executive -> no privileges */

    if (!thread_attached) {
        /*
         * A secondary thread may only ask for what the process already
         * holds; it may not re-open the privilege question on its own.
         */
        uint64_t ask = process_ceiling_valid ? process_ceiling : request;

        status = vms_kif_register(vms_pid, ask);
        if (status != VMS_EXEC_SS_NORMAL && status != KSTAT_DUPNAM)
            return status;

        thread_attached = 1;
    }

    /*
     * Read back what the executive ACTUALLY granted. The request above is
     * advisory: vms.ko clamps it to what the caller's credentials authorize.
     * Callers must believe this value, not the one they asked for.
     */
    status = vms_exec_getprv(&cur, &perm, (uint8_t *)0);
    if (status != VMS_EXEC_SS_NORMAL)
        return status;

    if (!process_ceiling_valid) {
        process_ceiling = perm;
        process_ceiling_valid = 1;
    }

    if (granted)
        *granted = cur;

    return VMS_EXEC_SS_NORMAL;
}

uint32_t vms_exec_setprv(uint64_t mask, int enable, int permanent,
                         uint64_t *prev)
{
    if (prev)
        *prev = 0;

    if (!thread_attached || vms_kif_open() < 0)
        return VMS_EXEC_SS_NOSUCHDEV;

    return vms_kif_setprv(mask, enable, permanent, prev);
}

uint32_t vms_exec_chkpriv(uint64_t mask)
{
    if (!thread_attached || vms_kif_open() < 0)
        return VMS_EXEC_SS_NOSUCHDEV;   /* unreachable executive denies */

    return vms_kif_chkpriv(mask);
}

uint32_t vms_exec_getprv(uint64_t *cur, uint64_t *perm, uint8_t *mode)
{
    uint8_t m = 0;
    uint64_t c = 0, p = 0;
    uint32_t status;

    if (cur)  *cur = 0;
    if (perm) *perm = 0;
    if (mode) *mode = PSL_C_USER;   /* least privileged, never kernel */

    if (!thread_attached || vms_kif_open() < 0)
        return VMS_EXEC_SS_NOSUCHDEV;

    status = vms_kif_getmode(&m, &c, &p);
    if (status != VMS_EXEC_SS_NORMAL)
        return status;

    if (cur)  *cur = c;
    if (perm) *perm = p;
    if (mode) *mode = m;

    return VMS_EXEC_SS_NORMAL;
}

uint32_t vms_exec_setmode(uint8_t mode)
{
    if (!thread_attached || vms_kif_open() < 0)
        return VMS_EXEC_SS_NOSUCHDEV;

    return vms_kif_setmode(mode);
}
