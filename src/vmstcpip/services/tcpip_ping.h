/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tcpip_ping.h - TCP/IP Services for OVMX: PING (ICMP echo) over BGn: (vms-80b).
 *
 * PING was the one client tool tcpip_client.h deferred HONESTLY (a NOOP, never a
 * fake): it needs a RAW/ICMP executive socket, not the TCP stream socket the
 * TELNET/FTP tools ride. This header is that tool -- an ICMP echo engine that
 * drives its round-trip over the executive-resident INET pseudo-device BGn:
 * (vms-527) THE ORDINARY VMS WAY: $ASSIGN a channel to TCPIP$DEVICE:, then $QIO
 * on it. The one difference from the TCP tools is the socket KIND: IO$_SETMODE
 * carries the P2 socket-kind selector IO$K_SOCK_ICMP (iodef.h), so the executive
 * mints a raw AF_INET/SOCK_RAW/IPPROTO_ICMP host socket instead of a TCP stream
 * socket. From there it is the SAME public services the TCP tools use:
 *   IO$_SETMODE (P2=IO$K_SOCK_ICMP) -> create the raw ICMP socket
 *   IO$_ACCESS                      -> connect the socket to the target (so the
 *                                      datagram needs no per-send destination)
 *   IO$_WRITEVBLK                   -> send the ICMP echo request
 *   IO$_READVBLK                    -> receive ICMP datagrams (IP header + ICMP)
 *   IO$_DEACCESS / $DASSGN          -> shut down + release
 *
 * WHERE THE STACK IS. The socket is EXECUTIVE-RESIDENT (in vms.ko, over the host
 * in-kernel socket API); OVMX never reimplements ICMP or IP. What the ENGINE
 * builds is only the ICMP ECHO PDU (type 8 / code 0 / checksum / id / seq /
 * payload -- RFC 792) and its checksum; the host kernel builds the IP header,
 * routes the datagram, and generates the echo reply. On loopback (127.0.0.1) the
 * raw socket sees both the looped request (type 8) and the reply (type 0); the
 * engine matches on the echo REPLY carrying its own id/seq, exactly as a real
 * ping does. If /dev/vms is absent, $ASSIGN TCPIP$DEVICE: returns SS$_NOSUCHDEV
 * and every entry point fails honestly (CLAUDE.md Rule 9 / INV-6) -- never a
 * per-process fake that reports success while sharing nothing.
 *
 * WHY A HEADER-RESIDENT LIBRARY. Same reason tcpip_client.h is one: the SAME
 * engine backs the DCL PING verb (src/vmsdcl/dcl_cmd_misc.c, the user surface)
 * and the QEMU proof (tests/qemu/test_syssvc_tcpip_ping.c, byte-exact against a
 * real /dev/vms), so the shipped verb and the proven code are IDENTICAL. Every
 * function is `static inline`, so a TU using a subset draws no unused-function
 * diagnostic and no new compiled object joins DCL.EXE's native link graph.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8). The ICMP echo PDU + the Internet checksum are
 * from RFC 792 / RFC 1071 (open IETF standards -- no clean-room constraint on
 * the wire). The VMS-facing surface (TCPIP$DEVICE:, the QIO function map, the
 * P2 socket-kind selector) is from public VSI OpenVMS TCP/IP Services docs +
 * the already-landed BGn: driver; the P2 selector is a LABELED OVMX design
 * choice (iodef.h), never presented as a VMS-authentic layout.
 */

#ifndef _OVMX_TCPIP_PING_H
#define _OVMX_TCPIP_PING_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "starlet.h"
#include "descrip.h"
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"

/* ICMP message types (RFC 792). */
#define TCPIP_ICMP_ECHOREPLY 0u
#define TCPIP_ICMP_ECHO      8u

/* An ICMP echo header is 8 bytes: type, code, checksum(2), id(2), seq(2). */
#define TCPIP_ICMP_HDR_LEN   8u

/* Cap one echo PDU (header + payload) well under the BGn: per-QIO transfer cap
 * (VMS_BG_IOCTL_MAXLEN == 4096; repeated as a plain constant so this header
 * depends on no kernel header, exactly as tcpip_client.h's TCPIP_XFER_MAX does). */
