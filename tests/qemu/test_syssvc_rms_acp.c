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

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "rms/rms.h"

#define EXIT_SKIP  77
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
    fab.fab$b_rfm = rfm;                 /* RFM supplied by the reader (FAT-persist deferred) */
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

    /* If rms_io_write() ($PUT) writes a record one VBN too high while
     * rms_io_read() ($GET) still reads the true VBN, every byte-exact readback
     * above misses on re-$OPEN -- so this whole-suite gate reddens exactly when
     * the block-I/O substrate stops being byte-exact through the ACP window. */
    /* negctl: rms-put-wrong-vbn */
    check(fail == 0,
          "RMS-over-ACP: all records round-tripped byte-exact through the ACP window");

    vms_kif_acp_dmount(ODS2_UNIT);

    printf("=== RMS-over-ACP: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
