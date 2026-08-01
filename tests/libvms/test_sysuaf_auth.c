/*
 * test_sysuaf_auth.c - EVERY SYSUAF row with an unset hash refuses EVERY
 * password (vms-08f)
 *
 * WHY THIS TEST EXISTS AND WHY IT IS NOT ACCOUNT-SHAPED
 *
 * sysuaf_authenticate() used to treat an empty/unset password hash as "no
 * password required" -- an auth bypass. vms-72c measured and fixed it, but
 * only for the two accounts (SYSTEM, GUEST) its own UAT happened to log
 * into; OPERATOR/DEFAULT/USER1/USER2 shipped with the identical empty hash
 * and the identical bypass, undetected because no test drove them. That is
 * the UAT-shaped-scope hazard vms-08f exists to close, and a test written
 * by hand-naming those four accounts would reproduce the exact same error
 * for whichever account is added ninth.
 *
 * So this test does not name a single account. It PARSES the real shipped
 * SYSUAF.DAT (path given as argv[1], normally the file installed at
 * distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT -- the exact file
 * that ships in the boot image) and asserts the property over every row it
 * finds: an unset hash refuses the empty password, a made-up password, and
 * the username itself used as a password. A row with a real hash is
 * sanity-checked to refuse an unrelated wrong password too (it does not
 * assert the correct password succeeds, because this test does not know
 * the plaintext for an arbitrary row -- that is covered end-to-end for the
 * two known plaintexts, SYSTEM/GUEST, by tests/uat/vms_session_qemu.sh and
 * for OPERATOR's wrong-password path by tests/uat/test_sysuaf_operator*).
 *
 * GENERICITY IS PROVEN, NOT ASSERTED: after checking the real file, this
 * test builds a TEMP COPY with one synthetic extra row appended -- a
 * seventh-account scenario, an unset hash under a username that does not
 * exist in the shipped file -- and re-runs the identical parse+check loop
 * against the copy. If the loop's pass/fail counts did not move when a new
 * empty-hash row was added, the loop is not actually reading the file, and
 * this test would rather fail loudly than let that pass silently. The temp
 * copy is removed before exit either way.
 *
 * sysuaf_authenticate() takes a sysuaf_record_t*, not a file path, so this
 * test never touches SYS$SYSTEM: path resolution (/vms mount, the LNM
 * manager, vmsfs) at all -- it parses the pipe-delimited rows itself
 * (trivial field-splitting only; the AUTHENTICATION RULE under test lives
 * solely in sysuaf_authenticate(), never duplicated here) and hands each
 * row to the real function.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

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

/*
 * Minimal field split of one SYSUAF.DAT data line: USERNAME|HASH|... .
 * Only the first two fields matter here. Returns 1 on a parsed row, 0 for
 * a line that should be skipped (comment/blank/malformed).
 */
static int parse_row(char *line, char **username, char **hash)
{
    /* Strip trailing newline/CR */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';

    if (line[0] == '\0' || line[0] == '#')
        return 0;

    char *p = line;
    char *sep = strchr(p, '|');
    if (!sep)
        return 0;
    *sep = '\0';
    *username = p;
    p = sep + 1;

    sep = strchr(p, '|');
    if (sep)
        *sep = '\0';
    *hash = p;

    return (*username)[0] != '\0';
}

/*
 * Run the derived-from-file property check against one SYSUAF.DAT-shaped
 * file. Returns the number of data rows found (0 = parse failure --
 * caller must treat that as a harness error, not a vacuous pass), and
 * accumulates counts into *out_empty / *out_hashed.
 */
