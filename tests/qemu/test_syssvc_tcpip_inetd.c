/*
 * test_syssvc_tcpip_inetd.c - the OVMX auxiliary server (TCPIP$INETD-equivalent,
 * rd vms-cdb9) binds a well-known port over the executive BGn: seam, reads a
 * TCPIP$SERVICE.DAT service database, accepts an inbound TCP connection, and
 * SPAWNS the configured service image with the accepted connection as its
 * stdin/stdout -- all against a REAL /dev/vms, atop the proven server veneer
 * (ovmx_bind/listen/accept + ovmx_materialize_fd, vms-698 / vms-0cd).
 *
 * ============================================================
 * WHAT IT PROVES -- the inetd control loop over the executive, end to end.
 * The test parses a TCPIP$SERVICE.DAT record, binds its port with the veneer
 * server path (an executive-resident listener over BGn:), starts a plain
 * userspace client that connects to that port on 127.0.0.1 and sends a
 * message, then the auxiliary-server engine accepts the connection, MATERIALIZES
 * it as a real executive-backed fd, and fork()+execv()s the configured service
 * image with that fd as SYS$INPUT/SYS$OUTPUT. The service image is THIS test
 * binary re-executed with a marker argument (--ovmx-inetd-echo-service): it
 * reads the client's bytes from stdin and writes them back on stdout, and those
 * bytes transit the executive socket (the materialized [bgconn] fd), never a
 * host socketpair. The client reads its message back BYTE-EXACT -- proof that a
 * connection accepted by the auxiliary server reached a SPAWNED service image
 * and round-tripped through the executive. This is the exact mechanism that
 * unblocks the OpenSSH sshd ladder (vms-9ef): VMSSSHD.EXE launched by the
 * auxiliary server.
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is loaded,
 * exactly as test_syssvc_bgsock_echo.c does): the veneer's ovmx_socket() must
 * fail SS$_NOSUCHDEV -> ENODEV, never fabricate a private per-process socket
 * (CLAUDE.md Rule 9 / INV-6). This suite returns EXIT_SKIP (77) there.
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * tcpip-inetd-reply-not-connected disconnects the service image's SYS$OUTPUT
 * from the accepted connection; the accept still fires and the service is still
 * spawned and exits, so only the byte-exact round-trip assertion reddens.
 *
 * WATCHDOG: ovmx_accept() blocks in the executive, so a wedged peer would hang
 * the whole QEMU boot rather than fail one line. alarm() bounds the run.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "vms/pcb.h"
#include "vms_kif.h"
/* The auxiliary-server engine (includes vms_bgsock.h). Included by relative path
 * -- the same idiom test_syssvc_tcpip_ping.c uses for tcpip_ping.h -- because
 * src/vmstcpip/services is not on the QEMU test -I path (only .../sockets is). */
#include "../../src/vmstcpip/services/tcpip_inetd.h"

#define EXIT_SKIP   77
#define INETD_PORT  15007                       /* the well-known port under test */
#define SVC_MARKER  "--ovmx-inetd-echo-service"
static const char CLIENT_MSG[] = "OVMX TCPIP$INETD inbound -- vms-cdb9, first spawned service";

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * SERVICE-IMAGE MODE. When this binary is re-exec'd by the auxiliary server
 * with the marker argument, it IS the spawned service image: it reads the
 * client's bytes from stdin (the accepted connection) and echoes them back on
 * stdout (the same connection), then exits. It touches no VMS API -- it is an
 * ordinary image handed a connection on fd 0/1, exactly as an inetd service is.
 */
static int run_echo_service(void)
{
    char buf[512];
    ssize_t n;
    n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0)
        return 0;
    (void)!write(STDOUT_FILENO, buf, (size_t)n);
    return 0;
}

/* ----------------------------------------------------------------------- */