#define TCPIP_PING_MAX_PDU   1024u

/* The 8-byte socket address the BGn: IO$_ACCESS handler reads (matches
 * struct bg_sockaddr_in in src/libvms/syssvc/sys_qio.c). For a raw ICMP socket
 * the port is unused; connect just sets the datagram destination. */
struct tcpip_ping_sockaddr { uint16_t family; uint16_t port; uint32_t addr; };

/* The Internet checksum (RFC 1071): 16-bit one's-complement sum over the PDU,
 * treating the bytes as big-endian 16-bit words. Endianness-independent: the
 * bytes are combined explicitly, and the result placed big-endian is what the
 * receiver's own sum folds to zero. */
static inline uint16_t tcpip_icmp_cksum(const unsigned char *p, uint32_t len)
{
    uint32_t sum = 0;
    uint32_t i;

    for (i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)(((uint32_t)p[i] << 8) | (uint32_t)p[i + 1]);
    if (i < len)
        sum += (uint32_t)((uint32_t)p[i] << 8);      /* odd trailing byte */
    while (sum >> 16)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)(~sum & 0xffffu);
}

/*
 * tcpip_ping_echo - one ICMP echo round-trip over BGn: to addr_be (network
 * order), tagging the request with (id, seq) and payload[payload_len]. On a
 * matching echo reply, the reply's echoed payload is copied to reply_out (up to
 * reply_cap) and its length written to *reply_len; returns a VMS status. The
 * engine does NOT itself judge byte-exactness -- it returns the received payload
 * so the CALLER (DCL verb / QEMU proof) compares -- so a fault in what is SENT
 * reddens the caller's byte-exact assertion while the round-trip itself still
 * completes.
 */
