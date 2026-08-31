/*
 * test_syssvc_crtl_rms_veneer.c — the anti-fabrication proof for the C RTL
 * stdio -> RMS binding (vms-47e).
 *
 * WHAT IT KILLS. The alpha GCC-port's DECC$SHR fopen/fwrite/fread/fclose are
 * musl POSIX whose open()/write() are a raw kernel-VFS callsys — they never
 * reach RMS, the executive, or the ODS-2 volume (trace-grounded finding,
 * docs/design-gcc-port-surface-gaps-register.md §3.1). The alpha crtl_rms N=7
 * gate proved a same-CRTL fwrite->fread round-trip that a ramfs satisfies
 * IDENTICALLY, so it never proved ODS-2 landing. This suite proves the genuine
 * veneer (src/vmsrms/crtl_rms_stdio.c: ovmx_crtl_fopen/fwrite/fread/fclose ->
 * sys$create/$connect/$put/$get/$close) with the ONE thing a ramfs cannot fake:
 * a DIFFERENT reader — the executive ACP directory search (sys$parse+sys$search)
 * and the on-disk ODS-2 header (rms_file_attr) — sees the veneer-written file on
 * the real Files-11 volume with a genuine File ID and a ;1 version.
 *
 * WHY THIS IS UN-FAKEABLE. A same-CRTL read-back (what the alpha N=7 gate does)
 * proves nothing about WHERE the bytes went — ramfs round-trips perfectly. This
 * suite instead reads the file through a channel the veneer never touched:
 *   - sys$search over the ACP returns the resultant DEV:[DIR]NAME.TYP;VER and a
 *     genuine ODS-2 File ID (rms_search_fid) from the executive directory scan;
 *   - rms_file_attr reads the on-disk header (IO$_ACCESS) for the FID + version.
 * A ramfs POSIX write produces NEITHER an ODS-2 File ID NOR a ;1 version, so a
 * pass here is proof the veneer's fopen/fwrite genuinely landed on the volume.
 *
 * SCOPE (honest, no silent drop). This proves the veneer MECHANISM on the real
 * executive/ODS-2/ACP — the layer where an independent ODS-2 reader exists
 * today. Re-pointing the ALPHA port image's DECC$SHR decc$fopen ONTO this veneer
 * (compile crtl_rms_stdio.c into the alpha DECC$SHR, --use LIBVMSRMS$SHR in the
 * port link, and add the independent-reader step to the alpha boot gate) is the
 * remaining wiring, tracked as the vms-47e child — see the register §3.2. The
 * alpha-dec-vms port world has no RMS/vms_kif substrate yet, so that wiring is a
 * separate build, not a same-session re-point.
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
#define VNAME      "VENEER.DAT"
#define VSPEC      DIRSPEC VNAME
#define PT_SIZE    8192

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

/* INDEPENDENT reader #1: sys$parse + sys$search of `pattern` over the ACP.
 * Returns match count; fills the first match's ODS-2 File-ID number into *fid
 * and its resultant tail into tail[]. Models dcl_filespec.c's dir search. */
