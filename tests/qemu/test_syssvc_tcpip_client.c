/*
 * test_syssvc_tcpip_client.c - the DCL TCP/IP client tools (TELNET, FTP) drive
 * real connections over the INET pseudo-device BGn: (vms-527) via the PUBLIC
 * $ASSIGN / $QIO / $DASSGN system services against a REAL /dev/vms (vms-dbb).
 *
 * ============================================================
 * WHAT THIS PROVES. vms-527 gave OVMX its first network connection: a VMS
 * program reaches TCP the ordinary VMS way ($ASSIGN TCPIP$DEVICE:, $QIO on the
 * channel), with the socket EXECUTIVE-RESIDENT in vms.ko over the host in-kernel
 * socket API -- no userspace socket stack. vms-dbb builds the CLIENT TOOLS on
 * that seam: TELNET and FTP client protocol engines (src/vmstcpip/services/
 * tcpip_client.h) that the DCL TELNET/FTP verbs ship and this suite proves. The
 * SAME header is #included by src/vmsdcl/dcl_cmd_misc.c (the verbs) and by this
 * test (the proof), so the shipped code and the proven code are identical.
 *
 * This suite drives that path end to end against loopback peers it spins up
 * in-process (127.0.0.1, the kernel loopback, which this test brings up itself):
 *
 *   TELNET -- connect through BGn:, handle inline IAC option negotiation
 *             (refuse every DO/WILL), read the server banner with the telnet
 *             control bytes stripped, send a line, read it echoed back.
 *   FTP    -- connect a control channel, log in (USER/PASS), TYPE I, PASV,
 *             RETR a file and receive it BYTE-EXACT over the PASV data channel;
 *             then STOR a file and have the server receive it BYTE-EXACT.
 *
 * The peers are plain userspace listeners (the loopback other end, exactly like
 * test_syssvc_bg_echo.c's echo_peer); the CLIENT side -- the whole $QIO path --
 * is executive/in-kernel. No NIC is needed; this runs under `-nic none`.
 * ============================================================
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is loaded,
 * exactly as test_syssvc_bg_echo.c does): $ASSIGN TCPIP$DEVICE: must fail
 * SS$_NOSUCHDEV, never fabricate a private per-process socket (CLAUDE.md Rule 9
 * / INV-6). This suite returns EXIT_SKIP (77) with no /dev/vms.
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * the FTP byte-exact RETR assertion is anchored by the
 * tcpip-ftp-get-length-dropped defect, which drops the accumulated received
 * length in tcpip_ftp_get()'s data-drain loop (src/vmstcpip/services/
 * tcpip_client.h) -- so RETR returns zero bytes and only the byte-exact-file
 * assertions redden, while the connect / login / TELNET assertions stay green.
 *
 * WATCHDOG: a blocking $QIO recv lives in the executive, so a wedged peer would
 * hang the whole QEMU boot. alarm() bounds the run: on timeout the test prints a
 * FAIL line and exits, a named failure inside run_tests.sh's budget.
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

/* The client tool engines under test -- the SAME header the DCL verbs ship. */
#include "../../src/vmstcpip/services/tcpip_client.h"

#define EXIT_SKIP 77

static const char TELNET_BANNER[] = "OVMX-TELNETD READY";
static const char TELNET_LINE[]   = "PING OVMX\r\n";
static const char FTP_FILE_BODY[] =
    "OVMX FTP payload -- vms-dbb, RETR/STOR byte-exact over BGn: PASV data.\n"
    "line two of the transferred file, to span more than one recv.\n";

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_tcpip_client timed out (peer wedge)\n";
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

/* `-nic none` boots with lo DOWN; 127.0.0.1 is unreachable until it is up. A
 * raw SIOCSIFFLAGS is self-contained (no busybox applet dependency). */
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

/* Bind 127.0.0.1:0, listen, and hand back the ephemeral port chosen. */
static int listen_loopback(uint16_t *port_out)
{
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t sl = sizeof(a);
    int one = 1;
    if (lsock < 0) return -1;
    (void)setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(lsock, (struct sockaddr *)&a, sizeof(a)) < 0) { close(lsock); return -1; }
    if (listen(lsock, 1) < 0) { close(lsock); return -1; }
    if (getsockname(lsock, (struct sockaddr *)&a, &sl) < 0) { close(lsock); return -1; }
    *port_out = ntohs(a.sin_port);
    return lsock;
}

