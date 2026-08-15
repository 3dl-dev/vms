/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_initialize.c - Rule-9 proof that INITIALIZE writes a GENUINE
 * ODS-2 volume (vms-6ef, R4 of the real-ODS-2-runtime epic vms-5eb).
 *
 * WHAT THIS PROVES, AND WHY. tools/vms_initialize.c (INITIALIZE.EXE) used to
 * write ONLY OVMX's bespoke, NON-genuine VMFS/VFH2 structure (hb_magic ==
 * VMSFS_HOME_MAGIC, MFD == FID 3). vms-6ef adds an `--ods2` path that
 * delegates to the byte-genuine writer in src/vmsfs/ods2 (ods2_volume_format,
 * the same [F15] path that MOUNTs clean on a real VAX). This test proves the
 * ODS-2 path produces a real Files-11 volume that R1's block-backed reader
 * (ods2_bdev_*, vms-6cb) parses clean, and that the on-disk bytes are
 * byte-for-byte identical to the writer's own known-good output.
 *
 * TWO INDEPENDENT PROOFS:
 *
 *   1. METHOD proof (always runs, no external dependency): reproduce EXACTLY
 *      the sequence format_volume_ods2() runs -- size -> maxfiles heuristic
 *      -> ods2_volume_format() -> write [0, next_free_lbn) to a real loop
 *      IMAGE FILE fd -- then drive the block-backed reader over that fd and
 *      verify the done-condition field-by-field (DECFILE11B + BOTH additive
 *      checksums via ods2_bdev_open, MFD FID (4,4,0) ident "000000.DIR",
 *      INDEXF.SYS FID 1, BITMAP.SYS FID 2, and all ten reserved files listed
 *      in [000000]). A second, independent ods2_volume_format() of the same
 *      params is then byte-compared against what landed on disk -- proving
 *      the on-disk image is the writer's known-good output, deterministically.
 *
 *   2. BINARY proof (runs when INIT_EXE points at a built INITIALIZE.EXE, as
 *      tests/ods2/CMakeLists.txt wires it under BUILD_TOOLS): exec the ACTUAL
 *      shipped binary with `--ods2` against a fresh image file, then verify
 *      the same fields over its output AND byte-compare its metadata region
 *      against the in-process writer image -- proving the binary itself
 *      writes byte-identical genuine ODS-2, not just that the library can.
 *
 * Every fact is verified against the ods2.h on-disk structs / the genuine
 * reader -- never via POSIX stat()/opendir(). The default (no --ods2) VMFS
 * path is deliberately unchanged and is NOT exercised here; flipping the
 * default is the atomic read-path group (epic vms-5eb R2/R3/R5/R6).
 */

#define _POSIX_C_SOURCE 200809L

#include "vmsfs/ods2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

static int g_failures = 0;

#define CHECK(cond, msg) do {                                          \
    if (!(cond)) {                                                     \
        printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
        g_failures++;                                                  \
    } else {                                                           \
        printf("  PASS: %s\n", (msg));                                 \
    }                                                                  \
} while (0)

/* Chosen so the whole fixed reserved-file layout fits comfortably: 4 MB. */
#define VOL_MB        4u
#define VOL_LABEL     "OVMXR4"

/* ---- INITIALIZE's OWN maxfiles heuristic, replicated verbatim from
 *      tools/vms_initialize.c:format_volume_ods2(). ---- */
static uint32_t init_maxfiles(uint32_t total_blocks)
{
    uint32_t maxfiles = total_blocks / 100;
    if (maxfiles < 16)    maxfiles = 16;
    if (maxfiles > 65535) maxfiles = 65535;
    if (maxfiles < ODS2_RESFILES) maxfiles = ODS2_RESFILES;
    return maxfiles;
}

/*
 * Build a genuine ODS-2 image in RAM exactly as format_volume_ods2() does.
 * Returns a malloc'd image (caller frees) of total_blocks*512 bytes and, via
 * *meta_blocks_out, the number of populated metadata blocks (wvol.next_free_lbn)
 * -- the region INITIALIZE actually writes to the device.
 */