static inline uint32_t tcpip_ping_echo(uint32_t addr_be, uint16_t id,
                                       uint16_t seq,
                                       const void *payload, uint32_t payload_len,
                                       unsigned char *reply_out,
                                       uint32_t reply_cap, uint32_t *reply_len)
{
    static const char devname[] = "TCPIP$DEVICE:";
    struct dsc$descriptor_s dev;
    struct tcpip_ping_sockaddr sa;
    struct _iosb iosb;
    unsigned short chan = 0;
    unsigned char pdu[TCPIP_PING_MAX_PDU];
    unsigned char rbuf[TCPIP_PING_MAX_PDU];
    uint32_t pdu_len;
    uint32_t st;
    uint16_t ck;
    int attempts;

    if (reply_len)
        *reply_len = 0;
    if (payload_len > TCPIP_PING_MAX_PDU - TCPIP_ICMP_HDR_LEN)
        return SS$_BADPARAM;

    dev.dsc$w_length  = (uint16_t)(sizeof(devname) - 1);
    dev.dsc$b_dtype   = DSC$K_DTYPE_T;
    dev.dsc$b_class   = DSC$K_CLASS_S;
    dev.dsc$a_pointer = (char *)devname;

    /* $ASSIGN TCPIP$DEVICE: (fails SS$_NOSUCHDEV with no executive). */
    st = sys$assign(&dev, &chan, 0, NULL);
    if (!(st & 1))
        return st;

    /* IO$_SETMODE with the raw-ICMP socket-kind selector in P2. */
    st = sys$qiow(0, chan, IO$_SETMODE, &iosb, NULL, 0,
                  NULL, IO$K_SOCK_ICMP, 0, 0, 0, 0);
    if (!(st & 1)) { sys$dassgn(chan); return st; }

    /* IO$_ACCESS -- connect the raw socket to the target (port unused for ICMP). */
    sa.family = 2;                      /* AF_INET */
    sa.port   = 0;
    sa.addr   = addr_be;
    st = sys$qiow(0, chan, IO$_ACCESS, &iosb, NULL, 0,
                  &sa, (uint32_t)sizeof(sa), 0, 0, 0, 0);
    if (!(st & 1)) { sys$dassgn(chan); return st; }

    /* Build the ICMP echo request (RFC 792): type/code/checksum/id/seq/payload. */
    memset(pdu, 0, sizeof(pdu));
    pdu[0] = (unsigned char)TCPIP_ICMP_ECHO;   /* type = 8 */
    pdu[1] = 0;                                /* code = 0 */
    pdu[2] = 0; pdu[3] = 0;                    /* checksum = 0 for the sum */
    pdu[4] = (unsigned char)(id >> 8);   pdu[5] = (unsigned char)(id & 0xffu);
    pdu[6] = (unsigned char)(seq >> 8);  pdu[7] = (unsigned char)(seq & 0xffu);
    if (payload_len)
        memcpy(pdu + TCPIP_ICMP_HDR_LEN, payload, payload_len); /* NEGCTL tcpip-ping-payload-dropped */
    pdu_len = TCPIP_ICMP_HDR_LEN + payload_len;
    ck = tcpip_icmp_cksum(pdu, pdu_len);
    pdu[2] = (unsigned char)(ck >> 8); pdu[3] = (unsigned char)(ck & 0xffu);

    /* IO$_WRITEVBLK -- send the echo request. */
    st = sys$qiow(0, chan, IO$_WRITEVBLK, &iosb, NULL, 0,
                  pdu, pdu_len, 0, 0, 0, 0);
    if (!(st & 1)) {
        sys$qiow(0, chan, IO$_DEACCESS, &iosb, NULL, 0, NULL, 0, 0, 0, 0, 0);
        sys$dassgn(chan);
        return st;
    }

    /* IO$_READVBLK -- read ICMP datagrams (IP header + ICMP) until the echo
     * REPLY carrying our id/seq arrives. The raw socket also sees the looped
     * echo REQUEST (type 8) and possibly unrelated ICMP; both are skipped. The
     * loop is bounded so an environment that never replies fails honestly rather
     * than blocking forever (the QEMU proof's alarm() also bounds a wedge). */
    st = SS$_ABORT;
    for (attempts = 0; attempts < 16; attempts++) {
        uint32_t bcnt, ihl, icmp_off, icmp_len;
        uint32_t rst = sys$qiow(0, chan, IO$_READVBLK, &iosb, NULL, 0,
                                rbuf, (uint32_t)sizeof(rbuf), 0, 0, 0, 0);
        if (!(rst & 1)) { st = rst; break; }
        bcnt = iosb.iosb$w_bcnt;
        if (bcnt < 1)
            continue;
        ihl = (uint32_t)(rbuf[0] & 0x0fu) * 4u;         /* IPv4 IHL, in bytes */
        if (ihl < 20u || ihl + TCPIP_ICMP_HDR_LEN > bcnt)
            continue;                                   /* not a full ICMP echo */
        icmp_off = ihl;
        icmp_len = bcnt - icmp_off;
        if (rbuf[icmp_off] != TCPIP_ICMP_ECHOREPLY)     /* skip type 8 (looped req) */
            continue;
        {
            uint16_t rid  = (uint16_t)((rbuf[icmp_off + 4] << 8) | rbuf[icmp_off + 5]);
            uint16_t rseq = (uint16_t)((rbuf[icmp_off + 6] << 8) | rbuf[icmp_off + 7]);
            uint32_t rplen;
            if (rid != id || rseq != seq)
                continue;                               /* someone else's echo */
            rplen = icmp_len - TCPIP_ICMP_HDR_LEN;
            if (reply_out && reply_cap) {
                uint32_t take = (rplen < reply_cap) ? rplen : reply_cap;
                memcpy(reply_out, rbuf + icmp_off + TCPIP_ICMP_HDR_LEN, take);
                if (reply_len)
                    *reply_len = take;
            } else if (reply_len) {
                *reply_len = rplen;
            }
            st = SS$_NORMAL;
            break;
        }
    }

    sys$qiow(0, chan, IO$_DEACCESS, &iosb, NULL, 0, NULL, 0, 0, 0, 0, 0);
    sys$dassgn(chan);
    return st;
}

#endif /* _OVMX_TCPIP_PING_H */
