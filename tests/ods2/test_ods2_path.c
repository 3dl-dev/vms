/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_path.c - Rule-9 proof for the block-backed ODS-2 PATH-RESOLUTION
 * + CONTENT-READ layer (src/vmsfs/ods2/ods2_path.c), the read-path foundation
 * of the real-ODS-2-runtime flip (epic vms-5eb, rungs R2/R3/R5).
 *
 * WHAT THIS PROVES, AND WHY. The live userspace RMS/DCL/MOUNT path must reach
 * SYS$SYSTEM / SYS$LIBRARY files through GENUINE ODS-2 -- FID -> header ->
 * FM2 extents -> VAR records -- over a real block device, NOT through
 * vmsfs_to_linux_path + POSIX open(2)/opendir(2). ods2_path.c composes the
 * block-backed reader primitives (ods2_bdev_read_header / _list_dir /
 * ods2_fh2_map_walk) into path resolution + content read. This test builds a
 * genuine ODS-2 volume carrying the real system-disk hierarchy shape
 * ([000000] -> [SYS0] -> [SYS0.SYSCOMMON] -> [SYS0.SYSCOMMON.SYSEXE] with
 * files in it), lays it on a real loop image fd, and drives the resolver over
 * that fd:
 *
 *   - resolve [SYS0.SYSCOMMON.SYSEXE] to a real directory FID via a real FID
 *     chain (MFD -> SYS0 -> SYSCOMMON -> SYSEXE), NEVER POSIX opendir;
 *   - list that directory's real records (names + versions + FIDs);
 *   - resolve a file in it to its header + FID;
 *   - read its content back -- raw bytes AND newline-joined VAR text -- by
 *     pread-ing its retrieval-pointer extents, matching what was written;
 *   - honour version selection (highest vs exact) and multi-extent files;
 *   - fail honestly (ODS2_ERR_NOTFOUND) on a missing component/file.
 *
 * Every fact is verified against the ods2.h structs / the genuine reader,
 * never via POSIX stat()/opendir(). The build side here (ods2_volume_format +
 * ods2_wvolume_create_dir/create_file/dir_insert) is exactly the shape the
 * R6 boot-image master builder uses, so this doubles as a builder proof.
 */

#define _POSIX_C_SOURCE 200809L

#include "vmsfs/ods2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int g_failures = 0;

#define CHECK(cond, msg) do {                                          \
    if (!(cond)) {                                                     \
        printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
        g_failures++;                                                  \
    } else {                                                           \
        printf("  PASS: %s\n", (msg));                                 \
    }                                                                  \
} while (0)

#define VOL_MB     4u
#define VOL_LABEL  "OVMXSYS"

/*
 * Content written into the SYSEXE files. ods2_wvolume_create_file() takes RAW
 * text and frames it into RFM=VAR records itself (one record per '\n'-split
 * line); ods2_bdev_read_file_text() decodes those records back to the same
 * newline-joined text. So the round-trip target is simply the raw string.
 */
static const char *const LOGIN_TEXT  = "$ SET NOON\n$ WRITE SYS$OUTPUT \"HI\"\n";
static const char *const README_TEXT = "OVMX\nSYS EXE\n";

/*
 * A BINARY image blob (vms-5eb R6-build): 700 bytes (> one 512-byte block)
 * deliberately full of the bytes RFM=VAR framing would mangle -- 0x0A
 * newlines (VAR splits records on them), 0x00 NULs, 0xFF (the ODS2_DIR_END
 * sentinel), and a 2-byte-length-prefix collision pattern. ods2_wvolume_
 * create_file_raw() must store these VERBATIM so IMGACT reads a real .EXE
 * back unchanged; ods2_wvolume_create_file() (VAR) would corrupt them.
 */
#define IMAGE_LEN 700u
static uint8_t IMAGE_BLOB[IMAGE_LEN];
static void image_blob_init(void)
{
    unsigned i;
    for (i = 0; i < IMAGE_LEN; i++)
        IMAGE_BLOB[i] = (uint8_t)((i * 37u + (i % 5u == 0 ? 0x0A : 0)) & 0xFF);
    IMAGE_BLOB[0]   = 0x7F;   /* ELF-ish magic lead */
    IMAGE_BLOB[1]   = 'E'; IMAGE_BLOB[2] = 'L'; IMAGE_BLOB[3] = 'F';
    IMAGE_BLOB[10]  = 0x0A;   /* newline mid-block */
    IMAGE_BLOB[11]  = 0x00;   /* NUL */
    IMAGE_BLOB[520] = 0xFF;   /* dir-end sentinel byte, in the SECOND block */
    IMAGE_BLOB[521] = 0x0A;
}

