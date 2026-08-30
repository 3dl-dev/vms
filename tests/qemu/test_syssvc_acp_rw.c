/*
 * test_syssvc_acp_rw.c - the Files-11 (ODS-2) ACP answers IO$_READVBLK /
 * IO$_WRITEVBLK: virtual-block transfer on an ACCESSED file channel through its
 * VBN->LBN window, byte-exact, with an implicit EXTEND on a write past EOF
 * (vms-c60, epic vms-208), proven against a real /dev/vms.
 *
 * The FIFTH rung of the executive ACP (after the kernel-resident codec vms-dcd,
 * the channel front-end vms-149, the executive-global $MOUNT vms-127, and
 * IO$_ACCESS/DEACCESS vms-204): the ACP-QIO record/block I/O the IO$_ACCESS
 * window was built to carry. On real OpenVMS these are $QIO(IO$_READVBLK /
 * IO$_WRITEVBLK) on a channel $ASSIGNed to a mounted volume with a file
 * accessed, serviced by the XQP in the caller's context; RMS $GET/$PUT are
 * layered on them (VSI I/O User's Reference, "ACP-QIO Interface"). OVMX reaches
 * the executive over /dev/vms via vms_kif_acp_readvb()/_writevb().
 *
 * WHAT THIS SUITE PROVES, through the sys$/kif API against a real /dev/vms, over
 * the real-VAX ODS-2 fixture the harness seeds on VDA0: (run_tests.sh, from the
 * staged /ods2_real.img == tests/ods2/real_vax_ods2.dsk, copied to a WRITABLE
 * temp so the write below never touches the committed fixture):
 *
 *   1. IO$_READVBLK IS BYTE-EXACT vs THE CODEC. IO$_ACCESS [OVMXDIR]HELLO.TXT
 *      then READVBLK its whole 17218 valid bytes (efblk 34, ffbyte 322) through
 *      the window returns EXACTLY the committed golden (tests/ods2/
 *      ovmxdir_hello.golden -- the real on-disk content, produced by the
 *      userspace codec), byte for byte. The window mapping is the genuine ACP's.
 *
 *   2. IO$_WRITEVBLK IN PLACE round-trips byte-exact. Accessed for WRITE,
 *      WRITEVBLK a known pattern to VBN 1, then READVBLK VBN 1 back == the
 *      pattern; and after DEACCESS + re-ACCESS (window rebuilt from the on-disk
 *      FH2, block re-read) READVBLK VBN 1 STILL == the pattern -- proving it hit
 *      the platter, not a per-process buffer (INV-6).
 *
 *   3. IO$_WRITEVBLK PAST EOF IMPLICITLY EXTENDS. WRITEVBLK a pattern to VBN 35
 *      (one past the file's 34 allocated blocks) reports extended==1,
 *      new_hiblk==35, new_efblk==35; after DEACCESS + re-ACCESS the FH2 reports
 *      the grown size (attr hiblk/efblk 35) and READVBLK VBN 35 reads the
 *      pattern back byte-exact -- a NEW block was allocated from BITMAP.SYS, the
 *      retrieval map + EOF grew, all on disk. VBN 1 still reads (2)'s pattern.
 *
 *   4. FAIL-HONEST EDGES (INV-6, never a fabricated success):
 *        - WRITEVBLK on a channel accessed READ-ONLY is SS$_NOPRIV;
 *        - READVBLK starting past EOF is SS$_ENDOFFILE;
 *        - a bad byte-offset (>= 512) is SS$_BADPARAM.
 *
 * ORDERING / ISOLATION. This suite MUTATES [OVMXDIR]HELLO.TXT (and allocates a
 * BITMAP.SYS block) on its VDA0: fixture COPY. That is safe: init.sh runs every
 * test_kmod_* before every test_syssvc_*, and globs sort, so both HELLO.TXT
 * content readers -- test_kmod_ods2_codec (kmod group) and test_syssvc_acp_access
 * (alphabetically before "_rw") -- run BEFORE this suite, and each shard boots
 * its OWN fresh writable fixture copy (run_tests.sh cp per boot). So no reader
 * ever observes this suite's mutation. (The by-the-book isolation is a dedicated
 * scratch volume; VDA0: is reused here to avoid a third virtio disk, which would
 * break the VDA200: "no third disk" negative controls in test_kmod_disk /
 * test_kmod_vmsfs_mountvis.)
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass: the ACP, the window, and
 * the transfer are executive-resident, so with no /dev/vms there is nothing to
 * assert (the contract every test_syssvc_* suite is held to, ci.yml).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

#define ODS2_UNIT       "VDA0:"
#define GOLDEN_PATH     "/test_data/hello.golden"

/* Ground truth read off tests/ods2/real_vax_ods2.dsk (see acp_access suite). */
#define OVMXDIR_FID_NUM   11u
#define HELLO_EFBLK       34u    /* end-of-file / highest allocated VBN */
#define HELLO_FFBYTE      322u   /* valid bytes in the EOF block */
#define HELLO_VALID       (((HELLO_EFBLK - 1u) * 512u) + HELLO_FFBYTE)  /* 17218 */

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

