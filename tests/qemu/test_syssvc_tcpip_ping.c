/*
 * test_syssvc_tcpip_ping.c - the DCL PING tool drives a real ICMP ECHO round-trip
 * over the INET pseudo-device BGn: (vms-527) via the PUBLIC $ASSIGN / $QIO /
 * $DASSGN system services against a REAL /dev/vms (vms-80b).
 *
 * ============================================================
 * WHAT THIS PROVES. vms-527/vms-dbb gave OVMX TCP over BGn: (a STREAM socket).
 * PING needs a different socket KIND -- a RAW/ICMP executive socket -- which the
 * TCP client tools deliberately deferred as a NOOP (never faked). vms-80b adds
 * that raw ICMP socket to the executive: IO$_SETMODE carries the P2 socket-kind
 * selector IO$K_SOCK_ICMP so vms.ko mints an AF_INET/SOCK_RAW/IPPROTO_ICMP host
 * socket, and the ordinary IO$_ACCESS/WRITEVBLK/READVBLK then move an ICMP echo
 * PDU the shared engine (src/vmstcpip/services/tcpip_ping.h) builds. The SAME
 * header the DCL PING verb ships (src/vmsdcl/dcl_cmd_misc.c) is #included here,
 * so the shipped verb and the proven code are identical.
 *
 * This suite pings 127.0.0.1 (the kernel loopback, which it brings up itself)
 * and asserts:
 *   - the ICMP echo round-trip COMPLETES over BGn: (an echo REPLY carrying our
 *     id/seq comes back through the executive raw socket), and
 *   - the reply's echoed data is BYTE-EXACT with what was sent.
 * The whole $QIO path -- socket create, connect, send, receive -- is
 * executive/in-kernel; the host kernel's own IP/ICMP stack generates the reply
 * (OVMX never reimplements ICMP). No NIC is needed; this runs under `-nic none`.
 * ============================================================
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is loaded,
 * exactly as test_syssvc_tcpip_client.c does): $ASSIGN TCPIP$DEVICE: must fail
 * SS$_NOSUCHDEV, never fabricate a private per-process raw socket (CLAUDE.md
 * Rule 9 / INV-6). This suite returns EXIT_SKIP (77) with no /dev/vms.
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * the byte-exact assertion is anchored by the tcpip-ping-payload-dropped defect,
 * which neutralises the payload copy in tcpip_ping_echo() -- so the request (and
 * therefore the reply) carries zeroed data. The round-trip STILL completes (a
 * reply of the right length arrives, keeping the round-trip assertion green) and
 * only the byte-exact assertion reddens.
 *
 * WATCHDOG: a blocking $QIO recv lives in the executive, so a host that never
 * replies would hang the whole QEMU boot. alarm() bounds the run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "starlet.h"
#include "descrip.h"
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

/* The PING engine under test -- the SAME header the DCL PING verb ships. */
#include "../../src/vmstcpip/services/tcpip_ping.h"

#define EXIT_SKIP 77

/* A recognizable, NON-ZERO payload so the byte-exact assertion actually fails
 * when the negative control zeroes the sent data. */
static const char PING_PAYLOAD[] =
    "OVMX-PING vms-80b: ICMP echo BYTE-EXACT over BGn: raw socket.";

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_tcpip_ping timed out (no ICMP reply)\n";
    (void)!write(1, m, sizeof(m) - 1);
    _exit(1);
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* `-nic none` boots with lo DOWN; 127.0.0.1 is unreachable until it is up. A raw
 * SIOCSIFFLAGS is self-contained (no busybox applet dependency). */
static void bring_lo_up(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    if (s < 0) return;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        (void)ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
}

int main(void)
{
    const uint32_t lo_be = 0x0100007fu;   /* 127.0.0.1, network byte order */
    unsigned char reply[128];
    uint32_t rlen = 0;
    uint32_t st;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_tcpip_ping (DCL PING / ICMP echo over BGn: $QIO, vms-80b) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        st = tcpip_ping_echo(0x0100007fu /* 127.0.0.1 */, 0x8080u, 1,
                             PING_PAYLOAD, (uint32_t)(sizeof(PING_PAYLOAD) - 1),
                             reply, sizeof(reply), &rlen);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: PING fails SS$_NOSUCHDEV, never a local per-process raw socket");
        printf("=== test_syssvc_tcpip_ping: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    signal(SIGALRM, watchdog);
    alarm(30);
    bring_lo_up();

    /* One ICMP echo round-trip to the loopback address. */
    rlen = 0;
    st = tcpip_ping_echo(lo_be, 0x4f56u /* 'OV' */, 1,
                         PING_PAYLOAD, (uint32_t)(sizeof(PING_PAYLOAD) - 1),
                         reply, sizeof(reply), &rlen);

    CHECK((st & 1) && rlen == sizeof(PING_PAYLOAD) - 1,
          "PING loopback ICMP echo round-trip completes over BGn: (reply received)");
    /* negctl: tcpip-ping-payload-dropped */
    CHECK((st & 1) && rlen == sizeof(PING_PAYLOAD) - 1 &&
          memcmp(reply, PING_PAYLOAD, sizeof(PING_PAYLOAD) - 1) == 0,
          "PING receives the ICMP echo payload BYTE-EXACT from the executive raw socket");

    alarm(0);
    printf("=== test_syssvc_tcpip_ping: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
