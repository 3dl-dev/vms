/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tcpip_resolve.h - TCP/IP Services for OVMX: the host-name resolver behind
 * $ TELNET name / $ FTP name / $ PING name (vms-a9e7).
 *
 * ORDER (the TCP/IP Services resolver order, as a VMS system manager configures
 * it with TCPIP SET HOST and TCPIP SET NAME_SERVICE):
 *   1. the local host database, SYS$SYSTEM:TCPIP$HOST.DAT (the caller reads it
 *      through RMS over the Files-11 ACP and hands each record to
 *      tcpip_hosts_match_line below);
 *   2. DNS (RFC 1035) to the server named in SYS$SYSTEM:TCPIP$NAMESERVICE.DAT
 *      ("SERVER=ip", optional "DOMAIN=x"; tcpip_ns_parse_line), appending the
 *      domain to a dotless name.
 *
 * WHERE THE STACK IS. The DNS query travels as ONE UDP datagram over the
 * executive-resident INET pseudo-device BGn: (vms-527) THE ORDINARY VMS WAY:
 *   $ASSIGN TCPIP$DEVICE:
 *   IO$_SETMODE  P2 = IO$K_SOCK_UDP     -> the executive mints a UDP socket in
 *                                          vms.ko (host in-kernel socket API)
 *   IO$_SETMODE  P4 = socket-option list -> TCPIP$C_RCVTIMEO (iodef.h), so a
 *                                          silent server ends SS$_TIMEOUT
 *   IO$_ACCESS   server:53              -> connect (sets the datagram peer)
 *   IO$_WRITEVBLK / IO$_READVBLK        -> one datagram each way
 *   IO$_DEACCESS / $DASSGN
 * OVMX builds only the DNS MESSAGE (RFC 1035 sec. 4); the host kernel carries the
 * UDP/IP. Nothing here calls getaddrinfo/gethostbyname/res_*, reads /etc/hosts
 * or /etc/resolv.conf, or opens a libc socket. With no executive, $ASSIGN
 * TCPIP$DEVICE: fails SS$_NOSUCHDEV and the resolver reports it (Rule 9).
 *
 * THE RESPONSE PARSER IS A PEER-RECEIVE PATH: every byte of a reply is
 * attacker-controlled (anyone who can put a datagram on the wire). Every read
 * is bounds-checked against the received length; compression pointers must
 * point strictly BACKWARD and are hop-limited; the reply's ID and question must
 * match the query or the datagram is ignored; CNAME chains are length-limited;
 * an A record is accepted only with RDLENGTH 4. tests/vmstcpip/
 * test_tcpip_resolve{,_fuzz}.c hold it to that under ASan.
 *
 * WHY A HEADER-RESIDENT LIBRARY: the same reason as tcpip_ping.h -- the DCL verb
 * (src/vmsdcl/dcl_cmd_misc.c), the host unit/fuzz tests and the QEMU proof
 * (tests/qemu/test_syssvc_bg_udp.c) all compile this one source.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8). The DNS wire format is RFC 1035 (an open IETF
 * standard). The VMS-facing surface (TCPIP$DEVICE:, the QIO function map, the
 * host and name-service databases) is from public VSI TCP/IP Services docs and
 * the already-landed OVMX BGn: driver; the IO$K_SOCK_UDP selector and the
 * TCPIP$C_* option values are LABELLED OVMX design choices (iodef.h).
 */

#ifndef _OVMX_TCPIP_RESOLVE_H
#define _OVMX_TCPIP_RESOLVE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "starlet.h"
#include "descrip.h"
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"
#include "tcpip_qio.h"
#include "tcpip_client.h"   /* tcpip_parse_ipv4, tcpip_port_be */

/* ---- limits ------------------------------------------------------------- */

#define TCPIP_DNS_PORT          53u
#define TCPIP_DNS_HDR_LEN       12u
#define TCPIP_DNS_MAX_NAME      255u    /* RFC 1035 2.3.4: octets in a name   */
#define TCPIP_DNS_MAX_LABEL     63u
#define TCPIP_DNS_MAX_UDP       512u    /* RFC 1035 4.2.1: UDP message cap    */
#define TCPIP_DNS_RBUF          1024u   /* receive buffer (> 512; extras ignored) */
#define TCPIP_DNS_MAX_HOPS      32      /* compression pointers per name      */
#define TCPIP_DNS_MAX_CNAME     8       /* CNAME links followed               */
#define TCPIP_DNS_READS_PER_TRY 8       /* foreign datagrams skipped per try  */
#define TCPIP_DNS_TRIES         2       /* one send + one retry               */
#define TCPIP_DNS_TIMEOUT_MS    2000u

