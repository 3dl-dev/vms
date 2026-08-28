/*
 * test_syssvc_rms_p3_acp.c - the ACP end-to-end proof the READ rung could not
 * do alone (vms-045, epic vms-d0c): a GENUINE Files-11 Prolog-3 indexed file is
 * AUTHORED, populated (forcing a real data-bucket SPLIT), and read back BY KEY
 * over a REAL /dev/vms -- every block transfer is IO$_WRITEVBLK / IO$_READVBLK
 * on an ACCESSED file channel window, no RMS$_ORG, no /vms passthrough, no
 * .rms_idx sidecar (Rule 9 / INV-6).
 *
 * WHAT THIS PROVES, through the runtime write engine (src/vmsrms/rms_prolog3.c:
 * rms_p3_create / rms_p3_put / rms_p3_get_by_key) against the real-VAX ODS-2
 * fixture the harness mounts WRITABLE on DKA0::
 *
 *   1. IO$_CREATE lands a versioned directory entry for a fresh file in
 *      [OVMXDIR] and IO$_ACCESS builds a WRITE window on the channel.
 *   2. rms_p3_create writes the Prolog-3 PROLOGUE (fixed prolog + key descriptor
 *      + area descriptor), the root index bucket (Root Level 1) and the first
 *      data bucket, all via IO$_WRITEVBLK on that window -- the bytes hit the
 *      platter, not a per-process buffer.
 *   3. rms_p3_put inserts records until a data bucket fills and SPLITS: a new
 *      bucket is allocated (implicit EXTEND from BITMAP.SYS), records are
 *      redistributed by key, an RRV stub is left behind, and the index gains a
 *      2-byte child pointer -- genuine ISAM maintenance, on disk.
 *   4. rms_p3_get_by_key reads EVERY record back BY KEY via IO$_READVBLK,
 *      byte-exact, across the split -- writer<->reader agreement over the ACP.
 *   5. After DEACCESS + re-ACCESS (window rebuilt from the on-disk FH2, blocks
 *      re-read) the records STILL read back -- proving durability on the volume.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass (Rule 9): the ACP, the
 * window and the transfer are executive-resident, so with no /dev/vms there is
 * nothing to assert (the contract every test_syssvc_* suite is held to).
 *
 * ISOLATION: a UNIQUELY named file in [OVMXDIR], IO$_DELETEd before exit, on a
 * per-boot writable fixture copy (run_tests.sh) -- same discipline as
 * test_syssvc_acp_create.c / test_syssvc_acp_rw.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vmsfs/ods2.h"      /* ODS2_FK_* file-kind selectors for CREATE */
#include "rms_prolog3.h"     /* the write+read engine + rms_file_t handle */
#include "rmsdef.h"

#define EXIT_SKIP        77
#define ODS2_UNIT        "DKA0:"
#define OVMXDIR_FID_NUM  11u          /* [OVMXDIR] directory FID (fixture) */
#define TESTNAME         "P3IDX.TST"

#define KEY_SIZE 8u
#define NREC     40                   /* forces several 1-block-bucket splits */
#define SECNAME  "P3SEC.TST"          /* the secondary-key scenario file */

/* Secondary-key (SIDR) scenario record: [16 username][4 UIC][8 payload]. */
#define SEC_USERKEY 16u
#define SEC_UICPOS  16u
#define SEC_UICKEY  4u
#define SEC_RECLEN  28u
#define SEC_NREC    40                /* forces primary splits under the SIDR */

static int pass = 0;
static int fail = 0;

static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

static void make_record(int n, uint8_t *rec, uint16_t *len)
{
    char key[16], pay[64];
    int kn = snprintf(key, sizeof(key), "K%06d", n);
    int pn;
    memset(rec, ' ', KEY_SIZE);
    memcpy(rec, key, (size_t)kn < KEY_SIZE ? (size_t)kn : KEY_SIZE);
    pn = snprintf(pay, sizeof(pay), "value-for-%06d-payload", n);
    memcpy(rec + KEY_SIZE, pay, (size_t)pn);
    *len = (uint16_t)(KEY_SIZE + pn);
}

