/*
 * rightslist_rms.c - RIGHTSLIST as a genuine binary RMS Prolog-3 INDEXED file
 * (vms-3e0, epic vms-d0c). Authors and reads the real $RDBDEF identifier record
 * (rdb_identifier_record_t, 48 bytes) through the Files-11 Prolog-3 engine
 * (rms_prolog3.c) -- primary identifier-VALUE key + secondary identifier-NAME
 * key/SIDR. No ASCII colon/pipe format, no flat file, no /vms passthrough: the
 * index IS the on-disk Prolog-3 bucket tree the engine walks over the ACP
 * window (or the rms_io POSIX backend when /dev/vms is absent). See
 * rightslist_rms.h, and its header comment for the HOLDER-key deferred seam.
 */

#include "rightslist_rms.h"
#include "rmsdef.h"
#include "ssdef.h"     /* $VMS_STATUS_SUCCESS */

#include <ctype.h>
#include <string.h>

/* One bucket size for both keys. A $RDBDEF identifier record is 48 bytes; with
 * the engine's 11-byte data-record lead that is 59 bytes, so a 1-block data
 * bucket (512 - 14 header = 498 usable) holds ~8 records -- a modest identifier
 * count then forces a genuine data-bucket + SIDR split, exercising the same
 * split/RRV/SIDR machinery the engine tests do (mirrors sysuaf_rms.c's intent).
 */
#define RIGHTSLIST_BKT_BLOCKS 1u

/* Build the 32-byte NAME field/key: upcased (VMS folds identifier case),
 * blank-padded to 32. Shared by rdb_ident_set_name and the search-key builder
 * so a lookup matches the stored record byte-for-byte. */
static void name_field(const char *name, uint8_t *dst /* [32] */)
{
    size_t n = name ? strlen(name) : 0;
    for (size_t i = 0; i < RDB$K_NAME_LEN; i++)
        dst[i] = (i < n) ? (uint8_t)toupper((unsigned char)name[i])
                         : (uint8_t)' ';
}

void rdb_ident_set_name(rdb_identifier_record_t *r, const char *name)
{
    if (!r)
        return;
    name_field(name, (uint8_t *)r->rdb$t_name);
}

void rdb_ident_name(const rdb_identifier_record_t *r, char *out)
{
    if (!out)
        return;
    if (!r) { out[0] = '\0'; return; }
    size_t n = RDB$K_NAME_LEN;
    while (n > 0 && r->rdb$t_name[n - 1] == ' ')
        n--;
    memcpy(out, r->rdb$t_name, n);
    out[n] = '\0';
}

uint32_t rightslist_rms_create(rms_file_t *f, rightslist_rms_file_t *rf)
{
    if (!f || !rf)
        return RMS$_FAB;
    memset(rf, 0, sizeof(*rf));
    rf->f = f;

    /* primary key: 4-byte identifier VALUE at record offset 0x00. DUPLICATES
     * (oracle key 0: bin4, SEG0_POSITION 0, DUPLICATES yes): an identifier's
     * holder records share its value and follow its definition record. */
    p3_create_params_t vp;
    memset(&vp, 0, sizeof(vp));
    vp.key_size   = 4;
    vp.seg0_pos   = RDB$K_IDENTIFIER_OFF;   /* 0x00 */
    vp.seg0_siz   = 4;
    vp.dtp        = 0;                       /* stored, not decoded (memcmp key) */
    vp.bkt_blocks = RIGHTSLIST_BKT_BLOCKS;
    vp.allow_dup  = 1;

    uint32_t st = rms_p3_create(f, &vp, &rf->ctx);
    if (!$VMS_STATUS_SUCCESS(st)) {
        rf->ctx = NULL;
        rf->f = NULL;
        return st;
    }

    /* key 1 HOLDER: 8 bytes at 0x08 (oracle key 1: string, SEG0_POSITION 8,
     * DUPLICATES yes, NULL_KEY yes with null value 0 -- a definition record's
     * holder is 0, so only holder records are in this index). */
    p3_create_params_t hp;
    memset(&hp, 0, sizeof(hp));
    hp.key_size   = 8;
    hp.seg0_pos   = RDB$K_HOLDER_OFF;        /* 0x08 */
    hp.seg0_siz   = 8;
    hp.dtp        = 0;
    hp.bkt_blocks = RIGHTSLIST_BKT_BLOCKS;
    hp.allow_dup  = 1;
    hp.null_key   = 1;
    st = rms_p3_add_secondary_key(rf->ctx, &hp);

    /* key 2 NAME: 32 bytes at 0x10, unique (oracle key 2: string, SEG0_POSITION
     * 16, DUPLICATES no, NULL_KEY yes). A 16-byte holder record has no name and
     * is not in this index. */
    if ($VMS_STATUS_SUCCESS(st)) {
        p3_create_params_t np;
        memset(&np, 0, sizeof(np));
        np.key_size   = RDB$K_NAME_LEN;          /* 32 */
        np.seg0_pos   = RDB$K_NAME_OFF;          /* 0x10 */
        np.seg0_siz   = RDB$K_NAME_LEN;
        np.dtp        = 0;
        np.bkt_blocks = RIGHTSLIST_BKT_BLOCKS;
        np.allow_dup  = 0;
        np.null_key   = 1;
        st = rms_p3_add_secondary_key(rf->ctx, &np);
    }
    if (!$VMS_STATUS_SUCCESS(st)) {
        rms_p3_free(rf->ctx);
        rf->ctx = NULL;
        rf->f = NULL;
        return st;
    }
    rf->krf_holder = RIGHTSLIST_KRF_HOLDER;
    rf->krf_name   = RIGHTSLIST_KRF_NAME;
    return RMS$_CREATED;
}

