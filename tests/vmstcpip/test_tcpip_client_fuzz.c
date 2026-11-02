/*
 * test_tcpip_client_fuzz.c - the peer-receive parsers of the TELNET/FTP client
 * engines (src/vmstcpip/services/tcpip_client.h), fed bytes a remote server
 * controls (rd vms-d0c4).
 *
 *   tcpip_telnet_filter   the TELNET receive state machine (RFC 854 commands,
 *                         option negotiation, subnegotiation)
 *   tcpip_ftp_parse_pasv  the "227 Entering Passive Mode (h,h,h,h,p,p)" reply
 *
 * Both now face servers outside the VM, so both are fuzzed: random and
 * grammar-biased input into EXACT-sized heap buffers (ASan redzones right past
 * every buffer when the sanitizers are available), the outputs bounded by
 * deliberately tiny buffers so the "stop when full, resume" paths run. The
 * properties checked besides "never crashes, never over-reads/writes":
 *   - the filter's application data does not depend on how the stream was split
 *     into reads (one call over the whole stream == byte-at-a-time calls), so a
 *     command split across two IO$_READVBLKs is never lost or leaked as data;
 *   - every reply it owes is a well-formed IAC <DO|DONT|WILL|WONT> <opt> triple;
 *   - negotiation never loops: a repeated WILL ECHO is answered once.
 * Plus the fixed cases the client's policy rests on (WILL ECHO -> DO ECHO, DO
 * anything -> WONT, IAC IAC -> one 0xFF data byte, SB...SE swallowed) and the
 * PASV bounds (a field over 255 or a zero port is refused).
 */
#include "tcpip_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

/* Feed `in` through a fresh filter in chunks of `step` bytes (0 = all at once),
 * with output/reply buffers of the given sizes. Collects the data and the
 * replies; returns 0 if any reply was malformed or the filter made no progress. */
static int run_filter(const unsigned char *in, uint32_t inlen, uint32_t step,
                      uint32_t outsz, uint32_t replysz,
                      unsigned char *data, uint32_t *dlen,
                      unsigned char *replies, uint32_t *rlen, uint32_t rcap,
                      struct tcpip_telnet_state *final)
{
    struct tcpip_telnet_state t;
    uint32_t pos = 0;
    unsigned char *ob = malloc(outsz ? outsz : 1);
    unsigned char *rb = malloc(replysz ? replysz : 1);
    int ok = 1;

    tcpip_telnet_init(&t);
    *dlen = 0;
    *rlen = 0;
    while (pos < inlen) {
        uint32_t chunk = step ? step : inlen;
        uint32_t cpos = 0;
        if (chunk > inlen - pos)
            chunk = inlen - pos;
        /* the exact-sized input chunk */
        unsigned char *cb = malloc(chunk);
        memcpy(cb, in + pos, chunk);
        while (cpos < chunk) {
            uint32_t o = 0, r = 0, used;
            used = tcpip_telnet_filter(&t, cb + cpos, chunk - cpos, ob, outsz, &o,
                                       rb, replysz, &r);
            if (o > outsz || r > replysz || used > chunk - cpos) { ok = 0; break; }
            if (used == 0 && o == 0 && r == 0) { ok = 0; break; }   /* no progress */
            if (r % 3 != 0) ok = 0;
            for (uint32_t k = 0; k + 3 <= r; k += 3) {
                if (rb[k] != TN_IAC) ok = 0;
                if (rb[k + 1] != TN_DO && rb[k + 1] != TN_DONT &&
                    rb[k + 1] != TN_WILL && rb[k + 1] != TN_WONT) ok = 0;
            }
            memcpy(data + *dlen, ob, o);
            *dlen += o;
            if (*rlen + r <= rcap) {
                memcpy(replies + *rlen, rb, r);
                *rlen += r;
            }
            cpos += used;
        }
        free(cb);
        if (!ok) break;
        pos += chunk;
    }
    if (final) *final = t;
    free(ob);
    free(rb);
    return ok;
}