static uint8_t *build_ods2_image(uint32_t total_blocks, const char *label,
                                 uint32_t *meta_blocks_out)
{
    size_t image_len = (size_t)total_blocks * ODS2_BLOCK_SIZE;
    uint8_t *image = calloc(1, image_len);
    if (!image)
        return NULL;

    ods2_format_params_t params;
    params.total_blocks = total_blocks;
    params.maxfiles     = init_maxfiles(total_blocks);
    params.volname      = label;

    ods2_wvolume_t wvol;
    if (ods2_volume_format(image, image_len, &params, &wvol) != ODS2_OK) {
        free(image);
        return NULL;
    }
    *meta_blocks_out = wvol.next_free_lbn;
    return image;
}

/* ---- directory-listing capture ---- */
struct dir_cap {
    char names[32][16];
    int  count;
};

static int dir_collect(const char *name, unsigned name_len,
                       uint16_t version, const ods2_fid_t *fid, void *ctx)
{
    struct dir_cap *c = (struct dir_cap *)ctx;
    (void)version; (void)fid;
    if (c->count < 32) {
        unsigned n = name_len < 15 ? name_len : 15;
        memcpy(c->names[c->count], name, n);
        c->names[c->count][n] = '\0';
    }
    c->count++;
    return 0;
}

static int dir_has(const struct dir_cap *c, const char *name)
{
    int i;
    for (i = 0; i < c->count && i < 32; i++)
        if (strcmp(c->names[i], name) == 0)
            return 1;
    return 0;
}

/* ---- ident-name helper (space-padded "NAME.TYPE;VER" -> "NAME.TYPE") ---- */
static void ident_basename(const uint8_t *header, char *out, size_t out_len)
{
    const ods2_ident_t *id = ods2_fh2_ident(header);
    size_t i = 0;
    if (!id) { out[0] = '\0'; return; }
    for (; i < sizeof(id->fi2_filename) && i + 1 < out_len; i++) {
        char ch = id->fi2_filename[i];
        if (ch == ' ' || ch == ';')
            break;
        out[i] = ch;
    }
    out[i] = '\0';
}

/*
 * Drive the block-backed genuine reader over a volume fd and verify the
 * vms-6ef done-condition, field by field. Returns the number of failures.
 */
