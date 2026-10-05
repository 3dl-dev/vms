/*
 * test_datalink_exec_backend.c - the EXECUTIVE raw-L2 datalink backend
 * (src/libdatalink/ovmx_datalink.c built with SCS_DATALINK_VIA_EXECUTIVE, the
 * ovmx_datalink_exec library the booted DECNETD.EXE links; rd vms-1f69).
 *
 * Proves on the build host:
 *   1. The backend identifies itself as "executive" -- the readout DECNETD's
 *      --show-executor and DECNETD-I-DATALINK print, which the booted
 *      acceptance battery gates on.
 *   2. NO SILENT FALLBACK. Where /dev/vms is absent (a build host), opening the
 *      datalink -- even on loopback, which needs no NIC -- FAILS honestly
 *      (errno ENOENT, "no executive"), and in particular does NOT quietly open a
 *      userspace AF_PACKET socket: the probe code is not compiled into this
 *      library at all, so there is nothing to fall back to. Where /dev/vms IS
 *      present (a kmod/QEMU guest running this binary), a foreign station
 *      address is refused (errno EINVAL from SS$_BADPARAM) -- the executive's
 *      station policy reached through this exact client.
 * The live open/send through a real executive is
 * tests/qemu/test_syssvc_l2_datalink.c (kmod leg) and the DECnet NETACP section
 * of tests/qemu/lib/dcl_acceptance_battery.sh (booted image).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "scs_datalink.h"

static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else { printf("  FAIL: %s\n", m); fail++; } } while (0)

int main(void)
{
    const uint8_t dna142[6]  = { 0xAA, 0x00, 0x04, 0x00, 0x2A, 0x04 };
    const uint8_t foreign[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

    printf("=== test_datalink_exec_backend ===\n");
    CHECK(strcmp(scs_datalink_backend(), "executive") == 0,
          "the ovmx_datalink_exec backend names itself \"executive\"");

    if (access("/dev/vms", F_OK) != 0) {
        errno = 0;
        int fd = scs_datalink_open_station("lo", 0x6003, dna142);
        int e = errno;
        CHECK(fd < 0, "no /dev/vms: the datalink open FAILS (no executive) -- never a fake fd");
        CHECK(e == ENOENT, "no /dev/vms: errno is ENOENT (the executive is absent), not EPERM from a raw-socket attempt");
        fd = scs_datalink_open("lo", 0x6003);
        CHECK(fd < 0, "no /dev/vms: the plain open fails the same way (no AF_PACKET fallback compiled in)");
    } else {
        errno = 0;
        int fd = scs_datalink_open_station("lo", 0x6003, foreign);
        int e = errno;
        CHECK(fd < 0 && e == EINVAL,
              "executive present: a foreign station address is refused (SS$_BADPARAM -> EINVAL)");
        if (fd >= 0)
            scs_datalink_close(fd);
    }

    printf("=== test_datalink_exec_backend: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