#define TCPIP_DNS_TYPE_A        1u
#define TCPIP_DNS_TYPE_CNAME    5u
#define TCPIP_DNS_CLASS_IN      1u
#define TCPIP_DNS_RCODE_NXDOMAIN 3u

/* Outcome of a resolution step. Distinct so the caller can report each one
 * honestly (NXDOMAIN, timeout and no-name-service are different failures). */
enum tcpip_dns_rc {
    TCPIP_DNS_OK = 0,       /* an A record for the name                      */
    TCPIP_DNS_MISMATCH,     /* not a reply to OUR query (ID/question/QR)     */
    TCPIP_DNS_MALFORMED,    /* reply violates RFC 1035 framing               */
    TCPIP_DNS_TRUNCATED,    /* TC set: the answer did not fit a datagram     */
    TCPIP_DNS_NXDOMAIN,     /* RCODE 3: the name does not exist              */
    TCPIP_DNS_SERVFAIL,     /* any other non-zero RCODE                      */
    TCPIP_DNS_NODATA,       /* RCODE 0 but no A record for the name          */
    TCPIP_DNS_TIMEOUT,      /* no reply from the server                      */
    TCPIP_DNS_NONET,        /* the BGn: exchange itself failed (vms status)  */
    TCPIP_DNS_BADNAME       /* the name cannot be encoded as a query         */
};

/* ---- small helpers ------------------------------------------------------- */

static inline char tcpip_res_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive compare of two names, ignoring ONE trailing '.' on either. */
static inline int tcpip_res_name_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    size_t i;

    if (la && a[la - 1] == '.') la--;
    if (lb && b[lb - 1] == '.') lb--;
    if (la != lb)
        return 0;
    for (i = 0; i < la; i++)
        if (tcpip_res_lower(a[i]) != tcpip_res_lower(b[i]))
            return 0;
    return 1;
}

static inline int tcpip_res_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* ---- (a) the host database ---------------------------------------------- */

/*
 * One TCPIP$HOST.DAT record: "address hostname [alias ...]" -- the
 * space-padded shape TCPIP SET HOST writes. Returns 1 and fills *addr_be when
 * `name` equals the hostname or any alias (case-insensitively); 0 otherwise,
 * including for comment ("#"/"!") and malformed records.
 */
static inline int tcpip_hosts_match_line(const char *line, const char *name,
                                         uint32_t *addr_be)
{
    char tok[256];
    uint32_t a = 0;
    const char *p = line;
    int field = 0;

    if (!line || !name || !*name)
        return 0;
    for (;;) {
        size_t n = 0;
        while (tcpip_res_is_space(*p)) p++;
        if (*p == '\0')
            return 0;
        if (field == 0 && (*p == '#' || *p == '!'))
            return 0;
        while (*p && !tcpip_res_is_space(*p)) {
            if (n + 1 >= sizeof(tok))
                return 0;                       /* over-long token: skip record */
            tok[n++] = *p++;
        }
        tok[n] = '\0';
        if (field == 0) {
            if (!tcpip_parse_ipv4(tok, &a))
                return 0;
        } else if (tcpip_res_name_eq(tok, name)) {
            if (addr_be)
                *addr_be = a;
            return 1;
        }
        field++;
    }
}

/*
 * One TCPIP$NAMESERVICE.DAT record: "SERVER=ip" or "DOMAIN=x" (written by
 * TCPIP SET NAME_SERVICE). Fills server_be / domain as found; returns 1 if the
 * line set something.
 */
