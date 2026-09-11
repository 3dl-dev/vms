/*
 * test_syssvc_crtl_rms_fileops.c — x86_64 arch-discriminator for vms-3c1: does
 * the CRTL->RMS FILE-OP veneer (src/vmsrms/crtl_rms_stdio.c's vms-3320
 * open/creat/unlink/remove/rename/opendir/readdir/closedir additions) corrupt
 * the heap on the real executive ACP path, or is the crtl_rms3 SIGSEGV
 * (alpha-dec-vms cross-built joint-e2e image, tools/cross-alpha-vms/joint-e2e/
 * crtl_rms3_test.c) specific to the alpha cross-compiler's codegen?
 *
 * WHAT IT KILLS / SCOPE. test_syssvc_crtl_rms_veneer.c only drives the stdio
 * family (fopen/fwrite/fread/fclose) -- the mallocng calloc(56)->memset
 * SIGSEGV crtl_rms3 hits on alpha happens on a file-op sequence that suite
 * NEVER exercises (empty create+close, open-for-read+close, unlink, rename,
 * opendir/readdir/closedir). This suite drives EXACTLY that sequence against
 * the real /dev/vms executive + ODS-2 ACP on x86_64, built WITH
 * AddressSanitizer, so any heap-buffer-overflow / use-after-free / double-free
 * reachable from the veneer, the RMS engine, or the kernel ACP handler on
 * THIS arch aborts with a named file:line -- proving the corruption is
 * cross-arch (reachable on x86_64 too) rather than alpha-codegen-specific.
 *
 *   1. creat + close                    (empty file, no data -- crtl_rms3 op 1)
 *   2. open(O_RDONLY) + close           (no fread -- crtl_rms3 op 2)
 *   3. creat + close + unlink           (mint-then-delete -- crtl_rms3 op 3)
 *   4. creat + close + rename           (atomic re-link -- crtl_rms3 op 4)
 *   5. opendir / readdir* / closedir    (directory enumeration -- crtl_rms3 op 5)
 *
 * Every step is corroborated by an INDEPENDENT ACP reader (sys$parse+
 * sys$search, mirroring test_syssvc_crtl_rms_veneer's search_one) -- a ramfs
 * or a heap-corrupted veneer state cannot fake a genuine ODS-2 File ID.
 *
 * NO /dev/vms -> honest SKIP (77): the veneer is an executive-file consumer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vms_kif.h"
#include "rms/rms.h"
#include "rms/crtl_stdio.h"

/* Alpha (OSF/1) open() flag ABI -- matches crtl_rms3_test.c's own #defines, the
 * exact caller-side values the veneer's ovmx_crtl_open interprets. */
#ifndef O_RDONLY
#define O_RDONLY   0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY   0x0001
#endif
#ifndef O_CREAT
#define O_CREAT    0x0200
#endif
#ifndef O_TRUNC
#define O_TRUNC    0x0400
#endif

#define EXIT_SKIP  77
#define ODS2_UNIT  "VDA0:"
#define DIRSPEC    ODS2_UNIT "[OVMXDIR]"

#define FOPCRE     DIRSPEC "FOPCRE.DAT"   /* op 1: creat+close, left for op 2 */
#define FOPDEL     DIRSPEC "FOPDEL.DAT"   /* op 3: creat+close then unlink    */
#define FOPSRC     DIRSPEC "FOPSRC.DAT"   /* op 4: creat+close then rename    */
#define FOPDST     DIRSPEC "FOPDST.DAT"   /* op 4: rename target              */
#define FOPDIRA    DIRSPEC "FOPDIRA.DAT"  /* op 5: enumerated                 */
#define FOPDIRB    DIRSPEC "FOPDIRB.DAT"  /* op 5: enumerated                 */

static int pass = 0, fail = 0;

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

/* INDEPENDENT reader: sys$parse + sys$search of `pattern` over the ACP.
 * Returns match count; fills the first match's ODS-2 File-ID number into
 * *fid. Mirrors test_syssvc_crtl_rms_veneer's search_one. */
