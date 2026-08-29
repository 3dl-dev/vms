/*
 * test_syssvc_tcpip_daytime.c - the TCP/IP auxiliary server (TCPIP$INETD,
 * vms-cdb9) launches a REAL, SEPARATELY-BUILT shipped service image end to end
 * over the executive: TCPIP$DAYTIME.EXE, the RFC 867 Daytime service (rd
 * vms-477).
 *
 * ============================================================
 * WHAT IT PROVES -- the auxiliary server hands an accepted connection to a
 * GENUINE STANDALONE IMAGE whose reply transits the executive fd.
 *
 * The auxiliary-server rung (test_syssvc_tcpip_inetd, vms-cdb9) proved the
 * accept/spawn control loop, but its only "service" was THIS test binary
 * re-exec'd as an echo -- a self-exec, not a separately-shipped image. This rung
 * closes that: the configured service image is the real TCPIP$DAYTIME.EXE built
 * by tools/vms_tcpip_daytime.c and staged at SYS$SYSTEM, a wholly separate
 * program the test does not link, include, or re-exec. The test:
 *   1. binds the service's port with the proven veneer server path (an
 *      executive-resident listener over BGn:, vms-698 / vms-0cd),
 *   2. starts a plain userspace client that connects on 127.0.0.1,
 *   3. lets the auxiliary-server engine accept the connection, MATERIALIZE it as
 *      a real executive-backed fd, and fork()+execv() TCPIP$DAYTIME.EXE with
 *      that fd as SYS$INPUT/SYS$OUTPUT,
 *   4. and asserts the client reads back a WELL-FORMED RFC 867 daytime line --
 *      proof that the separately-built image ran on the accepted connection and
 *      its reply travelled back through the executive [bgconn] fd, never a host
 *      socketpair.
 *
 * WHY A SHAPE ASSERTION, NOT BYTE-EQUALITY. RFC 867 sends the *current* time, so
 * there is no fixed golden to compare against. The test validates the reply's
 * RFC 867 SHAPE (weekday / month / day / HH:MM:SS / 4-digit year / CR LF)
 * INDEPENDENTLY -- it does NOT include the daytime formatter header. That
 * independence is deliberate (the ODS-2-golden lesson, rd vms-dcd): the
 * per-facility negative control (tcpip-daytime-reply-not-formatted) mutates the
 * shipped image's formatter to emit a malformed line; a validator that recomputed
 * the expected string from the same mutated function would corrupt both sides and
 * hide the defect. Here the image's reply is judged against a fixed structural
 * oracle the mutation cannot reach, so the malformed line reddens exactly this
 * assertion.
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is loaded):
 * the veneer's ovmx_socket() must fail SS$_NOSUCHDEV -> ENODEV, never fabricate a
 * private per-process socket (CLAUDE.md Rule 9 / INV-6). This suite returns
 * EXIT_SKIP (77) there.
 *
 * NEGATIVE CONTROL (tests/qemu/facility_defects.sh): tcpip-daytime-reply-not-
 * formatted makes TCPIP$DAYTIME.EXE emit a malformed (non-RFC-867) line; the
 * accept still fires, the image is still spawned and still exits cleanly, so only
 * the well-formed-reply assertion reddens.
 *
 * WATCHDOG: ovmx_accept() blocks in the executive, so a wedged peer would hang
 * the whole QEMU boot rather than fail one line. alarm() bounds the run.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
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
 * -- the same idiom test_syssvc_tcpip_inetd.c / test_syssvc_tcpip_ping.c use --
 * because src/vmstcpip/services is not on the QEMU test -I path. NOTE: this test
 * deliberately does NOT include tcpip_daytime.h; the RFC 867 shape is validated
 * against a fixed oracle below, independent of the shipped image's formatter. */
#include "../../src/vmstcpip/services/tcpip_inetd.h"

#define EXIT_SKIP     77
#define DAYTIME_PORT  15013            /* the test port (the shipped DAT binds 13) */

/* The REAL, separately-built shipped image the auxiliary server spawns. Staged
 * at SYS$SYSTEM by tests/qemu/Dockerfile (and re-staged by inject_and_run.sh);
 * overridable so the suite is runnable from a plain build tree too. */