static void send_str(int fd, const char *s)
{
    (void)!write(fd, s, strlen(s));
}

/* ============ TELNET server mock (the loopback other end) ============ */
/*
 * On accept: offer two options (IAC WILL ECHO, IAC DO SGA) and a banner, then
 * read the client's stream stripping telnet control (the client's option
 * REFUSALS) to recover the one application line it sent, and echo that line
 * back as plain data. Proves the client both refused the options (no telnet
 * control leaked into the recovered line) and moved application data both ways.
 */
static void *telnet_mock(void *arg)
{
    int lsock = *(int *)arg;
    int c = accept(lsock, NULL, NULL);
    if (c >= 0) {
        unsigned char nego[] = { 255,251,1, 255,253,3 }; /* IAC WILL ECHO, IAC DO SGA */
        char appline[128];
        size_t an = 0;
        int state = 0;   /* 0=data, 1=saw IAC, 2=saw cmd (expect option byte) */

        (void)!write(c, nego, sizeof(nego));
        send_str(c, TELNET_BANNER);
        send_str(c, "\r\n");

        for (;;) {
            unsigned char b;
            ssize_t n = recv(c, &b, 1, 0);
            if (n <= 0) break;
            if (state == 1) { state = 2; continue; }       /* command byte */
            if (state == 2) { state = 0; continue; }       /* option byte  */
            if (b == 255) { state = 1; continue; }         /* IAC */
            if (an + 1 < sizeof(appline)) appline[an++] = (char)b;
            if (b == '\n') break;
        }
        appline[an] = '\0';
        (void)!write(c, appline, an);                      /* echo app line back */
        close(c);
    }
    close(lsock);
    return NULL;
}

/* ============ FTP server mock (the loopback other end) ============ */
struct ftpd_ctx {
    int      lsock;                 /* control listener */
    unsigned char store[4096];      /* bytes received by a STOR */
    uint32_t store_len;
    int      got_stor;
};

/* Read one CRLF-terminated command line from the control socket. */
static int ftpd_readline(int fd, char *buf, size_t sz)
{
    size_t n = 0;
    for (;;) {
        char ch;
        ssize_t r = recv(fd, &ch, 1, 0);
        if (r <= 0) return (n > 0) ? (int)n : -1;
        if (ch == '\r') continue;
        if (ch == '\n') break;
        if (n + 1 < sz) buf[n++] = ch;
    }
    buf[n] = '\0';
    return (int)n;
}

static void *ftpd_mock(void *arg)
{
    struct ftpd_ctx *ctx = (struct ftpd_ctx *)arg;
    int c = accept(ctx->lsock, NULL, NULL);
    int data_listen = -1;
    uint16_t data_port = 0;

    if (c < 0) { close(ctx->lsock); return NULL; }

    send_str(c, "220 OVMX-FTPD (test peer) ready\r\n");

    for (;;) {
        char cmd[600];
        int n = ftpd_readline(c, cmd, sizeof(cmd));
        if (n < 0) break;

        if (strncmp(cmd, "USER", 4) == 0) {
            send_str(c, "331 need password\r\n");
        } else if (strncmp(cmd, "PASS", 4) == 0) {
            send_str(c, "230 logged in\r\n");
        } else if (strncmp(cmd, "TYPE", 4) == 0) {
            send_str(c, "200 type set to I\r\n");
        } else if (strncmp(cmd, "PASV", 4) == 0) {
            char rep[96];
            if (data_listen >= 0) close(data_listen);
            data_listen = listen_loopback(&data_port);
            if (data_listen < 0) { send_str(c, "425 cannot open data port\r\n"); continue; }
            /* 127,0,0,1 and the port split into two bytes (RFC 959). */
            snprintf(rep, sizeof(rep),
                     "227 Entering Passive Mode (127,0,0,1,%u,%u)\r\n",
                     (unsigned)(data_port >> 8), (unsigned)(data_port & 0xff));
            send_str(c, rep);
        } else if (strncmp(cmd, "RETR", 4) == 0) {
            int d;
            if (data_listen < 0) { send_str(c, "425 no data connection\r\n"); continue; }
            send_str(c, "150 opening data connection\r\n");
            d = accept(data_listen, NULL, NULL);
            if (d >= 0) {
                (void)!write(d, FTP_FILE_BODY, sizeof(FTP_FILE_BODY) - 1);
                close(d);
            }
            close(data_listen); data_listen = -1;
            send_str(c, "226 transfer complete\r\n");
        } else if (strncmp(cmd, "STOR", 4) == 0) {
            int d;
            if (data_listen < 0) { send_str(c, "425 no data connection\r\n"); continue; }
            send_str(c, "150 ok to receive data\r\n");
            d = accept(data_listen, NULL, NULL);
            if (d >= 0) {
                for (;;) {
                    ssize_t r = recv(d, ctx->store + ctx->store_len,
                                     sizeof(ctx->store) - ctx->store_len, 0);
                    if (r <= 0) break;
                    ctx->store_len += (uint32_t)r;
                    if (ctx->store_len >= sizeof(ctx->store)) break;
                }
                close(d);
                ctx->got_stor = 1;
            }
            close(data_listen); data_listen = -1;
            send_str(c, "226 transfer complete\r\n");
        } else if (strncmp(cmd, "QUIT", 4) == 0) {
            send_str(c, "221 goodbye\r\n");
            break;
        } else {
            send_str(c, "502 not implemented\r\n");
        }
    }
    if (data_listen >= 0) close(data_listen);
    close(c);
    close(ctx->lsock);
    return NULL;
}

