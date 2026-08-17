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
    /* [OVMX] write high-water: a file the writer authored carries the next free
     * VBN here so a reopened file resumes $PUT allocation. Zero on a read-only
     * image (the reader never allocates), harmless. */
    ctx->alloc_next = p3_le32(vbn1 + P3_FP_OFF_ALLOC_NEXT);

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
        /* An RRV stub (IRC$V_RRV) left behind by a bucket split carries no key
         * or data -- only a forward pointer for RFA stability. Keyed scan skips
         * it (its datalen is 0 anyway, but skip explicitly so the writer's RRV
         * maintenance and the reader agree). */
        int is_rrv  = (ctrl >> P3_IRCV_RRV) & 1u;

        if (rec_next > free_off)
            break;                                 /* malformed / truncated */

        if (!deleted && !is_rrv && kd->seg0_siz > 0 &&
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

/*
 * ============================================================================
 * WRITE ENGINE (vms-045) -- $CREATE / $PUT (insert + bucket SPLIT + RRV) /
 * $UPDATE. Authors the genuine Files-11 Prolog-3 index over the ACP window via
 * rms_io_write_exact (IO$_WRITEVBLK when /dev/vms is present, Rule 9/INV-6).
 * Emits EXACTLY the byte layout rms_p3_get_by_key parses above -- same
 * [PIN]/[OVMX] offsets -- so writer and reader round-trip across a split.
 *
 * SCOPE (smallest genuine increment; the rest is fail-honest, never mis-written)
 *   Single primary key, uncompressed keys/records, a SINGLE-LEVEL index (the
 *   root, Root Level 1) over data buckets that SPLIT genuinely: a full data
 *   bucket is split into two, a new bucket is allocated, records are
 *   redistributed by key, an RRV (record-reference-vector) stub is left in the
 *   original bucket for each moved record (RFA stability, IRC$V_RRV), and the
 *   parent index bucket gains a 2-byte child pointer + high-key for the new
 *   bucket. If the single index bucket itself fills (the file would need a
 *   SECOND index level), $PUT fails honest with RMS$_ORG -- multi-level index
 *   growth is the labelled follow-on rung, NOT a silent mis-write (INV-6).
 *
 * INVARIANTS this engine maintains, and the reader relies on:
 *   - A data bucket body is [sorted live records][RRV stubs]. Stubs (datalen 0,
 *     IRC$V_RRV set) always sit at the tail; the reader skips them.
 *   - Records are physically key-sorted WITHIN a data bucket, and key-disjoint
 *     ACROSS buckets, partitioned by the index high-keys. $UPDATE's delete
 *     COMPACTS (no tombstone holes), so the live region is contiguous+sorted.
 *   - Each index entry's high-key == the maximum live key in the bucket it
 *     points at; the descent picks the first entry whose high-key >= search key.
 *
 * Substrate-agnostic (vms-5f0): fixed-width little-endian accessors only, no
 * per-substrate ifdef, no native-width serialized field -- VAX ILP32 and
 * Alpha/x86_64 LP64 compile and run this file identically.
 */

/* Write `nblk` consecutive 512-byte blocks starting at VBN `vbn` from buf. */
static int p3_write_blocks(rms_file_t *f, uint32_t vbn, uint32_t nblk,
                           const uint8_t *buf)
{
    off_t byte = (off_t)(vbn - 1u) * (off_t)P3_BLK;
    size_t want = (size_t)nblk * P3_BLK;
    if (rms_io_lseek(f, byte, 0 /*SEEK_SET*/) == (off_t)-1)
        return -1;
    if (rms_io_write_exact(f, buf, want) != 0)
        return -1;
    return 0;
}

/* Lay down a 14-byte bucket header. */
static void p3_set_bkt_header(uint8_t *bkt, uint8_t level, uint8_t flags,
                              uint16_t free_off, uint16_t free_recid,
                              uint32_t next_vbn)
{
    bkt[P3_BH_OFF_CHECK] = 0x4B;   /* [OVMX] check byte -- algo unpinned (oracle
                                    * §6: a valid byte RMS accepts, not derived) */
    bkt[P3_BH_OFF_KOR]   = 0;
    bkt[P3_BH_OFF_LEVEL] = level;
    bkt[P3_BH_OFF_FLAGS] = flags;
    p3_put_le16(bkt + P3_BH_OFF_FREESPACE, free_off);
    p3_put_le16(bkt + P3_BH_OFF_FREE_RECID, free_recid);
    p3_put_le16(bkt + P3_BH_OFF_VBN_SAMPLE, 0);
    p3_put_le32(bkt + P3_BH_OFF_NEXT_VBN, next_vbn);
}

/* Persist the allocation high-water into the fixed prolog (read-modify-write so
 * the key descriptor sharing VBN 1 is preserved). */
static int p3_flush_alloc_next(p3_ctx_t *ctx)
{
    uint8_t vbn1[P3_BLK];
    if (p3_read_blocks(ctx->f, P3_VBN_FIXED_PROLOG, 1u, vbn1) != 0)
        return -1;
    p3_put_le32(vbn1 + P3_FP_OFF_ALLOC_NEXT, ctx->alloc_next);
    return p3_write_blocks(ctx->f, P3_VBN_FIXED_PROLOG, 1u, vbn1);
}

uint32_t rms_p3_create(rms_file_t *f, const p3_create_params_t *p,
                       p3_ctx_t **out)
{
    p3_ctx_t *ctx;
    uint8_t  *blk;
    uint8_t   B;
    uint32_t  first_data, root_vbn;
    p3_keydesc_t *kd;

    if (!f || !p || !out)
        return RMS$_KEY;
    *out = NULL;
    if (p->key_size == 0 || p->key_size > 255)
        return RMS$_KEY;
    if (p->seg0_siz == 0 || p->seg0_siz > p->key_size)
        return RMS$_KEY;
    B = p->bkt_blocks ? p->bkt_blocks : 1u;
    if (B > P3_MAX_BKT_BLOCKS)
        return RMS$_KEY;
    /* An index entry (2-byte ptr + full key) must fit a fresh index bucket. */
    if ((uint32_t)P3_BKT_HDR_SIZE + P3_IDXREC_PTR_SIZE + p->key_size >
        (uint32_t)B * P3_BLK)
        return RMS$_KEY;

    /* Static layout [OVMX]: VBN1 fixed prolog + key desc; VBN2 reserved (VMS
     * chains further key descriptors here -- unused, single key); VBN3 area
     * descriptor; then the first data bucket, then the root index bucket. */
    first_data = 4u;
    root_vbn   = 4u + B;

    ctx = (p3_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return RMS$_DME;
    ctx->magic      = P3_CTX_MAGIC;
    ctx->f          = f;
    ctx->version    = P3_PROLOG_VERSION;
    ctx->num_keys   = 1;
    ctx->num_areas  = 1;
    ctx->writable   = 1;
    ctx->alloc_next = 4u + 2u * B;      /* next free VBN after the two buckets */

    kd = &ctx->keys[0];
    kd->ref            = 0;
    kd->dtp            = p->dtp;
    kd->flags          = p->allow_dup ? (uint16_t)(1u << P3_KEYV_DUPKEYS) : 0u;
    kd->root_level     = 1;
    kd->ibs            = B;
    kd->dbs            = B;
    kd->nseg           = 1;
    kd->root_vbn       = root_vbn;
    kd->first_data_vbn = first_data;
    kd->key_size       = p->key_size;
    kd->min_rec_size   = (uint16_t)(p->seg0_pos + p->seg0_siz);
    kd->seg0_pos       = p->seg0_pos;
    kd->seg0_siz       = p->seg0_siz;

    blk = (uint8_t *)calloc(1, (size_t)B * P3_BLK);
    if (!blk) { free(ctx); return RMS$_DME; }

    /* ---- VBN 1: fixed prolog + key descriptor #0 ---- */
    memset(blk, 0, P3_BLK);
    p3_put_le16(blk + P3_FP_OFF_VERSION,     P3_PROLOG_VERSION);   /* [PIN] 3 */
    p3_put_le16(blk + P3_FP_OFF_NUM_KEYS,    1);
    p3_put_le16(blk + P3_FP_OFF_NUM_AREAS,   1);
    p3_put_le16(blk + P3_FP_OFF_FIRST_AREAVBN, P3_VBN_AREA_DESC);  /* [PIN] 3 */
    p3_put_le16(blk + P3_FP_OFF_FIRST_KEYOFF,  P3_FP_HDR_SIZE);
    p3_put_le32(blk + P3_FP_OFF_ALLOC_NEXT,  ctx->alloc_next);
    {
        uint8_t *k = blk + P3_FP_HDR_SIZE;
        p3_put_le16(k + P3_KD_OFF_NEXT_VBN, 0);      /* single key: chain ends */
        p3_put_le16(k + P3_KD_OFF_NEXT_OFF, 0);
        k[P3_KD_OFF_REF]        = 0;
        k[P3_KD_OFF_DTP]        = p->dtp;
        p3_put_le16(k + P3_KD_OFF_FLAGS, kd->flags);
        k[P3_KD_OFF_ROOT_LEVEL] = 1;
        k[P3_KD_OFF_IBS]        = B;
        k[P3_KD_OFF_DBS]        = B;
        k[P3_KD_OFF_NSEG]       = 1;
        p3_put_le32(k + P3_KD_OFF_ROOT_VBN,      root_vbn);
        p3_put_le32(k + P3_KD_OFF_FIRST_DATAVBN, first_data);
        p3_put_le16(k + P3_KD_OFF_KEY_SIZE,      p->key_size);
        p3_put_le16(k + P3_KD_OFF_MIN_REC_SIZE,  kd->min_rec_size);
        p3_put_le16(k + P3_KD_OFF_SEG0_POS,      p->seg0_pos);
        k[P3_KD_OFF_SEG0_SIZ]   = p->seg0_siz;
        memcpy(k + P3_KD_OFF_NAME, "PRIMARY", 7);
    }
    if (p3_write_blocks(f, P3_VBN_FIXED_PROLOG, 1u, blk) != 0) goto wpl;

    /* ---- VBN 2: reserved (zeroed) ---- */
    memset(blk, 0, P3_BLK);
    if (p3_write_blocks(f, 2u, 1u, blk) != 0) goto wpl;

    /* ---- VBN 3: area descriptor #0 (64 bytes [PIN]) ---- */
    memset(blk, 0, P3_BLK);
    blk[0] = B;                                  /* bucket size */
    p3_put_le32(blk + 8, ctx->alloc_next);       /* [OVMX] extent-next mirror */
    if (p3_write_blocks(f, P3_VBN_AREA_DESC, 1u, blk) != 0) goto wpl;

    /* ---- first data bucket: empty, level 0, LASTBKT ---- */
    memset(blk, 0, (size_t)B * P3_BLK);
    p3_set_bkt_header(blk, 0, (uint8_t)(1u << P3_BKTV_LASTBKT),
                      P3_BKT_HDR_SIZE, 1, 0);
    if (p3_write_blocks(f, first_data, B, blk) != 0) goto wpl;

    /* ---- root index bucket: one entry {first_data, high-key = 0}; the
     * high-key grows to the bucket's true max on the first $PUT ---- */
    memset(blk, 0, (size_t)B * P3_BLK);
    {
        uint16_t off = P3_BKT_HDR_SIZE;
        p3_put_le16(blk + off, (uint16_t)first_data);            /* 2-byte ptr */
        memset(blk + off + P3_IDXREC_PTR_SIZE, 0, p->key_size);  /* high-key 0 */
        off = (uint16_t)(off + P3_IDXREC_PTR_SIZE + p->key_size);
        p3_set_bkt_header(blk, 1,
                          (uint8_t)((1u << P3_BKTV_ROOTBKT) |
                                    (1u << P3_BKTV_LASTBKT)),
                          off, 1, root_vbn);
    }
    if (p3_write_blocks(f, root_vbn, B, blk) != 0) goto wpl;

    free(blk);
    *out = ctx;
    return RMS$_CREATED;

wpl:
    free(blk);
    free(ctx);
    return RMS$_WPL;
}

/* Total on-disk size of the record at `off` (header + data). */
static uint16_t p3_rec_size(const uint8_t *bkt, uint16_t off)
{
    return (uint16_t)(P3_DR_HDR_SIZE + p3_le16(bkt + off + P3_DR_OFF_DATALEN));
}

/* Locate, in the (already read) single-level root index bucket `root`, the
 * entry that routes `key`: the first entry whose high-key >= key, else the last
 * entry (key is above every separator -> rightmost bucket). Returns the entry
 * byte offset in *entry_off and its 2-byte child VBN in *child. */
static uint32_t p3_index_locate(const p3_keydesc_t *kd, const uint8_t *root,
                                const uint8_t *key, uint16_t key_len,
                                uint16_t *entry_off, uint32_t *child)
{
    uint16_t ibs_cap = (uint16_t)((kd->ibs ? kd->ibs : 1u) * P3_BLK);
    uint16_t stride  = (uint16_t)(P3_IDXREC_PTR_SIZE + kd->key_size);
    uint16_t free_off = p3_le16(root + P3_BH_OFF_FREESPACE);
    uint16_t off, last_off = 0;
    uint32_t last_child = 0;
    int have = 0;

    if (root[P3_BH_OFF_LEVEL] != 1)
        return RMS$_ORG;                 /* not a single-level index (deferred) */
    if (free_off < P3_BKT_HDR_SIZE || free_off > ibs_cap)
        return RMS$_PLG;

    for (off = P3_BKT_HDR_SIZE; (uint16_t)(off + stride) <= free_off;
         off = (uint16_t)(off + stride)) {
        uint32_t c = p3_le16(root + off);
        const uint8_t *hk = root + off + P3_IDXREC_PTR_SIZE;
        last_off = off; last_child = c; have = 1;
        if (p3_keycmp(hk, kd->key_size, key, key_len) >= 0) {
            *entry_off = off; *child = c;
            return RMS$_NORMAL;
        }
    }
    if (!have)
        return RMS$_PLG;                 /* empty index bucket: corrupt */
    *entry_off = last_off; *child = last_child;   /* above all -> rightmost */
    return RMS$_NORMAL;
}

/*
 * Split the FULL data bucket X (VBN `data_vbn`, image in `xold`, its routing
 * index entry at `entry_off` in `root`) into X + a freshly allocated bucket Y.
 * Redistributes the sorted live records by key, leaves an RRV stub in X for each
 * moved record, and inserts Y's {child, high-key} entry into `root`. Writes X,
 * Y and the root back. `root` is updated in place (caller re-reads next pass).
 * Returns RMS$_NORMAL, RMS$_ORG (index bucket full -> would need a 2nd level),
 * RMS$_RSZ (bucket held < 2 live records -> unsplittable), or an I/O status.
 */
static uint32_t p3_split_data_bucket(p3_ctx_t *ctx, const p3_keydesc_t *kd,
                                     uint8_t *root, uint16_t entry_off,
                                     uint32_t data_vbn, const uint8_t *xold)
{
    uint32_t cap    = (kd->dbs ? kd->dbs : 1u) * P3_BLK;
    uint16_t istr   = (uint16_t)(P3_IDXREC_PTR_SIZE + kd->key_size);
    uint16_t icap   = (uint16_t)((kd->ibs ? kd->ibs : 1u) * P3_BLK);
    uint16_t rfree  = p3_le16(root + P3_BH_OFF_FREESPACE);
    uint16_t xfree  = p3_le16(xold + P3_BH_OFF_FREESPACE);
    uint8_t  xflags = xold[P3_BH_OFF_FLAGS];
    uint32_t xnext  = p3_le32(xold + P3_BH_OFF_NEXT_VBN);
    uint16_t xrecid = p3_le16(xold + P3_BH_OFF_FREE_RECID);
    uint32_t vy;
    uint16_t *live = NULL, *stub = NULL;
    uint16_t nlive = 0, nstub = 0, i, off;
    uint16_t maxrec = (uint16_t)(cap / P3_DR_HDR_SIZE + 1);
    uint8_t  *xbuf = NULL, *ybuf = NULL;
    uint16_t split_idx, xo, yo, yid;
    uint32_t st = RMS$_NORMAL;

    /* Index room for one more entry FIRST -- fail honest before any mutation. */
    if ((uint32_t)rfree + istr > icap)
        return RMS$_ORG;                 /* 2-level index growth: deferred rung */

    live = (uint16_t *)malloc((size_t)maxrec * sizeof(uint16_t));
    stub = (uint16_t *)malloc((size_t)maxrec * sizeof(uint16_t));
    xbuf = (uint8_t  *)malloc((size_t)cap);
    ybuf = (uint8_t  *)malloc((size_t)cap);
    if (!live || !stub || !xbuf || !ybuf) { st = RMS$_DME; goto done; }

    /* Enumerate X: live records (sorted, at the front) then RRV stubs (tail). */
    for (off = P3_BKT_HDR_SIZE; (uint16_t)(off + P3_DR_HDR_SIZE) <= xfree; ) {
        uint8_t ctrl = xold[off + P3_DR_OFF_CTRL];
        uint16_t rn  = (uint16_t)(off + p3_rec_size(xold, off));
        if (rn > xfree) break;
        if ((ctrl >> P3_IRCV_RRV) & 1u) {
            if (nstub < maxrec) stub[nstub++] = off;
        } else if (!((ctrl >> P3_IRCV_DELETED) & 1u)) {
            if (nlive < maxrec) live[nlive++] = off;
        }
        off = rn;
    }
    if (nlive < 2) { st = RMS$_RSZ; goto done; }   /* can't split one record */

    split_idx = (uint16_t)(nlive / 2);             /* lower [0,split) stay in X */

    vy = ctx->alloc_next;
    ctx->alloc_next += (kd->dbs ? kd->dbs : 1u);

    /* ---- build Y: moved records [split_idx, nlive) with fresh record IDs ---- */
    memset(ybuf, 0, (size_t)cap);
    yo = P3_BKT_HDR_SIZE; yid = 1;
    for (i = split_idx; i < nlive; i++) {
        uint16_t so = live[i];
        uint16_t rs = p3_rec_size(xold, so);
        memcpy(ybuf + yo, xold + so, rs);
        ybuf[yo + P3_DR_OFF_CTRL] = 0;
        p3_put_le16(ybuf + yo + P3_DR_OFF_RECID, yid);
        p3_put_le16(ybuf + yo + P3_DR_OFF_RRVID, yid);
        p3_put_le32(ybuf + yo + P3_DR_OFF_RRVPTR, vy);   /* home bucket = self */
        yo = (uint16_t)(yo + rs);
        yid++;
    }
    p3_set_bkt_header(ybuf, 0,
                      (uint8_t)(xflags & (1u << P3_BKTV_LASTBKT)),
                      yo, yid, xnext);

    /* ---- rebuild X: retained live [0,split_idx) + old stubs + new stubs ---- */
    memset(xbuf, 0, (size_t)cap);
    xo = P3_BKT_HDR_SIZE;
    for (i = 0; i < split_idx; i++) {
        uint16_t so = live[i];
        uint16_t rs = p3_rec_size(xold, so);
        memcpy(xbuf + xo, xold + so, rs);
        xo = (uint16_t)(xo + rs);
    }
    for (i = 0; i < nstub; i++) {           /* preserve prior RRV stubs */
        uint16_t so = stub[i];
        memcpy(xbuf + xo, xold + so, P3_DR_HDR_SIZE);
        p3_put_le16(xbuf + xo + P3_DR_OFF_DATALEN, 0);
        xo = (uint16_t)(xo + P3_DR_HDR_SIZE);
    }
    for (i = split_idx; i < nlive; i++) {   /* new stub per moved record */
        uint16_t so = live[i];
        uint16_t oldid = p3_le16(xold + so + P3_DR_OFF_RECID);
        uint16_t newid = (uint16_t)(i - split_idx + 1);
        xbuf[xo + P3_DR_OFF_CTRL] = (uint8_t)(1u << P3_IRCV_RRV);
        p3_put_le16(xbuf + xo + P3_DR_OFF_RECID, oldid);   /* RFA stays stable */
        p3_put_le16(xbuf + xo + P3_DR_OFF_RRVID, newid);   /* -> new id in Y */
        p3_put_le32(xbuf + xo + P3_DR_OFF_RRVPTR, vy);     /* -> Y bucket */
        p3_put_le16(xbuf + xo + P3_DR_OFF_DATALEN, 0);
        xo = (uint16_t)(xo + P3_DR_HDR_SIZE);
    }
    p3_set_bkt_header(xbuf, 0,
                      (uint8_t)(xflags & ~(1u << P3_BKTV_LASTBKT)),
                      xo, xrecid, vy);      /* X now chains horizontally to Y */

    /* ---- index maintenance: X's entry high-key = X's new max; insert Y ---- */
    {
        const uint8_t *xhigh = xold + live[split_idx - 1] + P3_DR_HDR_SIZE +
                               kd->seg0_pos;               /* new max in X */
        const uint8_t *yhigh = xold + live[nlive - 1] + P3_DR_HDR_SIZE +
                               kd->seg0_pos;               /* max overall -> Y */
        uint16_t pos = (uint16_t)(entry_off + istr);       /* just after X's */
        uint8_t *xe = root + entry_off + P3_IDXREC_PTR_SIZE;
        uint8_t *ye = root + pos + P3_IDXREC_PTR_SIZE;
        uint16_t padlo = (uint16_t)(kd->key_size - kd->seg0_siz);
        /* shift the tail up to open a slot, then write {vy, yhigh}. The high-key
         * is the record's seg-0 key (seg0_siz bytes), zero-padded to key_size --
         * the same encoding the insert path writes (a full-width index key). */
        memmove(root + pos + istr, root + pos, (size_t)(rfree - pos));
        memcpy(xe, xhigh, kd->seg0_siz);
        if (padlo) memset(xe + kd->seg0_siz, 0, padlo);
        p3_put_le16(root + pos, (uint16_t)vy);
        memcpy(ye, yhigh, kd->seg0_siz);
        if (padlo) memset(ye + kd->seg0_siz, 0, padlo);
        rfree = (uint16_t)(rfree + istr);
        p3_put_le16(root + P3_BH_OFF_FREESPACE, rfree);
    }

    /* ---- commit: Y, X, root, and the allocation high-water ---- */
    if (p3_write_blocks(ctx->f, vy, kd->dbs ? kd->dbs : 1u, ybuf) != 0 ||
        p3_write_blocks(ctx->f, data_vbn, kd->dbs ? kd->dbs : 1u, xbuf) != 0 ||
        p3_write_blocks(ctx->f, kd->root_vbn, kd->ibs ? kd->ibs : 1u, root) != 0 ||
        p3_flush_alloc_next(ctx) != 0) {
        st = RMS$_WPL;
        goto done;
    }

done:
    free(live); free(stub); free(xbuf); free(ybuf);
    return st;
}

uint32_t rms_p3_put(p3_ctx_t *ctx, uint8_t krf,
                    const uint8_t *rec, uint16_t rec_len)
{
    const p3_keydesc_t *kd;
    uint8_t *root = NULL, *data = NULL;
    uint32_t cap, icap, st = RMS$_NORMAL;
    const uint8_t *nkey;
    int attempt;

    if (!ctx || ctx->magic != P3_CTX_MAGIC || !ctx->writable || !rec)
        return RMS$_PLG;
    kd = p3_find_key(ctx, krf);
    if (!kd)
        return RMS$_KEY;
    if (kd->flags & P3_KEYM_ANY_COMPR)
        return RMS$_PLG;                 /* compression is a follow-on rung */
    if (kd->key_size == 0 || kd->key_size > 255)
        return RMS$_PLG;
    if ((uint32_t)kd->seg0_pos + kd->seg0_siz > rec_len)
        return RMS$_RSZ;                 /* record too short to carry the key */
    nkey = rec + kd->seg0_pos;

    cap  = (kd->dbs ? kd->dbs : 1u) * P3_BLK;
    icap = (kd->ibs ? kd->ibs : 1u) * P3_BLK;
    root = (uint8_t *)malloc((size_t)icap);
    data = (uint8_t *)malloc((size_t)cap);
    if (!root || !data) { st = RMS$_DME; goto done; }

    /* At most one split is needed to make room for one record; loop with a
     * small guard so a second split (rightmost cascade) is still handled. */
    for (attempt = 0; attempt < 4; attempt++) {
        uint16_t entry_off = 0, dfree, ins_off, need;
        uint32_t data_vbn = 0;
        uint16_t off, recid;

        if (p3_read_blocks(ctx->f, kd->root_vbn, kd->ibs ? kd->ibs : 1u, root) != 0) {
            st = RMS$_RER; goto done;
        }
        st = p3_index_locate(kd, root, nkey, kd->seg0_siz, &entry_off, &data_vbn);
        if (!$VMS_STATUS_SUCCESS(st)) goto done;

        if (p3_read_blocks(ctx->f, data_vbn, kd->dbs ? kd->dbs : 1u, data) != 0) {
            st = RMS$_RER; goto done;
        }
        if (data[P3_BH_OFF_LEVEL] != 0) { st = RMS$_PLG; goto done; }
        dfree = p3_le16(data + P3_BH_OFF_FREESPACE);
        if (dfree < P3_BKT_HDR_SIZE || dfree > cap) { st = RMS$_PLG; goto done; }

        /* duplicate check (allow_dup off) + sorted insertion offset (before the
         * first larger live key, or the first RRV stub -> end of live region) */
        ins_off = dfree;
        for (off = P3_BKT_HDR_SIZE; (uint16_t)(off + P3_DR_HDR_SIZE) <= dfree; ) {
            uint8_t  ctrl = data[off + P3_DR_OFF_CTRL];
            uint16_t rn   = (uint16_t)(off + p3_rec_size(data, off));
            int cmp;
            if (rn > dfree) break;
            if ((ctrl >> P3_IRCV_RRV) & 1u) { ins_off = off; break; }  /* stubs */
            if ((ctrl >> P3_IRCV_DELETED) & 1u) { off = rn; continue; }
            cmp = p3_keycmp(data + off + P3_DR_HDR_SIZE + kd->seg0_pos,
                            kd->seg0_siz, nkey, kd->seg0_siz);
            if (cmp == 0 && !(kd->flags & (1u << P3_KEYV_DUPKEYS))) {
                st = RMS$_DUP; goto done;
            }
            if (cmp > 0) { ins_off = off; break; }
            off = rn;
        }

        need = (uint16_t)(P3_DR_HDR_SIZE + rec_len);
        if ((uint32_t)dfree + need <= cap) {
            /* ---- insert in place (open a gap, write the record) ---- */
            recid = p3_le16(data + P3_BH_OFF_FREE_RECID);
            memmove(data + ins_off + need, data + ins_off,
                    (size_t)(dfree - ins_off));
            data[ins_off + P3_DR_OFF_CTRL] = 0;
            p3_put_le16(data + ins_off + P3_DR_OFF_RECID, recid);
            p3_put_le16(data + ins_off + P3_DR_OFF_RRVID, recid);
            p3_put_le32(data + ins_off + P3_DR_OFF_RRVPTR, data_vbn); /* self */
            p3_put_le16(data + ins_off + P3_DR_OFF_DATALEN, rec_len);
            memcpy(data + ins_off + P3_DR_HDR_SIZE, rec, rec_len);
            dfree = (uint16_t)(dfree + need);
            p3_put_le16(data + P3_BH_OFF_FREESPACE, dfree);
            p3_put_le16(data + P3_BH_OFF_FREE_RECID, (uint16_t)(recid + 1));

            if (p3_write_blocks(ctx->f, data_vbn, kd->dbs ? kd->dbs : 1u,
                                data) != 0) { st = RMS$_WPL; goto done; }

            /* index high-key must cover the bucket's new max: only grows when
             * the located entry was the rightmost fallthrough (nkey > its key) */
            if (p3_keycmp(nkey, kd->seg0_siz,
                          root + entry_off + P3_IDXREC_PTR_SIZE,
                          kd->key_size) > 0) {
                memcpy(root + entry_off + P3_IDXREC_PTR_SIZE, nkey, kd->seg0_siz);
                if (kd->key_size > kd->seg0_siz)
                    memset(root + entry_off + P3_IDXREC_PTR_SIZE + kd->seg0_siz,
                           0, kd->key_size - kd->seg0_siz);
                if (p3_write_blocks(ctx->f, kd->root_vbn, kd->ibs ? kd->ibs : 1u,
                                    root) != 0) { st = RMS$_WPL; goto done; }
            }
            st = RMS$_NORMAL; goto done;
        }

        /* ---- no room: split this bucket, then loop to reinsert ---- */
        st = p3_split_data_bucket(ctx, kd, root, entry_off, data_vbn, data);
        if (!$VMS_STATUS_SUCCESS(st)) goto done;
        /* loop: re-read root/data, the record now fits one of the two halves */
    }
    st = RMS$_ORG;      /* did not converge (should not happen this rung) */

done:
    free(root);
    free(data);
    return st;
}

uint32_t rms_p3_update(p3_ctx_t *ctx, uint8_t krf,
                       const uint8_t *key, uint16_t key_len,
                       const uint8_t *rec, uint16_t rec_len)
{
    const p3_keydesc_t *kd;
    uint8_t *root = NULL, *data = NULL;
    uint32_t cap, icap, st = RMS$_NORMAL;
    uint16_t entry_off = 0, dfree, off, rec_off = 0xFFFF, oldlen = 0;
    uint32_t data_vbn = 0;

    if (!ctx || ctx->magic != P3_CTX_MAGIC || !ctx->writable || !key || !rec)
        return RMS$_PLG;
    kd = p3_find_key(ctx, krf);
    if (!kd)
        return RMS$_KEY;
    if (kd->flags & P3_KEYM_ANY_COMPR)
        return RMS$_PLG;
    if ((uint32_t)kd->seg0_pos + kd->seg0_siz > rec_len)
        return RMS$_RSZ;
    /* $UPDATE may not change the primary key (VMS: RMS$_KEY). */
    if (p3_keycmp(rec + kd->seg0_pos, kd->seg0_siz, key, key_len) != 0)
        return RMS$_KEY;

    cap  = (kd->dbs ? kd->dbs : 1u) * P3_BLK;
    icap = (kd->ibs ? kd->ibs : 1u) * P3_BLK;
    root = (uint8_t *)malloc((size_t)icap);
    data = (uint8_t *)malloc((size_t)cap);
    if (!root || !data) { st = RMS$_DME; goto done; }

    if (p3_read_blocks(ctx->f, kd->root_vbn, kd->ibs ? kd->ibs : 1u, root) != 0) {
        st = RMS$_RER; goto done;
    }
    st = p3_index_locate(kd, root, key, key_len, &entry_off, &data_vbn);
    if (!$VMS_STATUS_SUCCESS(st)) goto done;
    if (p3_read_blocks(ctx->f, data_vbn, kd->dbs ? kd->dbs : 1u, data) != 0) {
        st = RMS$_RER; goto done;
    }
    dfree = p3_le16(data + P3_BH_OFF_FREESPACE);
    if (dfree < P3_BKT_HDR_SIZE || dfree > cap) { st = RMS$_PLG; goto done; }

    /* find the live record whose embedded key matches */
    for (off = P3_BKT_HDR_SIZE; (uint16_t)(off + P3_DR_HDR_SIZE) <= dfree; ) {
        uint8_t  ctrl = data[off + P3_DR_OFF_CTRL];
        uint16_t rn   = (uint16_t)(off + p3_rec_size(data, off));
        if (rn > dfree) break;
        if (!((ctrl >> P3_IRCV_RRV) & 1u) && !((ctrl >> P3_IRCV_DELETED) & 1u)) {
            if (p3_keycmp(data + off + P3_DR_HDR_SIZE + kd->seg0_pos,
                          kd->seg0_siz, key, key_len) == 0) {
                rec_off = off;
                oldlen  = p3_le16(data + off + P3_DR_OFF_DATALEN);
                break;
            }
        }
        off = rn;
    }
    if (rec_off == 0xFFFF) { st = RMS$_RNF; goto done; }

    if (rec_len == oldlen) {
        /* same size -> overwrite in place, record ID preserved */
        memcpy(data + rec_off + P3_DR_HDR_SIZE, rec, rec_len);
        if (p3_write_blocks(ctx->f, data_vbn, kd->dbs ? kd->dbs : 1u, data) != 0)
            st = RMS$_WPL;
        else
            st = RMS$_NORMAL;
        goto done;
    }

    /* different size -> COMPACT the old record out (no tombstone), write the
     * bucket, then reinsert the new image (may itself split). RFA may change,
     * exactly as real RMS when a record grows past its slot. */
    {
        uint16_t rs = p3_rec_size(data, rec_off);
        memmove(data + rec_off, data + rec_off + rs,
                (size_t)(dfree - (rec_off + rs)));
        dfree = (uint16_t)(dfree - rs);
        p3_put_le16(data + P3_BH_OFF_FREESPACE, dfree);
        if (p3_write_blocks(ctx->f, data_vbn, kd->dbs ? kd->dbs : 1u, data) != 0) {
            st = RMS$_WPL; goto done;
        }
    }
    st = rms_p3_put(ctx, krf, rec, rec_len);

done:
    free(root);
    free(data);
    return st;
}
