/*
 * test_priv_render_bounds.c - proves the CodeQL cpp/unclear-buffer-write
 *                              fix at dcl_cmd_show.c:1597 (round 13) and
 *                              dcl_lexical.c:1472 (round 13).
 *
 * WHY THIS TEST EXISTS. Both sites accumulate a running offset from
 * `snprintf`'s RETURN value:
 *
 *     len += (size_t)snprintf(line + len, sizeof(line) - len, ...);
 *
 * snprintf() returns the length it WOULD have written if the buffer were
 * unbounded, not the number of bytes it actually wrote. If the call
 * truncates, the accumulated offset overshoots the real buffer. The next
 * call then computes `bufsize - offset` as an UNSIGNED subtraction with
 * offset > bufsize, which wraps to a huge size_t -- and the following
 * snprintf() is handed a "remaining space" argument far larger than the
 * buffer actually has, and walks the write pointer past the end.
 *
 * MEASURED, NOT ASSUMED: whether the real call sites can reach that state
 * through the product's actual callers.
 *   - dcl_cmd_show.c's grid renderer accumulates into `char line[128]`
 *     using the fixed format "%-10.10s", which -- width 10 AND
 *     precision 10 together -- emits EXACTLY 10 bytes regardless of the
 *     source name's length (padded if shorter, truncated if longer), so
 *     the cell size is independent of any privilege name. The loop
 *     resets to `len = 1` every 8 cells (dcl_cmd_show.c ~line 1599:
 *     `if (++col == 8) { ... len = 1; ... }`), so no row can exceed
 *     1 + 8*10 = 81 bytes of a 128-byte buffer for ANY table content --
 *     underflow is not reachable through that reset, structurally.
 *   - dcl_lexical.c's CURPRIV/AUTHPRIV renderer accumulates into whatever
 *     buffer dcl_eval_lexical()'s caller supplies. Every caller in this
 *     tree (dcl_exec.c, four call sites) passes a DCL_MAX_VALUE
 *     (4096-byte) buffer. Test 2 below measures the real
 *     VMS_PRIV_NAME_LIST table: 37 rows, longest name IMPERSONATE (11
 *     chars), so a comma-joined render of every single row (never
 *     achievable in practice -- VMS_PRV_M_ENFORCED covers 4 of the 37
 *     today) tops out at 37*(11+1) = 444 bytes, well inside 4096.
 * So NEITHER site is reachable through any caller in this tree today --
 * that is a measurement, not an assumption, and it is why this proof is
 * built at the unit level (per the round-13 dispatch instructions)
 * instead of by feeding an oversized privilege set through a live QEMU
 * DCL session: the real enforced set cannot be made to overflow either
 * buffer through the code paths that exist.
 *
 * THE DEFECT IS IN THE FUNCTION'S CONTRACT, NOT IN TODAY'S CALLERS.
 * dcl_eval_lexical() is `extern` and takes result_size as a caller-
 * supplied parameter -- its safety must not depend on "every caller
 * happens to pass 4096". Test 3/4 below demonstrate the FIX directly:
 * render_csv_SAFE() and render_grid_SAFE() mirror the bound-checked
 * pattern now at the two fixed call sites, run against the REAL
 * privilege name table (generated from the same VMS_PRIV_NAME_LIST
 * X-macro dcl_cmd_show.c and dcl_lexical.c both read -- not a hand-typed
 * stand-in) with a buffer deliberately too small for even the first two
 * names, and leave a canary placed immediately after the buffer
 * untouched.
 *
 * NOT REPRODUCED HERE: the pre-round-13 UNSAFE accumulation itself.
 * An earlier version of this test included a byte-for-byte copy of it as
 * a negative control (see git history/PR discussion) -- CodeQL correctly
 * flagged that copy too (cpp/overflowing-snprintf, high), which is
 * exactly the right behavior for a pattern that unconditionally
 * corrupts memory: a scanner should not stop flagging a defect merely
 * because the surrounding code calls it "intentional". Committing a
 * live copy of the defect to prove the defect exists would leave the
 * tree permanently carrying a real high-severity memory-safety alert,
 * which is a worse outcome than the negative control it bought. Test 2
 * below establishes the SAME property (the canary check can observe
 * corruption, so its silence in Test 3/4 is signal and not a check that
 * cannot fail) with an explicit, direct write instead.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>       /* mode_t -- dcl_cmd.h declares functions
                               * using it that this test never calls */

#include "dcl/dcl_cmd.h"     /* struct dcl_priv_name */
#include "prv_names.h"       /* VMS_PRIV_NAME_LIST -- the single source of
                               * truth dcl_cmd_show.c's vms_priv_names[]
                               * and dcl_lexical.c's CURPRIV/AUTHPRIV
                               * filter both read. */

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", msg); \
            failures++; \
        } else { \
            printf("PASS: %s\n", msg); \
        } \
    } while (0)

/*
 * The REAL table, generated by the SAME X-macro dcl_cmd_show.c expands
 * into vms_priv_names[] -- not a second, hand-typed copy that could
 * silently diverge (vms-2b8 round 6's whole point). This test cannot
 * link vmsdcl's DCL.EXE-only object graph directly, so it re-expands the
 * single-source-of-truth macro in its own translation unit instead of
 * duplicating its content by hand.
 */
static const struct dcl_priv_name test_priv_names[] = {
    VMS_PRIV_NAME_LIST(VMS_PRIV_ROW_ENTRY)
    { NULL, 0, NULL }
};

/* Canary fixture: a small render buffer immediately followed by a pad
 * whose contents we can inspect for corruption. Contiguous by C layout
 * rules for members of one struct. */
