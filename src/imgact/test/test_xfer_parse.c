/*
 * test_xfer_parse.c -- host unit test for the .vms$xfer parse and the Alpha
 * standard-call Argument-Information (AI) layout (bead vms-f60d).
 *
 * Proves:
 *   (1) A well-formed .vms$xfer with flavor==VMS_STD is parsed: flavor, count,
 *       and main_off (== the LAST entry, the __main transfer address).
 *   (2) A .vms$xfer with flavor==SYSV parses as SYSV (valid, tail-jump path).
 *   (3) ZERO-REGRESSION: an ABSENT section (the case for every image OVMX ships
 *       today) yields flavor==OVMX_ACT_SYSV and valid==0 -- the caller takes the
 *       unchanged tail-jump path. So do a short, bad-magic, zero-count, and
 *       truncated-entry-array section (all must fall back to SYSV, never
 *       mis-parse into a spurious standard call).
 *   (4) The AI-register value IMGACT places in R25 is built by the documented
 *       layout: arg count in <7:0>, six 3-bit kind groups in <25:8>. For six
 *       64-bit args (AI$K_AR_I64==0) the value is 6; a probe with non-I64 kinds
 *       proves the groups land at the documented bit positions (not hard-coded).
 *
 * Exit 0 on success, 1 on first failure.
 */
#define _POSIX_C_SOURCE 200809L

#include "imgact_xfer.h"
#include "ovmx_image.h"
#include "ovmx_activation.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do {                       \
    if (cond) { printf("PASS: %s\n", msg); }        \
    else { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* Build a .vms$xfer blob into `buf` (>= 512 bytes): header + `count` entries. */
static unsigned long build_xfer(unsigned char *buf, uint32_t magic,
                                uint32_t flavor, uint32_t count,
                                const uint64_t *entries)
{
    struct ovmx_xfer_header h;
    h.magic = magic;
    h.flavor = flavor;
    h.count = count;
    h.reserved = 0;
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), entries, (size_t)count * sizeof(uint64_t));
    return sizeof(h) + (unsigned long)count * sizeof(uint64_t);
}

