/*
 * mkimage_ods2_sysvol.c - Master a GENUINE ODS-2 (Files-11 L2, "DECFILE11B")
 * test volume carrying a real SYSTEM-DISK directory tree, so
 * tests/qemu/test_syssvc_dirlogical_acp.c (vms-0044, epic vms-208) can prove
 * the full rooted/concealed directory-logical resolution chain against a real
 * /dev/vms:
 *
 *     SYS$SYSTEM:SYSUAF.DAT
 *       -> lnm+$PARSE compose -> DKA300:[SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT;
 *       -> the Files-11 ACP walks MFD -> [SYS0] -> [SYSCOMMON] -> [SYSEXE]
 *          -> SYSUAF.DAT, returning the file FID; $GET reads it byte-exact.
 *
 * The tree deliberately mirrors a running system disk's concealed-rooted layout
 * (VSI OpenVMS System Manager's Manual, Vol. 1, "The System Disk"): the shared
 * files live under the COMMON root [SYS0.SYSCOMMON.*], while the node-specific
 * root [SYS0] carries only SYSCOMMON.DIR. So the composed search-list order
 * (node member [SYS0.SYSEXE] first, common member [SYS0.SYSCOMMON.SYSEXE]
 * second) MUST fall through the empty node member and resolve on the common
 * member -- the exact search-list behaviour the test asserts.
 *
 * DIRECTORY TREE (deterministic; the writer allocates FIDs sequentially from
 * ODS2_RESFILES+1 and PRINTS every FID it lays down, so the test's ground truth
 * is the writer's own output, not values the test invents):
 *
 *     [000000]SYS0.DIR
 *       [SYS0]SYSCOMMON.DIR
 *         [SYS0.SYSCOMMON]SYSEXE.DIR
 *           SYSUAF.DAT;1        (raw/FIXED; the REAL shipped SYSUAF.DAT bytes)
 *           RIGHTSLIST.DAT;1    (raw/FIXED; the REAL shipped RIGHTSLIST.DAT bytes)
 *         [SYS0.SYSCOMMON]SYSMGR.DIR
 *           STARTUP.COM;1       (text)
 *           WELCOME.TXT;1       (text)
 *         [SYS0.SYSCOMMON]SYSLIB.DIR
 *           STARLET.OLB;1       (raw/FIXED, 512 bytes, known pattern)
 *
 * THE SYSTEM DATA FILES ARE THE PRODUCT'S OWN, NOT INVENTED HERE (vms-5f0, epic
 * vms-208). SYSUAF.DAT and RIGHTSLIST.DAT are copied VERBATIM from
 * distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/ -- the exact files ovmx_init
 * provisions onto the system disk on the real boot -- so the login/rights suites
 * that read them back through the RMS-over-ACP rooted-logical open
 * (SYS$SYSTEM:SYSUAF.DAT -> DKA300:[SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT) assert
 * against what actually boots, not a fixture invented for the harness. They are
 * written stream-LF verbatim (add_raw), which is byte-identical to what a real
 * $CREATE/$PUT of the same text would lay down (one line == one LF-delimited
 * record), so RMS $GET reads them back as records.
 *
 * Built with the SAME byte-genuine writer (src/vmsfs/ods2/ods2_writer.c) that
 * the boot master, mkimage_ods2_real.c and mkimage_ods2_search.c use -- it adds
 * no new ODS-2 knowledge (CLAUDE.md Rule 8; see vmsfs/ods2.h's provenance).
 *
 * Usage: mkimage_ods2_sysvol <output-file> <size-in-MB> \
 *                            <sysuaf-path> <rightslist-path> \
 *                            [SUBDIR:NAME=host-path]...
 *
 * OPTIONAL TRAILING ARGS (vms-3f5, rung of vms-104). Each extra arg masters one
 * verbatim binary file into a named system directory:
 *   SYSEXE:TCC.EXE=/path/to/TCC.EXE      -> [SYS0.SYSCOMMON.SYSEXE]TCC.EXE
 *   SYSLIB:DECC$SHR.EXE=/path/to/it      -> [SYS0.SYSCOMMON.SYSLIB]DECC$SHR.EXE
 * so the KE toolchain harness (tests/qemu/test_syssvc_mmk_build.c) can activate
 * the MMK-driven image THROUGH THE ACP off this generated system volume -- the
 * OVMX-native toolchain (TCC/LIBRARIAN/LINK.EXE) and the C run-time shareable
 * (DECC$SHR.EXE) IMGACT binds -- WITHOUT ever mastering OVMX images onto the
 * clean-room real-VAX DKA0: fixture (vms-29ff). Multi-MB binaries are handled by
 * the same byte-genuine ODS-2 writer (its >256-block retrieval-map split landed
 * in ods2_edit.c, vms-3a8); raise <size-in-MB> to fit them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "vmsfs/ods2.h"

#define DEFAULT_SIZE_MB 2
#define MIN_BLOCKS      64

/* Read an entire file into a malloc'd buffer. Exits on any error -- a missing
 * or unreadable shipped file must fail the master LOUDLY (the suites that read
 * it back would otherwise assert against a stale/absent fixture). */
