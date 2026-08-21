/*
 * rms_prolog3.h - RMS Prolog-3 indexed-file READ engine (vms-5f0, epic vms-d0c
 * / vms-1e5). The genuine Files-11 Prolog-3 on-disk index, read over the
 * executive ACP ($ASSIGN + IO$_ACCESS + IO$_READVBLK), superseding the faked
 * in-memory `.rms_idx` B-tree sidecar (rms_idx.c) for the /dev/vms-present
 * runtime path.
 *
 * ====================================================================
 * WHAT THIS IS
 * ====================================================================
 * A reader for a real Prolog-3 indexed file: it parses the prologue (fixed
 * prolog block + area descriptors + per-key descriptors), then walks the
 * key-of-reference index buckets down to the primary data bucket and returns
 * the record whose embedded key matches a search key ($GET/$FIND by key). It
 * does NOT invent an index in a private sidecar; the index IS the on-disk
 * bucket tree, read block-by-block through the same rms_io_* positioned-I/O
 * vocabulary the seq/rel engines use -- which dispatches to IO$_READVBLK on the
 * ACP channel window when /dev/vms is present (Rule 9 / INV-6), and to a plain
 * fd only on the executive-absent host defer / netbsd-vax cross.
 *
 * ====================================================================
 * ORACLE GROUNDING (docs/oracle/vax73-alpha84-rms-prolog3.md, vms-8438)
 * ====================================================================
 * The geometry was measured clean-room (ANALYZE/RMS_FILE + DUMP/BLOCKS on real
 * OpenVMS VAX V7.3 and Alpha V8.4; no VSI source). The oracle pins the field
 * SET, the field ORDER, and specific facts; it does NOT publish byte-level
 * offsets WITHIN the fixed-prolog block, the key descriptor, or the 14-byte
 * bucket header (ANALYZE labels the fields, it does not dump their offsets).
 * Per CLAUDE.md Rule 8, where the byte layout is not published OVMX defines its
 * own representation and LABELS it an OVMX design choice. Below, [PIN] marks a
 * fact taken verbatim from the oracle; [OVMX] marks a byte-offset choice this
 * header defines so the OVMX writer (vms-045) and this reader agree.
 *
 *   [PIN] Prolog Version == 3.
 *   [PIN] Area-descriptor array begins at VBN 3; descriptors are 64 bytes each,
 *         carrying {bucket size, reclaimed-bucket VBN, current extent
 *         {start,blocks,used,next}, default extend qty, total allocation}.
 *   [PIN] Key descriptor #0 lives in VBN 1; descriptors chain by
 *         {next-descriptor VBN, offset}; per key: Root VBN, First Data Bucket
 *         VBN, Root Level, Index/Data Bucket Size, Key Flags bitfield, Key
 *         Segments, Key Size, Minimum Record Size, Segment Positions/Sizes,
 *         Data Type, Name.
 *   [PIN] Key-flag bit positions: DUPKEYS 0, CHGKEYS 1, NULKEYS 2, IDX_COMPR 3,
 *         INITIDX 4, KEY_COMPR 6, REC_COMPR 7.
 *   [PIN] Bucket header is 14 bytes; records begin at bucket offset 0x0E.
 *         Header fields (in order): check character, key-of-reference, VBN
 *         sample, free-space offset (next free byte in bucket), free record ID,
 *         next-bucket VBN (horizontal chain), level (0 == data), flags
 *         (LASTBKT bit 0, ROOTBKT bit 1).
 *   [PIN] Index records use 2-byte bucket pointers, ordered {pointer, key}.
 *   [PIN] Primary data record leads with a record-control-flags byte
 *         (IRC$V_DELETED bit 2, IRC$V_RRV bit 3, IRC$V_NOPTRSZ bit 4,
 *         IRC$V_RU_DELETE bit 5, IRC$V_RU_UPDATE bit 6), a Record ID, and an
 *         RRV entry (RRV ID + a 2-or-4-byte bucket pointer), then the key.
 *   [PIN] No VAX/Alpha divergence -- the format is architecture-independent
 *         (this is why the reader below uses fixed-width LE fields only, so one
 *         implementation serves x86_64/aarch64 LP64 and VAX ILP32 alike).
 *
 * ====================================================================
 * SUBSTRATE-AGNOSTIC INVARIANT (vms-5f0 thin-seam)
 * ====================================================================
 * Every serialized field below is a fixed-width uint8/16/32 read through the
 * le16()/le32() accessors -- NEVER a native long/size_t/pointer. There is NO
 * substrate #ifdef in rms_prolog3.c; per-substrate block transfer lives behind
 * rms_io_* (rms_io.h). VAX ILP32 + Alpha/x86_64 LP64 therefore share this file
 * byte-for-byte.
 *
 * ====================================================================
 * SCOPE (smallest genuine increment; see rms_prolog3.c header comment)
 * ====================================================================
 * READ by primary key over a single-level index (Root Level 1), uncompressed
 * keys/records. Compression, multi-level descent, bucket overflow/split chains,
 * secondary-key SIDR read, and the WRITE engine are labelled follow-on rungs
 * (compression + SIDR: this epic; write: vms-045). Where a file asks for a
 * feature this rung does not implement (e.g. a compression flag is set), the
 * reader FAILS HONESTLY with RMS$_PLG rather than mis-decoding (INV-6).
 */
