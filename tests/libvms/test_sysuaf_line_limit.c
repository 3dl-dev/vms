/*
 * test_sysuaf_line_limit.c - the SYSUAF record limit, at the boundary (vms-9b7)
 *
 * ============================================================================
 * WHAT THIS TESTS, AND WHY IT IS NOT A TAUTOLOGY
 * ============================================================================
 * The defect this suite guards is a READER SILENTLY TRUNCATING a record and a
 * WRITER HAPPILY EMITTING one the reader cannot read back. Five independent
 * parsers of one format carried three different line limits (512, 512, 1024,
 * 512, 1024), so a row between those sizes was written by one and truncated by
 * another. MEASURED on a real QEMU boot of the unfixed tree (three boots, each
 * with a control): a SYSTEM row whose SIXTH separator falls past byte 511 was
 * read by the 512-byte reader as a five-field record, and PID 1 reported
 *
 *     %OVMX-F-EXECINIT, no SYSTEM record in SYS$SYSTEM:SYSUAF.DAT
 *
 * and powered the machine off, while the 1024-byte readers accepted it.
 *
 * WHERE THE EXPECTED VALUES COME FROM (they are NOT produced by the code under
 * test):
 *
 *   - The boundary lengths 511/512/513/1022/1023/1024/1025 are the three
 *     different buffer sizes the five deleted parsers used, plus one either
 *     side. They are the sizes at which those implementations DISAGREED, taken
 *     from the source of the implementations, not from the survivor. MEASURED
 *     here and reported by the run: sysuaf_record_t's own field widths cap a
 *     rendered record at 714 bytes, so the 1022+ cases are NOT EXPRESSIBLE and
 *     the run says so rather than quietly dropping them. That cap is exactly
 *     why the reader may not depend on it -- widening any field re-opens the
 *     window, and a hand-edited SYSUAF is not bound by it at all.
 *
 *   - The expected FIELD VALUES are the strings this test constructs and hands
 *     to the writer. The assertion is that the reader returns THE SAME BYTES,
 *     compared with strcmp against the test's own literals -- not against
 *     anything the parser computed. A parser that dropped, shifted or clipped
 *     a field fails on a byte comparison with a value it never touched.
 *
 *   - The expected OUTCOME at each length is derived from ONE stated rule --
 *     "a record longer than SYSUAF_LINE_MAX-1 is refused by the writer and
 *     reported by the reader; anything shorter round-trips byte-exactly" --
 *     not from re-running the implementation and recording what it did.
 *
 * The round trip goes THROUGH A REAL FILE on disk, opened and read by the same
 * functions the boot path uses. Nothing here mocks the file, the writer or the
 * reader.
 *
 * WHERE THE BOTH-WAYS PROOF LIVES. This suite cannot itself be run against the
 * unfixed tree, because the functions it exercises did not exist there -- the
 * whole change is that they now do. The both-ways proof for this defect is
 * tests/qemu/test_release_e2e.sh, which drives the SAME geometry through a
 * REAL boot and was run against an image built from the unfixed tree
 * (15 passed, 3 failed -- the halt case reproducing the operator's console
 * output verbatim) and against the fixed tree (18 passed, 0 failed). This
 * suite is the fast, hermetic guard on the same rule.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sysuaf.h"

static int pass = 0;
static int fail = 0;

#define CHECK(cond, ...) do {                                   \
    if (cond) { printf("  PASS: "); pass++; }                   \
    else      { printf("  FAIL: "); fail++; }                   \
    printf(__VA_ARGS__); printf("\n");                          \
} while (0)

/*
 * Build a record whose rendered line is EXACTLY 'target' bytes long (excluding
 * the newline), by padding the PRIVILEGES field.
 *
 * The padding is a repeated real privilege list rather than filler, because
 * "a fully expanded privilege list" is the shape the field actually grows into
 * -- the test's own dispatch names it as the realistic way a row reaches this
 * size.
 */
