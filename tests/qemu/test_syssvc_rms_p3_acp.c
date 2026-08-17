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
    /* negctl: rms-put-wrong-vbn -- this suite rides rms_io.c's write substrate
     * (rms_io_write -> IO$_WRITEVBLK), so a $PUT that lands one VBN too high
     * leaves the keyed read-back unable to find the record. */
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

    vms_kif_dassgn(chan);

    printf("=== %s: %d pass, %d fail ===\n",
           fail ? "FAILED" : "PASSED", pass, fail);
    return fail ? 1 : 0;
}