#ifndef RMS_PROLOG3_H
#define RMS_PROLOG3_H

#include <stddef.h>
#include <stdint.h>
#include "rms_io.h"

/* -------- block / VBN geometry -------- */
#define P3_BLK              512u
#define P3_PROLOG_VERSION   3u
/* Maximum bucket size this reader will buffer (blocks). RMS caps bucket size at
 * 63 blocks; the reader rejects anything larger as a malformed prologue. */
#define P3_MAX_BKT_BLOCKS   63u

/* VBN assignments (1-based). [PIN] area descriptors at VBN 3. The fixed-prolog
 * + key-descriptor placement in VBN 1 is [OVMX] (oracle pins key descriptor #0
 * "at VBN 1" but not the intra-VBN split with the fixed-prolog fields). */
#define P3_VBN_FIXED_PROLOG 1u
#define P3_VBN_AREA_DESC    3u   /* [PIN] */

/* -------- fixed prolog block, VBN 1 offset 0 ([OVMX] field offsets) -------- */
#define P3_FP_OFF_VERSION       0u   /* u16, [PIN] value == 3               */
#define P3_FP_OFF_NUM_KEYS      2u   /* u16                                 */
#define P3_FP_OFF_NUM_AREAS     4u   /* u16                                 */
#define P3_FP_OFF_FIRST_AREAVBN 6u   /* u16, [PIN] value == 3               */
#define P3_FP_OFF_FIRST_KEYOFF  8u   /* u16, byte offset of key-desc array  */
/* [OVMX] write high-water: the next free VBN the writer (vms-045) will hand out
 * for a new bucket. The oracle carries this notion in the area descriptor's
 * "current extent {used,next}"; OVMX stores a single file-wide allocation cursor
 * here (u32) so a reopened file resumes $PUT allocation without re-scanning the
 * whole prologue. Zero on a read-only image the reader never allocates into. */
#define P3_FP_OFF_ALLOC_NEXT    10u  /* u32  [OVMX] next free VBN (write only) */
#define P3_FP_HDR_SIZE          16u  /* key descriptors start here by default */

/* -------- area descriptor (VBN 3+), 64 bytes each [PIN size], [OVMX] offs -- */
#define P3_AREADESC_SIZE        64u  /* [PIN] */

/* -------- key descriptor ([OVMX] offsets; [PIN] 102-byte stride) ----------- */
#define P3_KEYDESC_SIZE         102u /* [PIN] key-descriptor record length 0x66 */
#define P3_KD_OFF_NEXT_VBN      0u   /* u16  next key descriptor VBN         */
#define P3_KD_OFF_NEXT_OFF      2u   /* u16  next key descriptor byte offset */
#define P3_KD_OFF_REF           4u   /* u8   key of reference                */
#define P3_KD_OFF_DTP           5u   /* u8   data type                       */
#define P3_KD_OFF_FLAGS         6u   /* u16  key flags ([PIN] bit positions) */
#define P3_KD_OFF_ROOT_LEVEL    8u   /* u8   root level                      */
#define P3_KD_OFF_IBS           9u   /* u8   index bucket size (blocks)      */
#define P3_KD_OFF_DBS           10u  /* u8   data bucket size (blocks)       */
#define P3_KD_OFF_NSEG          11u  /* u8   number of key segments          */
#define P3_KD_OFF_ROOT_VBN      12u  /* u32  root bucket VBN                  */
#define P3_KD_OFF_FIRST_DATAVBN 16u  /* u32  first data bucket VBN            */
#define P3_KD_OFF_KEY_SIZE      20u  /* u16  total key size                   */
#define P3_KD_OFF_MIN_REC_SIZE  22u  /* u16  minimum record size              */
#define P3_KD_OFF_SEG0_POS      24u  /* u16  segment 0 position               */
#define P3_KD_OFF_SEG0_SIZ      26u  /* u8   segment 0 size                   */
#define P3_KD_OFF_NAME          40u  /* char[32] key name                     */

