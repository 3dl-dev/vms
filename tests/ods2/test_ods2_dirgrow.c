/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_dirgrow.c - MULTI-BLOCK DIRECTORY GROWTH proof for the genuine
 * ODS-2 writer ([F17], increment 10, vms-1bd -- R8 of the real-ODS-2-runtime
 * epic vms-5eb).
 *
 * Builds a genuine ODS-2 volume with the SAME writer path that mounts clean
 * on a real VAX (ods2_volume_format() + create_dir/create_file/dir_insert),
 * then inserts MANY more files into one directory than fit in a single
 * 512-byte directory block -- forcing ods2_wvolume_dir_insert() to grow the
 * directory across multiple blocks + extend its FH2 map/recattr.
 *
 * Proven, per the vms-1bd done-condition:
 *   - the grown directory's FH2 header maps >= 2 data blocks (hiblk grew,
 *     efblk == hiblk + 1, ffbyte == 0 -- the [F15]/[F17] EOF-position rule);
 *   - the IN-MEMORY reader (ods2_volume_list_dir) lists ALL N entries with
 *     correct {version, fid};
 *   - R1's BLOCK-BACKED reader (ods2_bdev_list_dir, run over a real loop-image
 *     fd via pread) lists the SAME N entries -- byte-for-byte agreeing with
 *     the in-memory reader over the multi-block directory;
 *   - a created file's content in the grown directory round-trips through the
 *     RMS VAR-record framing ([F16]) read back BOTH via ods2_file_read_text()
 *     (in-memory) AND via the block-backed path (bdev_read_header +
 *     bdev_read_block + ods2_var_records_decode) -- i.e. `TYPE`-equivalent
 *     content is correct on a file living in a multi-block directory;
 *   - growing OVMXDIR did NOT corrupt the MFD (still lists its 11 entries);
 *   - the MFD itself grows past one block when enough top-level entries are
 *     inserted (exercises growth of a reserved directory too).
 *
 * Every fact is verified against the ods2.h on-disk structs / the genuine
 * reader -- never via POSIX stat()/opendir(). The only POSIX calls are the
 * raw block I/O laying + (via the reader) reading the loop image.
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
    }                                                                  \
} while (0)

#define CHECK_EQ(a, b, msg) do {                                       \
    long _va = (long)(a), _vb = (long)(b);                            \
    if (_va != _vb) {                                                  \
        printf("  FAIL: %s: got %ld, want %ld  (%s:%d)\n",           \
               (msg), _va, _vb, __FILE__, __LINE__);                   \
        g_failures++;                                                  \
    }                                                                  \
} while (0)

#define TOTAL_BLOCKS 800u    /* mirrors real_vax_ods2.dsk's RX50 geometry */
#define MAXFILES     200u
#define BLK          ODS2_BLOCK_SIZE

/* Enough files to force OVMXDIR past a single 512-byte directory block.
 * Each "FILEnn.TXT" record is 24 on-disk bytes (6-byte header + 10-byte
 * name + 8-byte value entry), ~21 fit per block, so 60 needs 3 blocks. */
#define NFILES       60u
/* Enough top-level dirs to force the MFD itself past one block (10 reserved
 * entries already present; "SUBDIRnn.DIR" records are 26 bytes each). */
#define NSUBDIRS     40u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ---- directory-listing capture (comparable between the two readers) ---- */

struct dir_entry_cap {
    char     name[32];
    uint16_t version;
    uint32_t fid_num;
};

struct dir_cap {
    struct dir_entry_cap e[128];
    int count;
    int overflow;
};

static int dir_collect(const char *name, unsigned name_len,
                       uint16_t version, const ods2_fid_t *fid, void *ctx)
{
    struct dir_cap *c = (struct dir_cap *)ctx;
    if (c->count < (int)(sizeof(c->e) / sizeof(c->e[0]))) {
        struct dir_entry_cap *e = &c->e[c->count];
        unsigned n = name_len < sizeof(e->name) - 1 ? name_len : sizeof(e->name) - 1;
        memcpy(e->name, name, n);
        e->name[n] = '\0';
        e->version = version;
        e->fid_num = ods2_fid_number(fid);
    } else {
        c->overflow = 1;
    }
    c->count++;
    return 0;
}

