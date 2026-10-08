/*
 * rightslist_live.c - LIVE $ASCTOID / $IDTOASC over the binary $RDBDEF
 * RIGHTSLIST (vms-f15a, epic vms-d0c). See rightslist_live.h for the
 * layering seam, the protection-context rationale and the substrate rules.
 * This is the executive-context reader of the runtime rights database; the
 * ASCII colon-delimited facade (the old libvms rtl/rightslist.c scan) is
 * retired.
 */

#define _POSIX_C_SOURCE 200809L

/*
 * OVMX userspace service register (rd vms-5b4):
 * OVMX-PARTIAL: sys$add_ident (vms-7d5a) -- exec: RIGHTSLIST.DAT is read and
 *     written over the executive Files-11 ACP, which checks the caller's access.
 * OVMX-LOCAL: sys$add_ident -- the record is composed and the next general
 *     identifier chosen in this process (one above the highest %X8001xxxx).
 * OVMX-PARTIAL: sys$rem_ident (vms-7d5a) -- exec: the same ACP file access.
 * OVMX-LOCAL: sys$rem_ident -- the identifier's records are found in this process.
 * OVMX-PARTIAL: sys$add_holder (vms-7d5a) -- exec: the same ACP file access.
 * OVMX-LOCAL: sys$add_holder -- the existence checks run in this process.
 * OVMX-PARTIAL: sys$rem_holder (vms-7d5a) -- exec: the same ACP file access.
 * OVMX-LOCAL: sys$rem_holder -- the holder record is found in this process.
 * OVMX-PARTIAL: sys$find_holder (vms-7d5a) -- exec: RIGHTSLIST.DAT read over the ACP.
 * OVMX-LOCAL: sys$find_holder -- the context is a position kept by the caller.
 * OVMX-PARTIAL: sys$find_held (vms-7d5a) -- exec: RIGHTSLIST.DAT read over the ACP.
 * OVMX-LOCAL: sys$find_held -- the context is a position kept by the caller.
 * OVMX-USERSPACE: sys$finish_rdb (vms-7d5a) -- clears the caller's position;
 *     there is no stream held open between calls.
 */
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

/* ======================================================================
 * The rights-database WRITE services and holder queries (rd vms-7d5a):
 * $ADD_IDENT $REM_IDENT $ADD_HOLDER $REM_HOLDER $FIND_HOLDER $FIND_HELD
 * $FINISH_RDB. Statuses are those the OpenVMS VAX V7.3 / Alpha V8.4 probe
 * returns (docs/oracle/semantics/rights). The database is SYS$SYSTEM:
 * RIGHTSLIST.DAT, opened in the caller's context: what may change it is
 * decided by its file protection in the executive ACP, as on VMS (the probe:
 * the writes need no privilege, only access to the file).
 *
 * UPGRADE. A RIGHTSLIST written before the HOLDER key (two keys, NAME at key 1)
 * is rewritten as a new version with the three oracle keys the first time a
 * write needs it -- every record carried over -- the way a SYSUAF record of an
 * older layout is converted when it is next written.
 * ====================================================================== */
#include <stdlib.h>
#include "descrip.h"

#define RDB_SS_DUPLNAM   0x0094u   /* SS$_DUPLNAM  */
#define RDB_SS_DUPIDENT  0x222Cu   /* SS$_DUPIDENT */
#define RDB_SS_IVIDENT   0x2224u   /* SS$_IVIDENT  */
#define RDB_SS_IVCHAN    0x013Cu   /* SS$_IVCHAN   */
#define RDB_GENERAL_BASE 0x80010000u

