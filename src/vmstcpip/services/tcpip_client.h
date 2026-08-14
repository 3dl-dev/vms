/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tcpip_client.h - TCP/IP Services for OVMX: the client TOOL layer (vms-dbb).
 *
 * TELNET and FTP client protocol engines that drive their connections over the
 * INET pseudo-device BGn: (vms-527) THE ORDINARY VMS WAY -- $ASSIGN a channel
 * to TCPIP$DEVICE:, then $QIO on it (IO$_SETMODE creates the socket, IO$_ACCESS
 * connects, IO$_WRITEVBLK / IO$_READVBLK move data, IO$_DEACCESS shuts down,
 * $DASSGN releases). The socket is EXECUTIVE-RESIDENT (vms.ko, over the host
 * in-kernel socket API); there is NO userspace socket stack here -- this file
 * speaks ONLY the public $ASSIGN/$QIO/$DASSGN system services, exactly as
 * test_syssvc_bg_echo.c does. If /dev/vms is absent, $ASSIGN TCPIP$DEVICE:
 * returns SS$_NOSUCHDEV and every entry point fails honestly (CLAUDE.md Rule 9
 * / INV-6): no per-process fake that reports success while sharing nothing.
 *
 * WHY A HEADER-RESIDENT LIBRARY. The exact same protocol engines back two
 * consumers -- the DCL TELNET/FTP verbs (src/vmsdcl/dcl_cmd_misc.c, the user
 * surface) and the QEMU proof (tests/qemu/test_syssvc_tcpip_client.c, which
 * drives them byte-exact against a real /dev/vms). Both #include this one file
 * so the shipped verb and the proven code are the SAME code. Every function is
 * `static inline` so a translation unit that uses only a subset draws no
 * unused-function diagnostic, and so no new compiled object joins DCL.EXE's
 * VMS-native link graph (which would need the three-enumeration wiring). This
 * is the single-header-library idiom, chosen deliberately for that reason.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8). The TELNET NVT / IAC option handling is from
 * RFC 854; the FTP command/reply grammar and passive-mode (PASV) data model
 * are from RFC 959. Both are open IETF standards -- no clean-room constraint on
 * the wire (docs/design-tcpip-services-ovmx.md §7). The VMS-facing surface
 * (device name, QIO function map, DCL command grammar) is from the public VSI
 * OpenVMS TCP/IP Services documentation, never from VSI/HPE source.
 *
 * SCOPE (vms-dbb increment). CLIENT tools over loopback / a localhost peer.
 * FTP uses PASSIVE mode only (the client always connects the data channel, so
 * no listen/accept is needed -- that matches the vms-527 client path exactly).
 * Name resolution is not wired (the BIND resolver is a later phase): a host
 * argument must be a dotted-quad IPv4 literal, else the caller reports an
 * honest "resolver not available" error rather than guessing. PING (ICMP echo)
 * is NOT here: it needs a raw/ICMP executive socket (a vms.ko extension), out
 * of scope for this TCP-client increment -- see the item's deferral note.
 */

#ifndef _OVMX_TCPIP_CLIENT_H
#define _OVMX_TCPIP_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "starlet.h"
#include "descrip.h"
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"

/* The BGn: driver caps one $QIO transfer at 4096 bytes (VMS_BG_IOCTL_MAXLEN,
 * an OVMX design cap stated in src/kernel/vms_bg.h). Repeated here as a plain
 * constant so this header depends on no kernel header; a send larger than this
 * is chunked, a recv returns at most this per call (TCP is a byte stream). */
#define TCPIP_XFER_MAX 4096u

/* TELNET protocol bytes (RFC 854). */
#define TN_IAC  255u
#define TN_DONT 254u
#define TN_DO   253u
#define TN_WONT 252u
#define TN_WILL 251u
#define TN_SB   250u
#define TN_SE   240u

/* A BGn: connection: the VMS channel plus a small read buffer so callers can
 * read line-at-a-time (FTP control) or fixed amounts (FTP data) off a byte
 * stream that the host kernel may coalesce or split arbitrarily. */