/* key-flag bit positions [PIN] */
#define P3_KEYV_DUPKEYS   0u
#define P3_KEYV_CHGKEYS   1u
#define P3_KEYV_NULKEYS   2u
#define P3_KEYV_IDX_COMPR 3u
#define P3_KEYV_INITIDX   4u
#define P3_KEYV_KEY_COMPR 6u
#define P3_KEYV_REC_COMPR 7u
#define P3_KEYM_ANY_COMPR ((1u<<P3_KEYV_IDX_COMPR)|(1u<<P3_KEYV_KEY_COMPR)| \
                           (1u<<P3_KEYV_REC_COMPR))

/* -------- bucket header, 14 bytes [PIN size + records@0x0E]; [OVMX] offs ---- */
#define P3_BKT_HDR_SIZE         14u  /* [PIN] records begin at 0x0E */
#define P3_BH_OFF_CHECK         0u   /* u8   check character (algo [OVMX])   */
#define P3_BH_OFF_KOR           1u   /* u8   key of reference                */
#define P3_BH_OFF_LEVEL         2u   /* u8   level (0 == data bucket)         */
#define P3_BH_OFF_FLAGS         3u   /* u8   flags ([PIN] LASTBKT 0, ROOT 1)  */
#define P3_BH_OFF_FREESPACE     4u   /* u16  next free byte in bucket         */
#define P3_BH_OFF_FREE_RECID    6u   /* u16  next record ID                   */
#define P3_BH_OFF_VBN_SAMPLE    8u   /* u16  VBN sample (low 16 bits)         */
#define P3_BH_OFF_NEXT_VBN      10u  /* u32  next bucket VBN (horizontal)     */
#define P3_BKTV_LASTBKT   0u   /* [PIN] */
#define P3_BKTV_ROOTBKT   1u   /* [PIN] */

/* -------- index record ([PIN] {2-byte pointer, key}; [OVMX] fixed stride) --
 * Uncompressed high-key stored at full key_size (this rung is IDX_COMPR off).
 *   u16 child_vbn ; key_size bytes high_key
 * stride = 2 + key_size */
#define P3_IDXREC_PTR_SIZE      2u   /* [PIN] 2-byte bucket pointer */

/* -------- primary data record ([PIN] field set/order; [OVMX] widths/lenfld) -
 *   u8  control_flags ([PIN] IRC$V_* bits)
 *   u16 record_id
 *   u16 rrv_id
 *   u32 rrv_bucket_ptr   ([PIN] RRV carries a bucket pointer; 4-byte per oracle)
 *   u16 rec_data_len (L) ([OVMX] variable-record length within the bucket)
 *   L   record_data      (the full user record; embedded key at seg0 position)
 * stride = 11 + L */
#define P3_DR_OFF_CTRL          0u
#define P3_DR_OFF_RECID         1u
#define P3_DR_OFF_RRVID         3u
#define P3_DR_OFF_RRVPTR        5u
#define P3_DR_OFF_DATALEN       9u
#define P3_DR_HDR_SIZE          11u  /* bytes before record_data */
#define P3_IRCV_DELETED   2u   /* [PIN] */
#define P3_IRCV_RRV       3u   /* [PIN] */
#define P3_IRCV_NOPTRSZ   4u   /* [PIN] */
#define P3_IRCV_RU_DELETE 5u   /* [PIN] */
#define P3_IRCV_RU_UPDATE 6u   /* [PIN] */

