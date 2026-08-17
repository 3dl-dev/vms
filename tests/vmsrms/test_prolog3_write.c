/*
 * test_prolog3_write.c - vms-045: genuine Files-11 Prolog-3 indexed WRITE.
 *
 * The paired WRITE half of test_prolog3_read.c. It drives the runtime write
 * engine (src/vmsrms/rms_prolog3.c: rms_p3_create / rms_p3_put / rms_p3_update)
 * to AUTHOR a real Prolog-3 indexed file -- prologue (fixed prolog + key
 * descriptor), root index bucket (Root Level 1) and data buckets -- then $PUTs
 * enough records to FORCE at least one genuine data-bucket SPLIT (new bucket
 * allocated, records redistributed by key, RRV stubs left behind, the parent
 * index bucket's 2-byte child pointers updated), and reads EVERY record back BY
 * KEY through the READ engine (rms_p3_get_by_key). Writer and reader agree on
 * the exact [PIN]/[OVMX] byte offsets in rms_prolog3.h, so the round-trip is the
 * proof they share one on-disk format across a split.
 *
 * The engine reads/writes every block through rms_io_read_exact/write_exact
 * (rms_io.h); here the handle wraps a plain fd (rms_io_posix_wrap) because host
 * ctest has no /dev/vms. In the /dev/vms-present runtime the SAME engine writes
 * the SAME bytes over IO$_WRITEVBLK on the ACP channel window -- proven by the
 * paired positive tests/qemu/test_syssvc_rms_p3_acp.c against a real /dev/vms.
 *
 * Oracle grounding: docs/oracle/vax73-alpha84-rms-prolog3.md (vms-8438).
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

#define KEY_SIZE 8u
#define NREC     40          /* enough to force several 1-block-bucket splits */

/* record body: 8-byte key at offset 0, then "val:<nnn>:<key>" padding to ~40B */
static void make_record(int n, uint8_t *rec, uint16_t *len)
{
    char key[16], pay[64];
    int kn = snprintf(key, sizeof(key), "K%06d", n);       /* K000000..K000039 */
    int pn;
    memset(rec, ' ', KEY_SIZE);
    memcpy(rec, key, (size_t)kn < KEY_SIZE ? (size_t)kn : KEY_SIZE);
    pn = snprintf(pay, sizeof(pay), "value-for-%06d-payload", n);  /* ~26 bytes */
    memcpy(rec + KEY_SIZE, pay, (size_t)pn);
    *len = (uint16_t)(KEY_SIZE + pn);
}

static void key_of(int n, uint8_t *k)
{
    uint8_t rec[64]; uint16_t l;
    make_record(n, rec, &l);
    memcpy(k, rec, KEY_SIZE);
}

