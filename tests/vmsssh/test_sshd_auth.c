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

/* Little-endian writers for the binary record fields the expiry gate reads.
 * These set the RAW $UAFDEF bytes directly -- the same on-disk representation
 * mksysuaf/AUTHORIZE/the ACP writer produce -- so the test drives the real
 * predicate against a genuine record, never a mock of the flag. */
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}
/* Admin-forced expiry: set UAI$M_PWD_EXPIRED in the raw flags longword. Read
 * back through le32 by sysuaf_password_expired -- NOT via the flag-name string
 * (PWD_EXPIRED is not in the names table). */
static void mark_pwd_expired_flag(sysuaf_record_t *rec)
{
    uint32_t f = (uint32_t)rec->raw.uaf$l_flags[0] |
                 ((uint32_t)rec->raw.uaf$l_flags[1] << 8) |
                 ((uint32_t)rec->raw.uaf$l_flags[2] << 16) |
                 ((uint32_t)rec->raw.uaf$l_flags[3] << 24);
    put_le32(rec->raw.uaf$l_flags, f | UAI$M_PWD_EXPIRED);
}
/* Lifetime-elapsed expiry: a non-zero lifetime with the change-date at the VMS
 * epoch (0), so now > pwd_date + lifetime. */
static void mark_pwd_lifetime_elapsed(sysuaf_record_t *rec)
{
    put_le64(rec->raw.uaf$q_pwd_date, 0);          /* changed at the VMS epoch  */
    put_le64(rec->raw.uaf$q_pwd_lifetime, 1);      /* 1-tick lifetime -> elapsed */
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

    /* ---- 5b: PWD_EXPIRED (admin-forced) refuses even WITH the correct
     *          password (vms-c6df). The enabled account authenticates and is
     *          login-permitted, but the expired credential is a THIRD gate. */
    build(&rec, "STALEPWD", "Sekrit-Purdy-Pw-1", NULL);
    mark_pwd_expired_flag(&rec);
    check(sysuaf_authenticate(&rec, "Sekrit-Purdy-Pw-1") == 1,
          "sanity: the PWD_EXPIRED account's password DOES Purdy-verify");
    check(sysuaf_interactive_login_permitted(&rec) == 1,
          "sanity: the PWD_EXPIRED account is NOT DISUSER/DISACNT (login-permitted)");
    check(sysuaf_password_expired(&rec) == 1,
          "sanity: sysuaf_password_expired() reads the admin-forced PWD_EXPIRED bit from raw");
    check(ovmx_sshd_check_login((struct sysuaf_record *)&rec,
                                "Sekrit-Purdy-Pw-1") == 0,
          "PWD_EXPIRED account is refused DESPITE the correct password (expiry gate)");

    /* ---- 5c: lifetime-elapsed expiry refuses likewise (PWD_DATE+LIFETIME past). */
    build(&rec, "OLDPWD", "Sekrit-Purdy-Pw-1", NULL);
    mark_pwd_lifetime_elapsed(&rec);
    check(sysuaf_password_expired(&rec) == 1,
          "sanity: lifetime-elapsed record reads as expired (PWD_DATE+LIFETIME in the past)");
    check(ovmx_sshd_check_login((struct sysuaf_record *)&rec,
                                "Sekrit-Purdy-Pw-1") == 0,
          "lifetime-elapsed account is refused DESPITE the correct password");

    /* ---- 5d: a normal unexpired account is NOT falsely refused (no false
     *          positive: lifetime 0 => never expires by age, flag clear). */
    build(&rec, "FRESHPWD", "Sekrit-Purdy-Pw-1", NULL);
    check(sysuaf_password_expired(&rec) == 0,
          "unexpired account: not expired (no false positive)");
    check(ovmx_sshd_check_login((struct sysuaf_record *)&rec,
                                "Sekrit-Purdy-Pw-1") == 1,
          "unexpired account with the correct password logs in normally");

    /* ---- 6: NULL record / NULL password fail closed ---- */
    check(ovmx_sshd_check_login(NULL, "x") == 0, "NULL record fails closed");
    check(sysuaf_password_expired(NULL) == 1,
          "sysuaf_password_expired(NULL) fails closed (treated as expired)");
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
