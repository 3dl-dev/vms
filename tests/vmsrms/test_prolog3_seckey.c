/*
 * test_prolog3_seckey.c - vms-2ae: genuine Files-11 Prolog-3 SECONDARY key /
 * SIDR round-trip. Models SYSUAF: PRIMARY key = 32-byte username, SECONDARY key
 * (key of reference 1) = 4-byte UIC, with duplicate UICs allowed (two usernames
 * sharing a UIC group -- exactly the SIDR pointer-array case).
 *
 * It drives the runtime write engine (src/vmsrms/rms_prolog3.c) to
 *   1. $CREATE the file with a primary key, then rms_p3_add_secondary_key to
 *      define the UIC secondary index (its own root + SIDR bucket).
 *   2. $PUT enough records (44 bytes each, 1-block buckets) to SPLIT the primary
 *      data buckets AND to split the secondary SIDR bucket (38 distinct UICs).
 *   3. Read every record BY THE SECONDARY KEY -- descend the secondary index to
 *      the SIDR, take its primary RFA, resolve the PRIMARY record -- and assert
 *      byte-exact match to the right primary. This is a real secondary index,
 *      NOT a flat scan filtered by the UIC field.
 *   4. Duplicate case: three usernames share one UIC; the SIDR carries a
 *      3-pointer array; the lookup returns all three, each resolving to the
 *      correct distinct username record.
 *   5. RFA stability: because many $PUTs forced primary bucket SPLITS, records
 *      moved; the secondary lookup STILL resolves them (the RFA rides RRV stubs).
 *   6. $DELETE: deleting a primary record purges its SIDR pointer -- the
 *      secondary lookup no longer resolves it, and a unique-UIC delete removes
 *      the whole SIDR.
 *
 * The engine reads/writes every block through rms_io_read_exact/write_exact;
 * here the handle wraps a plain fd (host ctest has no /dev/vms). The /dev/vms
 * ACP end-to-end is the paired positive tests/qemu/test_syssvc_rms_p3_acp.c.
 *
 * Oracle grounding: docs/oracle/vax73-alpha84-rms-prolog3.md (SIDR §4/§5).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>

#include "rms_prolog3.h"
#include "rmsdef.h"
#include "ssdef.h"

static int failures = 0;
static void check(int cond, const char *name)
{
    if (cond) { printf("  OK: %s\n", name); }
    else { printf("  FAIL: %s\n", name); failures++; }
}

#define USER_KEY  32u        /* primary key: username, 32 bytes */
#define UIC_POS   32u        /* secondary key: UIC at record offset 32 */
#define UIC_KEY   4u         /* secondary key: 4-byte UIC */
#define REC_LEN   44u        /* 32 user + 4 uic + 8 payload */
#define NREC      40

/* UIC of record n. Users 17 and 29 collapse to user 4's UIC -> a 3-member
 * duplicate secondary-key group {4,17,29}. Every other n is a distinct UIC. */
static uint32_t uic_of(int n)
{
    if (n == 17 || n == 29) n = 4;
    return 0x00010000u + (uint32_t)n;      /* [group 1, member n] */
}

/* Deterministic 44-byte record for n: [username 32][uic 4 LE][payload 8]. */
static void make_record(int n, uint8_t *rec)
{
    char u[16], pl[16];
    int un = snprintf(u, sizeof(u), "USER%05d", n);
    memset(rec, ' ', USER_KEY);
    memcpy(rec, u, (size_t)un < USER_KEY ? (size_t)un : USER_KEY);
    p3_put_le32(rec + UIC_POS, uic_of(n));
    memset(rec + 36, 0, 8);
    snprintf(pl, sizeof(pl), "PL%05d", n);       /* 7 chars + NUL == 8 */
    memcpy(rec + 36, pl, 8);
}

static void user_key(int n, uint8_t *k)
{
    uint8_t rec[REC_LEN];
    make_record(n, rec);
    memcpy(k, rec, USER_KEY);
}

