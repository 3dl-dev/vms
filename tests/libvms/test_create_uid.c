/*
 * test_create_uid.c - SYS$CREATE_UID returns 128-bit identifiers that are
 * unique, well formed (version 1, RFC 4122 variant) and tied to the clock.
 *
 * What is asserted is observable structure and the uniqueness the service
 * exists to provide: 5000 consecutive uids are pairwise distinct (including a
 * burst issued faster than the clock ticks), the version/variant bits are set,
 * the timestamp embedded in each uid is non-decreasing and tracks SYS$GETTIM,
 * and a NULL buffer is refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ssdef.h"
#include "starlet.h"
#include "prcdef.h"

static int pass = 0, fail = 0;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

#define N 5000
static uint8_t uids[N][16];

static uint64_t uid_time(const uint8_t *u)
{
    uint64_t t = 0;
    for (int i = 0; i < 7; i++) t |= (uint64_t)u[i] << (8 * i);
    t |= (uint64_t)(u[7] & 0x0F) << 56;
    return t;
}

static int cmp16(const void *a, const void *b) { return memcmp(a, b, 16); }

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_create_uid ===\n");

    uint64_t before = 0, after = 0;
    sys$gettim(&before);
    int ok = 1;
    for (int i = 0; i < N; i++)
        if (!(sys$create_uid(uids[i]) & 1)) { ok = 0; break; }
    sys$gettim(&after);
    CHECK(ok, "5000 uids created");

    int vers = 1, var = 1, mono = 1;
    for (int i = 0; i < N; i++) {
        if ((uids[i][7] >> 4) != 1) vers = 0;
        if ((uids[i][8] & 0xC0) != 0x80) var = 0;
        if (i && uid_time(uids[i]) <= uid_time(uids[i - 1])) mono = 0;
    }
    CHECK(vers, "every uid carries version 1");
    CHECK(var, "every uid carries the RFC 4122 variant bits");
    CHECK(mono, "timestamps strictly increase, even for a burst inside one clock tick");

    uint8_t sorted[N][16];
    memcpy(sorted, uids, sizeof sorted);
    qsort(sorted, N, 16, cmp16);
    int dup = 0;
    for (int i = 1; i < N; i++) if (memcmp(sorted[i], sorted[i - 1], 16) == 0) dup++;
    CHECK(dup == 0, "all 5000 uids are pairwise distinct");

    const uint64_t epoch = 0x01B21DD213814000ull;
    uint64_t first = uid_time(uids[0]), last = uid_time(uids[N - 1]);
    CHECK(first + 0 >= before + epoch && first <= after + epoch + N,
          "the first uid's timestamp is the clock at creation (VMS time + epoch offset)");
    CHECK(last >= first && last <= after + epoch + N,
          "the last uid's timestamp is not ahead of the clock by more than the burst");

    int same_node = 1;
    for (int i = 1; i < N; i++)
        if (memcmp(uids[i] + 10, uids[0] + 10, 6) != 0) same_node = 0;
    CHECK(same_node, "the node field is stable for the process");

    /* The node field is the SCSNODE name, blank-padded to six characters (the lab
     * OpenVMS Alpha V8.4 node's uids carry "ALPHA1" there). With no readable SCSNODE
     * the fallback is a random node with the multicast bit set. */
    {
        char scs[16];
        uint16_t sl = 0;
        struct item_list_3 il[2];
        memset(scs, 0, sizeof scs);
        il[0].buflen = 6; il[0].item_code = SYI$_SCSNODE; il[0].bufaddr = scs; il[0].retlen = &sl;
        il[1].buflen = 0; il[1].item_code = 0; il[1].bufaddr = NULL; il[1].retlen = NULL;
        if ((sys$getsyiw(0, NULL, NULL, il, NULL, NULL, 0) & 1) && sl > 0) {
            char want[6];
            memset(want, ' ', sizeof want);
            memcpy(want, scs, sl < 6 ? sl : 6);
            CHECK(memcmp(uids[0] + 10, want, 6) == 0,
                  "the node field is the blank-padded SCSNODE name");
        } else {
            CHECK(uids[0][10] & 0x01, "no SCSNODE readable: the node field is random with the multicast bit");
        }
    }

    CHECK(sys$create_uid(NULL) == SS$_ACCVIO, "a NULL buffer is refused with SS$_ACCVIO");

    printf("=== test_create_uid: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