uint32_t rightslist_rms_open(rms_file_t *f, rightslist_rms_file_t *rf)
{
    if (!f || !rf)
        return RMS$_FAB;
    memset(rf, 0, sizeof(*rf));
    rf->f = f;
    uint32_t st = rms_p3_bind(f, &rf->ctx);
    if (!$VMS_STATUS_SUCCESS(st)) {
        rf->ctx = NULL;
        rf->f = NULL;
        return st;
    }
    /* NAME / HOLDER by position: an older two-key file has NAME at key 1. */
    rf->krf_name = RIGHTSLIST_KRF_NONE;
    rf->krf_holder = RIGHTSLIST_KRF_NONE;
    for (uint16_t i = 0; i < rf->ctx->num_keys; i++) {
        const p3_keydesc_t *k = &rf->ctx->keys[i];
        if (k->ref != 0 && k->seg0_pos == RDB$K_NAME_OFF)
            rf->krf_name = k->ref;
        else if (k->ref != 0 && k->seg0_pos == RDB$K_HOLDER_OFF)
            rf->krf_holder = k->ref;
    }
    if (rf->krf_name == RIGHTSLIST_KRF_NONE) {
        rms_p3_free(rf->ctx);
        rf->ctx = NULL;
        rf->f = NULL;
        return RMS$_PLG;                           /* not a RIGHTSLIST image */
    }
    return st;
}

int rightslist_rms_is_current(const rightslist_rms_file_t *rf)
{
    return rf && rf->ctx && rf->krf_holder == RIGHTSLIST_KRF_HOLDER &&
           rf->krf_name == RIGHTSLIST_KRF_NAME;
}

uint32_t rightslist_put_holder(rightslist_rms_file_t *rf, const rdb_holder_record_t *rec)
{
    if (!rf || !rf->ctx || !rec)
        return RMS$_FAB;
    if (rf->krf_holder == RIGHTSLIST_KRF_NONE)
        return RMS$_KEY;
    return rms_p3_put(rf->ctx, RIGHTSLIST_KRF_VALUE,
                      (const uint8_t *)rec, RDB$K_HOLDER_RECORD_SIZE);
}

struct holder_match { uint32_t lo, hi; int any_holder; };

/* A holder record of the key's identifier, for this holder (or any). */
static int match_holder(const uint8_t *rec, uint16_t rec_len, void *arg)
{
    const struct holder_match *m = (const struct holder_match *)arg;
    if (rec_len < RDB$K_HOLDER_RECORD_SIZE || rec_len >= RDB$K_IDENT_RECORD_SIZE)
        return 0;
    if (m->any_holder)
        return 1;
    return p3_le32(rec + RDB$K_HOLDER_OFF) == m->lo &&
           p3_le32(rec + RDB$K_HOLDER_OFF + 4) == m->hi;
}

/* The identifier's definition record (48 bytes). */
static int match_definition(const uint8_t *rec, uint16_t rec_len, void *arg)
{
    (void)rec; (void)arg;
    return rec_len >= RDB$K_IDENT_RECORD_SIZE;
}

uint32_t rightslist_delete_holder(rightslist_rms_file_t *rf, uint32_t id,
                                  uint32_t holder_lo, uint32_t holder_hi)
{
    struct holder_match m = { holder_lo, holder_hi, 0 };
    uint8_t key[4];
    if (!rf || !rf->ctx)
        return RMS$_FAB;
    p3_put_le32(key, id);
    return rms_p3_delete_match(rf->ctx, key, 4, match_holder, &m);
}