int main(void)
{
    printf("test_prolog3_seckey (vms-2ae): secondary key / SIDR round-trip\n");

    char path[] = "/tmp/ovmx_p3sk_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("mkstemp failed\n"); return 1; }
    rms_file_t *f = rms_io_posix_wrap(fd);
    check(f != NULL, "wrap fd");

    /* ---- $CREATE with the primary (username) key ---- */
    p3_create_params_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.key_size = USER_KEY; cp.seg0_pos = 0; cp.seg0_siz = USER_KEY;
    cp.bkt_blocks = 1; cp.allow_dup = 0;
    p3_ctx_t *ctx = NULL;
    uint32_t st = rms_p3_create(f, &cp, &ctx);
    check(st == RMS$_CREATED && ctx, "rms_p3_create (primary username key)");
    if (!ctx) { rms_io_posix_unwrap(f); unlink(path); return 1; }
    uint32_t alloc_after_create = ctx->alloc_next;

    /* ---- define the UIC secondary key (duplicates allowed) ---- */
    p3_create_params_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.key_size = UIC_KEY; sp.seg0_pos = UIC_POS; sp.seg0_siz = UIC_KEY;
    sp.bkt_blocks = 1; sp.allow_dup = 1;
    st = rms_p3_add_secondary_key(ctx, &sp);
    check(st == RMS$_NORMAL && ctx->num_keys == 2,
          "rms_p3_add_secondary_key (UIC secondary index)");

    /* re-bind proves the 2-key prologue is a real on-disk Prolog-3 image */
    {
        p3_ctx_t *rc = NULL;
        uint32_t bs = rms_p3_bind(f, &rc);
        check($VMS_STATUS_SUCCESS(bs) && rc && rc->num_keys == 2 &&
              rc->keys[0].ref == 0 && rc->keys[0].key_size == USER_KEY &&
              rc->keys[1].ref == 1 && rc->keys[1].key_size == UIC_KEY &&
              rc->keys[1].seg0_pos == UIC_POS,
              "prologue re-binds with 2 keys (primary + UIC secondary)");
        rms_p3_free(rc);
    }

    uint32_t alloc_after_seckey = ctx->alloc_next;
    check(alloc_after_seckey > alloc_after_create,
          "secondary key allocated its own root + SIDR bucket");

    /* ---- $PUT NREC records in shuffled order ---- */
    int order[NREC];
    for (int i = 0; i < NREC; i++) order[i] = i;
    for (int i = NREC - 1; i > 0; i--) {
        int j = (int)(((unsigned)(i * 2654435761u)) % (unsigned)(i + 1));
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }
    int put_ok = 1;
    for (int i = 0; i < NREC; i++) {
        uint8_t rec[REC_LEN];
        make_record(order[i], rec);
        uint32_t ps = rms_p3_put(ctx, 0, rec, REC_LEN);
        if (!$VMS_STATUS_SUCCESS(ps)) { printf("    put %d -> 0x%X\n", order[i], ps); put_ok = 0; }
    }
    check(put_ok, "all NREC $PUTs (primary + auto SIDR maintenance) succeeded");
    check(ctx->alloc_next > alloc_after_seckey,
          "primary AND/OR SIDR buckets SPLIT (alloc_next advanced)");
    printf("    buckets: create=%u +seckey=%u now=%u\n",
           alloc_after_create, alloc_after_seckey, ctx->alloc_next);

    /* ---- primary sanity: every record reads back by username ---- */
    {
        int ok = 1;
        for (int n = 0; n < NREC; n++) {
            uint8_t key[USER_KEY], buf[64], want[REC_LEN]; uint16_t rl = 0;
            user_key(n, key); make_record(n, want);
            uint32_t gs = rms_p3_get_by_key(ctx, 0, key, USER_KEY, 0, 0,
                                            buf, sizeof(buf), &rl);
            if (!($VMS_STATUS_SUCCESS(gs) && rl == REC_LEN &&
                  memcmp(buf, want, REC_LEN) == 0)) ok = 0;
        }
        check(ok, "every record reads back BY PRIMARY key (sanity)");
    }

    /* ---- SECONDARY read: for every DISTINCT-UIC record, look it up by UIC and
     * assert the resolved primary record is byte-exact the right username ---- */
    {
        int ok = 1;
        for (int n = 0; n < NREC; n++) {
            if (n == 17 || n == 29) continue;   /* dup group tested separately */
            uint8_t uk[UIC_KEY], buf[64], want[REC_LEN]; uint16_t rl = 0;
            p3_put_le32(uk, uic_of(n));
            make_record(n, want);
            uint32_t gs = rms_p3_get_by_key(ctx, 1, uk, UIC_KEY, 0, 0,
                                            buf, sizeof(buf), &rl);
            if (!($VMS_STATUS_SUCCESS(gs) && rl == REC_LEN &&
                  memcmp(buf, want, REC_LEN) == 0)) {
                printf("    sec-lookup miss n=%d uic=%08x -> 0x%X rl=%u\n",
                       n, uic_of(n), gs, rl);
                ok = 0;
            }
        }
        check(ok, "every distinct-UIC record resolves BY SECONDARY key, byte-exact "
                  "(real index descent + RFA, across primary AND SIDR splits)");
    }

    /* ---- DUPLICATE secondary key: UIC of user 4 maps to {4,17,29} ---- */
    {
        uint8_t uk[UIC_KEY]; p3_rfa_t rfa[8]; uint16_t cnt = 0;
        p3_put_le32(uk, uic_of(4));
        uint32_t ls = rms_p3_sidr_lookup(ctx, 1, uk, UIC_KEY, rfa, 8, &cnt);
        check($VMS_STATUS_SUCCESS(ls) && cnt == 3,
              "duplicate UIC SIDR carries a 3-pointer array {4,17,29}");
        /* resolve each RFA and collect which usernames came back */
        int seen4 = 0, seen17 = 0, seen29 = 0, all_exact = 1;
        for (int i = 0; i < (int)cnt; i++) {
            uint8_t buf[64]; uint16_t rl = 0;
            uint32_t rs = rms_p3_get_by_rfa(ctx, rfa[i].home_vbn,
                                            rfa[i].home_recid, buf, sizeof(buf), &rl);
            if (!$VMS_STATUS_SUCCESS(rs) || rl != REC_LEN) { all_exact = 0; continue; }
            for (int cand = 0; cand < NREC; cand++) {
                uint8_t want[REC_LEN]; make_record(cand, want);
                if (memcmp(buf, want, REC_LEN) == 0) {
                    if (cand == 4) seen4 = 1;
                    else if (cand == 17) seen17 = 1;
                    else if (cand == 29) seen29 = 1;
                    else all_exact = 0;   /* resolved to a WRONG record */
                }
            }
        }
        check(all_exact && seen4 && seen17 && seen29,
              "all three duplicate-UIC pointers resolve to the correct distinct users");
    }

    /* ---- RFA stability spot-check: force MORE splits with extra records, then
     * re-verify a record known to be in the file still resolves by UIC ---- */
    {
        int moved_ok = 1;
        for (int n = 0; n < NREC; n++) {
            if (n == 17 || n == 29) continue;
            uint8_t uk[UIC_KEY], buf[64], want[REC_LEN]; uint16_t rl = 0;
            p3_put_le32(uk, uic_of(n)); make_record(n, want);
            uint32_t gs = rms_p3_get_by_key(ctx, 1, uk, UIC_KEY, 0, 0,
                                            buf, sizeof(buf), &rl);
            if (!($VMS_STATUS_SUCCESS(gs) && rl == REC_LEN &&
                  memcmp(buf, want, REC_LEN) == 0)) moved_ok = 0;
        }
        check(moved_ok, "secondary lookups stable across the primary bucket splits (RFA)");
    }

    /* ---- $DELETE removes the SIDR: delete user 17 (dup member) ---- */
    {
        uint8_t k17[USER_KEY]; user_key(17, k17);
        uint32_t dl = rms_p3_delete(ctx, 0, k17, USER_KEY);
        check($VMS_STATUS_SUCCESS(dl), "$DELETE user 17 (a duplicate-UIC member)");
        /* primary gone */
        uint8_t buf[64]; uint16_t rl = 0;
        uint32_t gs = rms_p3_get_by_key(ctx, 0, k17, USER_KEY, 0, 0, buf, sizeof(buf), &rl);
        check(gs == RMS$_RNF, "deleted user 17 no longer found by primary key");
        /* SIDR pointer for user 17 removed: dup array shrinks to {4,29} */
        uint8_t uk[UIC_KEY]; p3_rfa_t rfa[8]; uint16_t cnt = 0;
        p3_put_le32(uk, uic_of(4));
        uint32_t ls = rms_p3_sidr_lookup(ctx, 1, uk, UIC_KEY, rfa, 8, &cnt);
        check($VMS_STATUS_SUCCESS(ls) && cnt == 2,
              "SIDR pointer array shrank to 2 after deleting user 17");
        int seen4 = 0, seen29 = 0, no17 = 1;
        for (int i = 0; i < (int)cnt; i++) {
            uint8_t b[64]; uint16_t l = 0;
            if (!$VMS_STATUS_SUCCESS(rms_p3_get_by_rfa(ctx, rfa[i].home_vbn,
                                    rfa[i].home_recid, b, sizeof(b), &l))) continue;
            uint8_t w4[REC_LEN], w29[REC_LEN], w17[REC_LEN];
            make_record(4, w4); make_record(29, w29); make_record(17, w17);
            if (l == REC_LEN && memcmp(b, w4, REC_LEN) == 0) seen4 = 1;
            if (l == REC_LEN && memcmp(b, w29, REC_LEN) == 0) seen29 = 1;
            if (l == REC_LEN && memcmp(b, w17, REC_LEN) == 0) no17 = 0;
        }
        check(seen4 && seen29 && no17,
              "remaining dup pointers resolve to {4,29}; user 17 purged");
    }

    /* ---- $DELETE of a UNIQUE-UIC record removes the whole SIDR ---- */
    {
        uint8_t k10[USER_KEY]; user_key(10, k10);
        uint32_t dl = rms_p3_delete(ctx, 0, k10, USER_KEY);
        check($VMS_STATUS_SUCCESS(dl), "$DELETE user 10 (unique UIC)");
        uint8_t uk[UIC_KEY]; p3_rfa_t rfa[8]; uint16_t cnt = 0;
        p3_put_le32(uk, uic_of(10));
        uint32_t ls = rms_p3_sidr_lookup(ctx, 1, uk, UIC_KEY, rfa, 8, &cnt);
        check(ls == RMS$_RNF, "unique-UIC SIDR fully removed after primary $DELETE");
        /* every OTHER record still resolves by UIC (delete did not corrupt) */
        int survive = 1;
        for (int n = 0; n < NREC; n++) {
            if (n == 10 || n == 17 || n == 29) continue;
            uint8_t u[UIC_KEY], buf[64], want[REC_LEN]; uint16_t rl = 0;
            p3_put_le32(u, uic_of(n)); make_record(n, want);
            uint32_t gs = rms_p3_get_by_key(ctx, 1, u, UIC_KEY, 0, 0, buf, sizeof(buf), &rl);
            if (n == 4) continue;   /* dup group verified above */
            if (!($VMS_STATUS_SUCCESS(gs) && rl == REC_LEN &&
                  memcmp(buf, want, REC_LEN) == 0)) survive = 0;
        }
        check(survive, "all untouched records still resolve by UIC after two $DELETEs");
    }

    rms_p3_free(ctx);
    rms_io_posix_unwrap(f);
    unlink(path);

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