/* Open RIGHTSLIST for write, upgrading an older two-key file first. */
static uint32_t live_open_write(rms_file_t **hp, rightslist_rms_file_t *rf)
{
    uint32_t st = RMS$_FAB;
    rms_file_t *h = rms_open_named_handle(RIGHTSLIST_LIVE_PATH, 1, 0, &st);
    rms_file_t *nh;
    rightslist_rms_file_t nrf;

    if (!h)
        return $VMS_STATUS_SUCCESS(st) ? RMS$_FNF : st;
    st = rightslist_rms_open(h, rf);
    if (!$VMS_STATUS_SUCCESS(st)) {
        rms_close_named_handle(h);
        return st;
    }
    if (rf->ctx && h->writable)
        rf->ctx->writable = 1;
    if (rightslist_rms_is_current(rf)) {
        *hp = h;
        return RMS$_NORMAL;
    }

    /* ---- upgrade: a new version with the three oracle keys, every record
     * carried over, then the write goes there ---- */
    nh = rms_open_named_handle(RIGHTSLIST_LIVE_PATH, 1, 1, &st);
    if (!nh) {
        live_close(h, rf);
        return $VMS_STATUS_SUCCESS(st) ? RMS$_CRE : st;
    }
    memset(&nrf, 0, sizeof(nrf));
    st = rightslist_rms_create(nh, &nrf);
    if (st == RMS$_CREATED)
        st = rightslist_copy(rf, &nrf);
    live_close(h, rf);
    if (!$VMS_STATUS_SUCCESS(st)) {
        live_close(nh, &nrf);
        return st;
    }
    *rf = nrf;
    *hp = nh;
    return RMS$_NORMAL;
}

/* Copy a descriptor's name (blank-trimmed) into a C string. 0 when empty or too long. */
static size_t desc_name(const struct dsc$descriptor_s *d, char *out, size_t cap)
{
    size_t n;
    if (!d || !d->dsc$a_pointer)
        return 0;
    n = d->dsc$w_length;
    while (n > 0 && d->dsc$a_pointer[n - 1] == ' ')
        n--;
    if (n == 0 || n >= cap)
        return 0;
    memcpy(out, d->dsc$a_pointer, n);
    out[n] = '\0';
    return n;
}

struct max_general { uint32_t max; };
static int max_general_cb(const uint8_t *rec, uint16_t rec_len, void *arg)
{
    struct max_general *m = (struct max_general *)arg;
    uint32_t v = p3_le32(rec);
    if (rec_len >= RDB$K_IDENT_RECORD_SIZE && (v & 0xFFFF0000u) == RDB_GENERAL_BASE && v > m->max)
        m->max = v;
    return 0;
}

uint32_t sys$add_ident(const struct dsc$descriptor_s *name, uint32_t id, uint32_t attrib,
                       uint32_t *resid)
{
    char nm[RDB$K_NAME_LEN + 1];
    rms_file_t *h = NULL;
    rightslist_rms_file_t rf;
    rdb_identifier_record_t rec, cur;
    uint32_t st;

    if (!desc_name(name, nm, sizeof(nm)))
        return RDB_SS_IVIDENT;
    memset(&rf, 0, sizeof(rf));
    st = live_open_write(&h, &rf);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;
    if (rightslist_get_by_name(&rf, nm, &cur) == RMS$_NORMAL) {
        st = RDB_SS_DUPLNAM;
        goto out;
    }
    if (id == 0) {
        struct max_general m = { RDB_GENERAL_BASE };
        (void)rightslist_enum(&rf, max_general_cb, &m);
        id = m.max + 1u;                       /* the next general identifier */
    } else if (rightslist_get_by_value(&rf, id, &cur) == RMS$_NORMAL) {
        st = RDB_SS_DUPIDENT;
        goto out;
    }
    memset(&rec, 0, sizeof(rec));
    rdb_ident_set_value(&rec, id);
    rdb_ident_set_attributes(&rec, attrib);
    rdb_ident_set_name(&rec, nm);
    st = rightslist_put_identifier(&rf, &rec);
    if (st == RMS$_DUP)
        st = RDB_SS_DUPIDENT;
    else if ($VMS_STATUS_SUCCESS(st)) {
        st = SS$_NORMAL;
        if (resid)
            *resid = id;
    }
out:
    live_close(h, &rf);
    return st;
}

