/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_longname.c - Rule-9 proof for the ODS-2 WRITER's long-filename
 * IDENT-area encoding (vms-88d, R6-build unblocker for the ODS-2 flip
 * vms-5eb).
 *
 * WHAT THIS PROVES, AND WHY. write_fh2_header_ext() (ods2_writer.c) used to
 * cap the "NAME.TYPE;VERSION" ident text at 20 chars (ODS2_ERR_ARGS beyond
 * that), because fi2_filename ([N] ods2_ident_t, ods2.h) is only 20 bytes.
 * The real distro tree this writer must lay down for a bootable ODS-2
 * system disk contains names like "LIBVMSPROCESS$SHR.EXE;1" (23 chars),
 * which the cap rejected outright.
 *
 * The fix splits any ident text over 20 chars across BOTH ident fields:
 * fi2_filename[20] (offset 0) gets the first 20 chars, fi2_filenamext[66]
 * (offset 54) gets the continuation (chars 21..86), per the struct's
 * documented "name.type;ver continuation" semantics. This test:
 *
 *   1. Creates a file with a 23-char ident text via the block-device-backed
 *      writer, then inspects the raw on-disk IDENT area (via
 *      ods2_fh2_ident() over a block fetched with ods2_bdev_read_header())
 *      to assert the split is byte-EXACT: fi2_filename == first 20 chars
 *      (fully filled, no padding needed), fi2_filenamext == the 3-char
 *      continuation left-justified and space-padded to fill the remaining
 *      63 bytes.
 *   2. Resolves the file by its full "NAME.TYPE" + version through
 *      ods2_bdev_resolve_file() and reads its content back via
 *      ods2_bdev_read_file(), proving the long name round-trips through
 *      the SAME path DCL/RMS would use -- not just the raw header bytes.
 *   3. Regression-guards the <=20-char case at the exact 20-char boundary
 *      (idbuf fully fills fi2_filename, fi2_filenamext untouched/zero) and
 *      a short, ordinary name -- both must be BYTE-IDENTICAL to the
 *      pre-fix encoding (fi2_filenamext all zero, never space-padded when
 *      unused).
 */

#define _POSIX_C_SOURCE 200809L

#include "vmsfs/ods2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, msg) do {                                          \
    if (!(cond)) {                                                     \
        printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
        g_failures++;                                                  \
    } else {                                                           \
        printf("  PASS: %s\n", (msg));                                 \
    }                                                                  \
} while (0)

#define CHECK_EQ(a, b, msg) do {                                       \
    long _va = (long)(a), _vb = (long)(b);                             \
    if (_va != _vb) {                                                  \
        printf("  FAIL: %s: got %ld, want %ld  (%s:%d)\n",             \
               (msg), _va, _vb, __FILE__, __LINE__);                   \
        g_failures++;                                                  \
    } else {                                                           \
        printf("  PASS: %s\n", (msg));                                 \
    }                                                                  \
} while (0)

#define TOTAL_BLOCKS 800u
#define MAXFILES     200u
#define BLK          ODS2_BLOCK_SIZE

/* Open a fresh, unlinked temp loop-image file, ftruncate'd to size --
 * matches test_ods2_write_bdev.c's helper exactly. */