/* ACCESS [OVMXDIR]HELLO.TXT on `chan`, for write iff want_write; returns the
 * IO$_ACCESS status and (on success) the file's attributes in *a. */
static uint32_t access_hello(uint32_t chan, int want_write,
                             struct vms_acp_access_args *a)
{
    memset(a, 0, sizeof(*a));
    a->chan = chan;
    a->did_num = OVMXDIR_FID_NUM;   /* FIB$W_DID = [OVMXDIR] */
    a->did_seq = 1;
    if (want_write)
        a->acctl = VMS_ACP_ACCTL_WRITE;
    strncpy(a->name, "HELLO.TXT", VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_access(a);
}

static void rw_init(struct vms_acp_rw_args *r, uint32_t chan, uint32_t vbn,
                    uint32_t offset, uint32_t length, void *buf)
{
    memset(r, 0, sizeof(*r));
    r->chan = chan;
    r->vbn = vbn;
    r->offset = offset;
    r->length = length;
    r->buffer = (uint64_t)(uintptr_t)buf;
}

int main(void)
{
    struct vms_acp_access_args a;
    struct vms_acp_rw_args r;
    uint32_t st, chan = 0;
    uint8_t *golden = NULL, *rdbuf = NULL, *pat1 = NULL, *pat2 = NULL;
    long golden_len = 0;
    int have_golden = 0;
    unsigned i;

    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== test_syssvc_acp_rw: executive ACP answers IO$_READVBLK/IO$_WRITEVBLK "
           "(virtual-block transfer, implicit extend; vms-c60, epic vms-208) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        printf("=== test_syssvc_acp_rw: 0 passed, 0 failed (SKIPPED: no /dev/vms -- "
               "the ACP, the window and the transfer are executive-resident) ===\n");
        return EXIT_SKIP;
    }

    rdbuf = malloc(HELLO_VALID + 4096);
    pat1  = malloc(512);
    pat2  = malloc(512);
    if (!rdbuf || !pat1 || !pat2) {
        printf("  FAIL: malloc\n");
        return 1;
    }
    for (i = 0; i < 512; i++) { pat1[i] = (uint8_t)(0xA5u ^ i); pat2[i] = (uint8_t)(0x5Au + i); }

    /* Committed golden (the codec's own read of HELLO.TXT) -- the byte-exact
     * oracle for (1). Absent -> the round-trip proofs still run. */
    {
        FILE *g = fopen(GOLDEN_PATH, "rb");
        if (g) {
            golden = malloc(HELLO_VALID + 16);
            if (golden) {
                golden_len = (long)fread(golden, 1, HELLO_VALID + 16, g);
                have_golden = (golden_len == (long)HELLO_VALID);
            }
            fclose(g);
        }
    }

