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
#include <fcntl.h>          /* vms-3320: O_* for ovmx_crtl_open */

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
     * 5. FILE-OP VENEER (vms-3320): decc$creat/open/unlink/remove/rename/ *
     *    opendir/readdir/closedir over RMS, each proven by the SAME       *
     *    INDEPENDENT ACP reader a ramfs cannot fake.                      *
     * ================================================================= */
#define CREATNAME  "CVENEER.DAT"
#define CREATSPEC  DIRSPEC CREATNAME
#define RENSRC     DIRSPEC "RENSRC.DAT"
#define RENDST     DIRSPEC "RENDST.DAT"
#define DIRA       DIRSPEC "ENUMA.DAT"
#define DIRB       DIRSPEC "ENUMB.DAT"

    /* --- 5.1 creat mints a real ODS-2 file the independent reader sees. --- */
    erase_spec(CREATSPEC ";*");
    int cfd = ovmx_crtl_creat(CREATSPEC, 0);
    check(cfd >= OVMX_CRTL_FD_BASE,
          "5.1a: ovmx_crtl_creat -> sys$create over the ACP, returns a veneer fd");
    if (cfd >= 0) ovmx_crtl_fdclose(cfd);
    {
        uint16_t cfid = 0; char ctail[128]; uint32_t cend = 0;
        int cn = search_one(CREATSPEC ";*", &cfid, ctail, sizeof(ctail), &cend);
        check(cn == 1,
              "5.1b: independent sys$search finds the creat-minted file "
              "(ramfs cannot appear on the ACP directory)");
        check(cfid != 0,
              "5.1c: the creat file carries a genuine nonzero ODS-2 File ID");
        check(strstr(ctail, ";1") != NULL,
              "5.1d: creat minted version ;1 (a genuine ODS-2 create)");
        printf("  [independent ACP reader] creat resultant='%s' fid=(%u,...)\n",
               ctail, cfid);
    }

    /* --- 5.2 unlink removes it; the independent reader sees it GONE. --- */
    check(ovmx_crtl_unlink(CREATSPEC ";*") == 0,
          "5.2a: ovmx_crtl_unlink -> sys$erase NORMAL");
    check(search_one(CREATSPEC ";*", NULL, NULL, 0, &endst) == 0,
          "5.2b: independent sys$search finds the unlinked file GONE "
          "(a real ODS-2 directory-entry removal)");

    /* --- 5.3 open(O_CREAT) mints; remove() (ISO C) deletes; reader agrees. --- */
    {
        erase_spec(CREATSPEC ";*");
        int ofd = ovmx_crtl_open(CREATSPEC, O_CREAT | O_WRONLY | O_TRUNC);
        check(ofd >= OVMX_CRTL_FD_BASE,
              "5.3a: ovmx_crtl_open(O_CREAT) -> sys$create, returns a veneer fd");
        if (ofd >= 0) ovmx_crtl_fdclose(ofd);
        uint16_t ofid = 0;
        check(search_one(CREATSPEC ";*", &ofid, NULL, 0, &endst) == 1 && ofid != 0,
              "5.3b: independent reader sees the open(O_CREAT) file with a File ID");
        check(ovmx_crtl_remove(CREATSPEC ";*") == 0,
              "5.3c: ovmx_crtl_remove -> sys$erase NORMAL");
        check(search_one(CREATSPEC ";*", NULL, NULL, 0, &endst) == 0,
              "5.3d: independent reader sees the removed file GONE");
    }

    /* --- 5.4 rename: the ATOMIC re-link keeps the SAME File ID (teeth). --- */
    {
        erase_spec(RENSRC ";*");
        erase_spec(RENDST ";*");
        /* Create the source through the proven stdio veneer, capture its FID. */
        OVMX_CRTL_FILE *sf = ovmx_crtl_fopen(RENSRC, "w");
        check(sf != NULL, "5.4a: create RENSRC.DAT (fopen->sys$create)");
        if (sf) { ovmx_crtl_fwrite("RENAMEME", 1, 8, sf); ovmx_crtl_fclose(sf); }
        uint16_t src_fid = 0;
        check(search_one(RENSRC ";*", &src_fid, NULL, 0, &endst) == 1 && src_fid != 0,
              "5.4b: independent reader sees RENSRC.DAT with File ID X");

        check(ovmx_crtl_rename(RENSRC, RENDST) == 0,
              "5.4c: ovmx_crtl_rename -> sys$rename (ACP MODIFY!M_MOVE) NORMAL");

        check(search_one(RENSRC ";*", NULL, NULL, 0, &endst) == 0,
              "5.4d: independent reader sees the OLD name RENSRC.DAT GONE");
        uint16_t dst_fid = 0; char dtail[128];
        int dn = search_one(RENDST ";*", &dst_fid, dtail, sizeof(dtail), &endst);
        check(dn == 1,
              "5.4e: independent reader sees the NEW name RENDST.DAT present");
        check(dst_fid != 0 && dst_fid == src_fid,
              "5.4f: RENDST.DAT carries the SAME File ID as RENSRC had -- proves "
              "an ATOMIC directory-entry re-link, NOT erase+create (a new FID)");
        printf("  [independent ACP reader] rename: RENSRC fid=(%u,...) -> "
               "RENDST '%s' fid=(%u,...) SAME=%s\n",
               src_fid, dtail, dst_fid, (src_fid == dst_fid) ? "YES" : "NO");
        /* On-disk header confirms the moved file keeps its FID + allocation. */
        struct rms_fileattr rattr; memset(&rattr, 0, sizeof rattr);
        uint32_t rst = rms_file_attr(RENDST, &rattr);
        check($VMS_STATUS_SUCCESS(rst) && rattr.fid_num == src_fid,
              "5.4g: RENDST.DAT on-disk header File ID == the source's (two "
              "independent readers agree the file kept its FID)");
        erase_spec(RENDST ";*");
    }

    /* --- 5.5 opendir/readdir enumerate the REAL ODS-2 directory entries. --- */
    {
        erase_spec(DIRA ";*");
        erase_spec(DIRB ";*");
        OVMX_CRTL_FILE *fa = ovmx_crtl_fopen(DIRA, "w");
        if (fa) ovmx_crtl_fclose(fa);
        OVMX_CRTL_FILE *fb = ovmx_crtl_fopen(DIRB, "w");
        if (fb) ovmx_crtl_fclose(fb);
        check(fa != NULL && fb != NULL, "5.5a: create ENUMA.DAT + ENUMB.DAT");

        OVMX_CRTL_DIR *dp = ovmx_crtl_opendir(DIRSPEC);
        check(dp != NULL, "5.5b: ovmx_crtl_opendir(dir) -> sys$parse over the ACP");
        int saw_a = 0, saw_b = 0, fid_a = 0, fid_b = 0, total = 0;
        if (dp) {
            struct ovmx_crtl_dirent *e;
            while ((e = ovmx_crtl_readdir(dp)) != NULL) {
                total++;
                if (strstr(e->d_name, "ENUMA.DAT")) { saw_a = 1; fid_a = e->d_fileid; }
                if (strstr(e->d_name, "ENUMB.DAT")) { saw_b = 1; fid_b = e->d_fileid; }
            }
            check(ovmx_crtl_closedir(dp) == 0,
                  "5.5c: ovmx_crtl_closedir -> rms_search_end (context released)");
        }
        check(saw_a && saw_b,
              "5.5d: readdir enumerated BOTH real ODS-2 entries by name");
        check(fid_a != 0 && fid_b != 0 && fid_a != fid_b,
              "5.5e: each enumerated entry carries its genuine (distinct) File ID");
        /* Cross-check against the independent single-file searches. */
        uint16_t ia = 0, ib = 0;
        search_one(DIRA ";*", &ia, NULL, 0, &endst);
        search_one(DIRB ";*", &ib, NULL, 0, &endst);
        check((uint16_t)fid_a == ia && (uint16_t)fid_b == ib,
              "5.5f: readdir's File IDs match the independent sys$search File IDs "
              "(same genuine ODS-2 directory, two readers agree)");
        printf("  [independent ACP reader] readdir enumerated %d entries; "
               "ENUMA fid=%d (search %u), ENUMB fid=%d (search %u)\n",
               total, fid_a, ia, fid_b, ib);
        erase_spec(DIRA ";*");
        erase_spec(DIRB ";*");
    }

    /* ================================================================= *
     * 6. Isolation — erase the stdio-veneer file so the fixture is       *
     *    restored.                                                       *
     * ================================================================= */
    st = erase_spec(VSPEC ";*");
    check($VMS_STATUS_SUCCESS(st), "6a: sys$erase VENEER.DAT (isolation)");
    check(search_one(VSPEC ";*", NULL, NULL, 0, &endst) == 0,
          "6b: a final search finds NONE (fixture restored)");

    free(buf);

done:
    printf("=== test_syssvc_crtl_rms_veneer: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
