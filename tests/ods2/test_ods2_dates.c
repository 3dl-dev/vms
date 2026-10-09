/*
 * test_ods2_dates.c - vms-263e: file header dates and revision count.
 *
 * The real VAX volume tests/ods2/real_vax_ods2.dsk (OpenVMS V7.3) shows: a file
 * created and written carries a creation date, a revision date at its close and
 * revision count 1; a directory made by CREATE/DIRECTORY carries both dates and
 * revision count 0; no file has an expiration or backup date. The codec:
 *   - ods2_fh2_build leaves revision count 0 (never the version) and no dates;
 *   - ods2_fh2_set_dates stores creation/revision dates, nothing else;
 *   - ods2_fh2_touch_revision adds one to the count and sets the revision date;
 *   - ods2_fh2_rename keeps count and dates.
 */
#include "vmsfs/ods2.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int pass, fail;
#define CHECK(c, m) do { if (c) { pass++; printf("  PASS: %s\n", m); } \
                         else { fail++; printf("  FAIL: %s\n", m); } } while (0)

static uint64_t q(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

int main(void)
{
    uint8_t hdr[ODS2_BLOCK_SIZE];
    ods2_uic_t owner = { 4, 1 };
    ods2_fid_t backlink = { 4, 4, 0, 0 };
    const ods2_ident_t *id;
    const uint64_t t0 = 0x00BC3AB5C5957000ULL;   /*  8-OCT-2026 14:00:00.00 */
    const uint64_t t1 = 0x00BC3AB5CCBC7E00ULL;   /*  8-OCT-2026 14:00:12.00 */

    printf("=== ODS-2 codec: header dates + revision count (vms-263e) ===\n");
    memset(hdr, 0, sizeof hdr);
    CHECK(ods2_fh2_build(hdr, 100, 1, "HELLO.TXT", 7, 0, ODS2_FK_DATA_STMLF,
                         NULL, 0, 0, backlink, owner, 0, 200) == ODS2_OK, "build");
    id = ods2_fh2_ident(hdr);
    CHECK(id != NULL, "ident area");
    if (!id) return 1;
    CHECK(id->fi2_revision == 0, "a new header's revision count is 0, not the version (7)");
    CHECK(q(id->fi2_credate) == 0 && q(id->fi2_revdate) == 0, "build leaves the dates to the caller");

    CHECK(ods2_fh2_set_dates(hdr, t0, t0) == ODS2_OK, "set_dates");
    CHECK(q(id->fi2_credate) == t0 && q(id->fi2_revdate) == t0, "creation == revision date as set");
    CHECK(q(id->fi2_expdate) == 0 && q(id->fi2_bakdate) == 0, "no expiration or backup date");
    CHECK(id->fi2_revision == 0, "set_dates leaves the revision count");

    CHECK(ods2_fh2_touch_revision(hdr, t1) == ODS2_OK, "touch_revision");
    CHECK(id->fi2_revision == 1, "one modification: revision count 1");
    CHECK(q(id->fi2_revdate) == t1 && q(id->fi2_credate) == t0, "revision date moves, creation date stays");
    CHECK(ods2_fh2_touch_revision(hdr, t1 + 1) == ODS2_OK && id->fi2_revision == 2, "a second modification: 2");

    ods2_fh2_reseal(hdr);
    {
        ods2_fh2_t parsed;
        CHECK(ods2_fh2_parse(hdr, ODS2_BLOCK_SIZE, &parsed) == ODS2_OK, "resealed header parses");
    }
    CHECK(ods2_fh2_rename(hdr, "WORLD.TXT", 3, NULL) == ODS2_OK, "rename");
    CHECK(id->fi2_revision == 2 && q(id->fi2_credate) == t0 && q(id->fi2_revdate) == t1 + 1,
          "rename keeps the revision count and dates");
    CHECK(ods2_fh2_set_dates(NULL, t0, t0) == ODS2_ERR_ARGS, "NULL header refused");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
