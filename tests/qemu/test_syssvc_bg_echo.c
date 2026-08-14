/*
 * test_syssvc_bg_echo.c - a VMS program opens a TCP connection through the
 * INET pseudo-device BGn: and round-trips a loopback echo, all via the PUBLIC
 * $ASSIGN / $QIO / $DASSGN system services against a REAL /dev/vms (vms-527).
 *
 * ============================================================
 * THE POINT OF vms-527 -- THE FIRST NETWORK CONNECTION IN OVMX. On OpenVMS a
 * program reaches TCP the ordinary VMS way: $ASSIGN a channel to the INET
 * device (TCPIP$DEVICE: / BGn:), then $QIO on that channel -- IO$_SETMODE
 * creates the socket, IO$_ACCESS connects, IO$_WRITEVBLK / IO$_READVBLK move
 * data, IO$_DEACCESS shuts down. BGn: is a KERNEL-MODE DEVICE DRIVER in the
 * executive (src/kernel/vms_bg.c): the socket is executive-resident and $QIO
 * routes through vms.ko into the HOST kernel's in-kernel socket API, the direct
 * analogue of the executive-resident mailbox (socket-for-queue). There is NO
 * userspace socket -- exactly as VMS's BGn: is a driver, not a library.
 *
 * This suite drives that path end to end: it $ASSIGNs TCPIP$DEVICE:, creates
 * the socket, connects to a 127.0.0.1 echo peer it spins up in-process, writes
 * a message, reads the echo back BYTE-EXACT, deaccesses and deassigns. The
 * peer is a plain userspace listener (the loopback other end); the CLIENT side
 * -- the whole $QIO path -- is executive/in-kernel. It needs no NIC and runs
 * under `-nic none`: 127.0.0.1 is the kernel loopback, which this test brings
 * up itself (init.sh does not).
 * ============================================================
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is loaded,
 * exactly as test_syssvc_mbx_crossproc.c does): $ASSIGN TCPIP$DEVICE: must fail
 * SS$_NOSUCHDEV, never fabricate a private per-process socket (CLAUDE.md Rule 9
 * / INV-6). A test_syssvc_* suite that returned 0 with no executive would be
 * claiming a pass it never earned; this one returns EXIT_SKIP (77).
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * the byte-exact echo assertion is anchored by the bg-recv-length-zeroed
 * defect, which zeroes the received length in the BGn: driver's IO$_READVBLK
 * handler (kernel/vms_bg.c) -- so the echo comes back with the wrong byte
 * count and only that assertion reddens, while the assign / setmode / connect /
 * write assertions stay green.
 *
 * WATCHDOG: a blocking $QIO recv lives in the executive, so a wedged peer would
 * hang the whole QEMU boot rather than fail one line. alarm() bounds the run:
 * on timeout the test prints a FAIL line and exits, so a wedge is a named
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

#define EXIT_SKIP 77

/* Short enough to be a single loopback segment (one recv returns it whole). */
static const char MSG[] = "OVMX BGn: loopback echo -- vms-527, the first network connection";

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_bg_echo timed out (peer/echo wedge)\n";
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

/* Same skip-vs-run decision as test_syssvc_mbx_crossproc.c: open only to
 * decide, never register by hand (the sys$ services bind lazily). */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* Bring the kernel loopback interface up -- init.sh does not, and a `-nic none`
 * guest boots with lo DOWN, so 127.0.0.1 would be unreachable to both the
 * userspace peer and the executive's in-kernel client. A raw SIOCSIFFLAGS is
 * self-contained (no dependency on a busybox `ip`/`ifconfig` applet). */
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

/* The loopback echo peer: accept one connection, read one buffer, write it
 * straight back, close. The listening fd is created before the thread starts
 * (so the client's connect never races an unbound port), and handed here. */