static int open_loop_image(uint32_t total_blocks)
{
    char path[] = "/tmp/ods2_longname_XXXXXX";
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

/* Create `name`;`version` in `dir_fid` (already-created), returning its
 * FID via *fid_out. Small fixed content, RFM=FIXED (create_file_raw) so
 * the content bytes are stored verbatim -- irrelevant to the ident-area
 * fix under test, but exercises the SAME path the R6 boot-image builder
 * uses for .EXE files. */
static int create_named_file(ods2_wvolume_t *wvol, const char *name,
                             uint16_t version, ods2_fid_t parent_dir,
                             ods2_fid_t *fid_out)
{
    static const uint8_t content[] = "long-name IDENT split content\n";
    ods2_status_t st;

    st = ods2_wvolume_create_file_raw(wvol, name, version, content,
                                      sizeof(content) - 1, parent_dir,
                                      fid_out);
    CHECK_EQ(st, ODS2_OK, "create_file_raw()");
    if (st != ODS2_OK)
        return 0;

    st = ods2_wvolume_dir_insert(wvol, parent_dir, name, version, *fid_out);
    CHECK_EQ(st, ODS2_OK, "dir_insert()");
    return st == ODS2_OK;
}

/* Fetch fidnum's raw header block via the block-backed reader and hand
 * back its ods2_ident_t* (points into `hdr_out`, which must outlive use). */
static const ods2_ident_t *fetch_ident(const ods2_bdev_t *bv, uint32_t fidnum,
                                       uint8_t *hdr_out)
{
    ods2_status_t st = ods2_bdev_read_header(bv, fidnum, hdr_out, BLK);
    CHECK_EQ(st, ODS2_OK, "bdev_read_header() for ident inspection");
    if (st != ODS2_OK)
        return NULL;
    return ods2_fh2_ident(hdr_out);
}

int main(void)
{
    ods2_format_params_t params;
    ods2_wvolume_t wvol;
    ods2_bdev_t bv;
    ods2_status_t st;
    int fd;
    ods2_fid_t dir_fid;
    ods2_fid_t long_fid, exact20_fid, short_fid;

    /* 23-char ident text: "LIBVMSPROCESS$SHR.EXE;1" -- the exact name from
     * the real distro tree that motivated vms-88d. base=21 chars, ";1"=2,
     * total=23 > 20. */
    static const char *const LONG_NAME = "LIBVMSPROCESS$SHR.EXE";
    static const char LONG_IDENT[]     = "LIBVMSPROCESS$SHR.EXE;1";
    static const char LONG_FIRST20[20] = "LIBVMSPROCESS$SHR.EX";
    static const char LONG_EXT3[3]     = "E;1";

    /* Exactly 20 chars total ("EIGHTEENCHARNAME18;1") -- the boundary
     * case: fi2_filename is COMPLETELY filled with no padding needed,
     * fi2_filenamext must stay untouched/zero (n is not > 20). Declared
     * WITHOUT an explicit array size, and WITHOUT a trailing NUL check
     * below (memcmp only, never strlen()) -- a [20]-sized array holding a
     * literal that is exactly 20 chars long has no room for the string's
     * NUL terminator, which is legal C but makes strlen() undefined. */
    static const char *const EXACT20_NAME  = "EIGHTEENCHARNAME18";
    static const char EXACT20_IDENT[]      = "EIGHTEENCHARNAME18;1";

    /* Ordinary short name, well under the cap -- the plain regression
     * case matching what every other ods2 test already exercises. */
    static const char *const SHORT_NAME = "HELLO.TXT";

    printf("=== ODS-2 writer long-filename IDENT split (vms-88d) ===\n");

    CHECK_EQ(strlen(LONG_IDENT), 23, "LONG_IDENT premise: 23 chars");
    CHECK_EQ(strlen(EXACT20_IDENT), 20, "EXACT20_IDENT premise: exactly 20 chars");
    CHECK_EQ(sizeof(EXACT20_IDENT) - 1, 20, "EXACT20_IDENT array premise: 20 chars + NUL");

    fd = open_loop_image(TOTAL_BLOCKS);
    if (fd < 0) {
        printf("FAIL: could not open the loop-image file\n");
        return 1;
    }

    memset(&params, 0, sizeof(params));
    params.total_blocks = TOTAL_BLOCKS;
    params.maxfiles      = MAXFILES;
    params.volname        = "OVMXLNAM";

    st = ods2_wvolume_format_bdev(fd, 0, &params, &wvol);
    CHECK_EQ(st, ODS2_OK, "ods2_wvolume_format_bdev");
    if (st != ODS2_OK) {
        close(fd);
        return 1;
    }

    st = ods2_wvolume_create_dir(&wvol, "OVMXLDIR.DIR", 1, wvol.mfd_fid, &dir_fid);
    CHECK_EQ(st, ODS2_OK, "create_dir(OVMXLDIR.DIR)");
    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "OVMXLDIR.DIR", 1, dir_fid);
    CHECK_EQ(st, ODS2_OK, "dir_insert(MFD, OVMXLDIR.DIR)");

    if (!create_named_file(&wvol, LONG_NAME, 1, dir_fid, &long_fid) ||
        !create_named_file(&wvol, EXACT20_NAME, 1, dir_fid, &exact20_fid) ||
        !create_named_file(&wvol, SHORT_NAME, 1, dir_fid, &short_fid)) {
        printf("FAIL: file creation aborted (%d failures so far)\n", g_failures);
        close(fd);
        return 1;
    }

    ods2_wvolume_close(&wvol);

    st = ods2_bdev_open(&bv, fd, 0);
    CHECK_EQ(st, ODS2_OK, "ods2_bdev_open");

    /* ---- 1. long name (>20 chars): byte-exact ident split ---- */
    {
        uint8_t hdr[BLK];
        const ods2_ident_t *id = fetch_ident(&bv, ods2_fid_number(&long_fid), hdr);

        CHECK(id != NULL, "ods2_fh2_ident() resolves the long-name header");
        if (id) {
            uint8_t ext_pad[63];

            memset(ext_pad, ' ', sizeof(ext_pad));
            CHECK(memcmp(id->fi2_filename, LONG_FIRST20, 20) == 0,
                  "fi2_filename == first 20 chars, fully filled, no padding");
            CHECK(memcmp(id->fi2_filenamext, LONG_EXT3, 3) == 0,
                  "fi2_filenamext[0..3) == the 3-char continuation \"E;1\"");
            CHECK(memcmp(id->fi2_filenamext + 3, ext_pad, sizeof(ext_pad)) == 0,
                  "fi2_filenamext[3..66) space-padded (same convention as "
                  "fi2_filename)");
        }
    }

    /* ---- 2. exact 20-char boundary: fi2_filename full, filenamext untouched (zero) ---- */
    {
        uint8_t hdr[BLK];
        const ods2_ident_t *id = fetch_ident(&bv, ods2_fid_number(&exact20_fid), hdr);

        CHECK(id != NULL, "ods2_fh2_ident() resolves the exact-20 header");
        if (id) {
            uint8_t zero66[66];

            memset(zero66, 0, sizeof(zero66));
            CHECK(memcmp(id->fi2_filename, EXACT20_IDENT, 20) == 0,
                  "fi2_filename == the exact-20-char ident text (no space pad needed)");
            CHECK(memcmp(id->fi2_filenamext, zero66, sizeof(zero66)) == 0,
                  "fi2_filenamext all zero at the n==20 boundary (regression guard: "
                  "identical to the pre-fix < 21 sizeof(idbuf) code path)");
        }
    }

    /* ---- 3. ordinary short name: byte-identical to the pre-fix encoding ---- */
    {
        uint8_t hdr[BLK];
        const ods2_ident_t *id = fetch_ident(&bv, ods2_fid_number(&short_fid), hdr);

        CHECK(id != NULL, "ods2_fh2_ident() resolves the short-name header");
        if (id) {
            char want[20];
            uint8_t zero66[66];

            memset(want, ' ', sizeof(want));
            memcpy(want, "HELLO.TXT;1", strlen("HELLO.TXT;1"));
            memset(zero66, 0, sizeof(zero66));

            CHECK(memcmp(id->fi2_filename, want, 20) == 0,
                  "fi2_filename space-padded exactly as before (short name, no regression)");
            CHECK(memcmp(id->fi2_filenamext, zero66, sizeof(zero66)) == 0,
                  "fi2_filenamext all zero for a short name (no regression)");
        }
    }

    /* ---- resolve + read the long name back over the SAME real path DCL/RMS
     *      use (ods2_bdev_resolve_file / ods2_bdev_read_file), not just the
     *      raw ident bytes -- proves the split doesn't just look right, it
     *      WORKS: dir_insert's 255-char name resolution finds the file and
     *      the FM2 map / recattr the header carries reads its content back. ---- */
    {
        const char *comps[1] = { "OVMXLDIR" };
        uint8_t filehdr[BLK];
        uint8_t data[256];
        size_t data_len = 0;
        ods2_fid_t resolved_fid;
        static const char content[] = "long-name IDENT split content\n";

        st = ods2_bdev_resolve_file(&bv, comps, 1, "LIBVMSPROCESS$SHR.EXE", 1,
                                    &resolved_fid, filehdr, sizeof(filehdr));
        CHECK_EQ(st, ODS2_OK, "resolve_file(OVMXLDIR, LIBVMSPROCESS$SHR.EXE;1)");
        CHECK_EQ(ods2_fid_number(&resolved_fid), ods2_fid_number(&long_fid),
                 "resolved FID matches the FID create_file_raw() returned");

        st = ods2_bdev_read_file(&bv, filehdr, data, sizeof(data), &data_len);
        CHECK_EQ(st, ODS2_OK, "read_file(LIBVMSPROCESS$SHR.EXE;1)");
        CHECK_EQ(data_len, sizeof(content) - 1, "read-back length matches");
        CHECK(data_len == sizeof(content) - 1 &&
              memcmp(data, content, data_len) == 0,
              "read-back content is BYTE-EXACT for the long-named file");
    }

    close(fd);

    if (g_failures == 0) {
        printf("PASS: long ODS-2 filenames (>20 chars) split correctly across "
               "fi2_filename/fi2_filenamext, resolve, and read back; <=20-char "
               "names are byte-identical to the pre-fix encoding\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
