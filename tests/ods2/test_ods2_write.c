/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_write.c - Round-trip test for the GENUINE ODS-2 WRITER
 * (increment 3 of the genuine-ODS-2 effort, src/vmsfs/ods2/ods2_writer.c).
 *
 * Builds a small volume entirely with ods2_volume_format() /
 * ods2_wvolume_create_dir() / ods2_wvolume_create_file() /
 * ods2_wvolume_dir_insert(), then reads it back with the SAME increment-1/2
 * reader (ods2_reader.c) that was already validated byte-for-byte against a
 * real OpenVMS VAX V7.3 volume (test_ods2_real.c). This proves the writer
 * and reader agree on every structure the writer touches: home block pair
 * (+checksums), SCB, the ten reserved-file headers, a created subdirectory,
 * two created data files, and directory-record insertion at two directory
 * levels ([000000] and the created subdirectory).
 *
 * This test does NOT by itself prove a real VAX can MOUNT the result --
 * that is the separate lab-2 cross-check described in the increment-3 task
 * (see docs/oracle or the PR description for the outcome). It DOES prove
 * the writer's output is internally self-consistent per the documented
 * Files-11 layout, using the same reader increment 2 already validated
 * against real bytes.
 *
 * Volume geometry (800 blocks / 10 sectors / 80 tracks / 1 cylinder)
 * deliberately mirrors tests/ods2/real_vax_ods2.dsk's RX50 geometry -- the
 * one already proven to work end-to-end on lab-2 for increment 2 -- so a
 * dumped image (ODS2_WRITE_DUMP env var) can be attached the same way.
 */

#include "vmsfs/ods2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        printf("  FAIL: %s: got %ld, want %ld  (%s:%d)\n",            \
               (msg), _va, _vb, __FILE__, __LINE__);                   \
    g_failures++;                                                      \
    }                                                                  \
} while (0)

#define TOTAL_BLOCKS 800u   /* mirrors real_vax_ods2.dsk's RX50 geometry */
#define MAXFILES     200u

struct extent_capture {
    ods2_extent_t ext[4];
    unsigned      n;
};

static int extent_collect(const ods2_extent_t *ext, void *ctx)
{
    struct extent_capture *c = (struct extent_capture *)ctx;
    if (c->n < 4)
        c->ext[c->n] = *ext;
    c->n++;
    return 0;
}

struct dir_entry_capture {
    char     name[32];
    uint16_t version;
    uint32_t fid_num;
};

struct dir_capture {
    struct dir_entry_capture entries[16]; /* MFD now holds 10 reserved-file
                                              entries (increment 4, [F8]) +
                                              caller-created ones */
    int count;
};

static int dir_collect(const char *name, unsigned name_len,
                       uint16_t version, const ods2_fid_t *fid, void *ctx)
{
    struct dir_capture *c = (struct dir_capture *)ctx;
    if (c->count < 16) {
        struct dir_entry_capture *e = &c->entries[c->count];
        unsigned n = name_len < sizeof(e->name) - 1 ? name_len : sizeof(e->name) - 1;
        memcpy(e->name, name, n);
        e->name[n] = '\0';
        e->version = version;
        e->fid_num = ods2_fid_number(fid);
    }
    c->count++;
    return 0;
}

static const struct dir_entry_capture *find_entry(const struct dir_capture *c,
                                                   const char *name)
{
    int i;
    for (i = 0; i < c->count && i < 16; i++)
        if (strcmp(c->entries[i].name, name) == 0)
            return &c->entries[i];
    return NULL;
}

