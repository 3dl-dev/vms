/*
 * test_prolog3_read.c - vms-5f0: genuine Files-11 Prolog-3 indexed READ.
 *
 * Exercises the real on-disk Prolog-3 read engine (src/vmsrms/rms_prolog3.c)
 * against a byte-authored Prolog-3 image: prologue (fixed prolog + key
 * descriptor), a single-level (Root Level 1) key-of-reference index bucket with
 * 2-byte child pointers, and two primary data buckets (14-byte header, records
 * at offset 0x0E, control-flags + Record ID + RRV + record data). It builds the
 * image with a test-local writer (the runtime WRITE engine is vms-045), reads
 * it back BY PRIMARY KEY, and checks the returned records byte-for-byte -- the
 * round-trip the task calls for.
 *
 * The engine reads every block through rms_io_read_exact (rms_io.h); here the
 * handle wraps a plain fd (rms_io_posix_wrap) because host ctest has no
 * /dev/vms. In the /dev/vms-present runtime the SAME engine reads the SAME
 * bytes over IO$_READVBLK on the ACP channel window -- this test proves the
 * parser/index-walk; the QEMU/ACP end-to-end is the paired positive pending the
 * writer rung (see the report).
 *
 * Oracle offsets honored: 14-byte bucket header + records@0x0E, area descriptor
 * @VBN 3 (64 bytes), key descriptor @VBN 1, 2-byte index pointers, data-record
 * control-flags/Record-ID/RRV lead, key-flag bit positions, Prolog Version 3
 * (docs/oracle/vax73-alpha84-rms-prolog3.md, vms-8438). Byte offsets the oracle
 * does not publish are the OVMX choices defined in rms_prolog3.h ([OVMX]).
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

/* ---- fixture geometry ---- */
#define KEY_SIZE   8u
#define VBN_KD     1u    /* fixed prolog + key descriptor */
#define VBN_AREA   3u    /* [PIN] area descriptors */
#define VBN_DATA_A 4u    /* data bucket A */
#define VBN_DATA_B 5u    /* data bucket B */
#define VBN_ROOT   10u   /* root index bucket */
#define NBLK       10u   /* total blocks (VBN 1..10) */

/* zero-padded 8-char keys, in order */
static const char *const KEYS[] = {
    "ALPHA   ", "BRAVO   ", "CHARLIE ", "DELTA   ",
    "ECHO    ", "FOXTROT ", "GOLF    ", "HOTEL   ",
};
#define NREC 8

/* payload for a key: "rec:<key>" */
static void make_record(const char *key, uint8_t *rec, uint16_t *len)
{
    char pay[64];
    int n = snprintf(pay, sizeof(pay), "rec:%.8s", key);
    memcpy(rec, key, KEY_SIZE);              /* seg0 at offset 0 */
    memcpy(rec + KEY_SIZE, pay, (size_t)n);
    *len = (uint16_t)(KEY_SIZE + n);
}

/* Append one data record to a bucket buffer at *off, updating *off. */
static void put_data_record(uint8_t *bkt, uint16_t *off, uint16_t recid,
                            const char *key)
{
    uint8_t rec[64];
    uint16_t rlen = 0;
    uint16_t o = *off;
    make_record(key, rec, &rlen);
    bkt[o + P3_DR_OFF_CTRL] = 0;                 /* not deleted, no RRV flag */
    p3_put_le16(bkt + o + P3_DR_OFF_RECID, recid);
    p3_put_le16(bkt + o + P3_DR_OFF_RRVID, recid);
    p3_put_le32(bkt + o + P3_DR_OFF_RRVPTR, VBN_DATA_A); /* self area, demo RRV */
    p3_put_le16(bkt + o + P3_DR_OFF_DATALEN, rlen);
    memcpy(bkt + o + P3_DR_HDR_SIZE, rec, rlen);
    *off = (uint16_t)(o + P3_DR_HDR_SIZE + rlen);
}

