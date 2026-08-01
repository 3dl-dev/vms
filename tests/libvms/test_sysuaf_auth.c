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
 * two known plaintexts, SYSTEM/GUEST, and for OPERATOR's wrong-password
 * path, by tests/uat/vms_session_qemu.sh).
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
 * THIS TEST STAYS GREEN WHEN THE HARDENING IT ADVERTISES ACTUALLY LANDS.
 * The property under test is two-sided -- an unset hash authenticates
 * nothing, a set hash authenticates only the matching password -- and
 * both sides are checked without assuming which rows in the real file are
 * currently empty. A third pass builds a copy where every empty-hash row
 * is given a hash THIS TEST generated (so it knows the plaintext), and
 * checks the correct-password-succeeds half of the property there. If
 * OPERATOR/DEFAULT/USER1/USER2 are ever given real passwords, pass 1
 * simply finds fewer empty-hash rows (reported, not required to be
 * nonzero) and pass 3 has fewer rows to generate -- neither pass fails.
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
#include <stdint.h>

#include "sysuaf.h"
#include "sha256.h"

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

/*
 * Split a mutable line (no trailing newline) into up to max_fields
 * pipe-delimited fields. SYSUAF.DAT rows have 7 fields
 * (USERNAME|HASH|UIC_GROUP|UIC_MEMBER|DEFAULT_DIR|FLAGS|PRIVILEGES);
 * max_fields is set well above that so a malformed row is passed through
 * whole rather than truncated.
 */
#define MAX_FIELDS 12
static int split_fields(char *line, char *fields[], int max_fields)
{
    int n = 0;
    char *p = line;
    fields[n++] = p;
    while (n < max_fields) {
        char *sep = strchr(p, '|');
        if (!sep)
            break;
        *sep = '\0';
        p = sep + 1;
        fields[n++] = p;
    }
    return n;
}

/*
 * Build a copy of src_path at dst_path where every data row with an EMPTY
 * password hash gets a hash THIS TEST generated (SHA256 of
 * "<USERNAME>_TESTPW") instead. Rows that already carry a hash are copied
 * unchanged; comment/blank lines pass through verbatim.
 *
 * This exists to prove the OTHER half of the authentication property --
 * "a set hash authenticates only the matching password" -- against a
 * state where the hardening this item's disposition calls for has
 * actually happened, without needing to know the real plaintext for any
 * production account. Every (username, generated plaintext) pair for a
 * row this function hashed is recorded into synth_user[]/synth_pw[] so
 * the caller can assert the positive case afterward.
 */
