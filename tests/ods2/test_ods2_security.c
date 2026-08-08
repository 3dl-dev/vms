/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_security.c - Increment 4: SECURITY.SYS VBN1 data-block writer
 * and reader (ods2_security_build() / ods2_security_parse(), see ods2.h's
 * WRITER [F6] provenance comment).
 *
 * Two kinds of assertion:
 *   (1) OFFLINE ROUND-TRIP: ods2_security_build() writes a block,
 *       ods2_security_parse() reads it back and must recover the same
 *       label/owner UIC, and a single flipped byte must fail the checksum.
 *   (2) REGRESSION PIN AGAINST THE REAL-VAX ORACLE: the checksum algorithm
 *       was derived from 12 real `INITIALIZE`+`MOUNT` trials on lab-2 (pod
 *       vaxlab-8) across 9 labels and 2 device geometries -- see
 *       PROVENANCE-real_vax_ods2.md's increment-4 addendum for the raw
 *       DUMP/BYTE captures. This test pins a representative subset of
 *       those EXACT observed checksum values (not just "does our own
 *       writer agree with our own reader", which would pass even if both
 *       drifted from real VMS together) so a future change to the
 *       algorithm that silently diverges from the real oracle fails loudly
 *       here instead of only at the next lab-2 MOUNT.
 */

#include "vmsfs/ods2.h"

#include <stdio.h>
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
        printf("  FAIL: %s: got %ld, want %ld  (%s:%d)\n",             \
               (msg), _va, _vb, __FILE__, __LINE__);                   \
        g_failures++;                                                  \
    }                                                                  \
} while (0)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Real lab-2 checksum values (raw disk-image bytes at the file's LBN,
 * offset 0, read with `xxd`/`cmp -l` -- see PROVENANCE increment-4
 * addendum). label -> expected checksum longword.
 */
struct oracle_case {
    const char *label;
    uint32_t    checksum;
};

static const struct oracle_case g_oracle[] = {
    { "A",            0x000f084dU },  /* RX50/800, single-char edge case   */
    { "AB",           0x424e084cU },  /* RX50/800                          */
    { "XYZ",          0x5957084cU },  /* RX50/800, odd length              */
    { "SECTEST",      0x16195c0fU },  /* RX50/800                          */
    { "ALPHA9Z",      0x750f401cU },  /* RX50/800, different label, len 7  */
    { "TESTVOL1",     0x0a0d5c1bU },  /* RX50/800                          */
    { "ABCDEFGHIJKL", 0x4e420454U },  /* RX50/800, max 12-char label       */
    { "BIGVOL",       0x05025e0bU },  /* RX33/2400 -- different geometry   */
};

int main(void)
{
    unsigned i;
    uint8_t block[ODS2_BLOCK_SIZE];
    ods2_status_t st;
    ods2_uic_t owner, owner_out;
    char label_out[16];

    printf("=== ODS-2 SECURITY.SYS data block (increment 4) ===\n");

    owner.uic_member = 4;
    owner.uic_group  = 1;

    /* ---- (1) offline round-trip ---- */
    st = ods2_security_build(block, "OVMXVOL1", owner);
    CHECK_EQ(st, ODS2_OK, "ods2_security_build");

    st = ods2_security_parse(block, sizeof(block), label_out, sizeof(label_out), &owner_out);
    CHECK_EQ(st, ODS2_OK, "ods2_security_parse round-trip");
    CHECK(strcmp(label_out, "OVMXVOL1") == 0, "label round-trips");
    CHECK_EQ(owner_out.uic_member, 4, "owner UIC member round-trips");
    CHECK_EQ(owner_out.uic_group, 1, "owner UIC group round-trips");

    /* corrupting the checksum must be caught */
    {
        uint8_t corrupt[ODS2_BLOCK_SIZE];
        memcpy(corrupt, block, sizeof(corrupt));
        corrupt[0] ^= 0xFF;
        st = ods2_security_parse(corrupt, sizeof(corrupt), NULL, 0, NULL);
        CHECK_EQ(st, ODS2_ERR_CHECKSUM, "corrupted checksum rejected");
    }

    /* corrupting a label byte (without fixing the checksum) must be caught */
    {
        uint8_t corrupt[ODS2_BLOCK_SIZE];
        memcpy(corrupt, block, sizeof(corrupt));
        corrupt[0x52] ^= 0xFF;
        st = ods2_security_parse(corrupt, sizeof(corrupt), NULL, 0, NULL);
        CHECK_EQ(st, ODS2_ERR_CHECKSUM, "corrupted label rejected");
    }

    /* argument validation */
    st = ods2_security_build(block, "", owner);
    CHECK_EQ(st, ODS2_ERR_ARGS, "empty label rejected");
    st = ods2_security_build(block, "THIRTEEN-CHR", owner); /* 12 chars, should be OK */
    CHECK_EQ(st, ODS2_OK, "12-char label accepted");
    st = ods2_security_build(block, "FOURTEEN-CHRS", owner); /* 13 chars */
    CHECK_EQ(st, ODS2_ERR_ARGS, "13-char label rejected");

    /* ---- (2) regression pin against the real-VAX oracle ---- */
    for (i = 0; i < sizeof(g_oracle) / sizeof(g_oracle[0]); i++) {
        uint32_t got;

        st = ods2_security_build(block, g_oracle[i].label, owner);
        CHECK_EQ(st, ODS2_OK, "oracle case build");
        got = le32(block);
        if (got != g_oracle[i].checksum) {
            printf("  FAIL: oracle checksum for %-14s got %08x, want %08x  (%s:%d)\n",
                   g_oracle[i].label, got, g_oracle[i].checksum, __FILE__, __LINE__);
            g_failures++;
        }

        /* and the reader must accept our own writer's output */
        st = ods2_security_parse(block, sizeof(block), label_out, sizeof(label_out), NULL);
        CHECK_EQ(st, ODS2_OK, "oracle case parses");
        CHECK(strcmp(label_out, g_oracle[i].label) == 0, "oracle case label round-trips");
    }

    if (g_failures) {
        printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("PASS: all checks passed\n");
    return 0;
}
