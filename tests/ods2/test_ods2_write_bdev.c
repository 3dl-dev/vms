/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_write_bdev.c - Rule-9 proof for the BLOCK-DEVICE-BACKED genuine
 * ODS-2 WRITER (vms-6d3b, R2 of the real-ODS-2-runtime epic vms-5eb --
 * the WRITE counterpart to vms-6cb's block-backed READER, proven by
 * test_ods2_bdev.c).
 *
 * Opens a real loop-image fd (mkstemp + ftruncate, exactly what /dev/vms or
 * a losetup'd loop device presents to pread/pwrite), then drives the
 * ENTIRE writer surface directly against that fd:
 *   - ods2_wvolume_format_bdev()   (home block pair, reserved files, MFD)
 *   - ods2_wvolume_create_dir()    (a subdirectory)
 *   - ods2_wvolume_create_file()   (a VAR-record data file)
 *   - ods2_wvolume_dir_insert()    (linking both into their parent dirs)
 * -- NEVER via ods2_volume_format()/an in-memory image buffer. The volume
 * is then read back with the EXISTING, already-validated block-backed
 * READER (ods2_bdev_open/_read_header/_list_dir, ods2_bdev.c, vms-6cb) over
 * the SAME fd, proving:
 *   - the home block validates (both additive checksums + "DECFILE11B  "),
 *   - the FID chain is walkable (MFD -> OVMXWDIR -> HELLO.TXT),
 *   - HELLO.TXT's VAR-framed records decode back to the EXACT original text
 *     (ods2_var_records_decode() over blocks fetched via ods2_bdev_read_block()),
 *   - and, decisively, that writing the SAME volume in-memory
 *     (ods2_volume_format() + the same create_dir/create_file/dir_insert
 *     calls) produces a BYTE-IDENTICAL image -- the block-device-backed
 *     writer changes WHERE the bytes are committed, never WHAT they are.
 *
 * The write path never allocates a buffer sized to the whole volume: only
 * ODS2_BLOCK_SIZE-sized cache entries and pwrite() calls are used to reach
 * `fd`. See ods2.h's "BLOCK-DEVICE-BACKED WRITER" section for the design.
 */

/* mkstemp / pread / pwrite / ftruncate / lseek / off_t -- make this TU
 * self-sufficient regardless of the compiler's default -std extensions,
 * matching test_ods2_bdev.c's own convention. */
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

#define TOTAL_BLOCKS 800u    /* mirrors test_ods2_bdev.c's RX50 geometry */
#define MAXFILES     200u
#define BLK          ODS2_BLOCK_SIZE

static const char HELLO_DATA[] =
    "hello from the block-device-backed ODS-2 writer\n"
    "written one block at a time, via pwrite, over a real fd\n";

/* Open a fresh temp loop-image file sized to `total_blocks` (ftruncate --
 * sparse, no data written; a REAL disk device would already be that size).
 * Already unlinked so it disappears on close. Returns fd >= 0, or -1. */
static int open_loop_image(uint32_t total_blocks)
{
    char path[] = "/tmp/ods2_wbdev_XXXXXX";
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

/* Build the SAME volume (format + create_dir + create_file + dir_insert)
 * against a block-device-backed wvol on `fd`. */
static int build_via_bdev(int fd, ods2_fid_t *dir_fid, ods2_fid_t *file_fid)
{
    ods2_format_params_t params;
    ods2_wvolume_t wvol;
    ods2_status_t st;

    memset(&params, 0, sizeof(params));
    params.total_blocks = TOTAL_BLOCKS;
    params.maxfiles      = MAXFILES;
    params.volname        = "OVMXWDEV";

    st = ods2_wvolume_format_bdev(fd, 0 /* auto-detect via lseek */,
                                  &params, &wvol);
    CHECK_EQ(st, ODS2_OK, "ods2_wvolume_format_bdev");
    if (st != ODS2_OK)
        return 0;
    CHECK_EQ(wvol.is_bdev, 1, "wvol.is_bdev set");

    st = ods2_wvolume_create_dir(&wvol, "OVMXWDIR.DIR", 1, wvol.mfd_fid, dir_fid);
    CHECK_EQ(st, ODS2_OK, "bdev create_dir(OVMXWDIR.DIR)");
    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "OVMXWDIR.DIR", 1, *dir_fid);
    CHECK_EQ(st, ODS2_OK, "bdev dir_insert(MFD, OVMXWDIR.DIR)");

    st = ods2_wvolume_create_file(&wvol, "HELLO.TXT", 1,
                                  (const uint8_t *)HELLO_DATA,
                                  strlen(HELLO_DATA), *dir_fid, file_fid);
    CHECK_EQ(st, ODS2_OK, "bdev create_file(HELLO.TXT)");

    st = ods2_wvolume_dir_insert(&wvol, *dir_fid, "HELLO.TXT", 1, *file_fid);
    CHECK_EQ(st, ODS2_OK, "bdev dir_insert(OVMXWDIR, HELLO.TXT)");

    ods2_wvolume_close(&wvol);   /* best-effort final flush; already flushed
                                  * by each call above -- exercised here for
                                  * coverage of the close() path itself. */
    return g_failures == 0;
}

/* Build the SAME sequence of calls against an in-memory wvol, for the
 * byte-identical cross-check. */
static int build_via_memory(uint8_t *image, size_t image_len,
                            ods2_fid_t *dir_fid, ods2_fid_t *file_fid)
{
    ods2_format_params_t params;
    ods2_wvolume_t wvol;
    ods2_status_t st;

    memset(&params, 0, sizeof(params));
    params.total_blocks = TOTAL_BLOCKS;
    params.maxfiles      = MAXFILES;
    params.volname        = "OVMXWDEV";

    st = ods2_volume_format(image, image_len, &params, &wvol);
    CHECK_EQ(st, ODS2_OK, "ods2_volume_format (in-memory reference)");
    if (st != ODS2_OK)
        return 0;

    st = ods2_wvolume_create_dir(&wvol, "OVMXWDIR.DIR", 1, wvol.mfd_fid, dir_fid);
    CHECK_EQ(st, ODS2_OK, "memory create_dir(OVMXWDIR.DIR)");
    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "OVMXWDIR.DIR", 1, *dir_fid);
    CHECK_EQ(st, ODS2_OK, "memory dir_insert(MFD, OVMXWDIR.DIR)");

    st = ods2_wvolume_create_file(&wvol, "HELLO.TXT", 1,
                                  (const uint8_t *)HELLO_DATA,
                                  strlen(HELLO_DATA), *dir_fid, file_fid);
    CHECK_EQ(st, ODS2_OK, "memory create_file(HELLO.TXT)");

    st = ods2_wvolume_dir_insert(&wvol, *dir_fid, "HELLO.TXT", 1, *file_fid);
    CHECK_EQ(st, ODS2_OK, "memory dir_insert(OVMXWDIR, HELLO.TXT)");

    return g_failures == 0;
}