uint32_t sys$rem_ident(uint32_t id)
{
    rms_file_t *h = NULL;
    rightslist_rms_file_t rf;
    uint32_t st;

    memset(&rf, 0, sizeof(rf));
    st = live_open_write(&h, &rf);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;
    st = rightslist_delete_identifier(&rf, id);
    st = (st == RMS$_NORMAL) ? SS$_NORMAL : (st == RMS$_RNF) ? SS$_NOSUCHID : st;
    live_close(h, &rf);
    return st;
}

/* The `want`-th (0-based) holder record matching `by_id`/`by_holder`, in key-0 order. */
struct find_state {
    int by_holder;          /* 0: holders of `id`; 1: identifiers held by `holder` */
    uint32_t id, hlo, hhi;
    unsigned want, seen;
    int found, id_exists;
    uint8_t rec[RDB$K_HOLDER_RECORD_SIZE];
};
static int find_cb(const uint8_t *rec, uint16_t rec_len, void *arg)
{
    struct find_state *f = (struct find_state *)arg;
    int hit;
    if (rec_len >= RDB$K_IDENT_RECORD_SIZE) {
        if (!f->by_holder && p3_le32(rec) == f->id)
            f->id_exists = 1;
        return 0;
    }
    if (rec_len < RDB$K_HOLDER_RECORD_SIZE)
        return 0;
    hit = f->by_holder
        ? (p3_le32(rec + RDB$K_HOLDER_OFF) == f->hlo && p3_le32(rec + RDB$K_HOLDER_OFF + 4) == f->hhi)
        : (p3_le32(rec) == f->id);
    if (!hit)
        return 0;
    if (f->seen++ == f->want) {
        memcpy(f->rec, rec, sizeof(f->rec));
        f->found = 1;
        return 1;
    }
    return 0;
}

/* A holder record {id, holder}: does it exist? */
static int find_pair_cb(const uint8_t *rec, uint16_t rec_len, void *arg)
{
    struct find_state *f = (struct find_state *)arg;
    if (rec_len >= RDB$K_HOLDER_RECORD_SIZE && rec_len < RDB$K_IDENT_RECORD_SIZE &&
        p3_le32(rec) == f->id && p3_le32(rec + RDB$K_HOLDER_OFF) == f->hlo &&
        p3_le32(rec + RDB$K_HOLDER_OFF + 4) == f->hhi) {
        f->found = 1;
        return 1;
    }
    return 0;
}

uint32_t sys$add_holder(uint32_t id, const uint32_t *holder, uint32_t attrib)
{
    rms_file_t *h = NULL;
    rightslist_rms_file_t rf;
    rdb_identifier_record_t cur;
    rdb_holder_record_t hr;
    uint32_t st;

    if (!holder)
        return SS$_BADPARAM;
    memset(&rf, 0, sizeof(rf));
    st = live_open_write(&h, &rf);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;
    /* the identifier, and the holder (a UIC identifier), must both exist */
    if (rightslist_get_by_value(&rf, id, &cur) != RMS$_NORMAL ||
        rightslist_get_by_value(&rf, holder[0], &cur) != RMS$_NORMAL) {
        st = SS$_NOSUCHID;
        goto out;
    }
    /* a holder holds an identifier once */
    {
        struct find_state f;
        memset(&f, 0, sizeof(f));
        f.by_holder = 1;
        f.hlo = holder[0];
        f.hhi = holder[1];
        f.want = 0xFFFFFFFFu;                   /* count them all */
        f.id = id;
        (void)rightslist_enum(&rf, find_pair_cb, &f);
        if (f.found) {
            st = RDB_SS_DUPIDENT;
            goto out;
        }
    }
    rdb_holder_set(&hr, id, attrib, holder[0], holder[1]);
    st = rightslist_put_holder(&rf, &hr);
    if ($VMS_STATUS_SUCCESS(st))
        st = SS$_NORMAL;
out:
    live_close(h, &rf);
    return st;
}