static const unsigned char TOK[] = {
    TN_IAC, TN_IAC, TN_IAC, TN_WILL, TN_WONT, TN_DO, TN_DONT, TN_SB, TN_SE,
    TN_OPT_ECHO, TN_OPT_SGA, 24, 31, 241, 249, 'a', 'Z', '\r', '\n', 0
};
#define NTOK ((int)sizeof TOK)

static void fixed_cases(void)
{
    unsigned char data[256], rep[256];
    uint32_t dl, rl;
    struct tcpip_telnet_state t;

    {   /* busybox telnetd's opening: DO ECHO, DO NAWS, WILL ECHO, WILL SGA */
        static const unsigned char in[] = { 255,253,1, 255,253,31, 255,251,1, 255,251,3,
                                            '/', ' ', '#', ' ' };
        static const unsigned char want[] = { 255,252,1, 255,252,31, 255,253,1, 255,253,3 };
        check(run_filter(in, sizeof in, 0, 64, 64, data, &dl, rep, &rl, sizeof rep, &t) &&
              dl == 4 && memcmp(data, "/ # ", 4) == 0,
              "a real telnetd's opening negotiation is stripped; its prompt is data");
        check(rl == sizeof want && memcmp(rep, want, sizeof want) == 0,
              "replies: WONT ECHO, WONT NAWS (we offer nothing), DO ECHO, DO SGA (server may echo)");
        check(tcpip_telnet_remote_echo(&t), "WILL ECHO accepted -> character mode");
    }
    {   /* a repeated WILL ECHO is not answered again (no negotiation loop) */
        static const unsigned char in[] = { 255,251,1, 255,251,1, 255,251,1 };
        check(run_filter(in, sizeof in, 1, 8, 3, data, &dl, rep, &rl, sizeof rep, NULL) &&
              rl == 3 && dl == 0, "WILL ECHO x3 -> exactly one DO ECHO");
    }
    {   /* WONT ECHO after WILL ECHO -> DONT once, mode back to line */
        static const unsigned char in[] = { 255,251,1, 255,252,1, 255,252,1 };
        static const unsigned char want[] = { 255,253,1, 255,254,1 };
        check(run_filter(in, sizeof in, 0, 8, 64, data, &dl, rep, &rl, sizeof rep, &t) &&
              rl == sizeof want && memcmp(rep, want, sizeof want) == 0 &&
              !tcpip_telnet_remote_echo(&t),
              "WONT ECHO after WILL -> one DONT ECHO, line mode again");
    }
    {   /* other WILLs refused */
        static const unsigned char in[] = { 255,251,24 };
        static const unsigned char want[] = { 255,254,24 };
        check(run_filter(in, sizeof in, 0, 8, 64, data, &dl, rep, &rl, sizeof rep, NULL) &&
              rl == 3 && memcmp(rep, want, 3) == 0, "WILL TTYPE -> DONT TTYPE");
    }
    {   /* IAC IAC = data 0xFF; SB ... IAC IAC ... IAC SE swallowed; NOP ignored */
        static const unsigned char in[] = { 'A', 255,255, 'B', 255,250,24,1,255,255,9,255,240,
                                            255,241, 'C' };
        check(run_filter(in, sizeof in, 1, 1, 64, data, &dl, rep, &rl, sizeof rep, NULL) &&
              dl == 4 && memcmp(data, "A\xff" "BC", 4) == 0 && rl == 0,
              "IAC IAC -> 0xFF data; subnegotiation and NOP stripped, byte-at-a-time");
    }
    {   /* reply buffer full mid-stream: the filter stops, then resumes */
        static const unsigned char in[] = { 255,253,1, 255,253,3, 'x' };
        unsigned char ob[8], rb[3];
        struct tcpip_telnet_state s;
        uint32_t o, r, u;
        tcpip_telnet_init(&s);
        u = tcpip_telnet_filter(&s, in, sizeof in, ob, sizeof ob, &o, rb, sizeof rb, &r);
        check(u == 5 && r == 3 && o == 0, "a full reply buffer stops the filter before the next reply");
        u += tcpip_telnet_filter(&s, in + u, sizeof in - u, ob, sizeof ob, &o, rb, sizeof rb, &r);
        check(u == sizeof in && r == 3 && o == 1 && ob[0] == 'x', "and it resumes where it stopped");
    }
}

