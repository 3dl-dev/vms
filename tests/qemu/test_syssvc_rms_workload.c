/*
 * test_syssvc_rms_workload.c - the RMS file-op WORKLOAD a real compiler driver
 * (cpp -> cc1 -> as -> ld) performs, BEYOND the single fopen/fwrite/fread/fclose
 * round-trip, proven through the public RMS system services against a real
 * /dev/vms over the Files-11 ODS-2 ACP (vms-1b5, GCC self-host ladder, epic
 * vms-208 / gap-register docs/design-gcc-port-surface-gaps-register.md sec 3.2).
 *
 * WHY THIS SUITE EXISTS.
 *
 * The CRTL->RMS stdio route-through (fopen/fwrite/fread/fclose) is exercised by
 * test_syssvc_rms_acp.c as a single-version create/put/get/close/erase lifecycle.
 * A real compiler does MORE than stream one file: it rewrites outputs (creating
 * NEW VERSIONS, the VMS way), and it ENUMERATES directories to find its sources,
 * headers and libraries (opendir/readdir class). The gap register (sec 3.2) names
 * exactly these as the remaining "workload coverage" holes for vms-1b5:
 *   - "Directory enumeration (opendir/readdir/stat-class) over RMS/ACP" -- a
 *     build driver finds headers/libraries this way.
 *   - "CRTL-driven version bump (;N -> ;N+1 on a second create)" -- proven at the
 *     RMS-API level, but never REACHED A SECOND TIME on the same name through the
 *     create path (test_syssvc_rms_acp uses a UNIQUE name per RFM; the crtl_rms
 *     port test only ever created PORTTEST.DAT once).
 *
 * This suite drives the SAME public RMS services the compiler's CRTL binds to
 * (sys$create / sys$open / sys$connect / sys$get / sys$erase / sys$parse /
 * sys$search + rms_file_attr / rms_search_fid, src/vmsrms/) through a driver-
 * shaped pipeline, over the real-VAX ODS-2 fixture the harness mounts WRITABLE on
 * VDA0:. Every op is a genuine $QIO to the executive ACP -- no POSIX bypass.
 *
 * WHAT THIS PROVES (each over the real ACP, /dev/vms):
 *
 *   A. VMS FILE VERSIONING THROUGH sys$create, REACHED TWICE. Creating
 *      [OVMXDIR]WKOBJ.OBJ, then creating it AGAIN (versionless), yields ;2 -- a
 *      SECOND file, NOT a POSIX-style supersede of ;1. Both versions coexist and
 *      resolve independently: the versionless (highest) open reads the ;2 payload,
 *      an explicit ;1 open still reads the ;1 payload. This is the exact semantic
 *      a POSIX open("w") CANNOT fake (it would overwrite) -- so it is the teeth
 *      that distinguish a genuine RMS/ODS-2 create from a rootfs passthrough.
 *   B. DIRECTORY ENUMERATION THROUGH sys$parse + sys$search. A wildcard search of
 *      [OVMXDIR]WKSRC.*;* enumerates exactly the three source-shaped files just
 *      created, each carrying the GENUINE ODS-2 File ID the executive directory
 *      search returned (rms_search_fid), and terminates RMS$_NMF. A search of
 *      WKOBJ.OBJ;* enumerates BOTH versions (ties enumeration to versioning). A
 *      search that matches nothing terminates RMS$_NMF with ZERO fabricated hits.
 *   C. STAT-CLASS ATTRIBUTE READ (rms_file_attr) reads a file's genuine on-disk
 *      ODS-2 header (File ID + version + is-directory) through IO$_ACCESS -- the
 *      F$FILE_ATTRIBUTES / existence-probe the driver uses -- and fails honest
 *      (RMS$_FNF, output zeroed) on a name that does not exist.
 *   D. TEMP-FILE CLEANUP THROUGH sys$erase. Every WK* file is erased; a final
 *      enumeration finds NONE, so the fixture directory is restored (only
 *      BITMAP/INDEXF bits cycle, harmlessly), matching the isolation discipline
 *      of test_syssvc_rms_acp.c / test_syssvc_acp_create.c.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass (Rule 9 / INV-6): RMS is an
 * executive-file consumer, so with no ACP there is nothing to assert.
 *
 * SCOPE NOTE (no silent scope drop). This proves the RMS ENGINE the compiler's
 * file ops route THROUGH is ready for the driver workload. Re-pointing the alpha
 * port's DECC$SHR file layer (fopen/open/opendir/readdir/remove) from musl-POSIX
 * onto these sys$ services -- so the PORT IMAGE's own file ops reach the ACP
 * rather than ramfs -- is the separate CRTL->RMS binding rung (R2/vms-dfb),
 * tracked as a child of vms-1b5.
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
#define DIRSPEC    ODS2_UNIT "[OVMXDIR]"

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

/* sys$create [OVMXDIR]<name> (versionless => highest+1), $CONNECT, $PUT one
 * variable record `rec`, $CLOSE. Returns the create status. */
