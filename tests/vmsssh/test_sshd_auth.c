/*
 * test_sshd_auth.c - the wrapped-OpenSSH-sshd SYSUAF login decision, unit-tested
 * against BINARY $UAFDEF records (rd vms-0cd, RUNG-3 step 3c).
 *
 * Exercises the REAL product function ovmx_sshd_check_login()
 * (src/vmsssh/sshd_auth.c) -- the policy sys_auth_passwd() runs after reading a
 * SYSUAF record -- linked directly, over in-memory binary records built the same
 * way the seed/runtime build them (sysuaf_view_to_raw + sysuaf_set_password, the
 * real Purdy hash). No ASCII, no SHA-256, no /etc/shadow. The property is
 * structural, mirroring tests/libvms/test_sysuaf_auth.c and tools/vms_login.c:
 *
 *   1. a Purdy-passworded account ACCEPTS only its correct password;
 *   2. the wrong password and the empty password are REFUSED;
 *   3. a NO-Purdy account (no credential on file) is REFUSED for every password;
 *   4. a DISUSER account is REFUSED even WITH the correct password (login-flag
 *      enforcement is separate from the password check);
 *   5. a DISACNT account is likewise refused with the correct password;
 *   6. a NULL record / NULL password fail CLOSED.
 *
 * And the fail-closed contract of the sys_auth_passwd body itself: on a host
 * with no binary SYSUAF (sysuaf_lookup -> -1, the engine/ACP absent),
 * ovmx_sshd_sysuaf_auth() and ovmx_sshd_fill_passwd() REFUSE -- never a
 * fabricated accept (INV-6). The full sysuaf_lookup-over-ACP + SSH handshake is
 * proven end-to-end by tests/qemu/test_syssvc_ssh_server.c against a real
 * /dev/vms; this test pins the decision that rides on top of it.
 */

#include <stdio.h>
#include <string.h>
#include <pwd.h>

#include "sysuaf.h"
#include "sshd_auth.h"

static int g_fail = 0;
static void check(int cond, const char *msg)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond)
        g_fail = 1;
}

/* Build one binary SYSUAF view record: username + UIC + optional Purdy password
 * + optional flag names (e.g. "DISUSER"). */
static void build(sysuaf_record_t *rec, const char *user,
                  const char *pw, const char *flags)
{
    memset(rec, 0, sizeof(*rec));
    strncpy(rec->username, user, sizeof(rec->username) - 1);
    rec->uic_group = 200;
    rec->uic_member = 17;
    snprintf(rec->default_dir, sizeof(rec->default_dir),
             "SYS$SYSDEVICE:[USERS.%s]", user);
    strncpy(rec->privileges, "TMPMBX,NETMBX", sizeof(rec->privileges) - 1);
    if (flags)
        strncpy(rec->flags, flags, sizeof(rec->flags) - 1);
    sysuaf_view_to_raw(rec);
    if (pw)
        sysuaf_set_password(rec, pw);
}

int main(void)
{
    sysuaf_record_t rec;

    printf("=== test_sshd_auth: SYSUAF login decision over binary records "
           "(vms-0cd 3c) ===\n");

    /* ---- 1/2: a passworded, enabled account accepts only its password ---- */
    build(&rec, "OVMXUSER", "Sekrit-Purdy-Pw-1", NULL);
    check(ovmx_sshd_check_login((struct sysuaf_record *)&rec,
                                "Sekrit-Purdy-Pw-1") == 1,
          "enabled account accepts its correct Purdy password");
    check(ovmx_sshd_check_login((struct sysuaf_record *)&rec,
                                "wrong-password") == 0,
          "enabled account refuses a wrong password");
    check(ovmx_sshd_check_login((struct sysuaf_record *)&rec, "") == 0,
          "enabled account refuses the empty password");

    /* ---- 3: a no-Purdy account refuses every password ---- */
    build(&rec, "NOPWUSER", NULL, NULL);
    check(ovmx_sshd_check_login((struct sysuaf_record *)&rec, "") == 0,
          "no-Purdy account refuses the empty password");
    check(ovmx_sshd_check_login((struct sysuaf_record *)&rec,
                                "anything") == 0,
          "no-Purdy account refuses an arbitrary password");

    /* ---- 4: DISUSER refuses even WITH the correct password ---- */
    build(&rec, "DISUSED", "Sekrit-Purdy-Pw-1", "DISUSER");
    check(sysuaf_authenticate(&rec, "Sekrit-Purdy-Pw-1") == 1,
          "sanity: the DISUSER account's password DOES Purdy-verify");
    check(ovmx_sshd_check_login((struct sysuaf_record *)&rec,
                                "Sekrit-Purdy-Pw-1") == 0,
          "DISUSER account is refused DESPITE the correct password "
          "(login-flag enforcement)");

    /* ---- 5: DISACNT refuses even WITH the correct password ---- */
    build(&rec, "DISACCT", "Sekrit-Purdy-Pw-1", "DISACNT");
    check(ovmx_sshd_check_login((struct sysuaf_record *)&rec,
                                "Sekrit-Purdy-Pw-1") == 0,
          "DISACNT account is refused DESPITE the correct password");

    /* ---- 6: NULL record / NULL password fail closed ---- */
    check(ovmx_sshd_check_login(NULL, "x") == 0, "NULL record fails closed");
    build(&rec, "OVMXUSER", "Sekrit-Purdy-Pw-1", NULL);
    check(ovmx_sshd_check_login((struct sysuaf_record *)&rec, NULL) == 0,
          "NULL password fails closed");

    /* ---- fail-closed sys_auth_passwd body: no binary SYSUAF on this host ---
     * sysuaf_lookup returns -1 (the engine/ACP is not present under host
     * ctest), so the password-auth body and the getpwnam body must REFUSE /
     * decline -- never a fabricated accept or a /etc/passwd stand-in. */
    check(ovmx_sshd_sysuaf_auth("OVMXUSER", "Sekrit-Purdy-Pw-1") == 0,
          "sys_auth_passwd body fails closed when the binary SYSUAF is absent");
    {
        struct passwd pw;
        struct ovmx_sshd_pwbufs b;
        check(ovmx_sshd_fill_passwd("OVMXUSER", &pw, &b) == -1,
              "getpwnam body declines (falls through) when SYSUAF is absent");
    }

    /* ---- the DCL exec seam: basename detection + the image path ---- */
    check(ovmx_sshd_is_dcl_path("/vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE") == 1,
          "is_dcl_path recognises the DCL image by basename");
    check(ovmx_sshd_is_dcl_path("/bin/sh") == 0,
          "is_dcl_path rejects a non-DCL shell");
    check(ovmx_sshd_is_dcl_path(ovmx_sshd_dcl_image_path()) == 1,
          "the configured DCL image path is itself recognised");

    printf("=== test_sshd_auth: %d failure(s) ===\n", g_fail);
    return g_fail ? 1 : 0;
}
