/*
 * test_syssvc_acp_ods5.c - the executive Files-11 ACP mounts, reads and writes a
 * REAL ODS-5 volume (rd vms-7e7e, epic vms-810), against a real /dev/vms.
 *
 * The volume is tests/lab/captures/ods5-alpha-20261022/ods5t.dsk: made on a
 * real OpenVMS Alpha V8.4 system by INITIALIZE/STRUCTURE=5, populated there with
 * extended names, and copied out byte for byte (RESULTS.md in that directory).
 * The harness attaches it as the sixth disk (vdf, VDA500: / DKA500: on Alpha).
 * Every expected FID and name below is what the real system put on it.
 *
 * WHAT THIS SUITE PROVES
 *   1. $MOUNT accepts the level-5 volume (it refused it before rd vms-7e7e:
 *      acp_validate_ods2 took level 2 only).
 *   2. Lookups are the real system's: c++config.h, a.b.c.d, the 73-byte name,
 *      MixedCase.Txt are found by name; differently-cased spellings
 *      (C++CONFIG.H, mixedcase.txt, CAF<E-acute upper>.TXT) find the same FIDs.
 *   3. The file the real system wrote reads back through IO$_READVBLK
 *      ("#define CXXCONFIG 1").
 *   4. $SEARCH lists the directory in the real system's order with case
 *      preserved, and takes the type after the LAST dot ("*.d" -> a.b.c.d).
 *   5. IO$_CREATE on the ODS-5 volume: a mixed-case name keeps its case, a
 *      different-case CREATE of an existing name makes the next VERSION of
 *      that name (the real system's E.txt;2), the header on the platter is a
 *      level-5 header with the FI5 name, the directory record carries the
 *      ISL-1 name type -- read back off the raw device, not through the ACP.
 *   6. IO$_MODIFY rename to a long ODS-5 name, IO$_DELETE by a different case.
 *
 * NEGATIVE CONTROLS
 *   - an ODS-2 volume (VDA0:) still matches byte-exact names only in its own
 *     upper case world: its search lists the same names as before;
 *   - a name absent from the ODS-5 directory is SS$_NOSUCHFILE.
 *
 * NOT HERE (tracked): UCS-2 names (^U4E2D.txt) cannot cross the ACP's 8-bit
 * name ABI yet (rd vms-f1a2); names over 79 bytes need the wider ABI (rd
 * vms-7e7e, next piece).
 *
 * NO /dev/vms -> SKIP (77): the ACP is executive-resident.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "vmsfs/ods2.h"

#define EXIT_SKIP 77
#define TEST_DIR_FID  11u   /* [TEST] on the real ODS-5 volume */
#define LONGNAME "this_is_a_long_extended_file_name_well_past_the_ods2_limit_of_39.header"

static int pass = 0, fail = 0;

static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
}

static uint32_t fidof(uint16_t num, uint8_t nmx) { return (uint32_t)num | ((uint32_t)nmx << 16); }