static int search_one(const char *pattern, uint16_t *fid)
{
    struct FAB fab;
    struct NAM nam;
    char esa[512], rsa[512];
    int n = 0;

    if (fid) *fid = 0;

    fab = cc$rms_fab;
    fab.fab$l_fna = (char *)pattern;
    fab.fab$b_fns = (uint8_t)strlen(pattern);
    nam = cc$rms_nam;
    nam.nam$l_esa = esa;
    nam.nam$b_ess = (uint8_t)(sizeof(esa) > 255 ? 255 : sizeof(esa));
    nam.nam$l_rsa = rsa;
    nam.nam$b_rss = (uint8_t)(sizeof(rsa) > 255 ? 255 : sizeof(rsa));
    fab.fab$l_nam = &nam;

    if (sys$parse(&fab, 0, 0) != RMS$_NORMAL) {
        rms_search_end(&nam);
        return 0;
    }
    while (sys$search(&fab, 0, 0) == RMS$_NORMAL) {
        if (n == 0 && fid) {
            uint16_t num = 0, seq = 0; uint8_t rvn = 0, nmx = 0;
            rms_search_fid(&nam, &num, &seq, &rvn, &nmx);
            *fid = num;
        }
        n++;
    }
    rms_search_end(&nam);
    return n;
}

