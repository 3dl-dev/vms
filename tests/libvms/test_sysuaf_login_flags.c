/*
 * test_sysuaf_login_flags.c - SYSUAF login-flag enforcement (vms-c8fa)
 *
 * THE BUG THIS GUARDS. Before vms-c8fa the console login path
 * (tools/vms_login.c / LOGINOUT) checked ONLY the SHA-256 password hash
 * (sysuaf_authenticate) and read no flag field at all: a DISUSER (disabled)
 * account with a correct password logged straight in to a "$" prompt. The
 * flags existed and already parsed (sysuaf_flags_to_mask, uaidef.h) but were
 * IGNORED at login. This is a SECURITY defect -- authenticating the password
 * proves the credential, not that the account may log in.
 *
 * WHAT IS TESTED HERE. The security DECISION now lives in two pure predicates
 * in the SYSUAF library, exercised directly (they take a parsed
 * sysuaf_record_t, so no file/path/executive dependency):
 *
 *   sysuaf_interactive_login_permitted() -- 0 iff a DISABLING flag
 *      (UAI$M_DISUSER or UAI$M_DISACNT) is set. This is the gate that must
 *      FAIL CLOSED: a correct password on a disabled account is still
 *      refused, and a NULL record is refused.
 *   sysuaf_account_captive() -- 1 iff UAI$M_CAPTIVE is set (the session is
 *      permitted but confined to its login command procedure, no "$").
 *
 * PLUS SOURCE GUARDS (argv[1]=tools/vms_login.c, argv[2]=src/vmsdcl/dcl_main.c
 * when passed by CTest): the predicates being correct is worthless if the
 * login path does not CALL them, so this test also asserts the wiring is
 * present -- LOGINOUT refuses on the permit predicate BEFORE starting a
 * session and passes --captive; DCL disables Ctrl/Y and suppresses the REPL
 * for a captive login. Same source-guard technique as
 * tests/tools/test_loginout_display.c.
 *
 * Grounding for the per-flag behavior (OpenVMS Guide to System Security:
 * "Disusering Accounts", "Captive Accounts") is cited at each predicate in
 * src/libvms/rtl/sysuaf.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sysuaf.h"

static int failures = 0;

#define CHECK(cond, msg) do {                                            \
        if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }        \
        else         { printf("ok:   %s\n", (msg)); }                    \
    } while (0)

/* Build a record carrying only a FLAGS field (the only field the predicates
 * read). username is set non-empty so the record is otherwise well-formed. */
static sysuaf_record_t rec_with_flags(const char *flags)
{
    sysuaf_record_t rec;
    memset(&rec, 0, sizeof(rec));
    strncpy(rec.username, "TESTUSR", sizeof(rec.username) - 1);
    if (flags)
        strncpy(rec.flags, flags, sizeof(rec.flags) - 1);
    return rec;
}

/* Little-endian writers for the raw $UAFDEF expiry fields (vms-c6df). The
 * expiry predicate reads these from the binary record, NOT from the flag-NAME
 * string, so the test must set the raw bytes -- the same representation the
 * on-disk record and AUTHORIZE/mksysuaf produce. */
static void le32w(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void le64w(uint8_t *p, uint64_t v)
{
    le32w(p, (uint32_t)v); le32w(p + 4, (uint32_t)(v >> 32));
}

/* Slurp a source file for the source-guard checks. Returns malloc'd buffer
 * (caller frees) or NULL. */
static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, fp);
    buf[got] = '\0';
    fclose(fp);
    return buf;
}

