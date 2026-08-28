/*
 * test_syssvc_bg_server.c - a VMS program is a TCP SERVER through the INET
 * pseudo-device BGn:: it $ASSIGNs a channel, $QIOs bind + listen, $QIOs an
 * accept that hands the inbound connection to a SECOND BG channel, and moves
 * bytes over that accepted channel BYTE-EXACT -- all via the PUBLIC $ASSIGN /
 * $QIO / $DASSGN system services against a REAL /dev/vms (vms-698).
 *
 * ============================================================
 * THE POINT OF vms-698 -- THE SERVER HALF OF THE FIRST NETWORK SEAM. vms-527
 * landed the BGn: CLIENT path (create/setmode/connect/send/recv/close). This
 * suite proves the INBOUND half a server needs -- and that sshd (vms-0cd) and
 * inetd (vms-3bf) are unblocked on: a VMS program reaches TCP the ordinary VMS
 * way, $ASSIGN a channel to TCPIP$DEVICE:, then $QIO on it -- IO$_SETMODE
 * creates the socket, IO$_SETMODE with a local address BINDs it, IO$_SETMODE
 * with a backlog LISTENs, IO$_ACCESS|IO$M_ACCEPT ACCEPTs an inbound connection
 * onto a second BG channel, and IO$_READVBLK / IO$_WRITEVBLK move data over
 * that accepted channel (the vms-527 path, unchanged). BGn: is a KERNEL-MODE
 * DEVICE DRIVER in the executive (src/kernel/vms_bg.c): every socket -- the
 * listener AND the accepted connection -- is executive-resident and $QIO routes
 * through vms.ko into the HOST kernel's in-kernel socket API. There is NO
 * userspace socket on the VMS side; the accept is the executive-resident
 * analogue of the mailbox handing a queued message to another channel.
 *
 * This suite drives that path end to end: it $ASSIGNs TCPIP$DEVICE:, creates +
 * binds (port 0 -> the executive reads the ephemeral port back) + listens, then
 * spins up a plain userspace client that connects to 127.0.0.1 on the learned
 * port and sends a message; the VMS program accepts onto a second channel,
 * reads the client's bytes back BYTE-EXACT, writes a reply, and tears both
 * channels down. The client peer is the loopback other end; the SERVER side --
 * bind/listen/accept and the accepted-channel I/O -- is executive/in-kernel. It
 * needs no NIC and runs under `-nic none`: 127.0.0.1 is the kernel loopback,
 * which this test brings up itself (init.sh does not).
 * ============================================================
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is loaded,
 * exactly as test_syssvc_bg_echo.c does): $ASSIGN TCPIP$DEVICE: must fail
 * SS$_NOSUCHDEV, never fabricate a private per-process socket (CLAUDE.md Rule 9
 * / INV-6). This suite returns EXIT_SKIP (77) there.
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * the two accepted-channel assertions are anchored by the
 * bg-accept-socket-not-installed defect, which makes vms_ioctl_bg_accept report
 * success but NOT install the accepted socket onto the target channel -- so the
 * accept still returns SS$_NORMAL while the read and write on the accepted
 * channel both fail SS$_IVCHAN, reddening exactly those two assertions while the
 * assign / setmode / bind / listen / accept-status assertions (and the
 * no-executive honest-skip) all stay green.
 *
 * WATCHDOG: a blocking $QIO accept/recv lives in the executive, so a wedged peer
 * would hang the whole QEMU boot rather than fail one line. alarm() bounds the
 * run: on timeout the test prints a FAIL line and exits, so a wedge is a named
 * failure inside run_tests.sh's budget, not a harness-wide timeout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include "starlet.h"
#include "descrip.h"
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

/* Short enough to be a single loopback segment (one recv returns it whole). */
static const char MSG[]  = "OVMX BGn: inbound -- vms-698, the first accepted connection";
static const char REPLY[] = "OVMX BGn: server reply over the accepted channel";

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_bg_server timed out (accept/peer wedge)\n";
    (void)!write(1, m, sizeof(m) - 1);
    _exit(1);
}

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* Same skip-vs-run decision as test_syssvc_bg_echo.c: open only to decide,
 * never register by hand (the sys$ services bind lazily). */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* Bring the kernel loopback interface up -- init.sh does not, and a `-nic none`
 * guest boots with lo DOWN, so 127.0.0.1 would be unreachable. A raw
 * SIOCSIFFLAGS is self-contained (no dependency on a busybox applet). */
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