static uint32_t create_rec(const char *name, const char *rec)
{
    struct FAB fab;
    struct RAB rab;
    char spec[128];
    uint32_t st;

    snprintf(spec, sizeof(spec), DIRSPEC "%s", name);

    fab = cc$rms_fab;
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_VAR;
    fab.fab$w_mrs = 64;
    fab.fab$b_fac = FAB$M_PUT;
    st = sys$create(&fab, 0, 0);
    if (st != RMS$_NORMAL)
        return st;

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    if (sys$connect(&rab, 0, 0) == RMS$_NORMAL) {
        rab.rab$l_rbf = (char *)rec;
        rab.rab$w_rsz = (uint16_t)strlen(rec);
        st = sys$put(&rab, 0, 0);
    }
    sys$close(&fab, 0, 0);
    return st;
}

/* sys$open <spec>, $CONNECT, $GET the first record into out (NUL-terminated),
 * $CLOSE. Returns the open status; *out empty on any miss. */
static uint32_t open_first_rec(const char *spec, char *out, size_t outsz)
{
    struct FAB fab;
    struct RAB rab;
    char ubuf[128];
    uint32_t st;

    if (out && outsz) out[0] = '\0';

    fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_VAR;
    fab.fab$w_mrs = 64;
    fab.fab$b_fac = FAB$M_GET;
    st = sys$open(&fab, 0, 0);
    if (st != RMS$_NORMAL)
        return st;

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    rab.rab$l_ubf = ubuf;
    rab.rab$w_usz = (uint16_t)sizeof(ubuf);
    if (sys$connect(&rab, 0, 0) == RMS$_NORMAL &&
        sys$get(&rab, 0, 0) == RMS$_NORMAL) {
        size_t n = rab.rab$w_rsz;
        if (out && outsz) {
            if (n > outsz - 1) n = outsz - 1;
            memcpy(out, rab.rab$l_ubf, n);
            out[n] = '\0';
        }
    }
    sys$close(&fab, 0, 0);
    return st;
}

/* sys$erase <spec>. Returns the erase status. */
static uint32_t erase_spec(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    return sys$erase(&fab, 0, 0);
}

/* sys$parse + sys$search wildcard enumeration of `pattern`. Collects up to
 * `cap` resultant NAME.TYP;VER tails into names[][NLEN] with their genuine ODS-2
 * File-ID number in fidnum[]. Returns the match count; *end_status gets the
 * terminating $SEARCH status (RMS$_NMF exhausted / RMS$_DNF no-dir). Models
 * src/vmsdcl/dcl_filespec.c dcl_rms_dir_{open,next,close} exactly. */
#define NLEN 64
static int enumerate(const char *pattern, char names[][NLEN], uint16_t *fidnum,
                     int cap, uint32_t *end_status)
{
    struct FAB fab;
    struct NAM nam;
    char esa[512], rsa[512];
    int n = 0;

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

    while (sys$search(&fab, 0, 0) == RMS$_NORMAL && n < cap) {
        /* Resultant is "DEV:[DIR]NAME.TYP;VER" -- take the NAME.TYP;VER tail. */
        char match[512];
        size_t rl = nam.nam$b_rsl;
        if (rl > sizeof(match) - 1) rl = sizeof(match) - 1;
        memcpy(match, nam.nam$l_rsa, rl);
        match[rl] = '\0';

        const char *nt = match;
        const char *rb = strrchr(match, ']');
        if (!rb) rb = strrchr(match, '>');
        if (rb) nt = rb + 1;

        strncpy(names[n], nt, NLEN - 1);
        names[n][NLEN - 1] = '\0';

        uint16_t num = 0, seq = 0; uint8_t rvn = 0, nmx = 0;
        rms_search_fid(&nam, &num, &seq, &rvn, &nmx);
        fidnum[n] = num;
        n++;
    }

    if (end_status) *end_status = fab.fab$l_sts;
    rms_search_end(&nam);
    return n;
}

