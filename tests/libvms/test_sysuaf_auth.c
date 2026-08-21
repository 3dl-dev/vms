/*
 * test_sysuaf_auth.c - an account with no Purdy password refuses EVERY
 * password; a Purdy-passworded account accepts only the right one (vms-08f,
 * flipped to the binary $UAFDEF path by vms-d92)
 *
 * WHY THIS TEST EXISTS AND WHY IT IS NOT ACCOUNT-SHAPED
 *
 * sysuaf_authenticate() used to treat an empty/unset password as "no password
 * required" -- an auth bypass. vms-72c fixed it, but only for the two accounts
 * its own UAT logged into; the others shipped with the identical bypass,
 * undetected because no test drove them. That is the UAT-shaped-scope hazard
 * this test closes, so it does not name a single account.
 *
 * THE FLIP (vms-d92). SYSUAF is now the binary $UAFDEF record and the password
 * is the real VMS Purdy hash, so the property is STRUCTURAL: sysuaf_authenticate
 * keys off UAF$B_ENCRYPT. A record whose UAF$B_ENCRYPT is not UAI$C_PURDY_S (an
 * account with no Purdy credential) authenticates NOTHING; a record with a
 * Purdy password accepts only the matching plaintext. This test builds binary
 * records in memory -- no ASCII, no SHA-256 -- and asserts both sides of the
 * property over a table of accounts, then proves the check is not a fixed list
 * by adding a synthetic "extra" no-password account and re-running the loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sysuaf.h"

static int g_failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        g_failures++;
    }
}

/* One account under test: a username, and either a Purdy password (pw != NULL)
 * or no credential at all (pw == NULL). */
struct acct {
    const char *user;
    const char *pw;     /* NULL => no Purdy password on file (cannot log in) */
};

/* Build the binary view record for one account (uic/dir are cosmetic here). */
static void build(sysuaf_record_t *rec, const struct acct *a)
{
    memset(rec, 0, sizeof(*rec));
    strncpy(rec->username, a->user, sizeof(rec->username) - 1);
    rec->uic_group = 200; rec->uic_member = 200;
    snprintf(rec->default_dir, sizeof(rec->default_dir),
             "SYS$SYSDEVICE:[USERS.%s]", a->user);
    strncpy(rec->privileges, "TMPMBX,NETMBX", sizeof(rec->privileges) - 1);
    sysuaf_view_to_raw(rec);
    if (a->pw)
        sysuaf_set_password(rec, a->pw);
    /* else: uaf$b_encrypt stays 0 (!= UAI$C_PURDY_S) -> refuses every password */
}

/* Run the two-sided property over one account. Returns 1 if it was a
 * no-password account (for counting), 0 otherwise. */
static int check_acct(const struct acct *a)
{
    sysuaf_record_t rec;
    char label[192];
    build(&rec, a);

    if (!a->pw) {
        snprintf(label, sizeof(label),
                 "%s (no Purdy password) refuses the empty password", a->user);
        check(sysuaf_authenticate(&rec, "") == 0, label);

        snprintf(label, sizeof(label),
                 "%s (no Purdy password) refuses an arbitrary password",
                 a->user);
        check(sysuaf_authenticate(&rec, "SomeRandomPassword123") == 0, label);

        snprintf(label, sizeof(label),
                 "%s (no Purdy password) refuses its own username as password",
                 a->user);
        check(sysuaf_authenticate(&rec, a->user) == 0, label);
        return 1;
    }

    snprintf(label, sizeof(label),
             "%s authenticates its correct Purdy password", a->user);
    check(sysuaf_authenticate(&rec, a->pw) == 1, label);

    snprintf(label, sizeof(label),
             "%s refuses an unrelated wrong password", a->user);
    check(sysuaf_authenticate(&rec, "definitely_not_the_real_password_xyz") == 0,
          label);

    snprintf(label, sizeof(label),
             "%s refuses the empty password", a->user);
    check(sysuaf_authenticate(&rec, "") == 0, label);
    return 0;
}

int main(void)
{
    printf("test_sysuaf_auth: no-Purdy accounts refuse every password; "
           "passworded accounts accept only the right one (vms-08f/vms-d92)\n");

    /* A representative table: some accounts have a Purdy password, some have
     * none (the shipped SYSTEM=MANAGER / GUEST=GUEST plaintexts, plus the
     * shipped passwordless OPERATOR/DEFAULT/USER1/USER2 pattern). */
    static const struct acct accts[] = {
        { "SYSTEM",   "MANAGER" },
        { "GUEST",    "GUEST"   },
        { "OPERATOR", NULL      },
        { "DEFAULT",  NULL      },
        { "USER1",    NULL      },
        { "USER2",    NULL      },
    };
    int n = (int)(sizeof(accts) / sizeof(accts[0]));

    int empty1 = 0;
    for (int i = 0; i < n; i++)
        empty1 += check_acct(&accts[i]);
    printf("pass 1: %d account(s), %d with no Purdy password\n", n, empty1);
    check(empty1 > 0, "pass 1 exercised at least one no-password account");

    /* Prove the check is not a fixed list: a synthetic "seventh" no-password
     * account -- a username not in the table above -- must be caught too. */
    struct acct seventh = { "ZZTEST_SEVENTH_ACCOUNT", NULL };
    int empty2 = check_acct(&seventh);
    check(empty2 == 1,
          "the synthetic seventh no-password account is caught (the property "
          "is structural, not a hand-enumerated list)");

    printf("test_sysuaf_auth: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