static const struct dir_entry_cap *find_entry(const struct dir_cap *c,
                                              const char *name)
{
    int i, cap = (int)(sizeof(c->e) / sizeof(c->e[0]));
    for (i = 0; i < c->count && i < cap; i++)
        if (strcmp(c->e[i].name, name) == 0)
            return &c->e[i];
    return NULL;
}

static int lay_image_file(const uint8_t *image, size_t image_len)
{
    char path[] = "/tmp/ods2_dirgrow_XXXXXX";
    int fd = mkstemp(path);
    size_t done = 0;

    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }
    unlink(path);

    while (done < image_len) {
        ssize_t n = write(fd, image + done, image_len - done);
        if (n <= 0) {
            perror("write");
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    return fd;
}

/* Content generator: unique, multi-word-length-varying single-line text per
 * file, so the VAR-record round-trip is exercised (not a constant). */
static size_t file_body(unsigned i, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "OVMX file number %u body text\n", i);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

int main(void)
{
    static uint8_t image[TOTAL_BLOCKS * BLK];
    ods2_format_params_t params;
    ods2_wvolume_t wvol;
    ods2_status_t st;
    ods2_fid_t ovmxdir_fid;
    ods2_fid_t file_fid[NFILES];
    char names[NFILES][16];
    unsigned i;
    int fd;
    uint32_t ovmxdir_hiblk = 0, ovmxdir_num;

    printf("=== ODS-2 multi-block directory growth (vms-1bd, [F17]) ===\n");

    memset(&params, 0, sizeof(params));
    params.total_blocks = TOTAL_BLOCKS;
    params.maxfiles     = MAXFILES;
    params.volname      = "OVMXGROW";

    st = ods2_volume_format(image, sizeof(image), &params, &wvol);
    CHECK_EQ(st, ODS2_OK, "ods2_volume_format");
    if (st != ODS2_OK) {
        printf("FAIL: %d check(s) failed (format aborted)\n", g_failures);
        return 1;
    }

    st = ods2_wvolume_create_dir(&wvol, "OVMXDIR.DIR", 1, wvol.mfd_fid,
                                 &ovmxdir_fid);
    CHECK_EQ(st, ODS2_OK, "create_dir(OVMXDIR.DIR)");
    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "OVMXDIR.DIR", 1,
                                 ovmxdir_fid);
    CHECK_EQ(st, ODS2_OK, "dir_insert(MFD, OVMXDIR.DIR)");
    ovmxdir_num = ods2_fid_number(&ovmxdir_fid);

    /* ---- create NFILES files in OVMXDIR, forcing multi-block growth ---- */
    for (i = 0; i < NFILES; i++) {
        char body[64];
        size_t blen = file_body(i, body, sizeof(body));
        snprintf(names[i], sizeof(names[i]), "FILE%02u.TXT", i);
        st = ods2_wvolume_create_file(&wvol, names[i], 1,
                                      (const uint8_t *)body, blen,
                                      ovmxdir_fid, &file_fid[i]);
        CHECK_EQ(st, ODS2_OK, "create_file(FILEnn.TXT)");
        st = ods2_wvolume_dir_insert(&wvol, ovmxdir_fid, names[i], 1,
                                     file_fid[i]);
        CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, FILEnn.TXT)");
    }

    /* ---- grow the MFD past one block too (top-level subdirectories) ---- */
    for (i = 0; i < NSUBDIRS; i++) {
        char dname[16];
        ods2_fid_t sub_fid;
        snprintf(dname, sizeof(dname), "SUBDIR%02u.DIR", i);
        st = ods2_wvolume_create_dir(&wvol, dname, 1, wvol.mfd_fid, &sub_fid);
        CHECK_EQ(st, ODS2_OK, "create_dir(SUBDIRnn.DIR)");
        st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, dname, 1, sub_fid);
        CHECK_EQ(st, ODS2_OK, "dir_insert(MFD, SUBDIRnn.DIR)");
    }

    /* Optional dump for a manual real-VAX lab-2 MOUNT/DIRECTORY cross-check. */
    {
        const char *dump = getenv("ODS2_WRITE_DUMP");
        if (dump && *dump) {
            FILE *f = fopen(dump, "wb");
            if (f) { fwrite(image, 1, sizeof(image), f); fclose(f);
                     printf("  (dumped image to %s)\n", dump); }
        }
    }

    /* ---- lay into a real loop-image fd for the block-backed reader ---- */
    fd = lay_image_file(image, sizeof(image));
    if (fd < 0) {
        printf("FAIL: could not lay the loop-image file\n");
        return 1;
    }

    {
        ods2_volume_t vol;
        ods2_bdev_t   bv;
        uint8_t hdr[BLK], bhdr[BLK];
        struct dir_cap mem_dc, bdev_dc, mfd_dc;

        st = ods2_volume_open(&vol, image, sizeof(image));
        CHECK_EQ(st, ODS2_OK, "ods2_volume_open (in-memory)");
        st = ods2_bdev_open(&bv, fd, 0);
        CHECK_EQ(st, ODS2_OK, "ods2_bdev_open (block device)");
        CHECK_EQ(bv.nblocks, TOTAL_BLOCKS, "bdev nblocks == volume size");

        /* ---- the grown OVMXDIR header maps >= 2 blocks ([F17] rule 3) ---- */
        st = ods2_volume_read_header(&vol, ovmxdir_num, hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "read_header(OVMXDIR)");
        if (st == ODS2_OK) {
            ods2_fh2_t parsed;
            st = ods2_fh2_parse(hdr, BLK, &parsed);
            CHECK_EQ(st, ODS2_OK, "fh2_parse(OVMXDIR)");
            if (st == ODS2_OK) {
                ovmxdir_hiblk = ods2_recattr_hiblk(&parsed.fh2_recattr);
                CHECK(ovmxdir_hiblk >= 2,
                      "OVMXDIR grew to >= 2 allocated blocks");
                CHECK_EQ(ods2_recattr_efblk(&parsed.fh2_recattr),
                         ovmxdir_hiblk + 1,
                         "OVMXDIR efblk == hiblk + 1 (EOF-position rule)");
                CHECK_EQ(parsed.fh2_recattr.fat_ffbyte, 0,
                         "OVMXDIR ffbyte == 0 (ends on a block boundary)");
                CHECK(parsed.fh2_map_inuse >= 2,
                      "OVMXDIR map area has >= 1 retrieval pointer");
            }
        }

        /* the block-backed reader reads the SAME grown header */
        st = ods2_bdev_read_header(&bv, ovmxdir_num, bhdr, sizeof(bhdr));
        CHECK_EQ(st, ODS2_OK, "bdev_read_header(OVMXDIR)");
        if (st == ODS2_OK)
            CHECK(memcmp(hdr, bhdr, BLK) == 0,
                  "bdev OVMXDIR header byte-identical to in-memory header");

        /* ---- in-memory reader lists ALL NFILES entries ---- */
        memset(&mem_dc, 0, sizeof(mem_dc));
        st = ods2_volume_list_dir(&vol, hdr, dir_collect, &mem_dc);
        CHECK_EQ(st, ODS2_OK, "list_dir(OVMXDIR, in-memory)");
        CHECK(!mem_dc.overflow, "in-memory capture did not overflow");
        CHECK_EQ(mem_dc.count, (int)NFILES,
                 "in-memory: OVMXDIR lists exactly NFILES entries");

        /* ---- block-backed reader lists the SAME NFILES entries ---- */
        memset(&bdev_dc, 0, sizeof(bdev_dc));
        st = ods2_bdev_list_dir(&bv, bhdr, dir_collect, &bdev_dc);
        CHECK_EQ(st, ODS2_OK, "bdev_list_dir(OVMXDIR)");
        CHECK(!bdev_dc.overflow, "bdev capture did not overflow");
        CHECK_EQ(bdev_dc.count, (int)NFILES,
                 "block-backed: OVMXDIR lists exactly NFILES entries");

        /* every file present, correct {version, fid}, and both readers agree */
        for (i = 0; i < NFILES; i++) {
            const struct dir_entry_cap *me = find_entry(&mem_dc, names[i]);
            const struct dir_entry_cap *be = find_entry(&bdev_dc, names[i]);
            CHECK(me != NULL, "in-memory: FILEnn.TXT listed");
            CHECK(be != NULL, "block-backed: FILEnn.TXT listed");
            if (me) {
                CHECK_EQ(me->version, 1, "FILEnn.TXT version");
                CHECK_EQ(me->fid_num, ods2_fid_number(&file_fid[i]),
                         "FILEnn.TXT fid");
            }
            if (me && be) {
                CHECK_EQ(be->version, me->version,
                         "readers agree on FILEnn.TXT version");
                CHECK_EQ(be->fid_num, me->fid_num,
                         "readers agree on FILEnn.TXT fid");
            }
        }

        /* names come back in ascending order across the WHOLE grown dir
         * ([F13] sort is global, not per-block) */
        for (i = 1; i < (unsigned)mem_dc.count; i++)
            CHECK(strcmp(mem_dc.e[i - 1].name, mem_dc.e[i].name) < 0,
                  "OVMXDIR entries globally ascending across blocks");

        /* ---- created-file content round-trips (VAR framing, [F16]) both
         *      in-memory AND via the block-backed path, for a file living in
         *      the multi-block directory. Spot-check a spread of files. ---- */
        for (i = 0; i < NFILES; i += 13) {
            uint8_t fhdr[BLK], fbhdr[BLK];
            char want[64], mem_text[128], bdev_text[128];
            size_t want_len = file_body(i, want, sizeof(want));
            size_t mlen = 0, blen = 0;
            uint32_t fnum = ods2_fid_number(&file_fid[i]);

            /* in-memory: ods2_file_read_text() -> ods2_var_records_decode() */
            st = ods2_volume_read_header(&vol, fnum, fhdr, sizeof(fhdr));
            CHECK_EQ(st, ODS2_OK, "read_header(FILEnn.TXT)");
            if (st == ODS2_OK) {
                st = ods2_file_read_text(&vol, fhdr, mem_text,
                                         sizeof(mem_text), &mlen);
                CHECK_EQ(st, ODS2_OK, "ods2_file_read_text(FILEnn.TXT)");
                if (st == ODS2_OK) {
                    CHECK_EQ(mlen, want_len, "in-memory decoded length");
                    CHECK(memcmp(mem_text, want, want_len) == 0,
                          "in-memory content round-trips via VAR framing");
                }
            }

            /* block-backed: bdev_read_header + bdev_read_block +
             * ods2_var_records_decode() -- the same VAR decoder a real VAX
             * RMS TYPE runs, over pread'd blocks. */
            st = ods2_bdev_read_header(&bv, fnum, fbhdr, sizeof(fbhdr));
            CHECK_EQ(st, ODS2_OK, "bdev_read_header(FILEnn.TXT)");
            if (st == ODS2_OK) {
                ods2_fh2_t fp;
                struct { ods2_extent_t ext[4]; unsigned n; } ec = { {{0,0}}, 0 };
                (void)ec;
                st = ods2_fh2_parse(fbhdr, BLK, &fp);
                CHECK_EQ(st, ODS2_OK, "fh2_parse(bdev FILEnn.TXT)");
                if (st == ODS2_OK) {
                    /* the file is a single-block VAR file; read its one data
                     * block and decode. Recover its LBN from the map. */
                    uint8_t dblk[BLK];
                    uint32_t data_lbn = 0;
                    unsigned mp = fbhdr[offsetof(ods2_fh2_t, fh2_mpoffset)] * 2u;
                    uint16_t w0 = rd16(fbhdr + mp), w1 = rd16(fbhdr + mp + 2);
                    data_lbn = ((uint32_t)((w0 >> 8) & 0x3F) << 16) | w1;
                    st = ods2_bdev_read_block(&bv, data_lbn, dblk, sizeof(dblk));
                    CHECK_EQ(st, ODS2_OK, "bdev_read_block(FILEnn.TXT data)");
                    if (st == ODS2_OK) {
                        size_t data_bytes =
                            ods2_recattr_data_bytes(&fp.fh2_recattr);
                        st = ods2_var_records_decode(dblk, data_bytes,
                                                     bdev_text,
                                                     sizeof(bdev_text), &blen);
                        CHECK_EQ(st, ODS2_OK,
                                 "ods2_var_records_decode(bdev FILEnn.TXT)");
                        if (st == ODS2_OK) {
                            CHECK_EQ(blen, want_len, "block-backed decoded length");
                            CHECK(memcmp(bdev_text, want, want_len) == 0,
                                  "block-backed content round-trips via VAR framing");
                        }
                    }
                }
            }
        }

        /* ---- growing OVMXDIR did not corrupt the (also grown) MFD ---- */
        st = ods2_volume_read_header(&vol, ODS2_FID_MFD, hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "read_header(MFD)");
        if (st == ODS2_OK) {
            ods2_fh2_t mparsed;
            memset(&mfd_dc, 0, sizeof(mfd_dc));
            st = ods2_volume_list_dir(&vol, hdr, dir_collect, &mfd_dc);
            CHECK_EQ(st, ODS2_OK, "list_dir(MFD)");
            /* 10 reserved + OVMXDIR + NSUBDIRS created dirs */
            CHECK_EQ(mfd_dc.count, (int)(ODS2_RESFILES + 1 + NSUBDIRS),
                     "MFD lists reserved + OVMXDIR + all subdirs");
            {
                static const char *resnames[ODS2_RESFILES] = {
                    "INDEXF.SYS", "BITMAP.SYS", "BADBLK.SYS", "000000.DIR",
                    "CORIMG.SYS", "VOLSET.SYS", "CONTIN.SYS", "BACKUP.SYS",
                    "BADLOG.SYS", "SECURITY.SYS",
                };
                uint32_t f;
                for (f = 1; f <= ODS2_RESFILES; f++)
                    CHECK(find_entry(&mfd_dc, resnames[f - 1]) != NULL,
                          "reserved file still listed in grown MFD");
            }
            CHECK(find_entry(&mfd_dc, "OVMXDIR.DIR") != NULL,
                  "OVMXDIR.DIR still listed in grown MFD");

            st = ods2_fh2_parse(hdr, BLK, &mparsed);
            if (st == ODS2_OK)
                CHECK(ods2_recattr_hiblk(&mparsed.fh2_recattr) >= 2,
                      "MFD itself grew past one block");

            /* block-backed reader agrees on the grown MFD too */
            {
                struct dir_cap mfd_bdev;
                st = ods2_bdev_read_header(&bv, ODS2_FID_MFD, bhdr, sizeof(bhdr));
                CHECK_EQ(st, ODS2_OK, "bdev_read_header(MFD)");
                if (st == ODS2_OK) {
                    memset(&mfd_bdev, 0, sizeof(mfd_bdev));
                    st = ods2_bdev_list_dir(&bv, bhdr, dir_collect, &mfd_bdev);
                    CHECK_EQ(st, ODS2_OK, "bdev_list_dir(MFD)");
                    CHECK_EQ(mfd_bdev.count, mfd_dc.count,
                             "block-backed MFD count == in-memory MFD count");
                }
            }
        }
    }

    close(fd);

    if (g_failures == 0) {
        printf("PASS: multi-block directory growth lists + reads back through "
               "both readers\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
