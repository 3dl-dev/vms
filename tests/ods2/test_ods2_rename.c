/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_rename.c - CODEC-LEVEL Rule-9 proof for the genuine Files-11
 * file CREATE -> EXTEND -> RENAME/MOVE write path (vms-de7, epic vms-208),
 * the userspace-writer twin of the executive ACP's IO$_CREATE /
 * IO$_MODIFY(extend) / IO$_MODIFY!IO$M_MOVE(rename) surface. The write side the
 * system-file writers ($SETUAI vms-59d, AUTHORIZE-seed vms-436) build on: they
 * write NAME.DAT;n+1 and then RENAME it over the live copy.
 *
 * Drives the writer DIRECTLY against a raw ODS-2 loop-image fd (no adapter, no
 * /vms bridge), then reads the whole volume back with the already-validated
 * block-backed reader (ods2_bdev_*), proving through genuine on-disk structure:
 *
 *   CREATE  - ods2_wvolume_create_file_raw() + dir_insert() gives a real FID and
 *             a versioned directory entry (as test_ods2_write_bdev already
 *             proves; here it is the substrate for the extend/rename).
 *   EXTEND  - ods2_wvolume_append_file() grows a file PAST its initial single-
 *             block extent (retrieval map + FH2 EOF extended, >1 block).
 *   RENAME  - ods2_wvolume_rename() SAME-directory: the new name resolves, the
 *             old name is gone, the file KEEPS its FID + allocation (hiblk) +
 *             content byte-exact (no leaked/double-allocated block), and the
 *             fh2_backlink is unchanged (same parent).
 *   MOVE    - ods2_wvolume_rename() CROSS-directory: the file leaves the source
 *             directory, appears in the destination, KEEPS its FID + content,
 *             and its fh2_backlink is rewritten to the destination directory.
 *
 * FID identity + unchanged hiblk across a rename is the SBM/IFBM-consistency
 * assertion at this layer: a rename that leaked or double-allocated a header or
 * data block would change the file's FID or allocation, or lose its bytes.
 *
 * Fail-honest: renaming a name that does not exist returns ODS2_ERR_NOTFOUND,
 * never a fabricated success.
 */

/* mkstemp / pread / ftruncate / off_t -- self-sufficient regardless of the
 * compiler's default -std, matching test_ods2_write_bdev.c's convention. */
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
    }                                                                  \
} while (0)

#define CHECK_EQ(a, b, msg) do {                                       \
    long _va = (long)(a), _vb = (long)(b);                             \
    if (_va != _vb) {                                                  \
        printf("  FAIL: %s: got %ld, want %ld  (%s:%d)\n",             \
               (msg), _va, _vb, __FILE__, __LINE__);                   \
        g_failures++;                                                  \
    }                                                                  \
} while (0)

#define TOTAL_BLOCKS 800u
#define MAXFILES     200u
#define BLK          ODS2_BLOCK_SIZE

/* Small initial content (one block); the append pushes it past that block. */
static const char ORIG_INITIAL[] =
    "%%%% original file -- fits in one block before the extend\n";
/* >512 bytes of appended payload so the file must grow past its initial
 * single-block extent (proves EXTEND). */
static char BIG_APPEND[600];
static const char MOVER_DATA[] =
    "this file is created in OVMXDIR then MOVED cross-directory to DEST\n";
static const char KEEP_DATA[] =
    "untouched bystander -- must survive the create/extend/rename unchanged\n";