static void watchdog(int sig)
{
    (void)sig;
    printf("  FAIL: watchdog fired -- auxiliary server accept/spawn wedged\n");
    printf("=== test_syssvc_tcpip_inetd: %d passed, %d failed (WATCHDOG) ===\n",
           pass, fail + 1);
    _exit(1);
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

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

/* The inbound client peer: a PLAIN userspace socket (the loopback other end,
 * exactly as test_syssvc_bg_server's peer). It connects to the auxiliary
 * server's well-known port, sends CLIENT_MSG, and reads the reply. It exits 0
 * IFF the reply came back BYTE-EXACT, so the parent reads the round-trip
 * verdict from the child's exit status. */
static void client_peer(void)
{
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    struct timeval tv;
    char rbuf[512];
    ssize_t got;
    int ok = 0;

    if (c < 0) _exit(2);
    tv.tv_sec = 5; tv.tv_usec = 0;
    (void)setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(INETD_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* The listener may not be ready the instant we fork; retry the connect. */
    {
        int tries;
        for (tries = 0; tries < 50; tries++) {
            if (connect(c, (struct sockaddr *)&sa, sizeof(sa)) == 0)
                break;
            usleep(20000);
        }
    }
    if (write(c, CLIENT_MSG, sizeof(CLIENT_MSG)) == (ssize_t)sizeof(CLIENT_MSG)) {
        got = recv(c, rbuf, sizeof(rbuf), 0);
        if (got == (ssize_t)sizeof(CLIENT_MSG) &&
            memcmp(rbuf, CLIENT_MSG, sizeof(CLIENT_MSG)) == 0)
            ok = 1;
    }
    close(c);
    _exit(ok ? 0 : 1);
}

int main(int argc, char *argv[])
{
    char self[TCPIP_INETD_PATH_MAX];
    ssize_t sl;
    char db[1024];
    struct tcpip_service svcs[TCPIP_INETD_MAX_SERVICES];
    int nsvc, listen_h;
    pid_t client_pid, svc_pid;
    struct sockaddr_in peer;
    int cstatus = -1, sstatus = -1;

    /* SERVICE-IMAGE MODE: re-exec'd by the auxiliary server. */
    if (argc > 1 && strcmp(argv[1], SVC_MARKER) == 0)
        return run_echo_service();

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_tcpip_inetd (auxiliary server binds a well-known port over BGn:, spawns the service image on connect, vms-cdb9) ===\n");

    /* A per-process PCB is a prerequisite for every executive channel call; a
     * gcc test binary must make its own (see test_syssvc_bgsock_echo.c). */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    /* ---- The service database (OVMX TCPIP$SERVICE.DAT text) --------------
     * Built at runtime so the configured service image is THIS binary's real
     * path (self-exec), and the ECHO line carries the service-mode marker as
     * an image argument -- proving both the DB parse and the spawn of a
     * configured image with arguments. */
    sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sl <= 0) {
        /* Fall back to argv[0] if /proc is unavailable. */
        strncpy(self, argv[0], sizeof(self) - 1);
        self[sizeof(self) - 1] = '\0';
    } else {
        self[sl] = '\0';
    }
    snprintf(db, sizeof(db),
             "! TCPIP$SERVICE.DAT (test) -- OVMX text service DB\n"
             "!\n"
             "ECHO  %u  %s  %s\n",
             (unsigned)INETD_PORT, self, SVC_MARKER);

    nsvc = tcpip_inetd_parse_db(db, svcs, TCPIP_INETD_MAX_SERVICES);
    CHECK(nsvc == 1 && svcs[0].port == INETD_PORT &&
          strcmp(svcs[0].name, "ECHO") == 0 &&
          strcmp(svcs[0].image, self) == 0 &&
          strcmp(svcs[0].args, SVC_MARKER) == 0,
          "TCPIP$SERVICE.DAT parses to the ECHO service (port, image, args)");

    if (!executive_present()) {
        /*
         * NO EXECUTIVE: the veneer's ovmx_socket() (and thus the auxiliary
         * server's listen) must fail honestly ENODEV, never a per-process
         * socket (Rule 9 / INV-6). Run on the host before vms.ko.
         */
        errno = 0;
        listen_h = tcpip_inetd_listen(&svcs[0]);
        CHECK(listen_h < 0 && errno == ENODEV,
              "no executive: the auxiliary server's listen fails ENODEV (SS$_NOSUCHDEV), never a local per-process socket");
        printf("=== test_syssvc_tcpip_inetd: %d passed, %d failed (SKIPPED: no /dev/vms -- auxiliary server not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    signal(SIGALRM, watchdog);
    alarm(25);
    bring_lo_up();

    /* ---- The auxiliary server binds the well-known port over BGn: -------- */
    listen_h = tcpip_inetd_listen(&svcs[0]);
    CHECK(listen_h >= 0,
          "the auxiliary server binds the well-known port and listens over the executive BGn: seam");
    if (listen_h < 0) {
        printf("=== test_syssvc_tcpip_inetd: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* Start the inbound client peer AFTER listen so its connect never races an
     * un-listening socket. It runs as a separate process (a plain loopback
     * client) and reports the byte-exact verdict via its exit status. */
    client_pid = fork();
    if (client_pid == 0)
        client_peer();                          /* never returns */
    if (client_pid < 0) {
        printf("  FAIL: fork(client_peer) failed\n");
        return 1;
    }

    /* ---- Accept the inbound connection and SPAWN the service image ------- */
    memset(&peer, 0, sizeof(peer));
    svc_pid = tcpip_inetd_accept_dispatch(listen_h, &svcs[0], &peer);
    CHECK(svc_pid > 0,
          "accept fires on the inbound connect and the auxiliary server spawns the configured service image");

    /* The spawned service image ran and exited cleanly. */
    if (svc_pid > 0)
        waitpid(svc_pid, &sstatus, 0);
    CHECK(svc_pid > 0 && WIFEXITED(sstatus) && WEXITSTATUS(sstatus) == 0,
          "the spawned service image ran on the accepted connection and exited cleanly");

    /* The client got its message back BYTE-EXACT: the accepted connection is
     * the spawned service image's SYS$OUTPUT and the reply transited the
     * executive. (This is the assertion the negctl reddens.) */
    waitpid(client_pid, &cstatus, 0);
    /* negctl: tcpip-inetd-reply-not-connected */
    CHECK(WIFEXITED(cstatus) && WEXITSTATUS(cstatus) == 0,
          "the service's reply came back BYTE-EXACT over the connection -- the accepted connection is the spawned service image's SYS$OUTPUT");

    /* Tear the listener down. */
    (void)ovmx_socket_close(listen_h);
    alarm(0);

    printf("=== test_syssvc_tcpip_inetd: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
