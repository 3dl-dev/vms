/*
 * test_l2_station.c - host unit test of the executive L2 STATION-address policy
 * (rd vms-1f69, src/kernel/vms_l2_station.h): the SAME header-only rule
 * VMS_IOCTL_L2_OPEN enforces in vms.ko (src/kernel-core/vms_l2.c), driven here
 * without a kernel. The live-executive proof (accept/refuse + the executive
 * stamping the station as every frame's source, observed on the wire) is
 * tests/qemu/test_syssvc_l2_datalink.c section 6 on the kmod leg.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vms_l2_station.h"

static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else { printf("  FAIL: %s\n", m); fail++; } } while (0)

int main(void)
{
    const uint8_t nic[6]     = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    const uint8_t zero[6]    = { 0 };
    const uint8_t dna142[6]  = { 0xAA, 0x00, 0x04, 0x00, 0x2A, 0x04 };
    const uint8_t dna00[6]   = { 0xAA, 0x00, 0x04, 0x00, 0x00, 0x00 };
    const uint8_t foreign[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    const uint8_t bcast[6]   = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    const uint8_t mcast[6]   = { 0xAB, 0x00, 0x00, 0x03, 0x00, 0x00 }; /* all-routers */
    const uint8_t near[6]    = { 0xAA, 0x00, 0x04, 0x01, 0x2A, 0x04 }; /* wrong HIORD */
    uint8_t got[6];

    printf("=== test_l2_station ===\n");

    CHECK(vms_l2_station_check(zero, nic) == VMS_L2_STATION_USE_NIC,
          "all-zero request -> use the NIC hwaddr (the pre-vms-1f69 default)");
    CHECK(vms_l2_station_check(nic, nic) == VMS_L2_STATION_OK,
          "the NIC's own hwaddr is accepted");
    CHECK(vms_l2_station_check(dna142, nic) == VMS_L2_STATION_OK,
          "DECnet Phase IV AA-00-04-00-2A-04 (1.42) is accepted");
    CHECK(vms_l2_station_check(foreign, nic) == VMS_L2_STATION_REFUSED,
          "a foreign unicast station is REFUSED (no impersonation)");
    CHECK(vms_l2_station_check(bcast, nic) == VMS_L2_STATION_REFUSED,
          "broadcast as a source is REFUSED");
    CHECK(vms_l2_station_check(mcast, nic) == VMS_L2_STATION_REFUSED,
          "a DECnet multicast (AB-00-00-03-00-00) as a source is REFUSED");
    CHECK(vms_l2_station_check(near, nic) == VMS_L2_STATION_REFUSED,
          "AA-00-04-01-.. (not the HIORD block) is REFUSED");
    CHECK(vms_l2_station_check(dna00, nic) == VMS_L2_STATION_REFUSED,
          "AA-00-04-00-00-00 (DECnet 0.0 names no node) is REFUSED");
    /* a NIC that is itself zero (loopback) still treats zero as 'use NIC' */
    CHECK(vms_l2_station_check(zero, zero) == VMS_L2_STATION_USE_NIC,
          "all-zero request on a zero-hwaddr interface (lo) -> use NIC");
    CHECK(vms_l2_station_check(dna142, zero) == VMS_L2_STATION_OK,
          "Phase IV station on a zero-hwaddr interface (lo) is accepted");

    vms_l2_dna_station(1, 42, got);
    CHECK(memcmp(got, dna142, 6) == 0,
          "vms_l2_dna_station(1,42) = AA-00-04-00-2A-04 (LE16(1<<10|42))");
    vms_l2_dna_station(1, 1, got);
    {
        const uint8_t v11[6] = { 0xAA, 0x00, 0x04, 0x00, 0x01, 0x04 };
        CHECK(memcmp(got, v11, 6) == 0,
              "vms_l2_dna_station(1,1) = AA-00-04-00-01-04 (the vms-3be VAX 1.1 source)");
    }
    vms_l2_dna_station(63, 1023, got);
    {
        const uint8_t vmax[6] = { 0xAA, 0x00, 0x04, 0x00, 0xFF, 0xFF };
        CHECK(memcmp(got, vmax, 6) == 0, "vms_l2_dna_station(63,1023) = AA-00-04-00-FF-FF");
    }

    printf("=== test_l2_station: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
