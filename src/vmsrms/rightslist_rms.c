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

    /* primary key: 4-byte identifier VALUE at record offset 0x00, unique
     * (oracle §1 key 0: bin4, SEG0_POSITION 0, DUPLICATES no). */
    p3_create_params_t vp;
    memset(&vp, 0, sizeof(vp));
    vp.key_size   = 4;
    vp.seg0_pos   = RDB$K_IDENTIFIER_OFF;   /* 0x00 */
    vp.seg0_siz   = 4;
    vp.dtp        = 0;                       /* stored, not decoded (memcmp key) */
    vp.bkt_blocks = RIGHTSLIST_BKT_BLOCKS;
    vp.allow_dup  = 0;                       /* one definition record per value  */

    uint32_t st = rms_p3_create(f, &vp, &rf->ctx);
    if (!$VMS_STATUS_SUCCESS(st)) {
        rf->ctx = NULL;
        rf->f = NULL;
        return st;
    }

    /* secondary key: 32-byte identifier NAME at record offset 0x10, unique
     * (oracle §1 key 2: string, SEG0_POSITION 16, DUPLICATES no). On real VMS
     * this is key of reference 2 (HOLDER is key 1); the HOLDER key is the
     * deferred seam, so NAME is added here as key of reference 1. */
    p3_create_params_t np;
    memset(&np, 0, sizeof(np));
    np.key_size   = RDB$K_NAME_LEN;          /* 32 */
    np.seg0_pos   = RDB$K_NAME_OFF;          /* 0x10 */
    np.seg0_siz   = RDB$K_NAME_LEN;
    np.dtp        = 0;
    np.bkt_blocks = RIGHTSLIST_BKT_BLOCKS;
    np.allow_dup  = 0;                        /* identifier names are unique      */

    st = rms_p3_add_secondary_key(rf->ctx, &np);
    if (!$VMS_STATUS_SUCCESS(st)) {
        rms_p3_free(rf->ctx);
        rf->ctx = NULL;
        rf->f = NULL;
        return st;
    }
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
    }
    return st;
}

uint32_t rightslist_put_identifier(rightslist_rms_file_t *rf,
                                   const rdb_identifier_record_t *rec)
{
    if (!rf || !rf->ctx || !rec)
        return RMS$_FAB;
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
    return rms_p3_get_by_key(rf->ctx, RIGHTSLIST_KRF_NAME,
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
    return rms_p3_get_by_key(rf->ctx, RIGHTSLIST_KRF_VALUE,
                             key, 4, 0, 0,
                             (uint8_t *)out, RDB$K_IDENT_RECORD_SIZE, &rl);
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
 * DEFERRED SEAM (labelled) -- holder records + the HOLDER key + the
 * $$MAINTENANCE_RECORD.
 *
 * This rung stores and queries identifier-DEFINITION records only, indexed by
 * VALUE (primary) and NAME (secondary). NOT built here, by design (kept honest
 * per the anti-cheat -- no stub that reports success):
 *
 *   1. HOLDER records (rdb_holder_record_t, 16 bytes) -- the identifier<->UIC
 *      GRANT rows a real VMS GRANT/IDENTIFIER creates (oracle §2b). The struct
 *      + offsets are pinned in rightslist_rms.h, but no record of this kind is
 *      $PUT here.
 *   2. The HOLDER key (oracle key of reference 1: string @0x08, DUPLICATES
 *      yes). Adding it -- and shifting NAME to key of reference 2 to match a
 *      real RIGHTSLIST -- is what lets a real VMS AUTHORIZE mount an
 *      OVMX-written file, and is the prerequisite for the holder-relationship
 *      queries ("what does this UIC hold?" via the HOLDER key; "who holds this
 *      identifier?" by scanning holder records of a value).
 *   3. The $$MAINTENANCE_RECORD (oracle §4: a 64-byte metadata record, value
 *      0x80010004, carrying a VMS quadword date + a 0x0101 version pair). Its
 *      date/flag sub-fields are explicitly NOT pinned by the oracle; emitting
 *      an OVMX-generated one so real AUTHORIZE accepts the file is follow-on.
 *
 * A holder-relationship query attempted against THIS rung has no faked answer:
 * the API surface above simply does not expose it, so a caller cannot receive a
 * false success. Closing the seam is tracked follow-on work under epic vms-d0c.
 * ========================================================================= */
