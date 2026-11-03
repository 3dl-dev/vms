/*
 * test_tcpip_resolve.c - host unit test for the resolver engine's pure parts
 * (src/vmstcpip/services/tcpip_resolve.h, vms-a9e7): the TCPIP$HOST.DAT record
 * match, the TCPIP$NAMESERVICE.DAT record parse, domain qualification, the
 * RFC 1035 A query, and the reply parser on hand-built replies -- a plain A
 * answer, CNAME then A, NXDOMAIN, SERVFAIL, TC, wrong ID, wrong question, a
 * truncated packet, a compression-pointer loop, a pointer past the end, a huge
 * RDLENGTH, and an A record with RDLENGTH != 4. Each reply lives in an
 * EXACT-sized heap buffer so ASan traps any read past it.
 *
 * The BGn: UDP exchange itself is proven against a real /dev/vms by
 * tests/qemu/test_syssvc_bg_udp.c; nothing here touches the network.
 */
#include "tcpip_resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

/* ---- a tiny reply builder ------------------------------------------------ */

struct msg { unsigned char b[1024]; size_t n; };

static void put8(struct msg *m, unsigned v)  { m->b[m->n++] = (unsigned char)v; }
static void put16(struct msg *m, unsigned v) { put8(m, v >> 8); put8(m, v & 0xff); }
static void put32(struct msg *m, unsigned long v)
{
    put16(m, (unsigned)(v >> 16)); put16(m, (unsigned)(v & 0xffff));
}
static void putname(struct msg *m, const char *name)
{
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        put8(m, (unsigned)l);
        memcpy(m->b + m->n, p, l);
        m->n += l;
        p += l;
        if (*p == '.') p++;
    }
    put8(m, 0);
}
static void hdr(struct msg *m, unsigned id, unsigned flags, unsigned qd, unsigned an)
{
    m->n = 0;
    put16(m, id); put16(m, flags); put16(m, qd); put16(m, an); put16(m, 0); put16(m, 0);
}
static void question(struct msg *m, const char *name)
{
    putname(m, name); put16(m, TCPIP_DNS_TYPE_A); put16(m, TCPIP_DNS_CLASS_IN);
}
/* RR whose owner is a pointer to offset 12 (the question name). */
static void rr_a_ptr(struct msg *m, unsigned a, unsigned b, unsigned c, unsigned d)
{
    put16(m, 0xc00c); put16(m, TCPIP_DNS_TYPE_A); put16(m, TCPIP_DNS_CLASS_IN);
    put32(m, 300); put16(m, 4); put8(m, a); put8(m, b); put8(m, c); put8(m, d);
}

static enum tcpip_dns_rc parse_exact(const struct msg *m, uint16_t id,
                                     const char *qname, uint32_t *addr)
{
    unsigned char *p = (unsigned char *)malloc(m->n ? m->n : 1);
    enum tcpip_dns_rc rc;
    memcpy(p, m->b, m->n);
    rc = tcpip_dns_parse_reply(p, m->n, id, qname, addr);
    free(p);
    return rc;
}

static uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d)
{
    unsigned char o[4] = { (unsigned char)a, (unsigned char)b,
                           (unsigned char)c, (unsigned char)d };
    uint32_t r;
    memcpy(&r, o, 4);
    return r;
}

/* Satisfy the header's sys$gettim reference without an executive. */
uint32_t sys$gettim(uint64_t *t) { if (t) *t = 0x123456789abcdefULL; return SS$_NORMAL; }

