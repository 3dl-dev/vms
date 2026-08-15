/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_bdev.c - Rule-9 proof for the BLOCK-BACKED genuine ODS-2 reader
 * (vms-6cb, R1 of the real-ODS-2-runtime epic vms-5eb).
 *
 * Builds a genuine ODS-2 system-disk image with the SAME writer path that
 * mounts clean on a real VAX (ods2_volume_format() + create_dir/create_file/
 * dir_insert -- the [F15] path), lays it into a real on-disk loop IMAGE FILE
 * (an fd, exactly what /dev/vms or a losetup'd loop device presents to
 * pread), and then drives the block-backed reader (ods2_bdev_open/
 * _read_block/_read_header/_list_dir) over that fd.
 *
 * Every fact is verified FIELD-BY-FIELD against the ods2.h on-disk structs --
 * NEVER via POSIX stat()/opendir()/readdir(). The only POSIX calls here are
 * the raw block I/O that lays and (via the reader) reads the image: mkstemp,
 * write, pread (inside ods2_bdev), lseek, close, unlink.
 *
 * Proven, per the vms-6cb done-condition:
 *   - MFD          FID (4,4,0)      (num=4, seq=4, rvn=0), ident "000000.DIR"
 *   - INDEXF.SYS   FID 1  (seq 1),  ident "INDEXF.SYS"
 *   - BITMAP.SYS   FID 2  (seq 2),  ident "BITMAP.SYS"
 *   - a walkable FID chain WITH file versions (MFD -> OVMXDIR -> HELLO/WORLD)
 *   - the home block validates BOTH additive checksums + "DECFILE11B  "
 * and, decisively, that the block-backed reader returns results BYTE-FOR-BYTE
 * IDENTICAL to the in-memory reader over the same volume (every one of the
 * volume's blocks, every reserved header, and both directory listings).
 */

/* mkstemp / pread / write / lseek / off_t -- make this TU self-sufficient
 * regardless of the compiler's default -std extensions. */
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

/* ---- directory-listing capture (comparable between the two readers) ---- */

struct dir_entry_cap {
    char     name[32];
    uint16_t version;
    uint32_t fid_num;
};

struct dir_cap {
    struct dir_entry_cap e[16];
    int count;
};

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

/* Two captures are identical iff same count and same entry tuples in order. */
static int dir_cap_identical(const struct dir_cap *a, const struct dir_cap *b)
{
    int i, n;
    if (a->count != b->count)
        return 0;
    n = a->count < 16 ? a->count : 16;
    for (i = 0; i < n; i++) {
        if (strcmp(a->e[i].name, b->e[i].name) != 0) return 0;
        if (a->e[i].version != b->e[i].version)      return 0;
        if (a->e[i].fid_num != b->e[i].fid_num)      return 0;
    }
    return 1;
}

/* ---- little-endian scalar reads for the raw-block checksum re-derivation - */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ---------------------------------------------------------------- */

static int build_volume(uint8_t *image, size_t image_len,
                        ods2_fid_t *ovmxdir_fid,
                        ods2_fid_t *hello_fid, ods2_fid_t *world_fid)
{
    ods2_format_params_t params;
    ods2_wvolume_t wvol;
    ods2_status_t st;
    const char hello_data[] = "hello from the block-backed ODS-2 reader\n";
    const char world_data[] = "genuine Files-11 bytes over a real fd\n";

    memset(&params, 0, sizeof(params));
    params.total_blocks = TOTAL_BLOCKS;
    params.maxfiles     = MAXFILES;
    params.volname      = "OVMXBDEV";

    st = ods2_volume_format(image, image_len, &params, &wvol);
    CHECK_EQ(st, ODS2_OK, "ods2_volume_format");
    if (st != ODS2_OK)
        return 0;

    st = ods2_wvolume_create_dir(&wvol, "OVMXDIR.DIR", 1, wvol.mfd_fid, ovmxdir_fid);
    CHECK_EQ(st, ODS2_OK, "create_dir(OVMXDIR.DIR)");
    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "OVMXDIR.DIR", 1, *ovmxdir_fid);
    CHECK_EQ(st, ODS2_OK, "dir_insert(MFD, OVMXDIR.DIR)");

    st = ods2_wvolume_create_file(&wvol, "HELLO.TXT", 1,
                                  (const uint8_t *)hello_data,
                                  strlen(hello_data), *ovmxdir_fid, hello_fid);
    CHECK_EQ(st, ODS2_OK, "create_file(HELLO.TXT)");
    st = ods2_wvolume_create_file(&wvol, "WORLD.TXT", 2,
                                  (const uint8_t *)world_data,
                                  strlen(world_data), *ovmxdir_fid, world_fid);
    CHECK_EQ(st, ODS2_OK, "create_file(WORLD.TXT v2)");

    st = ods2_wvolume_dir_insert(&wvol, *ovmxdir_fid, "HELLO.TXT", 1, *hello_fid);
    CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, HELLO.TXT)");
    st = ods2_wvolume_dir_insert(&wvol, *ovmxdir_fid, "WORLD.TXT", 2, *world_fid);
    CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, WORLD.TXT v2)");

    return g_failures == 0;
}