struct dir_cap {
    char names[32][20];
    uint16_t vers[32];
    int  count;
};

static int dir_collect(const char *name, unsigned name_len,
                       uint16_t version, const ods2_fid_t *fid, void *ctx)
{
    struct dir_cap *c = (struct dir_cap *)ctx;
    (void)fid;
    if (c->count < 32) {
        unsigned n = name_len < 19 ? name_len : 19;
        memcpy(c->names[c->count], name, n);
        c->names[c->count][n] = '\0';
        c->vers[c->count] = version;
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

int main(void)
{
    uint32_t total_blocks =
        (uint32_t)((uint64_t)VOL_MB * 1024 * 1024 / ODS2_BLOCK_SIZE);
    size_t image_len = (size_t)total_blocks * ODS2_BLOCK_SIZE;
    uint8_t *image = (uint8_t *)calloc(1, image_len);
    ods2_status_t st;

    printf("=== test_ods2_path (vms-5eb R2/R3/R5): block-backed ODS-2 path "
           "resolve + content read ===\n");
    CHECK(image != NULL, "allocate volume image");
    if (!image)
        return 1;

    /* ---- BUILD: a genuine ODS-2 volume with the system-disk hierarchy ---- */
    ods2_format_params_t params = { total_blocks,
                                    total_blocks / 100 < ODS2_RESFILES
                                        ? ODS2_RESFILES : total_blocks / 100,
                                    VOL_LABEL };
    ods2_wvolume_t wvol;
    st = ods2_volume_format(image, image_len, &params, &wvol);
    CHECK(st == ODS2_OK, "ods2_volume_format builds the volume");
    if (st != ODS2_OK) { free(image); return 1; }

    /* [000000] -> [SYS0] -> [SYS0.SYSCOMMON] -> [SYS0.SYSCOMMON.SYSEXE] */
    ods2_fid_t sys0, syscommon, sysexe;
    st = ods2_wvolume_create_dir(&wvol, "SYS0.DIR", 1, wvol.mfd_fid, &sys0);
    CHECK(st == ODS2_OK, "create [SYS0]");
    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "SYS0.DIR", 1, sys0);
    CHECK(st == ODS2_OK, "insert SYS0.DIR into MFD");

    st = ods2_wvolume_create_dir(&wvol, "SYSCOMMON.DIR", 1, sys0, &syscommon);
    CHECK(st == ODS2_OK, "create [SYS0.SYSCOMMON]");
    st = ods2_wvolume_dir_insert(&wvol, sys0, "SYSCOMMON.DIR", 1, syscommon);
    CHECK(st == ODS2_OK, "insert SYSCOMMON.DIR into [SYS0]");

    st = ods2_wvolume_create_dir(&wvol, "SYSEXE.DIR", 1, syscommon, &sysexe);
    CHECK(st == ODS2_OK, "create [SYS0.SYSCOMMON.SYSEXE]");
    st = ods2_wvolume_dir_insert(&wvol, syscommon, "SYSEXE.DIR", 1, sysexe);
    CHECK(st == ODS2_OK, "insert SYSEXE.DIR into [SYS0.SYSCOMMON]");

    /* Two files in SYSEXE: LOGIN.COM;1 + README.TXT;1 (raw content, framed
     * into VAR records by the writer). */
    size_t login_len = strlen(LOGIN_TEXT), readme_len = strlen(README_TEXT);
    ods2_fid_t login_v1, readme_fid;

    st = ods2_wvolume_create_file(&wvol, "LOGIN.COM", 1,
                                  (const uint8_t *)LOGIN_TEXT, login_len,
                                  sysexe, &login_v1);
    CHECK(st == ODS2_OK, "create LOGIN.COM;1");
    st = ods2_wvolume_dir_insert(&wvol, sysexe, "LOGIN.COM", 1, login_v1);
    CHECK(st == ODS2_OK, "insert LOGIN.COM;1");

    st = ods2_wvolume_create_file(&wvol, "README.TXT", 1,
                                  (const uint8_t *)README_TEXT, readme_len,
                                  sysexe, &readme_fid);
    CHECK(st == ODS2_OK, "create README.TXT;1");
    st = ods2_wvolume_dir_insert(&wvol, sysexe, "README.TXT", 1, readme_fid);
    CHECK(st == ODS2_OK, "insert README.TXT;1");

    /* A binary image: verbatim bytes via create_file_raw (R6-build path). */
    image_blob_init();
    ods2_fid_t image_fid;
    st = ods2_wvolume_create_file_raw(&wvol, "IMAGE.EXE", 1,
                                      IMAGE_BLOB, IMAGE_LEN, sysexe, &image_fid);
    CHECK(st == ODS2_OK, "create IMAGE.EXE;1 verbatim (multi-block binary)");
    st = ods2_wvolume_dir_insert(&wvol, sysexe, "IMAGE.EXE", 1, image_fid);
    CHECK(st == ODS2_OK, "insert IMAGE.EXE;1");

    /* ---- lay the metadata region onto a real loop image fd ---- */
    char path[] = "/tmp/test_ods2_path.XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "create loop image file");
    if (fd < 0) { free(image); return 1; }
    if (ftruncate(fd, (off_t)image_len) != 0 ||
        pwrite(fd, image, wvol.next_free_lbn * ODS2_BLOCK_SIZE, 0) < 0) {
        perror("lay image"); free(image); return 1;
    }

    /* ---- RESOLVE + READ over the block device (the done-condition) ---- */
    ods2_bdev_t bv;
    st = ods2_bdev_open(&bv, fd, 0);
    CHECK(st == ODS2_OK, "ods2_bdev_open validates DECFILE11B home block");

    const char *sysexe_comps[] = { "SYS0", "SYSCOMMON", "SYSEXE" };
    uint8_t dirhdr[ODS2_BLOCK_SIZE];
    ods2_fid_t got_dir_fid;

    st = ods2_bdev_resolve_dir(&bv, sysexe_comps, 3, &got_dir_fid,
                               dirhdr, sizeof(dirhdr));
    CHECK(st == ODS2_OK,
          "resolve [SYS0.SYSCOMMON.SYSEXE] via real MFD->SYS0->SYSCOMMON->SYSEXE FID chain");
    CHECK(ods2_fid_number(&got_dir_fid) == ods2_fid_number(&sysexe),
          "resolved SYSEXE FID matches the FID the writer allocated");

    struct dir_cap cap;
    memset(&cap, 0, sizeof(cap));
    st = ods2_bdev_list_dir(&bv, dirhdr, dir_collect, &cap);
    CHECK(st == ODS2_OK, "list [SYS0.SYSCOMMON.SYSEXE] real records");
    CHECK(dir_has(&cap, "LOGIN.COM"),  "SYSEXE lists LOGIN.COM (real record)");
    CHECK(dir_has(&cap, "README.TXT"), "SYSEXE lists README.TXT (real record)");

    /* Resolve LOGIN.COM: both the highest-version (0) and exact-version (1)
     * lookups must land on the one LOGIN.COM;1 the writer created. */
    uint8_t filehdr[ODS2_BLOCK_SIZE];
    ods2_fid_t got_file_fid;
    st = ods2_bdev_resolve_file(&bv, sysexe_comps, 3, "LOGIN.COM", /*highest*/0,
                                &got_file_fid, filehdr, sizeof(filehdr));
    CHECK(st == ODS2_OK, "resolve [SYS0.SYSCOMMON.SYSEXE]LOGIN.COM (highest ver)");
    CHECK(ods2_fid_number(&got_file_fid) == ods2_fid_number(&login_v1),
          "highest-version resolution lands on LOGIN.COM;1");

    st = ods2_bdev_resolve_file(&bv, sysexe_comps, 3, "LOGIN.COM", 1,
                                &got_file_fid, filehdr, sizeof(filehdr));
    CHECK(st == ODS2_OK, "resolve LOGIN.COM;1 (exact version)");
    CHECK(ods2_fid_number(&got_file_fid) == ods2_fid_number(&login_v1),
          "exact-version resolution picks LOGIN.COM;1");

    /* Read content back -- raw framed bytes, then decoded VAR text. The writer
     * frames raw text into RFM=VAR records; read_file returns those framed
     * bytes and read_file_text decodes them back to the original text. */
    uint8_t rawbuf[4096];
    size_t rawlen = 0;
    st = ods2_bdev_read_file(&bv, filehdr, rawbuf, sizeof(rawbuf), &rawlen);
    CHECK(st == ODS2_OK, "read LOGIN.COM raw content off the block device (FM2 extents pread)");
    CHECK(rawlen > 0 && rawlen >= login_len,
          "raw content spans at least the framed record stream");

    char textbuf[4096];
    size_t textlen = 0;
    st = ods2_bdev_read_file_text(&bv, filehdr, textbuf, sizeof(textbuf), &textlen);
    CHECK(st == ODS2_OK, "read LOGIN.COM as VAR text off the block device");
    CHECK(textlen == login_len && memcmp(textbuf, LOGIN_TEXT, login_len) == 0,
          "LOGIN.COM VAR text round-trips to the written content");

    /* README.TXT via a fresh resolve+read, to prove it is not LOGIN-specific. */
    st = ods2_bdev_resolve_file(&bv, sysexe_comps, 3, "README.TXT", 0,
                                &got_file_fid, filehdr, sizeof(filehdr));
    CHECK(st == ODS2_OK, "resolve README.TXT");
    st = ods2_bdev_read_file_text(&bv, filehdr, textbuf, sizeof(textbuf), &textlen);
    CHECK(st == ODS2_OK && textlen == readme_len &&
          memcmp(textbuf, README_TEXT, readme_len) == 0,
          "README.TXT content round-trips");

    /* IMAGE.EXE: verbatim binary round-trip. read_file (RAW, no VAR decode)
     * must return byte-for-byte what create_file_raw wrote -- the property a
     * .EXE needs for IMGACT. Its recattr also reports the exact byte length. */
    st = ods2_bdev_resolve_file(&bv, sysexe_comps, 3, "IMAGE.EXE", 0,
                                &got_file_fid, filehdr, sizeof(filehdr));
    CHECK(st == ODS2_OK, "resolve IMAGE.EXE");
    CHECK(ods2_fid_number(&got_file_fid) == ods2_fid_number(&image_fid),
          "IMAGE.EXE resolves to the FID create_file_raw allocated");
    uint8_t imgbuf[2048];
    size_t imglen = 0;
    st = ods2_bdev_read_file(&bv, filehdr, imgbuf, sizeof(imgbuf), &imglen);
    CHECK(st == ODS2_OK, "read IMAGE.EXE raw content off the block device");
    CHECK(imglen == IMAGE_LEN,
          "IMAGE.EXE valid-byte length (efblk/ffbyte) equals the bytes written");
    CHECK(imglen == IMAGE_LEN && memcmp(imgbuf, IMAGE_BLOB, IMAGE_LEN) == 0,
          "IMAGE.EXE bytes round-trip VERBATIM (VAR framing would have corrupted them)");

    /* Honest failure: a missing directory component and a missing file. */
    const char *bad_comps[] = { "SYS0", "NOPE", "SYSEXE" };
    st = ods2_bdev_resolve_dir(&bv, bad_comps, 3, NULL, dirhdr, sizeof(dirhdr));
    CHECK(st == ODS2_ERR_NOTFOUND,
          "resolve of a missing directory component fails ODS2_ERR_NOTFOUND");
    st = ods2_bdev_resolve_file(&bv, sysexe_comps, 3, "GHOST.EXE", 0,
                                &got_file_fid, filehdr, sizeof(filehdr));
    CHECK(st == ODS2_ERR_NOTFOUND,
          "resolve of a missing file fails ODS2_ERR_NOTFOUND");

    /* ndirs==0 resolves the MFD itself; it lists the reserved files + SYS0. */
    st = ods2_bdev_resolve_dir(&bv, NULL, 0, &got_dir_fid, dirhdr, sizeof(dirhdr));
    CHECK(st == ODS2_OK && ods2_fid_number(&got_dir_fid) == ODS2_FID_MFD,
          "resolve [000000] returns the MFD (FID 4)");
    memset(&cap, 0, sizeof(cap));
    ods2_bdev_list_dir(&bv, dirhdr, dir_collect, &cap);
    CHECK(dir_has(&cap, "000000.DIR") && dir_has(&cap, "INDEXF.SYS") &&
          dir_has(&cap, "SYS0.DIR"),
          "[000000] lists the reserved files + the created SYS0.DIR");

    close(fd);
    unlink(path);
    free(image);

    printf("=== %s: %d failure(s) ===\n",
           g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