static inline int tcpip_ns_parse_line(const char *line, uint32_t *server_be,
                                      int *have_server, char *domain,
                                      size_t domain_cap)
{
    if (!line)
        return 0;
    while (tcpip_res_is_space(*line)) line++;
    if (strncmp(line, "SERVER=", 7) == 0) {
        char ip[64];
        size_t n = 0;
        const char *v = line + 7;
        while (v[n] && !tcpip_res_is_space(v[n]) && n + 1 < sizeof(ip)) {
            ip[n] = v[n];
            n++;
        }
        ip[n] = '\0';
        if (tcpip_parse_ipv4(ip, server_be)) {
            if (have_server) *have_server = 1;
            return 1;
        }
        return 0;
    }
    if (strncmp(line, "DOMAIN=", 7) == 0 && domain && domain_cap) {
        size_t n = 0;
        const char *v = line + 7;
        while (v[n] && !tcpip_res_is_space(v[n]) && n + 1 < domain_cap) {
            domain[n] = v[n];
            n++;
        }
        domain[n] = '\0';
        return n > 0;
    }
    return 0;
}

/*
 * The name actually queried: `name`, plus "." DOMAIN when the name has no dot
 * and a domain is configured. Returns 0 if it does not fit `out`.
 */
static inline int tcpip_res_qualify(const char *name, const char *domain,
                                    char *out, size_t cap)
{
    size_t ln = strlen(name), ld = domain ? strlen(domain) : 0;

    if (strchr(name, '.') == NULL && ld) {
        if (ln + 1 + ld + 1 > cap)
            return 0;
        memcpy(out, name, ln);
        out[ln] = '.';
        memcpy(out + ln + 1, domain, ld + 1);
        return 1;
    }
    if (ln + 1 > cap)
        return 0;
    memcpy(out, name, ln + 1);
    return 1;
}

/* ---- (b) the query ------------------------------------------------------- */

/*
 * Build an RFC 1035 standard query (RD set) for `name`, QTYPE A, QCLASS IN,
 * into buf[cap]. Returns the message length, or 0 if the name is empty, has an
 * empty or >63-octet label, or encodes to more than 255 octets.
 */
static inline size_t tcpip_dns_build_query(unsigned char *buf, size_t cap,
                                           uint16_t id, const char *name)
{
    size_t off = TCPIP_DNS_HDR_LEN;
    size_t nlen = strlen(name);
    size_t start = 0, i;

    if (nlen && name[nlen - 1] == '.')
        nlen--;                                 /* an absolute name */
    if (nlen == 0 || nlen + 2 > TCPIP_DNS_MAX_NAME ||
        cap < TCPIP_DNS_HDR_LEN + nlen + 2 + 4)
        return 0;

    memset(buf, 0, TCPIP_DNS_HDR_LEN);
    buf[0] = (unsigned char)(id >> 8);
    buf[1] = (unsigned char)(id & 0xffu);
    buf[2] = 0x01;                              /* RD */
    buf[5] = 1;                                 /* QDCOUNT = 1 */

    for (i = 0; i <= nlen; i++) {
        if (i == nlen || name[i] == '.') {
            size_t ll = i - start;
            if (ll == 0 || ll > TCPIP_DNS_MAX_LABEL)
                return 0;
            buf[off++] = (unsigned char)ll;
            memcpy(buf + off, name + start, ll);
            off += ll;
            start = i + 1;
        }
    }
    buf[off++] = 0;                             /* root label */
    buf[off++] = 0; buf[off++] = TCPIP_DNS_TYPE_A;
    buf[off++] = 0; buf[off++] = TCPIP_DNS_CLASS_IN;
    return off;
}

/* A query ID that differs per call: the time of day mixed with a counter and a
 * stack address. Not cryptographic -- the ID + question match is the check that
 * stops a stray datagram being taken as the answer. */
static inline uint16_t tcpip_dns_new_id(void)
{
    static uint32_t ctr;
    uint64_t t = 0;
    uint32_t x;
    int local;

    (void)sys$gettim(&t);
    x = (uint32_t)t ^ (uint32_t)(t >> 32) ^ (uint32_t)(uintptr_t)&local ^
        (++ctr * 0x9e3779b9u);
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    return (uint16_t)(x ^ (x >> 16));
}

/* ---- (c) the response parser (PEER-RECEIVE: untrusted bytes) ------------- */

static inline uint16_t tcpip_dns_get16(const unsigned char *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | (unsigned)p[1]);
}

/*
 * Decode the (possibly compressed) name at *off in msg[len] into `out` as
 * lower-case dotted text (no trailing dot; the root is ""). On success *off is
 * advanced past the name's IN-PLACE bytes (a pointer counts its two octets)
 * and 0 is returned; -1 on any framing fault: a read past `len`, a reserved
 * label type, a pointer that does not point strictly backward, more than
 * TCPIP_DNS_MAX_HOPS pointers, or a name longer than 255 octets / `cap`.
 */