/* ============ TELNET client read-until-complete helper ============ */
/*
 * Read option-stripped application data from a TELNET stream, ACCUMULATING
 * across as many $QIO IO$_READVBLK reads as the byte stream takes, until
 * `needle` appears in what has been received or the peer closes (EOF).
 *
 * This is the only correct way to consume a TCP byte stream for a whole logical
 * line: one IO$_READVBLK returns only whatever the host kernel has buffered at
 * that instant (vms_ioctl_bg_recv -> kernel_recvmsg with flags 0, a possibly
 * short read), so the peer's banner CRLF or the echoed line can arrive in a
 * LATER segment -- a single read cannot be assumed to carry the whole line.
 * That is exactly the vms-5ef flake: tcpip_telnet_recv returns as soon as it
 * has application bytes and its buffer is drained, so the banner's trailing
 * "\r\n" could be left unconsumed and a one-shot echo read would then capture
 * that leftover CRLF (or a fragmented echo) instead of "PING OVMX\r\n".
 *
 * Each tcpip_telnet_recv blocks on REAL socket data, so this loop is driven by
 * socket readiness -- never a timed sleep -- and the suite's alarm() watchdog
 * still bounds a genuinely wedged peer. `*saw_iac` reports whether any raw
 * telnet control byte leaked into the recovered application data (it must not).
 */
