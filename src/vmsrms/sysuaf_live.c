/*
 * sysuaf_live.c - LIVE SYSUAF access over the binary $UAFDEF indexed file
 * (vms-d92, epic vms-d0c). See sysuaf_live.h for the layering seam and the
 * substrate rules. This is the ONE reader/writer of the runtime SYSUAF; the
 * ASCII+SHA-256 facade (the old libvms rtl/sysuaf.c scan + AUTHORIZE fprintf)
 * is retired.
 */

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <ctype.h>

#include "sysuaf_live.h"
#include "sysuaf_rms.h"
#include "rms_io.h"
#include "rms_prolog3.h"
#include "rmsdef.h"
#include "ssdef.h"
#include "ovmx_layout.h"     /* VMS_SYSUAF_PATH */

#define SYSUAF_LIVE_PATH  VMS_SYSUAF_PATH

/* Bind an existing SYSUAF for read or write. Returns the open handle in *hp and
 * a bound sysuaf_rms_file_t in *sf; the caller closes with live_close(). */
static uint32_t live_open(int want_write, rms_file_t **hp, sysuaf_rms_file_t *sf)
{
    uint32_t st = RMS$_FAB;
    rms_file_t *h = rms_open_named_handle(SYSUAF_LIVE_PATH, want_write, 0, &st);
    if (!h)
        return $VMS_STATUS_SUCCESS(st) ? RMS$_FNF : st;

    st = sysuaf_rms_open(h, sf);
    if (!$VMS_STATUS_SUCCESS(st)) {
        rms_close_named_handle(h);
        return st;
    }
    /* rms_p3_bind leaves the ctx read-only; the writable engines ($PUT/$UPDATE)
     * gate on ctx->writable. Promote it when the handle was opened for write. */
    if (want_write && sf->ctx && h->writable)
        sf->ctx->writable = 1;
    *hp = h;
    return RMS$_NORMAL;
}

static void live_close(rms_file_t *h, sysuaf_rms_file_t *sf)
{
    if (sf)
        sysuaf_rms_close(sf);
    if (h)
        rms_close_named_handle(h);
}

uint32_t ovmx_sysuaf_read_user(const char *username, sysuaf_rms_record_t *out)
{
    rms_file_t *h = NULL;
    sysuaf_rms_file_t sf;
    uint32_t st;

    if (!username || !out)
        return RMS$_FAB;
    memset(&sf, 0, sizeof(sf));
    st = live_open(0, &h, &sf);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;
    st = sysuaf_get_by_username(&sf, username, out);
    live_close(h, &sf);
    return st;
}

uint32_t ovmx_sysuaf_read_uic(uint32_t uic, sysuaf_rms_record_t *out)
{
    rms_file_t *h = NULL;
    sysuaf_rms_file_t sf;
    uint32_t st;

    if (!out)
        return RMS$_FAB;
    memset(&sf, 0, sizeof(sf));
    st = live_open(0, &h, &sf);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;
    st = sysuaf_get_by_uic(&sf, uic, out);
    live_close(h, &sf);
    return st;
}

/* Adapter: the p3 enum hands back raw record bytes; re-present them as a
 * 644-byte $UAFDEF record for the typed callback. */
struct live_enum_ctx {
    ovmx_sysuaf_enum_cb cb;
    void               *arg;
};
static int live_enum_trampoline(const uint8_t *rec, uint16_t rec_len, void *arg)
{
    struct live_enum_ctx *e = (struct live_enum_ctx *)arg;
    sysuaf_rms_record_t r;
    if (rec_len < SYSUAF_UAF_RECORD_SIZE)
        return 0;                        /* skip a short/foreign record */
    memcpy(&r, rec, SYSUAF_UAF_RECORD_SIZE);
    return e->cb(&r, e->arg);
}

uint32_t ovmx_sysuaf_enum(ovmx_sysuaf_enum_cb cb, void *arg)
{
    rms_file_t *h = NULL;
    sysuaf_rms_file_t sf;
    struct live_enum_ctx e;
    uint32_t st;

    if (!cb)
        return RMS$_FAB;
    memset(&sf, 0, sizeof(sf));
    st = live_open(0, &h, &sf);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;
    e.cb = cb;
    e.arg = arg;
    st = rms_p3_enum_primary(sf.ctx, live_enum_trampoline, &e);
    live_close(h, &sf);
    return st;
}

uint32_t ovmx_sysuaf_store_user(const sysuaf_rms_record_t *rec)
{
    rms_file_t *h = NULL;
    sysuaf_rms_file_t sf;
    sysuaf_rms_record_t existing;
    uint8_t key[SYSUAF_KEY_USERNAME_SIZ];
    uint32_t st;

    if (!rec)
        return RMS$_FAB;
    memset(&sf, 0, sizeof(sf));
    st = live_open(1, &h, &sf);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;

    /* The USERNAME primary key is embedded in the record at its seg-0 offset;
     * probe by that raw 32-byte key -> $UPDATE in place if present, else $PUT. */
    memcpy(key, ((const uint8_t *)rec) + SYSUAF_KEY_USERNAME_POS,
           SYSUAF_KEY_USERNAME_SIZ);
    {
        uint16_t rl = 0;
        uint32_t g = rms_p3_get_by_key(sf.ctx, SYSUAF_KRF_USERNAME,
                                       key, SYSUAF_KEY_USERNAME_SIZ, 0, 0,
                                       (uint8_t *)&existing,
                                       SYSUAF_UAF_RECORD_SIZE, &rl);
        if (g == RMS$_NORMAL)
            st = rms_p3_update(sf.ctx, SYSUAF_KRF_USERNAME,
                               key, SYSUAF_KEY_USERNAME_SIZ,
                               (const uint8_t *)rec, SYSUAF_UAF_RECORD_SIZE);
        else if (g == RMS$_RNF)
            st = rms_p3_put(sf.ctx, SYSUAF_KRF_USERNAME,
                            (const uint8_t *)rec, SYSUAF_UAF_RECORD_SIZE);
        else
            st = g;
    }

    live_close(h, &sf);
    return st;
}

uint32_t ovmx_sysuaf_write_all(const sysuaf_rms_record_t *recs, unsigned n)
{
    rms_file_t *h = NULL;
    sysuaf_rms_file_t sf;
    uint32_t st;
    unsigned i;

    if (!recs && n)
        return RMS$_FAB;
    memset(&sf, 0, sizeof(sf));

    h = rms_open_named_handle(SYSUAF_LIVE_PATH, 1, 1, &st);
    if (!h)
        return $VMS_STATUS_SUCCESS(st) ? RMS$_CRE : st;

    st = sysuaf_rms_create(h, &sf);
    if (st != RMS$_CREATED) {
        rms_close_named_handle(h);
        return st;
    }

    for (i = 0; i < n; i++) {
        st = sysuaf_put_record(&sf, &recs[i]);
        if (!$VMS_STATUS_SUCCESS(st)) {
            live_close(h, &sf);
            return st;
        }
    }
    live_close(h, &sf);
    return RMS$_NORMAL;
}