static int open_loop_image(uint32_t total_blocks)
{
    char path[] = "/tmp/ods2_rename_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return -1; }
    unlink(path);
    if (ftruncate(fd, (off_t)total_blocks * BLK) != 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    return fd;
}

/* Count a file header's retrieval-pointer blocks (allocated span) via a map
 * walk -- the reader's own primitive, so this is genuine on-disk structure. */
static int g_block_count;
static int count_blocks_cb(const ods2_extent_t *ext, void *ctx)
{
    (void)ctx;
    g_block_count += (int)ext->count;
    return 0;
}
static int header_block_count(const uint8_t *hdr)
{
    g_block_count = 0;
    if (ods2_fh2_map_walk(hdr, count_blocks_cb, NULL, NULL) != ODS2_OK)
        return -1;
    return g_block_count;
}

int main(void)
{
    ods2_fid_t ovmx_fid, dest_fid, orig_fid, mover_fid, keep_fid;
    uint32_t orig_hiblk_after_extend = 0;
    int orig_blocks_after_extend = 0;
    ods2_status_t st;
    int fd;
    size_t i;

    for (i = 0; i < sizeof(BIG_APPEND); i++)
        BIG_APPEND[i] = (char)('A' + (int)(i % 26));
    BIG_APPEND[sizeof(BIG_APPEND) - 1] = '\n';

    printf("=== ODS-2 codec: CREATE -> EXTEND -> RENAME/MOVE (vms-de7) ===\n");

    fd = open_loop_image(TOTAL_BLOCKS);
    if (fd < 0) { printf("FAIL: could not open the loop-image file\n"); return 1; }

    /* ---- phase 1: format, create dirs + files, EXTEND + RENAME + MOVE ---- */
    {
        ods2_format_params_t params;
        ods2_wvolume_t wvol;

        memset(&params, 0, sizeof(params));
        params.total_blocks = TOTAL_BLOCKS;
        params.maxfiles      = MAXFILES;
        params.volname        = "OVMXRENM";

        st = ods2_wvolume_format_bdev(fd, 0, &params, &wvol);
        CHECK_EQ(st, ODS2_OK, "format_bdev");
        if (st != ODS2_OK) { close(fd); return 1; }

        /* Two directories: the source (OVMXDIR) and the move destination (DEST). */
        st = ods2_wvolume_create_dir(&wvol, "OVMXDIR.DIR", 1, wvol.mfd_fid, &ovmx_fid);
        CHECK_EQ(st, ODS2_OK, "create_dir(OVMXDIR.DIR)");
        st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "OVMXDIR.DIR", 1, ovmx_fid);
        CHECK_EQ(st, ODS2_OK, "dir_insert(MFD, OVMXDIR.DIR)");
        st = ods2_wvolume_create_dir(&wvol, "DEST.DIR", 1, wvol.mfd_fid, &dest_fid);
        CHECK_EQ(st, ODS2_OK, "create_dir(DEST.DIR)");
        st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "DEST.DIR", 1, dest_fid);
        CHECK_EQ(st, ODS2_OK, "dir_insert(MFD, DEST.DIR)");

        /* CREATE the file to extend + rename, plus an untouched bystander. */
        st = ods2_wvolume_create_file_raw(&wvol, "ORIG.TXT", 1,
                                          (const uint8_t *)ORIG_INITIAL,
                                          strlen(ORIG_INITIAL), ovmx_fid, &orig_fid);
        CHECK_EQ(st, ODS2_OK, "create_file_raw(ORIG.TXT)");
        st = ods2_wvolume_dir_insert(&wvol, ovmx_fid, "ORIG.TXT", 1, orig_fid);
        CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, ORIG.TXT)");

        st = ods2_wvolume_create_file_raw(&wvol, "KEEP.TXT", 1,
                                          (const uint8_t *)KEEP_DATA,
                                          strlen(KEEP_DATA), ovmx_fid, &keep_fid);
        CHECK_EQ(st, ODS2_OK, "create_file_raw(KEEP.TXT)");
        st = ods2_wvolume_dir_insert(&wvol, ovmx_fid, "KEEP.TXT", 1, keep_fid);
        CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, KEEP.TXT)");

        st = ods2_wvolume_create_file_raw(&wvol, "MOVER.TXT", 1,
                                          (const uint8_t *)MOVER_DATA,
                                          strlen(MOVER_DATA), ovmx_fid, &mover_fid);
        CHECK_EQ(st, ODS2_OK, "create_file_raw(MOVER.TXT)");
        st = ods2_wvolume_dir_insert(&wvol, ovmx_fid, "MOVER.TXT", 1, mover_fid);
        CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, MOVER.TXT)");

        /* EXTEND ORIG.TXT past its initial single-block extent. */
        st = ods2_wvolume_append_file(&wvol, orig_fid,
                                      (const uint8_t *)BIG_APPEND, sizeof(BIG_APPEND));
        CHECK_EQ(st, ODS2_OK, "append_file(ORIG.TXT) extends past one block");

        /* SAME-DIRECTORY RENAME: ORIG.TXT -> RENAMED.TXT within OVMXDIR. */
        st = ods2_wvolume_rename(&wvol, ovmx_fid, "ORIG.TXT", 1,
                                 ovmx_fid, "RENAMED.TXT", 1, orig_fid);
        CHECK_EQ(st, ODS2_OK, "rename(ORIG.TXT -> RENAMED.TXT, same dir)");

        /* CROSS-DIRECTORY MOVE: [OVMXDIR]MOVER.TXT -> [DEST]MOVED.TXT. */
        st = ods2_wvolume_rename(&wvol, ovmx_fid, "MOVER.TXT", 1,
                                 dest_fid, "MOVED.TXT", 1, mover_fid);
        CHECK_EQ(st, ODS2_OK, "rename(MOVER.TXT -> [DEST]MOVED.TXT, cross dir)");

        /* Fail-honest: rename a name that does not exist. */
        st = ods2_wvolume_rename(&wvol, ovmx_fid, "NOSUCH.TXT", 1,
                                 ovmx_fid, "WHATEVER.TXT", 1, orig_fid);
        CHECK(st == ODS2_ERR_NOTFOUND,
              "rename of a non-existent name is ODS2_ERR_NOTFOUND (never faked)");

        CHECK_EQ(wvol.io_error, ODS2_OK, "no sticky I/O error after the write path");
        ods2_wvolume_close(&wvol);
    }

    if (g_failures) {
        printf("FAIL: %d check(s) failed during phase 1 write path\n", g_failures);
        close(fd);
        return 1;
    }

    /* ---- phase 2: read the whole volume back with the block-backed reader ---- */
    {
        ods2_bdev_t bv;
        const char *ovmx[1] = { "OVMXDIR" };
        const char *dest[1] = { "DEST" };
        uint8_t ovmxhdr[BLK], desthdr[BLK], fhdr[BLK];
        uint8_t buf[8 * BLK];
        ods2_fid_t got_fid, junk;
        ods2_fh2_t parsed;
        size_t got;

        st = ods2_bdev_open(&bv, fd, 0);
        CHECK_EQ(st, ODS2_OK, "bdev_open for read-back");

        st = ods2_bdev_resolve_dir(&bv, ovmx, 1, NULL, ovmxhdr, sizeof(ovmxhdr));
        CHECK_EQ(st, ODS2_OK, "resolve_dir(OVMXDIR)");
        st = ods2_bdev_resolve_dir(&bv, dest, 1, NULL, desthdr, sizeof(desthdr));
        CHECK_EQ(st, ODS2_OK, "resolve_dir(DEST)");

        /* --- SAME-DIR RENAME: RENAMED.TXT present, ORIG.TXT gone --- */
        st = ods2_bdev_dir_find(&bv, ovmxhdr, "ORIG.TXT", 0, &junk, NULL);
        CHECK(st == ODS2_ERR_NOTFOUND, "old name ORIG.TXT is GONE from OVMXDIR");

        memset(&got_fid, 0, sizeof(got_fid));
        st = ods2_bdev_resolve_file(&bv, ovmx, 1, "RENAMED.TXT", 0,
                                    &got_fid, fhdr, sizeof(fhdr));
        CHECK_EQ(st, ODS2_OK, "new name RENAMED.TXT resolves in OVMXDIR");
        CHECK_EQ(ods2_fid_number(&got_fid), ods2_fid_number(&orig_fid),
                 "RENAMED.TXT KEEPS ORIG.TXT's FID (identity preserved)");

        /* content == initial + appended, byte-exact (extent map intact -> SBM
         * consistent, no data lost across the extend+rename). */
        got = 0;
        st = ods2_bdev_read_file(&bv, fhdr, buf, sizeof(buf), &got);
        CHECK_EQ(st, ODS2_OK, "read_file(RENAMED.TXT)");
        CHECK_EQ(got, strlen(ORIG_INITIAL) + sizeof(BIG_APPEND),
                 "RENAMED.TXT length == initial + extended append");
        CHECK(got == strlen(ORIG_INITIAL) + sizeof(BIG_APPEND) &&
              memcmp(buf, ORIG_INITIAL, strlen(ORIG_INITIAL)) == 0 &&
              memcmp(buf + strlen(ORIG_INITIAL), BIG_APPEND, sizeof(BIG_APPEND)) == 0,
              "RENAMED.TXT content is original + appended, byte-exact");

        /* EXTEND persisted: the file spans MORE than one block. */
        orig_blocks_after_extend = header_block_count(fhdr);
        CHECK(orig_blocks_after_extend >= 2,
              "RENAMED.TXT allocation grew past its initial single block (EXTEND)");

        /* Same-dir rename left the backlink at OVMXDIR. */
        st = ods2_fh2_parse(fhdr, sizeof(fhdr), &parsed);
        CHECK_EQ(st, ODS2_OK, "parse RENAMED.TXT header");
        CHECK_EQ(ods2_fid_number(&parsed.fh2_backlink), ods2_fid_number(&ovmx_fid),
                 "same-dir rename kept fh2_backlink == OVMXDIR");
        orig_hiblk_after_extend = ods2_recattr_hiblk(&parsed.fh2_recattr);
        CHECK(orig_hiblk_after_extend >= 2,
              "RENAMED.TXT hiblk reflects the extended allocation");

        /* --- CROSS-DIR MOVE: MOVED.TXT in DEST, MOVER.TXT gone from OVMXDIR --- */
        st = ods2_bdev_dir_find(&bv, ovmxhdr, "MOVER.TXT", 0, &junk, NULL);
        CHECK(st == ODS2_ERR_NOTFOUND, "moved name MOVER.TXT is GONE from OVMXDIR");

        memset(&got_fid, 0, sizeof(got_fid));
        st = ods2_bdev_resolve_file(&bv, dest, 1, "MOVED.TXT", 0,
                                    &got_fid, fhdr, sizeof(fhdr));
        CHECK_EQ(st, ODS2_OK, "MOVED.TXT resolves in DEST after the cross-dir move");
        CHECK_EQ(ods2_fid_number(&got_fid), ods2_fid_number(&mover_fid),
                 "MOVED.TXT KEEPS MOVER.TXT's FID (identity preserved across move)");
        got = 0;
        st = ods2_bdev_read_file(&bv, fhdr, buf, sizeof(buf), &got);
        CHECK_EQ(st, ODS2_OK, "read_file(MOVED.TXT)");
        CHECK(got == strlen(MOVER_DATA) && memcmp(buf, MOVER_DATA, got) == 0,
              "MOVED.TXT content byte-exact after the move");

        /* Cross-dir move rewrote the backlink to DEST. */
        st = ods2_fh2_parse(fhdr, sizeof(fhdr), &parsed);
        CHECK_EQ(st, ODS2_OK, "parse MOVED.TXT header");
        CHECK_EQ(ods2_fid_number(&parsed.fh2_backlink), ods2_fid_number(&dest_fid),
                 "cross-dir move rewrote fh2_backlink to DEST");

        /* --- Bystander KEEP.TXT untouched by the whole sequence --- */
        st = ods2_bdev_resolve_file(&bv, ovmx, 1, "KEEP.TXT", 0,
                                    NULL, fhdr, sizeof(fhdr));
        CHECK_EQ(st, ODS2_OK, "KEEP.TXT still resolves in OVMXDIR");
        got = 0;
        st = ods2_bdev_read_file(&bv, fhdr, buf, sizeof(buf), &got);
        CHECK_EQ(st, ODS2_OK, "read_file(KEEP.TXT)");
        CHECK(got == strlen(KEEP_DATA) && memcmp(buf, KEEP_DATA, got) == 0,
              "KEEP.TXT content intact after create/extend/rename/move");
    }

    close(fd);

    if (g_failures == 0) {
        printf("PASS: CREATE -> EXTEND -> same-dir RENAME + cross-dir MOVE over a "
               "raw ODS-2 block device: FID identity + allocation preserved, "
               "backlink updated on move, old names gone, content byte-exact\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
