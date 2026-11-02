/*
 * test_tcpip_resolve_fuzz.c - never-over-read fuzz of the DNS reply parser
 * (tcpip_dns_parse_reply in src/vmstcpip/services/tcpip_resolve.h, vms-a9e7).
 *
 * The parser is a PEER-RECEIVE path: anyone who can put a UDP datagram on the
 * wire controls every byte it reads. Bar (same as test_tcpip_svcdb_fuzz): each
 * fuzzed reply lives in an EXACT-sized heap allocation so ASan's redzone sits
 * immediately past it and any over-read is a hard trap; built with
 * -fsanitize=address,undefined where available. Seeds are VALID replies (A,
 * CNAME->A, NXDOMAIN, multi-answer with compression) mutated by byte flips,
 * pointer-shaped injections, length-field smashes, truncation and extension.
 * Assertions: no crash/over-read (ASan), every result is a defined enum value,
 * an OK result came with a 4-byte address, and NON-VACUITY -- the mutated
 * corpus still reached OK, NXDOMAIN, MISMATCH and MALFORMED, so a change that
 * stops the parser reaching its deep paths fails here rather than passing idle.
 */
#include "tcpip_resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t sys$gettim(uint64_t *t) { if (t) *t = 1; return SS$_NORMAL; }

static int failures = 0;
static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

struct msg { unsigned char b[600]; size_t n; };
static void put8(struct msg *m, unsigned v)  { if (m->n < sizeof m->b) m->b[m->n++] = (unsigned char)v; }
static void put16(struct msg *m, unsigned v) { put8(m, v >> 8); put8(m, v & 0xff); }
static void putname(struct msg *m, const char *name)
{
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        put8(m, (unsigned)l);
        while (l--) put8(m, (unsigned char)*p++);
        if (*p == '.') p++;
    }
    put8(m, 0);
}
static void hdr(struct msg *m, unsigned id, unsigned flags, unsigned an)
{
    m->n = 0;
    put16(m, id); put16(m, flags); put16(m, 1); put16(m, an); put16(m, 0); put16(m, 0);
}

#define QNAME "host.lab.example"
#define QID   0x4242u

static void seeds(struct msg s[4])
{
    /* 0: plain A, owner compressed */
    hdr(&s[0], QID, 0x8180, 2); putname(&s[0], QNAME); put16(&s[0], 1); put16(&s[0], 1);
    put16(&s[0], 0xc00c); put16(&s[0], 1); put16(&s[0], 1); put16(&s[0], 0); put16(&s[0], 60);
    put16(&s[0], 4); put16(&s[0], 0x0a00); put16(&s[0], 0x0001);
    put16(&s[0], 0xc00c); put16(&s[0], 1); put16(&s[0], 1); put16(&s[0], 0); put16(&s[0], 60);
    put16(&s[0], 4); put16(&s[0], 0x0a00); put16(&s[0], 0x0002);
    /* 1: CNAME -> A */
    hdr(&s[1], QID, 0x8180, 2); putname(&s[1], QNAME); put16(&s[1], 1); put16(&s[1], 1);
    put16(&s[1], 0xc00c); put16(&s[1], 5); put16(&s[1], 1); put16(&s[1], 0); put16(&s[1], 60);
    put16(&s[1], 7);                 /* "\4edge" + ptr to lab.example (offset 17) */
    put8(&s[1], 4); put8(&s[1], 'e'); put8(&s[1], 'd'); put8(&s[1], 'g'); put8(&s[1], 'e');
    put16(&s[1], 0xc011);
    {
        unsigned cn = (unsigned)(s[1].n - 7);
        put16(&s[1], 0xc000 | cn); put16(&s[1], 1); put16(&s[1], 1); put16(&s[1], 0);
        put16(&s[1], 60); put16(&s[1], 4); put16(&s[1], 0x0a00); put16(&s[1], 0x0009);
    }
    /* 2: NXDOMAIN */
    hdr(&s[2], QID, 0x8183, 0); putname(&s[2], QNAME); put16(&s[2], 1); put16(&s[2], 1);
    /* 3: answer whose owner is spelled out (no compression), two labels deep */
    hdr(&s[3], QID, 0x8180, 1); putname(&s[3], QNAME); put16(&s[3], 1); put16(&s[3], 1);
    putname(&s[3], QNAME); put16(&s[3], 1); put16(&s[3], 1); put16(&s[3], 0); put16(&s[3], 9);
    put16(&s[3], 4); put16(&s[3], 0xc0a8); put16(&s[3], 0x0101);
}

