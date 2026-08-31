/*
 * sshd_session.c - LOGINOUT pre-drop setup for the wrapped OpenSSH sshd session
 * seam (rd vms-0cd, RUNG-3 step 3c). See sshd_session.h.
 *
 * This is the SSH mirror of the pre-execl block in tools/vms_login.c (LOGINOUT)
 * and the child of the old libssh vmssshd.c -- the SAME calls, same order,
 * transplanted behind the OpenSSH permanently_set_uid seam so OpenSSH stays
 * unmodified. The credential drop ITSELF is OpenSSH's own permanently_set_uid
 * (from the SYSUAF UIC that sshd_auth.c put in the struct passwd), so cred_drop.c
 * is not linked here -- OpenSSH performs the ordered, permanent, fail-closed
 * drop natively.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <pwd.h>

#include "sshd_session.h"
#include "sysuaf.h"
#include "ssh_ident.h"
#include "ovmx_accounting.h"
#include "ovmx_banner.h"
#include "vms_kif.h"

/*
 * Production identity syscall table: the real vms_kif_setident behind the
 * injectable seam (ssh_ident.h). Defined here (NOT in vmssshd.c, which the
 * OpenSSH sshd does not link) so ovmx_ssh_establish_identity resolves in the
 * wrapped sshd; the unit test (tests/vmsssh/test_ssh_ident.c) injects its own
 * table and never references this symbol.
 */
const struct ovmx_ident_syscalls ovmx_ident_real_syscalls = {
    .fn_setident = vms_kif_setident,
};

void ovmx_sshd_pre_drop_pw(const struct passwd *pw)
{
    static const char *const months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    sysuaf_record_t rec;
    uint32_t uic, ist = 0;
    uint64_t privs;
    time_t last_login = 0;

    if (pw == NULL || pw->pw_name == NULL)
        return;

    /* Re-read the binary SYSUAF for the privilege mask + exact UIC. If this is
     * not a SYSUAF account (the privsep 'sshd' user also routes through the
     * permanently_set_uid wrap), do nothing -- let the real drop proceed. */
    memset(&rec, 0, sizeof(rec));
    if (sysuaf_lookup(pw->pw_name, &rec) != 0)
        return;

    uic   = (rec.uic_group << 16) | rec.uic_member;
    privs = sysuaf_record_privileges(&rec);   /* the $UAFDEF quadword mask */

    /*
     * ESTABLISH THE AUTHENTICATED IDENTITY IN THE EXECUTIVE, FAIL CLOSED
     * (vms-6ae / INV-6, audit-vms-040 §3.7). Must precede the drop: setident
     * needs the SETPRV the root registration granted. On refusal -- including
     * /dev/vms absent (an even status) -- DENY the session: print
     * %OVMX-F-NOIDENT and _exit(1), constructing no identity the executive
     * refused. Mirrors tools/vms_login.c exactly. The diagnostic goes to the
     * session's stdout (the PTY slave / channel), so the client sees it.
     */
    if (ovmx_ssh_establish_identity(rec.username, uic, privs,
                                    &ovmx_ident_real_syscalls, &ist) != 0) {
        printf("%%OVMX-F-NOIDENT, the executive refused the authenticated "
               "identity (status %u)\n", (unsigned)ist);
        fflush(stdout);
        _exit(1);
    }

    /* SYS$WELCOME banner + last-interactive-login line (real, from the same
     * accounting store the console login uses). */
    printf("\n");
    ovmx_banner_welcome(stdout);
    if (ovmx_accounting_get_lastlogin(rec.username, &last_login) == 0
            && last_login > 0) {
        struct tm *tm = localtime(&last_login);
        if (tm)
            printf("\n   Last interactive login on %02d-%s-%04d "
                   "%02d:%02d:%02d\n\n",
                   tm->tm_mday, months[tm->tm_mon], tm->tm_year + 1900,
                   tm->tm_hour, tm->tm_min, tm->tm_sec);
        else
            printf("\n   Last login time could not be determined.\n\n");
    } else {
        printf("\n   No previous interactive login recorded.\n\n");
    }

    /* Record this login (after showing the last, before the drop+DCL exec). */
    ovmx_accounting_record_login(rec.username);
    fflush(stdout);
}