    /* Precondition: VDA0: mounted executive-global + a file-class channel. */
    st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$MOUNT of the genuine ODS-2 " ODS2_UNIT " (precondition)");
    st = vms_kif_acp_assign(ODS2_UNIT, &chan);
    check($VMS_STATUS_SUCCESS(st) && chan != 0, "$ASSIGN a file-class channel (precondition)");
    if (chan == 0) {
        printf("=== test_syssvc_acp_rw: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* --- (1) READVBLK byte-exact vs the codec golden ---------------------- */
    st = access_hello(chan, 0, &a);
    check($VMS_STATUS_SUCCESS(st) && a.attr.efblk == HELLO_EFBLK,
          "IO$_ACCESS HELLO.TXT for read (efblk 34)");
    rw_init(&r, chan, 1, 0, HELLO_VALID, rdbuf);
    st = vms_kif_acp_readvb(&r);
    check($VMS_STATUS_SUCCESS(st) && r.xferred == HELLO_VALID,
          "IO$_READVBLK returns all 17218 valid bytes of HELLO.TXT");
    if (have_golden)
        check($VMS_STATUS_SUCCESS(st) && r.xferred == HELLO_VALID &&
              memcmp(rdbuf, golden, HELLO_VALID) == 0,
              "IO$_READVBLK content is BYTE-EXACT vs the committed codec golden");
    else
        printf("  INFO: %s absent -- byte-exact-vs-golden check skipped (round-trip still proves the path)\n",
               GOLDEN_PATH);
    /* Read-past-EOF fail-honest: start at VBN 34 offset 322 == EOF exactly. */
    rw_init(&r, chan, HELLO_EFBLK, HELLO_FFBYTE, 16, rdbuf);
    st = vms_kif_acp_readvb(&r);
    check(st == SS$_ENDOFFILE, "IO$_READVBLK starting AT end-of-file is SS$_ENDOFFILE (fail-honest)");
    (void)vms_kif_acp_deaccess(chan);

    /* --- write to a READ-ONLY-accessed channel is refused ----------------- */
    st = access_hello(chan, 0, &a);
    check($VMS_STATUS_SUCCESS(st), "re-ACCESS HELLO.TXT read-only (for the write-lock proof)");
    rw_init(&r, chan, 1, 0, 512, pat1);
    st = vms_kif_acp_writevb(&r);
    check(st == SS$_NOPRIV,
          "IO$_WRITEVBLK on a channel accessed READ-ONLY is SS$_NOPRIV (fail-honest)");
    /* bad byte-offset */
    rw_init(&r, chan, 1, 512, 4, pat1);
    st = vms_kif_acp_writevb(&r);
    check(st == SS$_BADPARAM, "IO$_WRITEVBLK with byte-offset >= 512 is SS$_BADPARAM (fail-honest)");
    (void)vms_kif_acp_deaccess(chan);

    /* --- (2) WRITEVBLK in place, byte-exact round-trip -------------------- */
    st = access_hello(chan, 1, &a);
    check($VMS_STATUS_SUCCESS(st), "IO$_ACCESS HELLO.TXT for WRITE");
    rw_init(&r, chan, 1, 0, 512, pat1);
    st = vms_kif_acp_writevb(&r);
    check($VMS_STATUS_SUCCESS(st) && r.xferred == 512 && r.extended == 0,
          "IO$_WRITEVBLK of VBN 1 in place (no extend)");
    memset(rdbuf, 0, 512);
    rw_init(&r, chan, 1, 0, 512, rdbuf);
    st = vms_kif_acp_readvb(&r);
    check($VMS_STATUS_SUCCESS(st) && memcmp(rdbuf, pat1, 512) == 0,
          "IO$_READVBLK VBN 1 on the same channel reads the written pattern back");
    (void)vms_kif_acp_deaccess(chan);
    /* persistence: re-ACCESS (window rebuilt from the on-disk FH2) and re-read */
    st = access_hello(chan, 0, &a);
    memset(rdbuf, 0, 512);
    rw_init(&r, chan, 1, 0, 512, rdbuf);
    st = vms_kif_acp_readvb(&r);
    check($VMS_STATUS_SUCCESS(st) && memcmp(rdbuf, pat1, 512) == 0,
          "after DEACCESS + re-ACCESS, VBN 1 STILL reads the pattern -- it hit the platter (INV-6)");
    (void)vms_kif_acp_deaccess(chan);

    /* --- (3) WRITEVBLK past EOF implicitly extends ------------------------ */
    st = access_hello(chan, 1, &a);
    check($VMS_STATUS_SUCCESS(st), "IO$_ACCESS HELLO.TXT for WRITE (extend)");
    rw_init(&r, chan, HELLO_EFBLK + 1u, 0, 512, pat2);   /* VBN 35, one past HIBLK */
    st = vms_kif_acp_writevb(&r);
    check($VMS_STATUS_SUCCESS(st) && r.xferred == 512 && r.extended == 1 &&
          r.new_hiblk == HELLO_EFBLK + 1u && r.new_efblk == HELLO_EFBLK + 1u,
          "IO$_WRITEVBLK past EOF extends: extended=1, HIBLK 35, EOF 35 (BITMAP.SYS alloc + FH2 grow)");
    /* same channel: VBN 35 reads pattern2, VBN 1 still reads pattern1 */
    memset(rdbuf, 0, 512);
    rw_init(&r, chan, HELLO_EFBLK + 1u, 0, 512, rdbuf);
    st = vms_kif_acp_readvb(&r);
    check($VMS_STATUS_SUCCESS(st) && memcmp(rdbuf, pat2, 512) == 0,
          "IO$_READVBLK of the freshly extended VBN 35 reads its pattern back");
    (void)vms_kif_acp_deaccess(chan);
    /* persistence: re-ACCESS -> FH2 reports the grown size, read the new block */
    st = access_hello(chan, 0, &a);
    check($VMS_STATUS_SUCCESS(st) && a.attr.efblk == HELLO_EFBLK + 1u,
          "after re-ACCESS the on-disk FH2 reports the grown end-of-file (efblk 35)");
    memset(rdbuf, 0, 512);
    rw_init(&r, chan, HELLO_EFBLK + 1u, 0, 512, rdbuf);
    st = vms_kif_acp_readvb(&r);
    /* negctl: acp-writevb-extend-alloc-offbyone -- the extent PERSISTED to the
     * on-disk FH2 map must point at the SAME LBN the data was written to. An
     * off-by-one in ods2_fh2_map_append's LBN encoding writes the data to LBN X
     * but records the extent at X+1, so the same-channel read above (in-memory
     * window, correct LBN) still passes while THIS re-ACCESS read (window rebuilt
     * from the corrupted on-disk map) reads the wrong block. */
    /* negctl: acp-writevb-extend-alloc-offbyone */
    check($VMS_STATUS_SUCCESS(st) && memcmp(rdbuf, pat2, 512) == 0,
          "after DEACCESS + re-ACCESS, the extended VBN 35 STILL reads its pattern (allocation persisted)");
    memset(rdbuf, 0, 512);
    rw_init(&r, chan, 1, 0, 512, rdbuf);
    st = vms_kif_acp_readvb(&r);
    check($VMS_STATUS_SUCCESS(st) && memcmp(rdbuf, pat1, 512) == 0,
          "VBN 1 still reads pattern1 after the extend -- the new extent did not collide with the old");
    (void)vms_kif_acp_deaccess(chan);

    (void)vms_kif_dassgn(chan);
    st = vms_kif_acp_dmount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$DISMOUNT removes the ODS-2 volume");

    free(rdbuf); free(pat1); free(pat2); free(golden);
    printf("=== test_syssvc_acp_rw: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