/* Is there an enumerated name whose tail contains `needle` (case-sensitive; the
 * ACP resultant is upper-cased already)? */
static int names_have(char names[][NLEN], int n, const char *needle)
{
    for (int i = 0; i < n; i++)
        if (strstr(names[i], needle))
            return 1;
    return 0;
}

static int all_fids_nonzero(const uint16_t *fidnum, int n)
{
    for (int i = 0; i < n; i++)
        if (fidnum[i] == 0)
            return 0;
    return n > 0;
}

int main(void)
{
    uint32_t st, endst;
    char rec[128];
    char names[16][NLEN];
    uint16_t fids[16];
    int nm;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_rms_workload: the compiler driver's RMS file-op "
           "workload (versioning + directory enumeration + stat + erase) over the "
           "real Files-11 ODS-2 ACP (vms-1b5) ===\n");

    if (!executive_present()) {
        printf("=== test_syssvc_rms_workload: 0 passed, 0 failed (SKIPPED: no "
               "/dev/vms -- RMS is an executive-file consumer, nothing to assert "
               "without a real ACP; Rule 9) ===\n");
        return EXIT_SKIP;
    }

    st = vms_kif_acp_mount(ODS2_UNIT);   /* idempotent */
    check($VMS_STATUS_SUCCESS(st), "VDA0: mounted executive-global for the RMS workload");

    /* ==================================================================== *
     * A. VMS FILE VERSIONING THROUGH sys$create, REACHED A SECOND TIME.     *
     * ==================================================================== */
    st = create_rec("WKOBJ.OBJ", "V1");
    check(st == RMS$_NORMAL, "A1: sys$create WKOBJ.OBJ (;1) + $PUT 'V1' -> NORMAL");

    st = create_rec("WKOBJ.OBJ", "V2");
    check(st == RMS$_NORMAL,
          "A2: sys$create WKOBJ.OBJ AGAIN (versionless) succeeds -- RMS allocates a "
          "NEW version ;2, it does not POSIX-supersede ;1");

    st = open_first_rec(DIRSPEC "WKOBJ.OBJ", rec, sizeof(rec));
    check(st == RMS$_NORMAL && strcmp(rec, "V2") == 0,
          "A3: sys$open WKOBJ.OBJ (versionless => highest) reads the ;2 payload 'V2' "
          "(newest version resolves)");

    st = open_first_rec(DIRSPEC "WKOBJ.OBJ;1", rec, sizeof(rec));
    check(st == RMS$_NORMAL && strcmp(rec, "V1") == 0,
          "A4: sys$open WKOBJ.OBJ;1 still reads the ;1 payload 'V1' -- both versions "
          "COEXIST (the VMS-versioning teeth a POSIX overwrite cannot fake)");

    st = open_first_rec(DIRSPEC "WKOBJ.OBJ;2", rec, sizeof(rec));
    check(st == RMS$_NORMAL && strcmp(rec, "V2") == 0,
          "A5: sys$open WKOBJ.OBJ;2 reads 'V2' (explicit newest version)");

    st = open_first_rec(DIRSPEC "WKOBJ.OBJ;3", rec, sizeof(rec));
    check(st == RMS$_FNF,
          "A6: sys$open WKOBJ.OBJ;3 (no such version) -> RMS$_FNF (fail-honest)");

    /* ==================================================================== *
     * B. DIRECTORY ENUMERATION THROUGH sys$parse + sys$search.             *
     * ==================================================================== */
    st = create_rec("WKSRC.C", "csrc");
    check(st == RMS$_NORMAL, "B0a: sys$create WKSRC.C (a source-shaped file)");
    st = create_rec("WKSRC.I", "isrc");
    check(st == RMS$_NORMAL, "B0b: sys$create WKSRC.I (cpp output)");
    st = create_rec("WKSRC.S", "ssrc");
    check(st == RMS$_NORMAL, "B0c: sys$create WKSRC.S (cc1 output)");

    nm = enumerate(DIRSPEC "WKSRC.*;*", names, fids, 16, &endst);
    check(nm == 3,
          "B1: sys$search WKSRC.*;* enumerates exactly the 3 source-shaped files");
    check(names_have(names, nm, "WKSRC.C") &&
          names_have(names, nm, "WKSRC.I") &&
          names_have(names, nm, "WKSRC.S"),
          "B2: the enumeration returns WKSRC.C, WKSRC.I and WKSRC.S by name");
    check(all_fids_nonzero(fids, nm),
          "B3: each match carries a genuine nonzero ODS-2 File ID (rms_search_fid, "
          "from the executive directory search -- not synthesized)");
    check(endst == (uint32_t)RMS$_NMF,
          "B4: the wildcard search terminates RMS$_NMF (no-more-files, exhausted an "
          "existing directory)");

    nm = enumerate(DIRSPEC "WKOBJ.OBJ;*", names, fids, 16, &endst);
    check(nm == 2 && endst == (uint32_t)RMS$_NMF,
          "B5: sys$search WKOBJ.OBJ;* enumerates BOTH versions (;2 and ;1) then NMF "
          "-- enumeration and versioning agree");

    nm = enumerate(DIRSPEC "WKNONE.XYZ;*", names, fids, 16, &endst);
    check(nm == 0 && endst == (uint32_t)RMS$_NMF,
          "B6: sys$search of a name that matches nothing returns ZERO hits and "
          "terminates RMS$_NMF (fail-honest, no fabricated match)");

    /* ==================================================================== *
     * C. STAT-CLASS ATTRIBUTE READ (rms_file_attr).                        *
     * ==================================================================== */
    {
        struct rms_fileattr at;
        memset(&at, 0, sizeof(at));
        st = rms_file_attr(DIRSPEC "WKSRC.C", &at);
        check(st == RMS$_NORMAL && at.fid_num != 0 && at.version == 1 &&
              at.is_directory == 0,
              "C1: rms_file_attr WKSRC.C reads the genuine on-disk header (real File "
              "ID, version 1, not a directory) via IO$_ACCESS");

        memset(&at, 0, sizeof(at));
        st = rms_file_attr(DIRSPEC "WKGHOST.NON", &at);
        check(st == RMS$_FNF && at.fid_num == 0,
              "C2: rms_file_attr of a nonexistent name -> RMS$_FNF, output zeroed "
              "(fail-honest existence probe)");
    }

    /* ==================================================================== *
     * D. TEMP-FILE CLEANUP THROUGH sys$erase (+ restore the fixture).      *
     * ==================================================================== */
    st = erase_spec(DIRSPEC "WKOBJ.OBJ;2");
    check(st == RMS$_NORMAL, "D1: sys$erase WKOBJ.OBJ;2 (delete the newest version)");
    st = open_first_rec(DIRSPEC "WKOBJ.OBJ;1", rec, sizeof(rec));
    check(st == RMS$_NORMAL && strcmp(rec, "V1") == 0,
          "D2: after erasing ;2, WKOBJ.OBJ;1 still resolves 'V1' (older version "
          "survives -- delete is per-version, VMS semantics)");
    st = erase_spec(DIRSPEC "WKOBJ.OBJ;1");
    check(st == RMS$_NORMAL, "D3: sys$erase WKOBJ.OBJ;1 (delete the last version)");
    st = open_first_rec(DIRSPEC "WKOBJ.OBJ", rec, sizeof(rec));
    check(st == RMS$_FNF,
          "D4: after erasing every version, sys$open WKOBJ.OBJ -> RMS$_FNF");

    check(erase_spec(DIRSPEC "WKSRC.C;1") == RMS$_NORMAL, "D5a: sys$erase WKSRC.C");
    check(erase_spec(DIRSPEC "WKSRC.I;1") == RMS$_NORMAL, "D5b: sys$erase WKSRC.I");
    check(erase_spec(DIRSPEC "WKSRC.S;1") == RMS$_NORMAL, "D5c: sys$erase WKSRC.S");

    nm = enumerate(DIRSPEC "WK*.*;*", names, fids, 16, &endst);
    check(nm == 0,
          "D6: a final enumeration finds NO WK* files -- the fixture directory is "
          "restored (isolation discipline; only BITMAP/INDEXF bits cycled)");

    vms_kif_acp_dmount(ODS2_UNIT);

    printf("=== test_syssvc_rms_workload: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
