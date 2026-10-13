/*
 * tcpip_inetd_ident.c - see tcpip_inetd_ident.h. Resolves a TCPIP$INETD
 * service's configured run-as identity in the auxiliary server, which then
 * creates the service process DETACHED with it (rd vms-8bd R4 G1; vms-1f8).
 * Fail-closed.
 *
 * Compiled straight into the TCPIP$INETD image AND the QEMU suite, so the test
 * drives the REAL logic (Rule 10).
 */

#include "tcpip_inetd_ident.h"

#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "sysuaf.h"     /* sysuaf_lookup_st, sysuaf_record_privileges, sysuaf_record_t */

/*
 * Fail-closed diagnostic (rd vms-8bd). The full operator detail -- account, raw
 * RMS status -- goes to stderr (fd 2 -> SYS$MANAGER:TCPIP$INETD.LOG when the
 * auxiliary server is detached), an operator log on the protected system disk.
 * The connecting client is told only that the service is unavailable, by
 * tcpip_inetd_spawn, on the connection itself (R4 posture, #1168): SYSUAF
 * internals are not for an unauthenticated client. write() goes straight to the
 * fd (no stdio buffering).
 */
static void inetd_ident_diag(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
    (void)!write(STDERR_FILENO, buf, len);
}

int tcpip_inetd_resolve_account(const char *user, struct tcpip_inetd_identity *id)
{
    if (id == NULL)
        return -1;
    memset(id, 0, sizeof(*id));

    /* No configured account -> fail-closed. A service with no identity must NOT
     * launch (and must NEVER inherit INETD's SYSTEM/all-privs -- that is G1). */
    if (user == NULL || user[0] == '\0') {
        inetd_ident_diag("%%OVMX-F-NOACCT, service has no run-as account "
                         "configured -- not launched (fail-closed, never SYSTEM)\n");
        return -1;
    }
    if (strlen(user) >= sizeof(id->username)) {
        inetd_ident_diag("%%OVMX-F-NOUSER, run-as account name '%s' is too long "
                         "-- not launched\n", user);
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

    memcpy(id->username, user, strlen(user) + 1);
    id->uic   = ((uint32_t)rec.uic_group << 16) | (rec.uic_member & 0xFFFFu);
    id->privs = sysuaf_record_privileges(&rec);
    return 0;
}

int tcpip_inetd_resolve_service_identity(const struct tcpip_service *svc,
                                         struct tcpip_inetd_identity *id)
{
    return tcpip_inetd_resolve_account(svc ? svc->user : NULL, id);
}