static inline int tcpip_dns_read_name(const unsigned char *msg, size_t len,
                                      size_t *off, char *out, size_t cap)
{
    size_t pos = *off;
    size_t end_inplace = 0;
    size_t olen = 0;        /* chars written to out (excl. NUL) */
    size_t wire = 0;        /* RFC 1035 encoded length so far */
    int hops = 0;
    int jumped = 0;

    if (cap == 0)
        return -1;
    for (;;) {
        unsigned c;
        if (pos >= len)
            return -1;
        c = msg[pos];
        if ((c & 0xc0u) == 0xc0u) {
            size_t target;
            if (pos + 1 >= len)
                return -1;
            target = ((size_t)(c & 0x3fu) << 8) | msg[pos + 1];
            if (target >= pos)                  /* forward/self pointer: a loop */
                return -1;
            if (++hops > TCPIP_DNS_MAX_HOPS)
                return -1;
            if (!jumped) {
                end_inplace = pos + 2;
                jumped = 1;
            }
            pos = target;
            continue;
        }
        if ((c & 0xc0u) != 0)
            return -1;                          /* 0x40 / 0x80: reserved */
        if (c == 0) {
            if (!jumped)
                end_inplace = pos + 1;
            break;
        }
        if (pos + 1 + c > len)
            return -1;
        wire += 1 + c;
        if (wire + 1 > TCPIP_DNS_MAX_NAME)
            return -1;
        if (olen + (olen ? 1u : 0u) + c + 1 > cap)
            return -1;
        if (olen)
            out[olen++] = '.';
        {
            unsigned k;
            for (k = 0; k < c; k++)
                out[olen++] = tcpip_res_lower((char)msg[pos + 1 + k]);
        }
        pos += 1 + c;
    }
    out[olen] = '\0';
    *off = end_inplace;
    return 0;
}

/* One resource record's fixed fields, read after its owner name. */
struct tcpip_dns_rr {
    char     owner[TCPIP_DNS_MAX_NAME + 1];
    uint16_t type, klass;
    size_t   rdata;         /* offset of RDATA in msg */
    uint16_t rdlength;
};

/* Read one RR at *off; advance past it. 0 / -1 (framing fault). */
static inline int tcpip_dns_read_rr(const unsigned char *msg, size_t len,
                                    size_t *off, struct tcpip_dns_rr *rr)
{
    size_t p = *off;

    if (tcpip_dns_read_name(msg, len, &p, rr->owner, sizeof(rr->owner)) != 0)
        return -1;
    if (p + 10 > len)
        return -1;
    rr->type     = tcpip_dns_get16(msg + p);
    rr->klass    = tcpip_dns_get16(msg + p + 2);
    rr->rdlength = tcpip_dns_get16(msg + p + 8);
    p += 10;
    if ((size_t)rr->rdlength > len - p)
        return -1;
    rr->rdata = p;
    *off = p + rr->rdlength;
    return 0;
}

/*
 * Parse a reply `msg[len]` to the query with `id` for `qname` (as sent, not
 * case-sensitive). On TCPIP_DNS_OK *addr_be gets the A record (network order)
 * for qname, following CNAMEs in the answer section. MISMATCH means the
 * datagram is not a reply to this query and must be ignored.
 */
