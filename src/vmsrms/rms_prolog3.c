/*
 * rms_prolog3.c - RMS Prolog-3 indexed-file READ engine (vms-5f0).
 *
 * The genuine on-disk Files-11 Prolog-3 index, read over the executive ACP.
 * See rms_prolog3.h for the full oracle-grounding table ([PIN]/[OVMX]) and the
 * substrate-agnostic invariant. This file contains NO substrate #ifdef and NO
 * native-width serialized field: every on-disk value is pulled with p3_le16()/
 * p3_le32(), so VAX ILP32 and x86_64/aarch64/Alpha LP64 compile it identically.
 *
 * READ ALGORITHM (smallest genuine increment):
 *   1. rms_p3_bind: read VBN 1, verify Prolog Version == 3, parse the fixed
 *      prolog header + the key-descriptor chain (root VBN, first-data VBN,
 *      root level, bucket sizes, key size, seg-0 position/size, flags).
 *   2. rms_p3_get_by_key: reject compression flags (fail-honest RMS$_PLG, this
 *      rung reads uncompressed keys/records only), then
 *        a. descend the index: from the root bucket, at each level pick the
 *           first index record whose high-key >= the search key and follow its
 *           2-byte child pointer, until level 0 (a data bucket) is reached.
 *           A single-level index (Root Level 1) descends exactly once.
 *        b. scan the data bucket's records (offset 0x0E .. free-space offset),
 *           skipping IRC$V_DELETED, extracting the embedded key at the key
 *           descriptor's seg-0 position, and returning the record whose key
 *           satisfies the comparison. For KGE/KGT the scan follows the
 *           horizontal next-bucket chain to find the next key up.
 *
 * All block reads ride rms_io_lseek + rms_io_read_exact (rms_io.h), which is
 * IO$_READVBLK on the ACP channel window when /dev/vms is present (Rule 9).
 */

#include <stdlib.h>
#include <string.h>

#include "rms_prolog3.h"
#include "rmsdef.h"
#include "ssdef.h"

/* Read `nblk` consecutive 512-byte blocks starting at VBN `vbn` (1-based) into
 * buf. Returns 0 on success, -1 on short read / I/O error. */
static int p3_read_blocks(rms_file_t *f, uint32_t vbn, uint32_t nblk,
                          uint8_t *buf)
{
    off_t byte = (off_t)(vbn - 1u) * (off_t)P3_BLK;
    size_t want = (size_t)nblk * P3_BLK;
    if (rms_io_lseek(f, byte, 0 /*SEEK_SET*/) == (off_t)-1)
        return -1;
    if ((size_t)rms_io_read_exact(f, buf, want) != want)
        return -1;
    return 0;
}

/* Compare two byte keys (unsigned, lexicographic over the shorter length, then
 * by length). Matches string-key ordering; a data-type-aware compare
 * (big-endian integer keys, etc.) is a labelled follow-on. Returns <0, 0, >0. */
static int p3_keycmp(const uint8_t *a, uint16_t alen,
                     const uint8_t *b, uint16_t blen)
{
    uint16_t n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c != 0)
        return c;
    if (alen == blen)
        return 0;
    return alen < blen ? -1 : 1;
}

/* Parse one 102-byte key descriptor at buf into *kd. */
static void p3_parse_keydesc(const uint8_t *buf, p3_keydesc_t *kd)
{
    kd->ref            = buf[P3_KD_OFF_REF];
    kd->dtp            = buf[P3_KD_OFF_DTP];
    kd->flags          = p3_le16(buf + P3_KD_OFF_FLAGS);
    kd->root_level     = buf[P3_KD_OFF_ROOT_LEVEL];
    kd->ibs            = buf[P3_KD_OFF_IBS];
    kd->dbs            = buf[P3_KD_OFF_DBS];
    kd->nseg           = buf[P3_KD_OFF_NSEG];
    kd->root_vbn       = p3_le32(buf + P3_KD_OFF_ROOT_VBN);
    kd->first_data_vbn = p3_le32(buf + P3_KD_OFF_FIRST_DATAVBN);
    kd->key_size       = p3_le16(buf + P3_KD_OFF_KEY_SIZE);
    kd->min_rec_size   = p3_le16(buf + P3_KD_OFF_MIN_REC_SIZE);
    kd->seg0_pos       = p3_le16(buf + P3_KD_OFF_SEG0_POS);
    kd->seg0_siz       = buf[P3_KD_OFF_SEG0_SIZ];
}