static int check_file(const char *path, int *out_empty, int *out_hashed)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "FATAL: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    int rows = 0;
    *out_empty = 0;
    *out_hashed = 0;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *username, *hash;
        if (!parse_row(line, &username, &hash))
            continue;
        rows++;

        sysuaf_record_t rec;
        memset(&rec, 0, sizeof(rec));
        strncpy(rec.password_hash, hash, sizeof(rec.password_hash) - 1);

        char label[160];

        if (hash[0] == '\0') {
            (*out_empty)++;

            snprintf(label, sizeof(label),
                     "%s (empty hash) refuses empty password", username);
            check(sysuaf_authenticate(&rec, "") == 0, label);

            snprintf(label, sizeof(label),
                     "%s (empty hash) refuses an arbitrary password", username);
            check(sysuaf_authenticate(&rec, "SomeRandomPassword123") == 0, label);

            snprintf(label, sizeof(label),
                     "%s (empty hash) refuses its own username as password",
                     username);
            check(sysuaf_authenticate(&rec, username) == 0, label);
        } else {
            (*out_hashed)++;

            snprintf(label, sizeof(label),
                     "%s (hash on file) refuses an unrelated wrong password",
                     username);
            check(sysuaf_authenticate(&rec, "definitely_not_the_real_password_xyz") == 0,
                  label);
        }
    }

    fclose(fp);
    return rows;
}

int main(int argc, char *argv[])
{
    printf("test_sysuaf_auth: every SYSUAF row with an unset hash refuses "
           "every password (vms-08f)\n");

    if (argc < 2) {
        fprintf(stderr,
                "FATAL: usage: %s <path-to-SYSUAF.DAT>\n"
                "       (normally distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT)\n",
                argv[0]);
        return 2;
    }

    /* --- Pass 1: the real shipped file, unmodified --------------------- */
    int empty1 = 0, hashed1 = 0;
    int rows1 = check_file(argv[1], &empty1, &hashed1);
    if (rows1 <= 0) {
        fprintf(stderr,
                "FATAL: parsed zero data rows from %s -- that is a harness "
                "failure, not an empty pass. Refusing to report success.\n",
                argv[1]);
        return 2;
    }
    printf("pass 1 (real file %s): %d row(s), %d empty-hash, %d hashed\n",
           argv[1], rows1, empty1, hashed1);

    if (empty1 == 0) {
        fprintf(stderr,
                "FATAL: the shipped SYSUAF.DAT has ZERO empty-hash rows -- "
                "the empty-hash branch of this test never ran, so it proves "
                "nothing about the property it exists to check. If every "
                "account has been given a real password, this test's data "
                "assumption is stale and must be revisited, not silenced.\n");
        return 2;
    }

    /* --- Pass 2: the same file plus one synthetic empty-hash row ------- *
     * Proves the check is DERIVED FROM THE FILE, not enumerated by hand:
     * a seventh account with an empty hash must be caught without this
     * test's source being touched. */
    char tmp_path[] = "/tmp/sysuaf_auth_negctl_XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        fprintf(stderr, "FATAL: mkstemp failed: %s\n", strerror(errno));
        return 2;
    }
    close(tmp_fd);

    {
        FILE *src = fopen(argv[1], "r");
        FILE *dst = fopen(tmp_path, "w");
        if (!src || !dst) {
            fprintf(stderr, "FATAL: could not build the synthetic copy\n");
            if (src) fclose(src);
            if (dst) fclose(dst);
            remove(tmp_path);
            return 2;
        }
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
            fwrite(buf, 1, n, dst);
        fclose(src);
        /* The synthetic seventh account: a username that does not appear
         * in the real file, empty hash, otherwise well-formed. */
        fprintf(dst, "ZZTEST_SEVENTH_ACCOUNT||999|999|SYS$SYSDEVICE:[USERS.ZZ]||\n");
        fclose(dst);
    }

    int empty2 = 0, hashed2 = 0;
    int rows2 = check_file(tmp_path, &empty2, &hashed2);
    remove(tmp_path);

    if (rows2 <= 0) {
        fprintf(stderr, "FATAL: parsed zero rows from the synthetic copy\n");
        return 2;
    }
    printf("pass 2 (real file + 1 synthetic empty-hash row): %d row(s), "
           "%d empty-hash, %d hashed\n",
           rows2, empty2, hashed2);

    check(rows2 == rows1 + 1,
          "the synthetic copy has exactly one more row than the real file");
    check(empty2 == empty1 + 1,
          "the synthetic seventh account is counted as an empty-hash row "
          "(proves the check is derived from the file, not a fixed list)");
    check(hashed2 == hashed1,
          "the synthetic row did not change the hashed-row count");

    printf("test_sysuaf_auth: %d failure(s) across %d + %d checked rows\n",
           g_failures, rows1, rows2);
    return g_failures ? 1 : 0;
}