int main(int argc, char *argv[])
{
    /* ---- Predicate behavior (the security decision) ---- */

    /* Unflagged account -- the demo/SYSTEM shape the boot-smoke logs in.
     * MUST be permitted and MUST NOT be captive: the enforcement is additive
     * and cannot touch a normal login. */
    {
        sysuaf_record_t r = rec_with_flags("");
        CHECK(sysuaf_interactive_login_permitted(&r) == 1,
              "empty flags: login permitted (normal account unaffected)");
        CHECK(sysuaf_account_captive(&r) == 0,
              "empty flags: not captive");
    }

    /* A realistic normal privilege-flag string is still a permitted,
     * non-captive login. */
    {
        sysuaf_record_t r = rec_with_flags("TMPMBX");
        CHECK(sysuaf_interactive_login_permitted(&r) == 1,
              "normal flags (TMPMBX): login permitted");
        CHECK(sysuaf_account_captive(&r) == 0,
              "normal flags (TMPMBX): not captive");
    }

    /* DISUSER -- the reported bug. A correct password is NOT enough; the
     * disabled account must be refused. */
    {
        sysuaf_record_t r = rec_with_flags("DISUSER");
        CHECK(sysuaf_interactive_login_permitted(&r) == 0,
              "DISUSER: login REFUSED (the vms-c8fa bug)");
    }

    /* DISACNT -- account disabled, refused identically. */
    {
        sysuaf_record_t r = rec_with_flags("DISACNT");
        CHECK(sysuaf_interactive_login_permitted(&r) == 0,
              "DISACNT: login REFUSED");
    }

    /* CAPTIVE -- permitted but confined. */
    {
        sysuaf_record_t r = rec_with_flags("CAPTIVE");
        CHECK(sysuaf_interactive_login_permitted(&r) == 1,
              "CAPTIVE: login permitted (constrained, not denied)");
        CHECK(sysuaf_account_captive(&r) == 1,
              "CAPTIVE: account is captive");
    }

    /* CAPTIVE + DISUSER -- a disabling flag wins: refused, and still marked
     * captive (order/independence of the two predicates). */
    {
        sysuaf_record_t r = rec_with_flags("CAPTIVE,DISUSER");
        CHECK(sysuaf_interactive_login_permitted(&r) == 0,
              "CAPTIVE,DISUSER: login REFUSED (disabling flag wins)");
        CHECK(sysuaf_account_captive(&r) == 1,
              "CAPTIVE,DISUSER: still captive");
    }

    /* LOCKPWD is NOT a login denial (it constrains SET PASSWORD, not entry). */
    {
        sysuaf_record_t r = rec_with_flags("LOCKPWD");
        CHECK(sysuaf_interactive_login_permitted(&r) == 1,
              "LOCKPWD: login permitted (not a login-denial flag)");
        CHECK(sysuaf_account_captive(&r) == 0,
              "LOCKPWD: not captive");
    }

    /* Fail closed on a NULL record. */
    CHECK(sysuaf_interactive_login_permitted(NULL) == 0,
          "NULL record: login refused (fail closed)");
    CHECK(sysuaf_account_captive(NULL) == 0,
          "NULL record: not captive (fail closed)");

    /* ---- Password-expiration predicate (vms-c6df) ---- */

    /* A record with neither the flag nor a lifetime is NOT expired -- a normal
     * account is unaffected (no false positive). */
    {
        sysuaf_record_t r;
        memset(&r, 0, sizeof(r));
        strncpy(r.username, "TESTUSR", sizeof(r.username) - 1);
        CHECK(sysuaf_password_expired(&r) == 0,
              "no PWD_EXPIRED flag, zero lifetime: NOT expired (normal account)");
    }

    /* Admin-forced expiry: UAI$M_PWD_EXPIRED set in the raw flags longword.
     * Read via le32 from raw, NOT via the flag-name string (PWD_EXPIRED is not
     * in the names table -- a names round-trip would drop it). */
    {
        sysuaf_record_t r;
        memset(&r, 0, sizeof(r));
        strncpy(r.username, "TESTUSR", sizeof(r.username) - 1);
        le32w(r.raw.uaf$l_flags, UAI$M_PWD_EXPIRED);
        CHECK(sysuaf_password_expired(&r) == 1,
              "UAI$M_PWD_EXPIRED (admin-forced): password EXPIRED (the vms-c6df hole)");
    }

    /* Lifetime elapsed: non-zero lifetime with change-date at the VMS epoch, so
     * now > PWD_DATE + PWD_LIFETIME. */
    {
        sysuaf_record_t r;
        memset(&r, 0, sizeof(r));
        strncpy(r.username, "TESTUSR", sizeof(r.username) - 1);
        le64w(r.raw.uaf$q_pwd_date, 0);
        le64w(r.raw.uaf$q_pwd_lifetime, 1);
        CHECK(sysuaf_password_expired(&r) == 1,
              "lifetime elapsed (PWD_DATE+LIFETIME in the past): password EXPIRED");
    }

    /* A future expiry instant is NOT expired: a large lifetime from a recent
     * change-date puts PWD_DATE+LIFETIME well beyond now. */
    {
        sysuaf_record_t r;
        memset(&r, 0, sizeof(r));
        strncpy(r.username, "TESTUSR", sizeof(r.username) - 1);
        /* pwd_date ~ now (a big VMS-tick value) + a ~3-year lifetime; the sum is
         * far in the future, so not expired regardless of the wall clock. */
        le64w(r.raw.uaf$q_pwd_date, 0x00E0000000000000ULL);
        le64w(r.raw.uaf$q_pwd_lifetime, 0x0010000000000000ULL);
        CHECK(sysuaf_password_expired(&r) == 0,
              "lifetime NOT yet elapsed (expiry instant in the future): NOT expired");
    }

    /* Fail closed on a NULL record: an unreadable record is treated as expired,
     * never granted a normal login. */
    CHECK(sysuaf_password_expired(NULL) == 1,
          "NULL record: password treated as EXPIRED (fail closed)");

    /* ---- Source guards: the login path actually CALLS the predicates ---- */

    if (argc > 1) {
        char *login = slurp(argv[1]);
        CHECK(login != NULL, "read tools/vms_login.c for source guard");
        if (login) {
            CHECK(strstr(login, "sysuaf_interactive_login_permitted") != NULL,
                  "LOGINOUT gates login on sysuaf_interactive_login_permitted");
            CHECK(strstr(login, "sysuaf_account_captive") != NULL,
                  "LOGINOUT consults sysuaf_account_captive");
            CHECK(strstr(login, "--captive") != NULL,
                  "LOGINOUT passes --captive to DCL for a captive account");
            CHECK(strstr(login, "sysuaf_password_expired") != NULL,
                  "LOGINOUT gates login on sysuaf_password_expired (vms-c6df)");
            free(login);
        }
    }

    if (argc > 2) {
        char *dcl = slurp(argv[2]);
        CHECK(dcl != NULL, "read src/vmsdcl/dcl_main.c for source guard");
        if (dcl) {
            CHECK(strstr(dcl, "--captive") != NULL,
                  "DCL handles --captive");
            CHECK(strstr(dcl, "ctrl_y_enabled = 0") != NULL,
                  "DCL disables Ctrl/Y for a captive login");
            CHECK(strstr(dcl, "captive_mode")
                  && strstr(dcl, "logout_requested = 1") != NULL,
                  "DCL suppresses the REPL (logs out) for a captive login");
            free(dcl);
        }
    }

    if (failures == 0)
        printf("\nAll SYSUAF login-flag enforcement checks passed.\n");
    else
        printf("\n%d check(s) FAILED.\n", failures);
    return failures ? 1 : 0;
}