static void pasv_cases(void)
{
    uint32_t a = 0;
    uint16_t p = 0;
    check(tcpip_ftp_parse_pasv("227 Entering Passive Mode (172,17,0,2,195,80).", &a, &p) &&
          memcmp(&a, "\xac\x11\x00\x02", 4) == 0 && p == 195 * 256 + 80,
          "PASV (172,17,0,2,195,80) -> 172.17.0.2 port 50000");
    check(!tcpip_ftp_parse_pasv("227 (1,2,3,4,256,1)", &a, &p), "a PASV field over 255 is refused");
    check(!tcpip_ftp_parse_pasv("227 (1,2,3,4,99999999999999999999,1)", &a, &p),
          "a PASV field that would overflow is refused");
    check(!tcpip_ftp_parse_pasv("227 (1,2,3,4,0,0)", &a, &p), "a PASV port of 0 is refused");
    check(!tcpip_ftp_parse_pasv("227 (1,2,3,4,5)", &a, &p), "a short PASV tuple is refused");
}

#define ITERS 100000

int main(void)
{
    unsigned char *in, *d1, *d2, *r1, *r2;
    uint32_t accepted = 0;

    printf("=== test_tcpip_client_fuzz (TELNET filter + FTP PASV parser, rd vms-d0c4) ===\n");
    fixed_cases();
    pasv_cases();

    srand(0xd0c4);
    d1 = malloc(4096); d2 = malloc(4096); r1 = malloc(16384); r2 = malloc(16384);
    {
        int split_ok = 1, form_ok = 1;
        for (int it = 0; it < ITERS; it++) {
            uint32_t n = (uint32_t)(rand() % 300) + 1;
            uint32_t dl1, dl2, rl1, rl2;
            in = malloc(n);
            for (uint32_t i = 0; i < n; i++)
                in[i] = (rand() % 3) ? TOK[rand() % NTOK] : (unsigned char)rand();
            if (!run_filter(in, n, 0, 4096, 4096, d1, &dl1, r1, &rl1, 16384, NULL))
                form_ok = 0;
            if (!run_filter(in, n, (uint32_t)(rand() % 7) + 1, (uint32_t)(rand() % 5) + 1,
                            (uint32_t)(rand() % 3 + 1) * 3, d2, &dl2, r2, &rl2, 16384, NULL))
                form_ok = 0;
            if (dl1 != dl2 || memcmp(d1, d2, dl1) != 0)
                split_ok = 0;
            if (dl1) accepted++;
            free(in);
        }
        check(form_ok, "fuzz: every reply a well-formed IAC triple, bounded output, always progress");
        check(split_ok, "fuzz: application data identical however the stream is split into reads");
        check(accepted > ITERS / 4, "fuzz is non-vacuous (data reached the output path)");
    }
    {
        int pasv_hits = 0;
        static const char AL[] = "0123456789,,,()( 227.x-";
        for (int it = 0; it < ITERS; it++) {
            int n = rand() % 64 + 1;
            char *s = malloc((size_t)n);
            uint32_t a; uint16_t p;
            if (it & 1) {
                /* a well-shaped tuple with random (some out-of-range) fields */
                char t[96];
                int k = snprintf(t, sizeof t, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
                                 rand() % 300, rand() % 300, rand() % 300, rand() % 300,
                                 rand() % 300, rand() % 300);
                free(s);
                n = k + 1;
                s = malloc((size_t)n);
                memcpy(s, t, (size_t)n);
            } else {
                for (int i = 0; i < n - 1; i++)
                    s[i] = AL[rand() % (int)(sizeof AL - 1)];
                s[n - 1] = '\0';
            }
            if (tcpip_ftp_parse_pasv(s, &a, &p)) {
                pasv_hits++;
                if (p == 0) failures++;
            }
            free(s);
        }
        check(pasv_hits > ITERS / 20, "fuzz: PASV parser never over-reads an exact-sized reply (non-vacuous)");
        printf("  (PASV fuzz accepted %d inputs)\n", pasv_hits);
    }
    free(d1); free(d2); free(r1); free(r2);
    printf("=== %s (%d failure(s)) ===\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