static void *echo_peer(void *arg)
{
    int lsock = *(int *)arg;
    int c = accept(lsock, NULL, NULL);
    if (c >= 0) {
        char buf[256];
        ssize_t n = recv(c, buf, sizeof(buf), 0);
        if (n > 0)
            (void)!write(c, buf, (size_t)n);   /* send() the bytes back */
        close(c);
    }
    close(lsock);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_bg_echo (VMS program opens a TCP echo through BGn: $QIO, vms-527) ===\n");

    /* A per-process PCB is a prerequisite for every sys$ channel call (the
     * channel table lives in it); a gcc test binary must make its own -- see
     * test_syssvc_mbx_crossproc.c. Needed on BOTH branches below. */
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
        printf("=== test_syssvc_bg_echo: %d passed, %d failed (SKIPPED: no /dev/vms -- TCP echo not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    signal(SIGALRM, watchdog);
    alarm(20);

    bring_lo_up();

    /* Loopback echo peer: bind 127.0.0.1:0, listen, learn the port, then start
     * the accept/echo thread. Creating and listening BEFORE the client connects
     * means the connect cannot race an unbound port. */
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) { printf("  FAIL: peer socket() failed\n"); return 1; }
    int one = 1;
    (void)setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 127.0.0.1 */
    la.sin_port = 0;                                /* ephemeral */
    if (bind(lsock, (struct sockaddr *)&la, sizeof(la)) < 0) {
        printf("  FAIL: peer bind(127.0.0.1) failed\n"); return 1;
    }
    if (listen(lsock, 1) < 0) { printf("  FAIL: peer listen() failed\n"); return 1; }

    socklen_t sl = sizeof(la);
    if (getsockname(lsock, (struct sockaddr *)&la, &sl) < 0) {
        printf("  FAIL: peer getsockname() failed\n"); return 1;
    }
    uint16_t peer_port = ntohs(la.sin_port);

    pthread_t th;
    if (pthread_create(&th, NULL, echo_peer, &lsock) != 0) {
        printf("  FAIL: pthread_create(echo_peer) failed\n"); return 1;
    }

    /* ----- the VMS program: $ASSIGN BGn: and $QIO connect/send/recv ----- */
    uint16_t chan = 0;
    struct dsc$descriptor_s dev = mkdsc("TCPIP$DEVICE:");
    uint32_t ast = sys$assign(&dev, &chan, 0, NULL);
    CHECK(ast & 1, "$ASSIGN TCPIP$DEVICE: returns a BG channel");

    struct _iosb smi = {0};
    uint32_t sm = sys$qiow(0, chan, IO$_SETMODE, &smi, NULL, 0,
                           NULL, 0, 0, 0, 0, 0);
    CHECK(sm & 1, "$QIO IO$_SETMODE creates the executive-resident socket");

    /* IO$_ACCESS = connect. P1 = sockaddr (family/port/addr), P2 = its length;
     * struct sockaddr_in's first eight bytes are exactly what the driver reads. */
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(peer_port);
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct _iosb ci = {0};
    uint32_t cst = sys$qiow(0, chan, IO$_ACCESS, &ci, NULL, 0,
                            &peer, (uint32_t)sizeof(peer), 0, 0, 0, 0);
    CHECK(cst & 1, "$QIO IO$_ACCESS connects to the 127.0.0.1 echo peer");

    struct _iosb wi = {0};
    uint32_t wst = sys$qiow(0, chan, IO$_WRITEVBLK, &wi, NULL, 0,
                            (void *)MSG, (uint32_t)sizeof(MSG), 0, 0, 0, 0);
    CHECK((wst & 1) && wi.iosb$w_bcnt == (uint16_t)sizeof(MSG),
          "$QIO IO$_WRITEVBLK sends the message over the connection");

    char rbuf[256];
    memset(rbuf, 0, sizeof(rbuf));
    struct _iosb ri = {0};
    uint32_t rst = sys$qiow(0, chan, IO$_READVBLK, &ri, NULL, 0,
                            rbuf, (uint32_t)sizeof(rbuf), 0, 0, 0, 0);
    /* negctl: bg-recv-length-zeroed */
    CHECK((rst & 1) && ri.iosb$w_bcnt == (uint16_t)sizeof(MSG) &&
          memcmp(rbuf, MSG, sizeof(MSG)) == 0,
          "BG $QIO IO$_READVBLK returns the exact bytes the echo peer sent back");

    struct _iosb di = {0};
    uint32_t dst = sys$qiow(0, chan, IO$_DEACCESS, &di, NULL, 0,
                            NULL, 0, 0, 0, 0, 0);
    CHECK(dst & 1, "$QIO IO$_DEACCESS shuts the connection down");

    uint32_t das = sys$dassgn(chan);
    CHECK(das & 1, "$DASSGN releases the BG channel and its executive socket");

    pthread_join(th, NULL);
    alarm(0);

    printf("=== test_syssvc_bg_echo: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
