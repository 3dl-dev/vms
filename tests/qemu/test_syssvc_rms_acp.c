/*
 * test_syssvc_rms_acp.c - RMS reaches files through the Files-11 (ODS-2) ACP:
 * $CREATE/$PUT/$GET/$CLOSE/$OPEN/$EXTEND/$ERASE ride channel + $QIO
 * (IO$_ACCESS / IO$_CREATE / IO$_READVBLK / IO$_WRITEVBLK / IO$_MODIFY /
 * IO$_DELETE) on a real /dev/vms, NOT a POSIX fd -- the RMS-over-$QIO rung of
 * epic vms-208 (vms-bc7).
 *
 * WHAT THIS PROVES, through the public RMS system services (sys$create /
 * sys$put / sys$get / sys$close / sys$open / sys$extend / sys$erase,
 * src/vmsrms/rms_core.c + rms_seq.c + rms_io.c) against the real-VAX ODS-2
 * fixture the harness mounts WRITABLE on VDA0::
 *
 *   1. SEQUENTIAL $CREATE + $PUT lands records ON DISK via IO$_WRITEVBLK. A
 *      sys$create of [OVMXDIR]<name> assigns a real FID (IO$_CREATE from
 *      INDEXF.SYS), and each sys$put writes its record through the file's
 *      VBN->LBN window -- no _linux_fd, no POSIX write.
 *   2. $CLOSE + re-$OPEN + $GET reads them back BYTE/RECORD-EXACT via
 *      IO$_READVBLK: the bytes IO$_WRITEVBLK put on the platter are exactly
 *      what a fresh IO$_ACCESS + read returns (INV-6: it hit the disk).
 *   3. Proven for RFM=VAR, RFM=STMLF and RFM=FIX -- the record FRAMING logic
 *      (2-byte count prefix / LF delimiter / fixed size) is intact on top of
 *      the swapped block-I/O substrate.
 *   4. $EXTEND grows the file's allocation (IO$_MODIFY) without moving EOF.
 *   5. $ERASE deletes it (IO$_DELETE): a following sys$open is RMS$_FNF.
 *   6. $CREATE records the creator's rfm/rat/mrs in the header FAT and $CLOSE
 *      the longest record (vms-b447): a DEFAULT-FAB re-$OPEN reads them back,
 *      XABFHC/F$FILE_ATTRIBUTES/DIRECTORY/FULL report them; $OPEN/$CREATE/
 *      $SEARCH/$PARSE return nam$w_fid / nam$w_did (vms-6e28), the FID being
 *      the one $SEARCH reports to DIRECTORY/FULL.
 *
 * NAME->FID VIA THE ACP. resolve_filename no longer calls vmsfs_to_linux_path;
 * "VDA0:[OVMXDIR]<name>" is resolved by $ASSIGNing VDA0: and walking the
 * directory to a FID through IO$_ACCESS -- exercised here every open/create.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass (Rule 9): RMS is now an
 * executive-file consumer, so with no ACP there is nothing to assert. This is
 * the ATOMIC-FLIP-GROUP behaviour: the plain userspace ctest (no /dev/vms) sees
 * RMS fail-honest, and only this QEMU harness, with a real mounted SYS$DISK,
 * proves the flip.
 *
 * ISOLATION. Every file is created under a UNIQUE name in [OVMXDIR] and ERASED
 * before exit, so the net directory state is restored (only BITMAP/INDEXF bits
 * cycle) -- the same discipline as test_syssvc_acp_create.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "rms/rms.h"
#include "rms/nam.h"
#include "rms/rab.h"
#include "tcg_deadline.h"

#define EXIT_SKIP  77

/* $SETDDIR (src/libvms/syssvc/sys_misc.c): starlet.h carries no prototype
 * (a corpus program declares its own, conflicting one). */
extern uint32_t sys$setddir(const struct dsc$descriptor_s *new_dir,
                            unsigned short *old_len,
                            struct dsc$descriptor_s *old_dir);
#define ODS2_UNIT  "VDA0:"

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

/* One full RFM round-trip: create [OVMXDIR]<name>, $PUT `nrec` records, close,
 * reopen, $GET them back and compare byte-exact, $EXTEND, then $ERASE and
 * confirm the file is gone. Records are "REC<nnn>:<name>" so a stale read or a
 * wrong record is caught. Returns nothing; asserts via check(). */