int main(void)
{
    struct msg m;
    uint32_t a = 0, srv = 0;
    int have = 0;
    char dom[64] = "", fq[64];
    unsigned char q[512];
    size_t ql;

    printf("=== test_tcpip_resolve (vms-a9e7) ===\n");

    /* (a) host database */
    check(tcpip_hosts_match_line("10.1.2.3        vaxa", "VAXA", &a) && a == ip(10,1,2,3),
          "hosts: hostname matches case-insensitively");
    check(tcpip_hosts_match_line("10.1.2.4 alpha.lab alpha", "alpha", &a) && a == ip(10,1,2,4),
          "hosts: alias matches");
    check(!tcpip_hosts_match_line("10.1.2.3 vaxa", "vax", &a), "hosts: prefix is not a match");
    check(!tcpip_hosts_match_line("# 10.1.2.3 vaxa", "vaxa", &a), "hosts: comment skipped");
    check(!tcpip_hosts_match_line("notanip vaxa", "vaxa", &a), "hosts: bad address skipped");
    check(!tcpip_hosts_match_line("", "vaxa", &a), "hosts: empty record skipped");

    /* name service */
    check(tcpip_ns_parse_line("SERVER=192.168.2.1", &srv, &have, dom, sizeof dom) &&
          have && srv == ip(192,168,2,1), "nameservice: SERVER= parsed");
    check(tcpip_ns_parse_line("DOMAIN=lab.example", &srv, &have, dom, sizeof dom) &&
          strcmp(dom, "lab.example") == 0, "nameservice: DOMAIN= parsed");
    check(tcpip_res_qualify("vaxa", dom, fq, sizeof fq) && strcmp(fq, "vaxa.lab.example") == 0,
          "qualify: dotless name gets the domain");
    check(tcpip_res_qualify("www.example.org", dom, fq, sizeof fq) &&
          strcmp(fq, "www.example.org") == 0, "qualify: dotted name is left alone");

    /* (b) the query */
    ql = tcpip_dns_build_query(q, sizeof q, 0xbeef, "www.Example.org");
    {
        static const unsigned char want[] = {
            0xbe,0xef, 0x01,0x00, 0,1, 0,0, 0,0, 0,0,
            3,'w','w','w', 7,'E','x','a','m','p','l','e', 3,'o','r','g', 0,
            0,1, 0,1 };
        check(ql == sizeof want && memcmp(q, want, ql) == 0,
              "query: RFC 1035 header + QNAME + QTYPE A + QCLASS IN, RD set");
    }
    check(tcpip_dns_build_query(q, sizeof q, 1, "a..b") == 0, "query: empty label refused");
    check(tcpip_dns_build_query(q, sizeof q, 1, "") == 0, "query: empty name refused");
    {
        char big[80];
        memset(big, 'x', 64); big[64] = '\0';
        check(tcpip_dns_build_query(q, sizeof q, 1, big) == 0, "query: 64-octet label refused");
    }

    /* (c) replies */
    hdr(&m, 0x1234, 0x8180, 1, 1); question(&m, "www.example.org"); rr_a_ptr(&m, 93,184,216,34);
    a = 0;
    check(parse_exact(&m, 0x1234, "WWW.example.org", &a) == TCPIP_DNS_OK && a == ip(93,184,216,34),
          "reply: A answer (owner compressed) gives the address");
    check(parse_exact(&m, 0x1235, "www.example.org", &a) == TCPIP_DNS_MISMATCH,
          "reply: wrong ID is ignored (MISMATCH)");
    check(parse_exact(&m, 0x1234, "ftp.example.org", &a) == TCPIP_DNS_MISMATCH,
          "reply: wrong question is ignored (MISMATCH)");
    {
        struct msg t = m; t.b[2] &= 0x7f;   /* QR clear: a query, not a reply */
        check(parse_exact(&t, 0x1234, "www.example.org", &a) == TCPIP_DNS_MISMATCH,
              "reply: QR=0 is ignored (MISMATCH)");
    }
    /* every truncation of a valid reply is handled without an over-read */
    {
        size_t cut;
        int ok = 1;
        for (cut = 0; cut < m.n; cut++) {
            struct msg t = m; t.n = cut;
            enum tcpip_dns_rc rc = parse_exact(&t, 0x1234, "www.example.org", &a);
            if (rc == TCPIP_DNS_OK) ok = 0;
        }
        check(ok, "reply: every truncated prefix is refused (never OK)");
    }

    /* CNAME then A */
    hdr(&m, 7, 0x8180, 1, 2); question(&m, "www.example.org");
    {
        size_t cname_rdata;
        put16(&m, 0xc00c); put16(&m, TCPIP_DNS_TYPE_CNAME); put16(&m, 1); put32(&m, 60);
        put16(&m, 0);                         /* RDLENGTH patched below */
        cname_rdata = m.n;
        put8(&m, 4); memcpy(m.b + m.n, "edge", 4); m.n += 4; put16(&m, 0xc010); /* edge.example.org */
        m.b[cname_rdata - 2] = 0; m.b[cname_rdata - 1] = (unsigned char)(m.n - cname_rdata);
        put16(&m, 0xc000 | (unsigned)cname_rdata); put16(&m, TCPIP_DNS_TYPE_A); put16(&m, 1);
        put32(&m, 60); put16(&m, 4); put8(&m, 10); put8(&m, 0); put8(&m, 0); put8(&m, 7);
    }
    a = 0;
    check(parse_exact(&m, 7, "www.example.org", &a) == TCPIP_DNS_OK && a == ip(10,0,0,7),
          "reply: CNAME then A follows the chain");

    /* NXDOMAIN / SERVFAIL / TC */
    hdr(&m, 9, 0x8183, 1, 0); question(&m, "nope.example.org");
    check(parse_exact(&m, 9, "nope.example.org", &a) == TCPIP_DNS_NXDOMAIN,
          "reply: RCODE 3 is NXDOMAIN");
    hdr(&m, 9, 0x8182, 1, 0); question(&m, "nope.example.org");
    check(parse_exact(&m, 9, "nope.example.org", &a) == TCPIP_DNS_SERVFAIL,
          "reply: RCODE 2 is SERVFAIL");
    hdr(&m, 9, 0x8380, 1, 0); question(&m, "big.example.org");
    check(parse_exact(&m, 9, "big.example.org", &a) == TCPIP_DNS_TRUNCATED,
          "reply: TC is TRUNCATED");
    hdr(&m, 9, 0x8180, 1, 0); question(&m, "empty.example.org");
    check(parse_exact(&m, 9, "empty.example.org", &a) == TCPIP_DNS_NODATA,
          "reply: RCODE 0 with no answer is NODATA");

    /* pointer loop in an answer owner (points at itself) */
    hdr(&m, 5, 0x8180, 1, 1); question(&m, "x.org");
    { size_t at = m.n; put16(&m, 0xc000 | (unsigned)at); }
    put16(&m, 1); put16(&m, 1); put32(&m, 1); put16(&m, 4); put32(&m, 0x01020304);
    check(parse_exact(&m, 5, "x.org", &a) == TCPIP_DNS_MALFORMED,
          "reply: self-pointer (loop) is MALFORMED");
    /* pointer past the end */
    hdr(&m, 5, 0x8180, 1, 1); question(&m, "x.org");
    put16(&m, 0xc000 | 0x3ff0); put16(&m, 1); put16(&m, 1); put32(&m, 1); put16(&m, 4);
    put32(&m, 0x01020304);
    check(parse_exact(&m, 5, "x.org", &a) == TCPIP_DNS_MALFORMED,
          "reply: pointer past the end is MALFORMED");
    /* huge RDLENGTH */
    hdr(&m, 5, 0x8180, 1, 1); question(&m, "x.org");
    put16(&m, 0xc00c); put16(&m, 1); put16(&m, 1); put32(&m, 1); put16(&m, 0xffff);
    put32(&m, 0x01020304);
    check(parse_exact(&m, 5, "x.org", &a) == TCPIP_DNS_MALFORMED,
          "reply: RDLENGTH past the end is MALFORMED");
    /* A with RDLENGTH 16 is not taken as an address */
    hdr(&m, 5, 0x8180, 1, 1); question(&m, "x.org");
    put16(&m, 0xc00c); put16(&m, 1); put16(&m, 1); put32(&m, 1); put16(&m, 16);
    put32(&m, 1); put32(&m, 2); put32(&m, 3); put32(&m, 4);
    check(parse_exact(&m, 5, "x.org", &a) == TCPIP_DNS_NODATA,
          "reply: A record with RDLENGTH != 4 is ignored (NODATA)");
    /* reserved label type 0x40 */
    hdr(&m, 5, 0x8180, 1, 1); question(&m, "x.org");
    put8(&m, 0x41); put8(&m, 'a'); put8(&m, 0);
    put16(&m, 1); put16(&m, 1); put32(&m, 1); put16(&m, 4); put32(&m, 0x01020304);
    check(parse_exact(&m, 5, "x.org", &a) == TCPIP_DNS_MALFORMED,
          "reply: reserved label type is MALFORMED");

    printf("=== test_tcpip_resolve: %s (%d failure%s) ===\n",
           failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
