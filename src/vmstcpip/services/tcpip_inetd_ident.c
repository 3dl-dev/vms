/*
 * tcpip_inetd_ident.c - see tcpip_inetd_ident.h. Establishes a TCPIP$INETD
 * service's configured run-as identity in the forked child before execv, the
 * same identity-drop LOGINOUT/sshd use (rd vms-8bd, R4 G1). Fail-closed.
 *
 * Compiled straight into the TCPIP$INETD image AND the ctest, so the test drives
 * the REAL logic (Rule 10). The apply half is injectable so its fail-closed
 * policy is unit-tested with mocks (no live /dev/vms), exactly as sshd's
 * cred_drop.c / ssh_ident.c are.
 */

#include "tcpip_inetd_ident.h"

#include <stddef.h>

#include "sysuaf.h"     /* sysuaf_lookup, sysuaf_record_privileges, sysuaf_record_t */
#include "vms_kif.h"    /* vms_kif_setident -- the real executive setident ioctl    */

/* INETD's own production identity table (the real executive setident). Defined
 * locally rather than reusing sshd's ovmx_ident_real_syscalls (which lives in the
 * SSH images, not linked here) -- same underlying vms_kif_setident, no cross-image
 * symbol dependency. */
static const struct ovmx_ident_syscalls tcpip_inetd_ident_real = {
    .fn_setident = vms_kif_setident,
};

int tcpip_inetd_apply_identity(const char *username, uint32_t uic,
                               uint32_t uic_group, uint32_t uic_member,
                               uint64_t privs,
                               const struct ovmx_ident_syscalls *ident_sc,
                               const struct ovmx_cred_syscalls *cred_sc)
{
    if (ident_sc == NULL) ident_sc = &tcpip_inetd_ident_real;
    if (cred_sc  == NULL) cred_sc  = &ovmx_cred_real_syscalls;

    /* (1) Stamp the EXECUTIVE identity (survives execv). Fail-closed on refusal
     *     (even status), including the /dev/vms-absent case. */
    uint32_t ist = 0;
    if (ovmx_ssh_establish_identity(username, uic, privs, ident_sc, &ist) != 0)
        return -1;

    /* (2) Drop Linux credentials to the account's UIC (clear groups, setgid
     *     BEFORE setuid, verify -- ordering + fail-closed enforced by
     *     ovmx_cred_drop_to_uic). A partial drop is a failed drop. */
    if (ovmx_cred_drop_to_uic(uic_group, uic_member, cred_sc) != 0)
        return -1;

    return 0;
}

int tcpip_inetd_establish_service_identity(const char *user,
                               const struct ovmx_ident_syscalls *ident_sc,
                               const struct ovmx_cred_syscalls *cred_sc)
{
    /* No configured account -> fail-closed. A service with no identity must NOT
     * launch (and must NEVER inherit INETD's SYSTEM/all-privs -- that is G1). */
    if (user == NULL || user[0] == '\0')
        return -1;

    /* Resolve the account in SYSUAF. Unknown/unreadable -> fail-closed. */
    sysuaf_record_t rec;
    if (sysuaf_lookup(user, &rec) != 0)
        return -1;

    uint32_t uic   = ((uint32_t)rec.uic_group << 16) | (rec.uic_member & 0xFFFFu);
    uint64_t privs = sysuaf_record_privileges(&rec);

    return tcpip_inetd_apply_identity(user, uic, rec.uic_group, rec.uic_member,
                                      privs, ident_sc, cred_sc);
}