static void rfm_roundtrip(const char *name, uint8_t rfm, uint16_t mrs)
{
    char spec[128];
    char recs[16][64];
    uint16_t reclen[16];
    const int nrec = 8;
    struct FAB fab;
    struct RAB rab;
    uint32_t st;
    char label[64];

    snprintf(spec, sizeof(spec), "%s[OVMXDIR]%s", ODS2_UNIT, name);

    for (int i = 0; i < nrec; i++) {
        snprintf(recs[i], sizeof(recs[i]), "REC%03d:%s", i, name);
        reclen[i] = (uint16_t)strlen(recs[i]);
        if (rfm == FAB$C_FIX) {
            /* FIX records are exactly mrs bytes; sys$put space-pads a short
             * record and sys$get returns the full mrs. Pad the source here so
             * the byte-exact compare below is against the on-disk form. */
            while (reclen[i] < mrs && reclen[i] < sizeof(recs[i]) - 1)
                recs[i][reclen[i]++] = ' ';
            recs[i][reclen[i]] = '\0';
        }
    }

    /* ---- $CREATE ---- */
    fab = cc$rms_fab;
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = rfm;
    fab.fab$b_rat = 0;
    fab.fab$w_mrs = mrs;
    fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;

    st = sys$create(&fab, 0, 0);
    snprintf(label, sizeof(label), "[%s] sys$create -> NORMAL", name);
    check(st == RMS$_NORMAL, label);
    snprintf(label, sizeof(label), "[%s] RMS file handle built (ACP window)", name);
    check(fab._rms_file != 0, label);
    if (st != RMS$_NORMAL)
        return;

    /* ---- $CONNECT + $PUT loop ---- */
    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    st = sys$connect(&rab, 0, 0);
    snprintf(label, sizeof(label), "[%s] sys$connect -> NORMAL", name);
    check(st == RMS$_NORMAL, label);

    int put_ok = 1;
    for (int i = 0; i < nrec; i++) {
        rab.rab$l_rbf = recs[i];
        rab.rab$w_rsz = reclen[i];
        st = sys$put(&rab, 0, 0);
        if (st != RMS$_NORMAL) { put_ok = 0; break; }
    }
    snprintf(label, sizeof(label), "[%s] sys$put all records -> WRITEVBLK", name);
    check(put_ok, label);

    st = sys$close(&fab, 0, 0);
    snprintf(label, sizeof(label), "[%s] sys$close after create -> NORMAL", name);
    check(st == RMS$_NORMAL, label);

    /* ---- re-$OPEN + $GET loop, byte-exact ---- */
    fab = cc$rms_fab;
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = rfm;                 /* ($OPEN replaces these with the header's, vms-b447) */
    fab.fab$w_mrs = mrs;
    fab.fab$b_fac = FAB$M_GET;
    st = sys$open(&fab, 0, 0);
    snprintf(label, sizeof(label), "[%s] sys$open (reopen) -> NORMAL", name);
    check(st == RMS$_NORMAL, label);
    if (st != RMS$_NORMAL) { sys$erase(&fab, 0, 0); return; }

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    char ubuf[64];
    rab.rab$l_ubf = ubuf;
    rab.rab$w_usz = sizeof(ubuf);
    st = sys$connect(&rab, 0, 0);
    check(st == RMS$_NORMAL, "  reopen sys$connect -> NORMAL");

    int readback_ok = 1, got = 0;
    for (int i = 0; i < nrec; i++) {
        st = sys$get(&rab, 0, 0);
        if (st != RMS$_NORMAL) { readback_ok = 0; break; }
        got++;
        if (rab.rab$w_rsz != reclen[i] ||
            memcmp(rab.rab$l_ubf, recs[i], reclen[i]) != 0) {
            readback_ok = 0;
            break;
        }
    }
    snprintf(label, sizeof(label),
             "[%s] $GET reads back %d records BYTE-EXACT (READVBLK)", name, nrec);
    check(readback_ok && got == nrec, label);

    /* Next $GET is EOF. */
    st = sys$get(&rab, 0, 0);
    snprintf(label, sizeof(label), "[%s] sys$get at EOF -> RMS$_EOF", name);
    check(st == RMS$_EOF, label);

    sys$close(&fab, 0, 0);

    /* ---- $EXTEND (IO$_MODIFY): reopen for write, allocate, close ---- */
    fab = cc$rms_fab;
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = rfm;
    fab.fab$w_mrs = mrs;
    fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;
    st = sys$open(&fab, 0, 0);
    if (st == RMS$_NORMAL) {
        fab.fab$l_alq = 4;               /* allocate 4 more blocks */
        st = sys$extend(&fab, 0, 0);
        snprintf(label, sizeof(label), "[%s] sys$extend (IO$_MODIFY) -> NORMAL", name);
        check(st == RMS$_NORMAL, label);
        sys$close(&fab, 0, 0);
    } else {
        snprintf(label, sizeof(label), "[%s] reopen-for-extend -> NORMAL", name);
        check(0, label);
    }

    /* ---- $ERASE (IO$_DELETE), then confirm gone ---- */
    fab = cc$rms_fab;
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    st = sys$erase(&fab, 0, 0);
    snprintf(label, sizeof(label), "[%s] sys$erase (IO$_DELETE) -> NORMAL", name);
    check(st == RMS$_NORMAL, label);

    fab = cc$rms_fab;
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = rfm;
    fab.fab$b_fac = FAB$M_GET;
    st = sys$open(&fab, 0, 0);
    snprintf(label, sizeof(label), "[%s] sys$open after erase -> RMS$_FNF", name);
    check(st == RMS$_FNF, label);
    if (st == RMS$_NORMAL) sys$close(&fab, 0, 0);
}


/* ---- vms-b447 + vms-6e28 helpers ----------------------------------------- */

/* One file of the record-attribute round trip: what $CREATE asked for, the
 * records written, and the IDs RMS handed back. */
struct fat_case {
    const char *name;        /* [OVMXDIR]<name> */
    uint8_t     rfm, rat;
    uint16_t    mrs;
    const char *recs[3];
    uint16_t    lrl;         /* longest record the header must report */
    uint16_t    fid[3];      /* nam$w_fid from $CREATE */
    uint16_t    did[3];      /* nam$w_did from $CREATE */
    int         created;
    int         hdr_ok;      /* default-FAB $OPEN took rfm/rat/mrs from the header */
    int         get_ok;      /* ...and $GET returned every record byte-exact */
};

static int id_eq(const uint16_t a[3], const uint16_t b[3])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

/* The record as it is on disk / as $GET returns it: a FIX record is
 * space-padded to mrs. */
static uint16_t fat_rec(const struct fat_case *c, int i, char *out, size_t outsz)
{
    size_t n = strlen(c->recs[i]);
    memcpy(out, c->recs[i], n);
    if (c->rfm == FAB$C_FIX) {
        while (n < c->mrs && n < outsz) out[n++] = ' ';
    }
    return (uint16_t)n;
}

/* $CREATE with a NAM, $PUT the records, $CLOSE. */
static void fat_create(struct fat_case *c)
{
    char spec[128], rsa[NAM$C_MAXRSS + 1], esa[NAM$C_MAXESS + 1], label[160];
    struct NAM nam = cc$rms_nam;
    struct FAB fab = cc$rms_fab;
    struct RAB rab;
    uint32_t st;

    snprintf(spec, sizeof spec, "%s[OVMXDIR]%s", ODS2_UNIT, c->name);
    nam.nam$l_rsa = rsa; nam.nam$b_rss = NAM$C_MAXRSS;
    nam.nam$l_esa = esa; nam.nam$b_ess = NAM$C_MAXESS;
    fab.fab$l_fna = spec; fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = c->rfm; fab.fab$b_rat = c->rat; fab.fab$w_mrs = c->mrs;
    fab.fab$b_fac = FAB$M_PUT; fab.fab$l_nam = &nam;
    st = sys$create(&fab, 0, 0);
    snprintf(label, sizeof label, "vms-b447: [%s] $CREATE (rfm %u, rat %u, mrs %u) with a NAM",
             c->name, c->rfm, c->rat, c->mrs);
    check(st == RMS$_NORMAL, label);
    if (st != RMS$_NORMAL) return;
    c->created = 1;
    memcpy(c->fid, nam.nam$w_fid, sizeof c->fid);
    memcpy(c->did, nam.nam$w_did, sizeof c->did);
    printf("  [%s] $CREATE nam$w_fid (%u,%u,%u) nam$w_did (%u,%u,%u)\n", c->name,
           c->fid[0], c->fid[1], c->fid[2], c->did[0], c->did[1], c->did[2]);
    snprintf(label, sizeof label, "vms-6e28: [%s] $CREATE returns a nonzero nam$w_fid", c->name);
    check(c->fid[0] != 0 && c->fid[1] != 0, label);
    snprintf(label, sizeof label, "vms-6e28: [%s] $CREATE returns the directory's nam$w_did", c->name);
    check(c->did[0] != 0 && c->did[1] != 0 && !id_eq(c->did, c->fid), label);

    rab = cc$rms_rab; rab.rab$l_fab = &fab;
    (void)sys$connect(&rab, 0, 0);
    int ok = 1;
    for (int i = 0; i < 3 && c->recs[i]; i++) {
        /* A FIXED record is exactly mrs bytes: RMS refuses any other size with
         * RMS$_RSZ, as OpenVMS does (RMS.PUT.FIX.SHORT, docs/oracle/semantics/
         * rms/, vms-3b5), so the writer pads it -- the bytes fat_rec() expects
         * back. */
        char rec[64];
        uint16_t rl = fat_rec(c, i, rec, sizeof rec);
        rab.rab$l_rbf = rec;
        rab.rab$w_rsz = rl;
        if (sys$put(&rab, 0, 0) != RMS$_NORMAL) ok = 0;
    }
    snprintf(label, sizeof label, "vms-b447: [%s] $PUT the records", c->name);
    check(ok, label);
    st = sys$close(&fab, 0, 0);
    snprintf(label, sizeof label, "vms-b447: [%s] $CLOSE -> NORMAL", c->name);
    check(st == RMS$_NORMAL, label);
}

