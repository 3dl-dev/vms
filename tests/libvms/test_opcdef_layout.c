/*
 * test_opcdef_layout.c - the OPCDEF message header carries the full 24-bit target-class
 * mask. The earlier struct had a ONE-byte target, so OPC$M_NM_SECURITY (0x100) and every
 * OPERn class (0x1000 ... 0x800000) were truncated away. Classes set through
 * OPC$SET_TARGET must read back unchanged, land in bytes 1..3 of the header, and leave
 * the type byte, request id and text offset where the oracle's $OPCDEF puts them.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "opcdef.h"

static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

int main(void)
{
    printf("=== test_opcdef_layout ===\n");
    struct opcdef h;
    memset(&h, 0, sizeof h);
    h.opc$b_ms_type = OPC$_RQ_RQST;
    OPC$SET_TARGET(h, OPC$M_NM_CENTRL | OPC$M_NM_SECURITY | OPC$M_NM_OPER3 | OPC$M_NM_OPER12);
    uint32_t want = OPC$M_NM_CENTRL | OPC$M_NM_SECURITY | OPC$M_NM_OPER3 | OPC$M_NM_OPER12;
    CHECK(OPC$GET_TARGET(h) == want, "CENTRAL|SECURITY|OPER3|OPER12 read back unchanged");
    const uint8_t *b = (const uint8_t *)&h;
    CHECK(b[1] == (want & 0xFF) && b[2] == ((want >> 8) & 0xFF) && b[3] == ((want >> 16) & 0xFF),
          "the mask occupies header bytes 1..3 (little-endian)");
    h.opc$b_ms_target = OPC$M_NM_CENTRL;   /* what real programs write (sys_sndopr.c) */
    CHECK(b[1] == OPC$M_NM_CENTRL, "assigning the opc$b_ms_target member works as in VMS sources");
    CHECK(b[0] == OPC$_RQ_RQST, "the type byte is untouched at +0");
    CHECK(OPC$K_MS_HDRLEN == 8 && offsetof(struct opcdef, opc$l_ms_rqstid) == 4,
          "header length 8, request id at +4, text at +8");
    CHECK(OPC$M_NM_OPER12 == 0x800000 && (OPC$M_NM_OPER12 >> 16) <= 0xFF,
          "OPER12 (bit 23) fits the 3-byte field");
    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
