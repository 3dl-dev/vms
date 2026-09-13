/*
 * test_tcpip_svcdb_fuzz.c - never-crash fuzz for the TCPIP$SERVICE.DAT parsers
 * (rd vms-deb3). The auxiliary server (TCPIP$INETD-equiv) and the DCL
 * {SET,SHOW,ENABLE,DISABLE,DELETE} SERVICE management engine both parse the
 * SERVICE.DAT grammar ("name port [user] image [args]", leading-"!" disabled)
 * out of a persisted, DCL-writable database -- untrusted-ish text into fixed
 * buffers, the gate before a client-reading service (e.g. SSH) is enabled.
 * Coordinated with the TCP/IP lane (surface owner); the higher-untrusted
 * network-byte response parsers (tcpip_client.h TELNET/FTP, tcpip_ping.h ICMP)
 * are the owner's R4 sweep (vms-c9ef), not this rung.
 *
 * Bar (same as the cluster/nodespec fuzz, scoped with the owner): each fuzzed
 * input lives in an EXACT-sized heap allocation (NUL-terminated, the C-string
 * contract these `const char *` APIs take), so ASan's redzone sits immediately
 * past the string and any over-read is a hard trap. Assert: no over-read / OOB /
 * UB / crash on adversarial input (missing/short fields, embedded NUL, tokens
 * longer than every buffer, out-of-range/negative/overflow port, bare "!",
 * all-whitespace, no-newline, user-vs-image ':'/'/'‑disambiguation), and the
 * parse_rec -> format_rec -> re-parse round-trip never over-reads. NOT
 * clean-on-reject (contract is clean-skip per the owner; ASan is the assertion).
 * Non-vacuity: assert the parsers actually accepted+deep-parsed some inputs, so
 * a change that stops reaching the field-copy paths fails HARD, not silently.
 * This test never patches the codec; a trip hands the bytes to the owner.
 */
#include "tcpip_inetd.h"        /* tcpip_inetd_parse_db, struct tcpip_service      */
#include "tcpip_service_db.h"   /* tcpip_svcdb_parse_rec/format_rec, tcpip_svcdb_rec */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

/* Alphabet biased to the grammar tokens so branches (name/port/user/image/args,
 * "!" disable, ':'/'/' disambiguation, whitespace, embedded NUL) are reached. */
static const char AL[] = {
    'a','b','A','Z','1','2','9',' ',' ','\t','!',':','/','\n',
    '.','[',']','$','-',';','_','\0','0'
};
#define NAL ((int)sizeof AL)

static void fill(char *s, int n) { for (int i = 0; i < n; i++) s[i] = AL[rand() % NAL]; }

/* Exact-sized NUL-terminated copy of `src` (which already ends in a NUL at
 * index n-1): the buffer is exactly n bytes so ASan red-zones index n. */
static char *exact(const char *src, int n)
{
    char *p = (char *)malloc(n > 0 ? n : 1);
    if (p && n > 0) memcpy(p, src, n);
    return p;
}

#define ITERS 200000

int main(void)
{
    struct tcpip_service svcs[8];
    struct tcpip_svcdb_rec rec;
    long svc_hits = 0, rec_hits = 0;

    printf("test_tcpip_svcdb_fuzz: TCPIP$SERVICE.DAT parser never-crash fuzz (rd vms-deb3)\n");
    srand(0x5DCB);

    /* (1) tcpip_inetd_parse_db: NUL-terminated adversarial multi-line text. */
    for (int i = 0; i < ITERS; i++) {
        char s[600];
        int body = rand() % (int)(sizeof s - 1);
        fill(s, body);
        s[body] = '\0';
        char *b = exact(s, body + 1);
        int n = tcpip_inetd_parse_db(b, svcs, 8);
        if (n > 0) svc_hits += n;
        free(b);
    }
    check(1, "tcpip_inetd_parse_db: 200k NUL-terminated adversarial texts, no over-read/crash");

    /* (2) tcpip_svcdb_parse_rec + parse->format->re-parse round-trip. */
    for (int i = 0; i < ITERS; i++) {
        char s[600];
        int body = rand() % (int)(sizeof s - 1);
        fill(s, body);
        s[body] = '\0';
        char *b = exact(s, body + 1);
        if (tcpip_svcdb_parse_rec(b, &rec) == 1) {
            rec_hits++;
            char line[1200];
            struct tcpip_svcdb_rec r2;
            struct tcpip_service sv2[8];
            tcpip_svcdb_format_rec(&rec, line, sizeof line);   /* render back */
            (void)tcpip_svcdb_parse_rec(line, &r2);            /* re-parse */
            (void)tcpip_inetd_parse_db(line, sv2, 8);         /* cross-parse */
        }
        free(b);
    }
    check(1, "tcpip_svcdb_parse_rec + format->re-parse round-trip: 200k, no over-read/crash");

    /* (3) non-vacuity: the parsers must genuinely accept+deep-parse some inputs
     * (else the strncpy/format/round-trip paths never ran -- a silent facade). */
    check(svc_hits > 0, "inetd_parse_db reached the record/field-copy path (non-vacuous)");
    check(rec_hits > 0, "svcdb_parse_rec reached the record/format/round-trip path (non-vacuous)");

    /* (4) hand-crafted edge corpus. */
    static const char *corpus[] = {
        "", "!", "! banner text", "   ", "\t\t", "\n\n\n",
        "svc", "svc 0", "svc -1 img", "svc 99999 img", "svc 70000 img",
        "svc 4294967337 img",                        /* port strtol overflow */
        "svc abc img", "svc 42", "svc 42 user img a1 a2",
        "svc 42 disk:[dir]img.exe",                  /* token has ':' -> image */
        "svc 42 user/path img",                      /* token has '/' -> image */
        "svc 42 plainuser img.exe extra args here",  /* no ':'/'/' -> run-as user */
        "! svc 42 img", "!svc 42 img",
        "svc\t42\timg.exe",                          /* tab-separated */
        "svc 42 img\r",                              /* trailing CR, no LF */
        NULL
    };
    for (int i = 0; corpus[i]; i++) {
        int n = (int)strlen(corpus[i]) + 1;
        char *b = exact(corpus[i], n);
        (void)tcpip_inetd_parse_db(b, svcs, 8);
        (void)tcpip_svcdb_parse_rec(b, &rec);
        free(b);
    }
    /* a service-name / image / args token longer than every fixed buffer */
    {
        char big[900];
        memset(big, 'Z', sizeof big);
        big[3] = ' '; big[6] = '4'; big[7] = '2'; big[8] = ' ';  /* "ZZZ 42 Z..." */
        big[sizeof big - 1] = '\0';
        char *b = exact(big, (int)strlen(big) + 1);
        (void)tcpip_inetd_parse_db(b, svcs, 8);
        (void)tcpip_svcdb_parse_rec(b, &rec);
        free(b);
    }
    check(1, "edge corpus (bare '!', bad/overflow port, user-vs-image, oversize tokens, CR/tab), no over-read/crash");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