/* Re-$OPEN with a DEFAULT FAB: the header's rfm/rat/mrs come back, the NAM's
 * IDs match $CREATE's, $DISPLAY's XABFHC carries them, $GET is byte-exact. */
static void fat_reopen(struct fat_case *c)
{
    char spec[128], rsa[NAM$C_MAXRSS + 1], label[200];
    struct NAM nam = cc$rms_nam;
    struct FAB fab = cc$rms_fab;          /* STMLF, CR, mrs 0 */
    struct XABFHC fhc = cc$rms_xabfhc;
    struct RAB rab;
    uint32_t st;

    if (!c->created) return;
    snprintf(spec, sizeof spec, "%s[OVMXDIR]%s", ODS2_UNIT, c->name);
    nam.nam$l_rsa = rsa; nam.nam$b_rss = NAM$C_MAXRSS;
    fab.fab$l_fna = spec; fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$l_nam = &nam;
    fab.fab$l_xab = (struct XABKEY *)&fhc;
    st = sys$open(&fab, 0, 0);
    snprintf(label, sizeof label, "vms-b447: [%s] $OPEN with a default FAB", c->name);
    check(st == RMS$_NORMAL, label);
    if (st != RMS$_NORMAL) return;
    printf("  [%s] $OPEN -> rfm %u rat %u mrs %u, nam$w_fid (%u,%u,%u)\n", c->name,
           fab.fab$b_rfm, fab.fab$b_rat, fab.fab$w_mrs,
           nam.nam$w_fid[0], nam.nam$w_fid[1], nam.nam$w_fid[2]);
    snprintf(label, sizeof label, "vms-b447: [%s] $OPEN reads fab$b_rfm %u from the header", c->name, c->rfm);
    check(fab.fab$b_rfm == c->rfm, label);
    snprintf(label, sizeof label, "vms-b447: [%s] $OPEN reads fab$b_rat %u from the header", c->name, c->rat);
    check(fab.fab$b_rat == c->rat, label);
    snprintf(label, sizeof label, "vms-b447: [%s] $OPEN reads fab$w_mrs %u from the header (FAT$W_MAXREC)", c->name, c->mrs);
    check(fab.fab$w_mrs == c->mrs, label);
    c->hdr_ok = fab.fab$b_rfm == c->rfm && fab.fab$b_rat == c->rat &&
                fab.fab$w_mrs == c->mrs;
    snprintf(label, sizeof label, "vms-6e28: [%s] $OPEN's nam$w_fid is the FID $CREATE returned", c->name);
    check(id_eq(nam.nam$w_fid, c->fid), label);
    snprintf(label, sizeof label, "vms-6e28: [%s] $OPEN's nam$w_did is the DID $CREATE returned", c->name);
    check(id_eq(nam.nam$w_did, c->did), label);

    check(sys$display(&fab, 0, 0) == RMS$_NORMAL, "  $DISPLAY -> NORMAL");
    snprintf(label, sizeof label, "vms-b447: [%s] XABFHC xab$w_mrz %u, xab$w_lrl %u (got %u, %u)",
             c->name, c->mrs, c->lrl, fhc.xab$w_mrz, fhc.xab$w_lrl);
    check(fhc.xab$w_mrz == c->mrs && fhc.xab$w_lrl == c->lrl &&
          (fhc.xab$b_rfm & 0x0F) == c->rfm && fhc.xab$b_atr == c->rat, label);

    rab = cc$rms_rab; rab.rab$l_fab = &fab;
    char ubuf[128], want[128];
    rab.rab$l_ubf = ubuf; rab.rab$w_usz = sizeof ubuf;
    (void)sys$connect(&rab, 0, 0);
    int ok = 1, n = 0;
    while (sys$get(&rab, 0, 0) == RMS$_NORMAL) {
        if (n >= 3 || !c->recs[n]) { ok = 0; break; }
        uint16_t wl = fat_rec(c, n, want, sizeof want);
        if (rab.rab$w_rsz != wl || memcmp(ubuf, want, wl) != 0) ok = 0;
        n++;
    }
    int nrec = 0;
    while (nrec < 3 && c->recs[nrec]) nrec++;
    snprintf(label, sizeof label,
             "vms-b447: [%s] $GET after the default-FAB $OPEN returns the %d records byte-exact",
             c->name, nrec);
    check(ok && n == nrec, label);
    c->get_ok = ok && n == nrec;
    (void)sys$close(&fab, 0, 0);

    /* A caller-supplied size does not stand: the header's is the file's. */
    if (c->rfm == FAB$C_FIX) {
        fab = cc$rms_fab;
        fab.fab$l_fna = spec; fab.fab$b_fns = (uint8_t)strlen(spec);
        fab.fab$b_rfm = FAB$C_FIX; fab.fab$w_mrs = 99;
        st = sys$open(&fab, 0, 0);
        snprintf(label, sizeof label,
                 "vms-b447: [%s] $OPEN with fab$w_mrs 99 returns the header's %u", c->name, c->mrs);
        check(st == RMS$_NORMAL && fab.fab$w_mrs == c->mrs, label);
        if (st == RMS$_NORMAL) (void)sys$close(&fab, 0, 0);
    }
}

/* $PARSE + $SEARCH: $SEARCH's nam$w_fid is the FID it reports through
 * rms_search_fid (DIRECTORY/FULL "File ID:") and the FID $CREATE returned;
 * $PARSE returns the directory ID and no file ID. And the header attributes
 * DIRECTORY/FULL + F$FILE_ATTRIBUTES read (rms_file_attr). */