struct tcpip_conn {
    uint16_t chan;                     /* $ASSIGN channel, 0 when closed */
    int      connected;                /* IO$_ACCESS succeeded */
    unsigned char rbuf[TCPIP_XFER_MAX];/* buffered unconsumed inbound bytes */
    uint32_t rlen;                     /* valid bytes in rbuf */
    uint32_t rpos;                     /* next unconsumed byte */
};

/* ---- byte-order + parsing helpers (no libc net calls, so nothing new lands
 * in DECC$SHR's symbol vector) --------------------------------------------- */

/* Host 16-bit -> a uint16 whose MEMORY is big-endian (network order), which is
 * what the executive hands straight to the host kernel's sockaddr_in.sin_port.
 * Endianness-independent: the bytes are placed explicitly. */
static inline uint16_t tcpip_port_be(uint16_t host_port)
{
    unsigned char b[2];
    uint16_t r;
    b[0] = (unsigned char)(host_port >> 8);
    b[1] = (unsigned char)(host_port & 0xffu);
    memcpy(&r, b, sizeof(r));
    return r;
}

/* Parse "a.b.c.d" into a network-order (big-endian memory) IPv4 address.
 * Returns 1 on success, 0 if the text is not a dotted-quad literal. */
static inline int tcpip_parse_ipv4(const char *s, uint32_t *addr_be)
{
    unsigned char oct[4];
    int i;
    const char *p = s;

    if (!s || !addr_be)
        return 0;
    for (i = 0; i < 4; i++) {
        unsigned int n = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            n = n * 10u + (unsigned int)(*p - '0');
            if (n > 255u)
                return 0;
            p++;
            digits++;
        }
        if (!digits)
            return 0;
        oct[i] = (unsigned char)n;
        if (i < 3) {
            if (*p != '.')
                return 0;
            p++;
        }
    }
    if (*p != '\0')
        return 0;
    memcpy(addr_be, oct, 4);
    return 1;
}

/* ---- BGn: connection primitives (public $ASSIGN/$QIO/$DASSGN only) -------- */

/* $ASSIGN TCPIP$DEVICE:, IO$_SETMODE (create socket), IO$_ACCESS (connect).
 * addr_be/port_host identify the peer. Returns a VMS status; on failure the
 * channel is already released. */
static inline uint32_t tcpip_connect(struct tcpip_conn *c,
                                     uint32_t addr_be, uint16_t port_host)
{
    static const char devname[] = "TCPIP$DEVICE:";
    struct dsc$descriptor_s dev;
    struct _iosb iosb;
    uint32_t st;
    /* The 8-byte socket address the BGn: IO$_ACCESS handler reads: AF_INET(2),
     * port (network order), IPv4 addr (network order). Matches the executive's
     * struct bg_sockaddr_in (src/libvms/syssvc/sys_qio.c). */
    struct { uint16_t family; uint16_t port; uint32_t addr; } sa;

    memset(c, 0, sizeof(*c));

    dev.dsc$w_length  = (uint16_t)(sizeof(devname) - 1);
    dev.dsc$b_dtype   = DSC$K_DTYPE_T;
    dev.dsc$b_class   = DSC$K_CLASS_S;
    dev.dsc$a_pointer = (char *)devname;

    st = sys$assign(&dev, &c->chan, 0, NULL);
    if (!(st & 1))
        return st;

    st = sys$qiow(0, c->chan, IO$_SETMODE, &iosb, NULL, 0,
                  NULL, 0, 0, 0, 0, 0);
    if (!(st & 1)) {
        sys$dassgn(c->chan);
        c->chan = 0;
        return st;
    }

    sa.family = 2;                      /* AF_INET */
    sa.port   = tcpip_port_be(port_host);
    sa.addr   = addr_be;
    st = sys$qiow(0, c->chan, IO$_ACCESS, &iosb, NULL, 0,
                  &sa, (uint32_t)sizeof(sa), 0, 0, 0, 0);
    if (!(st & 1)) {
        sys$dassgn(c->chan);
        c->chan = 0;
        return st;
    }
    c->connected = 1;
    return SS$_NORMAL;
}

/* IO$_DEACCESS (graceful shutdown) then $DASSGN. Idempotent. */
static inline void tcpip_close(struct tcpip_conn *c)
{
    struct _iosb iosb;
    if (c->chan) {
        if (c->connected)
            sys$qiow(0, c->chan, IO$_DEACCESS, &iosb, NULL, 0,
                     NULL, 0, 0, 0, 0, 0);
        sys$dassgn(c->chan);
    }
    c->chan = 0;
    c->connected = 0;
}