/* -------- SIDR: secondary index data record (vms-2ae) ----------------------
 * The oracle ([PIN], oracle §4/§5): a SECONDARY key of reference has its OWN
 * index tree; its leaf (level-0) buckets hold SIDR records, one per distinct
 * secondary-key VALUE, and each SIDR is `{key value, then an ARRAY of pointers
 * to the primary records carrying that value}`. Non-duplicate -> one pointer;
 * duplicate secondary values grow the pointer array. ANALYZE labels ONLY the
 * SIDR key -- the per-pointer sub-layout is NOT published, so it is an [OVMX]
 * design choice below (oracle §5, Rule 8).
 *
 * [OVMX] on-disk SIDR record (variable length, in a level-0 SIDR bucket whose
 * 14-byte header is the SAME p3 bucket header as a data bucket):
 *     u8  ctrl        (IRC$V_DELETED tombstone bit; SIDRs carry NO RRV -- nothing
 *                      references a SIDR by RFA, so no stub is ever left)
 *     u16 payload_len (P == bytes after this field: key_size + 2 + nptr*6)
 *     key_size bytes  secondary key value (zero-padded to key_size)
 *     u16 nptr        (number of primary pointers, >= 1)
 *     nptr * RFA      each a primary Record File Address (see below)
 *   stride = P3_SIDR_HDR_SIZE + P
 *
 * [OVMX] RFA (primary Record File Address, stable across a primary split):
 *     u32 home_vbn    (the data bucket the record was FIRST inserted into)
 *     u16 home_recid  (the record id assigned in that home bucket)
 * The primary write engine keeps this address resolvable across bucket splits
 * by (a) leaving an RRV stub at the home {vbn,id} that chains to the record's
 * new location and (b) PRESERVING the home {vbn,id} in the moved record's
 * rrv-ptr/rrv-id fields, so rms_p3_get_by_rfa resolves the CURRENT record and
 * rms_p3_delete recovers the stored RFA to purge SIDRs. */
#define P3_SIDR_OFF_CTRL    0u
#define P3_SIDR_OFF_LEN     1u   /* u16 payload length */
#define P3_SIDR_HDR_SIZE    3u   /* bytes before the key value */
#define P3_RFA_SIZE         6u   /* u32 home_vbn + u16 home_recid */
#define P3_SIDR_NPTR_SIZE   2u   /* u16 pointer count before the RFA array */

/* -------- little-endian fixed-width accessors (substrate-agnostic) --------- */
static inline uint16_t p3_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t p3_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void p3_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
}
static inline void p3_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);        p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff); p[3] = (uint8_t)((v >> 24) & 0xff);
}
static inline uint64_t p3_le64(const uint8_t *p)
{
    return (uint64_t)p3_le32(p) | ((uint64_t)p3_le32(p + 4) << 32);
}
static inline void p3_put_le64(uint8_t *p, uint64_t v)
{
    p3_put_le32(p, (uint32_t)(v & 0xffffffffu));
    p3_put_le32(p + 4, (uint32_t)((v >> 32) & 0xffffffffu));
}

/* -------- parsed per-key descriptor (host-side, native widths OK: not
 *          serialized -- this is in-memory working state) -------- */
typedef struct p3_keydesc {
    uint8_t  ref;
    uint8_t  dtp;
    uint16_t flags;
    uint8_t  root_level;
    uint8_t  ibs;            /* index bucket size (blocks) */
    uint8_t  dbs;            /* data bucket size (blocks)  */
    uint8_t  nseg;
    uint32_t root_vbn;
    uint32_t first_data_vbn;
    uint16_t key_size;
    uint16_t min_rec_size;
    uint16_t seg0_pos;
    uint8_t  seg0_siz;
} p3_keydesc_t;

/* -------- bound Prolog-3 file context (stored on FAB->_rms_state) ---------- */
#define P3_CTX_MAGIC 0x50334358u  /* "P3CX" -- distinguishes from btree_t */
#define P3_MAX_KEYS  8u
typedef struct p3_ctx {
    uint32_t     magic;          /* P3_CTX_MAGIC (first field: discriminator) */
    rms_file_t  *f;              /* the ACP/POSIX handle (borrowed from FAB)  */
    uint16_t     version;
    uint16_t     num_keys;
    uint16_t     num_areas;
    uint32_t     alloc_next;     /* write high-water: next free VBN to hand out */
    int          writable;       /* 1 if bound/created for $PUT/$UPDATE        */
    p3_keydesc_t keys[P3_MAX_KEYS];
} p3_ctx_t;

/* Parse the prologue of an already-open Prolog-3 file into a fresh ctx.
 * Returns RMS$_NORMAL, or RMS$_PLG on a bad/unsupported prologue. On success
 * *out points to a malloc'd ctx the caller frees with rms_p3_free(). */
uint32_t rms_p3_bind(rms_file_t *f, p3_ctx_t **out);
void     rms_p3_free(p3_ctx_t *ctx);