static void fat_search_attr(const struct fat_case *c)
{
    char spec[128], esa[NAM$C_MAXESS + 1], rsa[NAM$C_MAXRSS + 1], label[200];
    struct NAM nam = cc$rms_nam;
    struct FAB fab = cc$rms_fab;
    uint32_t st;

    if (!c->created) return;
    snprintf(spec, sizeof spec, "%s[OVMXDIR]%s", ODS2_UNIT, c->name);
    nam.nam$l_esa = esa; nam.nam$b_ess = NAM$C_MAXESS;
    nam.nam$l_rsa = rsa; nam.nam$b_rss = NAM$C_MAXRSS;
    nam.nam$w_fid[0] = 0xBEEF;            /* $PARSE must clear it */
    fab.fab$l_fna = spec; fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$l_nam = &nam;
    st = sys$parse(&fab, 0, 0);
    snprintf(label, sizeof label, "vms-6e28: [%s] $PARSE returns nam$w_did = the directory's ID and clears nam$w_fid", c->name);
    check(st == RMS$_NORMAL && id_eq(nam.nam$w_did, c->did) &&
          nam.nam$w_fid[0] == 0 && nam.nam$w_fid[1] == 0 && nam.nam$w_fid[2] == 0, label);
    st = sys$search(&fab, 0, 0);
    uint16_t n = 0, sq = 0; uint8_t rvn = 0, nmx = 0;
    int have = (st == RMS$_NORMAL) && rms_search_fid(&nam, &n, &sq, &rvn, &nmx);
    printf("  [%s] $SEARCH nam$w_fid (%u,%u,%u), rms_search_fid (%u,%u,%u|%u)\n", c->name,
           nam.nam$w_fid[0], nam.nam$w_fid[1], nam.nam$w_fid[2], n, sq, rvn, nmx);
    snprintf(label, sizeof label,
             "vms-6e28: [%s] $SEARCH's nam$w_fid is nonzero and equals the FID $SEARCH reports (rms_search_fid)", c->name);
    check(have && nam.nam$w_fid[0] != 0 && nam.nam$w_fid[0] == n && nam.nam$w_fid[1] == sq &&
          nam.nam$w_fid[2] == (uint16_t)(rvn | (nmx << 8)), label);
    snprintf(label, sizeof label, "vms-6e28: [%s] $SEARCH's nam$w_fid / nam$w_did are $CREATE's", c->name);
    check(id_eq(nam.nam$w_fid, c->fid) && id_eq(nam.nam$w_did, c->did), label);
    rms_search_end(&nam);

    struct rms_fileattr fa;
    st = rms_file_attr(spec, &fa);
    snprintf(label, sizeof label,
             "vms-b447: [%s] header (F$FILE_ATTRIBUTES source): RFM %u RAT %u MRS %u LRL %u ORG SEQ (got %u %u %u %u %u)",
             c->name, c->rfm, c->rat, c->mrs, c->lrl, fa.rfm, fa.rat, fa.mrs, fa.lrl, fa.org);
    check(st == RMS$_NORMAL && fa.rfm == c->rfm && fa.rat == c->rat &&
          fa.mrs == c->mrs && fa.lrl == c->lrl && fa.org == FAB$C_SEQ, label);
}

/* Feed `script` to /bin/DCL.EXE; capture its output. 0 = DCL.EXE ran to EOF. */
static int run_dcl(const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];

    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl("/bin/DCL.EXE", "DCL.EXE", (char *)NULL);
        printf("SETUP_FAIL exec\n");
        fflush(stdout);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    (void)!write(in_pipe[1], script, strlen(script));
    close(in_pipe[1]);
    size_t used = 0;
    int dr = ovmx_tcg_drain_pipe(out_pipe[0], out, outsz, ovmx_tcg_ms(30000), pid, &used);
    close(out_pipe[0]);
    int ws;
    while (waitpid(pid, &ws, 0) < 0 && errno == EINTR)
        ;
    if (dr == OVMX_TCG_DRAIN_TIMEOUT) {
        printf("  ---- partial DCL.EXE output (%zu bytes) ----\n%s\n  ---- end ----\n", used, out);
        return -1;
    }
    return 0;
}

/* `line` appears in `out` as a whole line (DCL.EXE may end it CR LF). */
static int has_line(const char *out, const char *line)
{
    size_t n = strlen(line);
    for (const char *p = strstr(out, line); p; p = strstr(p + 1, line)) {
        if ((p == out || p[-1] == '\n') && (p[n] == '\n' || p[n] == '\r'))
            return 1;
    }
    return 0;
}

static void fat_erase(const struct fat_case *c)
{
    char spec[128];
    struct FAB fab = cc$rms_fab;
    if (!c->created) return;
    snprintf(spec, sizeof spec, "%s[OVMXDIR]%s", ODS2_UNIT, c->name);
    fab.fab$l_fna = spec; fab.fab$b_fns = (uint8_t)strlen(spec);
    (void)sys$erase(&fab, 0, 0);
}