/* Send the whole buffer, chunked to the driver's per-QIO cap. */
static inline uint32_t tcpip_send_all(struct tcpip_conn *c,
                                      const void *buf, uint32_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    uint32_t off = 0;

    while (off < len) {
        struct _iosb iosb;
        uint32_t chunk = len - off;
        uint32_t st;
        if (chunk > TCPIP_XFER_MAX)
            chunk = TCPIP_XFER_MAX;
        st = sys$qiow(0, c->chan, IO$_WRITEVBLK, &iosb, NULL, 0,
                      (void *)(p + off), chunk, 0, 0, 0, 0);
        if (!(st & 1))
            return st;
        if (iosb.iosb$w_bcnt == 0)
            return SS$_ABORT;           /* wrote nothing -- treat as broken */
        off += iosb.iosb$w_bcnt;
    }
    return SS$_NORMAL;
}

/* Refill the read buffer with one IO$_READVBLK. *got is the byte count (0 on an
 * orderly peer close = EOF). Returns a VMS status. */
static inline uint32_t tcpip_fill(struct tcpip_conn *c, uint32_t *got)
{
    struct _iosb iosb;
    uint32_t st;

    c->rpos = 0;
    c->rlen = 0;
    st = sys$qiow(0, c->chan, IO$_READVBLK, &iosb, NULL, 0,
                  c->rbuf, (uint32_t)sizeof(c->rbuf), 0, 0, 0, 0);
    if (!(st & 1)) {
        *got = 0;
        return st;
    }
    c->rlen = iosb.iosb$w_bcnt;
    *got = c->rlen;
    return SS$_NORMAL;
}

/* Read one raw byte off the stream (buffering as needed). Returns 1 on a byte
 * (in *out), 0 on EOF, -1 on error. */
static inline int tcpip_getc(struct tcpip_conn *c, unsigned char *out)
{
    if (c->rpos >= c->rlen) {
        uint32_t got = 0;
        uint32_t st = tcpip_fill(c, &got);
        if (!(st & 1))
            return -1;
        if (got == 0)
            return 0;                   /* EOF */
    }
    *out = c->rbuf[c->rpos++];
    return 1;
}

/* Read a CRLF- (or LF-) terminated line into out (NUL-terminated, terminator
 * stripped). Returns the line length in *len. Result: 1 = got a line, 0 = EOF
 * with nothing buffered, -1 = error. */
static inline int tcpip_recv_line(struct tcpip_conn *c, char *out,
                                  uint32_t outsz, uint32_t *len)
{
    uint32_t n = 0;
    int any = 0;

    for (;;) {
        unsigned char ch;
        int r = tcpip_getc(c, &ch);
        if (r < 0)
            return -1;
        if (r == 0) {                   /* EOF */
            if (!any) {
                *len = 0;
                return 0;
            }
            break;
        }
        any = 1;
        if (ch == '\n')
            break;
        if (ch == '\r')
            continue;                   /* fold CR of CRLF */
        if (n + 1 < outsz)
            out[n++] = (char)ch;
    }
    out[n] = '\0';
    *len = n;
    return 1;
}

/* ---- TELNET (RFC 854) ---------------------------------------------------- */

/* Send application text, byte-stuffing IAC (0xFF -> 0xFF 0xFF) so payload data
 * is never mistaken for a command. */
static inline uint32_t tcpip_telnet_send(struct tcpip_conn *c,
                                         const void *buf, uint32_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    unsigned char tmp[TCPIP_XFER_MAX];
    uint32_t i = 0, o = 0;

    while (i < len) {
        if (o >= sizeof(tmp) - 1) {
            uint32_t st = tcpip_send_all(c, tmp, o);
            if (!(st & 1))
                return st;
            o = 0;
        }
        if (p[i] == TN_IAC)
            tmp[o++] = TN_IAC;          /* escape */
        tmp[o++] = p[i++];
    }
    if (o)
        return tcpip_send_all(c, tmp, o);
    return SS$_NORMAL;
}