static uint32_t access_in(uint32_t chan, uint16_t did, const char *name, uint16_t ver,
                          int wr, struct vms_acp_access_args *a)
{
    memset(a, 0, sizeof(*a));
    a->chan = chan;
    a->did_num = did;
    a->did_seq = 1;
    a->version = ver;
    if (wr)
        a->acctl = VMS_ACP_ACCTL_WRITE;
    strncpy(a->name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_access(a);
}

static uint32_t find_fid(uint32_t chan, const char *name, uint16_t *ver)
{
    struct vms_acp_access_args a;
    uint32_t st = access_in(chan, TEST_DIR_FID, name, 0, 0, &a);
    (void)vms_kif_acp_deaccess(chan);
    if (!$VMS_STATUS_SUCCESS(st))
        return 0;
    if (ver)
        *ver = a.out_version;
    return fidof(a.fid_num, a.fid_nmx);
}

static uint32_t fileop(uint32_t chan, uint32_t func, uint32_t mods, const char *name,
                       uint16_t ver, struct vms_acp_fileop_args *f)
{
    memset(f, 0, sizeof(*f));
    f->chan = chan;
    f->func = func;
    f->modifiers = mods;
    f->kind = ODS2_FK_DATA_STMLF;
    f->did_num = TEST_DIR_FID;
    f->did_seq = 1;
    f->version = ver;
    strncpy(f->name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_fileop(f);
}

/* One full $SEARCH pass: the "NAME;VER" strings, NUL-separated, in order. */
static unsigned search_all(uint32_t chan, uint16_t did, const char *pattern,
                           char out[][VMS_ACP_RESNAM_SIZE], unsigned max)
{
    struct vms_acp_acpcontrol_args a;
    unsigned n = 0;
    int first = 1;

    for (;;) {
        memset(&a, 0, sizeof(a));
        a.chan = chan;
        a.func = VMS_ACP_CTL_SEARCH;
        a.did_num = did;
        a.did_seq = 1;
        a.wcc_reset = first ? 1 : 0;
        strncpy(a.pattern, pattern, VMS_ACP_NAME_SIZE - 1);
        first = 0;
        if (!$VMS_STATUS_SUCCESS(vms_kif_acp_acpcontrol(&a)) || n >= max)
            break;
        memcpy(out[n], a.resnam, VMS_ACP_RESNAM_SIZE);
        out[n][VMS_ACP_RESNAM_SIZE - 1] = '\0';
        n++;
    }
    return n;
}

/* ---- raw-device reads (independent of the ACP): O_DIRECT, so no stale page
 * cache can stand in for what the executive wrote ---- */

static int g_rawfd = -1;
static uint8_t *g_buf;

static int raw_read(uint32_t lbn, uint8_t *out)
{
    if (pread(g_rawfd, g_buf, 4096, (off_t)(lbn & ~7u) * 512) < (ssize_t)(((lbn & 7u) + 1u) * 512))
        return -1;
    memcpy(out, g_buf + (lbn & 7u) * 512, 512);
    return 0;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* LBN of file `fid`'s header: its INDEXF.SYS VBN through the index file's map
 * (fmt-1 pointers only, which is all the real volume's INDEXF.SYS uses). */
static uint32_t raw_hdr_lbn(uint32_t fid)
{
    uint8_t home[512], ixf[512];
    uint32_t vbn, seen = 0;
    unsigned p, end;

    if (raw_read(1, home) || raw_read(rd16(home + 24) + rd16(home + 32), ixf))
        return 0;
    vbn = rd16(home + 22) + rd16(home + 32) + fid - 1u;
    p = ixf[1] * 2u;
    end = p + ixf[58] * 2u;
    while (p + 4 <= end) {
        uint16_t w0 = rd16(ixf + p);
        uint32_t cnt, lbn;
        if ((w0 >> 14) != 1)
            return 0;
        cnt = (w0 & 0xFFu) + 1u;
        lbn = ((uint32_t)((w0 >> 8) & 0x3Fu) << 16) | rd16(ixf + p + 2);
        if (vbn <= seen + cnt)
            return lbn + (vbn - seen - 1u);
        seen += cnt;
        p += 4;
    }
    return 0;
}

/* The flags byte of the record named `name` in [TEST]'s first data block
 * (LBN 31 on the real volume), or -1. */
static int raw_dir_flags(const char *name)
{
    uint8_t blk[512];
    unsigned off = 0, nl = (unsigned)strlen(name);

    if (raw_read(31, blk))
        return -1;
    while (off + 6 <= 512 && rd16(blk + off) != 0xFFFF) {
        if (blk[off + 5] == nl && memcmp(blk + off + 6, name, nl) == 0)
            return blk[off + 4];
        off += 2u + rd16(blk + off);
    }
    return -1;
}

int main(void)
{
    static const char *const units[] = { "VDA500:", "DKA500:" };
    struct vms_acp_access_args a;
    struct vms_acp_fileop_args f;
    struct vms_acp_rw_args r;
    static char res[32][VMS_ACP_RESNAM_SIZE];
    char backing[32], path[48];
    const char *unit = NULL;
    uint32_t st, chan = 0, maj = 0, min = 0, fid;
    uint16_t ver = 0;
    unsigned n, i;
    uint8_t hdr[512];

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_acp_ods5: the executive ACP mounts + reads + writes a real "
           "Alpha V8.4 ODS-5 volume (rd vms-7e7e) ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    {
        int fd = vms_kif_open();
        if (fd < 0) {
            printf("=== test_syssvc_acp_ods5: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
            return EXIT_SKIP;
        }
        vms_kif_close();
    }

    for (i = 0; i < sizeof(units) / sizeof(units[0]); i++)
        if (vms_kif_disk_resolve(units[i], backing, sizeof(backing), &maj, &min) == SS$_NORMAL) {
            unit = units[i];
            break;
        }
    check(unit != NULL, "the ODS-5 fixture disk is attached (VDA500:/DKA500:, the sixth disk)");
    if (!unit) {
        printf("=== test_syssvc_acp_ods5: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }
    snprintf(path, sizeof(path), "/dev/%s", backing);
    g_rawfd = open(path, O_RDONLY | O_DIRECT);
    if (posix_memalign((void **)&g_buf, 4096, 4096) != 0)
        g_buf = NULL;
    check(g_rawfd >= 0 && g_buf != NULL, "raw O_DIRECT view of the backing device (independent of the ACP)");

    /* ---- 1. MOUNT ---- */
    st = vms_kif_acp_mount(unit);
    check($VMS_STATUS_SUCCESS(st), "$MOUNT accepts the real level-5 (ODS-5) volume");
    st = vms_kif_acp_assign(unit, &chan);
    check($VMS_STATUS_SUCCESS(st) && chan != 0, "$ASSIGN a file-class channel to it");
    if (!chan) {
        printf("=== test_syssvc_acp_ods5: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }
    memset(&a, 0, sizeof(a));
    a.chan = chan;
    strncpy(a.name, "TEST.DIR", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&a);
    check($VMS_STATUS_SUCCESS(st) && fidof(a.fid_num, a.fid_nmx) == TEST_DIR_FID,
          "[000000]TEST.DIR resolves to FID 11 (the real directory)");
    (void)vms_kif_acp_deaccess(chan);

    /* ---- 2. lookups, case-blind ---- */
    check(find_fid(chan, "c++config.h", &ver) == 12 && ver == 1, "c++config.h -> FID (12,1,0) ;1");
    check(find_fid(chan, "C++CONFIG.H", NULL) == 12, "C++CONFIG.H finds the same file (case-blind)");
    check(find_fid(chan, "libstdc++.a", NULL) == 13, "libstdc++.a -> FID 13");
    check(find_fid(chan, "mixedcase.txt", NULL) == 14, "mixedcase.txt finds MixedCase.Txt (FID 14)");
    check(find_fid(chan, "a.b.c.d", NULL) == 15, "a.b.c.d (multi-dot) -> FID 15");
    check(find_fid(chan, "CAF\xc9.TXT", NULL) == 16, "CAF<E-acute>.TXT finds caf<e-acute>.txt (Latin-1 fold)");
    check(find_fid(chan, LONGNAME, NULL) == 18, "the 73-byte name -> FID 18");
    check(find_fid(chan, "a b.txt", NULL) == 20, "'a b.txt' (a space) -> FID 20");
    check(find_fid(chan, "nosuch.h", NULL) == 0, "an absent name is not found (negative control)");

    /* ---- 3. read what the real system wrote ---- */
    st = access_in(chan, TEST_DIR_FID, "c++config.h", 0, 0, &a);
    {
        char buf[512];
        memset(&r, 0, sizeof(r));
        memset(buf, 0, sizeof(buf));
        r.chan = chan; r.vbn = 1; r.length = 512; r.buffer = (uint64_t)(uintptr_t)buf;
        st = vms_kif_acp_readvb(&r);
        check($VMS_STATUS_SUCCESS(st) && memmem(buf, sizeof(buf), "#define CXXCONFIG 1", 19) != NULL,
              "IO$_READVBLK of c++config.h returns the record the real system wrote");
    }
    (void)vms_kif_acp_deaccess(chan);

    /* ---- 4. $SEARCH: real order, case preserved, type after the last dot ---- */
    {
        static const char *const want[] = {
            "a b.txt;1", "a.b.c.d;1", "c++config.h;1", "caf\xe9.txt;1", "libstdc++.a;1",
            "MixedCase.Txt;1", "PLAIN.TXT;1", "README.;1", LONGNAME ";1" };
        int ok;
        n = search_all(chan, TEST_DIR_FID, "*.*;*", res, 32);
        ok = (n == sizeof(want) / sizeof(want[0]));
        for (i = 0; ok && i < n; i++)
            ok = strcmp(res[i], want[i]) == 0;
        if (!ok)
            for (i = 0; i < n; i++)
                printf("    search[%u] = '%s'\n", i, res[i]);
        check(ok, "$SEARCH *.*;* lists the 8-bit names in the real directory order, case preserved");
        n = search_all(chan, TEST_DIR_FID, "*.d", res, 32);
        check(n == 1 && strcmp(res[0], "a.b.c.d;1") == 0, "$SEARCH *.d matches a.b.c.d (type after the last dot)");
        n = search_all(chan, TEST_DIR_FID, "*.TXT", res, 32);
        check(n == 4, "$SEARCH *.TXT matches the four .txt/.Txt/.TXT names case-blind");
    }

    /* ---- 5. CREATE ---- */
    st = fileop(chan, VMS_ACP_FOP_CREATE, VMS_ACP_M_CREATE, "Gcc-Plugin.h", 0, &f);
    fid = fidof(f.fid_num, f.fid_nmx);
    check($VMS_STATUS_SUCCESS(st) && f.out_version == 1 && fid > 21,
          "IO$_CREATE Gcc-Plugin.h on the ODS-5 volume -> a new FID ;1");
    check(find_fid(chan, "GCC-PLUGIN.H", NULL) == fid, "it is found under another case");
    n = search_all(chan, TEST_DIR_FID, "gcc*.*", res, 32);
    check(n == 1 && strcmp(res[0], "Gcc-Plugin.h;1") == 0, "$SEARCH returns it as created: Gcc-Plugin.h;1");
    {
        uint32_t lbn = raw_hdr_lbn(fid);
        int ok = lbn && raw_read(lbn, hdr) == 0;
        check(ok && rd16(hdr + 6) == ODS2_STRUCLEV_V5 && hdr[0] == 40,
              "on the platter: a level-5 header, ident at word 40 (as the real volume's)");
        check(ok && hdr[80] == ODS5_NAMETYPE_ODS2 && hdr[81] == 14 &&
              memcmp(hdr + 80 + 76, "Gcc-Plugin.h;1", 14) == 0,
              "on the platter: FI5 name 'Gcc-Plugin.h;1', type ODS-2 (an ODS-2-legal mixed-case name)");
    }
    check(raw_dir_flags("Gcc-Plugin.h") == 0x00, "on the platter: its directory record, name type ODS-2 (flags 0x00)");

    st = fileop(chan, VMS_ACP_FOP_CREATE, VMS_ACP_M_CREATE, "gcc++-new.h", 0, &f);
    check($VMS_STATUS_SUCCESS(st) && f.out_version == 1, "IO$_CREATE gcc++-new.h (ISL-1 characters)");
    check(raw_dir_flags("gcc++-new.h") == 0x08, "on the platter: its directory record, name type ISL-1 (flags 0x08)");
    {
        uint32_t lbn = raw_hdr_lbn(fidof(f.fid_num, f.fid_nmx));
        check(lbn && raw_read(lbn, hdr) == 0 && hdr[80] == ODS5_NAMETYPE_ISL1 &&
              memcmp(hdr + 80 + 76, "gcc++-new.h;1", 13) == 0,
              "on the platter: FI5 name 'gcc++-new.h;1', type ISL-1");
    }

    /* different case of an existing name -> the next version of THAT name */
    st = fileop(chan, VMS_ACP_FOP_CREATE, VMS_ACP_M_CREATE, "LIBSTDC++.A", 0, &f);
    check($VMS_STATUS_SUCCESS(st) && f.out_version == 2,
          "IO$_CREATE LIBSTDC++.A makes libstdc++.a;2 (case-blind, as the real E.txt;2)");
    n = search_all(chan, TEST_DIR_FID, "libstdc*.*;*", res, 32);
    check(n == 2 && strcmp(res[0], "libstdc++.a;2") == 0 && strcmp(res[1], "libstdc++.a;1") == 0,
          "the record keeps its first case: libstdc++.a;2, libstdc++.a;1");

    /* ---- 6. rename + delete ---- */
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_MODIFY; f.modifiers = VMS_ACP_M_MOVE;
    f.did_num = TEST_DIR_FID; f.did_seq = 1; f.version = 1;
    strncpy(f.name, "gcc++-new.h", VMS_ACP_NAME_SIZE - 1);
    strncpy(f.new_name, "gcc-plugin-api.version.h", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_fileop(&f);
    check($VMS_STATUS_SUCCESS(st), "IO$_MODIFY!IO$M_MOVE gcc++-new.h -> gcc-plugin-api.version.h");
    check(find_fid(chan, "gcc-plugin-api.version.h", NULL) != 0 && find_fid(chan, "gcc++-new.h", NULL) == 0,
          "the new multi-dot name resolves, the old one is gone");

    st = fileop(chan, VMS_ACP_FOP_DELETE, VMS_ACP_M_DELETE, "GCC-PLUGIN-API.VERSION.H", 1, &f);
    check($VMS_STATUS_SUCCESS(st), "IO$_DELETE by a different case");
    st = fileop(chan, VMS_ACP_FOP_DELETE, VMS_ACP_M_DELETE, "gcc-plugin.H", 1, &f);
    check($VMS_STATUS_SUCCESS(st), "IO$_DELETE gcc-plugin.H removes Gcc-Plugin.h");
    st = fileop(chan, VMS_ACP_FOP_DELETE, VMS_ACP_M_DELETE, "libstdc++.a", 2, &f);
    check($VMS_STATUS_SUCCESS(st), "IO$_DELETE libstdc++.a;2");
    n = search_all(chan, TEST_DIR_FID, "*.*;*", res, 32);
    check(n == 9, "after the deletes [TEST] lists its nine original 8-bit names again");

    /* ---- negative control: the ODS-2 system volume is still ODS-2 ---- */
    {
        static const char *const sys[] = { "VDA0:", "DKA0:" };
        uint32_t c2 = 0;
        const char *u2 = NULL;
        for (i = 0; i < 2 && !u2; i++)
            if (vms_kif_disk_resolve(sys[i], backing, sizeof(backing), &maj, &min) == SS$_NORMAL)
                u2 = sys[i];
        if (u2 && $VMS_STATUS_SUCCESS(vms_kif_acp_mount(u2)) &&
            $VMS_STATUS_SUCCESS(vms_kif_acp_assign(u2, &c2)) && c2) {
            memset(&a, 0, sizeof(a));
            a.chan = c2;
            strncpy(a.name, "OVMXDIR.DIR", VMS_ACP_NAME_SIZE - 1);
            st = vms_kif_acp_access(&a);
            (void)vms_kif_acp_deaccess(c2);
            n = search_all(c2, 11, "hello.*", res, 32);
            check($VMS_STATUS_SUCCESS(st) && fidof(a.fid_num, a.fid_nmx) == 11 && n == 1 &&
                  strcmp(res[0], "HELLO.TXT;1") == 0,
                  "the real-VAX ODS-2 volume still resolves [OVMXDIR] and lists HELLO.TXT;1");
        } else {
            check(0, "the ODS-2 fixture volume (VDA0:/DKA0:) mounts and assigns");
        }
    }

    if (g_rawfd >= 0)
        close(g_rawfd);
    printf("=== test_syssvc_acp_ods5: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