/* Directory-listing capture, mirroring test_ods2_bdev.c's helper. */
struct dir_entry_cap { char name[32]; uint16_t version; uint32_t fid_num; };
struct dir_cap { struct dir_entry_cap e[16]; int count; };

static int dir_collect(const char *name, unsigned name_len,
                       uint16_t version, const ods2_fid_t *fid, void *ctx)
{
    struct dir_cap *c = (struct dir_cap *)ctx;
    if (c->count < 16) {
        struct dir_entry_cap *e = &c->e[c->count];
        unsigned n = name_len < sizeof(e->name) - 1 ? name_len
                                                    : (unsigned)sizeof(e->name) - 1;
        memcpy(e->name, name, n);
        e->name[n] = '\0';
        e->version = version;
        e->fid_num = ods2_fid_number(fid);
    }
    c->count++;
    return 0;
}

static const struct dir_entry_cap *find_entry(const struct dir_cap *c,
                                              const char *name)
{
    int i;
    for (i = 0; i < c->count && i < 16; i++)
        if (strcmp(c->e[i].name, name) == 0)
            return &c->e[i];
    return NULL;
}

/* ods2_fh2_map_walk() callback: capture the FIRST retrieval-pointer extent
 * (the only shape ods2_wvolume_create_file() ever produces for a fresh,
 * un-fragmented file), then stop the walk. */
static int capture_first_extent_cb(const ods2_extent_t *ext, void *ctx)
{
    *(ods2_extent_t *)ctx = *ext;
    return 1;   /* stop after the first extent */
}

