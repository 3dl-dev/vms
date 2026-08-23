/*
 * vmspurdy.c - cross-width golden-vector gate for the VMS Purdy password hash
 * (UAI$C_PURDY_S), src/libvms/rtl/purdy.c. Runs the SEVEN real OpenVMS oracle
 * vectors (VAX V7.3 + Alpha V8.4, docs/oracle/purdy-hash-vectors.md -- the same
 * table tests/libvms/test_purdy.c asserts on the host) and requires every one
 * byte-exact. Built STATIC for elf32-vax and run on a real NetBSD/vax guest by
 * tests/lab-vax/run-purdy.sh so the identical vectors are proven on BOTH widths
 * (host LP64 in ctest, VAX ILP32 here). This is the regression gate for the
 * gcc-vax 13.3.0 -O2 DImode miscompile (rd vms-b86) that broke pqexp on VAX and
 * blocked VAX login: it fails on ILP32 if purdy.c's -O0 workaround is removed or
 * any width-value divergence returns. exit 0 = all vectors match, 1 = any miss.
 *
 * Clean-room (CLAUDE.md Rule 8): the vectors are OBSERVED AUTHORIZE output, not
 * VSI source; the algorithm lives in purdy.c, nothing here hard-codes a pass.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "purdy.h"

int main(void)
{
    static const struct {
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
    int failures = 0;

    printf("vmspurdy (rd vms-b86 gate): PURDY_S on ILP32 vs real OpenVMS oracle\n");
    for (int i = 0; i < NV; i++) {
        uint64_t got = purdy_s_hash(oracle[i].pw, strlen(oracle[i].pw),
                                    oracle[i].user, oracle[i].salt);
        if (got == oracle[i].q) {
            printf("  ok   %-9s %016llX\n", oracle[i].tag,
                   (unsigned long long)got);
        } else {
            failures++;
            printf("  FAIL %-9s got=%016llX want=%016llX\n", oracle[i].tag,
                   (unsigned long long)got, (unsigned long long)oracle[i].q);
        }
    }

    /* Determinism on this width: repeat calls agree (catches non-repeatable
     * miscodegen that a single call could mask). */
    for (int i = 0; i < NV; i++) {
        uint64_t a = purdy_s_hash(oracle[i].pw, strlen(oracle[i].pw),
                                  oracle[i].user, oracle[i].salt);
        uint64_t b = purdy_s_hash(oracle[i].pw, strlen(oracle[i].pw),
                                  oracle[i].user, oracle[i].salt);
        if (a != b) { failures++; printf("  FAIL %-9s non-deterministic\n",
                                         oracle[i].tag); }
    }

    if (failures == 0) {
        printf("PURDY-VAX-PASS: all %d real OpenVMS vectors byte-exact on ILP32\n",
               NV);
        return 0;
    }
    printf("PURDY-VAX-FAIL: %d miss(es) -- VAX Purdy diverges (rd vms-b86; is the "
           "purdy.c -O0 workaround intact?)\n", failures);
    return 1;
}
