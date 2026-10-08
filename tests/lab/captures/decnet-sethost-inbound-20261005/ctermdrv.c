/*
 * ctermdrv.c - LAB HARNESS ONLY (not product code): runs OVMX's compiled CTERM
 * host FSM (src/vmsdecnet/cterm/dnet_cterm_hostfsm.c) against a real OpenVMS
 * SET HOST client, with ctermprobe.py carrying the NSP link.
 *
 * The session's "terminal" is a pseudo-terminal running a tiny stand-in login
 * script (ctlogin.sh). In the product the terminal is an executive-minted RTAn:
 * with LOGINOUT $CREPRC'd onto it (dnet_cterm_host_open_desc); only the wire
 * protocol is under test here, and it is the same compiled FSM NETACP runs.
 *
 * stdin  (from the probe): "D <hex>"  one NSP data segment from the client
 *                          "Q"        the link went down
 * stdout (to the probe):   "S <hex>"  send this NSP data segment
 *                          "L <text>" log line
 *                          "X <why>"  the session is over (probe disconnects)
 *
 * Build: musl-gcc -static -O2 -I../../../../src/vmsdecnet/cterm/include \
 *          ctermdrv.c ../../../../src/vmsdecnet/cterm/dnet_cterm_hostfsm.c -o ctermdrv
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "dnet_cterm_hostfsm.h"

static struct dnet_cth H;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void flush_tx(void)
{
    uint8_t b[DNET_CTH_SEG_MAX];
    size_t n;
    while (dnet_cth_tx_pop(&H, b, sizeof b, &n)) {
        printf("S ");
        for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
        printf("\n");
    }
    fflush(stdout);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void log_printable(const char *tag, const uint8_t *b, size_t n)
{
    printf("L %s \"", tag);
    for (size_t i = 0; i < n; i++)
        if (b[i] >= 0x20 && b[i] < 0x7f && b[i] != '"') putchar(b[i]);
        else printf("\\x%02x", b[i]);
    printf("\"\n");
}

int main(int argc, char **argv)
{
    const char *script = argc > 1 ? argv[1] : "/tmp/ctlogin.sh";
    int mfd = -1;
    pid_t pid = forkpty(&mfd, NULL, NULL, NULL);
    if (pid < 0) { perror("forkpty"); return 2; }
    if (pid == 0) {
        execl("/bin/sh", "sh", script, (char *)NULL);
        _exit(127);
    }
    fcntl(mfd, F_SETFL, fcntl(mfd, F_GETFL) | O_NONBLOCK);
    setvbuf(stdout, NULL, _IOLBF, 0);

    dnet_cth_init(&H, 60);
    dnet_cth_open(&H);                /* the host speaks first */
    flush_tx();

    char line[2 * DNET_CTH_SEG_MAX + 16];
    size_t ll = 0;
    int term_gone = 0;
    for (;;) {
        struct pollfd pf[2] = { { 0, POLLIN, 0 }, { mfd, POLLIN, 0 } };
        poll(pf, term_gone ? 1 : 2, 20);
        uint64_t t = now_ms();

        if (pf[0].revents & (POLLIN | POLLHUP)) {
            char c;
            ssize_t r;
            while ((r = read(0, &c, 1)) == 1) {
                if (c != '\n') { if (ll < sizeof line - 1) line[ll++] = c; continue; }
                line[ll] = 0;
                if (line[0] == 'Q') { printf("X link-down\n"); return 0; }
                if (line[0] == 'D' && ll > 2) {
                    uint8_t seg[DNET_CTH_SEG_MAX];
                    size_t n = 0;
                    for (size_t i = 2; i + 1 < ll && n < sizeof seg; i += 2)
                        seg[n++] = (uint8_t)(hexval(line[i]) << 4 | hexval(line[i + 1]));
                    int rc = dnet_cth_rx(&H, seg, n, t);
                    if (rc == DNET_CTH_EPROTO) printf("L protocol error from the client\n");
                    if (H.peer_unbound) { printf("X client-unbound reason %u\n", H.peer_unbind_reason); return 0; }
                }
                ll = 0;
                break;                     /* one line per pass keeps the order */
            }
            if (r == 0) { printf("X stdin-eof\n"); return 0; }
        }

        if (!term_gone) {
            uint8_t b[1024];
            ssize_t r = read(mfd, b, sizeof b);
            if (r > 0) {
                log_printable("terminal-output", b, (size_t)r);
                dnet_cth_term_output(&H, b, (size_t)r, t);
            } else if (r == 0 || (r < 0 && errno == EIO)) {
                term_gone = 1;             /* the session process exited */
            }
        }

        struct termios tio;
        int echo = (tcgetattr(mfd, &tio) == 0) ? !!(tio.c_lflag & ECHO) : 1;
        uint8_t in[256];
        size_t k;
        while ((k = dnet_cth_term_input(&H, in, sizeof in, echo)) > 0) {
            log_printable(echo ? "typed-line" : "typed-line(no-echo)", echo ? in : (const uint8_t *)"<redacted>", echo ? k : 10);
            (void)write(mfd, in, k);
        }
        if (term_gone) {
            dnet_cth_close(&H);            /* flush, then Unbind 02 03 00 */
            flush_tx();
            int st = 0;
            waitpid(pid, &st, 0);
            printf("X session-exited status %d\n", WEXITSTATUS(st));
            return 0;
        }
        dnet_cth_tick(&H, t, echo);
        flush_tx();
    }
}