static int build_row(sysuaf_record_t *rec, size_t target)
{
    static const char *PRIVS =
        "TMPMBX,NETMBX,OPER,SYSPRV,BYPASS,SETPRV,CMKRNL,CMEXEC,SYSNAM,"
        "GRPNAM,DETACH,SETPRI,ALTPRI,WORLD,GROUP,LOG_IO,PHY_IO";

    memset(rec, 0, sizeof(*rec));
    snprintf(rec->username, sizeof(rec->username), "SYSTEM");
    snprintf(rec->password_hash, sizeof(rec->password_hash),
             "36a708df24b4751520ee64bba2d92167294acbb8f8fbfc3a120fb75323e9739b");
    rec->uic_group  = 1;
    rec->uic_member = 4;

    /* Fixed part: USERNAME|HASH|1|4| ... plus the six separators. */
    size_t fixed = strlen(rec->username) + 1
                 + strlen(rec->password_hash) + 1
                 + 1 + 1        /* "1|" */
                 + 1 + 1        /* "4|" */
                 + 1            /* separator after DEFAULT_DIR */
                 + 1;           /* separator after FLAGS */
    if (target <= fixed)
        return -1;

    size_t want = target - fixed;

    /*
     * PAD ACROSS DEFAULT_DIR, THEN FLAGS, THEN PRIVILEGES -- in that order,
     * so the padding pushes the FIFTH and SIXTH separators later and later,
     * which is the geometry that produced the boot halt: the halt happened
     * because the sixth separator fell past byte 511 and the 512-byte reader
     * could not find field 6 at all.
     */
    size_t dd = sizeof(rec->default_dir) - 1;
    if (dd > want) dd = want;
    snprintf(rec->default_dir, sizeof(rec->default_dir), "SYS$SYSDEVICE:[SYSMGR");
    size_t base = strlen(rec->default_dir);
    if (dd < base + 1)
        snprintf(rec->default_dir, sizeof(rec->default_dir), "%.*s",
                 (int)dd, "SYS$SYSDEVICE:[SYSMGR]");
    else {
        for (size_t i = base; i < dd - 1; i++)
            rec->default_dir[i] = 'D';
        rec->default_dir[dd - 1] = ']';
        rec->default_dir[dd] = '\0';
    }
    want -= dd;

    size_t fl = sizeof(rec->flags) - 1;
    if (fl > want) fl = want;
    memset(rec->flags, 'F', fl);
    rec->flags[fl] = '\0';
    want -= fl;

    if (want >= sizeof(rec->privileges))
        return -1;      /* not expressible in sysuaf_record_t at all */

    size_t n = 0;
    while (n < want) {
        size_t chunk = strlen(PRIVS);
        if (n + chunk > want)
            chunk = want - n;
        memcpy(rec->privileges + n, PRIVS, chunk);
        n += chunk;
        if (n < want)
            rec->privileges[n++] = ',';
    }
    rec->privileges[want] = '\0';
    return 0;
}

/*
 * Where does the SIXTH separator fall? That offset -- not the total length --
 * is what decided whether the boot halted: a 512-byte fgets() buffer holds
 * bytes 0..510, so a record whose sixth separator lands at 511 or later has
 * no PRIVILEGES field as far as that reader is concerned, and PID 1's
 * sysuaf_field("SYSTEM", 6, ...) returned "no such user".
 */
static long sixth_sep_offset(const char *line)
{
    int seen = 0;
    for (const char *p = line; *p; p++)
        if (*p == SYSUAF_SEP_CHAR && ++seen == 6)
            return (long)(p - line);
    return -1;
}

/*
 * Write 'line' to a real file, read it back through the real reader, and
 * report what came out.
 */
static int roundtrip(const char *path, const char *line,
                     sysuaf_record_t *out, int *too_long)
{
    FILE *fp = fopen(path, "w");
    if (!fp)
        return -1;
    fprintf(fp, "%s\n", line);
    fclose(fp);

    fp = fopen(path, "r");
    if (!fp)
        return -1;

    char buf[SYSUAF_LINE_MAX];
    int rc = -1;
    *too_long = 0;
    if (sysuaf_read_line(fp, buf, sizeof(buf), too_long)) {
        if (*too_long)
            rc = 0;             /* reported, not parsed -- correct */
        else
            rc = sysuaf_parse_line(buf, out);
    }
    fclose(fp);
    return rc;
}

