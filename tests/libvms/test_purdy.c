/*
 * test_purdy.c - byte-exact acceptance test for the VMS Purdy password hash
 * (UAI$C_PURDY_S), src/libvms/rtl/purdy.c (vms-631e, epic vms-d0c).
 *
 * THE acceptance bar (CLAUDE.md INV-6): reproduce, byte-exact, the 7 real
 * OpenVMS UAF$Q_PWD quadwords captured from VAX V7.3 + Alpha V8.4 AUTHORIZE
 * (docs/oracle/purdy-hash-vectors.md). A green test that only reproduces
 * OVMX's own output is NOT a VMS-authenticity proof -- these real-VMS vectors
 * are. This test calls the real algorithm on the real inputs; no output is
 * hard-coded to pass (the algorithm is in purdy.c, not here).
 *
 * Plus: determinism (same input -> same output), salt sensitivity, username
 * sensitivity, password sensitivity, and password-length sensitivity -- the
 * structural properties the oracle rows demonstrate on real VMS.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "purdy.h"

static int failures = 0;
static int checks   = 0;

static void expect_eq(const char *what, uint64_t got, uint64_t want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %-40s got=%016llX want=%016llX\n",
               what, (unsigned long long)got, (unsigned long long)want);
    } else {
        printf("  ok   %-40s %016llX\n", what, (unsigned long long)got);
    }
}

static void expect_true(const char *what, int cond)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
}

/* helper: hash a NUL-terminated ASCII password */
static uint64_t H(const char *pw, const char *user, uint16_t salt)
{
    return purdy_s_hash(pw, strlen(pw), user, salt);
}

int main(void)
{
    struct {
        const char *tag;
        const char *user;
        const char *pw;
        uint16_t    salt;
        uint64_t    q;
    } oracle[] = {
        /* --- OpenVMS VAX V7.3 --- */
        { "VAX  V1", "A1ORA",  "KNOWNPW12",  0x4D63, 0x716CBDC03C071C59ull },
        { "VAX  V2", "A2ORA",  "KNOWNPW12",  0x4EE2, 0x84A8F6D135BE8B58ull },
        { "VAX  V3", "A1ORA",  "NEWPWXY34",  0x4D63, 0x01B533EEFFDC1841ull },
        { "VAX  V4", "A3ORA",  "DIFFPWXY99", 0x506C, 0x382C2366EB6DEDA4ull },
        /* --- OpenVMS Alpha V8.4 (same single implementation) --- */
        { "ALPHA A1", "A1ORA", "KNOWNPW12",  0xF52E, 0xC0730697D9B5D7ADull },
        { "ALPHA A2", "A2ORA", "KNOWNPW12",  0xF6D7, 0x787F23E84D1D1DEAull },
        { "ALPHA A3", "A3ORA", "DIFFPWXY99", 0xF834, 0x46F7A9867A983675ull },
    };
    const int NV = (int)(sizeof(oracle) / sizeof(oracle[0]));

    printf("test_purdy (vms-631e): PURDY_S vs real OpenVMS oracle vectors\n");

    /* 1. The whole point: every real-VMS vector, byte-exact. */
    printf("[oracle] %d real VAX V7.3 + Alpha V8.4 UAF$Q_PWD quadwords:\n", NV);
    for (int i = 0; i < NV; i++)
        expect_eq(oracle[i].tag,
                  H(oracle[i].pw, oracle[i].user, oracle[i].salt),
                  oracle[i].q);

    /* 2. Determinism: same (pw, user, salt) -> identical quadword. */
    printf("[determinism]\n");
    for (int i = 0; i < NV; i++) {
        uint64_t a = H(oracle[i].pw, oracle[i].user, oracle[i].salt);
        uint64_t b = H(oracle[i].pw, oracle[i].user, oracle[i].salt);
        expect_true("repeat hash is identical", a == b);
    }

    /* 3. Salt sensitivity (oracle V1 vs A1: same user+pw, different salt). */
    printf("[salt sensitivity]\n");
    expect_true("different salt -> different hash",
                H("KNOWNPW12", "A1ORA", 0x4D63) !=
                H("KNOWNPW12", "A1ORA", 0xF52E));

    /* 4. Username sensitivity (V1 vs V2 concept: username is folded in). */
    printf("[username sensitivity]\n");
    expect_true("different username, same pw+salt -> different hash",
                H("KNOWNPW12", "A1ORA", 0x4D63) !=
                H("KNOWNPW12", "A2ORA", 0x4D63));

    /* 5. Password sensitivity (V1 vs V3: same user+salt, different pw). */
    printf("[password sensitivity]\n");
    expect_true("different password, same user+salt -> different hash",
                H("KNOWNPW12", "A1ORA", 0x4D63) !=
                H("NEWPWXY34", "A1ORA", 0x4D63));

    /* 6. Case-insensitivity: VMS upcases before hashing, so a lowercase
     *    attempt hashes identically to its uppercase form. */
    printf("[case folding]\n");
    expect_true("lowercase password hashes like uppercase",
                H("knownpw12", "a1ora", 0x4D63) ==
                H("KNOWNPW12", "A1ORA", 0x4D63));

    /* 7. Trailing-blank trim: the stored username is blank-padded, but the
     *    hashed name is trimmed -- padding must not change the result. */
    printf("[username trim]\n");
    expect_true("trailing blanks on username are ignored",
                H("KNOWNPW12", "A1ORA   ", 0x4D63) ==
                H("KNOWNPW12", "A1ORA",    0x4D63));

    /* 8. Password-length is folded in (PURDY_S): distinct lengths differ even
     *    when one is a prefix mix. */
    printf("[length fold]\n");
    expect_true("password length participates in the hash",
                purdy_s_hash("AB", 2, "A1ORA", 0x4D63) !=
                purdy_s_hash("AB", 1, "A1ORA", 0x4D63));

    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures == 0)
        printf("PASS: PURDY_S is byte-exact against all %d real OpenVMS vectors\n", NV);
    else
        printf("FAIL: %d check(s) did not match\n", failures);
    return failures == 0 ? 0 : 1;
}