static uint32_t telnet_recv_until(struct tcpip_conn *conn, char *buf,
                                  uint32_t bufsz, const char *needle,
                                  uint32_t *total_out, int *saw_iac)
{
    uint32_t total = 0;
    uint32_t st = SS$_NORMAL;

    *saw_iac = 0;
    buf[0] = '\0';
    for (;;) {
        uint32_t got = 0;
        uint32_t i;
        uint32_t room = (total + 1 < bufsz) ? bufsz - 1 - total : 0;

        if (room == 0)
            break;                 /* buffer full -- return what we have */

        st = tcpip_telnet_recv(conn, buf + total, room, &got);
        if (!(st & 1))
            break;
        for (i = 0; i < got; i++)
            if ((unsigned char)buf[total + i] == TN_IAC)
                *saw_iac = 1;
        total += got;
        buf[total] = '\0';
        if (needle && strstr(buf, needle))
            break;                 /* the full expected line has arrived */
        if (got == 0)
            break;                 /* EOF: peer closed, nothing more coming */
    }
    *total_out = total;
    return st;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_tcpip_client (DCL TELNET + FTP over BGn: $QIO, vms-dbb) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        struct tcpip_conn c;
        uint32_t st = tcpip_connect(&c, 0x0100007fu /* 127.0.0.1 */, 23);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: TELNET/FTP connect fails SS$_NOSUCHDEV, never a local per-process socket");
        printf("=== test_syssvc_tcpip_client: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    signal(SIGALRM, watchdog);
    alarm(30);
    bring_lo_up();

    uint32_t lo_be = 0;
    if (!tcpip_parse_ipv4("127.0.0.1", &lo_be)) {
        printf("  FAIL: tcpip_parse_ipv4(127.0.0.1) failed\n");
        return 1;
    }

    /* --------------------------- TELNET --------------------------- */
    {
        uint16_t port = 0;
        int lsock = listen_loopback(&port);
        CHECK(lsock >= 0, "TELNET peer listens on 127.0.0.1");
        if (lsock >= 0) {
            pthread_t th;
            struct tcpip_conn conn;
            char buf[256];
            uint32_t got = 0;
            int has_iac = 0;
            uint32_t st;

            pthread_create(&th, NULL, telnet_mock, &lsock);

            st = tcpip_connect(&conn, lo_be, port);
            CHECK(st & 1, "TELNET $ASSIGN TCPIP$DEVICE: + $QIO connect to the telnet peer");

            /* The banner is delivered with all telnet control (IAC WILL/DO)
             * stripped -- proving the client processed and refused negotiation
             * inline rather than leaking option bytes into application data.
             * Accumulate until the banner is fully in hand (it may span
             * segments); has_iac stays set if any control byte leaked. */
            st = telnet_recv_until(&conn, buf, sizeof(buf), TELNET_BANNER,
                                   &got, &has_iac);
            CHECK((st & 1) && !has_iac && strstr(buf, TELNET_BANNER) != NULL,
                  "TELNET reads the server banner with IAC option negotiation stripped");

            st = tcpip_telnet_send(&conn, TELNET_LINE, (uint32_t)strlen(TELNET_LINE));
            CHECK(st & 1, "TELNET sends an application line over the connection");

            /* Read the echo the SAME way -- accumulate across $QIO reads until
             * the whole echoed line ("PING OVMX...") is in hand. A single
             * IO$_READVBLK returns only the bytes buffered at that instant, so
             * the banner's trailing CRLF (if left unconsumed in a later segment)
             * or a fragmented echo would otherwise make a one-shot read capture
             * the wrong/partial bytes -- the historical flake (vms-5ef). */
            st = telnet_recv_until(&conn, buf, sizeof(buf), "PING OVMX",
                                   &got, &has_iac);
            CHECK((st & 1) && strstr(buf, "PING OVMX") != NULL,
                  "TELNET reads the line echoed back by the server");

            tcpip_close(&conn);
            pthread_join(th, NULL);
        }
    }

    /* --------------------------- FTP RETR --------------------------- */
    {
        uint16_t port = 0;
        struct ftpd_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.lsock = listen_loopback(&port);
        CHECK(ctx.lsock >= 0, "FTP (RETR) peer listens on 127.0.0.1");
        if (ctx.lsock >= 0) {
            pthread_t th;
            unsigned char got[4096];
            uint32_t glen = 0;
            uint32_t st;

            pthread_create(&th, NULL, ftpd_mock, &ctx);

            st = tcpip_ftp_get(lo_be, port, "anonymous", "ovmx@ovmx",
                               "TESTFILE.TXT", got, sizeof(got), &glen);
            CHECK(st & 1, "FTP RETR completes (connect, USER/PASS, TYPE I, PASV, RETR, 226)");
            /* negctl: tcpip-ftp-get-length-dropped */
            CHECK((st & 1) && glen == sizeof(FTP_FILE_BODY) - 1 &&
                  memcmp(got, FTP_FILE_BODY, sizeof(FTP_FILE_BODY) - 1) == 0,
                  "FTP RETR receives the file BYTE-EXACT over the PASV data channel");

            pthread_join(th, NULL);
        }
    }

    /* --------------------------- FTP STOR --------------------------- */
    {
        uint16_t port = 0;
        struct ftpd_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.lsock = listen_loopback(&port);
        CHECK(ctx.lsock >= 0, "FTP (STOR) peer listens on 127.0.0.1");
        if (ctx.lsock >= 0) {
            pthread_t th;
            uint32_t st;

            pthread_create(&th, NULL, ftpd_mock, &ctx);

            st = tcpip_ftp_put(lo_be, port, "anonymous", "ovmx@ovmx",
                               "UPLOAD.TXT",
                               (const unsigned char *)FTP_FILE_BODY,
                               (uint32_t)(sizeof(FTP_FILE_BODY) - 1));
            CHECK(st & 1, "FTP STOR completes (connect, USER/PASS, TYPE I, PASV, STOR, 226)");

            pthread_join(th, NULL);

            CHECK(ctx.got_stor &&
                  ctx.store_len == sizeof(FTP_FILE_BODY) - 1 &&
                  memcmp(ctx.store, FTP_FILE_BODY, sizeof(FTP_FILE_BODY) - 1) == 0,
                  "FTP STOR uploads the file BYTE-EXACT (server received the exact bytes)");
        }
    }

    alarm(0);
    printf("=== test_syssvc_tcpip_client: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