static void build_all_hashed_copy(const char *src_path, const char *dst_path,
                                   char synth_user[][64], char synth_pw[][96],
                                   int *n_synth, int max_synth)
{
    FILE *src = fopen(src_path, "r");
    FILE *dst = fopen(dst_path, "w");
    if (!src || !dst) {
        fprintf(stderr, "FATAL: could not open files for the all-hashed "
                         "synthetic copy (%s -> %s): %s\n",
                src_path, dst_path, strerror(errno));
        if (src) fclose(src);
        if (dst) fclose(dst);
        exit(2);
    }

    *n_synth = 0;
    char line[1024];
    while (fgets(line, sizeof(line), src)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (line[0] == '\0' || line[0] == '#') {
            fprintf(dst, "%s\n", line);
            continue;
        }

        char *fields[MAX_FIELDS];
        int nf = split_fields(line, fields, MAX_FIELDS);
        if (nf < 2 || fields[0][0] == '\0') {
            /* Malformed row: not this function's job to fix, pass through. */
            fprintf(dst, "%s\n", line);
            continue;
        }

        char newhash[65];
        const char *hash_to_write = fields[1];
        if (fields[1][0] == '\0') {
            char plaintext[96];
            snprintf(plaintext, sizeof(plaintext), "%s_TESTPW", fields[0]);
            sha256_hex((const uint8_t *)plaintext, strlen(plaintext), newhash);
            hash_to_write = newhash;

            if (*n_synth < max_synth) {
                snprintf(synth_user[*n_synth], 64, "%s", fields[0]);
                snprintf(synth_pw[*n_synth], 96, "%s", plaintext);
                (*n_synth)++;
            }
        }

        for (int i = 0; i < nf; i++) {
            fputs(i == 1 ? hash_to_write : fields[i], dst);
            if (i < nf - 1)
                fputc('|', dst);
        }
        fputc('\n', dst);
    }

    fclose(src);
    fclose(dst);
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

    /* NOT a FATAL when empty1 == 0. A test that hard-fails the moment
     * every shipped account gets a real password is a test that will be
     * deleted by the next person who does that hardening, rather than
     * obeyed by it -- the exact opposite of what this item is for. The
     * empty-hash branch of check_file() simply runs zero times in that
     * case; pass 3 below independently proves the "set hash authenticates
     * the right password" half of the property using hashes THIS TEST
     * generates, so the property is still exercised end to end even when
     * the real file has nothing left to test it with. */
    if (empty1 == 0) {
        printf("pass 1 note: the shipped SYSUAF.DAT has zero empty-hash "
               "rows -- every account already carries a real password. "
               "That is the intended end state, not a test failure; pass 3 "
               "below still exercises the set-hash side of the property.\n");
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

    /* --- Pass 3: a copy where every empty-hash row has been hardened --- *
     * Proves "a set hash authenticates only the right password" against a
     * state where the hardening this item's disposition calls for has
     * actually happened -- with hashes THIS TEST generates (so it knows
     * the plaintext), independent of whatever the real file currently
     * ships. This is what keeps the whole test green whether the shipped
     * file has 0 or 4 empty-hash rows: the property is proven either way,
     * not inferred from a count that could drift. */
    char allhashed_path[] = "/tmp/sysuaf_auth_allhashed_XXXXXX";
    int allhashed_fd = mkstemp(allhashed_path);
    if (allhashed_fd < 0) {
        fprintf(stderr, "FATAL: mkstemp failed: %s\n", strerror(errno));
        return 2;
    }
    close(allhashed_fd);

    enum { MAX_SYNTH_ROWS = 64 };
    char synth_user[MAX_SYNTH_ROWS][64];
    char synth_pw[MAX_SYNTH_ROWS][96];
    int n_synth = 0;
    build_all_hashed_copy(argv[1], allhashed_path, synth_user, synth_pw,
                           &n_synth, MAX_SYNTH_ROWS);

    printf("pass 3 (real file with every empty-hash row hardened): %d "
           "row(s) given a generated hash\n", n_synth);

    int empty3 = 0, hashed3 = 0;
    int rows3 = check_file(allhashed_path, &empty3, &hashed3);
    if (rows3 <= 0) {
        fprintf(stderr, "FATAL: parsed zero rows from the all-hashed copy\n");
        remove(allhashed_path);
        return 2;
    }
    printf("pass 3 re-check: %d row(s), %d empty-hash, %d hashed\n",
           rows3, empty3, hashed3);
    check(empty3 == 0,
          "the all-hashed copy has zero empty-hash rows left "
          "(the generator actually hashed every row that was empty)");
    check(rows3 == rows1,
          "the all-hashed copy has the same row count as the real file "
          "(hardening did not add or drop a row)");

    /* check_file() above (via its "hashed" branch) already proved every
     * one of these rows refuses an unrelated wrong password. This proves
     * the other half: the CORRECT password succeeds, for the exact rows
     * this test hardened and knows the plaintext for. */
    if (n_synth == 0) {
        printf("pass 3 note: nothing needed generating -- the shipped file "
               "already had zero empty-hash rows.\n");
    } else {
        FILE *fp = fopen(allhashed_path, "r");
        if (!fp) {
            fprintf(stderr, "FATAL: cannot reopen %s: %s\n",
                    allhashed_path, strerror(errno));
            remove(allhashed_path);
            return 2;
        }
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            char *username, *hash;
            if (!parse_row(line, &username, &hash))
                continue;
            for (int i = 0; i < n_synth; i++) {
                if (strcmp(username, synth_user[i]) != 0)
                    continue;

                sysuaf_record_t rec;
                memset(&rec, 0, sizeof(rec));
                strncpy(rec.password_hash, hash, sizeof(rec.password_hash) - 1);

                char label[192];
                snprintf(label, sizeof(label),
                         "%s (now hashed) authenticates its own generated "
                         "correct password", username);
                check(sysuaf_authenticate(&rec, synth_pw[i]) == 1, label);

                snprintf(label, sizeof(label),
                         "%s (now hashed) still refuses a wrong password "
                         "after hardening", username);
                check(sysuaf_authenticate(&rec, "definitely_not_it") == 0,
                      label);
                break;
            }
        }
        fclose(fp);
    }
    remove(allhashed_path);

    printf("test_sysuaf_auth: %d failure(s) across %d + %d + %d checked "
           "rows\n",
           g_failures, rows1, rows2, rows3);
    return g_failures ? 1 : 0;
}