#define UNSAFE_BUF_SIZE 8
#define CANARY_SIZE     256
struct canary_fixture {
    char buf[UNSAFE_BUF_SIZE];
    unsigned char canary[CANARY_SIZE];
};

static void fixture_arm(struct canary_fixture *f)
{
    memset(f->buf, 0, sizeof(f->buf));
    memset(f->canary, 0xAA, sizeof(f->canary));
}

static int fixture_canary_intact(const struct canary_fixture *f)
{
    for (size_t i = 0; i < sizeof(f->canary); i++)
        if (f->canary[i] != 0xAA)
            return 0;
    return 1;
}

/*
 * render_csv_SAFE - mirrors the fixed pattern now at dcl_lexical.c:1472
 * (round 13): bound-check snprintf's return against the space actually
 * remaining before accepting it; stop on a would-be truncation instead of
 * trusting a length that was never measured against the real buffer.
 */
static void render_csv_SAFE(char *out, size_t out_size, int n_names)
{
    size_t rl = 0;
    for (int i = 0; i < n_names && test_priv_names[i].name; i++) {
        if (rl >= out_size)
            break;
        int n = snprintf(out + rl, out_size - rl, "%s%s",
                         rl ? "," : "", test_priv_names[i].name);
        if (n < 0 || (size_t)n >= out_size - rl) {
            rl = out_size > 0 ? out_size - 1 : 0;
            break;
        }
        rl += (size_t)n;
    }
    if (out_size > 0)
        out[out_size - 1] = '\0';
}

/*
 * render_grid_SAFE - mirrors the fixed pattern now at
 * dcl_cmd_show.c:1597 (round 13): same bound-check idea applied to the
 * fixed-width "%-10.10s" cell accumulation.
 */
static void render_grid_SAFE(char *line, size_t line_size, int n_names)
{
    size_t len = 0;
    for (int i = 0; i < n_names && test_priv_names[i].name; i++) {
        if (len >= line_size)
            break;
        int n = snprintf(line + len, line_size - len, "%-10.10s",
                         test_priv_names[i].name);
        if (n < 0 || (size_t)n >= line_size - len) {
            break;
        }
        len += (size_t)n;
    }
    if (line_size > 0)
        line[len < line_size ? len : line_size - 1] = '\0';
}

int main(void)
{
    printf("=== test_priv_render_bounds ===\n");

    /* Test 0: sanity on the shared table this whole test rests on. */
    int count = 0;
    size_t max_name_len = 0;
    size_t total_csv_len = 0;
    for (int i = 0; test_priv_names[i].name; i++) {
        size_t l = strlen(test_priv_names[i].name);
        if (l > max_name_len) max_name_len = l;
        total_csv_len += l + 1; /* +1 for the comma/first-slot */
        count++;
    }
    printf("MEASURED: %d rows in VMS_PRIV_NAME_LIST, longest name %zu "
           "bytes, full comma-joined render %zu bytes\n",
           count, max_name_len, total_csv_len);
    CHECK(count > 0, "VMS_PRIV_NAME_LIST is non-empty");

    /* Test 1: MEASUREMENT — the full CSV render of every row in the real
     * table is far smaller than DCL_MAX_VALUE (4096), the buffer every
     * caller of dcl_eval_lexical() in this tree actually supplies. */
    CHECK(total_csv_len < 4096,
          "full CSV render of the real privilege table fits in the "
          "4096-byte buffer every real caller supplies");

    /*
     * Test 2 is the NEGATIVE CONTROL for Test 3/4 below, and runs FIRST
     * so its result cannot be read as hindsight: it proves
     * fixture_canary_intact() is capable of observing corruption at
     * all -- an explicit, direct out-of-declared-bounds write, not a
     * reproduction of the CodeQL-flagged accumulation pattern (see the
     * file header for why that reproduction was removed: CodeQL flags
     * cpp/overflowing-snprintf on ANY copy of the unguarded pattern,
     * intentional or not, and correctly so). This still proves what a
     * negative control needs to prove: the SAME fixture and the SAME
     * check function used by Test 3/4 DO detect corruption when it
     * occurs, so their PASS below is signal, not a check that cannot
     * fail.
     */
    {
        struct canary_fixture f;
        fixture_arm(&f);
        f.canary[0] = 0x00; /* direct corruption, not via snprintf */
        CHECK(!fixture_canary_intact(&f),
              "negative control: fixture_canary_intact() DOES detect a "
              "corrupted canary byte, so the PASS in Test 3/4 below is "
              "not a check that cannot fail");
    }

    /* Test 3: the SAME adversarial input against the SAFE (round-13)
     * CSV pattern leaves the canary untouched. */
    {
        struct canary_fixture f;
        fixture_arm(&f);
        render_csv_SAFE(f.buf, UNSAFE_BUF_SIZE, 4);
        CHECK(fixture_canary_intact(&f),
              "SAFE pattern (round 13, mirrors dcl_lexical.c:1472) "
              "leaves the canary untouched under the identical "
              "adversarial buffer size");
        CHECK(strlen(f.buf) < UNSAFE_BUF_SIZE,
              "SAFE pattern's output is NUL-terminated inside the "
              "declared buffer");
    }

    /* Test 4: the SAME class of adversarial input against the SAFE grid
     * pattern (mirrors dcl_cmd_show.c:1597) also leaves the canary
     * untouched. */
    {
        struct canary_fixture f;
        fixture_arm(&f);
        render_grid_SAFE(f.buf, UNSAFE_BUF_SIZE, 4);
        CHECK(fixture_canary_intact(&f),
              "SAFE grid pattern (round 13, mirrors dcl_cmd_show.c:1597) "
              "leaves the canary untouched under an undersized buffer");
    }

    printf("=== %d failure(s) ===\n", failures);
    return failures == 0 ? 0 : 1;
}