/* Fill a bucket header. */
static void bkt_header(uint8_t *bkt, uint8_t level, uint8_t flags,
                       uint16_t free_off, uint32_t next_vbn)
{
    bkt[P3_BH_OFF_CHECK]  = 0x4B;                /* [OVMX] check byte */
    bkt[P3_BH_OFF_KOR]    = 0;                   /* key of reference 0 */
    bkt[P3_BH_OFF_LEVEL]  = level;
    bkt[P3_BH_OFF_FLAGS]  = flags;
    p3_put_le16(bkt + P3_BH_OFF_FREESPACE, free_off);
    p3_put_le16(bkt + P3_BH_OFF_FREE_RECID, 1);
    p3_put_le16(bkt + P3_BH_OFF_VBN_SAMPLE, 0);
    p3_put_le32(bkt + P3_BH_OFF_NEXT_VBN, next_vbn);
}

/* Build the whole 10-block Prolog-3 image into img (NBLK*512). */
static void build_image(uint8_t *img)
{
    memset(img, 0, (size_t)NBLK * P3_BLK);

    /* ---- VBN 1: fixed prolog + key descriptor #0 ---- */
    uint8_t *fp = img + (VBN_KD - 1) * P3_BLK;
    p3_put_le16(fp + P3_FP_OFF_VERSION, P3_PROLOG_VERSION);   /* [PIN] == 3 */
    p3_put_le16(fp + P3_FP_OFF_NUM_KEYS, 1);
    p3_put_le16(fp + P3_FP_OFF_NUM_AREAS, 1);
    p3_put_le16(fp + P3_FP_OFF_FIRST_AREAVBN, VBN_AREA);      /* [PIN] == 3 */
    p3_put_le16(fp + P3_FP_OFF_FIRST_KEYOFF, P3_FP_HDR_SIZE);

    uint8_t *kd = fp + P3_FP_HDR_SIZE;
    p3_put_le16(kd + P3_KD_OFF_NEXT_VBN, 0);                  /* chain end */
    p3_put_le16(kd + P3_KD_OFF_NEXT_OFF, 0);
    kd[P3_KD_OFF_REF]   = 0;
    kd[P3_KD_OFF_DTP]   = 0;                                  /* string */
    p3_put_le16(kd + P3_KD_OFF_FLAGS, 0);                     /* no compression */
    kd[P3_KD_OFF_ROOT_LEVEL] = 1;                             /* single-level */
    kd[P3_KD_OFF_IBS]   = 1;
    kd[P3_KD_OFF_DBS]   = 1;
    kd[P3_KD_OFF_NSEG]  = 1;
    p3_put_le32(kd + P3_KD_OFF_ROOT_VBN, VBN_ROOT);
    p3_put_le32(kd + P3_KD_OFF_FIRST_DATAVBN, VBN_DATA_A);
    p3_put_le16(kd + P3_KD_OFF_KEY_SIZE, KEY_SIZE);
    p3_put_le16(kd + P3_KD_OFF_MIN_REC_SIZE, KEY_SIZE);
    p3_put_le16(kd + P3_KD_OFF_SEG0_POS, 0);
    kd[P3_KD_OFF_SEG0_SIZ] = KEY_SIZE;
    memcpy(kd + P3_KD_OFF_NAME, "PRIMARY", 7);

    /* ---- VBN 3: one area descriptor (64 bytes) ---- */
    uint8_t *ad = img + (VBN_AREA - 1) * P3_BLK;
    ad[0] = 1;                                                /* bucket size */

    /* ---- VBN 4: data bucket A (ALPHA..DELTA), next -> B ---- */
    uint8_t *ba = img + (VBN_DATA_A - 1) * P3_BLK;
    {
        uint16_t off = P3_BKT_HDR_SIZE;
        for (int i = 0; i < 4; i++)
            put_data_record(ba, &off, (uint16_t)(i + 1), KEYS[i]);
        bkt_header(ba, 0, 0, off, VBN_DATA_B);
    }

    /* ---- VBN 5: data bucket B (ECHO..HOTEL), last bucket ---- */
    uint8_t *bb = img + (VBN_DATA_B - 1) * P3_BLK;
    {
        uint16_t off = P3_BKT_HDR_SIZE;
        for (int i = 4; i < 8; i++)
            put_data_record(bb, &off, (uint16_t)(i + 1), KEYS[i]);
        bkt_header(bb, 0, (uint8_t)(1u << P3_BKTV_LASTBKT), off, 0);
    }

    /* ---- VBN 10: root index bucket, entries {u16 child, key} ---- */
    uint8_t *rb = img + (VBN_ROOT - 1) * P3_BLK;
    {
        uint16_t off = P3_BKT_HDR_SIZE;
        /* entry -> bucket A, high-key = DELTA (KEYS[3]) */
        p3_put_le16(rb + off, (uint16_t)VBN_DATA_A);
        memcpy(rb + off + P3_IDXREC_PTR_SIZE, KEYS[3], KEY_SIZE);
        off = (uint16_t)(off + P3_IDXREC_PTR_SIZE + KEY_SIZE);
        /* entry -> bucket B, high-key = HOTEL (KEYS[7]) */
        p3_put_le16(rb + off, (uint16_t)VBN_DATA_B);
        memcpy(rb + off + P3_IDXREC_PTR_SIZE, KEYS[7], KEY_SIZE);
        off = (uint16_t)(off + P3_IDXREC_PTR_SIZE + KEY_SIZE);
        bkt_header(rb, 1,
                   (uint8_t)((1u << P3_BKTV_ROOTBKT) | (1u << P3_BKTV_LASTBKT)),
                   off, VBN_ROOT);
    }
}