static void key_of(int n, uint8_t *k)
{
    uint8_t rec[64]; uint16_t l;
    make_record(n, rec, &l);
    memcpy(k, rec, KEY_SIZE);
}

/* Seed an rms_file_t over the ACP channel `chan` from a CREATE+ACCESS result. */
static void seed_handle(rms_file_t *h, uint32_t chan,
                        const struct vms_acp_fileop_args *f)
{
    memset(h, 0, sizeof(*h));
    h->chan      = chan;
    h->assigned  = 1;
    h->accessed  = 1;
    h->writable  = 1;
    h->fd        = -1;                 /* ACP handle -> rms_io_* uses the window */
    h->fid_num   = f->fid_num;
    h->fid_seq   = f->fid_seq;
    h->fid_rvn   = f->fid_rvn;
    h->fid_nmx   = f->fid_nmx;
    h->hiblk     = f->new_hiblk;
    h->eof       = (uint64_t)(f->new_efblk ? (f->new_efblk - 1u) : 0) * 512u
                   + f->new_ffbyte;
}

/* --- secondary-key (SIDR) scenario helpers --- */
static uint32_t sec_uic_of(int n)
{
    if (n == 5 || n == 11) n = 2;              /* dup group {2,5,11} share a UIC */
    return 0x00010000u + (uint32_t)n;
}
static void sec_make(int n, uint8_t *rec)
{
    char u[24], d[16];
    int un = snprintf(u, sizeof(u), "ACCT%05d", n);
    memset(rec, ' ', SEC_USERKEY);
    memcpy(rec, u, (size_t)un < SEC_USERKEY ? (size_t)un : SEC_USERKEY);
    p3_put_le32(rec + SEC_UICPOS, sec_uic_of(n));
    memset(rec + 20, 0, 8);
    snprintf(d, sizeof(d), "D%05d", n);
    memcpy(rec + 20, d, 7);
}

/* Author [OVMXDIR]P3SEC.TST on `chan`: a primary (username) key + a UIC
 * secondary index, $PUT SEC_NREC records over IO$_WRITEVBLK, then read them BY
 * THE SECONDARY KEY over IO$_READVBLK (secondary index -> SIDR -> primary RFA),
 * exercise the duplicate-UIC pointer array, and $DELETE (SIDR purge). Every
 * block transfer is executive-resident; nothing here fakes a lookup. */
