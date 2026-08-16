/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_wvolume_reopen_append.c - CODEC-LEVEL Rule-9 proof for the two
 * reusable block-device writer primitives introduced by #611 (vms-02e):
 *
 *   - ods2_wvolume_open_bdev()    reattach a writer to an EXISTING formatted
 *                                 volume, reconstructing the bump-allocator
 *                                 watermark (next free block / next free FID)
 *                                 from the on-disk home block + bitmaps.
 *   - ods2_wvolume_append_file()  append verbatim bytes to an EXISTING
 *                                 RFM=FIXED file (extend its FM2 extent map,
 *                                 update the FH2 EOF fields).
 *
 * These are genuine Files-11 codec capability (an OPERATOR.LOG-style append,
 * and reopening a volume writer across mount/unmount) that a kernel Files-11
 * ACP (epic vms-208) reuses. They previously had NO direct test -- they were
 * exercised only transitively through the userspace SYS$DISK adapter's
 * test_ods2_write_adapter.c, which is being deleted with the adapter. This
 * test drives them DIRECTLY against a raw ODS-2 block device (no adapter, no
 * vmsfs_volume table, no /vms path bridge), so codec coverage never drops.
 *
 * Flow: format a genuine volume over a loop-image fd, create a subdirectory
 * and two RFM=FIXED files (create_file_raw), record the writer's watermark,
 * close it. Then REOPEN with open_bdev and assert the reconstructed watermark
 * is byte-exact; append to one file; create a third file (proving allocation
 * resumes cleanly from the reconstructed watermark); close. Finally read the
 * whole volume back with the already-validated block-backed reader
 * (ods2_bdev_*) and assert: the appended file reads back original+appended
 * bytes byte-exact, the untouched file is intact, and the post-reopen file
 * is correct -- so the reopen neither corrupted prior state nor mis-placed
 * new state.
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

#define TOTAL_BLOCKS 800u    /* mirrors test_ods2_write_bdev.c's RX50 geometry */
#define MAXFILES     200u
#define BLK          ODS2_BLOCK_SIZE

/* Initial content of LOG.TXT (>512 bytes NOT required; a small fixed-format
 * payload). append_file() extends it verbatim. */
static const char LOG_INITIAL[] =
    "%%%% OPERATOR.LOG opened -- initial verbatim block written once\n";
static const char LOG_APPEND[] =
    "%%%% appended line 1 after reopen via ods2_wvolume_append_file\n"
    "%%%% appended line 2 -- extends the FM2 map + FH2 EOF fields\n";
static const char KEEP_DATA[] =
    "this file is written before the reopen and must survive it unchanged\n";
static const char NEW_DATA[] =
    "created AFTER open_bdev -- proves allocation resumes from the rebuilt "
    "watermark without colliding with the pre-close files\n";

/* Open a fresh temp loop-image sized to `total_blocks` (sparse, unlinked so it
 * disappears on close). Returns fd >= 0 or -1. */
static int open_loop_image(uint32_t total_blocks)
{
    char path[] = "/tmp/ods2_reopen_XXXXXX";
    int fd = mkstemp(path);

    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }
    unlink(path);
    if (ftruncate(fd, (off_t)total_blocks * BLK) != 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    return fd;
}

