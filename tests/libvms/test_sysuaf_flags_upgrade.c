/*
 * test_sysuaf_flags_upgrade.c - an installed SYSUAF.DAT written before the UAI$M_
 * numbering moved to the V7.3/V8.4 $UAIDEF (vms-f811) must not be misread.
 *
 * UAF$L_FLAGS holds UAI$M_ bits whose VALUES changed (e.g. DISACNT 0x4000 -> 0x10,
 * and old 0x4000 is now AUTOLOGIN). Records carry an OVMX flags-layout marker at
 * @0x190: 0 on every record written before the marker existed. The reader must
 * convert such a record on first access and stamp the marker; a record already
 * marked current must be left alone. The assertions use the numbers that would be
 * WRONG if the old bits were read as new ones.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sysuaf.h"

static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

static void setle32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t le32(const uint8_t *p)
{ return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void legacy_record(sysuaf_rms_record_t *raw, const char *user, uint32_t oldflags)
{
    sysuaf_record_t v;
    memset(&v, 0, sizeof v);
    strncpy(v.username, user, sizeof v.username - 1);
    sysuaf_view_to_raw(&v);
    *raw = v.raw;
    setle32(raw->uaf$l_flags, oldflags);
    raw->uaf$b_flags_layout = UAF$K_FLAGS_LAYOUT_LEGACY;   /* as written before the marker */
}

int main(void)
{
    printf("=== test_sysuaf_flags_upgrade ===\n");
    sysuaf_rms_record_t raw;
    sysuaf_record_t v;

    /* old DISACNT (0x4000) | old CAPTIVE (0x80) */
    legacy_record(&raw, "OLDDIS", 0x4000u | 0x80u);
    sysuaf_raw_to_view(&raw, &v);
    CHECK(strstr(v.flags, "DISACNT") != NULL, "legacy 0x4000 is read as DISACNT");
    CHECK(strstr(v.flags, "CAPTIVE") != NULL, "legacy 0x80 is read as CAPTIVE");
    CHECK(strstr(v.flags, "AUTOLOGIN") == NULL,
          "legacy 0x4000 is NOT misread as AUTOLOGIN (its current meaning)");
    CHECK(le32(v.raw.uaf$l_flags) == (UAI$M_DISACNT | UAI$M_CAPTIVE),
          "the view's raw flags longword now holds the current bits");
    CHECK(v.raw.uaf$b_flags_layout == UAF$K_FLAGS_LAYOUT_V73, "the marker is stamped");

    /* admin-forced expiry, old bit 0x10000000 */
    legacy_record(&raw, "OLDEXP", 0x10000000u);
    sysuaf_raw_to_view(&raw, &v);
    CHECK(sysuaf_password_expired(&v) == 1, "a legacy PWD_EXPIRED record still reports an expired password");

    /* old DISUSER 0x10000 must stay an account-disabling flag */
    legacy_record(&raw, "OLDUSR", 0x10000u);
    sysuaf_raw_to_view(&raw, &v);
    CHECK(strstr(v.flags, "DISUSER") != NULL, "legacy DISUSER (0x10000) is read as DISUSER");

    /* a record already marked current: same number means the CURRENT thing */
    legacy_record(&raw, "NEWREC", UAI$M_AUTOLOGIN);
    raw.uaf$b_flags_layout = UAF$K_FLAGS_LAYOUT_V73;
    sysuaf_raw_to_view(&raw, &v);
    CHECK(strstr(v.flags, "AUTOLOGIN") != NULL && strstr(v.flags, "DISACNT") == NULL,
          "a marked record is not converted");

    /* round trip: a converted record written back carries the marker and the new bits */
    legacy_record(&raw, "OLDRT", 0x4000u);
    sysuaf_raw_to_view(&raw, &v);
    sysuaf_view_to_raw(&v);
    CHECK(v.raw.uaf$b_flags_layout == UAF$K_FLAGS_LAYOUT_V73 &&
          le32(v.raw.uaf$l_flags) == UAI$M_DISACNT,
          "write-back persists the converted flags with the marker");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