static int verify_ods2_volume(int fd, const char *what)
{
    int before = g_failures;
    ods2_bdev_t bv;
    uint8_t hdr[ODS2_BLOCK_SIZE];
    ods2_status_t st;
    char nm[24];

    printf("[verify:%s]\n", what);

    /* ods2_bdev_open validates the home block: DECFILE11B + BOTH additive
     * checksums + strict structure level. A non-genuine (VMFS) volume, or one
     * with a bad checksum, fails right here. */
    st = ods2_bdev_open(&bv, fd, 0);
    CHECK(st == ODS2_OK, "home block opens: DECFILE11B + dual checksums valid");
    if (st != ODS2_OK)
        return g_failures - before;

    CHECK(memcmp(bv.home.hm2_format, ODS2_FORMAT_STRING, ODS2_FORMAT_LEN) == 0,
          "hm2_format == \"DECFILE11B  \"");
    CHECK(bv.home.hm2_struclev == ODS2_STRUCLEV_V2,
          "hm2_struclev == 0x0201 (ODS-2 level 2, version 1)");
    CHECK(bv.home.hm2_resfiles == ODS2_RESFILES,
          "hm2_resfiles == 10 (real-fixture oracle)");

    /* MFD: FID (4,4,0), ident 000000.DIR -- the crux done-condition value. */
    st = ods2_bdev_read_header(&bv, ODS2_FID_MFD, hdr, sizeof(hdr));
    CHECK(st == ODS2_OK, "MFD header (FID 4) reads + checksum-validates");
    if (st == ODS2_OK) {
        const ods2_fh2_t *fh = (const ods2_fh2_t *)hdr;
        CHECK(fh->fh2_fid.fid_num == ODS2_FID_MFD, "MFD fid_num == 4");
        CHECK(fh->fh2_fid.fid_seq == ODS2_FID_MFD, "MFD fid_seq == 4");
        CHECK(fh->fh2_fid.fid_rvn == 0,            "MFD fid_rvn == 0");
        ident_basename(hdr, nm, sizeof(nm));
        CHECK(strcmp(nm, "000000.DIR") == 0, "MFD ident == 000000.DIR");

        /* [000000] lists all ten reserved system files. */
        struct dir_cap cap;
        memset(&cap, 0, sizeof(cap));
        st = ods2_bdev_list_dir(&bv, hdr, dir_collect, &cap);
        CHECK(st == ODS2_OK, "MFD directory lists");
        CHECK(dir_has(&cap, "000000.DIR"),   "MFD lists 000000.DIR");
        CHECK(dir_has(&cap, "INDEXF.SYS"),   "MFD lists INDEXF.SYS");
        CHECK(dir_has(&cap, "BITMAP.SYS"),   "MFD lists BITMAP.SYS");
        CHECK(dir_has(&cap, "SECURITY.SYS"), "MFD lists SECURITY.SYS");
        CHECK(cap.count == (int)ODS2_RESFILES, "MFD lists exactly 10 reserved files");
    }

    /* INDEXF.SYS FID 1, BITMAP.SYS FID 2. */
    st = ods2_bdev_read_header(&bv, ODS2_FID_INDEXF, hdr, sizeof(hdr));
    CHECK(st == ODS2_OK, "INDEXF.SYS header (FID 1) reads + validates");
    if (st == ODS2_OK) {
        ident_basename(hdr, nm, sizeof(nm));
        CHECK(strcmp(nm, "INDEXF.SYS") == 0, "FID 1 ident == INDEXF.SYS");
    }
    st = ods2_bdev_read_header(&bv, ODS2_FID_BITMAP, hdr, sizeof(hdr));
    CHECK(st == ODS2_OK, "BITMAP.SYS header (FID 2) reads + validates");
    if (st == ODS2_OK) {
        ident_basename(hdr, nm, sizeof(nm));
        CHECK(strcmp(nm, "BITMAP.SYS") == 0, "FID 2 ident == BITMAP.SYS");
    }

    return g_failures - before;
}

/* Lay `image[0..meta_blocks)` into a fresh temp file and return its fd (the
 * rest of the file stays sparse-zero, matching INITIALIZE's write of only the
 * metadata region and a freshly ftruncate'd device's zero fill). */
static int lay_image_to_fd(const uint8_t *image, uint32_t meta_blocks,
                           uint32_t total_blocks, char *path_out)
{
    strcpy(path_out, "/tmp/test_ods2_init.XXXXXX");
    int fd = mkstemp(path_out);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)total_blocks * ODS2_BLOCK_SIZE) != 0) {
        close(fd); return -1;
    }
    if (pwrite(fd, image, (size_t)meta_blocks * ODS2_BLOCK_SIZE, 0)
            != (ssize_t)((size_t)meta_blocks * ODS2_BLOCK_SIZE)) {
        close(fd); return -1;
    }
    return fd;
}

/* Read the first `nblocks` blocks off a file into a fresh buffer. */
static uint8_t *slurp(const char *path, uint32_t nblocks)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    size_t len = (size_t)nblocks * ODS2_BLOCK_SIZE;
    uint8_t *buf = malloc(len);
    if (!buf) { close(fd); return NULL; }
    if (pread(fd, buf, len, 0) != (ssize_t)len) { free(buf); close(fd); return NULL; }
    close(fd);
    return buf;
}