/* Read the record whose primary(-of-reference) key matches `key`
 * (key_len bytes). `krf` selects the key of reference (0 = primary). On match
 * copies up to buf_sz bytes of the record into buf and sets *rec_len.
 * rop_kge/rop_kgt select >= / > semantics (0/0 == exact match).
 * Returns RMS$_NORMAL, RMS$_RNF (no such key), RMS$_RTB (buffer too small,
 * *rec_len set to the true size), RMS$_KEY (bad key of reference / key),
 * RMS$_PLG (unsupported feature -- e.g. compression), RMS$_RER (read error). */
uint32_t rms_p3_get_by_key(p3_ctx_t *ctx, uint8_t krf,
                           const uint8_t *key, uint16_t key_len,
                           int rop_kge, int rop_kgt,
                           uint8_t *buf, uint16_t buf_sz, uint16_t *rec_len);

/* Locate-only variant of the above (for $FIND): resolves the record and
 * returns its size in *rec_len without copying data. */
uint32_t rms_p3_find_by_key(p3_ctx_t *ctx, uint8_t krf,
                            const uint8_t *key, uint16_t key_len,
                            int rop_kge, int rop_kgt, uint16_t *rec_len);

/* Sequential PRIMARY-key enumeration (vms-5f0): walk the primary key's data
 * buckets in key order (from First Data Bucket VBN, following the horizontal
 * next-bucket chain), invoking `cb` with each LIVE record (deleted records and
 * RRV stubs skipped). This is the smallest genuine "read every record" the
 * atomic-flip consumers need (SYSUAF home-directory provisioning, AUTHORIZE
 * LIST) without a general RAB$C_SEQ engine. `cb` returns 0 to continue, non-0
 * to stop early (enumeration then returns RMS$_NORMAL). Returns RMS$_NORMAL,
 * RMS$_KEY (no primary key), RMS$_PLG (compression / malformed bucket),
 * RMS$_RER (read error), RMS$_DME. The record pointer passed to `cb` is only
 * valid for the duration of the call. */
typedef int (*p3_enum_cb)(const uint8_t *rec, uint16_t rec_len, void *arg);
uint32_t rms_p3_enum_primary(p3_ctx_t *ctx, p3_enum_cb cb, void *arg);

/* ====================================================================
 * WRITE engine (vms-045): $CREATE / $PUT (insert + bucket SPLIT + RRV) /
 * $UPDATE, authoring the genuine Files-11 Prolog-3 index over the ACP
 * IO$_WRITEVBLK window. The writer emits EXACTLY the byte layout the reader
 * above parses (same [PIN]/[OVMX] offsets), so writer and reader round-trip.
 * ==================================================================== */

/* Parameters to author a fresh Prolog-3 indexed file (single primary key,
 * uncompressed keys/records this rung). [OVMX] layout is rms_prolog3.h. */
typedef struct p3_create_params {
    uint16_t key_size;      /* primary key length (bytes), 1..255            */
    uint16_t seg0_pos;      /* embedded-key byte offset within the record    */
    uint8_t  seg0_siz;      /* segment-0 size (== key_size, single segment)  */
    uint8_t  dtp;           /* data type (0 == string; stored, not decoded)  */
    uint8_t  bkt_blocks;    /* index/data bucket size in 512-byte blocks, >=1 */
    uint8_t  allow_dup;     /* 1 permits duplicate keys, 0 -> RMS$_DUP        */
} p3_create_params_t;

/* Author a fresh, EMPTY Prolog-3 indexed file over the ACP window of the
 * already-CREATEd/ACCESSED-for-write file `f` (an IO$_CREATE handle, or a
 * POSIX-backed handle on the executive-absent defer). Writes VBN 1 (fixed
 * prolog + key descriptor #0), VBN 3 (area descriptor), the root index bucket
 * (Root Level 1), and the first empty data bucket -- all via IO$_WRITEVBLK.
 * On success *out is a WRITABLE ctx (free with rms_p3_free), ready for $PUT.
 * Returns RMS$_CREATED, RMS$_WPL (prologue write error), RMS$_KEY (bad key
 * params), RMS$_PLG, RMS$_DME. */
uint32_t rms_p3_create(rms_file_t *f, const p3_create_params_t *p,
                       p3_ctx_t **out);

/* Insert one record (`rec`, `rec_len` bytes; embedded key at the key
 * descriptor's seg-0 position) into key-of-reference `krf` (0 == primary),
 * maintaining sorted order, the control-flags/Record-ID/RRV lead, and the
 * index. Splits the target data bucket when it fills (allocating a new bucket,
 * redistributing records, leaving RRV stubs for RFA stability, and updating the
 * parent index bucket's 2-byte child pointers). Returns RMS$_NORMAL, RMS$_DUP
 * (duplicate key, allow_dup off), RMS$_RSZ (record shorter than the embedded
 * key), RMS$_ORG (index would need a level this rung does not grow -- fail
 * honest, no mis-write), RMS$_RER/RMS$_WPL (I/O), RMS$_PLG. */