uint32_t sys$rem_holder(uint32_t id, const uint32_t *holder)
{
    rms_file_t *h = NULL;
    rightslist_rms_file_t rf;
    rdb_identifier_record_t cur;
    uint32_t st;

    if (!holder)
        return SS$_BADPARAM;
    memset(&rf, 0, sizeof(rf));
    st = live_open_write(&h, &rf);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;
    if (rightslist_get_by_value(&rf, id, &cur) != RMS$_NORMAL)
        st = SS$_NOSUCHID;
    else {
        st = rightslist_delete_holder(&rf, id, holder[0], holder[1]);
        st = (st == RMS$_NORMAL) ? SS$_NORMAL : (st == RMS$_RNF) ? SS$_NOSUCHID : st;
    }
    live_close(h, &rf);
    return st;
}

static uint32_t rdb_find(struct find_state *f, uint32_t *contxt)
{
    rms_file_t *h = NULL;
    rightslist_rms_file_t rf;
    uint32_t st;

    f->want = contxt ? *contxt : 0;
    memset(&rf, 0, sizeof(rf));
    st = live_open(&h, &rf);
    if (!$VMS_STATUS_SUCCESS(st))
        return SS$_NOSUCHID;
    st = rightslist_enum(&rf, find_cb, f);
    live_close(h, &rf);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;
    if (!f->found) {
        if (contxt)
            *contxt = 0;                       /* the walk is over: context cleared */
        return SS$_NOSUCHID;
    }
    if (contxt)
        *contxt = f->want + 1u;
    return SS$_NORMAL;
}

uint32_t sys$find_holder(uint32_t id, uint32_t *holder, uint32_t *attrib, uint32_t *contxt)
{
    struct find_state f;
    uint32_t st;
    memset(&f, 0, sizeof(f));
    f.id = id;
    st = rdb_find(&f, contxt);
    if (st == SS$_NORMAL) {
        if (holder) { holder[0] = p3_le32(f.rec + RDB$K_HOLDER_OFF); holder[1] = p3_le32(f.rec + RDB$K_HOLDER_OFF + 4); }
        if (attrib) *attrib = p3_le32(f.rec + RDB$K_ATTRIB_OFF);
    }
    return st;
}

uint32_t sys$find_held(const uint32_t *holder, uint32_t *id, uint32_t *attrib, uint32_t *contxt)
{
    struct find_state f;
    uint32_t st;
    if (!holder)
        return SS$_BADPARAM;
    memset(&f, 0, sizeof(f));
    f.by_holder = 1;
    f.hlo = holder[0];
    f.hhi = holder[1];
    st = rdb_find(&f, contxt);
    if (st == SS$_NORMAL) {
        if (id) *id = p3_le32(f.rec);
        if (attrib) *attrib = p3_le32(f.rec + RDB$K_ATTRIB_OFF);
    }
    return st;
}

uint32_t sys$finish_rdb(uint32_t *contxt)
{
    if (!contxt || *contxt == 0)
        return RDB_SS_IVCHAN;                  /* no rights-database stream open */
    *contxt = 0;
    return SS$_NORMAL;
}

/* An identifier's attributes (its definition record), for $ASCTOID/$IDTOASC. */
uint32_t ovmx_rightslist_attributes(uint32_t value, uint32_t *attrib)
{
    rms_file_t *h = NULL;
    rightslist_rms_file_t rf;
    rdb_identifier_record_t rec;
    uint32_t st;

    if (!attrib)
        return SS$_BADPARAM;
    memset(&rf, 0, sizeof(rf));
    st = live_open(&h, &rf);
    if (!$VMS_STATUS_SUCCESS(st))
        return SS$_NOSUCHID;
    st = rightslist_get_by_value(&rf, value, &rec);
    live_close(h, &rf);
    if (st != RMS$_NORMAL)
        return SS$_NOSUCHID;
    *attrib = rdb_ident_attributes(&rec);
    return SS$_NORMAL;
}
