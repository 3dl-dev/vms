/*
 * ssh_ident.c - Fail-honest establishment of the SSH session identity in the
 * executive. See ssh_ident.h for the full rationale (vms-6ae).
 *
 * This translation unit is dependency-free (it calls the executive only
 * through the injected function pointer), so the unit test links it directly
 * and injects a refusing / accepting setident -- proving the fail-honest
 * policy without a live /dev/vms. The production setident table
 * (ovmx_ident_real_syscalls) lives in vmssshd.c, where libvmssys is linked.
 */

#include <stddef.h>

#include "ssh_ident.h"

int ovmx_ssh_establish_identity(const char *username, uint32_t uic,
                                uint64_t authorized_privs,
                                const struct ovmx_ident_syscalls *sc,
                                uint32_t *out_status)
{
    /*
     * A missing syscall table is itself a refusal: we cannot establish the
     * identity, so we must not pretend we did. Fail closed.
     */
    if (sc == NULL || sc->fn_setident == NULL) {
        if (out_status != NULL)
            *out_status = 0;   /* even -> refused */
        return -1;
    }

    uint32_t ist = sc->fn_setident(username, uic, authorized_privs);
    if (out_status != NULL)
        *out_status = ist;

    /*
     * VMS status convention: odd = success (accepted), even = error (refused).
     * The /dev/vms-absent case surfaces here too -- the ioctl fails and the
     * status is even -- so an absent executive DENIES the session rather than
     * falling back to a local privileged identity (CLAUDE.md Rule 9 / INV-6).
     */
    return (ist & 1u) ? 0 : -1;
}