static inline enum tcpip_dns_rc tcpip_dns_parse_reply(const unsigned char *msg,
                                                      size_t len, uint16_t id,
                                                      const char *qname,
                                                      uint32_t *addr_be)
{
    char want[TCPIP_DNS_MAX_NAME + 1];
    char target[TCPIP_DNS_MAX_NAME + 1];
    struct tcpip_dns_rr rr;
    uint16_t qd, an, qtype, qclass;
    unsigned rcode;
    size_t off, ans_off;
    int link;

    if (!msg || len < TCPIP_DNS_HDR_LEN)
        return TCPIP_DNS_MISMATCH;              /* cannot be a reply: ignore */
    if (tcpip_dns_get16(msg) != id)
        return TCPIP_DNS_MISMATCH;
    if ((msg[2] & 0x80u) == 0 || (msg[2] & 0x78u) != 0)
        return TCPIP_DNS_MISMATCH;              /* not a response / not QUERY */
    qd = tcpip_dns_get16(msg + 4);
    an = tcpip_dns_get16(msg + 6);
    rcode = msg[3] & 0x0fu;

    /* The question must be ours: exactly one, same name/type/class. */
    if (qd != 1)
        return TCPIP_DNS_MISMATCH;
    off = TCPIP_DNS_HDR_LEN;
    if (tcpip_dns_read_name(msg, len, &off, want, sizeof(want)) != 0 ||
        off + 4 > len)
        return TCPIP_DNS_MISMATCH;              /* question unverifiable: ignore */
    qtype  = tcpip_dns_get16(msg + off);
    qclass = tcpip_dns_get16(msg + off + 2);
    off += 4;
    if (!tcpip_res_name_eq(want, qname) || qtype != TCPIP_DNS_TYPE_A ||
        qclass != TCPIP_DNS_CLASS_IN)
        return TCPIP_DNS_MISMATCH;

    if (msg[2] & 0x02u)
        return TCPIP_DNS_TRUNCATED;             /* TC */
    if (rcode == TCPIP_DNS_RCODE_NXDOMAIN)
        return TCPIP_DNS_NXDOMAIN;
    if (rcode != 0)
        return TCPIP_DNS_SERVFAIL;

    /* Walk the answer section once to prove its framing, then follow the
     * owner chain qname -> CNAME ... -> A. */
    ans_off = off;
    {
        size_t p = ans_off;
        unsigned i;
        for (i = 0; i < an; i++)
            if (tcpip_dns_read_rr(msg, len, &p, &rr) != 0)
                return TCPIP_DNS_MALFORMED;
    }

    memcpy(target, want, sizeof(target));
    for (link = 0; link <= TCPIP_DNS_MAX_CNAME; link++) {
        size_t p = ans_off;
        unsigned i;
        int moved = 0;
        char next[TCPIP_DNS_MAX_NAME + 1];

        next[0] = '\0';
        for (i = 0; i < an; i++) {
            if (tcpip_dns_read_rr(msg, len, &p, &rr) != 0)
                return TCPIP_DNS_MALFORMED;
            if (rr.klass != TCPIP_DNS_CLASS_IN ||
                !tcpip_res_name_eq(rr.owner, target))
                continue;
            if (rr.type == TCPIP_DNS_TYPE_A && rr.rdlength == 4) {
                if (addr_be)
                    memcpy(addr_be, msg + rr.rdata, 4);
                return TCPIP_DNS_OK;
            }
            if (rr.type == TCPIP_DNS_TYPE_CNAME && !moved) {
                size_t q = rr.rdata;
                /* The CNAME's own name must lie within its RDATA (pointers
                 * may still reach earlier in the message). */
                if (tcpip_dns_read_name(msg, len, &q, next, sizeof(next)) != 0 ||
                    q > rr.rdata + rr.rdlength)
                    return TCPIP_DNS_MALFORMED;
                moved = 1;
            }
        }
        if (!moved)
            return TCPIP_DNS_NODATA;
        memcpy(target, next, sizeof(target)); /* NEGCTL tcpip-resolve-cname-not-followed */
    }
    return TCPIP_DNS_NODATA;                    /* CNAME chain too long */
}

/* ---- (d) the BGn: UDP exchange ------------------------------------------ */

struct tcpip_res_sockaddr { uint16_t family; uint16_t port; uint32_t addr; };

/* IO$_SETMODE P4: arm the channel's receive timeout (TCPIP$C_RCVTIMEO). */
static inline uint32_t tcpip_bg_set_rcvtimeo(uint16_t chan, uint32_t timeout_ms)
{
    struct tcpip_timeval32 tv;
    struct tcpip_item_list_2 opt, desc;
    struct _iosb iosb;
    uint32_t st;

    tv.tv_sec  = (int)(timeout_ms / 1000u);
    tv.tv_usec = (int)((timeout_ms % 1000u) * 1000u);
    opt.length  = (unsigned short)sizeof(tv);
    opt.type    = TCPIP$C_RCVTIMEO;
    opt.address = &tv;
    desc.length  = (unsigned short)sizeof(opt);
    desc.type    = TCPIP$C_SOCKOPT;
    desc.address = &opt;
    st = sys$qiow(0, chan, IO$_SETMODE, &iosb, NULL, 0, NULL, 0, 0,
                  (uintptr_t)&desc, 0, 0);
    if (st & 1)
        st = iosb.iosb$w_status;
    return st;
}