uint32_t rightslist_delete_identifier(rightslist_rms_file_t *rf, uint32_t value)
{
    struct holder_match m = { 0, 0, 1 };
    uint8_t key[4];
    uint32_t st;
    if (!rf || !rf->ctx)
        return RMS$_FAB;
    p3_put_le32(key, value);
    st = rms_p3_delete_match(rf->ctx, key, 4, match_definition, NULL);
    if (st != RMS$_NORMAL)
        return st;
    do
        st = rms_p3_delete_match(rf->ctx, key, 4, match_holder, &m);
    while (st == RMS$_NORMAL);
    return st == RMS$_RNF ? RMS$_NORMAL : st;
}

uint32_t rightslist_enum(rightslist_rms_file_t *rf, p3_enum_cb cb, void *arg)
{
    if (!rf || !rf->ctx || !cb)
        return RMS$_FAB;
    return rms_p3_enum_primary(rf->ctx, cb, arg);
}

uint32_t rightslist_put_identifier(rightslist_rms_file_t *rf,
                                   const rdb_identifier_record_t *rec)
{
    if (!rf || !rf->ctx || !rec)
        return RMS$_FAB;
    {
        /* one definition record per value (key 0 allows duplicates for the
         * holder records, not for a second definition) */
        rdb_identifier_record_t cur;
        if (rightslist_get_by_value(rf, rdb_ident_value(rec), &cur) == RMS$_NORMAL)
            return RMS$_DUP;
    }
    return rms_p3_put(rf->ctx, RIGHTSLIST_KRF_VALUE,
                      (const uint8_t *)rec, RDB$K_IDENT_RECORD_SIZE);
}

uint32_t rightslist_get_by_name(rightslist_rms_file_t *rf, const char *name,
                                rdb_identifier_record_t *out)
{
    if (!rf || !rf->ctx || !name || !out)
        return RMS$_FAB;
    uint8_t key[RDB$K_NAME_LEN];
    name_field(name, key);
    uint16_t rl = 0;
    return rms_p3_get_by_key(rf->ctx, rf->krf_name,
                             key, RDB$K_NAME_LEN, 0, 0,
                             (uint8_t *)out, RDB$K_IDENT_RECORD_SIZE, &rl);
}

uint32_t rightslist_get_by_value(rightslist_rms_file_t *rf, uint32_t value,
                                 rdb_identifier_record_t *out)
{
    if (!rf || !rf->ctx || !out)
        return RMS$_FAB;
    uint8_t key[4];
    p3_put_le32(key, value);
    uint16_t rl = 0;
    uint32_t st = rms_p3_get_by_key(rf->ctx, RIGHTSLIST_KRF_VALUE,
                                    key, 4, 0, 0,
                                    (uint8_t *)out, RDB$K_IDENT_RECORD_SIZE, &rl);
    /* the first record of a value is its definition; a holder record alone
     * (16 bytes) is not an identifier */
    if (st == RMS$_NORMAL && rl < RDB$K_IDENT_RECORD_SIZE)
        return RMS$_RNF;
    return st;
}

void rightslist_rms_close(rightslist_rms_file_t *rf)
{
    if (!rf)
        return;
    if (rf->ctx) {
        rms_p3_free(rf->ctx);
        rf->ctx = NULL;
    }
    rf->f = NULL;   /* borrowed -- caller owns the handle */
}

/* =========================================================================
 * The $$MAINTENANCE_RECORD (oracle: a 64-byte metadata record carrying a VMS
 * date and a 0x0101 version pair) is not written: its date/flag sub-fields are
 * not pinned by the oracle. Holder records and the HOLDER key are (vms-7d5a).
 * ========================================================================= */

struct copy_arg { rightslist_rms_file_t *to; uint32_t st; };
static int copy_cb(const uint8_t *rec, uint16_t rec_len, void *arg)
{
    struct copy_arg *c = (struct copy_arg *)arg;
    if (rec_len >= RDB$K_IDENT_RECORD_SIZE)
        c->st = rightslist_put_identifier(c->to, (const rdb_identifier_record_t *)rec);
    else if (rec_len >= RDB$K_HOLDER_RECORD_SIZE)
        c->st = rightslist_put_holder(c->to, (const rdb_holder_record_t *)rec);
    else
        c->st = RMS$_NORMAL;                       /* nothing of ours */
    return !$VMS_STATUS_SUCCESS(c->st);
}

uint32_t rightslist_copy(rightslist_rms_file_t *from, rightslist_rms_file_t *to)
{
    struct copy_arg c = { to, RMS$_NORMAL };
    uint32_t st;
    if (!from || !from->ctx || !to || !to->ctx)
        return RMS$_FAB;
    st = rightslist_enum(from, copy_cb, &c);
    return $VMS_STATUS_SUCCESS(st) ? c.st : st;
}