int main(void)
{
    ods2_fid_t dir_fid, log_fid, keep_fid, new_fid;
    uint32_t saved_next_lbn = 0, saved_next_fid = 0;
    ods2_status_t st;
    int fd;

    printf("=== ODS-2 codec: reopen (open_bdev) + append_file (#611) ===\n");

    fd = open_loop_image(TOTAL_BLOCKS);
    if (fd < 0) {
        printf("FAIL: could not open the loop-image file\n");
        return 1;
    }

    /* ---- phase 1: format + populate, then CLOSE the writer ---- */
    {
        ods2_format_params_t params;
        ods2_wvolume_t wvol;

        memset(&params, 0, sizeof(params));
        params.total_blocks = TOTAL_BLOCKS;
        params.maxfiles      = MAXFILES;
        params.volname        = "OVMXREOP";

        st = ods2_wvolume_format_bdev(fd, 0 /* auto-detect */, &params, &wvol);
        CHECK_EQ(st, ODS2_OK, "format_bdev");
        if (st != ODS2_OK) { close(fd); return 1; }

        st = ods2_wvolume_create_dir(&wvol, "OVMXDIR.DIR", 1, wvol.mfd_fid, &dir_fid);
        CHECK_EQ(st, ODS2_OK, "create_dir(OVMXDIR.DIR)");
        st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "OVMXDIR.DIR", 1, dir_fid);
        CHECK_EQ(st, ODS2_OK, "dir_insert(MFD, OVMXDIR.DIR)");

        /* RFM=FIXED (verbatim) files -- the shape append_file() extends. */
        st = ods2_wvolume_create_file_raw(&wvol, "LOG.TXT", 1,
                                          (const uint8_t *)LOG_INITIAL,
                                          strlen(LOG_INITIAL), dir_fid, &log_fid);
        CHECK_EQ(st, ODS2_OK, "create_file_raw(LOG.TXT)");
        st = ods2_wvolume_dir_insert(&wvol, dir_fid, "LOG.TXT", 1, log_fid);
        CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, LOG.TXT)");

        st = ods2_wvolume_create_file_raw(&wvol, "KEEP.TXT", 1,
                                          (const uint8_t *)KEEP_DATA,
                                          strlen(KEEP_DATA), dir_fid, &keep_fid);
        CHECK_EQ(st, ODS2_OK, "create_file_raw(KEEP.TXT)");
        st = ods2_wvolume_dir_insert(&wvol, dir_fid, "KEEP.TXT", 1, keep_fid);
        CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, KEEP.TXT)");

        /* Record the writer's watermark so the reopen can be checked exactly. */
        saved_next_lbn = wvol.next_free_lbn;
        saved_next_fid = wvol.next_free_fid;
        CHECK(saved_next_lbn > 0 && saved_next_fid > ODS2_FID_MFD,
              "watermark advanced past the reserved layout");

        ods2_wvolume_close(&wvol);   /* flush + free; fd stays open */
    }

    if (g_failures) {
        printf("FAIL: %d check(s) failed during phase 1 build\n", g_failures);
        close(fd);
        return 1;
    }

    /* ---- phase 2: REOPEN the volume for incremental write ---- */
    {
        ods2_wvolume_t w2;

        st = ods2_wvolume_open_bdev(fd, 0 /* auto-detect */, &w2);
        CHECK_EQ(st, ODS2_OK, "open_bdev(existing volume)");
        if (st != ODS2_OK) { close(fd); return 1; }
        CHECK_EQ(w2.is_bdev, 1, "reopened wvol is bdev mode");

        /* The decisive watermark check: open_bdev must reconstruct the SAME
         * next-free-block and next-free-FID the writer held at close, purely
         * from the on-disk home block + bitmaps. */
        CHECK_EQ(w2.next_free_lbn, saved_next_lbn,
                 "open_bdev rebuilt next_free_lbn byte-exact");
        CHECK_EQ(w2.next_free_fid, saved_next_fid,
                 "open_bdev rebuilt next_free_fid byte-exact");
        CHECK_EQ(ods2_fid_number(&w2.mfd_fid), ODS2_FID_MFD,
                 "open_bdev recovered the MFD FID");

        /* Append to the existing RFM=FIXED file. */
        st = ods2_wvolume_append_file(&w2, log_fid,
                                      (const uint8_t *)LOG_APPEND,
                                      strlen(LOG_APPEND));
        CHECK_EQ(st, ODS2_OK, "append_file(LOG.TXT)");

        /* Create a NEW file after reopen -- must land at the reconstructed
         * watermark FID and not collide with any pre-close file. */
        st = ods2_wvolume_create_file_raw(&w2, "NEW.TXT", 1,
                                          (const uint8_t *)NEW_DATA,
                                          strlen(NEW_DATA), dir_fid, &new_fid);
        CHECK_EQ(st, ODS2_OK, "create_file_raw(NEW.TXT) after reopen");
        CHECK_EQ(ods2_fid_number(&new_fid), saved_next_fid,
                 "post-reopen file took the reconstructed watermark FID");
        CHECK(ods2_fid_number(&new_fid) != ods2_fid_number(&log_fid) &&
              ods2_fid_number(&new_fid) != ods2_fid_number(&keep_fid),
              "post-reopen file FID does not collide with pre-close files");
        st = ods2_wvolume_dir_insert(&w2, dir_fid, "NEW.TXT", 1, new_fid);
        CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, NEW.TXT)");

        CHECK_EQ(w2.io_error, ODS2_OK, "no sticky I/O error after reopen writes");
        ods2_wvolume_close(&w2);
    }

    if (g_failures) {
        printf("FAIL: %d check(s) failed during phase 2 reopen/append\n",
               g_failures);
        close(fd);
        return 1;
    }

    /* ---- phase 3: read the whole volume back with the block-backed reader,
     *      confirming append correctness AND that nothing else was corrupted */
    {
        ods2_bdev_t bv;
        const char *comps[1] = { "OVMXDIR" };
        uint8_t hdr[BLK];
        uint8_t buf[8 * BLK];
        size_t got;

        st = ods2_bdev_open(&bv, fd, 0);
        CHECK_EQ(st, ODS2_OK, "bdev_open for read-back");

        /* LOG.TXT: original + appended, byte-exact. */
        st = ods2_bdev_resolve_file(&bv, comps, 1, "LOG.TXT", 0,
                                    NULL, hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "resolve_file(LOG.TXT)");
        got = 0;
        st = ods2_bdev_read_file(&bv, hdr, buf, sizeof(buf), &got);
        CHECK_EQ(st, ODS2_OK, "read_file(LOG.TXT)");
        CHECK_EQ(got, strlen(LOG_INITIAL) + strlen(LOG_APPEND),
                 "LOG.TXT length == initial + appended");
        CHECK(got == strlen(LOG_INITIAL) + strlen(LOG_APPEND) &&
              memcmp(buf, LOG_INITIAL, strlen(LOG_INITIAL)) == 0 &&
              memcmp(buf + strlen(LOG_INITIAL), LOG_APPEND,
                     strlen(LOG_APPEND)) == 0,
              "LOG.TXT content is original followed by appended, byte-exact");

        /* KEEP.TXT: untouched across the reopen. */
        st = ods2_bdev_resolve_file(&bv, comps, 1, "KEEP.TXT", 0,
                                    NULL, hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "resolve_file(KEEP.TXT)");
        got = 0;
        st = ods2_bdev_read_file(&bv, hdr, buf, sizeof(buf), &got);
        CHECK_EQ(st, ODS2_OK, "read_file(KEEP.TXT)");
        CHECK_EQ(got, strlen(KEEP_DATA), "KEEP.TXT length unchanged");
        CHECK(got == strlen(KEEP_DATA) &&
              memcmp(buf, KEEP_DATA, got) == 0,
              "KEEP.TXT content intact after the reopen+append+create");

        /* NEW.TXT: created after reopen, correct content. */
        st = ods2_bdev_resolve_file(&bv, comps, 1, "NEW.TXT", 0,
                                    NULL, hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "resolve_file(NEW.TXT)");
        got = 0;
        st = ods2_bdev_read_file(&bv, hdr, buf, sizeof(buf), &got);
        CHECK_EQ(st, ODS2_OK, "read_file(NEW.TXT)");
        CHECK_EQ(got, strlen(NEW_DATA), "NEW.TXT length matches");
        CHECK(got == strlen(NEW_DATA) && memcmp(buf, NEW_DATA, got) == 0,
              "NEW.TXT content byte-exact");
    }

    close(fd);

    /* ---- negative: open_bdev on a bad fd fails honestly (Rule 9 / INV-6) ---- */
    {
        ods2_wvolume_t bad;
        st = ods2_wvolume_open_bdev(-1, 0, &bad);
        CHECK(st != ODS2_OK, "open_bdev(-1) fails honestly, never fakes success");
    }

    if (g_failures == 0) {
        printf("PASS: open_bdev reconstructs the writer watermark byte-exact "
               "and append_file extends an existing RFM=FIXED file, verified "
               "directly against a raw ODS-2 block device (no adapter)\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