static uint8_t *read_whole_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "mkimage_ods2_sysvol: cannot open %s\n", path); exit(1); }
    if (fseek(f, 0, SEEK_END) != 0) { perror("fseek"); exit(1); }
    long n = ftell(f);
    if (n < 0) { perror("ftell"); exit(1); }
    rewind(f);
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf) { fprintf(stderr, "mkimage_ods2_sysvol: OOM reading %s\n", path); exit(1); }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "mkimage_ods2_sysvol: short read on %s\n", path); exit(1);
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

#define STARLET_LEN 512
static void fill_starlet(uint8_t *b)
{
    for (int i = 0; i < STARLET_LEN; i++)
        b[i] = (uint8_t)(0xA5 ^ (i * 11u));
}

/* A stand-in SYS$SYSTEM image so test_syssvc_acp_access can prove the
 * per-file-class protection (vms-109): mastered by NAME "DCL.EXE" it takes the
 * World:RE (0xAA00) image default, distinct from RIGHTSLIST.DAT (World:R) and
 * SYSUAF.DAT (World:none). Content is arbitrary -- protection is keyed on the
 * file class (name/kind), not the bytes. */
#define DCLEXE_LEN 512
static void fill_dclexe(uint8_t *b)
{
    for (int i = 0; i < DCLEXE_LEN; i++)
        b[i] = (uint8_t)(0x5A ^ (i * 7u));
}

static int write_block(int fd, uint32_t lbn, const void *buf)
{
    off_t offset = (off_t)lbn * ODS2_BLOCK_SIZE;
    if (lseek(fd, offset, SEEK_SET) != offset)
        return -1;
    if (write(fd, buf, ODS2_BLOCK_SIZE) != ODS2_BLOCK_SIZE)
        return -1;
    return 0;
}

static void die(const char *what, ods2_status_t st)
{
    fprintf(stderr, "mkimage_ods2_sysvol: %s failed: %s\n", what,
            ods2_strerror(st));
    exit(1);
}

/* Create subdirectory <name>.DIR under <parent> and enter it there. */
static ods2_fid_t add_dir(ods2_wvolume_t *wvol, ods2_fid_t parent,
                          const char *dirname)
{
    char namdir[64];
    ods2_fid_t fid;
    ods2_status_t st;
    snprintf(namdir, sizeof(namdir), "%s.DIR", dirname);
    st = ods2_wvolume_create_dir(wvol, namdir, 1, parent, &fid);
    if (st != ODS2_OK) die(namdir, st);
    st = ods2_wvolume_dir_insert(wvol, parent, namdir, 1, fid);
    if (st != ODS2_OK) die("dir_insert(dir)", st);
    printf("  %-28s -> FID (%u,%u,%u)\n", namdir,
           (unsigned)ods2_fid_number(&fid), fid.fid_seq, fid.fid_rvn);
    return fid;
}

/* Create a text file (RFM=VAR) and enter it in <dir>. */
static void add_text(ods2_wvolume_t *wvol, ods2_fid_t dir,
                     const char *name, const char *text)
{
    ods2_fid_t fid;
    ods2_status_t st;
    st = ods2_wvolume_create_file(wvol, name, 1, (const uint8_t *)text,
                                  strlen(text), dir, &fid);
    if (st != ODS2_OK) die(name, st);
    st = ods2_wvolume_dir_insert(wvol, dir, name, 1, fid);
    if (st != ODS2_OK) die("dir_insert(text)", st);
    printf("  %-28s -> FID (%u,%u,%u)\n", name,
           (unsigned)ods2_fid_number(&fid), fid.fid_seq, fid.fid_rvn);
}