/* Read application data from the TELNET stream, handling inline IAC option
 * negotiation: every DO/WILL is refused (WONT/DONT) and every subnegotiation is
 * swallowed, so *out_len holds only the option-stripped application bytes the
 * server sent. Returns a VMS status; *out_len == 0 with success means EOF. */
static inline uint32_t tcpip_telnet_recv(struct tcpip_conn *c, char *out,
                                         uint32_t outsz, uint32_t *out_len)
{
    uint32_t n = 0;

    *out_len = 0;
    /* Read at least one buffer; process every byte currently available, then
     * return what accumulated. A first refill of 0 is EOF. */
    for (;;) {
        unsigned char ch;
        int r;

        if (c->rpos >= c->rlen) {
            if (n > 0)
                break;                  /* have data; don't block for more */
            {
                uint32_t got = 0;
                uint32_t st = tcpip_fill(c, &got);
                if (!(st & 1))
                    return st;
                if (got == 0)
                    break;              /* EOF */
            }
        }
        r = tcpip_getc(c, &ch);
        if (r < 0)
            return SS$_ABORT;
        if (r == 0)
            break;

        if (ch != TN_IAC) {
            if (n < outsz)
                out[n++] = (char)ch;
            continue;
        }

        /* IAC: read the command byte. */
        {
            unsigned char cmd;
            if (tcpip_getc(c, &cmd) <= 0)
                break;
            if (cmd == TN_IAC) {        /* escaped 0xFF = literal data byte */
                if (n < outsz)
                    out[n++] = (char)TN_IAC;
            } else if (cmd == TN_DO || cmd == TN_WILL) {
                unsigned char opt;
                unsigned char reply[3];
                if (tcpip_getc(c, &opt) <= 0)
                    break;
                reply[0] = TN_IAC;
                reply[1] = (cmd == TN_DO) ? TN_WONT : TN_DONT;
                reply[2] = opt;
                tcpip_send_all(c, reply, 3);   /* refuse every option */
            } else if (cmd == TN_DONT || cmd == TN_WONT) {
                unsigned char opt;
                (void)tcpip_getc(c, &opt);     /* acknowledge, no reply owed */
            } else if (cmd == TN_SB) {
                /* Swallow the subnegotiation up to IAC SE. */
                unsigned char b, prev = 0;
                while (tcpip_getc(c, &b) > 0) {
                    if (prev == TN_IAC && b == TN_SE)
                        break;
                    prev = b;
                }
            }
            /* other 2-byte commands (e.g. NOP, GA): command byte already
             * consumed, nothing further owed. */
        }
    }
    *out_len = n;
    return SS$_NORMAL;
}

/* ---- FTP (RFC 959), passive mode ---------------------------------------- */

/* Read one FTP reply, folding a multi-line ("NNN-...") reply into its final
 * line. Returns the 3-digit reply code in *code (0 if unparsable); the final
 * line text (code included) is copied to line. VMS status returned. */
static inline uint32_t tcpip_ftp_reply(struct tcpip_conn *c, int *code,
                                       char *line, uint32_t linesz)
{
    char buf[512];
    uint32_t len;

    for (;;) {
        int r = tcpip_recv_line(c, buf, sizeof(buf), &len);
        if (r < 0)
            return SS$_ABORT;
        if (r == 0)
            return SS$_ABORT;           /* EOF mid-reply */
        /* A final line is "NNN <text>"; a continuation is "NNN-<text>". */
        if (len >= 4 &&
            buf[0] >= '0' && buf[0] <= '9' &&
            buf[1] >= '0' && buf[1] <= '9' &&
            buf[2] >= '0' && buf[2] <= '9' &&
            buf[3] == ' ') {
            *code = (buf[0] - '0') * 100 + (buf[1] - '0') * 10 + (buf[2] - '0');
            if (linesz) {
                uint32_t k = (len < linesz - 1) ? len : linesz - 1;
                memcpy(line, buf, k);
                line[k] = '\0';
            }
            return SS$_NORMAL;
        }
        /* else: continuation or greeting fragment -- keep reading. */
    }
}

