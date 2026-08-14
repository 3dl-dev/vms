/*
 * cred_drop.c - SSH session credential drop to the authenticated UIC.
 *
 * See cred_drop.h for the vms-49e rationale. This translation unit is
 * dependency-free (no libssh) on purpose: it is compiled straight into both
 * the vmssshd daemon AND the tests/vmsssh unit test (the same pattern
 * term_map.c uses), so the test exercises the REAL product logic rather than
 * a copy that could drift (CLAUDE.md Rule 10).
 */

#include "cred_drop.h"

#include <unistd.h>
#include <grp.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* Production syscall table: the real credential calls.                */
/* ------------------------------------------------------------------ */

const struct ovmx_cred_syscalls ovmx_cred_real_syscalls = {
    .fn_setgroups = setgroups,
    .fn_setgid    = setgid,
    .fn_setuid    = setuid,
    .fn_getuid    = getuid,
    .fn_geteuid   = geteuid,
    .fn_getgid    = getgid,
    .fn_getegid   = getegid,
};

/* ------------------------------------------------------------------ */

int ovmx_cred_drop_to_uic(uint32_t uic_group, uint32_t uic_member,
                          const struct ovmx_cred_syscalls *sc)
{
    const uid_t want_uid = (uid_t)uic_member;
    const gid_t want_gid = (gid_t)uic_group;

    /*
     * ORDER IS LOAD-BEARING and short-circuits, exactly as the inline drop in
     * tools/vms_login.c:
     *   1. drop supplementary groups first (a leftover group is privilege);
     *   2. setgid BEFORE setuid -- once the uid is no longer 0 the kernel
     *      refuses setgid, so a uid-first order would silently keep the gid;
     *   3. only then setuid.
     * If any step fails we return immediately (errno intact), never attempting
     * a later step and never proceeding -- a partial drop is a failed drop.
     */
    if (sc->fn_setgroups(0, NULL) != 0)
        return -1;
    if (sc->fn_setgid(want_gid) != 0)
        return -1;
    if (sc->fn_setuid(want_uid) != 0)
        return -1;

    /*
     * VERIFY the drop actually took, for BOTH real and effective ids. A
     * setuid/setgid that returned 0 but left a saved-set id, or that a
     * seteuid-style call would have left recoverable, is caught here: the
     * real and effective ids must equal the target and nothing else.
     */
    if (sc->fn_getuid()  != want_uid || sc->fn_geteuid() != want_uid ||
        sc->fn_getgid()  != want_gid || sc->fn_getegid() != want_gid) {
        if (errno == 0)
            errno = EPERM;
        return -1;
    }

    return 0;
}