/* Create a raw/FIXED file (bytes verbatim) and enter it in <dir>. */
static void add_raw(ods2_wvolume_t *wvol, ods2_fid_t dir, const char *name,
                    const uint8_t *data, size_t len)
{
    ods2_fid_t fid;
    ods2_status_t st;
    st = ods2_wvolume_create_file_raw(wvol, name, 1, data, len, dir, &fid);
    if (st != ODS2_OK) die(name, st);
    st = ods2_wvolume_dir_insert(wvol, dir, name, 1, fid);
    if (st != ODS2_OK) die("dir_insert(raw)", st);
    printf("  %-28s -> FID (%u,%u,%u)  [%zu bytes verbatim]\n", name,
           (unsigned)ods2_fid_number(&fid), fid.fid_seq, fid.fid_rvn, len);
}

/* Create a STREAM-LF text file (bytes verbatim, RFM=STMLF) and enter it in
 * <dir>. Verbatim like add_raw (so a block-mode VBN read sees the exact file
 * bytes), but framed RFM=STMLF so an RMS $GET returns one LF-delimited record
 * per call -- the format a genuine VMS text file (SYSUAF.DAT, RIGHTSLIST.DAT)
 * carries, and what LOGINOUT's SYSUAF scan / F$IDENTIFIER's rights read need. */
static void add_stmlf(ods2_wvolume_t *wvol, ods2_fid_t dir, const char *name,
                      const uint8_t *data, size_t len)
{
    ods2_fid_t fid;
    ods2_status_t st;
    st = ods2_wvolume_create_file_stmlf(wvol, name, 1, data, len, dir, &fid);
    if (st != ODS2_OK) die(name, st);
    st = ods2_wvolume_dir_insert(wvol, dir, name, 1, fid);
    if (st != ODS2_OK) die("dir_insert(stmlf)", st);
    printf("  %-28s -> FID (%u,%u,%u)  [%zu bytes verbatim, RFM=STMLF]\n", name,
           (unsigned)ods2_fid_number(&fid), fid.fid_seq, fid.fid_rvn, len);
}