/* Send "CMD arg\r\n" on the control connection. */
static inline uint32_t tcpip_ftp_send_cmd(struct tcpip_conn *c,
                                          const char *verb, const char *arg)
{
    char out[600];
    uint32_t n = 0;
    uint32_t i;

    for (i = 0; verb[i] && n < sizeof(out) - 4; i++)
        out[n++] = verb[i];
    if (arg && arg[0]) {
        out[n++] = ' ';
        for (i = 0; arg[i] && n < sizeof(out) - 3; i++)
            out[n++] = arg[i];
    }
    out[n++] = '\r';
    out[n++] = '\n';
    return tcpip_send_all(c, out, n);
}

/* Parse a "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" reply. */
static inline int tcpip_ftp_parse_pasv(const char *line,
                                       uint32_t *addr_be, uint16_t *port)
{
    const char *p = strchr(line, '(');
    unsigned int v[6];
    int i;
    unsigned char b[4];

    if (!p)
        return 0;
    p++;
    for (i = 0; i < 6; i++) {
        unsigned int n = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            n = n * 10u + (unsigned int)(*p - '0');
            p++;
            digits++;
        }
        if (!digits)
            return 0;
        v[i] = n;
        if (i < 5) {
            if (*p != ',')
                return 0;
            p++;
        }
    }
    b[0] = (unsigned char)v[0];
    b[1] = (unsigned char)v[1];
    b[2] = (unsigned char)v[2];
    b[3] = (unsigned char)v[3];
    memcpy(addr_be, b, 4);
    *port = (uint16_t)(v[4] * 256u + v[5]);
    return 1;
}

/* Log in on an already-connected control channel (USER/PASS), returning the
 * VMS status. A non-2xx/3xx reply is reported as SS$_ABORT. */
static inline uint32_t tcpip_ftp_login(struct tcpip_conn *ctl,
                                       const char *user, const char *pass)
{
    char line[512];
    int code = 0;
    uint32_t st;

    st = tcpip_ftp_reply(ctl, &code, line, sizeof(line));   /* 220 greeting */
    if (!(st & 1) || code != 220)
        return SS$_ABORT;

    st = tcpip_ftp_send_cmd(ctl, "USER", user ? user : "anonymous");
    if (!(st & 1))
        return st;
    st = tcpip_ftp_reply(ctl, &code, line, sizeof(line));
    if (!(st & 1))
        return st;
    if (code == 331) {                                      /* password wanted */
        st = tcpip_ftp_send_cmd(ctl, "PASS", pass ? pass : "ovmx@ovmx");
        if (!(st & 1))
            return st;
        st = tcpip_ftp_reply(ctl, &code, line, sizeof(line));
        if (!(st & 1))
            return st;
    }
    if (code != 230)
        return SS$_ABORT;                                   /* not logged in */
    return SS$_NORMAL;
}

/* Open a PASV data connection for a transfer command. On success *data is a
 * connected channel and the control reply code for the transfer command is in
 * *xfer_code (a 1xx = transfer starting). */
static inline uint32_t tcpip_ftp_open_data(struct tcpip_conn *ctl,
                                           const char *verb, const char *arg,
                                           struct tcpip_conn *data,
                                           int *xfer_code)
{
    char line[512];
    int code = 0;
    uint32_t addr_be = 0;
    uint16_t port = 0;
    uint32_t st;

    /* Binary type, so a byte-exact transfer is byte-exact. */
    st = tcpip_ftp_send_cmd(ctl, "TYPE", "I");
    if (!(st & 1))
        return st;
    st = tcpip_ftp_reply(ctl, &code, line, sizeof(line));
    if (!(st & 1) || code != 200)
        return SS$_ABORT;

    st = tcpip_ftp_send_cmd(ctl, "PASV", NULL);
    if (!(st & 1))
        return st;
    st = tcpip_ftp_reply(ctl, &code, line, sizeof(line));
    if (!(st & 1) || code != 227)
        return SS$_ABORT;
    if (!tcpip_ftp_parse_pasv(line, &addr_be, &port))
        return SS$_ABORT;

    st = tcpip_connect(data, addr_be, port);
    if (!(st & 1))
        return st;

    st = tcpip_ftp_send_cmd(ctl, verb, arg);
    if (!(st & 1)) {
        tcpip_close(data);
        return st;
    }
    st = tcpip_ftp_reply(ctl, &code, line, sizeof(line));
    if (!(st & 1) || code >= 400) {     /* 1xx = starting; 4xx/5xx = refused */
        tcpip_close(data);
        return SS$_ABORT;
    }
    *xfer_code = code;
    return SS$_NORMAL;
}