/*
 * Ask the DNS server at server_be:port_host for the A record of `fqdn` over a
 * BGn: UDP channel. Up to TCPIP_DNS_TRIES sends; after each, up to
 * TCPIP_DNS_READS_PER_TRY datagrams are read, a datagram that is not a reply to
 * this query (wrong ID/question) is ignored, and a read that times out moves to
 * the retry. *vms_status (optional) gets the last BGn: status.
 */
static inline enum tcpip_dns_rc tcpip_dns_query_server(uint32_t server_be,
                                                       uint16_t port_host,
                                                       const char *fqdn,
                                                       uint32_t timeout_ms,
                                                       uint32_t *addr_be,
                                                       uint32_t *vms_status)
{
    static const char devname[] = "TCPIP$DEVICE:";
    struct dsc$descriptor_s dev;
    struct tcpip_res_sockaddr sa;
    struct _iosb iosb;
    unsigned char q[TCPIP_DNS_MAX_UDP];
    unsigned char r[TCPIP_DNS_RBUF];
    unsigned short chan = 0;
    enum tcpip_dns_rc rc = TCPIP_DNS_TIMEOUT;
    uint16_t id = tcpip_dns_new_id();
    size_t qlen;
    uint32_t st;
    int tries;

    if (vms_status) *vms_status = SS$_NORMAL;
    qlen = tcpip_dns_build_query(q, sizeof(q), id, fqdn);
    if (qlen == 0)
        return TCPIP_DNS_BADNAME;

    dev.dsc$w_length  = (uint16_t)(sizeof(devname) - 1);
    dev.dsc$b_dtype   = DSC$K_DTYPE_T;
    dev.dsc$b_class   = DSC$K_CLASS_S;
    dev.dsc$a_pointer = (char *)devname;

    st = sys$assign(&dev, &chan, 0, NULL);      /* SS$_NOSUCHDEV: no executive */
    if (!(st & 1)) { if (vms_status) *vms_status = st; return TCPIP_DNS_NONET; }

    st = tcpip_qiow(chan, IO$_SETMODE, &iosb, NULL, IO$K_SOCK_UDP, 0);
    if (st & 1)
        st = tcpip_bg_set_rcvtimeo(chan, timeout_ms);
    if (st & 1) {
        sa.family = 2;                          /* AF_INET */
        sa.port   = tcpip_port_be(port_host);
        sa.addr   = server_be;
        st = tcpip_qiow(chan, IO$_ACCESS, &iosb, &sa, (uint32_t)sizeof(sa), 0);
    }
    if (!(st & 1)) {
        sys$dassgn(chan);
        if (vms_status) *vms_status = st;
        return TCPIP_DNS_NONET;
    }

    for (tries = 0; tries < TCPIP_DNS_TRIES; tries++) {
        int reads;
        st = tcpip_qiow(chan, IO$_WRITEVBLK, &iosb, q, (uint32_t)qlen, 0);
        if (!(st & 1)) { rc = TCPIP_DNS_NONET; break; }
        for (reads = 0; reads < TCPIP_DNS_READS_PER_TRY; reads++) {
            enum tcpip_dns_rc prc;
            st = tcpip_qiow(chan, IO$_READVBLK, &iosb, r, (uint32_t)sizeof(r), 0);
            if (st == SS$_TIMEOUT) { rc = TCPIP_DNS_TIMEOUT; break; }
            if (!(st & 1)) { rc = TCPIP_DNS_NONET; break; }
            prc = tcpip_dns_parse_reply(r, iosb.iosb$w_bcnt, id, fqdn, addr_be);
            if (prc == TCPIP_DNS_MISMATCH)
                continue;                       /* someone else's datagram */
            rc = prc;
            goto done;
        }
        if (rc == TCPIP_DNS_NONET)
            break;
        if (reads == TCPIP_DNS_READS_PER_TRY)
            rc = TCPIP_DNS_TIMEOUT;             /* only foreign datagrams */
    }
done:
    if (vms_status) *vms_status = st;
    sys$qiow(0, chan, IO$_DEACCESS, &iosb, NULL, 0, NULL, 0, 0, 0, 0, 0);
    sys$dassgn(chan);
    return rc;
}

#endif /* _OVMX_TCPIP_RESOLVE_H */