int main(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: mkimage_ods2_sysvol <output-file> <size-in-MB> "
                        "<sysuaf-path> <rightslist-path> [SUBDIR:NAME=host-path]...\n");
        return 1;
    }
    const char *outpath   = argv[1];
    uint64_t    size_mb   = (uint64_t)strtoul(argv[2], NULL, 10);
    const char *sysuaf_p  = argv[3];
    const char *rights_p  = argv[4];
    if (size_mb == 0) size_mb = DEFAULT_SIZE_MB;
    uint32_t total_blocks = (uint32_t)((size_mb * 1024ULL * 1024ULL) / ODS2_BLOCK_SIZE);
    if (total_blocks < MIN_BLOCKS) {
        fprintf(stderr, "mkimage_ods2_sysvol: volume too small (%u blocks)\n",
                total_blocks);
        return 1;
    }

    uint32_t maxfiles = total_blocks / 100;
    if (maxfiles < 32)            maxfiles = 32;
    if (maxfiles > 65535)         maxfiles = 65535;
    if (maxfiles < ODS2_RESFILES) maxfiles = ODS2_RESFILES;

    size_t image_len = (size_t)total_blocks * ODS2_BLOCK_SIZE;
    uint8_t *image = calloc(1, image_len);
    if (!image) {
        fprintf(stderr, "mkimage_ods2_sysvol: cannot allocate %zu bytes\n", image_len);
        return 1;
    }

    ods2_format_params_t params;
    params.total_blocks = total_blocks;
    params.maxfiles     = maxfiles;
    params.volname      = "OVMXSYS";

    ods2_wvolume_t wvol;
    ods2_status_t st = ods2_volume_format(image, image_len, &params, &wvol);
    if (st != ODS2_OK) die("ods2_volume_format", st);

    printf("mkimage_ods2_sysvol: system-disk tree (concealed-rooted layout):\n");

    /* [000000]SYS0 -> [SYS0]SYSCOMMON -> the SYS$xxx dirs under COMMON. */
    ods2_fid_t sys0     = add_dir(&wvol, wvol.mfd_fid, "SYS0");
    ods2_fid_t syscommon= add_dir(&wvol, sys0,         "SYSCOMMON");
    ods2_fid_t sysexe   = add_dir(&wvol, syscommon,    "SYSEXE");
    ods2_fid_t sysmgr   = add_dir(&wvol, syscommon,    "SYSMGR");
    ods2_fid_t syslib   = add_dir(&wvol, syscommon,    "SYSLIB");

    /* The REAL shipped system data files (verbatim from distro/rootfs). */
    size_t sysuaf_len = 0, rights_len = 0;
    uint8_t *sysuaf = read_whole_file(sysuaf_p, &sysuaf_len);
    uint8_t *rights = read_whole_file(rights_p, &rights_len);
    uint8_t starlet[STARLET_LEN]; fill_starlet(starlet);
    uint8_t dclexe[DCLEXE_LEN];   fill_dclexe(dclexe);

    add_stmlf(&wvol, sysexe, "SYSUAF.DAT",     sysuaf, sysuaf_len);
    add_stmlf(&wvol, sysexe, "RIGHTSLIST.DAT", rights, rights_len);
    /* An image so the protection proof has a World:RE (0xAA00) file to test
     * alongside RIGHTSLIST.DAT (World:R) and SYSUAF.DAT (World:none), vms-109. */
    add_raw (&wvol, sysexe, "DCL.EXE",         dclexe, DCLEXE_LEN);
    add_text(&wvol, sysmgr, "STARTUP.COM", "$ WRITE SYS$OUTPUT \"OVMX boot\"\n");
    add_text(&wvol, sysmgr, "WELCOME.TXT", "Welcome to OVMX.\n");
    add_raw (&wvol, syslib, "STARLET.OLB", starlet, STARLET_LEN);

    free(sysuaf);
    free(rights);

    /* OPTIONAL: master extra verbatim binaries into SYSEXE / SYSLIB (vms-3f5).
     * Each trailing arg is "SUBDIR:NAME=host-path". add_raw lays the bytes down
     * verbatim (FIXED), so an IMGACT IO$_READVBLK reads the image byte-exact. */
    for (int ai = 5; ai < argc; ai++) {
        char spec[1024];
        strncpy(spec, argv[ai], sizeof(spec) - 1);
        spec[sizeof(spec) - 1] = '\0';
        char *colon = strchr(spec, ':');
        char *eq    = strchr(spec, '=');
        if (!colon || !eq || colon > eq) {
            fprintf(stderr, "mkimage_ods2_sysvol: malformed extra arg '%s' "
                            "(want SUBDIR:NAME=host-path)\n", argv[ai]);
            return 1;
        }
        *colon = '\0';
        *eq    = '\0';
        const char *subdir = spec;
        const char *name   = colon + 1;
        const char *hpath  = eq + 1;
        ods2_fid_t  target;
        if (strcmp(subdir, "SYSEXE") == 0)      target = sysexe;
        else if (strcmp(subdir, "SYSLIB") == 0) target = syslib;
        else if (strcmp(subdir, "SYSMGR") == 0) target = sysmgr;
        else {
            fprintf(stderr, "mkimage_ods2_sysvol: unknown SUBDIR '%s' "
                            "(SYSEXE|SYSLIB|SYSMGR)\n", subdir);
            return 1;
        }
        size_t   flen = 0;
        uint8_t *fbuf = read_whole_file(hpath, &flen);
        add_raw(&wvol, target, name, fbuf, flen);
        free(fbuf);
    }

    int fd = open(outpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(outpath); free(image); return 1; }
    if (ftruncate(fd, (off_t)image_len) < 0) { perror("ftruncate"); close(fd); free(image); return 1; }
    for (uint32_t lbn = 0; lbn < wvol.next_free_lbn; lbn++) {
        if (write_block(fd, lbn, image + (size_t)lbn * ODS2_BLOCK_SIZE) < 0) {
            perror("write_block"); close(fd); free(image); return 1;
        }
    }
    close(fd);
    free(image);

    printf("mkimage_ods2_sysvol: wrote %s (%u blocks, %u max files, "
           "genuine ODS-2/DECFILE11B)\n", outpath, total_blocks, maxfiles);
    return 0;
}