#define ITERS 300000

int main(void)
{
    struct msg s[4];
    unsigned long hits[TCPIP_DNS_BADNAME + 1];
    int bad_enum = 0, ok_bad_addr = 0, seed_ok = 1;
    long it;

    memset(hits, 0, sizeof hits);
    printf("=== test_tcpip_resolve_fuzz (vms-a9e7, %d iterations) ===\n", ITERS);
    srand(0xa9e7);
    seeds(s);

    /* The seeds themselves parse as intended (the corpus is meaningful). */
    {
        uint32_t a = 0;
        seed_ok &= tcpip_dns_parse_reply(s[0].b, s[0].n, QID, QNAME, &a) == TCPIP_DNS_OK;
        seed_ok &= tcpip_dns_parse_reply(s[1].b, s[1].n, QID, QNAME, &a) == TCPIP_DNS_OK;
        seed_ok &= tcpip_dns_parse_reply(s[2].b, s[2].n, QID, QNAME, &a) == TCPIP_DNS_NXDOMAIN;
        seed_ok &= tcpip_dns_parse_reply(s[3].b, s[3].n, QID, QNAME, &a) == TCPIP_DNS_OK;
    }
    check(seed_ok, "seed replies parse as A / CNAME->A / NXDOMAIN / A");

    for (it = 0; it < ITERS; it++) {
        struct msg m = s[rand() % 4];
        int nmut = 1 + rand() % 4, k;
        unsigned char *exact;
        uint32_t addr = 0xffffffffu;
        enum tcpip_dns_rc rc;

        for (k = 0; k < nmut; k++) {
            int op = rand() % 7;
            size_t at = m.n ? (size_t)rand() % m.n : 0;
            switch (op) {
            case 0: if (m.n) m.b[at] = (unsigned char)rand(); break;          /* byte */
            case 1: if (m.n) m.b[at] ^= (unsigned char)(1u << (rand() % 8)); break;
            case 2: if (m.n > 1) { m.b[at] = (unsigned char)(0xc0 | (rand() & 0x3f));   /* pointer */
                                   if (at + 1 < m.n) m.b[at + 1] = (unsigned char)rand(); } break;
            case 3: if (m.n > 1 && at + 1 < m.n) { m.b[at] = 0xff; m.b[at + 1] = 0xff; } break; /* length smash */
            case 4: m.n = m.n ? (size_t)rand() % m.n : 0; break;                 /* truncate */
            case 5: { int add = rand() % 40; while (add-- && m.n < sizeof m.b) m.b[m.n++] = (unsigned char)rand(); } break;
            case 6: if (m.n) m.b[at] = (unsigned char)(rand() % 64); break;     /* label length */
            }
        }
        exact = (unsigned char *)malloc(m.n ? m.n : 1);
        if (!exact) { check(0, "malloc"); break; }
        memcpy(exact, m.b, m.n);
        rc = tcpip_dns_parse_reply(exact, m.n, QID, QNAME, &addr);
        free(exact);
        if ((int)rc < 0 || rc > TCPIP_DNS_BADNAME) { bad_enum++; continue; }
        hits[rc]++;
        if (rc == TCPIP_DNS_OK && addr == 0xffffffffu) ok_bad_addr++;
    }

    printf("  results: OK=%lu MISMATCH=%lu MALFORMED=%lu TRUNCATED=%lu NXDOMAIN=%lu "
           "SERVFAIL=%lu NODATA=%lu\n",
           hits[TCPIP_DNS_OK], hits[TCPIP_DNS_MISMATCH], hits[TCPIP_DNS_MALFORMED],
           hits[TCPIP_DNS_TRUNCATED], hits[TCPIP_DNS_NXDOMAIN], hits[TCPIP_DNS_SERVFAIL],
           hits[TCPIP_DNS_NODATA]);
    check(bad_enum == 0, "every result is a defined tcpip_dns_rc value");
    check(ok_bad_addr == 0, "every OK result delivered an address");
    check(hits[TCPIP_DNS_OK] > 0 && hits[TCPIP_DNS_NXDOMAIN] > 0 &&
          hits[TCPIP_DNS_MISMATCH] > 0 && hits[TCPIP_DNS_MALFORMED] > 0,
          "non-vacuity: mutated corpus reached OK, NXDOMAIN, MISMATCH and MALFORMED");
    check(1, "no over-read / crash across the corpus (ASan is the assertion)");

    printf("=== test_tcpip_resolve_fuzz: %s (%d failure%s) ===\n",
           failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