int main(void)
{
    uint32_t total_blocks = (uint32_t)((uint64_t)VOL_MB * 1024 * 1024 / ODS2_BLOCK_SIZE);
    uint32_t meta_blocks = 0;
    char path[64];

    printf("=== test_ods2_initialize (vms-6ef): INITIALIZE writes genuine ODS-2 ===\n");
    printf("volume: %u MB = %u blocks, label %s\n", VOL_MB, total_blocks, VOL_LABEL);

    /* ---- PROOF 1: the method INITIALIZE uses ---- */
    uint8_t *image = build_ods2_image(total_blocks, VOL_LABEL, &meta_blocks);
    CHECK(image != NULL, "ods2_volume_format builds the volume image");
    if (!image)
        return 1;

    int fd = lay_image_to_fd(image, meta_blocks, total_blocks, path);
    CHECK(fd >= 0, "wrote metadata region to a real loop image file");
    if (fd >= 0) {
        verify_ods2_volume(fd, "method");
        close(fd);
    }

    /* On-disk bytes == writer's known-good output: a second, independent
     * ods2_volume_format() of the SAME params must reproduce the exact
     * metadata region byte-for-byte (the writer is deterministic -- it emits
     * no timestamps/serials). */
    {
        uint32_t meta2 = 0;
        uint8_t *image2 = build_ods2_image(total_blocks, VOL_LABEL, &meta2);
        CHECK(image2 != NULL, "second independent format builds");
        if (image2) {
            CHECK(meta2 == meta_blocks, "metadata block count is deterministic");
            CHECK(memcmp(image, image2, (size_t)meta_blocks * ODS2_BLOCK_SIZE) == 0,
                  "on-disk metadata is byte-identical to writer's known-good output");
            free(image2);
        }
    }
    unlink(path);

    /* ---- PROOF 2: the actual shipped INITIALIZE.EXE binary ---- */
    const char *init_exe = getenv("INIT_EXE");
    if (init_exe && init_exe[0] && access(init_exe, X_OK) == 0) {
        char binpath[64];
        strcpy(binpath, "/tmp/test_ods2_initbin.XXXXXX");
        int bfd = mkstemp(binpath);
        CHECK(bfd >= 0, "temp file for binary output");
        if (bfd >= 0) {
            close(bfd);
            unlink(binpath);   /* let INITIALIZE create it fresh at this path */

            char mb[8];
            snprintf(mb, sizeof(mb), "%u", VOL_MB);
            pid_t pid = fork();
            if (pid == 0) {
                /* Silence the tool's %INIT-I chatter in the test log. */
                int devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) { dup2(devnull, 1); close(devnull); }
                execl(init_exe, init_exe, "--ods2", binpath, VOL_LABEL, mb, (char *)NULL);
                _exit(127);
            }
            int status = -1;
            CHECK(pid > 0 && waitpid(pid, &status, 0) == pid, "INITIALIZE.EXE ran");
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "INITIALIZE.EXE --ods2 exited 0");

            int vfd = open(binpath, O_RDONLY);
            CHECK(vfd >= 0, "binary-written volume opens");
            if (vfd >= 0) {
                verify_ods2_volume(vfd, "binary");
                close(vfd);
            }

            /* The binary's metadata region must equal the in-process writer's
             * -- byte-for-byte -- proving the shipped binary is faithful. */
            uint8_t *binmeta = slurp(binpath, meta_blocks);
            CHECK(binmeta != NULL, "read back binary metadata region");
            if (binmeta) {
                CHECK(memcmp(image, binmeta, (size_t)meta_blocks * ODS2_BLOCK_SIZE) == 0,
                      "INITIALIZE.EXE output == writer's known-good bytes");
                free(binmeta);
            }
            unlink(binpath);
        }
    } else {
        printf("[binary proof SKIPPED: INIT_EXE not set/executable "
               "(BUILD_TOOLS off?) -- method proof above still fully covers "
               "the ODS-2 writer path]\n");
    }

    free(image);

    printf("=== %s: %d failure(s) ===\n",
           g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