static int search_one(const char *pattern, uint16_t *fid, char *tail, size_t tsz,
                      uint32_t *end_status)
{
    struct FAB fab;
    struct NAM nam;
    char esa[512], rsa[512];
    int n = 0;

    if (fid) *fid = 0;
    if (tail && tsz) tail[0] = '\0';
    if (end_status) *end_status = (uint32_t)RMS$_DNF;

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
        if (n == 0) {
            size_t rl = nam.nam$b_rsl;
            if (tail && tsz) {
                size_t c = rl > tsz - 1 ? tsz - 1 : rl;
                memcpy(tail, nam.nam$l_rsa, c);
                tail[c] = '\0';
            }
            uint16_t num = 0, seq = 0; uint8_t rvn = 0, nmx = 0;
            rms_search_fid(&nam, &num, &seq, &rvn, &nmx);
            if (fid) *fid = num;
        }
        n++;
    }
    if (end_status) *end_status = fab.fab$l_sts;
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
    printf("=== test_syssvc_crtl_rms_veneer: the C RTL stdio->RMS veneer "
           "(fopen/fwrite/fread/fclose -> sys$create/$put/$get) lands a file on "
           "the real ODS-2 volume, proven by an INDEPENDENT ACP reader (vms-47e) ===\n");

    if (!executive_present()) {
        printf("=== test_syssvc_crtl_rms_veneer: 0 passed, 0 failed (SKIPPED: no "
               "/dev/vms -- the CRTL veneer is an executive-file consumer, nothing "
               "to assert without a real ACP; Rule 9) ===\n");
        return EXIT_SKIP;
    }

    uint32_t st = vms_kif_acp_mount(ODS2_UNIT);   /* idempotent */
    check($VMS_STATUS_SUCCESS(st), "VDA0: mounted executive-global for the veneer proof");

    /* Clean any stale VENEER.DAT from a prior run so the version check is exact. */
    erase_spec(VSPEC ";*");

    /* Deterministic payload. */
    unsigned char *buf = malloc(PT_SIZE);
    check(buf != NULL, "heap: malloc(PT_SIZE)");
    if (!buf) goto done;
    for (int i = 0; i < PT_SIZE; i++)
        buf[i] = (unsigned char)((i * 7 + 3) & 0xFF);

    /* ================================================================= *
     * 1. WRITE through the veneer (fopen "w" -> fwrite -> fclose).       *
     * ================================================================= */
    OVMX_CRTL_FILE *wf = ovmx_crtl_fopen(VSPEC, "w");
    check(wf != NULL, "1a: ovmx_crtl_fopen(VENEER.DAT,\"w\") -> sys$create over the ACP");
    if (wf) {
        size_t nw = ovmx_crtl_fwrite(buf, 1, PT_SIZE, wf);
        check(nw == (size_t)PT_SIZE, "1b: ovmx_crtl_fwrite 8192 bytes -> sys$put (full count)");
        check(ovmx_crtl_fclose(wf) == 0, "1c: ovmx_crtl_fclose -> sys$close NORMAL");
    }

    /* ================================================================= *
     * 2. INDEPENDENT PROOF #1 — the ACP directory search sees it with a  *
     *    genuine ODS-2 File ID (a channel the veneer never touched). A   *
     *    ramfs POSIX write cannot appear here. THIS is the teeth.        *
     * ================================================================= */
    uint16_t fid = 0; char tail[128]; uint32_t endst = 0;
    int nfound = search_one(VSPEC ";*", &fid, tail, sizeof(tail), &endst);
    check(nfound == 1,
          "2a: sys$search VENEER.DAT;* finds exactly the veneer-written file "
          "(independent ACP reader -- ramfs cannot produce this)");
    check(fid != 0,
          "2b: the match carries a genuine nonzero ODS-2 File ID from the "
          "executive directory search (rms_search_fid -- not synthesized)");
    check(strstr(tail, ";1") != NULL,
          "2c: the veneer create minted version ;1 (a POSIX overwrite has no "
          "version -- ramfs cannot fake this)");
    check(endst == (uint32_t)RMS$_NMF,
          "2d: the wildcard search terminates RMS$_NMF (exhausted a real directory)");
    printf("  [independent ACP reader] resultant='%s' fid=(%u,...)\n", tail, fid);

    /* ================================================================= *
     * 3. INDEPENDENT PROOF #2 — the on-disk ODS-2 header (rms_file_attr, *
     *    IO$_ACCESS) confirms the file + its File ID.                    *
     * ================================================================= */
    {
        struct rms_fileattr attr;
        memset(&attr, 0, sizeof attr);
        uint32_t ast = rms_file_attr(VSPEC, &attr);
        check($VMS_STATUS_SUCCESS(ast),
              "3a: rms_file_attr reads the veneer file's on-disk ODS-2 header "
              "(IO$_ACCESS) -- present on the real volume");
        check(attr.fid_num != 0 && attr.fid_num == fid,
              "3b: the on-disk header File ID matches the directory-search File ID "
              "(same genuine ODS-2 file, two independent ACP readers agree)");
        check(attr.version == 1,
              "3c: the on-disk header records version ;1 (a real ODS-2 create)");
    }

    /* ================================================================= *
     * 4. Read the payload back through the veneer + verify byte-exact.   *
     * ================================================================= */
    OVMX_CRTL_FILE *rf = ovmx_crtl_fopen(VSPEC, "r");
    check(rf != NULL, "4a: ovmx_crtl_fopen(VENEER.DAT,\"r\") -> sys$open over the ACP");
    if (rf) {
        unsigned char *rbuf = calloc(1, PT_SIZE);
        size_t nr = rbuf ? ovmx_crtl_fread(rbuf, 1, PT_SIZE, rf) : 0;
        check(nr == (size_t)PT_SIZE, "4b: ovmx_crtl_fread reads all 8192 bytes back (sys$get)");
        check(rbuf && memcmp(buf, rbuf, PT_SIZE) == 0,
              "4c: the RMS round-trip is byte-exact (FIX mrs=0 put / mrs=1 get)");
        ovmx_crtl_fclose(rf);
        free(rbuf);
    }

    /* ================================================================= *
     * 5. Isolation — erase the file so the fixture is restored.         *
     * ================================================================= */
    st = erase_spec(VSPEC ";*");
    check($VMS_STATUS_SUCCESS(st), "5a: sys$erase VENEER.DAT (isolation)");
    check(search_one(VSPEC ";*", NULL, NULL, 0, &endst) == 0,
          "5b: a final search finds NONE (fixture restored)");

    free(buf);

done:
    printf("=== test_syssvc_crtl_rms_veneer: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