static uint32_t erase_spec(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    return sys$erase(&fab, 0, 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_crtl_rms_fileops: crtl_rms3's exact file-op "
           "sequence (create/close, open/close, unlink, rename, "
           "opendir/readdir/closedir) over the real executive ACP -- vms-3c1 "
           "x86_64 arch-discriminator ===\n");

    if (!executive_present()) {
        printf("=== test_syssvc_crtl_rms_fileops: 0 passed, 0 failed (SKIPPED: "
               "no /dev/vms -- the CRTL veneer is an executive-file consumer, "
               "nothing to assert without a real ACP; Rule 9) ===\n");
        return EXIT_SKIP;
    }

    uint32_t st = vms_kif_acp_mount(ODS2_UNIT);   /* idempotent */
    check($VMS_STATUS_SUCCESS(st), "VDA0: mounted executive-global for the file-op proof");

    /* Clean any stale fixtures from a prior run. */
    erase_spec(FOPCRE ";*");
    erase_spec(FOPDEL ";*");
    erase_spec(FOPSRC ";*");
    erase_spec(FOPDST ";*");
    erase_spec(FOPDIRA ";*");
    erase_spec(FOPDIRB ";*");

    /* ================================================================= *
     * 1. creat + close: EMPTY file, no fwrite/put at all -- crtl_rms3    *
     *    op 1. sys$close finalizes the ODS-2 header/FH2.                 *
     * ================================================================= */
    int cfd = ovmx_crtl_creat(FOPCRE, 0);
    check(cfd >= OVMX_CRTL_FD_BASE,
          "1a: ovmx_crtl_creat(FOPCRE.DAT) -> sys$create, veneer fd");
    if (cfd >= 0)
        check(ovmx_crtl_fdclose(cfd) == 0, "1b: ovmx_crtl_fdclose -> sys$close NORMAL");

    uint16_t cre_fid = 0;
    check(search_one(FOPCRE ";*", &cre_fid) == 1 && cre_fid != 0,
          "1c: independent sys$search sees the empty creat file with a genuine "
          "ODS-2 File ID (ramfs/heap-corrupted state cannot produce this)");

    /* ================================================================= *
     * 2. open(O_RDONLY) + close: no fread/get -- crtl_rms3 op 2.         *
     * ================================================================= */
    int ofd = ovmx_crtl_open(FOPCRE, O_RDONLY);
    check(ofd >= OVMX_CRTL_FD_BASE,
          "2a: ovmx_crtl_open(FOPCRE.DAT, O_RDONLY) -> sys$open, veneer fd");
    if (ofd >= 0)
        check(ovmx_crtl_fdclose(ofd) == 0, "2b: ovmx_crtl_fdclose -> sys$close NORMAL");

    /* ================================================================= *
     * 3. creat + close + unlink -- crtl_rms3 op 3.                       *
     * ================================================================= */
    int dfd = ovmx_crtl_creat(FOPDEL, 0);
    check(dfd >= OVMX_CRTL_FD_BASE, "3a: ovmx_crtl_creat(FOPDEL.DAT) -> sys$create");
    if (dfd >= 0)
        check(ovmx_crtl_fdclose(dfd) == 0, "3b: ovmx_crtl_fdclose -> sys$close NORMAL");
    check(ovmx_crtl_unlink(FOPDEL) == 0, "3c: ovmx_crtl_unlink -> sys$erase NORMAL");
    check(search_one(FOPDEL ";*", NULL) == 0,
          "3d: independent sys$search sees the unlinked file GONE");

    /* ================================================================= *
     * 4. creat + close + rename (atomic re-link, SAME File ID) --        *
     *    crtl_rms3 op 4.                                                 *
     * ================================================================= */
    int sfd = ovmx_crtl_creat(FOPSRC, 0);
    check(sfd >= OVMX_CRTL_FD_BASE, "4a: ovmx_crtl_creat(FOPSRC.DAT) -> sys$create");
    if (sfd >= 0)
        check(ovmx_crtl_fdclose(sfd) == 0, "4b: ovmx_crtl_fdclose -> sys$close NORMAL");

    uint16_t src_fid = 0;
    check(search_one(FOPSRC ";*", &src_fid) == 1 && src_fid != 0,
          "4c: independent sys$search sees FOPSRC.DAT with File ID X");

    check(ovmx_crtl_rename(FOPSRC, FOPDST) == 0,
          "4d: ovmx_crtl_rename -> sys$rename (ACP MODIFY!M_MOVE) NORMAL");
    check(search_one(FOPSRC ";*", NULL) == 0,
          "4e: independent sys$search sees the OLD name FOPSRC.DAT GONE");
    uint16_t dst_fid = 0;
    check(search_one(FOPDST ";*", &dst_fid) == 1 && dst_fid == src_fid,
          "4f: independent sys$search sees FOPDST.DAT with the SAME File ID as "
          "FOPSRC had (atomic re-link, not erase+create)");

    /* ================================================================= *
     * 5. opendir / readdir* / closedir -- crtl_rms3 op 5. This is the    *
     *    exact sequence step (dir enumeration after the file-op run)     *
     *    crtl_rms3's own self-check performs before returning sentinel 7.*
     * ================================================================= */
    {
        int afd = ovmx_crtl_creat(FOPDIRA, 0);
        if (afd >= 0) ovmx_crtl_fdclose(afd);
        int bfd = ovmx_crtl_creat(FOPDIRB, 0);
        if (bfd >= 0) ovmx_crtl_fdclose(bfd);
        check(afd >= OVMX_CRTL_FD_BASE && bfd >= OVMX_CRTL_FD_BASE,
              "5a: creat FOPDIRA.DAT + FOPDIRB.DAT for enumeration");

        OVMX_CRTL_DIR *dp = ovmx_crtl_opendir(DIRSPEC);
        check(dp != NULL, "5b: ovmx_crtl_opendir(dir) -> sys$parse over the ACP");
        int saw_a = 0, saw_b = 0, saw_cre = 0, saw_dst = 0, saw_del = 0, saw_src = 0;
        int nent = 0;
        if (dp) {
            struct ovmx_crtl_dirent *e;
            while ((e = ovmx_crtl_readdir(dp)) != NULL) {
                nent++;
                if (strstr(e->d_name, "FOPDIRA.DAT")) saw_a = 1;
                if (strstr(e->d_name, "FOPDIRB.DAT")) saw_b = 1;
                if (strstr(e->d_name, "FOPCRE.DAT"))  saw_cre = 1;
                if (strstr(e->d_name, "FOPDST.DAT"))  saw_dst = 1;
                if (strstr(e->d_name, "FOPDEL.DAT"))  saw_del = 1;
                if (strstr(e->d_name, "FOPSRC.DAT"))  saw_src = 1;
            }
            check(ovmx_crtl_closedir(dp) == 0,
                  "5c: ovmx_crtl_closedir -> rms_search_end (context released)");
        }
        printf("  [independent ACP reader] readdir enumerated %d entries; "
               "a=%d b=%d cre=%d dst=%d del=%d src=%d\n",
               nent, saw_a, saw_b, saw_cre, saw_dst, saw_del, saw_src);
        check(saw_a && saw_b,
              "5d: readdir enumerated both real ODS-2 entries created for this step");
        check(saw_cre && saw_dst,
              "5e: readdir agrees the op-1 creat and op-4 rename-target survive");
        check(!saw_del && !saw_src,
              "5f: readdir agrees the op-3 unlink and op-4 rename-source are GONE "
              "(same self-check crtl_rms3 performs before its sentinel-7 return)");
    }

    erase_spec(FOPCRE ";*");
    erase_spec(FOPDST ";*");
    erase_spec(FOPDIRA ";*");
    erase_spec(FOPDIRB ";*");

    /* Restore the executive-global mount table: this suite $MOUNTed VDA0: and
     * runs in a SHARED KE guest boot. Mirror test_syssvc_crtl_rms_bigwrite's
     * $DISMOUNT (the mount-leak lesson) so a later co-tenant that expects
     * SYS$SYSDEVICE unmounted (e.g. test_syssvc_startup_service's RUN/DETACHED
     * "/bin/sh") still finds it that way. Best-effort. */
    vms_kif_acp_dmount(ODS2_UNIT);

    printf("=== test_syssvc_crtl_rms_fileops: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