int main(void)
{
    ods2_fid_t dir_fid, file_fid;
    ods2_fid_t mem_dir_fid, mem_file_fid;
    static uint8_t ref_image[TOTAL_BLOCKS * BLK];
    ods2_bdev_t bv;
    ods2_status_t st;
    int fd;

    printf("=== ODS-2 block-device-backed WRITER (vms-6d3b) ===\n");

    /* ---- build the volume DIRECTLY against a real fd, block-by-block ---- */
    fd = open_loop_image(TOTAL_BLOCKS);
    if (fd < 0) {
        printf("FAIL: could not open the loop-image file\n");
        return 1;
    }
    if (!build_via_bdev(fd, &dir_fid, &file_fid)) {
        printf("FAIL: %d check(s) failed (bdev volume build aborted)\n",
               g_failures);
        return 1;
    }

    /* ---- read it back with the EXISTING, already-proven block-backed
     *      reader (vms-6cb) over the SAME fd ---- */
    st = ods2_bdev_open(&bv, fd, 0);
    CHECK_EQ(st, ODS2_OK, "ods2_bdev_open over the writer's own fd");
    CHECK_EQ(bv.nblocks, TOTAL_BLOCKS, "bdev nblocks == volume size");
    CHECK_EQ(bv.home.hm2_struclev, ODS2_STRUCLEV_V2, "home struclev 0x0201");
    CHECK(memcmp(bv.home.hm2_format, ODS2_FORMAT_STRING, ODS2_FORMAT_LEN) == 0,
          "home format == DECFILE11B (both checksums already validated by "
          "ods2_bdev_open)");
    CHECK(memcmp(bv.home.hm2_volname, "OVMXWDEV    ", 12) == 0, "home volname");

    /* MFD -> OVMXWDIR.DIR -> HELLO.TXT, walked via the block-backed reader */
    {
        uint8_t mfd[BLK];
        struct dir_cap cap;
        const struct dir_entry_cap *e;

        st = ods2_bdev_read_header(&bv, ODS2_FID_MFD, mfd, sizeof(mfd));
        CHECK_EQ(st, ODS2_OK, "bdev_read_header(MFD)");
        memset(&cap, 0, sizeof(cap));
        st = ods2_bdev_list_dir(&bv, mfd, dir_collect, &cap);
        CHECK_EQ(st, ODS2_OK, "bdev_list_dir(MFD)");
        CHECK_EQ(cap.count, 11, "MFD lists 11 entries (10 reserved + OVMXWDIR)");

        e = find_entry(&cap, "OVMXWDIR.DIR");
        CHECK(e != NULL, "OVMXWDIR.DIR listed in MFD");
        if (e) {
            CHECK_EQ(e->version, 1, "OVMXWDIR.DIR version == 1");
            CHECK_EQ(e->fid_num, ods2_fid_number(&dir_fid), "OVMXWDIR.DIR fid matches");
        }
    }

    {
        uint8_t dirhdr[BLK], filehdr[BLK];
        struct dir_cap cap;
        const struct dir_entry_cap *e;
        ods2_fh2_t parsed;

        st = ods2_bdev_read_header(&bv, ods2_fid_number(&dir_fid),
                                   dirhdr, sizeof(dirhdr));
        CHECK_EQ(st, ODS2_OK, "bdev_read_header(OVMXWDIR)");
        memset(&cap, 0, sizeof(cap));
        st = ods2_bdev_list_dir(&bv, dirhdr, dir_collect, &cap);
        CHECK_EQ(st, ODS2_OK, "bdev_list_dir(OVMXWDIR)");
        CHECK_EQ(cap.count, 1, "OVMXWDIR lists exactly HELLO.TXT");

        e = find_entry(&cap, "HELLO.TXT");
        CHECK(e != NULL, "HELLO.TXT listed (block device)");
        if (e) {
            CHECK_EQ(e->version, 1, "HELLO.TXT version == 1");
            CHECK_EQ(e->fid_num, ods2_fid_number(&file_fid), "HELLO.TXT fid matches");
        }

        /* ---- FH2 header + FM2 extent chain, validated field-by-field ---- */
        st = ods2_bdev_read_header(&bv, ods2_fid_number(&file_fid),
                                   filehdr, sizeof(filehdr));
        CHECK_EQ(st, ODS2_OK, "bdev_read_header(HELLO.TXT)");
        st = ods2_fh2_parse(filehdr, BLK, &parsed);
        CHECK_EQ(st, ODS2_OK, "fh2_parse(HELLO.TXT) -- checksum validates");
        CHECK_EQ(ods2_fid_number(&parsed.fh2_fid), ods2_fid_number(&file_fid),
                 "HELLO.TXT header self-reports its own FID");
        CHECK_EQ(parsed.fh2_fid.fid_seq, 1, "HELLO.TXT fh2_fid.seq == 1 (first generation)");
        CHECK_EQ(ods2_fid_number(&parsed.fh2_backlink), ods2_fid_number(&dir_fid),
                 "HELLO.TXT fh2_backlink == OVMXWDIR's FID");

        /* Walk the FM2 map: exactly one contiguous extent (create_file()
         * never fragments a fresh file), fetch its blocks via
         * ods2_bdev_read_block(), and decode the VAR-record stream. */
        {
            ods2_extent_t ext;
            unsigned n_ext = 0;
            uint8_t data[4 * BLK];
            uint32_t k;
            char text[512];
            size_t text_len = 0;
            size_t data_bytes;

            ext.lbn = 0;
            ext.count = 0;
            st = ods2_fh2_map_walk(filehdr, capture_first_extent_cb, &ext, &n_ext);
            CHECK_EQ(st, ODS2_OK, "fh2_map_walk(HELLO.TXT) captures extent");
            CHECK_EQ(n_ext, 1, "HELLO.TXT has exactly one FM2 extent");
            CHECK(ext.count >= 1, "captured extent has at least one block");

            CHECK(ext.count <= 4, "test buffer large enough for HELLO.TXT's extent");
            for (k = 0; k < ext.count && k < 4; k++) {
                st = ods2_bdev_read_block(&bv, ext.lbn + k, data + k * BLK, BLK);
                CHECK_EQ(st, ODS2_OK, "bdev_read_block(HELLO.TXT data)");
            }

            data_bytes = ods2_recattr_data_bytes(&parsed.fh2_recattr);
            CHECK(data_bytes > 0 && data_bytes <= sizeof(data),
                  "HELLO.TXT data_bytes in range");

            st = ods2_var_records_decode(data, data_bytes, text, sizeof(text),
                                         &text_len);
            CHECK_EQ(st, ODS2_OK, "ods2_var_records_decode(HELLO.TXT)");
            CHECK_EQ(text_len, strlen(HELLO_DATA), "decoded text length matches original");
            CHECK(text_len == strlen(HELLO_DATA) &&
                  memcmp(text, HELLO_DATA, text_len) == 0,
                  "decoded text is BYTE-EXACT vs what was written");
        }
    }

    close(fd);

    /* ---- decisive cross-check: the SAME writer calls, run in-memory,
     *      produce a BYTE-IDENTICAL volume -- the block-device-backed mode
     *      changes WHERE bytes are committed, never WHAT they are. ---- */
    {
        int fd2 = open_loop_image(TOTAL_BLOCKS);
        CHECK(fd2 >= 0, "second loop image opened for the identity re-check");
        if (fd2 >= 0) {
            ods2_fid_t d2, f2;
            int ok = build_via_bdev(fd2, &d2, &f2);
            CHECK(ok, "second bdev build (for identity re-check) succeeded");

            if (!build_via_memory(ref_image, sizeof(ref_image),
                                  &mem_dir_fid, &mem_file_fid)) {
                printf("FAIL: in-memory reference build failed\n");
            } else {
                uint32_t lbn;
                int mismatches = 0;
                for (lbn = 0; lbn < TOTAL_BLOCKS; lbn++) {
                    uint8_t blk[BLK];
                    ssize_t n;
                    off_t off = (off_t)lbn * BLK;
                    ssize_t done = 0;
                    while (done < BLK) {
                        n = pread(fd2, blk + done, BLK - done, off + done);
                        if (n <= 0) break;
                        done += n;
                    }
                    if (done != BLK ||
                        memcmp(blk, ref_image + (size_t)lbn * BLK, BLK) != 0)
                        mismatches++;
                }
                CHECK_EQ(mismatches, 0,
                         "block-device-backed writer output is BYTE-IDENTICAL "
                         "to the in-memory writer's own output");
                CHECK_EQ(ods2_fid_number(&d2), ods2_fid_number(&mem_dir_fid),
                         "dir FID identical between modes");
                CHECK_EQ(ods2_fid_number(&f2), ods2_fid_number(&mem_file_fid),
                         "file FID identical between modes");
            }
            close(fd2);
        }
    }

    /* ---- negative cases: honest failure, not silent success ---- */
    {
        ods2_format_params_t params;
        ods2_wvolume_t wvol;

        memset(&params, 0, sizeof(params));
        params.total_blocks = TOTAL_BLOCKS;
        params.maxfiles      = MAXFILES;
        params.volname        = "BAD";
        st = ods2_wvolume_format_bdev(-1, 0, &params, &wvol);
        CHECK_EQ(st, ODS2_ERR_ARGS, "bad fd rejected honestly");
    }

    if (g_failures == 0) {
        printf("PASS: block-device-backed writer produces a genuine, "
               "byte-identical-to-in-memory ODS-2 volume directly over a "
               "real fd, block-by-block via pwrite\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