/* RETR remote_file into out[outsz]; *out_len = bytes received. Full FTP client:
 * connect, log in, PASV, RETR, drain the data channel, confirm 226. */
static inline uint32_t tcpip_ftp_get(uint32_t server_addr_be, uint16_t server_port,
                                     const char *user, const char *pass,
                                     const char *remote_file,
                                     unsigned char *out, uint32_t outsz,
                                     uint32_t *out_len)
{
    struct tcpip_conn ctl, data;
    char line[512];
    int code = 0, xfer = 0;
    uint32_t total = 0;
    uint32_t st;

    *out_len = 0;

    st = tcpip_connect(&ctl, server_addr_be, server_port);
    if (!(st & 1))
        return st;
    st = tcpip_ftp_login(&ctl, user, pass);
    if (!(st & 1)) { tcpip_close(&ctl); return st; }

    st = tcpip_ftp_open_data(&ctl, "RETR", remote_file, &data, &xfer);
    if (!(st & 1)) { tcpip_close(&ctl); return st; }

    /* Drain the data connection to EOF. */
    for (;;) {
        uint32_t got = 0;
        st = tcpip_fill(&data, &got);
        if (!(st & 1)) { tcpip_close(&data); tcpip_close(&ctl); return st; }
        if (got == 0)
            break;
        {
            uint32_t take = got;
            if (total + take > outsz)
                take = outsz - total;
            if (take) {
                memcpy(out + total, data.rbuf, take);
                total += take;          /* NEGCTL tcpip-ftp-get-length-dropped */
            }
        }
        if (total >= outsz)
            break;
    }
    tcpip_close(&data);

    st = tcpip_ftp_reply(&ctl, &code, line, sizeof(line));   /* 226 complete */
    tcpip_ftp_send_cmd(&ctl, "QUIT", NULL);
    tcpip_ftp_reply(&ctl, &code, line, sizeof(line));        /* 221 bye */
    tcpip_close(&ctl);

    *out_len = total;
    return SS$_NORMAL;
}

/* STOR data[data_len] to remote_file. Full FTP client: connect, log in, PASV,
 * STOR, push the data, close the data channel, confirm 226. */
static inline uint32_t tcpip_ftp_put(uint32_t server_addr_be, uint16_t server_port,
                                     const char *user, const char *pass,
                                     const char *remote_file,
                                     const unsigned char *data, uint32_t data_len)
{
    struct tcpip_conn ctl, dc;
    char line[512];
    int code = 0, xfer = 0;
    uint32_t st;

    st = tcpip_connect(&ctl, server_addr_be, server_port);
    if (!(st & 1))
        return st;
    st = tcpip_ftp_login(&ctl, user, pass);
    if (!(st & 1)) { tcpip_close(&ctl); return st; }

    st = tcpip_ftp_open_data(&ctl, "STOR", remote_file, &dc, &xfer);
    if (!(st & 1)) { tcpip_close(&ctl); return st; }

    st = tcpip_send_all(&dc, data, data_len);
    tcpip_close(&dc);                    /* EOF tells the server the file ended */
    if (!(st & 1)) { tcpip_close(&ctl); return st; }

    st = tcpip_ftp_reply(&ctl, &code, line, sizeof(line));   /* 226 complete */
    /* Capture the STOR completion verdict BEFORE the QUIT/221 exchange reuses
     * `code` -- otherwise the 221 reply clobbers the 226 we just confirmed and
     * the completion check below always fails (the transfer itself succeeded). */
    {
        int completed = (st & 1) && code == 226;
        tcpip_ftp_send_cmd(&ctl, "QUIT", NULL);
        tcpip_ftp_reply(&ctl, &code, line, sizeof(line));    /* 221 bye */
        tcpip_close(&ctl);
        if (!completed)
            return SS$_ABORT;
    }
    return SS$_NORMAL;
}

#endif /* _OVMX_TCPIP_CLIENT_H */