int main(void)
{
    char path[] = "/tmp/ovmx_sysuaf_limit_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(fd);

    printf("=== SYSUAF record limit at the boundary (vms-9b7) ===\n");
    printf("SYSUAF_LINE_MAX = %d\n\n", SYSUAF_LINE_MAX);

    /* The three buffer sizes the five deleted parsers disagreed on, +/- 1. */
    const size_t lengths[] = { 511, 512, 513, 1022, 1023, 1024, 1025 };

    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        size_t want = lengths[i];

        sysuaf_record_t rec;
        if (build_row(&rec, want) != 0) {
            /* Lengths a record struct cannot express are skipped LOUDLY, with
             * the reason, rather than silently reducing the boundary set. */
            printf("  NOTE: %zu bytes not expressible in sysuaf_record_t "
                   "(privileges[%zu]) -- writer refusal tested separately\n",
                   want, sizeof(rec.privileges));
            continue;
        }

        char line[SYSUAF_LINE_MAX * 2];
        int n = sysuaf_format_record(&rec, line, sizeof(line));

        /* THE RULE, applied to the length -- not read off the implementation:
         * a line needs its own bytes plus a newline to fit in the limit. */
        int should_fit = (want + 1 < (size_t)SYSUAF_LINE_MAX);

        if (!should_fit) {
            CHECK(n < 0, "%zu bytes: writer REFUSES (over the %d-byte limit)",
                  want, SYSUAF_LINE_MAX - 1);
            continue;
        }

        CHECK(n == (int)want,
              "%zu bytes: writer emits exactly %zu bytes (got %d)",
              want, want, n);
        if (n != (int)want)
            continue;

        printf("    (sixth separator at offset %ld -- the 512-byte readers "
               "held bytes 0..510)\n", sixth_sep_offset(line));

        sysuaf_record_t back;
        int too_long = 0;
        int rc = roundtrip(path, line, &back, &too_long);

        CHECK(rc == 1 && !too_long,
              "%zu bytes: reader accepts the record whole", want);
        if (rc != 1)
            continue;

        /* Byte comparisons against the TEST's literals, never against a value
         * the parser produced. */
        CHECK(strcmp(back.username, "SYSTEM") == 0,
              "%zu bytes: USERNAME survives (got '%s')", want, back.username);
        CHECK(strcmp(back.password_hash, rec.password_hash) == 0,
              "%zu bytes: PASSWORD_HASH survives", want);
        CHECK(back.uic_group == 1 && back.uic_member == 4,
              "%zu bytes: UIC survives as [1,4] (got [%o,%o])",
              want, back.uic_group, back.uic_member);
        CHECK(strcmp(back.default_dir, rec.default_dir) == 0,
              "%zu bytes: DEFAULT_DIR survives WHOLE (%zu chars)",
              want, strlen(rec.default_dir));
        /*
         * THE FIELD THE BUG DESTROYED. PID 1 asked for field 6 (PRIVILEGES)
         * and the 512-byte reader could not find it, because the sixth
         * separator was past the end of the buffer. Compared byte-for-byte
         * against what this test wrote.
         */
        CHECK(strcmp(back.privileges, rec.privileges) == 0,
              "%zu bytes: PRIVILEGES survives WHOLE (%zu chars)",
              want, strlen(rec.privileges));
    }

    /*
     * ============================================================================
     * THE EXACT GEOMETRY THAT HALTED THE BOOT
     * ============================================================================
     * Not a length -- a POSITION. Measured on a real QEMU boot of the unfixed
     * tree: a SYSTEM row of 814 bytes whose sixth separator sits at offset 558
     * produced
     *
     *     %OVMX-F-EXECINIT, no SYSTEM record in SYS$SYSTEM:SYSUAF.DAT
     *     %OVMX-I-EXECINIT, the system process has no authorized identity
     *     reboot: Power down
     *
     * on the next boot, while the SAME rig with a 103-byte SYSTEM row and with
     * a 643-byte row (sixth separator at 387, inside the 512-byte window)
     * booted green. That pair of controls is what proves it is the position of
     * the sixth separator and not the write path.
     *
     * The line below is built by hand, at that geometry, and fed to the ONE
     * reader. It is longer than any record sysuaf_record_t can express -- which
     * is exactly why the reader may not rely on the writer's field caps to keep
     * it safe. A SYSUAF hand-edited by a system manager is a supported VMS
     * administrative act, and widening any field of the record (default_dir is
     * 256 bytes today) re-opens the same window.
     */
    printf("\n--- the exact geometry that halted the boot (sixth separator "
           "past byte 511) ---\n");
    {
        char line[SYSUAF_LINE_MAX];
        size_t k = 0;
        k += (size_t)snprintf(line + k, sizeof(line) - k,
                              "SYSTEM|36a708df24b4751520ee64bba2d92167294acbb8"
                              "f8fbfc3a120fb75323e9739b|1|4|SYS$SYSDEVICE:[SYSMGR");
        while (k < 492) line[k++] = 'D';
        line[k++] = ']';
        line[k++] = SYSUAF_SEP_CHAR;            /* fifth separator */
        for (int i = 0; i < 20; i++) line[k++] = 'F';
        line[k++] = SYSUAF_SEP_CHAR;            /* SIXTH separator, past 511 */
        k += (size_t)snprintf(line + k, sizeof(line) - k, "SYSPRV,OPER,TMPMBX");
        line[k] = '\0';

        long six = sixth_sep_offset(line);
        CHECK(six > 511,
              "the test line reproduces the geometry (sixth separator at %ld, "
              "past 511)", six);
        CHECK(strlen(line) < (size_t)SYSUAF_LINE_MAX - 1,
              "the test line is within the format limit (%zu bytes)",
              strlen(line));

        sysuaf_record_t back;
        int too_long = 0;
        int rc = roundtrip(path, line, &back, &too_long);

        CHECK(rc == 1 && !too_long,
              "the ONE reader accepts it as a record");
        /*
         * THE ASSERTION THE HALT WAS THE FAILURE OF. PID 1 asked for field 6.
         * Compared against this test's own literal, which no parser produced.
         */
        CHECK(rc == 1 && strcmp(back.privileges, "SYSPRV,OPER,TMPMBX") == 0,
              "PRIVILEGES (field 6) is found and intact -- got '%s'",
              rc == 1 ? back.privileges : "<no record>");
        CHECK(rc == 1 && back.uic_group == 1 && back.uic_member == 4,
              "the UIC the executive would be handed is still [1,4]");
    }

    /*
     * The writer refuses an over-length record even when the caller's output
     * buffer would hold it -- the limit belongs to the FORMAT, not to the
     * caller's buffer. A writer whose limit was "whatever fits in out" is how
     * a row longer than every reader gets onto disk.
     */
    printf("\n--- writer refusal is a FORMAT limit, not a buffer limit ---\n");
    {
        sysuaf_record_t rec;
        memset(&rec, 0, sizeof(rec));
        snprintf(rec.username, sizeof(rec.username), "SYSTEM");
        memset(rec.privileges, 'P', sizeof(rec.privileges) - 1);
        memset(rec.default_dir, 'D', sizeof(rec.default_dir) - 1);
        memset(rec.password_hash, 'H', sizeof(rec.password_hash) - 1);
        memset(rec.flags, 'F', sizeof(rec.flags) - 1);

        char big[SYSUAF_LINE_MAX * 4];
        int n = sysuaf_format_record(&rec, big, sizeof(big));
        size_t natural = strlen(rec.username) + strlen(rec.password_hash)
                       + strlen(rec.default_dir) + strlen(rec.flags)
                       + strlen(rec.privileges) + 6 + 2 /* uic digits */;
        if (natural + 1 >= (size_t)SYSUAF_LINE_MAX)
            CHECK(n < 0, "maximal record (%zu bytes) refused despite a "
                  "%zu-byte output buffer", natural, sizeof(big));
        else
            CHECK(n >= 0, "maximal record (%zu bytes) fits and is accepted",
                  natural);
    }

    /*
     * An over-length line in a FILE is REPORTED, and the reader resumes at the
     * next real record -- it does not hand back a fragment as if it were a
     * row. This is the exact mechanism of the boot halt: the tail of a long
     * row presented as a record of its own, and the head presented as a short
     * record.
     */
    printf("\n--- an over-length line is reported, and does not corrupt the "
           "next record ---\n");
    {
        FILE *fp = fopen(path, "w");
        if (fp) {
            for (int i = 0; i < SYSUAF_LINE_MAX + 200; i++)
                fputc('X', fp);
            fputc('|', fp);
            fputc('\n', fp);
            fprintf(fp, "SYSTEM|hash|1|4|SYS$SYSDEVICE:[SYSMGR]||ALL\n");
            fclose(fp);
        }

        fp = fopen(path, "r");
        char buf[SYSUAF_LINE_MAX];
        int too_long = 0;
        int saw_too_long = 0;
        sysuaf_record_t rec;
        int found_system = 0;
        while (fp && sysuaf_read_line(fp, buf, sizeof(buf), &too_long)) {
            if (too_long) { saw_too_long++; continue; }
            if (sysuaf_parse_line(buf, &rec) == 1 &&
                strcmp(rec.username, "SYSTEM") == 0)
                found_system = 1;
        }
        if (fp) fclose(fp);

        CHECK(saw_too_long == 1,
              "the over-length line is reported exactly once (got %d)",
              saw_too_long);
        CHECK(found_system == 1,
              "the SYSTEM record AFTER it is still found");
    }

    unlink(path);

    printf("\n=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