int main(void)
{
    printf("test_prolog3_write (vms-045): genuine Prolog-3 keyed WRITE + split\n");

    char path[] = "/tmp/ovmx_p3w_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("mkstemp failed\n"); return 1; }

    rms_file_t *f = rms_io_posix_wrap(fd);
    check(f != NULL, "wrap fd");

    /* ---- $CREATE: author an empty Prolog-3 indexed file (1-block buckets so a
     * modest record count forces real splits) ---- */
    p3_create_params_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.key_size   = KEY_SIZE;
    cp.seg0_pos   = 0;
    cp.seg0_siz   = KEY_SIZE;
    cp.dtp        = 0;
    cp.bkt_blocks = 1;
    cp.allow_dup  = 0;

    p3_ctx_t *ctx = NULL;
    uint32_t st = rms_p3_create(f, &cp, &ctx);
    check(st == RMS$_CREATED && ctx != NULL, "rms_p3_create authors Prolog-3 file");
    if (!ctx) { rms_io_posix_unwrap(f); unlink(path); return 1; }

    uint32_t alloc_after_create = ctx->alloc_next;

    /* verify the prologue is a real Prolog-3 image by BINDING it with the
     * independent read-engine parser (proves create wrote a parseable prologue) */
    {
        p3_ctx_t *rc = NULL;
        uint32_t bs = rms_p3_bind(f, &rc);
        check($VMS_STATUS_SUCCESS(bs) && rc &&
              rc->version == 3 && rc->num_keys == 1 &&
              rc->keys[0].key_size == KEY_SIZE && rc->keys[0].root_level == 1,
              "created prologue re-binds as Prolog-3 (version 3, 1 key)");
        rms_p3_free(rc);
    }

    /* ---- $PUT: insert NREC records in a shuffled order (exercises sorted
     * insertion + index routing + splits) ---- */
    int order[NREC];
    for (int i = 0; i < NREC; i++) order[i] = i;
    /* deterministic shuffle (fixed seed multiplier), avoids sorted-only insert */
    for (int i = NREC - 1; i > 0; i--) {
        int j = (int)(((unsigned)(i * 2654435761u)) % (unsigned)(i + 1));
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }

    int put_ok = 1;
    for (int i = 0; i < NREC; i++) {
        uint8_t rec[64]; uint16_t rl;
        make_record(order[i], rec, &rl);
        uint32_t ps = rms_p3_put(ctx, 0, rec, rl);
        if (!$VMS_STATUS_SUCCESS(ps)) {
            printf("    put %d -> 0x%X\n", order[i], ps);
            put_ok = 0;
        }
    }
    check(put_ok, "all NREC $PUTs succeeded");

    /* a split MUST have happened: alloc_next advanced past the create layout */
    check(ctx->alloc_next > alloc_after_create,
          "at least one genuine bucket SPLIT occurred (alloc_next advanced)");
    printf("    buckets allocated: create=%u now=%u (%u split(s))\n",
           alloc_after_create, ctx->alloc_next,
           ctx->alloc_next - alloc_after_create);

    /* ---- READ every record back BY KEY through the read engine ---- */
    int read_ok = 1;
    for (int n = 0; n < NREC; n++) {
        uint8_t key[KEY_SIZE], buf[128], want[64];
        uint16_t rl = 0, wl = 0;
        key_of(n, key);
        make_record(n, want, &wl);
        uint32_t gs = rms_p3_get_by_key(ctx, 0, key, KEY_SIZE, 0, 0,
                                        buf, sizeof(buf), &rl);
        if (!($VMS_STATUS_SUCCESS(gs) && rl == wl && memcmp(buf, want, wl) == 0)) {
            printf("    read-back miss for K%06d -> 0x%X (rl=%u wl=%u)\n",
                   n, gs, rl, wl);
            read_ok = 0;
        }
    }
    check(read_ok, "every record reads back BY KEY, byte-exact, across splits");

    /* ---- key ORDER: walk KGE from the smallest key up, assert ascending and
     * that exactly NREC distinct records are visited ---- */
    {
        uint8_t cur[KEY_SIZE];
        memset(cur, 0, sizeof(cur));           /* below every key */
        uint8_t prev[KEY_SIZE];
        int seen = 0, ordered = 1;
        int have_prev = 0;
        for (;;) {
            uint8_t buf[128]; uint16_t rl = 0;
            uint32_t gs = rms_p3_get_by_key(ctx, 0, cur, KEY_SIZE,
                                            0, 1 /*KGT*/, buf, sizeof(buf), &rl);
            if (gs == RMS$_RNF) break;
            if (!$VMS_STATUS_SUCCESS(gs)) { ordered = 0; break; }
            if (have_prev && memcmp(buf, prev, KEY_SIZE) <= 0) ordered = 0;
            memcpy(prev, buf, KEY_SIZE);
            memcpy(cur,  buf, KEY_SIZE);        /* advance strictly (KGT) */
            have_prev = 1;
            if (++seen > NREC + 4) { ordered = 0; break; }
        }
        check(ordered, "KGT walk visits keys in strict ascending order");
        check(seen == NREC, "KGT walk visits exactly NREC records");
    }

    /* ---- $UPDATE (same size, in place): record ID/content preserved ---- */
    {
        uint8_t rec[64]; uint16_t rl;
        uint8_t key[KEY_SIZE];
        make_record(7, rec, &rl);
        key_of(7, key);
        /* mutate the payload tail in place (same length) */
        rec[KEY_SIZE + 0] = 'V'; rec[KEY_SIZE + 1] = 'A'; rec[KEY_SIZE + 2] = 'L';
        uint32_t us = rms_p3_update(ctx, 0, key, KEY_SIZE, rec, rl);
        check($VMS_STATUS_SUCCESS(us), "$UPDATE same-size succeeds");
        uint8_t buf[128]; uint16_t got = 0;
        uint32_t gs = rms_p3_get_by_key(ctx, 0, key, KEY_SIZE, 0, 0,
                                        buf, sizeof(buf), &got);
        check($VMS_STATUS_SUCCESS(gs) && got == rl && memcmp(buf, rec, rl) == 0,
              "$UPDATE same-size read-back reflects the change");
    }

    /* ---- $UPDATE (grow: delete + reinsert), read back the longer image ---- */
    {
        uint8_t rec[80]; uint16_t rl;
        uint8_t key[KEY_SIZE];
        key_of(11, key);
        memset(rec, ' ', KEY_SIZE);
        memcpy(rec, key, KEY_SIZE);
        const char *big = "grown-record-payload-that-is-much-longer-11";
        uint16_t pl = (uint16_t)strlen(big);
        memcpy(rec + KEY_SIZE, big, pl);
        rl = (uint16_t)(KEY_SIZE + pl);
        uint32_t us = rms_p3_update(ctx, 0, key, KEY_SIZE, rec, rl);
        check($VMS_STATUS_SUCCESS(us), "$UPDATE grow (delete+reinsert) succeeds");
        uint8_t buf[128]; uint16_t got = 0;
        uint32_t gs = rms_p3_get_by_key(ctx, 0, key, KEY_SIZE, 0, 0,
                                        buf, sizeof(buf), &got);
        check($VMS_STATUS_SUCCESS(gs) && got == rl && memcmp(buf, rec, rl) == 0,
              "$UPDATE grow read-back reflects the longer record");
        /* every OTHER record still present after the grow-triggered churn */
        int still = 1;
        for (int n = 0; n < NREC; n++) {
            if (n == 7 || n == 11) continue;
            uint8_t k2[KEY_SIZE], b2[128], w2[64]; uint16_t r2 = 0, wl2 = 0;
            key_of(n, k2); make_record(n, w2, &wl2);
            uint32_t s2 = rms_p3_get_by_key(ctx, 0, k2, KEY_SIZE, 0, 0,
                                            b2, sizeof(b2), &r2);
            if (!($VMS_STATUS_SUCCESS(s2) && r2 == wl2 && memcmp(b2, w2, wl2) == 0))
                still = 0;
        }
        check(still, "all untouched records survive the $UPDATE churn");
    }

    /* ---- duplicate key is rejected fail-honest (allow_dup off) ---- */
    {
        uint8_t rec[64]; uint16_t rl;
        make_record(3, rec, &rl);
        uint32_t ds = rms_p3_put(ctx, 0, rec, rl);
        check(ds == RMS$_DUP, "duplicate key -> RMS$_DUP (fail honest)");
    }

    rms_p3_free(ctx);
    rms_io_posix_unwrap(f);       /* closes fd */
    unlink(path);

    /* ---- second file: a key WIDER than its segment (seg0_siz < key_size, key
     * not at offset 0) -- exercises the index-key zero-pad path across a split
     * and a non-zero seg-0 position ---- */
    {
        char p2[] = "/tmp/ovmx_p3w2_XXXXXX";
        int fd2 = mkstemp(p2);
        rms_file_t *f2 = rms_io_posix_wrap(fd2);
        p3_create_params_t c2;
        p3_ctx_t *cx = NULL;
        int ok2 = 1, n;
        uint32_t cs;
        memset(&c2, 0, sizeof(c2));
        c2.key_size = 10; c2.seg0_pos = 2; c2.seg0_siz = 6; c2.bkt_blocks = 1;
        cs = rms_p3_create(f2, &c2, &cx);
        check(cs == RMS$_CREATED && cx, "create key_size=10 seg0_pos=2 seg0_siz=6");
        for (n = 0; cx && n < 30; n++) {
            uint8_t rec[48]; uint16_t rl;
            char kk[8];
            memset(rec, 0, sizeof(rec));
            snprintf(kk, sizeof(kk), "S%05d", n);       /* 6-byte seg-0 key */
            memcpy(rec + 2, kk, 6);
            memcpy(rec + 12, "payload", 7);
            rl = 12 + 7;
            if (!$VMS_STATUS_SUCCESS(rms_p3_put(cx, 0, rec, rl))) ok2 = 0;
        }
        check(ok2, "wide-key $PUTs (with pad) succeed");
        check(cx && cx->alloc_next > 6, "wide-key inserts forced a split");
        ok2 = 1;
        for (n = 0; cx && n < 30; n++) {
            uint8_t key[6], buf[64]; uint16_t rl = 0;
            char kk[8];
            snprintf(kk, sizeof(kk), "S%05d", n);
            memcpy(key, kk, 6);
            if (!$VMS_STATUS_SUCCESS(rms_p3_get_by_key(cx, 0, key, 6, 0, 0,
                                                       buf, sizeof(buf), &rl)))
                ok2 = 0;
        }
        check(ok2, "wide-key records read back BY KEY across the split");
        rms_p3_free(cx);
        rms_io_posix_unwrap(f2);
        unlink(p2);
    }

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
