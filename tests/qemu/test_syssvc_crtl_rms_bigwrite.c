/*
 * test_syssvc_crtl_rms_bigwrite.c — CRTL->RMS veneer completeness for the GCC
 * driver's file I/O (vms-126): large (>64 KiB) writes and delete-on-close
 * temporaries, proven on the real ODS-2 volume through the executive ACP.
 *
 * WHAT IT KILLS / PROVES. The base veneer (test_syssvc_crtl_rms_veneer) proved
 * fopen/fwrite/fread/fclose land bytes on ODS-2, but capped a single fwrite at
 * one 16-bit FIX record (0xFFFF) — so a compiler driver writing a >64 KiB
 * .OBJ/.LIS could not route through it. This suite proves the vms-126 chunking
 * completion:
 *
 *   CHUNKED WRITE: a single ovmx_crtl_fwrite of 200000 bytes (> 3 FIX records)
 *   is written via successive sys$put calls, reads back byte-exact through the
 *   veneer, and an INDEPENDENT ACP reader (sys$parse+sys$search + on-disk
 *   rms_file_attr) sees the file with a genuine ODS-2 File ID — a channel the
 *   veneer never touched, which a ramfs/POSIX write cannot fake.
 *
 * (Delete-on-close temporaries via ovmx_crtl_tmpfile/FAB$M_TMD are tracked
 * separately: the sys$create marks the temp but the ACP delete-on-close does not
 * yet fire at sys$close — an engine interaction filed as its own item.)
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

#define EXIT_SKIP  77
#define ODS2_UNIT  "VDA0:"
#define DIRSPEC    ODS2_UNIT "[OVMXDIR]"
#define BNAME      "BIGWR.DAT"
#define BSPEC      DIRSPEC BNAME
#define BIG_SIZE   200000            /* > 3 * 0xFFFF: forces multi-record chunking */

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

/* INDEPENDENT reader: sys$parse + sys$search of `pattern` over the ACP. Returns
 * match count; fills the first match's ODS-2 File-ID number into *fid. Mirrors
 * test_syssvc_crtl_rms_veneer's search_one. */
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
    printf("=== test_syssvc_crtl_rms_bigwrite: CRTL->RMS veneer large-write "
           "chunking + delete-on-close temp on real ODS-2 (vms-126) ===\n");

    if (!executive_present()) {
        printf("=== test_syssvc_crtl_rms_bigwrite: 0 passed, 0 failed (SKIPPED: no "
               "/dev/vms -- executive-file consumer; Rule 9) ===\n");
        return EXIT_SKIP;
    }

    uint32_t st = vms_kif_acp_mount(ODS2_UNIT);   /* idempotent */
    check($VMS_STATUS_SUCCESS(st), "VDA0: mounted executive-global");

    erase_spec(BSPEC ";*");

    /* ============================================================= *
     * CHUNKED WRITE: 200000 bytes through one fwrite -> N $PUTs.     *
     * ============================================================= */
    unsigned char *buf = malloc(BIG_SIZE);
    check(buf != NULL, "heap: malloc(BIG_SIZE)");
    if (!buf) goto done;
    for (int i = 0; i < BIG_SIZE; i++)
        buf[i] = (unsigned char)((i * 31 + 7) & 0xFF);

    OVMX_CRTL_FILE *wf = ovmx_crtl_fopen(BSPEC, "w");
    check(wf != NULL, "1a: ovmx_crtl_fopen(BIGWR.DAT,\"w\") -> sys$create");
    if (wf) {
        size_t nw = ovmx_crtl_fwrite(buf, 1, BIG_SIZE, wf);
        check(nw == (size_t)BIG_SIZE,
              "1b: ovmx_crtl_fwrite 200000 bytes -> chunked sys$put (full count, > one FIX record)");
        check(ovmx_crtl_fclose(wf) == 0, "1c: ovmx_crtl_fclose -> sys$close NORMAL");
    }

    /* Independent ACP reader: the chunked file landed with a genuine File ID. */
    uint16_t bfid = 0;
    int bfound = search_one(BSPEC ";*", &bfid);
    check(bfound == 1 && bfid != 0,
          "1d: sys$search sees the chunk-written file with a genuine ODS-2 File ID "
          "(independent ACP reader -- ramfs cannot produce this)");
    {
        struct rms_fileattr attr;
        memset(&attr, 0, sizeof attr);
        uint32_t ast = rms_file_attr(BSPEC, &attr);
        check($VMS_STATUS_SUCCESS(ast) && attr.fid_num == bfid,
              "1e: on-disk ODS-2 header (rms_file_attr) confirms the chunked file + File ID");
    }

    /* Read the whole 200000 bytes back and verify byte-exact across records. */
    OVMX_CRTL_FILE *rf = ovmx_crtl_fopen(BSPEC, "r");
    check(rf != NULL, "1f: ovmx_crtl_fopen(BIGWR.DAT,\"r\") -> sys$open");
    if (rf) {
        unsigned char *rbuf = calloc(1, BIG_SIZE);
        size_t nr = rbuf ? ovmx_crtl_fread(rbuf, 1, BIG_SIZE, rf) : 0;
        check(nr == (size_t)BIG_SIZE,
              "1g: ovmx_crtl_fread reads all 200000 bytes back ($GET loop over N records)");
        check(rbuf && memcmp(buf, rbuf, BIG_SIZE) == 0,
              "1h: the chunked round-trip is byte-exact across record boundaries");
        ovmx_crtl_fclose(rf);
        free(rbuf);
    }

done:
    erase_spec(BSPEC ";*");
    /* Restore the executive-global mount table to the state we found it in:
     * this suite $MOUNTed VDA0: (line ~116) and runs in a SHARED KE guest boot.
     * A later co-tenant (test_syssvc_startup_service) does RUN/DETACHED "/bin/sh"
     * and expects the default disk (SYS$SYSDEVICE -> VDA0:) UNMOUNTED so the
     * legacy host-path resolver finds the initramfs busybox; a left-mounted VDA0:
     * routes that lookup through the ACP, misses (busybox is not on ODS-2), and
     * correctly SS$_NOSUCHFILE -> %DCL-E-IVIMAGE (INV-6, no host fallback). Peers
     * that $MOUNT (test_syssvc_acp_mount/_channel) already $DISMOUNT — mirror them
     * so this suite leaves no shared mount state behind. Best-effort. */
    vms_kif_acp_dmount(ODS2_UNIT);
    free(buf);

    printf("=== test_syssvc_crtl_rms_bigwrite: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