#define DAYTIME_IMAGE_DEFAULT \
    "/vms/SYS0/SYSCOMMON/SYSEXE/TCPIP$DAYTIME.EXE"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * RFC 867 SHAPE ORACLE (independent of the shipped image's formatter).
 * Accepts the ctime-style line OVMX emits: "Www Mmm dd hh:mm:ss yyyy\r\n"
 *   - weekday:  3 alpha
 *   - month:    3 alpha
 *   - day:      1-2 digits (space- or zero-padded)
 *   - time:     hh:mm:ss (two colons, digits around them)
 *   - year:     4 digits
 *   - ends CR LF
 * A malformed line (the negctl's "X\r\n", an empty reply, a truncated line)
 * fails this. Returns 1 if well-formed, 0 otherwise.
 */
static int is_rfc867_daytime(const char *s, size_t len)
{
    size_t i;
    int alpha_run, colons = 0, digits = 0, year_digits = 0;

    if (len < 20)                        /* "Www Mmm d h:m:s yyyy\r\n" floor */
        return 0;
    /* Must terminate CR LF. */
    if (s[len - 2] != '\r' || s[len - 1] != '\n')
        return 0;

    /* First token: exactly 3 alpha (weekday), then a space. */
    for (i = 0, alpha_run = 0; i < len && isalpha((unsigned char)s[i]); i++)
        alpha_run++;
    if (alpha_run != 3 || i >= len || s[i] != ' ')
        return 0;
    i++;
    /* Second token: exactly 3 alpha (month), then a space. */
    for (alpha_run = 0; i < len && isalpha((unsigned char)s[i]); i++)
        alpha_run++;
    if (alpha_run != 3 || i >= len || s[i] != ' ')
        return 0;

    /* Scan the remainder (up to the CR): count ':' (need 2 for hh:mm:ss) and
     * verify the trailing 4-digit year sits just before CR LF. */
    for (; i < len - 2; i++) {
        if (s[i] == ':')
            colons++;
        else if (isdigit((unsigned char)s[i]))
            digits++;
        else if (s[i] != ' ')
            return 0;                    /* only digits/colons/spaces past month */
    }
    if (colons != 2 || digits < 6)       /* hh mm ss (6) + day + year */
        return 0;

    /* Trailing 4 chars before CR LF must be the year (all digits). */
    for (i = len - 6; i < len - 2; i++)  /* len-2 = CR; year is [len-6, len-2) */
        if (isdigit((unsigned char)s[i]))
            year_digits++;
    return year_digits == 4;
}

/* ----------------------------------------------------------------------- */

static void watchdog(int sig)
{
    (void)sig;
    printf("  FAIL: watchdog fired -- auxiliary server accept/spawn wedged\n");
    printf("=== test_syssvc_tcpip_daytime: %d passed, %d failed (WATCHDOG) ===\n",
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

/* The inbound client peer: a PLAIN userspace socket (the loopback other end).
 * RFC 867 is a server-speaks-first protocol -- the client connects and READS the
 * daytime line (it sends nothing; the server throws client input away). It exits
 * 0 IFF the reply is a well-formed RFC 867 daytime line, so the parent reads the
 * verdict from the child's exit status. */
static void client_peer(void)
{
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    struct timeval tv;
    char rbuf[512];
    size_t off = 0;
    int ok = 0, tries;

    if (c < 0) _exit(2);
    tv.tv_sec = 5; tv.tv_usec = 0;
    (void)setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(DAYTIME_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* The listener may not be ready the instant we fork; retry the connect. */
    for (tries = 0; tries < 50; tries++) {
        if (connect(c, (struct sockaddr *)&sa, sizeof(sa)) == 0)
            break;
        usleep(20000);
    }

    /* Read the daytime line: accumulate until CR LF, EOF, or the buffer fills. */
    for (;;) {
        ssize_t got = recv(c, rbuf + off, sizeof(rbuf) - 1 - off, 0);
        if (got <= 0)
            break;                       /* EOF (server closed) or timeout */
        off += (size_t)got;
        if (off >= sizeof(rbuf) - 1)
            break;
        if (off >= 2 && rbuf[off - 2] == '\r' && rbuf[off - 1] == '\n')
            break;
    }
    rbuf[off] = '\0';
    ok = is_rfc867_daytime(rbuf, off);
    close(c);
    _exit(ok ? 0 : 1);
}

int main(void)
{
    const char *image = getenv("OVMX_DAYTIME");
    char db[1024];
    struct tcpip_service svcs[TCPIP_INETD_MAX_SERVICES];
    int nsvc, listen_h;
    pid_t client_pid, svc_pid;
    struct sockaddr_in peer;
    int cstatus = -1, sstatus = -1;

    if (!image || !*image)
        image = DAYTIME_IMAGE_DEFAULT;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_tcpip_daytime (auxiliary server launches the REAL standalone TCPIP$DAYTIME.EXE, RFC 867, over the executive, vms-477) ===\n");

    /* A per-process PCB is a prerequisite for every executive channel call. */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    /* ---- The service database (OVMX TCPIP$SERVICE.DAT text) --------------
     * Built at runtime so the configured service image is the REAL shipped
     * TCPIP$DAYTIME.EXE path (no self-exec, no args -- RFC 867 takes none). */
    snprintf(db, sizeof(db),
             "! TCPIP$SERVICE.DAT (test) -- OVMX text service DB\n"
             "!\n"
             "DAYTIME  %u  %s\n",
             (unsigned)DAYTIME_PORT, image);

    nsvc = tcpip_inetd_parse_db(db, svcs, TCPIP_INETD_MAX_SERVICES);
    CHECK(nsvc == 1 && svcs[0].port == DAYTIME_PORT &&
          strcmp(svcs[0].name, "DAYTIME") == 0 &&
          strcmp(svcs[0].image, image) == 0,
          "TCPIP$SERVICE.DAT parses to the DAYTIME service (port 13-class, real image path)");

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
        printf("=== test_syssvc_tcpip_daytime: %d passed, %d failed (SKIPPED: no /dev/vms -- auxiliary server not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* The shipped image must actually be present, or the spawn would fail exec
     * and the round-trip assertion would go red for the wrong reason (missing
     * subject, not a real defect). Absence is a hard setup failure. */
    if (access(image, X_OK) != 0) {
        printf("  FAIL: the shipped service image %s is not present/executable (%s) -- staging gap, not a facility defect\n",
               image, strerror(errno));
        printf("=== test_syssvc_tcpip_daytime: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    signal(SIGALRM, watchdog);
    alarm(25);
    bring_lo_up();

    /* ---- The auxiliary server binds the well-known port over BGn: -------- */
    listen_h = tcpip_inetd_listen(&svcs[0]);
    CHECK(listen_h >= 0,
          "the auxiliary server binds the well-known port and listens over the executive BGn: seam");
    if (listen_h < 0) {
        printf("=== test_syssvc_tcpip_daytime: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* Start the inbound client peer AFTER listen so its connect never races an
     * un-listening socket. */
    client_pid = fork();
    if (client_pid == 0)
        client_peer();                          /* never returns */
    if (client_pid < 0) {
        printf("  FAIL: fork(client_peer) failed\n");
        return 1;
    }

    /* ---- Accept the inbound connection and SPAWN the standalone image ---- */
    memset(&peer, 0, sizeof(peer));
    svc_pid = tcpip_inetd_accept_dispatch(listen_h, &svcs[0], &peer);
    CHECK(svc_pid > 0,
          "accept fires on the inbound connect and the auxiliary server spawns the REAL standalone TCPIP$DAYTIME.EXE");

    /* The spawned standalone image ran and exited cleanly. */
    if (svc_pid > 0)
        waitpid(svc_pid, &sstatus, 0);
    CHECK(svc_pid > 0 && WIFEXITED(sstatus) && WEXITSTATUS(sstatus) == 0,
          "the spawned standalone image ran on the accepted connection and exited cleanly");

    /* The client got a WELL-FORMED RFC 867 daytime line: the separately-built
     * image's reply transited the executive [bgconn] fd back to the client.
     * (This is the assertion the negctl reddens.) */
    waitpid(client_pid, &cstatus, 0);
    /* negctl: tcpip-daytime-reply-not-formatted */
    CHECK(WIFEXITED(cstatus) && WEXITSTATUS(cstatus) == 0,
          "the separately-built image's reply is a well-formed RFC 867 daytime line transited over the executive connection");

    /* Tear the listener down. */
    (void)ovmx_socket_close(listen_h);
    alarm(0);

    printf("=== test_syssvc_tcpip_daytime: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