/* The inbound client peer: connect to the VMS server's loopback port, send one
 * message, then read the server's reply (bounded by a socket receive timeout so
 * that if the server never replies -- e.g. under the negative control -- this
 * thread does not wedge). The port is learned by the server from its own bind
 * and handed here. */
static uint16_t g_peer_port;

static void *client_peer(void *arg)
{
    (void)arg;
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    struct timeval tv;
    char rbuf[256];

    if (c < 0) return NULL;

    tv.tv_sec = 3; tv.tv_usec = 0;              /* never block forever */
    (void)setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(g_peer_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(c, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        (void)!write(c, MSG, sizeof(MSG));      /* send() the message */
        (void)recv(c, rbuf, sizeof(rbuf), 0);   /* drain the reply (not asserted) */
    }
    close(c);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_bg_server (VMS program is a TCP server through BGn: $QIO, vms-698) ===\n");

    /* A per-process PCB is a prerequisite for every sys$ channel call; a gcc
     * test binary must make its own -- see test_syssvc_bg_echo.c. Needed on
     * BOTH branches below. */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        /*
         * NO EXECUTIVE: $ASSIGN TCPIP$DEVICE: must fail honestly, never a
         * private per-process socket (CLAUDE.md Rule 9 / INV-6). Run on the
         * host before vms.ko.
         */
        uint16_t chan = 0;
        struct dsc$descriptor_s dev = mkdsc("TCPIP$DEVICE:");
        uint32_t st = sys$assign(&dev, &chan, 0, NULL);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: $ASSIGN TCPIP$DEVICE: fails SS$_NOSUCHDEV, never a local per-process socket");
        printf("=== test_syssvc_bg_server: %d passed, %d failed (SKIPPED: no /dev/vms -- TCP server not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    signal(SIGALRM, watchdog);
    alarm(20);

    bring_lo_up();

    /* ----- the VMS program: $ASSIGN BGn:, then $QIO bind/listen/accept ----- */
    uint16_t lchan = 0;
    struct dsc$descriptor_s dev = mkdsc("TCPIP$DEVICE:");
    uint32_t ast = sys$assign(&dev, &lchan, 0, NULL);
    CHECK(ast & 1, "$ASSIGN TCPIP$DEVICE: returns a listening BG channel");

    /* Create the socket: IO$_SETMODE with no address (the vms-527 client shape,
     * unchanged). */
    struct _iosb smi = {0};
    uint32_t sm = sys$qiow(0, lchan, IO$_SETMODE, &smi, NULL, 0,
                           NULL, 0, 0, 0, 0, 0);
    CHECK(sm & 1, "$QIO IO$_SETMODE creates the executive-resident socket");

    /* Bind to 127.0.0.1:0 (ephemeral). IO$_SETMODE with P1 = local sockaddr:
     * the executive binds and writes the EFFECTIVE local address back into P1,
     * so the program learns the port the host kernel chose. */
    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_port = 0;                                /* ephemeral */
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);    /* 127.0.0.1 */
    struct _iosb bi = {0};
    uint32_t bst = sys$qiow(0, lchan, IO$_SETMODE, &bi, NULL, 0,
                            &la, (uint32_t)sizeof(la), 0, 0, 0, 0);
    CHECK((bst & 1) && la.sin_port != 0,
          "$QIO IO$_SETMODE binds the channel to 127.0.0.1 and returns the ephemeral port");
    g_peer_port = ntohs(la.sin_port);

    /* Listen: IO$_SETMODE with P3 = backlog. */
    struct _iosb li = {0};
    uint32_t lst = sys$qiow(0, lchan, IO$_SETMODE, &li, NULL, 0,
                            NULL, 0, 5, 0, 0, 0);
    CHECK(lst & 1, "$QIO IO$_SETMODE puts the channel into the LISTEN state");

    /* Now the inbound client peer: it connects to the port we just bound and
     * sends a message. Started AFTER listen so the connect never races an
     * un-listening socket. */
    pthread_t th;
    if (pthread_create(&th, NULL, client_peer, NULL) != 0) {
        printf("  FAIL: pthread_create(client_peer) failed\n"); return 1;
    }

    /* A SECOND BG channel to receive the accepted connection -- freshly
     * $ASSIGNed, no socket of its own. */
    uint16_t achan = 0;
    uint32_t ast2 = sys$assign(&dev, &achan, 0, NULL);
    CHECK(ast2 & 1, "$ASSIGN TCPIP$DEVICE: returns a second BG channel for the accept");

    /* Accept: IO$_ACCESS|IO$M_ACCEPT on the LISTENING channel, P1 = a
     * sockaddr to receive the peer address, P3 = the accept-target channel.
     * Blocks in the executive until the client connects. */
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    struct _iosb ai = {0};
    uint32_t acc = sys$qiow(0, lchan, IO$_ACCESS | IO$M_ACCEPT, &ai, NULL, 0,
                            &peer, (uint32_t)sizeof(peer), achan, 0, 0, 0);
    CHECK(acc & 1, "$QIO IO$_ACCESS|IO$M_ACCEPT accepts the inbound connection onto the second channel");

    /* Read the client's bytes over the ACCEPTED channel (the vms-527 path,
     * unchanged) -- byte-exact. */
    char rbuf[256];
    memset(rbuf, 0, sizeof(rbuf));
    struct _iosb ri = {0};
    uint32_t rst = sys$qiow(0, achan, IO$_READVBLK, &ri, NULL, 0,
                            rbuf, (uint32_t)sizeof(rbuf), 0, 0, 0, 0);
    /* negctl: bg-accept-socket-not-installed */
    CHECK((rst & 1) && ri.iosb$w_bcnt == (uint16_t)sizeof(MSG) &&
          memcmp(rbuf, MSG, sizeof(MSG)) == 0,
          "the accepted BG channel returns the exact bytes the inbound client sent");

    /* Write a reply back over the ACCEPTED channel (proves it is fully usable,
     * not read-only). */
    struct _iosb wi = {0};
    uint32_t wst = sys$qiow(0, achan, IO$_WRITEVBLK, &wi, NULL, 0,
                            (void *)REPLY, (uint32_t)sizeof(REPLY), 0, 0, 0, 0);
    /* negctl: bg-accept-socket-not-installed */
    CHECK((wst & 1) && wi.iosb$w_bcnt == (uint16_t)sizeof(REPLY),
          "the accepted BG channel sends a reply back to the inbound client");

    /* Tear both channels down. */
    struct _iosb di = {0};
    (void)sys$qiow(0, achan, IO$_DEACCESS, &di, NULL, 0, NULL, 0, 0, 0, 0, 0);
    uint32_t da2 = sys$dassgn(achan);
    CHECK(da2 & 1, "$DASSGN releases the accepted BG channel and its executive socket");

    struct _iosb di2 = {0};
    (void)sys$qiow(0, lchan, IO$_DEACCESS, &di2, NULL, 0, NULL, 0, 0, 0, 0, 0);
    uint32_t da1 = sys$dassgn(lchan);
    CHECK(da1 & 1, "$DASSGN releases the listening BG channel and its executive socket");

    pthread_join(th, NULL);
    alarm(0);

    printf("=== test_syssvc_bg_server: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