static int get_expect(p3_ctx_t *ctx, const char *key, const char *tag)
{
    uint8_t buf[128];
    uint16_t rlen = 0;
    uint8_t want[64];
    uint16_t wlen = 0;
    uint32_t st = rms_p3_get_by_key(ctx, 0, (const uint8_t *)key, KEY_SIZE,
                                    0, 0, buf, sizeof(buf), &rlen);
    make_record(key, want, &wlen);
    int ok = ($VMS_STATUS_SUCCESS(st) && rlen == wlen &&
              memcmp(buf, want, wlen) == 0);
    check(ok, tag);
    return ok;
}

int main(void)
{
    printf("test_prolog3_read (vms-5f0): genuine Prolog-3 keyed READ\n");

    /* author the image */
    uint8_t *img = malloc((size_t)NBLK * P3_BLK);
    if (!img) { printf("OOM\n"); return 1; }
    build_image(img);

    char path[] = "/tmp/ovmx_p3_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("mkstemp failed\n"); free(img); return 1; }
    if (write(fd, img, (size_t)NBLK * P3_BLK) != (ssize_t)((size_t)NBLK * P3_BLK)) {
        printf("write failed\n"); close(fd); unlink(path); free(img); return 1;
    }
    lseek(fd, 0, SEEK_SET);

    rms_file_t *f = rms_io_posix_wrap(fd);
    check(f != NULL, "wrap fd");

    /* bind prologue */
    p3_ctx_t *ctx = NULL;
    uint32_t st = rms_p3_bind(f, &ctx);
    check($VMS_STATUS_SUCCESS(st) && ctx != NULL, "rms_p3_bind Prolog-3 prologue");
    if (ctx) {
        check(ctx->version == 3, "prolog version == 3");
        check(ctx->num_keys == 1, "num_keys == 1");
        check(ctx->keys[0].root_vbn == VBN_ROOT, "key0 root VBN parsed");
        check(ctx->keys[0].first_data_vbn == VBN_DATA_A, "key0 first-data VBN parsed");
        check(ctx->keys[0].key_size == KEY_SIZE, "key0 key size parsed");
        check(ctx->keys[0].root_level == 1, "key0 root level parsed");
    }

    if (ctx) {
        /* exact-key reads across BOTH data buckets (index walk chooses child) */
        get_expect(ctx, "ALPHA   ", "GET ALPHA  (bucket A, first)");
        get_expect(ctx, "DELTA   ", "GET DELTA  (bucket A, last)");
        get_expect(ctx, "ECHO    ", "GET ECHO   (bucket B, first)");
        get_expect(ctx, "HOTEL   ", "GET HOTEL  (bucket B, last)");
        get_expect(ctx, "CHARLIE ", "GET CHARLIE(bucket A, mid)");
        get_expect(ctx, "GOLF    ", "GET GOLF   (bucket B, mid)");

        /* missing key -> RMS$_RNF (fail honest, not a fake hit) */
        {
            uint8_t buf[128]; uint16_t rlen = 0;
            uint32_t s = rms_p3_get_by_key(ctx, 0, (const uint8_t *)"ZULU    ",
                                           KEY_SIZE, 0, 0, buf, sizeof(buf), &rlen);
            check(s == RMS$_RNF, "GET ZULU -> RMS$_RNF (miss)");
        }

        /* $FIND locates without copying */
        {
            uint16_t rlen = 0;
            uint32_t s = rms_p3_find_by_key(ctx, 0, (const uint8_t *)"BRAVO   ",
                                            KEY_SIZE, 0, 0, &rlen);
            uint8_t want[64]; uint16_t wlen = 0; make_record("BRAVO   ", want, &wlen);
            check($VMS_STATUS_SUCCESS(s) && rlen == wlen, "FIND BRAVO locates record");
        }

        /* KGE: key >= "E......" spanning into bucket B returns ECHO */
        {
            uint8_t buf[128]; uint16_t rlen = 0;
            uint32_t s = rms_p3_get_by_key(ctx, 0, (const uint8_t *)"E       ",
                                           KEY_SIZE, 1, 0, buf, sizeof(buf), &rlen);
            uint8_t want[64]; uint16_t wlen = 0; make_record("ECHO    ", want, &wlen);
            check($VMS_STATUS_SUCCESS(s) && rlen == wlen &&
                  memcmp(buf, want, wlen) == 0, "GET KGE 'E' -> ECHO");
        }

        /* RTB: buffer too small reports the true record size, no overrun */
        {
            uint8_t small[4]; uint16_t rlen = 0;
            uint32_t s = rms_p3_get_by_key(ctx, 0, (const uint8_t *)"ALPHA   ",
                                           KEY_SIZE, 0, 0, small, sizeof(small), &rlen);
            check(s == RMS$_RTB && rlen > sizeof(small), "GET RTB reports true size");
        }
    }

    /* compression-flagged key must fail honest (INV-6), not mis-decode */
    {
        uint8_t *img2 = malloc((size_t)NBLK * P3_BLK);
        build_image(img2);
        uint8_t *kd = img2 + (VBN_KD - 1) * P3_BLK + P3_FP_HDR_SIZE;
        p3_put_le16(kd + P3_KD_OFF_FLAGS, (uint16_t)(1u << P3_KEYV_KEY_COMPR));
        char p2[] = "/tmp/ovmx_p3c_XXXXXX";
        int fd2 = mkstemp(p2);
        (void)!write(fd2, img2, (size_t)NBLK * P3_BLK);
        lseek(fd2, 0, SEEK_SET);
        rms_file_t *f2 = rms_io_posix_wrap(fd2);
        p3_ctx_t *c2 = NULL;
        rms_p3_bind(f2, &c2);
        uint8_t buf[128]; uint16_t rlen = 0;
        uint32_t s = c2 ? rms_p3_get_by_key(c2, 0, (const uint8_t *)"ALPHA   ",
                                            KEY_SIZE, 0, 0, buf, sizeof(buf), &rlen)
                        : RMS$_PLG;
        check(s == RMS$_PLG, "compression flag -> RMS$_PLG (fail honest)");
        rms_p3_free(c2);
        rms_io_posix_unwrap(f2);
        unlink(p2);
        free(img2);
    }

    rms_p3_free(ctx);
    rms_io_posix_unwrap(f);       /* closes fd */
    unlink(path);
    free(img);

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
