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
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "sysuaf.h"     /* sysuaf_lookup, sysuaf_record_privileges, sysuaf_record_t */
#include "vms_kif.h"    /* vms_kif_setident -- the real executive setident ioctl    */

/* INETD's own production identity table (the real executive setident). Defined
 * locally rather than reusing sshd's ovmx_ident_real_syscalls (which lives in the
 * SSH images, not linked here) -- same underlying vms_kif_setident, no cross-image
 * symbol dependency. */
static const struct ovmx_ident_syscalls tcpip_inetd_ident_real = {
    .fn_setident = vms_kif_setident,
};

/*
 * Fail-closed diagnostic (rd vms-8bd). On a launch refusal the service image is
 * never execv'd, so its %OVMX-F- line has nowhere to go and the connecting client
 * just sees an empty read -- indistinguishable from a dozen other launch failures.
 *
 * TWO SINKS, DELIBERATELY ASYMMETRIC (R4 posture, #1168):
 *  - stderr (fd 2 -> SYS$MANAGER:TCPIP$INETD.LOG when the aux server is detached):
 *    the FULL operator detail -- account, UIC, raw RMS status, errno. This is an
 *    operator log on the protected system disk; the detail is exactly what an
 *    admin needs to fix a mis-seeded service account.
 *  - the accepted connection socket (fd 1, in the spawned child): a GENERIC
 *    "service unavailable" only. Writing the SYSUAF/cred internals (which account,
 *    which RMS status, which errno) to an UNAUTHENTICATED :13 client would be the
 *    mild information disclosure this project's own R4 TCP/IP sweep flags, so the
 *    client is told the service is unavailable and nothing about why.
 *
 * write() goes straight to the fd (no stdio buffering to lose before _exit). fd 1
 * is only the socket in the spawned child; in the unit test it is the test's
 * stdout, where the generic line is harmless. */
static void inetd_ident_diag(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
    (void)!write(STDERR_FILENO, buf, len);        /* full detail -> INETD.LOG   */

    static const char generic[] =
        "%OVMX-F-NOSVC, service unavailable\n";    /* generic -> the client      */
    (void)!write(STDOUT_FILENO, generic, sizeof(generic) - 1);
}

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
    if (ovmx_ssh_establish_identity(username, uic, privs, ident_sc, &ist) != 0) {
        inetd_ident_diag("%%OVMX-F-NOIDENT, executive refused run-as identity "
                         "'%s' [%u,%u] (status %#x)\n",
                         username, uic >> 16, uic & 0xFFFFu, (unsigned)ist);
        return -1;
    }

    /* (2) Drop Linux credentials to the account's UIC (clear groups, setgid
     *     BEFORE setuid, verify -- ordering + fail-closed enforced by
     *     ovmx_cred_drop_to_uic). A partial drop is a failed drop. */
    if (ovmx_cred_drop_to_uic(uic_group, uic_member, cred_sc) != 0) {
        inetd_ident_diag("%%OVMX-F-NOUIC, credential drop to [%u,%u] failed "
                         "for '%s': %s\n",
                         uic_group, uic_member, username, strerror(errno));
        return -1;
    }

    return 0;
}

int tcpip_inetd_establish_service_identity(const char *user,
                               const struct ovmx_ident_syscalls *ident_sc,
                               const struct ovmx_cred_syscalls *cred_sc)
{
    /* No configured account -> fail-closed. A service with no identity must NOT
     * launch (and must NEVER inherit INETD's SYSTEM/all-privs -- that is G1). */
    if (user == NULL || user[0] == '\0') {
        inetd_ident_diag("%%OVMX-F-NOACCT, service has no run-as account "
                         "configured -- not launched (fail-closed, never SYSTEM)\n");
        return -1;
    }

    /* Resolve the account in SYSUAF. Unknown/unreadable -> fail-closed. The RMS
     * status distinguishes "account not in SYSUAF" from "SYSUAF unreadable over
     * the ACP" -- the two runtime-resolve failures we most need to tell apart. */
    sysuaf_record_t rec;
    uint32_t rms_st = 0;
    int lst = sysuaf_lookup_st(user, &rec, &rms_st);
    if (lst != 0) {
        inetd_ident_diag("%%OVMX-F-NOUSER, SYSUAF lookup of run-as account "
                         "'%s' failed (rc %d, RMS %#x) -- not launched\n",
                         user, lst, (unsigned)rms_st);
        return -1;
    }

    uint32_t uic   = ((uint32_t)rec.uic_group << 16) | (rec.uic_member & 0xFFFFu);
    uint64_t privs = sysuaf_record_privileges(&rec);

    return tcpip_inetd_apply_identity(user, uic, rec.uic_group, rec.uic_member,
                                      privs, ident_sc, cred_sc);
}
