/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_multiversion.c - MULTI-VERSION DIRECTORY INSERT proof (vms-9794,
 * additive substrate fix for the ODS-2 flip vms-5eb).
 *
 * Before this change, ods2_wvolume_dir_insert() rejected ANY duplicate NAME
 * with ODS2_ERR_ARGS, so a checkin could never mint ";2"/";3" of an already-
 * present file. Real ODS-2 directory records carry a VERSION LIST per name
 * (each name's record holds multiple {version, FID} value entries, versions
 * descending) -- and the READER (ods2_dir_block_scan(), ods2_reader.c)
 * already walks that array as a `while (val_off + sizeof(ods2_dir_ent_t) <=
 * rec_end)` loop, i.e. it already expects zero or more entries per name.
 * This test proves the WRITER now emits what the reader already consumes.
 *
 * Builds a genuine ODS-2 volume DIRECTLY against a real fd (mkstemp +
 * ftruncate, exactly what /dev/vms or a losetup'd loop device presents to
 * pread/pwrite) via ods2_wvolume_format_bdev() + create_dir/create_file/
 * dir_insert -- never an in-memory-only image -- then reads it back with the
 * EXISTING, already-validated block-backed reader (ods2_bdev_open/_read_
 * header/_list_dir/_dir_find, ods2_bdev.c, vms-6cb). Proves:
 *
 *   - FOO.TXT;1, ;2, ;3 (three SEPARATE create_file() calls, three SEPARATE
 *     FIDs) all dir_insert() successfully into the SAME name -- the prior
 *     code returned ODS2_ERR_ARGS on the second and third calls;
 *   - the reader resolves want_version==1/2/3 to the correct, DISTINCT FID
 *     each time, and want_version==0 (highest) resolves to ;3;
 *   - each version's own content (independent VAR-record streams) round-
 *     trips correctly through its own FID -- i.e. the three versions are
 *     genuinely separate files, not aliases;
 *   - the record's value-entry array is in DESCENDING version order (3, 2,
 *     1) as ods2_dir_block_scan() walks it -- matching ods2.h's directory-
 *     record comment and this writer's own [F17] convention;
 *   - re-inserting an ALREADY-PRESENT {name, version} pair is REJECTED
 *     (ODS2_ERR_ARGS, fail-honest, never silently duplicated/overwritten);
 *   - NO REGRESSION: a name inserted exactly ONCE (BAR.TXT;1, the
 *     regression control) produces a directory record BYTE-IDENTICAL to the
 *     documented single-version layout (dir_verlimit == 0x7FFF for a
 *     caller-created file [F14], dir_flags == 0, one 8-byte value entry) --
 *     checked field-by-field against the ods2.h ods2_dir_rec_t/ods2_dir_ent_t
 *     layout, not against a re-derived/guessed shape.
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
    long _va = (long)(a), _vb = (long)(b);                             \
    if (_va != _vb) {                                                  \
        printf("  FAIL: %s: got %ld, want %ld  (%s:%d)\n",             \
               (msg), _va, _vb, __FILE__, __LINE__);                   \
        g_failures++;                                                  \
    }                                                                  \
} while (0)

#define TOTAL_BLOCKS 800u    /* mirrors test_ods2_write_bdev.c's geometry */
#define MAXFILES     200u
#define BLK          ODS2_BLOCK_SIZE

static const char FOO_V1[] = "FOO.TXT version one -- the original checkin\n";
static const char FOO_V2[] = "FOO.TXT version two -- a longer checked-in revision body\n";
static const char FOO_V3[] = "FOO.TXT v3, shortest of the three\n";
static const char BAR_V1[] = "BAR.TXT, inserted exactly once (regression control)\n";

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* Open a fresh temp loop-image file sized to `total_blocks`. Already
 * unlinked so it disappears on close. Returns fd >= 0, or -1. */
