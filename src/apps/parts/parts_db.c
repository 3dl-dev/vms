/*
 * PARTS_DB.C - RMS indexed-file access layer for the PARTS demo.
 *
 * A thin, single-user wrapper over the Tier-1/2 RMS system services. It owns
 * one FAB, one RAB and one primary-key XABKEY per open file, and exposes
 * create / open / put / keyed-get / close. No Tier-3+ APIs, no record locking
 * (unwired in OVMX, bead vms-407) - the file is opened for exclusive use.
 *
 * Clean-room note: this is ORIGINAL OVMX application code written against the
 * public RMS control-block headers under src/vmsrms/include/rms/; it copies no
 * VMS source.
 */

#include <stdlib.h>
#include <string.h>

#include "rms/rms.h"      /* sys$create/$open/$connect/$put/$get/$close ...   */
#include "rms/fab.h"
#include "rms/rab.h"
#include "rms/xab.h"

#include "parts.h"

/*
 * The three RMS control blocks for one open file. Kept in one allocation so
 * the parts_db handle only carries pointers into it. The XABKEY defines the
 * primary key: string, at record offset 0, PARTS_KEY_SIZE bytes, single
 * segment, no duplicates.
 */
struct parts_rms {
    struct FAB    fab;
    struct RAB    rab;
    struct XABKEY xab;
};

/* Secondary status (errno) of the last create/open attempt - diagnostic. */
uint32_t parts_db_last_stv = 0;

/* Fill in the primary-key XABKEY (offset 0, PARTS_KEY_SIZE, string, no dup). */
static void init_key(struct XABKEY *xab)
{
    memset(xab, 0, sizeof(*xab));
    xab->xab$b_cod  = XAB$C_KEY;
    xab->xab$b_bln  = sizeof(struct XABKEY);
    xab->xab$b_ref  = 0;              /* primary key            */
    xab->xab$b_dtp  = XAB$C_STG;      /* string key             */
    xab->xab$w_flg  = 0;             /* no duplicates allowed  */
    xab->xab$w_pos0 = 0;             /* key at record offset 0 */
    xab->xab$b_siz0 = PARTS_KEY_SIZE;
    xab->xab$b_nseg = 1;
    xab->xab$w_tks  = PARTS_KEY_SIZE;
    xab->xab$l_nxt  = NULL;
}

/*
 * Common FAB setup for an indexed, fixed-length-record file. `create` selects
 * create-time options (exclusive access) vs open-time (get access).
 */
static void init_fab(struct FAB *fab, struct XABKEY *xab,
                     const char *filespec, int create)
{
    memset(fab, 0, sizeof(*fab));
    fab->fab$b_bid = FAB$C_BID;
    fab->fab$b_bln = (uint8_t)sizeof(struct FAB);   /* VMS bln is a byte field */
    fab->fab$b_org = FAB$C_IDX;                 /* indexed               */
    fab->fab$b_rfm = FAB$C_FIX;                 /* fixed-length records   */
    fab->fab$w_mrs = PARTS_REC_SIZE;            /* record size           */
    fab->fab$b_fac = create ? (FAB$M_PUT | FAB$M_GET) : FAB$M_GET;
    fab->fab$b_shr = FAB$M_NIL;                 /* single-user, no share  */
    fab->fab$l_fna = (char *)filespec;
    fab->fab$b_fns = (uint8_t)strlen(filespec);
    fab->fab$l_xab = xab;
    fab->_rms_file = 0;   /* vms-bc7: RMS handle (was _linux_fd) */
}

static void init_rab(struct RAB *rab, struct FAB *fab)
{
    memset(rab, 0, sizeof(*rab));
    rab->rab$b_bid = RAB$C_BID;
    rab->rab$b_bln = (uint8_t)sizeof(struct RAB);
    rab->rab$l_fab = fab;
    rab->rab$b_rac = RAB$C_SEQ;                 /* default; get sets KEY   */
}

static uint32_t db_setup(struct parts_db *db, const char *filespec, int create)
{
    struct parts_rms *r = calloc(1, sizeof(*r));
    if (!r)
        return RMS$_DME;

    init_key(&r->xab);
    init_fab(&r->fab, &r->xab, filespec, create);
    init_rab(&r->rab, &r->fab);

    uint32_t st = create ? sys$create(&r->fab, 0, 0) : sys$open(&r->fab, 0, 0);
    if (!$VMS_STATUS_SUCCESS(st)) {
        parts_db_last_stv = r->fab.fab$l_stv;   /* errno, for diagnosis */
        free(r);
        return st;
    }

    st = sys$connect(&r->rab, 0, 0);
    if (!$VMS_STATUS_SUCCESS(st)) {
        sys$close(&r->fab, 0, 0);
        free(r);
        return st;
    }

    db->fab  = &r->fab;
    db->rab  = &r->rab;
    db->xab  = &r->xab;
    db->open = 1;
    strncpy(db->spec, filespec, sizeof(db->spec) - 1);
    db->spec[sizeof(db->spec) - 1] = '\0';
    return st;
}

uint32_t parts_db_create(struct parts_db *db, const char *filespec)
{
    memset(db, 0, sizeof(*db));
    return db_setup(db, filespec, 1);
}

uint32_t parts_db_open(struct parts_db *db, const char *filespec)
{
    memset(db, 0, sizeof(*db));
    return db_setup(db, filespec, 0);
}

uint32_t parts_db_put(struct parts_db *db, const struct part_record *rec)
{
    struct RAB *rab = (struct RAB *)db->rab;
    rab->rab$b_rac = RAB$C_SEQ;
    rab->rab$l_rbf = (char *)rec;
    rab->rab$w_rsz = PARTS_REC_SIZE;
    return sys$put(rab, 0, 0);
}

uint32_t parts_db_get(struct parts_db *db, const char *part_number,
                      struct part_record *rec_out)
{
    struct RAB *rab = (struct RAB *)db->rab;
    rab->rab$b_rac = RAB$C_KEY;                 /* keyed random access     */
    rab->rab$b_krf = 0;                         /* primary key of reference*/
    rab->rab$l_kbf = (char *)part_number;       /* 8-byte key value        */
    rab->rab$b_ksz = PARTS_KEY_SIZE;
    rab->rab$l_rop = 0;                          /* exact match             */
    rab->rab$l_ubf = (char *)rec_out;
    rab->rab$w_usz = PARTS_REC_SIZE;
    return sys$get(rab, 0, 0);
}

void parts_db_close(struct parts_db *db)
{
    if (!db->open)
        return;
    struct RAB *rab = (struct RAB *)db->rab;
    struct FAB *fab = (struct FAB *)db->fab;
    sys$disconnect(rab, 0, 0);
    sys$close(fab, 0, 0);
    /* fab is the head of the single allocation made in db_setup(). */
    free(fab);
    db->fab = db->rab = db->xab = NULL;
    db->open = 0;
}

void parts_make_key(char *key_out, unsigned ordinal)
{
    /* "PNnnnnnn" - exactly PARTS_KEY_SIZE (8) bytes, no NUL terminator. */
    static const char digits[] = "0123456789";
    key_out[0] = 'P';
    key_out[1] = 'N';
    for (int i = PARTS_KEY_SIZE - 1; i >= 2; i--) {
        key_out[i] = digits[ordinal % 10];
        ordinal /= 10;
    }
}