int main(void)
{
    unsigned char buf[512];
    struct ovmx_xfer_info xi;

    /* (1) Well-formed VMS_STD, one entry. */
    {
        uint64_t ents[1] = { 0x1234 };
        unsigned long sz = build_xfer(buf, OVMX_XFER_MAGIC, OVMX_ACT_VMS_STD,
                                      1, ents);
        int r = ovmx_parse_xfer(buf, sz, &xi);
        CHECK(r == 1, "VMS_STD one-entry: parse returns 1");
        CHECK(xi.valid == 1, "VMS_STD one-entry: valid");
        CHECK(xi.flavor == OVMX_ACT_VMS_STD, "VMS_STD one-entry: flavor VMS_STD");
        CHECK(xi.count == 1, "VMS_STD one-entry: count 1");
        CHECK(xi.main_off == 0x1234, "VMS_STD one-entry: main_off == entry");
    }

    /* (1b) Multi-entry: main_off is the LAST (LIB$INITIALIZE handlers precede). */
    {
        uint64_t ents[3] = { 0xAAA, 0xBBB, 0xCAFE };
        unsigned long sz = build_xfer(buf, OVMX_XFER_MAGIC, OVMX_ACT_VMS_STD,
                                      3, ents);
        int r = ovmx_parse_xfer(buf, sz, &xi);
        CHECK(r == 1 && xi.count == 3, "VMS_STD three-entry: count 3");
        CHECK(xi.main_off == 0xCAFE, "VMS_STD three-entry: main_off == LAST entry");
    }

    /* (2) Well-formed SYSV flavor (explicit, valid). */
    {
        uint64_t ents[1] = { 0x40 };
        unsigned long sz = build_xfer(buf, OVMX_XFER_MAGIC, OVMX_ACT_SYSV,
                                      1, ents);
        int r = ovmx_parse_xfer(buf, sz, &xi);
        CHECK(r == 1 && xi.valid == 1 && xi.flavor == OVMX_ACT_SYSV,
              "explicit SYSV flavor: parsed, flavor SYSV");
    }

    /* (3) ZERO-REGRESSION fallbacks -- every one must yield SYSV + !valid. */
    {
        int r = ovmx_parse_xfer(NULL, 0, &xi);
        CHECK(r == 0 && xi.valid == 0 && xi.flavor == OVMX_ACT_SYSV,
              "absent section (NULL,0): SYSV, !valid  [zero-regression]");
    }
    {
        int r = ovmx_parse_xfer(buf, 4, &xi);   /* smaller than header */
        CHECK(r == 0 && xi.flavor == OVMX_ACT_SYSV,
              "short section (< header): SYSV, !valid");
    }
    {
        uint64_t ents[1] = { 0x99 };
        unsigned long sz = build_xfer(buf, 0xDEADBEEFu, OVMX_ACT_VMS_STD,
                                      1, ents);
        int r = ovmx_parse_xfer(buf, sz, &xi);
        CHECK(r == 0 && xi.flavor == OVMX_ACT_SYSV,
              "bad magic: SYSV, !valid (not mistaken for a standard-call image)");
    }
    {
        uint64_t ents[1] = { 0 };
        unsigned long sz = build_xfer(buf, OVMX_XFER_MAGIC, OVMX_ACT_VMS_STD,
                                      0, ents);
        (void)sz;
        int r = ovmx_parse_xfer(buf, sizeof(struct ovmx_xfer_header), &xi);
        CHECK(r == 0 && xi.flavor == OVMX_ACT_SYSV,
              "count==0: SYSV, !valid");
    }
    {
        /* Header claims 4 entries but only header+1 entry of bytes present. */
        uint64_t ents[1] = { 0x7 };
        build_xfer(buf, OVMX_XFER_MAGIC, OVMX_ACT_VMS_STD, 1, ents);
        struct ovmx_xfer_header *h = (struct ovmx_xfer_header *)buf;
        h->count = 4;   /* lie about the count */
        int r = ovmx_parse_xfer(buf, sizeof(*h) + sizeof(uint64_t), &xi);
        CHECK(r == 0 && xi.flavor == OVMX_ACT_SYSV,
              "truncated entry array (count > bytes): SYSV, !valid (bounds)");
    }

    /* (4) AI-register layout. */
    {
        uint64_t ai = OVMX_AI_VMS_ACTIVATION;
        CHECK((ai & 0xff) == 6, "AI: arg count field <7:0> == 6");
        CHECK(((ai >> 8) & 0x3ffff) == 0,
              "AI: six I64 kind groups <25:8> == 0 (all AI$K_AR_I64)");
        CHECK((ai >> 26) == 0, "AI: bits <63:26> == 0");
        CHECK(ai == 6, "AI: six 64-bit args -> R25 == 6");

        /* Probe: a distinct kind in group 0 and group 5 must land at the
         * documented bit positions 8 and 8+5*3=23 -- proving the value is
         * built from the layout, not a constant. (5 is an arbitrary probe
         * kind, not a real AI$K_ value.) */
        uint64_t p = OVMX_AI_BUILD6(5u, 0u, 0u, 0u, 0u, 5u);
        CHECK((p & 0xff) == 6, "AI probe: arg count still 6");
        CHECK(((p >> 8) & 7u) == 5u, "AI probe: kind group 0 at bit 8");
        CHECK(((p >> 23) & 7u) == 5u, "AI probe: kind group 5 at bit 23");
    }

    if (failures) {
        printf("\n%d CHECK(s) FAILED\n", failures);
        return 1;
    }
    printf("\nALL .vms$xfer PARSE + AI-LAYOUT CHECKS PASSED\n");
    return 0;
}