static int open_loop_image(uint32_t total_blocks)
{
    char path[] = "/tmp/ods2_mv_XXXXXX";
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

/* Directory-listing capture, ordered as the reader encounters entries (so
 * within-record value-entry ORDER is observable, not just membership). */
struct dir_entry_cap { char name[32]; uint16_t version; uint32_t fid_num; };
struct dir_cap { struct dir_entry_cap e[32]; int count; };

static int dir_collect(const char *name, unsigned name_len,
                       uint16_t version, const ods2_fid_t *fid, void *ctx)
{
    struct dir_cap *c = (struct dir_cap *)ctx;
    if (c->count < (int)(sizeof(c->e) / sizeof(c->e[0]))) {
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

/* Read a file's own content back via the block-backed reader (header ->
 * FM2 extent -> read_block -> VAR-record decode), mirroring what a real
 * RMS TYPE over /dev/vms would do. */
static ods2_status_t read_content(const ods2_bdev_t *bv, ods2_fid_t fid,
                                  char *out, size_t out_cap, size_t *out_len)
{
    uint8_t hdr[BLK];
    ods2_status_t st = ods2_bdev_read_header(bv, ods2_fid_number(&fid),
                                             hdr, sizeof(hdr));
    if (st != ODS2_OK)
        return st;
    return ods2_bdev_read_file_text(bv, hdr, out, out_cap, out_len);
}

int main(void)
{
    ods2_format_params_t params;
    ods2_wvolume_t wvol;
    ods2_status_t st;
    ods2_fid_t dir_fid, foo1, foo2, foo3, bar1, bogus;
    int fd;

    printf("=== ODS-2 multi-version dir_insert (vms-9794, [F17]) ===\n");

    fd = open_loop_image(TOTAL_BLOCKS);
    if (fd < 0) {
        printf("FAIL: could not open the loop-image file\n");
        return 1;
    }

    memset(&params, 0, sizeof(params));
    params.total_blocks = TOTAL_BLOCKS;
    params.maxfiles      = MAXFILES;
    params.volname        = "OVMXMVER";

    st = ods2_wvolume_format_bdev(fd, 0, &params, &wvol);
    CHECK_EQ(st, ODS2_OK, "ods2_wvolume_format_bdev");
    if (st != ODS2_OK) {
        printf("FAIL: %d check(s) failed (format aborted)\n", g_failures);
        return 1;
    }

    st = ods2_wvolume_create_dir(&wvol, "OVMXDIR.DIR", 1, wvol.mfd_fid, &dir_fid);
    CHECK_EQ(st, ODS2_OK, "create_dir(OVMXDIR.DIR)");
    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "OVMXDIR.DIR", 1, dir_fid);
    CHECK_EQ(st, ODS2_OK, "dir_insert(MFD, OVMXDIR.DIR)");

    /* ---- regression control: BAR.TXT inserted exactly once ---- */
    st = ods2_wvolume_create_file(&wvol, "BAR.TXT", 1,
                                  (const uint8_t *)BAR_V1, strlen(BAR_V1),
                                  dir_fid, &bar1);
    CHECK_EQ(st, ODS2_OK, "create_file(BAR.TXT;1)");
    st = ods2_wvolume_dir_insert(&wvol, dir_fid, "BAR.TXT", 1, bar1);
    CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, BAR.TXT;1)");

    /* ---- FOO.TXT;1, ;2, ;3: three SEPARATE files, SAME name ---- */
    st = ods2_wvolume_create_file(&wvol, "FOO.TXT", 1,
                                  (const uint8_t *)FOO_V1, strlen(FOO_V1),
                                  dir_fid, &foo1);
    CHECK_EQ(st, ODS2_OK, "create_file(FOO.TXT;1)");
    st = ods2_wvolume_dir_insert(&wvol, dir_fid, "FOO.TXT", 1, foo1);
    CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, FOO.TXT;1) -- first insert, must still succeed");

    st = ods2_wvolume_create_file(&wvol, "FOO.TXT", 2,
                                  (const uint8_t *)FOO_V2, strlen(FOO_V2),
                                  dir_fid, &foo2);
    CHECK_EQ(st, ODS2_OK, "create_file(FOO.TXT;2)");
    st = ods2_wvolume_dir_insert(&wvol, dir_fid, "FOO.TXT", 2, foo2);
    CHECK_EQ(st, ODS2_OK,
             "dir_insert(OVMXDIR, FOO.TXT;2) -- duplicate NAME must now MERGE, not reject");

    st = ods2_wvolume_create_file(&wvol, "FOO.TXT", 3,
                                  (const uint8_t *)FOO_V3, strlen(FOO_V3),
                                  dir_fid, &foo3);
    CHECK_EQ(st, ODS2_OK, "create_file(FOO.TXT;3)");
    st = ods2_wvolume_dir_insert(&wvol, dir_fid, "FOO.TXT", 3, foo3);
    CHECK_EQ(st, ODS2_OK, "dir_insert(OVMXDIR, FOO.TXT;3) -- third version merges too");

    CHECK(ods2_fid_number(&foo1) != ods2_fid_number(&foo2) &&
          ods2_fid_number(&foo2) != ods2_fid_number(&foo3) &&
          ods2_fid_number(&foo1) != ods2_fid_number(&foo3),
          "FOO.TXT;1/;2/;3 are three genuinely distinct FIDs");

    /* ---- fail-honest: re-inserting an ALREADY-PRESENT {name,version} is
     *      rejected, never silently duplicated/overwritten ---- */
    st = ods2_wvolume_create_file(&wvol, "BOGUS.TXT", 1, NULL, 0, dir_fid, &bogus);
    CHECK_EQ(st, ODS2_OK, "create_file(BOGUS.TXT) -- scratch FID for the dup-version probe");
    st = ods2_wvolume_dir_insert(&wvol, dir_fid, "FOO.TXT", 2, bogus);
    CHECK_EQ(st, ODS2_ERR_ARGS,
             "dir_insert(FOO.TXT;2 AGAIN) -- duplicate {name,version} still rejected");

    ods2_wvolume_close(&wvol);

    /* ================================================================
     * Read back with the GENUINE, already-validated block-backed reader.
     * ================================================================ */
    {
        ods2_bdev_t bv;
        uint8_t dirhdr[BLK];
        struct dir_cap cap;
        int i, foo_first = -1, foo_n = 0;
        uint16_t foo_versions[8];

        st = ods2_bdev_open(&bv, fd, 0);
        CHECK_EQ(st, ODS2_OK, "ods2_bdev_open over the writer's own fd");

        st = ods2_bdev_read_header(&bv, ods2_fid_number(&dir_fid),
                                   dirhdr, sizeof(dirhdr));
        CHECK_EQ(st, ODS2_OK, "bdev_read_header(OVMXDIR)");

        memset(&cap, 0, sizeof(cap));
        st = ods2_bdev_list_dir(&bv, dirhdr, dir_collect, &cap);
        CHECK_EQ(st, ODS2_OK, "bdev_list_dir(OVMXDIR)");
        CHECK_EQ(cap.count, 4,
                 "OVMXDIR lists exactly 4 {name,version} entries "
                 "(BAR.TXT;1, FOO.TXT;1/;2/;3)");

        /* ---- order: value entries within FOO.TXT's own record come back
         *      DESCENDING (3, 2, 1) -- [F17]'s stated convention ---- */
        for (i = 0; i < cap.count; i++) {
            if (strcmp(cap.e[i].name, "FOO.TXT") == 0) {
                if (foo_first < 0)
                    foo_first = i;
                if (foo_n < 8)
                    foo_versions[foo_n++] = cap.e[i].version;
            }
        }
        CHECK_EQ(foo_n, 3, "FOO.TXT contributes exactly 3 value entries");
        if (foo_n == 3) {
            CHECK_EQ(foo_versions[0], 3, "FOO.TXT value entries: [0] == ;3 (highest first)");
            CHECK_EQ(foo_versions[1], 2, "FOO.TXT value entries: [1] == ;2");
            CHECK_EQ(foo_versions[2], 1, "FOO.TXT value entries: [2] == ;1 (lowest last)");
        }

        /* ---- ods2_bdev_dir_find resolves each version to its own FID,
         *      and want_version==0 resolves to the HIGHEST (;3) ---- */
        {
            ods2_fid_t got;
            uint16_t got_ver;

            st = ods2_bdev_dir_find(&bv, dirhdr, "FOO.TXT", 1, &got, &got_ver);
            CHECK_EQ(st, ODS2_OK, "dir_find(FOO.TXT;1)");
            CHECK_EQ(got_ver, 1, "dir_find(FOO.TXT;1) version");
            CHECK_EQ(ods2_fid_number(&got), ods2_fid_number(&foo1), "dir_find(FOO.TXT;1) fid");

            st = ods2_bdev_dir_find(&bv, dirhdr, "FOO.TXT", 2, &got, &got_ver);
            CHECK_EQ(st, ODS2_OK, "dir_find(FOO.TXT;2)");
            CHECK_EQ(got_ver, 2, "dir_find(FOO.TXT;2) version");
            CHECK_EQ(ods2_fid_number(&got), ods2_fid_number(&foo2), "dir_find(FOO.TXT;2) fid");

            st = ods2_bdev_dir_find(&bv, dirhdr, "FOO.TXT", 3, &got, &got_ver);
            CHECK_EQ(st, ODS2_OK, "dir_find(FOO.TXT;3)");
            CHECK_EQ(got_ver, 3, "dir_find(FOO.TXT;3) version");
            CHECK_EQ(ods2_fid_number(&got), ods2_fid_number(&foo3), "dir_find(FOO.TXT;3) fid");

            st = ods2_bdev_dir_find(&bv, dirhdr, "FOO.TXT", 0, &got, &got_ver);
            CHECK_EQ(st, ODS2_OK, "dir_find(FOO.TXT, want_version=0 == highest)");
            CHECK_EQ(got_ver, 3, "dir_find(FOO.TXT, highest) resolves to ;3");
            CHECK_EQ(ods2_fid_number(&got), ods2_fid_number(&foo3),
                     "dir_find(FOO.TXT, highest) fid == ;3's fid");
        }

        /* ---- each version's OWN content round-trips through its OWN FID --
         *      i.e. the three versions are genuinely separate files ---- */
        {
            char text[256];
            size_t tlen;

            st = read_content(&bv, foo1, text, sizeof(text), &tlen);
            CHECK_EQ(st, ODS2_OK, "read FOO.TXT;1 content");
            CHECK(tlen == strlen(FOO_V1) && memcmp(text, FOO_V1, tlen) == 0,
                  "FOO.TXT;1 content is byte-exact");

            st = read_content(&bv, foo2, text, sizeof(text), &tlen);
            CHECK_EQ(st, ODS2_OK, "read FOO.TXT;2 content");
            CHECK(tlen == strlen(FOO_V2) && memcmp(text, FOO_V2, tlen) == 0,
                  "FOO.TXT;2 content is byte-exact");

            st = read_content(&bv, foo3, text, sizeof(text), &tlen);
            CHECK_EQ(st, ODS2_OK, "read FOO.TXT;3 content");
            CHECK(tlen == strlen(FOO_V3) && memcmp(text, FOO_V3, tlen) == 0,
                  "FOO.TXT;3 content is byte-exact");
        }

        /* ================================================================
         * NO REGRESSION: BAR.TXT's record (inserted exactly once) is
         * byte-identical to the documented single-version layout --
         * dir_size/dir_verlimit/dir_flags/dir_namecount/name/ONE value
         * entry, checked field-by-field against ods2.h's ods2_dir_rec_t /
         * ods2_dir_ent_t, not re-derived or guessed.
         * ================================================================ */
        {
            unsigned off, found = 0;
            const char *bar_name = "BAR.TXT";
            unsigned bar_nc = (unsigned)strlen(bar_name);

            /* Fetch OVMXDIR's data block via its FM2 map (the directory is
             * small, single-extent, single-block) -- same technique
             * test_ods2_dirgrow.c / test_ods2_write_bdev.c use to locate a
             * directory's/file's data from a parsed header. */
            {
                ods2_fh2_t parsed;
                unsigned mp;
                uint16_t w0, w1;
                uint32_t data_lbn;
                uint8_t dblk[BLK];

                st = ods2_fh2_parse(dirhdr, BLK, &parsed);
                CHECK_EQ(st, ODS2_OK, "fh2_parse(OVMXDIR) for regression check");
                mp = dirhdr[offsetof(ods2_fh2_t, fh2_mpoffset)] * 2u;
                w0 = rd16(dirhdr + mp);
                w1 = rd16(dirhdr + mp + 2);
                data_lbn = ((uint32_t)((w0 >> 8) & 0x3F) << 16) | w1;
                st = ods2_bdev_read_block(&bv, data_lbn, dblk, sizeof(dblk));
                CHECK_EQ(st, ODS2_OK, "bdev_read_block(OVMXDIR data)");

                for (off = 0; off + 6 <= BLK; ) {
                    uint16_t rec_size = rd16(dblk + off);
                    unsigned reclen, nc, val_off;

                    if (rec_size == ODS2_DIR_END)
                        break;
                    reclen = 2u + rec_size;
                    nc = dblk[off + 5];
                    if (nc == bar_nc &&
                        memcmp(dblk + off + 6, bar_name, bar_nc) == 0) {
                        uint8_t expect[32];
                        unsigned exp_len;

                        val_off = 6u + nc;
                        if (val_off & 1) val_off++;
                        exp_len = val_off + 8u;

                        /* Build the expected bytes independently from
                         * ods2.h's documented single-version layout: one
                         * dir_rec (dir_size, dir_verlimit == [F14]'s
                         * ODS2_DIR_VERLIMIT_DEFAULT for a caller-created
                         * file, dir_flags == 0, name) + exactly ONE
                         * dir_ent value entry. */
                        memset(expect, 0xFF, sizeof(expect));
                        put16(expect + 0, (uint16_t)(exp_len - 2));
                        put16(expect + 2, ODS2_DIR_VERLIMIT_DEFAULT);
                        expect[4] = 0;
                        expect[5] = (uint8_t)nc;
                        memcpy(expect + 6, bar_name, nc);
                        put16(expect + val_off + 0, 1);          /* dir_version */
                        put16(expect + val_off + 2, bar1.fid_num);
                        put16(expect + val_off + 4, bar1.fid_seq);
                        expect[val_off + 6] = bar1.fid_rvn;
                        expect[val_off + 7] = bar1.fid_nmx;

                        CHECK_EQ(reclen, exp_len,
                                 "BAR.TXT record length matches single-version layout");
                        CHECK(reclen == exp_len &&
                              memcmp(dblk + off, expect, exp_len) == 0,
                              "BAR.TXT record is BYTE-IDENTICAL to the documented "
                              "single-version layout (no regression)");
                        found = 1;
                        break;
                    }
                    off += reclen;
                }
                CHECK(found, "BAR.TXT record located in OVMXDIR's data block");
            }
        }
    }

    close(fd);

    if (g_failures == 0) {
        printf("PASS: ;1/;2/;3 round-trip via the genuine reader (correct, "
               "distinct FIDs + content, descending order), duplicate-"
               "version re-insert rejected, single-version insert "
               "byte-identical to today\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