uint32_t rms_p3_put(p3_ctx_t *ctx, uint8_t krf,
                    const uint8_t *rec, uint16_t rec_len);

/* Modify the record whose key-of-reference `krf` key matches `key`: in place if
 * the new image is the same length, else delete-and-reinsert (RFA may change,
 * as on real RMS when the record grows). Returns RMS$_NORMAL, RMS$_RNF (no such
 * record), RMS$_DUP/RMS$_RSZ/RMS$_ORG as $PUT, or an I/O status. */
uint32_t rms_p3_update(p3_ctx_t *ctx, uint8_t krf,
                       const uint8_t *key, uint16_t key_len,
                       const uint8_t *rec, uint16_t rec_len);

/* ====================================================================
 * SECONDARY KEYS / SIDR (vms-2ae) -- a second key of reference with its own
 * index tree whose leaves are SIDR records mapping a secondary-key VALUE to the
 * primary record(s) carrying it. Built on the SAME [PIN]/[OVMX] bucket + index
 * machinery as the primary key; no flat scan, no sidecar.
 * ==================================================================== */

/* A resolved primary Record File Address (stable across a primary split). */
typedef struct p3_rfa {
    uint32_t home_vbn;
    uint16_t home_recid;
} p3_rfa_t;

/* Define ONE secondary key of reference on the just-CREATEd, still-EMPTY file
 * `ctx` (must be writable, no records $PUT yet). Allocates the secondary key
 * descriptor (chained after the existing descriptors in VBN 1), a fresh root
 * index bucket (Root Level 1) and the first empty SIDR bucket, and bumps
 * ctx->num_keys. `p` gives the secondary key geometry: seg0_pos/seg0_siz locate
 * the key WITHIN the primary record; key_size is the stored/compared width
 * (>= seg0_siz; zero-padded). allow_dup permits duplicate secondary values.
 * Returns RMS$_NORMAL, RMS$_KEY (bad params / not empty / too many keys),
 * RMS$_WPL, RMS$_DME, RMS$_PLG. */
uint32_t rms_p3_add_secondary_key(p3_ctx_t *ctx, const p3_create_params_t *p);

/* Look up a SECONDARY key of reference `krf` (>= 1) by VALUE: descends the
 * secondary index to the SIDR bucket, finds the SIDR for `key`, and returns its
 * primary-pointer array into out[0..*count) (up to `max`). Duplicate secondary
 * values yield count > 1. Returns RMS$_NORMAL, RMS$_RNF (no such value),
 * RMS$_KEY (not a secondary key), RMS$_RTB (more pointers than `max`; *count is
 * the true count), RMS$_PLG, RMS$_RER. */
uint32_t rms_p3_sidr_lookup(p3_ctx_t *ctx, uint8_t krf,
                            const uint8_t *key, uint16_t key_len,
                            p3_rfa_t *out, uint16_t max, uint16_t *count);

/* Resolve a primary RFA (home_vbn/home_recid) to the CURRENT primary record --
 * following RRV stubs left by any bucket splits -- and copy up to buf_sz bytes,
 * setting *rec_len. Returns RMS$_NORMAL, RMS$_RNF (dangling/deleted), RMS$_RTB,
 * RMS$_RER, RMS$_PLG. This is how a secondary lookup reaches the data record. */
uint32_t rms_p3_get_by_rfa(p3_ctx_t *ctx, uint32_t home_vbn, uint16_t home_recid,
                           uint8_t *buf, uint16_t buf_sz, uint16_t *rec_len);

/* Delete the primary record whose primary key matches `key` (krf must be 0):
 * removes the record from its data bucket AND purges its pointer from every
 * secondary SIDR (by matching the record's stable RFA), so no secondary lookup
 * can resolve to a deleted record. Returns RMS$_NORMAL, RMS$_RNF, RMS$_KEY,
 * RMS$_WPL, RMS$_PLG, RMS$_DME, RMS$_RER. */
uint32_t rms_p3_delete(p3_ctx_t *ctx, uint8_t krf,
                       const uint8_t *key, uint16_t key_len);

#endif /* RMS_PROLOG3_H */