int main(void)
{
    static uint8_t image[TOTAL_BLOCKS * ODS2_BLOCK_SIZE];
    ods2_format_params_t params;
    ods2_wvolume_t wvol;
    ods2_status_t st;
    ods2_fid_t ovmxdir_fid, hello_fid, world_fid;
    const char hello_data[] = "hello from the OVMX ODS-2 writer\n";
    const char world_data[] = "genuine Files-11 bytes\n";

    printf("=== ODS-2 writer round-trip (increment 3) ===\n");

    /* ---------------------------------------------------------------
     * Build the volume.
     * --------------------------------------------------------------- */
    memset(&params, 0, sizeof(params));
    params.total_blocks = TOTAL_BLOCKS;
    params.maxfiles     = MAXFILES;
    params.volname       = "OVMXWRIT";

    st = ods2_volume_format(image, sizeof(image), &params, &wvol);
    CHECK_EQ(st, ODS2_OK, "ods2_volume_format");
    if (st != ODS2_OK) {
        printf("FAIL: %d check(s) failed (format failed, aborting)\n", g_failures);
        return 1;
    }

    st = ods2_wvolume_create_dir(&wvol, "OVMXDIR.DIR", 1, wvol.mfd_fid, &ovmxdir_fid);
    CHECK_EQ(st, ODS2_OK, "create_dir(OVMXDIR.DIR)");

    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "OVMXDIR.DIR", 1, ovmxdir_fid);
    CHECK_EQ(st, ODS2_OK, "dir_insert(MFD, OVMXDIR.DIR)");

    st = ods2_wvolume_create_file(&wvol, "HELLO.TXT", 1,
                                  (const uint8_t *)hello_data,
                                  strlen(hello_data), ovmxdir_fid, &hello_fid);
    CHECK_EQ(st, ODS2_OK, "create_file(HELLO.TXT)");

    st = ods2_wvolume_create_file(&wvol, "WORLD.TXT", 1,
                                  (const uint8_t *)world_data,
                                  strlen(world_data), ovmxdir_fid, &world_fid);
    CHECK_EQ(st, ODS2_OK, "create_file(WORLD.TXT)");

    st = ods2_wvolume_dir_insert(&wvol, ovmxdir_fid, "HELLO.TXT", 1, hello_fid);
    CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, HELLO.TXT)");

    st = ods2_wvolume_dir_insert(&wvol, ovmxdir_fid, "WORLD.TXT", 1, world_fid);
    CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, WORLD.TXT)");

    /* Optional: dump the finished image to disk for a manual lab-2 MOUNT
     * cross-check (see PROVENANCE-real_vax_ods2.md / the increment-3 PR
     * description for how this was driven). Not used by ctest itself. */
    {
        const char *dump_path = getenv("ODS2_WRITE_DUMP");
        if (dump_path && *dump_path) {
            FILE *f = fopen(dump_path, "wb");
            if (f) {
                fwrite(image, 1, sizeof(image), f);
                fclose(f);
                printf("  (dumped image to %s)\n", dump_path);
            } else {
                printf("  WARN: could not open ODS2_WRITE_DUMP path %s\n", dump_path);
            }
        }
    }

    /* ---------------------------------------------------------------
     * Read it all back with the increment-1/2 reader.
     * --------------------------------------------------------------- */
    {
        ods2_volume_t vol;
        uint8_t hdr[ODS2_BLOCK_SIZE];
        ods2_home_t alt_home;
        struct extent_capture ec;
        struct dir_capture dc;
        const struct dir_entry_capture *e;

        st = ods2_volume_open(&vol, image, sizeof(image));
        CHECK_EQ(st, ODS2_OK, "ods2_volume_open(written image)");
        if (st == ODS2_OK) {
            CHECK_EQ(vol.home.hm2_struclev, ODS2_STRUCLEV_V2, "struclev");
            CHECK_EQ(vol.home.hm2_maxfiles, MAXFILES, "maxfiles");
            CHECK_EQ(vol.home.hm2_resfiles, ODS2_RESFILES, "resfiles == 10");
            CHECK(memcmp(vol.home.hm2_volname, "OVMXWRIT    ", 12) == 0, "volname");
            CHECK(memcmp(vol.home.hm2_format, ODS2_FORMAT_STRING, 12) == 0, "format");
        }

        /* alternate home block parses and validates independently */
        st = ods2_home_parse(image + 2 * ODS2_BLOCK_SIZE, ODS2_BLOCK_SIZE,
                             &alt_home, 1);
        CHECK_EQ(st, ODS2_OK, "alt home block parses+checksums");
        if (st == ODS2_OK)
            CHECK_EQ(alt_home.hm2_homelbn, 2, "alt home hm2_homelbn == 2 (self)");

        /* every reserved file's header reads and checksums */
        {
            static const char *names[ODS2_RESFILES] = {
                "INDEXF.SYS", "BITMAP.SYS", "BADBLK.SYS", "000000.DIR",
                "CORIMG.SYS", "VOLSET.SYS", "CONTIN.SYS", "BACKUP.SYS",
                "BADLOG.SYS", "SECURITY.SYS",
            };
            uint32_t fid;
            for (fid = 1; fid <= ODS2_RESFILES; fid++) {
                const ods2_ident_t *id;
                ods2_fh2_t parsed;
                st = ods2_volume_read_header(&vol, fid, hdr, sizeof(hdr));
                CHECK_EQ(st, ODS2_OK, "read_header(reserved file)");
                if (st != ODS2_OK)
                    continue;
                id = ods2_fh2_ident(hdr);
                CHECK(id != NULL, "reserved file ident non-NULL");
                if (id) {
                    size_t nlen = strlen(names[fid - 1]);
                    CHECK(memcmp(id->fi2_filename, names[fid - 1], nlen) == 0,
                          "reserved file name matches");
                }
                /* [F2] every reserved file backlinks to the MFD (FID 4),
                 * and fh2_fid.seq == its own fid_num -- see ods2.h. A real
                 * VAX MOUNT rejected an earlier writer draft that left
                 * backlink zero ("index file header is bad"). */
                st = ods2_fh2_parse(hdr, ODS2_BLOCK_SIZE, &parsed);
                CHECK_EQ(st, ODS2_OK, "fh2_parse(reserved file)");
                if (st == ODS2_OK) {
                    CHECK_EQ(parsed.fh2_fid.fid_seq, fid, "reserved file seq == fid_num");
                    CHECK_EQ(parsed.fh2_backlink.fid_num, ODS2_FID_MFD,
                             "reserved file backlink == MFD");
                    CHECK_EQ(parsed.fh2_backlink.fid_seq, ODS2_FID_MFD,
                             "reserved file backlink seq == MFD seq");
                }
                /* [F4] regression guard: fh2_acoffset must be the sentinel
                 * 255 ("no ACL area"), not == fh2_mpoffset. A real VAX
                 * MOUNT rejected every volume built with acoffset ==
                 * mpoffset -- bisected on lab-2 (see ods2.h / PROVENANCE
                 * increment-3 addendum). This is the actual bug that made
                 * a real MOUNT fail after every other field was already
                 * correct, so it gets its own explicit guard here. */
                CHECK_EQ(hdr[2] /* fh2_acoffset raw byte */, 255,
                         "reserved file acoffset == 255 sentinel");
            }
        }

        /* SECURITY.SYS's VBN1 data block (increment 4, [F6]) parses off
         * its own map-derived first extent LBN, checksums, and round-trips
         * the volume's own label + SYSTEM owner UIC. This is the writer's
         * actual integration path (ods2_volume_format() -> write_fh2_header
         * + ods2_security_build()), not just the standalone builder
         * exercised in test_ods2_security.c. */
        st = ods2_volume_read_header(&vol, ODS2_FID_SECURITY, hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "read_header(SECURITY.SYS)");
        if (st == ODS2_OK) {
            memset(&ec, 0, sizeof(ec));
            st = ods2_fh2_map_walk(hdr, extent_collect, &ec, NULL);
            CHECK_EQ(st, ODS2_OK, "map_walk(SECURITY.SYS)");
            CHECK_EQ(ec.n, 1, "SECURITY.SYS one extent");
            if (ec.n >= 1) {
                CHECK_EQ(ec.ext[0].count, ODS2_SECURITY_DATA_BLOCKS,
                         "SECURITY.SYS extent is 6 blocks");
                const uint8_t *sec_blk = ods2_volume_block(&vol, ec.ext[0].lbn);
                CHECK(sec_blk != NULL, "SECURITY.SYS VBN1 block in range");
                if (sec_blk) {
                    char label[16];
                    ods2_uic_t owner;
                    st = ods2_security_parse(sec_blk, ODS2_BLOCK_SIZE,
                                             label, sizeof(label), &owner);
                    CHECK_EQ(st, ODS2_OK, "ods2_security_parse(SECURITY.SYS VBN1)");
                    if (st == ODS2_OK) {
                        CHECK(strcmp(label, "OVMXWRIT") == 0,
                              "SECURITY.SYS label == volume label");
                        CHECK_EQ(owner.uic_member, 4, "SECURITY.SYS owner member == SYSTEM");
                        CHECK_EQ(owner.uic_group, 1, "SECURITY.SYS owner group == SYSTEM");
                    }
                }
            }
        }

        /* SCB parses off BITMAP.SYS's map-derived VBN1 */
        st = ods2_volume_read_header(&vol, ODS2_FID_BITMAP, hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "read_header(BITMAP.SYS)");
        if (st == ODS2_OK) {
            memset(&ec, 0, sizeof(ec));
            st = ods2_fh2_map_walk(hdr, extent_collect, &ec, NULL);
            CHECK_EQ(st, ODS2_OK, "map_walk(BITMAP.SYS)");
            CHECK_EQ(ec.n, 1, "BITMAP.SYS one extent");
            if (ec.n >= 1) {
                const uint8_t *scb_blk = ods2_volume_block(&vol, ec.ext[0].lbn);
                ods2_scb_t scb;
                CHECK(scb_blk != NULL, "SCB block in range");
                if (scb_blk) {
                    st = ods2_scb_parse(scb_blk, ODS2_BLOCK_SIZE, &scb);
                    CHECK_EQ(st, ODS2_OK, "ods2_scb_parse");
                    if (st == ODS2_OK) {
                        CHECK_EQ(scb.scb_struclev, ODS2_STRUCLEV_V2, "SCB struclev");
                        CHECK_EQ(scb.scb_cluster, 1, "SCB cluster");
                        CHECK_EQ(scb.scb_volsize, TOTAL_BLOCKS, "SCB volsize");
                    }
                }
            }
        }

        /* MFD lists the 10 reserved system files (increment 4, [F8] -- a
         * real VAX MOUNT rejected this writer's earlier output with
         * "%MOUNT-F-BADSECSYS -SYSTEM-W-FILENUMCHK" even with a byte-
         * identical-to-real SECURITY.SYS header+content, and bisection
         * traced it to these missing directory entries -- see ods2.h/
         * ods2_writer.c's [F8] provenance comment and PROVENANCE-
         * real_vax_ods2.md's increment-4 addendum) plus OVMXDIR.DIR. */
        st = ods2_volume_read_header(&vol, ODS2_FID_MFD, hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "read_header(MFD)");
        if (st == ODS2_OK) {
            memset(&dc, 0, sizeof(dc));
            st = ods2_volume_list_dir(&vol, hdr, dir_collect, &dc);
            CHECK_EQ(st, ODS2_OK, "list_dir(MFD)");
            CHECK_EQ(dc.count, 11, "MFD has exactly 11 entries (10 reserved + OVMXDIR.DIR)");

            {
                static const char *resnames[ODS2_RESFILES] = {
                    "INDEXF.SYS", "BITMAP.SYS", "BADBLK.SYS", "000000.DIR",
                    "CORIMG.SYS", "VOLSET.SYS", "CONTIN.SYS", "BACKUP.SYS",
                    "BADLOG.SYS", "SECURITY.SYS",
                };
                uint32_t fid;
                for (fid = 1; fid <= ODS2_RESFILES; fid++) {
                    e = find_entry(&dc, resnames[fid - 1]);
                    CHECK(e != NULL, "reserved file listed in MFD");
                    if (e) {
                        CHECK_EQ(e->version, 1, "reserved file dir version");
                        CHECK_EQ(e->fid_num, fid, "reserved file dir fid");
                    }
                }
            }

            e = find_entry(&dc, "OVMXDIR.DIR");
            CHECK(e != NULL, "OVMXDIR.DIR listed in MFD");
            if (e) {
                CHECK_EQ(e->version, 1, "OVMXDIR.DIR version");
                CHECK_EQ(e->fid_num, ods2_fid_number(&ovmxdir_fid), "OVMXDIR.DIR fid");
            }
        }

        /* OVMXDIR lists exactly HELLO.TXT and WORLD.TXT */
        st = ods2_volume_read_header(&vol, ods2_fid_number(&ovmxdir_fid),
                                     hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "read_header(OVMXDIR)");
        if (st == ODS2_OK) {
            const ods2_ident_t *id = ods2_fh2_ident(hdr);
            CHECK(id != NULL, "OVMXDIR ident non-NULL");
            if (id)
                CHECK(memcmp(id->fi2_filename, "OVMXDIR.DIR", 11) == 0,
                      "OVMXDIR ident name");
            {
                ods2_fh2_t parsed;
                st = ods2_fh2_parse(hdr, ODS2_BLOCK_SIZE, &parsed);
                CHECK_EQ(st, ODS2_OK, "OVMXDIR fh2_parse");
                if (st == ODS2_OK) {
                    CHECK_EQ(parsed.fh2_filechar &
                             (ODS2_FH2_M_DIRECTORY | ODS2_FH2_M_CONTIG),
                             ODS2_FH2_M_DIRECTORY | ODS2_FH2_M_CONTIG,
                             "OVMXDIR has DIRECTORY|CONTIG filechar bits");
                    CHECK_EQ(parsed.fh2_fid.fid_seq, 1, "OVMXDIR seq == 1");
                    CHECK_EQ(parsed.fh2_backlink.fid_num, ODS2_FID_MFD,
                             "OVMXDIR backlink == MFD");
                    /* [F10] regression guard (increment 6, vms-0f3): the
                     * backlink's SEQ must match what the MFD's OWN header
                     * actually self-declares (ODS2_FID_MFD, i.e. 4 -- see
                     * ods2_volume_format()'s write_fh2_header(ODS2_FID_MFD,
                     * ODS2_FID_MFD, ...) call), not the "first generation"
                     * seq==1 convention used for the file's OWN identity.
                     * wvol->mfd_fid previously carried seq==1 here, silently
                     * producing a backlink that never matched the real MFD
                     * header on disk -- exactly the FID self-consistency
                     * mismatch Nankervis's accesshead() (and real VMS's
                     * MOUNT-time FILENUMCHK) rejects. */
                    CHECK_EQ(parsed.fh2_backlink.fid_seq, ODS2_FID_MFD,
                             "OVMXDIR backlink seq == MFD's own fid_seq (4)");
                }
            }

            memset(&dc, 0, sizeof(dc));
            st = ods2_volume_list_dir(&vol, hdr, dir_collect, &dc);
            CHECK_EQ(st, ODS2_OK, "list_dir(OVMXDIR)");
            CHECK_EQ(dc.count, 2, "OVMXDIR has exactly 2 entries");

            e = find_entry(&dc, "HELLO.TXT");
            CHECK(e != NULL, "HELLO.TXT listed");
            if (e) {
                CHECK_EQ(e->version, 1, "HELLO.TXT version");
                CHECK_EQ(e->fid_num, ods2_fid_number(&hello_fid), "HELLO.TXT fid");
            }

            e = find_entry(&dc, "WORLD.TXT");
            CHECK(e != NULL, "WORLD.TXT listed");
            if (e) {
                CHECK_EQ(e->version, 1, "WORLD.TXT version");
                CHECK_EQ(e->fid_num, ods2_fid_number(&world_fid), "WORLD.TXT fid");
            }
        }

        /* HELLO.TXT / WORLD.TXT data round-trips byte-for-byte */
        st = ods2_volume_read_header(&vol, ods2_fid_number(&hello_fid),
                                     hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "read_header(HELLO.TXT)");
        if (st == ODS2_OK) {
            ods2_fh2_t parsed;
            memset(&ec, 0, sizeof(ec));
            st = ods2_fh2_map_walk(hdr, extent_collect, &ec, NULL);
            CHECK_EQ(st, ODS2_OK, "map_walk(HELLO.TXT)");
            CHECK_EQ(ec.n, 1, "HELLO.TXT one extent");
            if (ec.n >= 1) {
                const uint8_t *data = ods2_volume_block(&vol, ec.ext[0].lbn);
                CHECK(data != NULL, "HELLO.TXT data block in range");
                if (data)
                    CHECK(memcmp(data, hello_data, strlen(hello_data)) == 0,
                          "HELLO.TXT content round-trips");
            }
            st = ods2_fh2_parse(hdr, ODS2_BLOCK_SIZE, &parsed);
            CHECK_EQ(st, ODS2_OK, "fh2_parse(HELLO.TXT)");
            if (st == ODS2_OK)
                CHECK_EQ(parsed.fh2_backlink.fid_num, ods2_fid_number(&ovmxdir_fid),
                         "HELLO.TXT backlink == OVMXDIR");
        }

        st = ods2_volume_read_header(&vol, ods2_fid_number(&world_fid),
                                     hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "read_header(WORLD.TXT)");
        if (st == ODS2_OK) {
            memset(&ec, 0, sizeof(ec));
            st = ods2_fh2_map_walk(hdr, extent_collect, &ec, NULL);
            CHECK_EQ(st, ODS2_OK, "map_walk(WORLD.TXT)");
            CHECK_EQ(ec.n, 1, "WORLD.TXT one extent");
            if (ec.n >= 1) {
                const uint8_t *data = ods2_volume_block(&vol, ec.ext[0].lbn);
                CHECK(data != NULL, "WORLD.TXT data block in range");
                if (data)
                    CHECK(memcmp(data, world_data, strlen(world_data)) == 0,
                          "WORLD.TXT content round-trips");
            }
        }
    }

    /* ---------------------------------------------------------------
     * Negative case: allocator exhaustion is reported, not silently wrong.
     * --------------------------------------------------------------- */
    {
        uint32_t lbn;
        ods2_status_t nst = ods2_wvolume_alloc_blocks(&wvol, 1000000, &lbn);
        CHECK_EQ(nst, ODS2_ERR_NOSPACE, "huge alloc reports NOSPACE");
    }

    if (g_failures == 0) {
        printf("PASS: writer output round-trips through the increment-1/2 reader\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
