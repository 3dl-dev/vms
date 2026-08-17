/*
 * sysuaf_rms.c - SYSUAF as a genuine binary RMS Prolog-3 INDEXED file
 * (vms-f88, epic vms-d0c). Authors and reads the real $UAFDEF record
 * (sysuaf_rms_record_t, 644 bytes) through the Files-11 Prolog-3 engine
 * (rms_prolog3.c) -- primary USERNAME key + secondary UIC key/SIDR. No ASCII
 * pipe format, no SHA-256, no flat file, no /vms passthrough: the index IS the
 * on-disk Prolog-3 bucket tree the engine walks over the ACP window (or the
 * rms_io POSIX backend when /dev/vms is absent). See sysuaf_rms.h.
 */

#include "sysuaf_rms.h"
#include "rmsdef.h"
#include "ssdef.h"     /* $VMS_STATUS_SUCCESS */

#include <ctype.h>
#include <string.h>

/* One bucket size for both keys. A $UAFDEF record is 644 bytes; with the
 * engine's 11-byte data-record lead that is 655 bytes, so a data bucket must
 * be >= 2 blocks to hold one record. 3 blocks (1536 - 14 header = 1522 usable)
 * holds two records and lets a modest account count force a genuine split --
 * exercising the same split/RRV/SIDR machinery the engine tests do. */
#define SYSUAF_BKT_BLOCKS 3u

/* Upcased, blank-padded 32-byte USERNAME primary key (VMS folds case). */
void sysuaf_rec_set_username(sysuaf_rms_record_t *r, const char *username)
{
    size_t n = username ? strlen(username) : 0;
    for (size_t i = 0; i < SYSUAF_USERNAME_LEN; i++) {
        if (i < n)
            r->uaf$t_username[i] =
                (char)toupper((unsigned char)username[i]);
        else
            r->uaf$t_username[i] = ' ';
    }
}

/* Build the 32-byte search key from a caller-supplied username the same way
 * sysuaf_rec_set_username builds the stored key, so a lookup matches. */
static void username_key(const char *username, uint8_t *key /* [32] */)
{
    size_t n = username ? strlen(username) : 0;
    for (size_t i = 0; i < SYSUAF_USERNAME_LEN; i++)
        key[i] = (i < n) ? (uint8_t)toupper((unsigned char)username[i])
                         : (uint8_t)' ';
}

uint32_t sysuaf_rms_create(rms_file_t *f, sysuaf_rms_file_t *sf)
{
    if (!f || !sf)
        return RMS$_FAB;
    memset(sf, 0, sizeof(*sf));
    sf->f = f;

    /* primary key: 32-byte USERNAME at record offset 0x04, unique */
    p3_create_params_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.key_size   = SYSUAF_KEY_USERNAME_SIZ;
    cp.seg0_pos   = SYSUAF_KEY_USERNAME_POS;
    cp.seg0_siz   = SYSUAF_KEY_USERNAME_SIZ;
    cp.dtp        = 0;                 /* string */
    cp.bkt_blocks = SYSUAF_BKT_BLOCKS;
    cp.allow_dup  = 0;                 /* usernames are unique */

    uint32_t st = rms_p3_create(f, &cp, &sf->ctx);
    if (!$VMS_STATUS_SUCCESS(st)) {
        sf->ctx = NULL;
        sf->f = NULL;
        return st;
    }

    /* secondary key: 4-byte UIC longword at record offset 0x24, dups allowed
     * (two accounts may hold the same UIC -> a SIDR pointer array) */
    p3_create_params_t up;
    memset(&up, 0, sizeof(up));
    up.key_size   = SYSUAF_KEY_UIC_SIZ;
    up.seg0_pos   = SYSUAF_KEY_UIC_POS;
    up.seg0_siz   = SYSUAF_KEY_UIC_SIZ;
    up.dtp        = 0;
    up.bkt_blocks = SYSUAF_BKT_BLOCKS;
    up.allow_dup  = 1;

    st = rms_p3_add_secondary_key(sf->ctx, &up);
    if (!$VMS_STATUS_SUCCESS(st)) {
        rms_p3_free(sf->ctx);
        sf->ctx = NULL;
        sf->f = NULL;
        return st;
    }
    return RMS$_CREATED;
}

uint32_t sysuaf_rms_open(rms_file_t *f, sysuaf_rms_file_t *sf)
{
    if (!f || !sf)
        return RMS$_FAB;
    memset(sf, 0, sizeof(*sf));
    sf->f = f;
    uint32_t st = rms_p3_bind(f, &sf->ctx);
    if (!$VMS_STATUS_SUCCESS(st)) {
        sf->ctx = NULL;
        sf->f = NULL;
    }
    return st;
}

uint32_t sysuaf_put_record(sysuaf_rms_file_t *sf, const sysuaf_rms_record_t *rec)
{
    if (!sf || !sf->ctx || !rec)
        return RMS$_FAB;
    return rms_p3_put(sf->ctx, SYSUAF_KRF_USERNAME,
                      (const uint8_t *)rec, SYSUAF_UAF_RECORD_SIZE);
}

uint32_t sysuaf_get_by_username(sysuaf_rms_file_t *sf, const char *username,
                                sysuaf_rms_record_t *out)
{
    if (!sf || !sf->ctx || !username || !out)
        return RMS$_FAB;
    uint8_t key[SYSUAF_KEY_USERNAME_SIZ];
    username_key(username, key);
    uint16_t rl = 0;
    return rms_p3_get_by_key(sf->ctx, SYSUAF_KRF_USERNAME,
                             key, SYSUAF_KEY_USERNAME_SIZ, 0, 0,
                             (uint8_t *)out, SYSUAF_UAF_RECORD_SIZE, &rl);
}

uint32_t sysuaf_get_by_uic(sysuaf_rms_file_t *sf, uint32_t uic,
                           sysuaf_rms_record_t *out)
{
    if (!sf || !sf->ctx || !out)
        return RMS$_FAB;
    uint8_t key[SYSUAF_KEY_UIC_SIZ];
    p3_put_le32(key, uic);
    uint16_t rl = 0;
    return rms_p3_get_by_key(sf->ctx, SYSUAF_KRF_UIC,
                             key, SYSUAF_KEY_UIC_SIZ, 0, 0,
                             (uint8_t *)out, SYSUAF_UAF_RECORD_SIZE, &rl);
}

void sysuaf_rms_close(sysuaf_rms_file_t *sf)
{
    if (!sf)
        return;
    if (sf->ctx) {
        rms_p3_free(sf->ctx);
        sf->ctx = NULL;
    }
    sf->f = NULL;   /* borrowed -- caller owns the handle */
}