uint32_t rms_p3_bind(rms_file_t *f, p3_ctx_t **out)
{
    uint8_t vbn1[P3_BLK];
    p3_ctx_t *ctx;
    uint16_t version, num_keys, first_key_off;
    uint32_t kd_vbn;
    uint16_t kd_off;
    uint8_t k;

    if (!f || !out)
        return RMS$_PLG;
    *out = NULL;

    /* VBN 1: fixed prolog + key-descriptor array. */
    if (p3_read_blocks(f, P3_VBN_FIXED_PROLOG, 1u, vbn1) != 0)
        return RMS$_RER;

    version = p3_le16(vbn1 + P3_FP_OFF_VERSION);
    if (version != P3_PROLOG_VERSION)
        return RMS$_PLG;                 /* not a Prolog-3 file: fail honest */

    num_keys      = p3_le16(vbn1 + P3_FP_OFF_NUM_KEYS);
    first_key_off = p3_le16(vbn1 + P3_FP_OFF_FIRST_KEYOFF);
    if (num_keys == 0 || num_keys > P3_MAX_KEYS)
        return RMS$_PLG;
    if (first_key_off == 0 || first_key_off >= P3_BLK)
        return RMS$_PLG;

    ctx = (p3_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return RMS$_DME;
    ctx->magic     = P3_CTX_MAGIC;
    ctx->f         = f;
    ctx->version   = version;
    ctx->num_keys  = num_keys;
    ctx->num_areas = p3_le16(vbn1 + P3_FP_OFF_NUM_AREAS);

    /* Walk the key-descriptor chain. Key descriptor #0 is at VBN 1,
     * first_key_off; each carries {next VBN, next offset} to the next. */
    kd_vbn = P3_VBN_FIXED_PROLOG;
    kd_off = first_key_off;
    for (k = 0; k < num_keys; k++) {
        uint8_t kdblk[P3_BLK];
        const uint8_t *kdp;

        if (kd_vbn == P3_VBN_FIXED_PROLOG) {
            kdp = vbn1;                  /* already have VBN 1 */
        } else {
            if (p3_read_blocks(f, kd_vbn, 1u, kdblk) != 0) {
                free(ctx);
                return RMS$_RER;
            }
            kdp = kdblk;
        }
        if ((size_t)kd_off + P3_KEYDESC_SIZE > P3_BLK) {
            free(ctx);
            return RMS$_PLG;
        }
        p3_parse_keydesc(kdp + kd_off, &ctx->keys[k]);

        /* advance to the next descriptor */
        kd_vbn = p3_le16(kdp + kd_off + P3_KD_OFF_NEXT_VBN);
        kd_off = p3_le16(kdp + kd_off + P3_KD_OFF_NEXT_OFF);
        if (kd_vbn == 0)
            break;                       /* chain terminated early */
    }

    *out = ctx;
    return RMS$_NORMAL;
}

void rms_p3_free(p3_ctx_t *ctx)
{
    if (ctx) {
        ctx->magic = 0;
        free(ctx);
    }
}

/* Locate the key descriptor for key of reference `krf`. */
static const p3_keydesc_t *p3_find_key(const p3_ctx_t *ctx, uint8_t krf)
{
    uint16_t i;
    for (i = 0; i < ctx->num_keys; i++)
        if (ctx->keys[i].ref == krf)
            return &ctx->keys[i];
    /* fall back to positional (files whose ref==index) */
    if (krf < ctx->num_keys)
        return &ctx->keys[krf];
    return NULL;
}

/*
 * Descend the index from the root to the data bucket that would hold `key`.
 * Returns the data-bucket VBN in *data_vbn, or 0-status on error.
 * Bucket buffer `bkt` must be at least max(ibs,dbs)*512 bytes.
 */
static uint32_t p3_descend(const p3_ctx_t *ctx, const p3_keydesc_t *kd,
                           const uint8_t *key, uint16_t key_len,
                           uint8_t *bkt, uint32_t *data_vbn)
{
    uint32_t vbn = kd->root_vbn;
    int guard;

    /* Root Level 1 means the root IS an index bucket one hop above the data
     * level; descend until we read a level-0 (data) bucket. Guard against a
     * malformed cyclic chain. */
    for (guard = 0; guard < 64; guard++) {
        uint8_t level, flags;
        uint16_t free_off, off;
        uint32_t chosen = 0;
        int chose = 0;

        if (p3_read_blocks(ctx->f, vbn, kd->ibs ? kd->ibs : 1u, bkt) != 0)
            return RMS$_RER;

        level    = bkt[P3_BH_OFF_LEVEL];
        flags    = bkt[P3_BH_OFF_FLAGS];
        free_off = p3_le16(bkt + P3_BH_OFF_FREESPACE);
        (void)flags;

        if (level == 0) {
            *data_vbn = vbn;             /* reached the data level */
            return RMS$_NORMAL;
        }

        /* Index bucket: entries are {u16 child_vbn, key_size high-key}. Pick
         * the first entry whose high-key >= search key; else the last entry
         * (search key is above every separator -> rightmost subtree). */
        if (free_off < P3_BKT_HDR_SIZE || free_off > (kd->ibs ? kd->ibs : 1u) * P3_BLK)
            return RMS$_PLG;

        off = P3_BKT_HDR_SIZE;
        while ((uint16_t)(off + P3_IDXREC_PTR_SIZE + kd->key_size) <= free_off) {
            uint32_t child = p3_le16(bkt + off);   /* 2-byte pointer [PIN] */
            const uint8_t *hk = bkt + off + P3_IDXREC_PTR_SIZE;
            chosen = child;
            chose = 1;
            if (p3_keycmp(hk, kd->key_size, key, key_len) >= 0)
                break;                              /* high-key >= search key */
            off = (uint16_t)(off + P3_IDXREC_PTR_SIZE + kd->key_size);
        }
        if (!chose)
            return RMS$_RNF;                        /* empty index bucket */
        vbn = chosen;
    }
    return RMS$_PLG;                                /* descent overran (cyclic) */
}

/*
 * Scan a data bucket (already read into `bkt`, dbs blocks) for the record whose
 * embedded key matches per the comparison. On an exact/KGE/KGT hit, sets
 * *found_off to the record offset and returns 1; returns 0 if no qualifying
 * record in THIS bucket. `best_*` track the smallest qualifying key so far
 * (for KGE/KGT across buckets).
 */
static int p3_scan_data_bucket(const p3_keydesc_t *kd, const uint8_t *bkt,
                               uint16_t free_off, const uint8_t *key,
                               uint16_t key_len, int rop_kge, int rop_kgt,
                               uint16_t *found_off)
{
    uint16_t off = P3_BKT_HDR_SIZE;
    int best = 0;
    uint16_t best_off = 0;
    uint8_t  best_key[256];
    uint16_t best_klen = 0;

    while ((uint16_t)(off + P3_DR_HDR_SIZE) <= free_off) {
        uint8_t  ctrl = bkt[off + P3_DR_OFF_CTRL];
        uint16_t dlen = p3_le16(bkt + off + P3_DR_OFF_DATALEN);
        uint16_t rec_next = (uint16_t)(off + P3_DR_HDR_SIZE + dlen);
        const uint8_t *rd = bkt + off + P3_DR_HDR_SIZE;
        int deleted = (ctrl >> P3_IRCV_DELETED) & 1u;

        if (rec_next > free_off)
            break;                                 /* malformed / truncated */

        if (!deleted && kd->seg0_siz > 0 &&
            (uint32_t)kd->seg0_pos + kd->seg0_siz <= dlen) {
            const uint8_t *ek = rd + kd->seg0_pos;  /* embedded key */
            uint16_t eklen = kd->seg0_siz;
            int c = p3_keycmp(ek, eklen, key, key_len);
            int qualifies = 0;

            if (rop_kgt)       qualifies = (c > 0);
            else if (rop_kge)  qualifies = (c >= 0);
            else               qualifies = (c == 0);

            if (qualifies) {
                if (!rop_kge && !rop_kgt) {
                    *found_off = off;               /* exact: first hit wins */
                    return 1;
                }
                /* KGE/KGT: keep the smallest qualifying key */
                if (!best || p3_keycmp(ek, eklen, best_key, best_klen) < 0) {
                    best = 1;
                    best_off = off;
                    best_klen = eklen < sizeof(best_key) ? eklen : sizeof(best_key);
                    memcpy(best_key, ek, best_klen);
                }
            }
        }
        off = rec_next;
    }

    if (best) {
        *found_off = best_off;
        return 1;
    }
    return 0;
}

/* Shared resolver for get/find: returns the record offset within a freshly
 * read data bucket, leaving that bucket in *bkt with its free-space offset in
 * *free_off. */
static uint32_t p3_resolve(p3_ctx_t *ctx, uint8_t krf,
                           const uint8_t *key, uint16_t key_len,
                           int rop_kge, int rop_kgt,
                           uint8_t *bkt, uint16_t *free_off,
                           uint16_t *rec_off)
{
    const p3_keydesc_t *kd = p3_find_key(ctx, krf);
    uint32_t data_vbn = 0, st;
    uint32_t dbs;
    int guard;

    if (!kd)
        return RMS$_KEY;
    if (!key || key_len == 0)
        return RMS$_KEY;
    /* This rung reads uncompressed keys/records only. A compression flag means
     * the byte stream is front/rear compressed -- fail honest, do NOT misread
     * (INV-6). Compression decode is a labelled follow-on rung. */
    if (kd->flags & P3_KEYM_ANY_COMPR)
        return RMS$_PLG;
    if (kd->key_size == 0 || kd->key_size > 255)
        return RMS$_PLG;
    if ((kd->ibs ? kd->ibs : 1u) > P3_MAX_BKT_BLOCKS ||
        (kd->dbs ? kd->dbs : 1u) > P3_MAX_BKT_BLOCKS)
        return RMS$_PLG;

    dbs = kd->dbs ? kd->dbs : 1u;

    st = p3_descend(ctx, kd, key, key_len, bkt, &data_vbn);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;

    /* Scan the located data bucket, then follow the horizontal chain for
     * KGE/KGT (the next key up may live in a following bucket). For an exact
     * match we stop at the located bucket. */
    for (guard = 0; guard < 65536; guard++) {
        uint16_t fo;
        uint32_t next_vbn;

        if (p3_read_blocks(ctx->f, data_vbn, dbs, bkt) != 0)
            return RMS$_RER;
        if (bkt[P3_BH_OFF_LEVEL] != 0)
            return RMS$_PLG;
        fo = p3_le16(bkt + P3_BH_OFF_FREESPACE);
        if (fo < P3_BKT_HDR_SIZE || fo > dbs * P3_BLK)
            return RMS$_PLG;

        if (p3_scan_data_bucket(kd, bkt, fo, key, key_len,
                                rop_kge, rop_kgt, rec_off)) {
            *free_off = fo;
            return RMS$_NORMAL;
        }

        if (!rop_kge && !rop_kgt)
            return RMS$_RNF;             /* exact match not in target bucket */

        next_vbn = p3_le32(bkt + P3_BH_OFF_NEXT_VBN);
        if (next_vbn == 0 || next_vbn == data_vbn)
            return RMS$_RNF;
        data_vbn = next_vbn;            /* KGE/KGT: try the next data bucket */
    }
    return RMS$_RNF;
}

uint32_t rms_p3_get_by_key(p3_ctx_t *ctx, uint8_t krf,
                           const uint8_t *key, uint16_t key_len,
                           int rop_kge, int rop_kgt,
                           uint8_t *buf, uint16_t buf_sz, uint16_t *rec_len)
{
    uint8_t *bkt;
    uint16_t free_off = 0, rec_off = 0, dlen;
    uint32_t st;

    if (!ctx || ctx->magic != P3_CTX_MAGIC || !buf || !rec_len)
        return RMS$_PLG;

    bkt = (uint8_t *)malloc((size_t)P3_MAX_BKT_BLOCKS * P3_BLK);
    if (!bkt)
        return RMS$_DME;

    st = p3_resolve(ctx, krf, key, key_len, rop_kge, rop_kgt,
                    bkt, &free_off, &rec_off);
    if (!$VMS_STATUS_SUCCESS(st)) {
        free(bkt);
        return st;
    }

    dlen = p3_le16(bkt + rec_off + P3_DR_OFF_DATALEN);
    *rec_len = dlen;
    if (dlen > buf_sz) {
        free(bkt);
        return RMS$_RTB;                /* *rec_len carries the true size */
    }
    if (dlen > 0)
        memcpy(buf, bkt + rec_off + P3_DR_HDR_SIZE, dlen);
    free(bkt);
    return RMS$_NORMAL;
}

uint32_t rms_p3_find_by_key(p3_ctx_t *ctx, uint8_t krf,
                            const uint8_t *key, uint16_t key_len,
                            int rop_kge, int rop_kgt, uint16_t *rec_len)
{
    uint8_t *bkt;
    uint16_t free_off = 0, rec_off = 0;
    uint32_t st;

    if (!ctx || ctx->magic != P3_CTX_MAGIC || !rec_len)
        return RMS$_PLG;

    bkt = (uint8_t *)malloc((size_t)P3_MAX_BKT_BLOCKS * P3_BLK);
    if (!bkt)
        return RMS$_DME;

    st = p3_resolve(ctx, krf, key, key_len, rop_kge, rop_kgt,
                    bkt, &free_off, &rec_off);
    if (!$VMS_STATUS_SUCCESS(st)) {
        free(bkt);
        return st;
    }

    *rec_len = p3_le16(bkt + rec_off + P3_DR_OFF_DATALEN);
    free(bkt);
    return RMS$_NORMAL;
}