int main(void)
{
    uint32_t st;

    printf("=== test_syssvc_rms_acp (RMS $CREATE/$PUT/$GET/$CLOSE/$EXTEND/$ERASE "
           "-> $QIO to the Files-11 ODS-2 ACP, vms-bc7) ===\n");

    if (!executive_present()) {
        printf("  SKIP: no /dev/vms -- RMS is an executive-file consumer; nothing "
               "to assert without a real ACP (Rule 9).\n");
        return EXIT_SKIP;
    }

    /* Mount the ODS-2 volume on VDA0: executive-global so $ASSIGN sees it. */
    st = vms_kif_acp_mount(ODS2_UNIT);   /* idempotent */
    check($VMS_STATUS_SUCCESS(st), "VDA0: mounted executive-global for RMS");

    /* --- vms-03b: the RMS executive-presence probe + its classification ------
     * The INV-6 hole this item names: acp_assign conflating "executive absent"
     * (/dev/vms unopenable) with "unit present but unmounted" would let RMS's
     * probe read an unmounted unit as "no executive" and silently defer a file
     * read to the /vms POSIX passthrough. On a LIVE executive the probe MUST read
     * PRESENT (0), so every RMS $OPEN/$CREATE below stays ACP-only. */
    check(rms_executive_absent() == 0,
          "vms-03b: executive live -> rms_executive_absent()==0 (RMS stays ACP-only, no /vms passthrough)");
    /* The load-bearing $ASSIGN-status classification, exercised DIRECTLY: the
     * probe itself only ever $ASSIGNs the always-mounted system disk, so its
     * SS$_DEVNOTMOUNT arm is unreachable end-to-end and could silently regress.
     * ONLY SS$_NOSUCHDEV (the userspace KIF's "/dev/vms unreachable") is absent;
     * SS$_DEVNOTMOUNT (a present-but-unmounted unit under a live executive) and
     * every success are PRESENT. A regression that classified SS$_DEVNOTMOUNT as
     * absent would reopen the passthrough masquerade -- and reddens right here. */
    check(rms_status_is_executive_absent(SS$_NOSUCHDEV) == 1,
          "vms-03b: SS$_NOSUCHDEV classifies as executive-ABSENT (/dev/vms unreachable)");
    check(rms_status_is_executive_absent(SS$_DEVNOTMOUNT) == 0,
          "vms-03b: SS$_DEVNOTMOUNT (unmounted unit, live executive) classifies as PRESENT -- no /vms passthrough (INV-6)");
    check(rms_status_is_executive_absent(SS$_NORMAL) == 0,
          "vms-03b: a successful $ASSIGN classifies as PRESENT");

    rfm_roundtrip("RMSVAR.DAT",  FAB$C_VAR,   0);
    rfm_roundtrip("RMSSTM.DAT",  FAB$C_STMLF, 0);
    rfm_roundtrip("RMSFIX.DAT",  FAB$C_FIX,   20);

    /* ---- vms-254: block I/O ($WRITE/$READ, FAC BIO) over the ACP window.
     * 1300 bytes written at VBN 1 span three blocks; after $CLOSE + re-$OPEN
     * a $READ from VBN 1 returns exactly those 1300 bytes (the end of file is
     * byte-exact on the ODS-2 header, EBK/FFB), and the next-block $READ is
     * RMS$_EOF. This is the byte-stream substrate the C RTL's read()/write()
     * on a stream file rides. ---- */
    {
        char spec[128];
        snprintf(spec, sizeof(spec), "%s[OVMXDIR]BLKIO.DAT", ODS2_UNIT);
        static char wbuf[1300], rbuf[2048];
        for (int i = 0; i < (int)sizeof wbuf; i++)
            wbuf[i] = (char)(' ' + (i * 7) % 95);

        struct FAB fab = cc$rms_fab;
        fab.fab$l_fna = spec;
        fab.fab$b_fns = (uint8_t)strlen(spec);
        fab.fab$b_org = FAB$C_SEQ;
        fab.fab$b_rfm = FAB$C_UDF;
        fab.fab$b_fac = FAB$M_PUT | FAB$M_GET | FAB$M_BIO;
        uint32_t bst = sys$create(&fab, 0, 0);
        check(bst == RMS$_NORMAL, "vms-254: sys$create BLKIO.DAT (FAC BIO) over the ACP");
        if (bst == RMS$_NORMAL) {
            struct RAB rab = cc$rms_rab;
            rab.rab$l_fab = &fab;
            sys$connect(&rab, 0, 0);
            rab.rab$l_bkt = 1;
            rab.rab$l_rbf = wbuf;
            rab.rab$w_rsz = sizeof wbuf;
            check(sys$write(&rab, 0, 0) == RMS$_NORMAL,
                  "vms-254: sys$write 1300 bytes at VBN 1 -> IO$_WRITEVBLK");
            sys$close(&fab, 0, 0);

            fab.fab$b_fac = FAB$M_GET | FAB$M_BIO;
            bst = sys$open(&fab, 0, 0);
            check(bst == RMS$_NORMAL, "vms-254: re-$OPEN BLKIO.DAT for block reads");
            if (bst == RMS$_NORMAL) {
                rab = cc$rms_rab;
                rab.rab$l_fab = &fab;
                sys$connect(&rab, 0, 0);
                rab.rab$l_bkt = 1;
                rab.rab$l_ubf = rbuf;
                rab.rab$w_usz = sizeof rbuf;
                bst = sys$read(&rab, 0, 0);
                check(bst == RMS$_NORMAL && rab.rab$w_rsz == sizeof wbuf &&
                      memcmp(rbuf, wbuf, sizeof wbuf) == 0,
                      "vms-254: sys$read from VBN 1 returns the 1300 bytes byte-exact (EOF from the header)");
                rab.rab$l_bkt = 0;
                check(sys$read(&rab, 0, 0) == RMS$_EOF,
                      "vms-254: next-block sys$read past the end of file -> RMS$_EOF");
                sys$close(&fab, 0, 0);
            }
            fab = cc$rms_fab;
            fab.fab$l_fna = spec;
            fab.fab$b_fns = (uint8_t)strlen(spec);
            check(sys$erase(&fab, 0, 0) == RMS$_NORMAL, "vms-254: sys$erase BLKIO.DAT");
        }
    }

    /* If rms_io_write() ($PUT) writes a record one VBN too high while
     * rms_io_read() ($GET) still reads the true VBN, every byte-exact readback
     * above misses on re-$OPEN -- so this whole-suite gate reddens exactly when
     * the block-I/O substrate stops being byte-exact through the ACP window. */
    /* negctl: rms-put-wrong-vbn */
    check(fail == 0,
          "RMS-over-ACP: all records round-tripped byte-exact through the ACP window");

    /* ---- vms-872: the process DEFAULT DIRECTORY is the executive's, and RMS
     * completes a relative file specification in it -- in this process and
     * in an image activated from it (REGISTER_CONTINUE inherits it). Before,
     * an image given "X.DAT" created VDA0:[000000]X.DAT whatever the user had
     * SET DEFAULT to. ---- */
    {
        const char *dd = ODS2_UNIT "[OVMXDIR]";
        struct dsc$descriptor_s d = { (unsigned short)strlen(dd), DSC$K_DTYPE_T,
                                      DSC$K_CLASS_S, (char *)dd };
        check(sys$setddir(&d, NULL, NULL) == SS$_NORMAL,
              "vms-872: $SETDDIR " ODS2_UNIT "[OVMXDIR]");
        char got[256] = "";
        check((vms_kif_ddir(NULL, got, sizeof got) & 1) && strcmp(got, dd) == 0,
              "vms-872: the executive holds the default directory $SETDDIR set");

        struct FAB fab = cc$rms_fab;
        fab.fab$l_fna = (char *)"DDIRREL.DAT"; fab.fab$b_fns = 11;
        fab.fab$b_fac = FAB$M_PUT;
        st = sys$create(&fab, 0, 0);
        if (st == RMS$_NORMAL) (void)sys$close(&fab, 0, 0);
        struct rms_fileattr fa;
        check(st == RMS$_NORMAL &&
              (rms_file_attr(ODS2_UNIT "[OVMXDIR]DDIRREL.DAT", &fa) & 1),
              "vms-872: $CREATE \"DDIRREL.DAT\" lands in the default directory [OVMXDIR]");
        check(!(rms_file_attr(ODS2_UNIT "[000000]DDIRREL.DAT", &fa) & 1),
              "vms-872: ...not in the volume root [000000]");

        /* An activated image (REGISTER_CONTINUE child) sees the same default
         * and its relative $OPEN finds the file. */
        int pp[2];
        uint32_t rep[2] = { 0, 0 };
        if (pipe(pp) == 0) {
            pid_t k = fork();
            if (k == 0) {
                close(pp[0]);
                uint32_t r[2] = { 0, 0 };
                vms_kif_close();
                if (vms_kif_open() >= 0 && (vms_kif_register_continue() & 1)) {
                    char cd[256] = "";
                    r[0] = ((vms_kif_ddir(NULL, cd, sizeof cd) & 1) && strcmp(cd, dd) == 0);
                    struct FAB f2 = cc$rms_fab;
                    f2.fab$l_fna = (char *)"DDIRREL.DAT"; f2.fab$b_fns = 11;
                    uint32_t os = sys$open(&f2, 0, 0);
                    r[1] = (os == RMS$_NORMAL);
                    if (os == RMS$_NORMAL) (void)sys$close(&f2, 0, 0);
                }
                (void)!write(pp[1], r, sizeof r);
                _exit(0);
            }
            close(pp[1]);
            (void)!read(pp[0], rep, sizeof rep);
            close(pp[0]);
            int ws; waitpid(k, &ws, 0);
        }
        check(rep[0] == 1, "vms-872: an activated image (REGISTER_CONTINUE) inherits the default directory");
        check(rep[1] == 1, "vms-872: ...and its relative $OPEN \"DDIRREL.DAT\" finds the file there");

        /* vms-0ae: relative directory syntax merges with the default directory
         * [OVMXDIR]: "[-.OVMXDIR]" is up to the MFD and back down; "[]" is the
         * default itself. Both $OPEN the file the default-relative create made. */
        {
            static const char *rel[] = { "[-.OVMXDIR]DDIRREL.DAT", "[]DDIRREL.DAT" };
            for (int i = 0; i < 2; i++) {
                struct FAB f3 = cc$rms_fab;
                f3.fab$l_fna = (char *)rel[i];
                f3.fab$b_fns = (uint8_t)strlen(rel[i]);
                uint32_t os = sys$open(&f3, 0, 0);
                if (os == RMS$_NORMAL) (void)sys$close(&f3, 0, 0);
                char lbl[128];
                snprintf(lbl, sizeof lbl,
                         "vms-0ae: $OPEN \"%s\" resolves against the default directory", rel[i]);
                check(os == RMS$_NORMAL, lbl);
            }
        }

        struct FAB fe = cc$rms_fab;
        fe.fab$l_fna = (char *)(ODS2_UNIT "[OVMXDIR]DDIRREL.DAT");
        fe.fab$b_fns = (uint8_t)strlen(fe.fab$l_fna);
        (void)sys$erase(&fe, 0, 0);
    }


    /* ---- vms-158 + vms-98e: $CREATE and $OPEN fill an attached NAM with the
     * RESULTANT spec, and $OPEN loads the file's own record format into the
     * FAB -- so a VAR file opened with a default (cc$rms_fab = STMLF) FAB reads
     * back record for record, not as one blob of length-prefixed bytes. ---- */
    {
        const char *spec = ODS2_UNIT "[OVMXDIR]NAMRFM.DAT";
        static const char *rr[] = { "first VAR record", "second, longer VAR record" };
        char rsa[NAM$C_MAXRSS + 1], esa[NAM$C_MAXESS + 1];
        struct NAM nam = cc$rms_nam;
        struct FAB fab = cc$rms_fab;
        struct RAB rab;
        nam.nam$l_rsa = rsa; nam.nam$b_rss = NAM$C_MAXRSS;
        nam.nam$l_esa = esa; nam.nam$b_ess = NAM$C_MAXESS;
        fab.fab$l_fna = (char *)spec; fab.fab$b_fns = (uint8_t)strlen(spec);
        fab.fab$b_rfm = FAB$C_VAR; fab.fab$b_rat = FAB$M_CR;
        fab.fab$b_fac = FAB$M_PUT; fab.fab$l_nam = &nam;
        st = sys$create(&fab, 0, 0);
        check(st == RMS$_NORMAL, "vms-98e: $CREATE VAR file with a NAM attached");
        rsa[nam.nam$b_rsl] = '\0'; esa[nam.nam$b_esl] = '\0';
        check(nam.nam$b_rsl > 0 && strstr(rsa, "[OVMXDIR]NAMRFM.DAT;") != NULL &&
              rsa[nam.nam$b_rsl - 1] >= '1' && rsa[nam.nam$b_rsl - 1] <= '9',
              "vms-98e: $CREATE returns the RESULTANT spec with the real version in nam$l_rsa");
        check(nam.nam$b_esl > 0 && strstr(esa, "[OVMXDIR]NAMRFM.DAT;") != NULL,
              "vms-98e: $CREATE returns the EXPANDED spec in nam$l_esa");
        printf("  (resultant %s, expanded %s)\n", rsa, esa);
        char created[NAM$C_MAXRSS + 1];
        snprintf(created, sizeof created, "%s", rsa);
        if (st == RMS$_NORMAL) {
            rab = cc$rms_rab; rab.rab$l_fab = &fab;
            (void)sys$connect(&rab, 0, 0);
            for (int i = 0; i < 2; i++) {
                rab.rab$l_rbf = (char *)rr[i]; rab.rab$w_rsz = (uint16_t)strlen(rr[i]);
                (void)sys$put(&rab, 0, 0);
            }
            (void)sys$close(&fab, 0, 0);
        }

        /* Re-open with a DEFAULT FAB (STMLF) and a fresh NAM. */
        struct NAM nam2 = cc$rms_nam;
        char rsa2[NAM$C_MAXRSS + 1];
        nam2.nam$l_rsa = rsa2; nam2.nam$b_rss = NAM$C_MAXRSS;
        struct FAB f2 = cc$rms_fab;
        f2.fab$l_fna = (char *)spec; f2.fab$b_fns = (uint8_t)strlen(spec);
        f2.fab$l_nam = &nam2;
        st = sys$open(&f2, 0, 0);
        check(st == RMS$_NORMAL, "vms-158: $OPEN the VAR file with a default (STMLF) FAB");
        check(f2.fab$b_rfm == FAB$C_VAR,
              "vms-158: $OPEN loads the file's record format (VAR) into fab$b_rfm");
        check((f2.fab$b_rat & FAB$M_CR) != 0,
              "vms-158: $OPEN loads the file's record attributes (CR) into fab$b_rat");
        rsa2[nam2.nam$b_rsl] = '\0';
        check(nam2.nam$b_rsl > 0 && strcmp(rsa2, created) == 0,
              "vms-98e: $OPEN returns the same resultant spec $CREATE did");
        int recs_ok = 1, nr = 0;
        if (st == RMS$_NORMAL) {
            char buf[128];
            rab = cc$rms_rab; rab.rab$l_fab = &f2;
            (void)sys$connect(&rab, 0, 0);
            rab.rab$l_ubf = buf; rab.rab$w_usz = sizeof buf;
            while (sys$get(&rab, 0, 0) == RMS$_NORMAL) {
                if (nr >= 2 || rab.rab$w_rsz != strlen(rr[nr]) ||
                    memcmp(buf, rr[nr], rab.rab$w_rsz) != 0) recs_ok = 0;
                nr++;
            }
            (void)sys$close(&f2, 0, 0);
        }
        check(recs_ok && nr == 2,
              "vms-158: $GET after a default-FAB $OPEN returns each VAR record whole (2 records, byte-exact)");
        struct FAB fe = cc$rms_fab;
        fe.fab$l_fna = (char *)spec; fe.fab$b_fns = (uint8_t)strlen(spec);
        (void)sys$erase(&fe, 0, 0);
    }

    /* ---- vms-dfa: $DISPLAY fills XABFHC (file-header characteristics) and
     * XABALL (allocation) from the REAL ODS-2 FAT over the ACP -- previously an
     * XABFHC/XABALL chained onto the FAB was silently walked over (the
     * rms$xab_silent_unsupported facade). Create a FIX file, close it, then
     * reopen with both XABs chained and read back the real header. Self-
     * contained: the rfm_roundtrip files above are erased, so this stages its
     * own XABFHC.DAT and erases it. ---- */
    {
        struct FAB fab = cc$rms_fab;
        struct RAB rab;
        char spec[128];
        snprintf(spec, sizeof(spec), "%s[OVMXDIR]XABFHC.DAT", ODS2_UNIT);
        fab.fab$l_fna = spec;
        fab.fab$b_fns = (uint8_t)strlen(spec);
        fab.fab$b_org = FAB$C_SEQ;
        fab.fab$b_rfm = FAB$C_FIX;
        fab.fab$w_mrs = 20;
        fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;

        uint32_t st = sys$create(&fab, 0, 0);
        check(st == RMS$_NORMAL, "vms-dfa: sys$create XABFHC.DAT (FIX/20) over the ACP");
        if (st == RMS$_NORMAL) {
            rab = cc$rms_rab;
            rab.rab$l_fab = &fab;
            sys$connect(&rab, 0, 0);
            char rec[20];
            memset(rec, ' ', sizeof(rec));
            memcpy(rec, "XABREC", 6);
            for (int i = 0; i < 4; i++) {
                rab.rab$l_rbf = rec;
                rab.rab$w_rsz = 20;
                sys$put(&rab, 0, 0);
            }
            sys$close(&fab, 0, 0);

            /* Reopen with XABFHC + XABALL chained; $DISPLAY fills them. */
            struct XABFHC fhc = cc$rms_xabfhc;
            struct XABALL all = cc$rms_xaball;
            /* vms-5dd2: XABDAT + XABPRO ride the same chain, seeded with
             * values no real header carries, so an unfilled XAB is caught. */
            struct XABDAT dat = cc$rms_xabdat;
            struct XABPRO pro = cc$rms_xabpro;
            dat.xab$q_cdt = dat.xab$q_rdt = 0x1234567812345678ULL;
            pro.xab$w_pro = 0xABCD;
            pro.xab$l_uic = 0xDEADBEEFu;
            fhc.xab$l_nxt = &all;
            all.xab$l_nxt = &dat;
            dat.xab$l_nxt = &pro;
            fab.fab$l_xab = (struct XABKEY *)&fhc;
            fab.fab$b_fac = FAB$M_GET;

            st = sys$open(&fab, 0, 0);
            check(st == RMS$_NORMAL, "vms-dfa: sys$open XABFHC.DAT over the ACP -> NORMAL");
            if (st == RMS$_NORMAL) {
                check(sys$display(&fab, 0, 0) == RMS$_NORMAL, "vms-dfa: sys$display -> NORMAL");
                /* Teeth: the old silent-skip left the XABFHC at its cc$rms init
                 * (xab$b_rfm == 0); a real FAT read yields FAB$C_FIX (== 1). */
                check(fhc.xab$b_rfm == FAB$C_FIX,
                      "vms-dfa: XABFHC xab$b_rfm == FAB$C_FIX read from the ODS-2 FAT (not silently skipped)");
                check(fhc.xab$l_ebk >= 1,
                      "vms-dfa: XABFHC xab$l_ebk is a real end-of-file VBN off the FAT");
                check(fhc.xab$l_hbk >= fhc.xab$l_ebk,
                      "vms-dfa: XABFHC xab$l_hbk (allocated) >= xab$l_ebk (EOF)");
                check(all.xab$l_alq >= 1 && all.xab$l_alq == fhc.xab$l_hbk,
                      "vms-dfa: XABALL xab$l_alq == the file's realized allocation (hiblk)");
                /* vms-5dd2: the header's dates/protection/owner over the ACP.
                 * The XABs were seeded with values no header holds; $DISPLAY
                 * must replace them with the FH2's (the ACP does not stamp
                 * dates at IO$_CREATE yet -- vms-ab49 -- so they read back as
                 * the header's zero, which is still the header's value). */
                check(dat.xab$q_cdt != 0x1234567812345678ULL &&
                          dat.xab$q_rdt != 0x1234567812345678ULL,
                      "vms-5dd2: XABDAT creation/revision dates come from the ODS-2 header (not left untouched)");
                check(pro.xab$w_pro != 0xABCD && pro.xab$l_uic != 0xDEADBEEFu,
                      "vms-5dd2: XABPRO protection + owner UIC come from the ODS-2 header (not left untouched)");
                sys$close(&fab, 0, 0);
            }
            sys$erase(&fab, 0, 0);
        }
    }


    /* ---- vms-b447 + vms-6e28: $CREATE records the creator's record format,
     * attributes and maximum record size in the file header (FAT$B_RTYPE,
     * FAT$B_RATTRIB, FAT$W_MAXREC; FAT$W_RSIZE = the longest record), so a
     * re-$OPEN with a DEFAULT FAB reads them back -- and every RMS service
     * that names the file returns its File ID in nam$w_fid and its
     * directory's in nam$w_did, the same ID $SEARCH reports. ---- */
    {
        static struct fat_case fc[] = {
            { "FATFIX.DAT", FAB$C_FIX,   0,        20,
              { "fixed one", "fixed record two", "3" }, 20, {0}, {0}, 0 },
            { "FATVAR.DAT", FAB$C_VAR,   FAB$M_CR, 0,
              { "var one", "the longest VAR record here", "v3" }, 27, {0}, {0}, 0 },
            { "FATSTM.DAT", FAB$C_STMLF, FAB$M_CR, 0,
              { "stream line one", "stream two", NULL }, 15, {0}, {0}, 0 },
        };
        const int nfc = (int)(sizeof fc / sizeof fc[0]);
        for (int i = 0; i < nfc; i++) fat_create(&fc[i]);
        for (int i = 0; i < nfc; i++) fat_reopen(&fc[i]);
        for (int i = 0; i < nfc; i++) fat_search_attr(&fc[i]);
        /* If IO$_CREATE dropped the record attributes, the FIX file would
         * keep the preset 512-byte record and re-open as one. */
        /* negctl: acp-fat-recattr-not-applied */
        check(fc[0].hdr_ok && fc[0].get_ok,
              "vms-b447: FIX mrs 20 -- a default-FAB $OPEN reads FIX/mrs 20 from the header and $GETs the 20-byte records byte-exact");
        check(fc[1].hdr_ok && fc[1].get_ok,
              "vms-b447: VAR mrs 0 -- a default-FAB $OPEN reads VAR/CR/mrs 0 from the header and $GETs each record whole");
        check(fc[2].hdr_ok && fc[2].get_ok,
              "vms-b447: STMLF -- a default-FAB $OPEN reads STMLF/CR/mrs 0 from the header and $GETs each line");

        /* vms-263e (folded in from #1560, vms-6f5c): the executive stamps the
         * creation and revision dates into the new header, so F$FILE_ATTRIBUTES
         * CDT/RDT (and DIRECTORY/FULL "Created:"/"Revised:") read real times. */
        {
            static char dout[8192];
            const char *dscript =
                "G = F$FILE_ATTRIBUTES(\"VDA0:[OVMXDIR]FATVAR.DAT\",\"CDT\")\n"
                "H = F$FILE_ATTRIBUTES(\"VDA0:[OVMXDIR]FATVAR.DAT\",\"RDT\")\n"
                "WRITE SYS$OUTPUT \"FA-DATES CDT=[''G'] RDT=[''H']\"\n"
                "LOGOUT\n";
            int dran = (access("/bin/DCL.EXE", X_OK) == 0) && run_dcl(dscript, dout, sizeof dout) == 0;
            const char *ln = strstr(dout, "FA-DATES CDT=[");
            printf("  ---- dates ----\n%s\n  ---- end ----\n", dout);
            /* negctl: acp-create-dates-not-stamped */
            check(dran && ln && strncmp(ln, "FA-DATES CDT=[]", 15) != 0 &&
                  strstr(ln, "RDT=[]") == NULL && strstr(ln, "-20") != NULL,
                  "vms-263e: a file the executive created has a creation and a revision date (F$FILE_ATTRIBUTES CDT/RDT not empty)");
        }

        /* DCL: DIRECTORY/FULL prints the VAR file's record format as a real
         * VMS does (docs/oracle/semantics/rights/vax73-rightslist.txt:
         * "Record format:      Variable length, maximum 64 bytes, longest 0
         * bytes") and its File ID as the NAM's; F$FILE_ATTRIBUTES returns the
         * header's MRS / LRL / RFM / ORG. */
        static char out[32768];
        const char *script =
            "DIRECTORY/FULL VDA0:[OVMXDIR]FATVAR.DAT\n"
            "DIRECTORY/FULL VDA0:[OVMXDIR]FATFIX.DAT\n"
            "DIRECTORY/FULL VDA0:[OVMXDIR]FATSTM.DAT\n"
            "E = F$FILE_ATTRIBUTES(\"VDA0:[OVMXDIR]FATVAR.DAT\",\"RAT\")\n"
            "F = F$FILE_ATTRIBUTES(\"VDA0:[OVMXDIR]FATFIX.DAT\",\"RAT\")\n"
            "WRITE SYS$OUTPUT \"FA-VAR-RAT=''E' FA-FIX-RAT=''F' END\"\n"
            "A = F$FILE_ATTRIBUTES(\"VDA0:[OVMXDIR]FATFIX.DAT\",\"MRS\")\n"
            "B = F$FILE_ATTRIBUTES(\"VDA0:[OVMXDIR]FATFIX.DAT\",\"RFM\")\n"
            "C = F$FILE_ATTRIBUTES(\"VDA0:[OVMXDIR]FATVAR.DAT\",\"LRL\")\n"
            "D = F$FILE_ATTRIBUTES(\"VDA0:[OVMXDIR]FATVAR.DAT\",\"ORG\")\n"
            "WRITE SYS$OUTPUT \"FA-FIX-MRS=''A' FA-FIX-RFM=''B' FA-VAR-LRL=''C' FA-VAR-ORG=''D'\"\n"
            "LOGOUT\n";
        int ran = (access("/bin/DCL.EXE", X_OK) == 0) && run_dcl(script, out, sizeof out) == 0;
        printf("  ---- DCL.EXE ----\n%s\n  ---- end ----\n", out);
        check(ran && strstr(out, "SETUP_FAIL") == NULL, "vms-b447: /bin/DCL.EXE ran DIRECTORY/FULL + F$FILE_ATTRIBUTES");
        check(has_line(out, "Record format:      Variable length, maximum 0 bytes, longest 27 bytes"),
              "vms-b447: DIRECTORY/FULL: \"Record format:      Variable length, maximum 0 bytes, longest 27 bytes\"");
        check(has_line(out, "Record attributes:  Carriage return carriage control"),
              "vms-b447: DIRECTORY/FULL: \"Record attributes:  Carriage return carriage control\"");
        {
            char fidline[64];
            snprintf(fidline, sizeof fidline, "File ID:  (%u,%u,%u)",
                     (unsigned)fc[1].fid[0] | ((unsigned)(fc[1].fid[2] >> 8) << 16),
                     fc[1].fid[1], fc[1].fid[2] & 0xFFu);
            check(fc[1].created && strstr(out, fidline) != NULL,
                  "vms-6e28: DIRECTORY/FULL's \"File ID:\" is the nam$w_fid $CREATE returned");
        }
        /* vms-a44: the other record formats, worded as a VAX V7.3 prints them
         * (tests/lab/captures/decnet-live-brackets-20261008/vax73-dirfull-recfmt.txt). */
        check(has_line(out, "Record format:      Fixed length 20 byte records"),
              "vms-a44: DIRECTORY/FULL of a FIX/mrs 20 file: \"Record format:      Fixed length 20 byte records\"");
        check(has_line(out, "Record attributes:  None"),
              "vms-a44: DIRECTORY/FULL of a file with no record attributes: \"Record attributes:  None\"");
        check(has_line(out, "Record format:      Stream_LF, maximum 0 bytes, longest 15 bytes"),
              "vms-a44: DIRECTORY/FULL of a STMLF file: \"Record format:      Stream_LF, maximum 0 bytes, longest 15 bytes\"");
        check(strstr(out, "FA-VAR-RAT=CR FA-FIX-RAT= END") != NULL,
              "vms-a44: F$FILE_ATTRIBUTES RAT names the attribute as VMS does (CR; empty for none), not a letter code");
        check(strstr(out, "FA-FIX-MRS=20 FA-FIX-RFM=FIX FA-VAR-LRL=27 FA-VAR-ORG=SEQ") != NULL,
              "vms-b447: F$FILE_ATTRIBUTES: FIX MRS 20, RFM FIX; VAR LRL 27, ORG SEQ");

        for (int i = 0; i < nfc; i++) fat_erase(&fc[i]);
    }

    vms_kif_acp_dmount(ODS2_UNIT);

    printf("=== RMS-over-ACP: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
