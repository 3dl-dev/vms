/*
 * test_devtab_fullnam.c - the executive's DVI$_FULLDEVNAM composition
 * (src/kernel-core/vms_devtab_fullnam.h, rd vms-1f7), the SAME header the
 * executive's devinfo_fill() runs, checked against what OpenVMS VAX V7.3
 * printed (tests/lab/captures/fulldevnam-standalone-20261101/, a node booted
 * VAXCLUSTER 0; docs/oracle/vax73-device-full-name.md).
 */
#include <stdio.h>
#include <string.h>

#include "vms_devtab_fullnam.h"

static int pass, fail;
#define CHECK(c, m) do { if (c) { pass++; printf("  PASS: %s\n", m); } \
                         else { fail++; printf("  FAIL: %s\n", m); } } while (0)

static const char *fn(const char *dev, unsigned cls, unsigned c2,
                      const char *node, unsigned alloc)
{
    static char out[24];
    if (vms_devtab_full_name(dev, cls, c2, node, node ? (unsigned)strlen(node) : 0u,
                             alloc, out, sizeof out) < 0)
        return "<none>";
    return out;
}

int main(void)
{
    const unsigned NNM = VMS_DEVTAB_DEV_M_NNM, TERM = 66, MBX = 160, SCOM = 32;

    printf("=== test_devtab_fullnam ===\n");
    CHECK(NNM == 512u, "DEV$M_NNM is 512 (vax73-starlet-defs/DEVDEF.txt)");

    /* capture: _VAX1$OPA0: / _VAX1$TTA0: / _VAX1$RTA0: on a standalone node */
    CHECK(strcmp(fn("OPA0:", TERM, NNM, "VAX1", 2), "VAX1$OPA0:") == 0,
          "a terminal is node-prefixed even with ALLOCLASS set: VAX1$OPA0:");
    CHECK(strcmp(fn("RTA1:", TERM, NNM, "VAX1", 0), "VAX1$RTA1:") == 0,
          "a remote terminal: VAX1$RTA1:");
    /* capture: _$2$DUA0: (ALLOCLASS 2) */
    CHECK(strcmp(fn("DUA0:", 1, NNM, "VAX1", 2), "$2$DUA0:") == 0,
          "a disk on an ALLOCLASS 2 system is $2$DUA0:");
    CHECK(strcmp(fn("VDA100:", 1, NNM, "OVMXB", 0), "OVMXB$VDA100:") == 0,
          "a disk with ALLOCLASS 0 is node-prefixed");
    /* capture: _NLA0:, _XQA0:, _NET0: carry no prefix (DEV$M_NNM clear) */
    CHECK(strcmp(fn("NLA0:", MBX, 0, "VAX1", 2), "NLA0:") == 0, "NLA0: has no prefix");
    CHECK(strcmp(fn("ETH0:", SCOM, 0, "VAX1", 2), "ETH0:") == 0,
          "a LAN controller has no prefix");
    /* already-full served names are returned as is */
    CHECK(strcmp(fn("$2$DUA2:", 1, NNM, "VAX1", 2), "$2$DUA2:") == 0,
          "a served $2$DUA2: is not prefixed twice");
    CHECK(strcmp(fn("VAX1$DUA2:", 1, NNM, "VAX2", 0), "VAX1$DUA2:") == 0,
          "a served VAX1$DUA2: keeps its server's node");
    CHECK(strcmp(fn("OPA0:", TERM, NNM, "ovmxb", 0), "OVMXB$OPA0:") == 0,
          "the node prefix is upper case, as SYI$_SCSNODE reads it");
    /* nothing loaded: nothing invented */
    CHECK(strcmp(fn("OPA0:", TERM, NNM, NULL, 0), "OPA0:") == 0,
          "with no SCSNODE loaded the full name is the physical name");
    CHECK(strcmp(fn("OPA0:", TERM, NNM, "", 0), "OPA0:") == 0,
          "an empty SCSNODE adds no bare '$'");
    {
        char small[8];
        CHECK(vms_devtab_full_name("OPA0:", TERM, NNM, "VAX1", 4, 0, small, sizeof small) < 0
              && small[0] == '\0', "a name that does not fit is refused, never truncated");
    }

    printf("=== test_devtab_fullnam: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