static void run_secondary_scenario(uint32_t chan)
{
    struct vms_acp_fileop_args f;
    struct vms_acp_access_args a;
    rms_file_t h;
    p3_ctx_t *ctx = NULL;
    p3_create_params_t cp, sp;
    uint32_t st;
    int i, order[SEC_NREC];

    /* fresh writable file */
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_DELETE; f.modifiers = VMS_ACP_M_DELETE;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1; f.version = 0;
    strncpy(f.name, SECNAME, VMS_ACP_NAME_SIZE - 1);
    (void)vms_kif_acp_fileop(&f);

    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_CREATE;
    f.modifiers = VMS_ACP_M_CREATE | VMS_ACP_M_ACCESS;
    f.acctl = VMS_ACP_ACCTL_WRITE; f.kind = ODS2_FK_DATA;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1; f.version = 0;
    strncpy(f.name, SECNAME, VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_fileop(&f);
    check($VMS_STATUS_SUCCESS(st), "IO$_CREATE [OVMXDIR]P3SEC.TST");
    if (!$VMS_STATUS_SUCCESS(st)) return;
    seed_handle(&h, chan, &f);

    memset(&cp, 0, sizeof(cp));
    cp.key_size = SEC_USERKEY; cp.seg0_pos = 0; cp.seg0_siz = SEC_USERKEY;
    cp.bkt_blocks = 1; cp.allow_dup = 0;
    st = rms_p3_create(&h, &cp, &ctx);
    check(st == RMS$_CREATED && ctx, "rms_p3_create (primary key) over the ACP");
    if (!ctx) { (void)vms_kif_acp_deaccess(chan); return; }

    memset(&sp, 0, sizeof(sp));
    sp.key_size = SEC_UICKEY; sp.seg0_pos = SEC_UICPOS; sp.seg0_siz = SEC_UICKEY;
    sp.bkt_blocks = 1; sp.allow_dup = 1;
    st = rms_p3_add_secondary_key(ctx, &sp);
    check(st == RMS$_NORMAL && ctx->num_keys == 2,
          "rms_p3_add_secondary_key (UIC index) writes its prologue on disk");

    for (i = 0; i < SEC_NREC; i++) order[i] = i;
    for (i = SEC_NREC - 1; i > 0; i--) {
        int j = (int)(((unsigned)(i * 2654435761u)) % (unsigned)(i + 1));
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }
    {
        int put_ok = 1;
        for (i = 0; i < SEC_NREC; i++) {
            uint8_t rec[SEC_RECLEN];
            sec_make(order[i], rec);
            if (!$VMS_STATUS_SUCCESS(rms_p3_put(ctx, 0, rec, SEC_RECLEN))) put_ok = 0;
        }
        check(put_ok, "all $PUTs land (primary + SIDR maintenance) over IO$_WRITEVBLK");
    }

    /* read every distinct-UIC record BY THE SECONDARY KEY over the ACP */
    {
        int ok = 1;
        for (i = 0; i < SEC_NREC; i++) {
            uint8_t uk[SEC_UICKEY], buf[64], want[SEC_RECLEN]; uint16_t rl = 0;
            if (i == 5 || i == 11) continue;
            p3_put_le32(uk, sec_uic_of(i)); sec_make(i, want);
            if (!($VMS_STATUS_SUCCESS(rms_p3_get_by_key(ctx, 1, uk, SEC_UICKEY,
                    0, 0, buf, sizeof(buf), &rl)) && rl == SEC_RECLEN &&
                  memcmp(buf, want, SEC_RECLEN) == 0)) ok = 0;
        }
        check(ok, "records resolve BY UIC over the ACP (index->SIDR->primary RFA, "
                  "across primary splits)");
    }

    /* duplicate UIC: SIDR carries {2,5,11} */
    {
        uint8_t uk[SEC_UICKEY]; p3_rfa_t rfa[8]; uint16_t cnt = 0;
        int seen2 = 0, seen5 = 0, seen11 = 0;
        p3_put_le32(uk, sec_uic_of(2));
        st = rms_p3_sidr_lookup(ctx, 1, uk, SEC_UICKEY, rfa, 8, &cnt);
        check($VMS_STATUS_SUCCESS(st) && cnt == 3, "duplicate-UIC SIDR has 3 pointers");
        for (i = 0; i < (int)cnt; i++) {
            uint8_t buf[64]; uint16_t rl = 0; int c;
            if (!$VMS_STATUS_SUCCESS(rms_p3_get_by_rfa(ctx, rfa[i].home_vbn,
                    rfa[i].home_recid, buf, sizeof(buf), &rl))) continue;
            for (c = 0; c < SEC_NREC; c++) {
                uint8_t w[SEC_RECLEN]; sec_make(c, w);
                if (rl == SEC_RECLEN && memcmp(buf, w, SEC_RECLEN) == 0) {
                    if (c == 2) seen2 = 1; else if (c == 5) seen5 = 1;
                    else if (c == 11) seen11 = 1;
                }
            }
        }
        check(seen2 && seen5 && seen11,
              "all 3 duplicate-UIC pointers resolve to the correct users over the ACP");
    }

    /* $DELETE purges the SIDR: delete user 5, dup array shrinks to {2,11} */
    {
        uint8_t k5[SEC_USERKEY], rec5[SEC_RECLEN];
        uint8_t uk[SEC_UICKEY]; p3_rfa_t rfa[8]; uint16_t cnt = 0;
        sec_make(5, rec5); memcpy(k5, rec5, SEC_USERKEY);
        check($VMS_STATUS_SUCCESS(rms_p3_delete(ctx, 0, k5, SEC_USERKEY)),
              "$DELETE user 5 over the ACP");
        p3_put_le32(uk, sec_uic_of(2));
        st = rms_p3_sidr_lookup(ctx, 1, uk, SEC_UICKEY, rfa, 8, &cnt);
        check($VMS_STATUS_SUCCESS(st) && cnt == 2,
              "SIDR pointer array shrank to 2 after $DELETE (SIDR purged on disk)");
    }

    /* durability: DEACCESS, re-ACCESS, re-bind, re-read one record by UIC */
    rms_p3_free(ctx); ctx = NULL;
    (void)vms_kif_acp_deaccess(chan);
    memset(&a, 0, sizeof(a));
    a.chan = chan; a.did_num = OVMXDIR_FID_NUM; a.did_seq = 1;
    strncpy(a.name, SECNAME, VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&a);
    check($VMS_STATUS_SUCCESS(st), "re-ACCESS P3SEC.TST");
    if ($VMS_STATUS_SUCCESS(st)) {
        p3_ctx_t *rc = NULL;
        memset(&h, 0, sizeof(h));
        h.chan = chan; h.assigned = 1; h.accessed = 1; h.writable = 0; h.fd = -1;
        h.eof = (uint64_t)(a.attr.efblk ? (a.attr.efblk - 1u) : 0) * 512u + a.attr.ffbyte;
        h.hiblk = a.attr.hiblk;
        if ($VMS_STATUS_SUCCESS(rms_p3_bind(&h, &rc)) && rc && rc->num_keys == 2) {
            uint8_t uk[SEC_UICKEY], buf[64], want[SEC_RECLEN]; uint16_t rl = 0;
            p3_put_le32(uk, sec_uic_of(7)); sec_make(7, want);
            check($VMS_STATUS_SUCCESS(rms_p3_get_by_key(rc, 1, uk, SEC_UICKEY,
                    0, 0, buf, sizeof(buf), &rl)) && rl == SEC_RECLEN &&
                  memcmp(buf, want, SEC_RECLEN) == 0,
                  "secondary index survives DEACCESS+re-ACCESS (durable SIDR on volume)");
            rms_p3_free(rc);
        } else {
            check(0, "re-bind 2-key prologue from disk");
        }
    }

    (void)vms_kif_acp_deaccess(chan);
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_DELETE; f.modifiers = VMS_ACP_M_DELETE;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1; f.version = 0;
    strncpy(f.name, SECNAME, VMS_ACP_NAME_SIZE - 1);
    (void)vms_kif_acp_fileop(&f);
}

/* Read every record back by key against ctx; returns 1 if all present+exact. */
static int read_all_by_key(p3_ctx_t *ctx)
{
    int n;
    for (n = 0; n < NREC; n++) {
        uint8_t key[KEY_SIZE], buf[128], want[64];
        uint16_t rl = 0, wl = 0;
        uint32_t gs;
        key_of(n, key);
        make_record(n, want, &wl);
        gs = rms_p3_get_by_key(ctx, 0, key, KEY_SIZE, 0, 0,
                               buf, sizeof(buf), &rl);
        if (!($VMS_STATUS_SUCCESS(gs) && rl == wl && memcmp(buf, want, wl) == 0)) {
            printf("    read-back miss K%06d -> 0x%X (rl=%u wl=%u)\n",
                   n, gs, rl, wl);
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    uint32_t chan = 0, st;
    struct vms_acp_fileop_args f;
    rms_file_t h;
    p3_ctx_t *ctx = NULL;
    p3_create_params_t cp;
    uint32_t alloc_after_create;
    int i;

    printf("test_syssvc_rms_p3_acp: genuine Prolog-3 indexed WRITE+READ over the ACP\n");

    if (!executive_present()) {
        printf("  SKIP: no /dev/vms (executive absent) -- nothing to assert\n");
        return EXIT_SKIP;
    }

    /* Provision the writable ACP volume the WRITE engine needs (vms-757). DKA0:
     * is a genuine real-VAX ODS-2 fixture the harness stages WRITABLE per boot
     * (run_tests.sh), but the executive-global mounted-volume table is per-boot
     * state a sibling suite may have left DISMOUNTED (each ACP suite $DMOUNTs on
     * exit). $ASSIGN of an unmounted unit fails-honest SS$_DEVNOTMOUNT
     * (vmsfs_acp.c vms_ioctl_acp_assign), which reddened the first assertion
     * before the engine ever ran. $MOUNT is idempotent (rms_acp / acp_rw /
     * acp_create all self-mount the same way) -- it VALIDATES the real Files-11
     * home block + SCB and records the volume executive-global, so the $ASSIGN
     * below reaches a genuine writable volume (never a fabricated channel). */
    st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "DKA0: mounted writable for the ACP (executive-global)");

    st = vms_kif_acp_assign(ODS2_UNIT, &chan);
    check($VMS_STATUS_SUCCESS(st), "$ASSIGN DKA0:");
    if (!$VMS_STATUS_SUCCESS(st)) { printf("  abort\n"); return 1; }

    /* best-effort: remove any stale copy from a prior aborted run */
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_DELETE; f.modifiers = VMS_ACP_M_DELETE;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1; f.version = 0;
    strncpy(f.name, TESTNAME, VMS_ACP_NAME_SIZE - 1);
    (void)vms_kif_acp_fileop(&f);

    /* ---- IO$_CREATE + IO$_ACCESS: a fresh writable ODS-2 file ---- */
    memset(&f, 0, sizeof(f));
    f.chan      = chan;
    f.func      = VMS_ACP_FOP_CREATE;
    f.modifiers = VMS_ACP_M_CREATE | VMS_ACP_M_ACCESS;
    f.acctl     = VMS_ACP_ACCTL_WRITE;
    f.kind      = ODS2_FK_DATA;               /* variable-record data file */
    f.did_num   = OVMXDIR_FID_NUM;
    f.did_seq   = 1;
    f.version   = 0;                          /* highest existing + 1 */
    strncpy(f.name, TESTNAME, VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_fileop(&f);
    check($VMS_STATUS_SUCCESS(st), "IO$_CREATE + IO$_ACCESS [OVMXDIR]P3IDX.TST");
    if (!$VMS_STATUS_SUCCESS(st)) { vms_kif_dassgn(chan); return 1; }

    seed_handle(&h, chan, &f);

    /* ---- rms_p3_create: author the Prolog-3 prologue over IO$_WRITEVBLK ---- */
    memset(&cp, 0, sizeof(cp));
    cp.key_size   = KEY_SIZE;
    cp.seg0_pos   = 0;
    cp.seg0_siz   = KEY_SIZE;
    cp.dtp        = 0;
    cp.bkt_blocks = 1;
    cp.allow_dup  = 0;
    st = rms_p3_create(&h, &cp, &ctx);
    check(st == RMS$_CREATED && ctx != NULL,
          "rms_p3_create authors the prologue on disk (IO$_WRITEVBLK)");
    if (!ctx) { (void)vms_kif_acp_deaccess(chan); vms_kif_dassgn(chan); return 1; }
    alloc_after_create = ctx->alloc_next;

    /* ---- $PUT NREC records (shuffled) -> forces a real bucket SPLIT ---- */
    {
        int order[NREC], put_ok = 1;
        for (i = 0; i < NREC; i++) order[i] = i;
        for (i = NREC - 1; i > 0; i--) {
            int j = (int)(((unsigned)(i * 2654435761u)) % (unsigned)(i + 1));
            int t = order[i]; order[i] = order[j]; order[j] = t;
        }
        for (i = 0; i < NREC; i++) {
            uint8_t rec[64]; uint16_t rl;
            uint32_t ps;
            make_record(order[i], rec, &rl);
            ps = rms_p3_put(ctx, 0, rec, rl);
            if (!$VMS_STATUS_SUCCESS(ps)) {
                printf("    $PUT %d -> 0x%X\n", order[i], ps);
                put_ok = 0;
            }
        }
        check(put_ok, "all NREC $PUTs land via IO$_WRITEVBLK");
    }
    check(ctx->alloc_next > alloc_after_create,
          "at least one genuine bucket SPLIT occurred (new bucket EXTENDed)");
    printf("    buckets: create=%u now=%u (%u split(s), on-disk)\n",
           alloc_after_create, ctx->alloc_next,
           ctx->alloc_next - alloc_after_create);

    /* ---- read every record back BY KEY over IO$_READVBLK ---- */
    /* A corrupted index descent (wrong child pointer) sends the keyed read to
     * the wrong data bucket, so the byte-exact read-back below cannot resolve
     * the record it just wrote across the split. */
    /* negctl: p3-index-child-pointer-offbyone */
    check(read_all_by_key(ctx),
          "every record reads back BY KEY over the ACP, byte-exact, across splits");

    /* ---- DURABILITY: DEACCESS, re-ACCESS (window rebuilt from FH2), re-read
     * with a freshly BOUND ctx -- proves the index+records hit the platter ---- */
    rms_p3_free(ctx); ctx = NULL;
    st = vms_kif_acp_deaccess(chan);
    check($VMS_STATUS_SUCCESS(st), "IO$_DEACCESS after write");
    {
        struct vms_acp_access_args a;
        memset(&a, 0, sizeof(a));
        a.chan    = chan;
        a.did_num = OVMXDIR_FID_NUM;
        a.did_seq = 1;
        strncpy(a.name, TESTNAME, VMS_ACP_NAME_SIZE - 1);
        st = vms_kif_acp_access(&a);
        check($VMS_STATUS_SUCCESS(st), "IO$_ACCESS re-open [OVMXDIR]P3IDX.TST");
        if ($VMS_STATUS_SUCCESS(st)) {
            p3_ctx_t *rc = NULL;
            uint32_t bs;
            memset(&h, 0, sizeof(h));
            h.chan = chan; h.assigned = 1; h.accessed = 1; h.writable = 0;
            h.fd = -1;
            h.eof = (uint64_t)(a.attr.efblk ? (a.attr.efblk - 1u) : 0) * 512u
                    + a.attr.ffbyte;
            h.hiblk = a.attr.hiblk;
            bs = rms_p3_bind(&h, &rc);
            check($VMS_STATUS_SUCCESS(bs) && rc && rc->version == 3,
                  "re-bind Prolog-3 prologue from the on-disk image");
            if (rc) {
                check(read_all_by_key(rc),
                      "records survive DEACCESS+re-ACCESS (durable on the volume)");
                rms_p3_free(rc);
            }
        }
    }

    /* ---- cleanup: DEACCESS + IO$_DELETE, restore the directory ---- */
    (void)vms_kif_acp_deaccess(chan);
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_DELETE; f.modifiers = VMS_ACP_M_DELETE;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1; f.version = 0;
    strncpy(f.name, TESTNAME, VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_fileop(&f);
    check($VMS_STATUS_SUCCESS(st), "IO$_DELETE removes the test file (isolation)");

    /* ---- secondary key / SIDR end-to-end over the ACP (vms-2ae) ---- */
    run_secondary_scenario(chan);

    vms_kif_dassgn(chan);
    vms_kif_acp_dmount(ODS2_UNIT);   /* leave the per-boot mount table clean (vms-757) */

    printf("=== %s: %d pass, %d fail ===\n",
           fail ? "FAILED" : "PASSED", pass, fail);
    return fail ? 1 : 0;
}
