/*
 * rightslist_live.c - LIVE $ASCTOID / $IDTOASC over the binary $RDBDEF
 * RIGHTSLIST (vms-f15a, epic vms-d0c). See rightslist_live.h for the
 * layering seam, the protection-context rationale and the substrate rules.
 * This is the executive-context reader of the runtime rights database; the
 * ASCII colon-delimited facade (the old libvms rtl/rightslist.c scan) is
 * retired.
 */

#define _POSIX_C_SOURCE 200809L

#include <string.h>

#include "rightslist_live.h"
#include "rightslist_rms.h"
#include "rms_io.h"
#include "rmsdef.h"
#include "ssdef.h"
#include "stsdef.h"          /* $VMS_STATUS_SUCCESS                            */
#include "ovmx_layout.h"     /* VMS_RIGHTSLIST_PATH                            */

#define RIGHTSLIST_LIVE_PATH  VMS_RIGHTSLIST_PATH

/* ------------------------------------------------------------------ *
 * The testable core: resolve over an already-bound handle.
 * ------------------------------------------------------------------ */

uint32_t rightslist_live_asctoid_rf(rightslist_rms_file_t *rf,
                                    const char *name, uint32_t *value)
{
    rdb_identifier_record_t rec;
    uint32_t st;

    if (!rf || !name || !value)
        return SS$_BADPARAM;

    /* rightslist_get_by_name upcases/blank-pads the name into the NAME key. */
    st = rightslist_get_by_name(rf, name, &rec);
    if (st == RMS$_NORMAL) {
        *value = rdb_ident_value(&rec);
        return SS$_NORMAL;
    }
    /* No such identifier (or any non-hit) is the honest $ASCTOID miss. */
    return SS$_NOSUCHID;
}

uint32_t rightslist_live_idtoasc_rf(rightslist_rms_file_t *rf,
                                    uint32_t value, char *name, size_t bufsz)
{
    rdb_identifier_record_t rec;
    char tmp[RDB$K_NAME_LEN + 1];
    uint32_t st;

    if (!rf || !name || !bufsz)
        return SS$_BADPARAM;

    st = rightslist_get_by_value(rf, value, &rec);
    if (st == RMS$_NORMAL) {
        rdb_ident_name(&rec, tmp);          /* trim the blank-padded 32-byte name */
        strncpy(name, tmp, bufsz - 1);
        name[bufsz - 1] = '\0';
        return SS$_NORMAL;
    }
    return SS$_NOSUCHID;
}

/* ------------------------------------------------------------------ *
 * Executive open of SYS$SYSTEM:RIGHTSLIST.DAT + resolve.
 * ------------------------------------------------------------------ */

/* Bind the rights database read-only for a resolution. Returns the open handle
 * in *hp and a bound rightslist_rms_file_t in *rf; the caller closes with
 * live_close(). Any open/bind failure returns a non-success status. */
static uint32_t live_open(rms_file_t **hp, rightslist_rms_file_t *rf)
{
    uint32_t st = RMS$_FAB;
    rms_file_t *h = rms_open_named_handle(RIGHTSLIST_LIVE_PATH, 0, 0, &st);
    if (!h)
        return $VMS_STATUS_SUCCESS(st) ? RMS$_FNF : st;

    st = rightslist_rms_open(h, rf);
    if (!$VMS_STATUS_SUCCESS(st)) {
        rms_close_named_handle(h);
        return st;
    }
    *hp = h;
    return RMS$_NORMAL;
}

static void live_close(rms_file_t *h, rightslist_rms_file_t *rf)
{
    if (rf)
        rightslist_rms_close(rf);
    if (h)
        rms_close_named_handle(h);
}

uint32_t ovmx_rightslist_asctoid(const char *name, uint32_t *value)
{
    rms_file_t *h = NULL;
    rightslist_rms_file_t rf;
    uint32_t st;

    if (!name || !value)
        return SS$_BADPARAM;
    memset(&rf, 0, sizeof(rf));

    st = live_open(&h, &rf);
    if (!$VMS_STATUS_SUCCESS(st))
        return SS$_NOSUCHID;         /* unreachable rights database -> the miss */

    st = rightslist_live_asctoid_rf(&rf, name, value);
    live_close(h, &rf);
    return st;
}

uint32_t ovmx_rightslist_idtoasc(uint32_t value, char *name, size_t bufsz)
{
    rms_file_t *h = NULL;
    rightslist_rms_file_t rf;
    uint32_t st;

    if (!name || !bufsz)
        return SS$_BADPARAM;
    memset(&rf, 0, sizeof(rf));

    st = live_open(&h, &rf);
    if (!$VMS_STATUS_SUCCESS(st))
        return SS$_NOSUCHID;

    st = rightslist_live_idtoasc_rf(&rf, value, name, bufsz);
    live_close(h, &rf);
    return st;
}
