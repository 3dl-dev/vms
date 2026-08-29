/*
 * test_sysuaf_quota_codec.c - the [OVMX] SYSUAF quota region encodes and
 * decodes SYSTEM's authorized JIB quota set losslessly, and an unseeded
 * record decodes as "no quota" (vms-14a, parent vms-050).
 *
 * WHAT THIS PROVES, host-side (no /dev/vms needed). vms-14a built the quota
 * source behind #884's honest omission: mksysuaf seeds an account's authorized
 * quota set into the [OVMX] uaf$r_quota region (sysuaf_quota_encode), LOGINOUT
 * decodes it (sysuaf_quota_decode) and hands it to the executive so
 * SHOW PROCESS/QUOTAS / SHOW WORKING_SET light VMS_PI_V_QUOTA with real values.
 * The end-to-end proof (real values on a live executive) is the QEMU DCL/SHOW
 * acceptance battery; what is proven HERE is the codec at the two ends of that
 * wire agrees, byte-for-byte, and that the presence marker keeps an UNSEEDED
 * record honestly quota-less (the executive must leave VMS_PI_V_QUOTA clear).
 *
 * The values are oracle-grounded (docs/oracle/vax73-show-process-quotas.md
 * §4): SYSTEM's authorized limits on a real OpenVMS VAX V7.3 system. They are
 * duplicated here from that oracle, NOT read from mksysuaf, so a silent edit to
 * the seed that diverges from the oracle is caught (INV-6 / Rule 10).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "sysuaf.h"

static int failures;

#define CHECK(cond, msg) do {                                           \
    if (cond) { printf("  ok: %s\n", (msg)); }                          \
    else      { printf("  FAIL: %s\n", (msg)); failures++; }            \
} while (0)

/* SYSTEM's authorized JIB quota set, oracle §4 (F$GETJPI on VAX V7.3). */
static const sysuaf_quota_t oracle_system = {
    .astlm = 100, .biolm = 100, .bytlm = 47872, .diolm = 100,
    .enqlm = 200, .fillm = 300, .pgflquota = 40960, .prclm = 10,
    .tqelm = 30, .wsdefault = 512, .wsquota = 1024, .wsextent = 28700,
};

int main(void)
{
    printf("test_sysuaf_quota_codec (vms-14a)\n");

    /* 1. Round-trip: encode the oracle set into a zeroed record, decode it. */
    sysuaf_rms_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sysuaf_quota_encode(&rec, &oracle_system);

    sysuaf_quota_t got;
    memset(&got, 0, sizeof(got));
    int present = sysuaf_quota_decode(&rec, &got);
    CHECK(present == 1, "seeded record decodes as present (marker set)");
    CHECK(got.astlm     == 100,   "ASTLM     round-trips (100)");
    CHECK(got.biolm     == 100,   "BIOLM     round-trips (100)");
    CHECK(got.bytlm     == 47872, "BYTLM     round-trips (47872)");
    CHECK(got.diolm     == 100,   "DIOLM     round-trips (100)");
    CHECK(got.enqlm     == 200,   "ENQLM     round-trips (200)");
    CHECK(got.fillm     == 300,   "FILLM     round-trips (300)");
    CHECK(got.pgflquota == 40960, "PGFLQUOTA round-trips (40960)");
    CHECK(got.prclm     == 10,    "PRCLM     round-trips (10)");
    CHECK(got.tqelm     == 30,    "TQELM     round-trips (30)");
    CHECK(got.wsdefault == 512,   "WSDEFAULT round-trips (512)");
    CHECK(got.wsquota   == 1024,  "WSQUOTA   round-trips (1024)");
    CHECK(got.wsextent  == 28700, "WSEXTENT  round-trips (28700)");
    CHECK(memcmp(&got, &oracle_system, sizeof(got)) == 0,
          "decoded block equals the oracle set exactly");

    /* 2. An unseeded (zeroed) record must decode as NOT present, so the
     *    executive omits VMS_PI_V_QUOTA rather than reporting a zeroed block as
     *    measured (INV-6 -- the anti-"zero-means-absent" discipline). */
    sysuaf_rms_record_t zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    sysuaf_quota_t none;
    CHECK(sysuaf_quota_decode(&zeroed, &none) == 0,
          "unseeded record decodes as NOT present (honest omission)");

    /* 3. The encode must not disturb the [PIN] record fields around the region
     *    -- the presence marker and cells live entirely inside uaf$r_quota. */
    sysuaf_rms_record_t r2;
    memset(&r2, 0xAB, sizeof(r2));
    uint8_t before_user[32];
    memcpy(before_user, r2.uaf$t_username, sizeof(before_user));
    sysuaf_quota_encode(&r2, &oracle_system);
    CHECK(memcmp(before_user, r2.uaf$t_username, sizeof(before_user)) == 0,
          "encode touches only uaf$r_quota, not the username [PIN] field");

    /* 4. LE byte order at the wire: the presence marker is byte 0 of the region
     *    and BYTLM's low byte is where the sub-offset says. Guards against a
     *    host-endianness assumption creeping into the fixed on-disk layout. */
    CHECK(rec.uaf$r_quota[UAF$K_QUO_PRESENT] == 1,
          "presence marker is a 1 at region byte 0");
    CHECK(rec.uaf$r_quota[UAF$K_QUO_BYTLM]     == (47872u & 0xff) &&
          rec.uaf$r_quota[UAF$K_QUO_BYTLM + 1] == ((47872u >> 8) & 0xff),
          "BYTLM is stored little-endian at its sub-offset");

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASSED\n");
    return 0;
}