/* Lay `image` into a fresh temp loop-image file; returns an fd open O_RDWR
 * (already unlinked so it disappears on close), or -1 on failure. */
static int lay_image_file(const uint8_t *image, size_t image_len)
{
    char path[] = "/tmp/ods2_bdev_XXXXXX";
    int fd = mkstemp(path);
    size_t done = 0;

    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }
    unlink(path);   /* keep only the open fd; auto-reclaimed on close */

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

int main(void)
{
    static uint8_t image[TOTAL_BLOCKS * BLK];
    ods2_fid_t ovmxdir_fid, hello_fid, world_fid;
    ods2_volume_t vol;                 /* in-memory reference reader        */
    ods2_bdev_t   bv;                  /* block-backed reader under test    */
    ods2_status_t st;
    int fd;

    printf("=== ODS-2 block-backed reader (vms-6cb) ===\n");

    /* ---- build a genuine volume + lay it into a real loop-image fd ---- */
    if (!build_volume(image, sizeof(image), &ovmxdir_fid, &hello_fid, &world_fid)) {
        printf("FAIL: %d check(s) failed (volume build aborted)\n", g_failures);
        return 1;
    }

    fd = lay_image_file(image, sizeof(image));
    if (fd < 0) {
        printf("FAIL: could not lay the loop-image file\n");
        return 1;
    }

    /* ---- open BOTH readers over the SAME volume ---- */
    st = ods2_volume_open(&vol, image, sizeof(image));
    CHECK_EQ(st, ODS2_OK, "ods2_volume_open (in-memory reference)");

    /* span_bytes == 0 -> auto-detect via lseek(SEEK_END) on the image file */
    st = ods2_bdev_open(&bv, fd, 0);
    CHECK_EQ(st, ODS2_OK, "ods2_bdev_open (block device, auto size)");
    CHECK_EQ(bv.nblocks, TOTAL_BLOCKS, "bdev nblocks == volume size");

    /* ---- home block: BOTH additive checksums + "DECFILE11B  " ---- */
    {
        uint8_t hb[BLK];
        uint16_t stored1, calc1, stored2, calc2;

        /* the parsed home block must be byte-identical between the two readers */
        CHECK(memcmp(&bv.home, &vol.home, sizeof(ods2_home_t)) == 0,
              "bdev home block identical to in-memory home block");

        CHECK_EQ(bv.home.hm2_struclev, ODS2_STRUCLEV_V2, "home struclev 0x0201");
        CHECK(memcmp(bv.home.hm2_format, ODS2_FORMAT_STRING, ODS2_FORMAT_LEN) == 0,
              "home format == DECFILE11B");
        CHECK(memcmp(bv.home.hm2_volname, "OVMXBDEV    ", 12) == 0, "home volname");

        /* Independently re-derive both additive checksums off the raw block
         * read back through the block device -- not just trust the parser. */
        st = ods2_bdev_read_block(&bv, 1, hb, sizeof(hb));
        CHECK_EQ(st, ODS2_OK, "bdev_read_block(home @LBN1)");
        stored1 = rd16(hb + offsetof(ods2_home_t, hm2_checksum1));
        calc1   = ods2_checksum(hb, 29);
        CHECK_EQ(stored1, calc1, "home checksum1 (first 29 words) validates");
        stored2 = rd16(hb + offsetof(ods2_home_t, hm2_checksum2));
        calc2   = ods2_block_checksum(hb);
        CHECK_EQ(stored2, calc2, "home checksum2 (all 255 words) validates");
        CHECK(memcmp(hb + offsetof(ods2_home_t, hm2_format),
                     ODS2_FORMAT_STRING, ODS2_FORMAT_LEN) == 0,
              "raw home block carries DECFILE11B format string");
    }

    /* ---- reserved-file headers: FID (4,4,0) MFD, FID 1 INDEXF, FID 2 BITMAP,
     *      each read via the block device, validated field-by-field vs ods2.h,
     *      and proven byte-identical to the in-memory reader's header. ---- */
    {
        static const struct { uint32_t fid; const char *name; } want[] = {
            { ODS2_FID_INDEXF, "INDEXF.SYS" },   /* FID 1, seq 1 */
            { ODS2_FID_BITMAP, "BITMAP.SYS" },   /* FID 2, seq 2 */
            { ODS2_FID_MFD,    "000000.DIR" },   /* FID 4, seq 4 (the (4,4,0)) */
        };
        unsigned i;
        for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
            uint8_t bhdr[BLK], mhdr[BLK];
            ods2_fh2_t parsed;
            const ods2_ident_t *id;

            st = ods2_bdev_read_header(&bv, want[i].fid, bhdr, sizeof(bhdr));
            CHECK_EQ(st, ODS2_OK, "bdev_read_header(reserved)");
            st = ods2_volume_read_header(&vol, want[i].fid, mhdr, sizeof(mhdr));
            CHECK_EQ(st, ODS2_OK, "in-memory read_header(reserved)");
            CHECK(memcmp(bhdr, mhdr, BLK) == 0,
                  "bdev header byte-identical to in-memory header");

            st = ods2_fh2_parse(bhdr, BLK, &parsed);
            CHECK_EQ(st, ODS2_OK, "fh2_parse(bdev header)");
            /* [F2] reserved files: fh2_fid.seq == own fid_num, rvn 0. */
            CHECK_EQ(ods2_fid_number(&parsed.fh2_fid), want[i].fid,
                     "reserved fh2_fid.num == fid");
            CHECK_EQ(parsed.fh2_fid.fid_seq, want[i].fid,
                     "reserved fh2_fid.seq == fid");
            CHECK_EQ(parsed.fh2_fid.fid_rvn, 0, "reserved fh2_fid.rvn == 0");

            id = ods2_fh2_ident(bhdr);
            CHECK(id != NULL, "reserved ident non-NULL");
            if (id)
                CHECK(memcmp(id->fi2_filename, want[i].name,
                             strlen(want[i].name)) == 0, "reserved ident name");
        }

        /* Spell out the headline (4,4,0) fact once, explicitly, off the MFD. */
        {
            uint8_t mfd[BLK];
            ods2_fh2_t p;
            st = ods2_bdev_read_header(&bv, ODS2_FID_MFD, mfd, sizeof(mfd));
            CHECK_EQ(st, ODS2_OK, "bdev_read_header(MFD)");
            st = ods2_fh2_parse(mfd, BLK, &p);
            CHECK_EQ(st, ODS2_OK, "fh2_parse(MFD)");
            CHECK(p.fh2_fid.fid_num == 4 && p.fh2_fid.fid_seq == 4 &&
                  p.fh2_fid.fid_rvn == 0, "MFD FID is (4,4,0)");
        }
    }

    /* ---- walkable FID chain WITH versions: MFD -> OVMXDIR -> {HELLO,WORLD}.
     *      Listed via the block device, and proven identical to in-memory. --- */
    {
        uint8_t bhdr[BLK], mhdr[BLK];
        struct dir_cap bcap, mcap;
        const struct dir_entry_cap *e;

        /* MFD listing (block device) vs MFD listing (in-memory) */
        st = ods2_bdev_read_header(&bv, ODS2_FID_MFD, bhdr, sizeof(bhdr));
        CHECK_EQ(st, ODS2_OK, "bdev_read_header(MFD) for list");
        memset(&bcap, 0, sizeof(bcap));
        st = ods2_bdev_list_dir(&bv, bhdr, dir_collect, &bcap);
        CHECK_EQ(st, ODS2_OK, "bdev_list_dir(MFD)");

        st = ods2_volume_read_header(&vol, ODS2_FID_MFD, mhdr, sizeof(mhdr));
        CHECK_EQ(st, ODS2_OK, "in-memory read_header(MFD) for list");
        memset(&mcap, 0, sizeof(mcap));
        st = ods2_volume_list_dir(&vol, mhdr, dir_collect, &mcap);
        CHECK_EQ(st, ODS2_OK, "in-memory list_dir(MFD)");

        CHECK(dir_cap_identical(&bcap, &mcap),
              "MFD listing identical: block device vs in-memory");
        CHECK_EQ(bcap.count, 11, "MFD lists 11 entries (10 reserved + OVMXDIR)");

        /* the FID chain's next link, WITH its version, read off the device */
        e = find_entry(&bcap, "OVMXDIR.DIR");
        CHECK(e != NULL, "OVMXDIR.DIR listed in MFD (block device)");
        if (e) {
            CHECK_EQ(e->version, 1, "OVMXDIR.DIR version == 1");
            CHECK_EQ(e->fid_num, ods2_fid_number(&ovmxdir_fid), "OVMXDIR.DIR fid");
        }
        /* a reserved entry carries a version too (INDEXF.SYS;1) */
        e = find_entry(&bcap, "INDEXF.SYS");
        CHECK(e != NULL, "INDEXF.SYS listed in MFD (block device)");
        if (e) {
            CHECK_EQ(e->version, 1, "INDEXF.SYS version == 1");
            CHECK_EQ(e->fid_num, ODS2_FID_INDEXF, "INDEXF.SYS fid == 1");
        }

        /* Follow the chain: read OVMXDIR's own header BY FID off the device,
         * list it, and confirm the two files with their DISTINCT versions. */
        st = ods2_bdev_read_header(&bv, ods2_fid_number(&ovmxdir_fid),
                                   bhdr, sizeof(bhdr));
        CHECK_EQ(st, ODS2_OK, "bdev_read_header(OVMXDIR)");
        st = ods2_volume_read_header(&vol, ods2_fid_number(&ovmxdir_fid),
                                     mhdr, sizeof(mhdr));
        CHECK_EQ(st, ODS2_OK, "in-memory read_header(OVMXDIR)");
        CHECK(memcmp(bhdr, mhdr, BLK) == 0, "OVMXDIR header byte-identical");

        memset(&bcap, 0, sizeof(bcap));
        st = ods2_bdev_list_dir(&bv, bhdr, dir_collect, &bcap);
        CHECK_EQ(st, ODS2_OK, "bdev_list_dir(OVMXDIR)");
        memset(&mcap, 0, sizeof(mcap));
        st = ods2_volume_list_dir(&vol, mhdr, dir_collect, &mcap);
        CHECK_EQ(st, ODS2_OK, "in-memory list_dir(OVMXDIR)");
        CHECK(dir_cap_identical(&bcap, &mcap),
              "OVMXDIR listing identical: block device vs in-memory");
        CHECK_EQ(bcap.count, 2, "OVMXDIR lists exactly HELLO.TXT + WORLD.TXT");

        e = find_entry(&bcap, "HELLO.TXT");
        CHECK(e != NULL, "HELLO.TXT listed (block device)");
        if (e) {
            CHECK_EQ(e->version, 1, "HELLO.TXT version == 1");
            CHECK_EQ(e->fid_num, ods2_fid_number(&hello_fid), "HELLO.TXT fid");
        }
        e = find_entry(&bcap, "WORLD.TXT");
        CHECK(e != NULL, "WORLD.TXT listed (block device)");
        if (e) {
            CHECK_EQ(e->version, 2, "WORLD.TXT version == 2 (distinct version)");
            CHECK_EQ(e->fid_num, ods2_fid_number(&world_fid), "WORLD.TXT fid");
        }
    }

    /* ---- decisive whole-volume identity sweep: every block read via the
     *      block device equals the same block of the in-memory image. ---- */
    {
        uint32_t lbn;
        int mismatches = 0;
        for (lbn = 0; lbn < TOTAL_BLOCKS; lbn++) {
            uint8_t blk[BLK];
            const uint8_t *mem;
            st = ods2_bdev_read_block(&bv, lbn, blk, sizeof(blk));
            if (st != ODS2_OK) { mismatches++; continue; }
            mem = ods2_volume_block(&vol, lbn);
            if (!mem || memcmp(blk, mem, BLK) != 0)
                mismatches++;
        }
        CHECK_EQ(mismatches, 0,
                 "all 800 blocks identical: block device vs in-memory image");
    }

    /* ---- negative cases: honest failure, not silent success ---- */
    {
        uint8_t blk[BLK];
        ods2_bdev_t nbv;
        /* out-of-range LBN rejected */
        st = ods2_bdev_read_block(&bv, TOTAL_BLOCKS, blk, sizeof(blk));
        CHECK_EQ(st, ODS2_ERR_RANGE, "out-of-range LBN rejected");
        /* undersized caller buffer rejected */
        st = ods2_bdev_read_block(&bv, 1, blk, 16);
        CHECK_EQ(st, ODS2_ERR_SIZE, "undersized read buffer rejected");
        /* opening a bad fd fails honestly */
        st = ods2_bdev_open(&nbv, -1, 0);
        CHECK_EQ(st, ODS2_ERR_ARGS, "bad fd rejected");
    }

    close(fd);

    if (g_failures == 0) {
        printf("PASS: block-backed reader matches the in-memory reader "
               "byte-for-byte over a real fd\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
