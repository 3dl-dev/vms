/*
 * dnet_nsp.c - DECnet Phase IV NSP transport codec (rd vms-6986).
 *
 * See dnet_nsp.h for the full clean-room provenance statement, the layer
 * boundary, and every PDU layout. The field-by-field map below annotates the
 * NSP Connect Initiate PDU carried in specimen #3 from
 * docs/decnet-provenance-register.md sec 4.6 (rd vms-3be / PR #665) -- the
 * ground-truth capture this codec round-trips byte-identical.
 *
 * Specimen #3 is a 53-byte routing frame; its NSP PDU begins at frame offset
 * 0x18 (after the 2-byte data-link length prefix, a 1-byte routing pad, and the
 * 21-byte Phase IV long-data-packet routing header -- all owned by the routing
 * rung, not this codec). The 29-byte NSP Connect Initiate PDU decodes as:
 *
 *   off  bytes            field       value       decode
 *   ---  ---------------  ----------  ----------  --------------------------
 *    0   18               MSGFLG      0x18        connect initiate
 *    1   00 00            DSTADDR     0x0000      dest logical link 0 (LE)
 *    3   01 20            SRCADDR     0x2001      src logical link 8193 (LE)
 *    5   01               SERVICES    0x01        flow-control option
 *    6   03               INFO        0x03        NSP version 4.1
 *    7   b3 05            SEGSIZE     0x05b3=1459 requested segment size (LE)
 *    9   00 2a 02 00 1a   DATA        -           connect data / session
 *        02 20 20 06 53               "SYSTEM"    control payload (20 bytes,
 *        59 53 54 45 4d                           opaque to NSP); carries the
 *        27 00 00 00 00                           plaintext access-control
 *                                                 username "SYSTEM"
 *
 * (tcpdump decode of the frame: "1.1 > 1.2 51 conn-initiate 8193>0 ver 4.1
 * segsize 1459".) All little-endian scalar accesses are byte-wise so the codec
 * is endian-neutral and freestanding (no <endian.h>, no unaligned casts).
 */
#include "dnet_nsp.h"

#include <string.h>

static inline uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline void wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

/* Classify a MSGFLG byte into an enum dnet_nsp_type, or 0 if unsupported. */
static uint8_t nsp_classify(uint8_t msgflg)
{
    switch (msgflg & DNET_NSP_CLASS_MASK) {
    case DNET_NSP_CLASS_DATA:
        return DNET_NSP_T_DATA;
    case DNET_NSP_CLASS_ACK:
        /* 0x04 = data acknowledgement (this codec's scope). Other ack
         * subtypes (0x14 other-data ack, 0x24 connect ack) are out of scope. */
        if (msgflg == DNET_NSP_MSGFLG_ACK)
            return DNET_NSP_T_ACK;
        return 0;
    case DNET_NSP_CLASS_CTL:
        switch (msgflg & DNET_NSP_CTL_SUBMASK) {
        case (DNET_NSP_MSGFLG_CI & DNET_NSP_CTL_SUBMASK): return DNET_NSP_T_CI;
        case (DNET_NSP_MSGFLG_CC & DNET_NSP_CTL_SUBMASK): return DNET_NSP_T_CC;
        case (DNET_NSP_MSGFLG_DI & DNET_NSP_CTL_SUBMASK): return DNET_NSP_T_DI;
        default: return 0;
        }
    default:
        return 0; /* reserved class 0x0c */
    }
}

int dnet_nsp_decode(const uint8_t *buf, size_t len,
                    struct dnet_nsp_msg *out, size_t *consumed)
{
    if (!buf || !out)
        return DNET_NSP_EINVAL;
    /* Every NSP PDU is at least MSGFLG + DSTADDR + SRCADDR = 5 bytes. */
    if (len < 5)
        return DNET_NSP_ETRUNC;

    uint8_t msgflg = buf[0];
    uint8_t type = nsp_classify(msgflg);
    if (type == 0)
        return DNET_NSP_EBADTYPE;

    memset(out, 0, sizeof(*out));
    out->type    = type;
    out->msgflg  = msgflg;
    out->dstaddr = rd_le16(buf + 1);
    out->srcaddr = rd_le16(buf + 3);

    const uint8_t *p = buf + 5;      /* cursor past the common header */
    size_t remain = len - 5;

    switch (type) {
    case DNET_NSP_T_CI:
    case DNET_NSP_T_CC: {
        /* SERVICES(1) INFO(1) SEGSIZE(2) then opaque connect data. */
        if (remain < 4)
            return DNET_NSP_ETRUNC;
        out->services = p[0];
        out->info     = p[1];
        out->segsize  = rd_le16(p + 2);
        p += 4;
        remain -= 4;
        if (remain > DNET_NSP_MAX_DATA)
            return DNET_NSP_EBADLEN;
        out->datalen = (uint16_t)remain;
        if (remain)
            memcpy(out->data, p, remain);
        p += remain;
        remain = 0;
        break;
    }
    case DNET_NSP_T_DI: {
        /* REASON(2) then opaque disconnect data. */
        if (remain < 2)
            return DNET_NSP_ETRUNC;
        out->reason = rd_le16(p);
        p += 2;
        remain -= 2;
        if (remain > DNET_NSP_MAX_DATA)
            return DNET_NSP_EBADLEN;
        out->datalen = (uint16_t)remain;
        if (remain)
            memcpy(out->data, p, remain);
        p += remain;
        remain = 0;
        break;
    }
    case DNET_NSP_T_DATA: {
        /* Optional piggyback ACKNUM (present iff its bit15 QUAL is set),
         * then a mandatory SEGNUM, then the opaque segment data. */
        if (remain >= 2 && (rd_le16(p) & DNET_NSP_ACK_QUAL)) {
            out->has_acknum = 1;
            out->acknum = rd_le16(p);
            p += 2;
            remain -= 2;
        }
        if (remain < 2)
            return DNET_NSP_ETRUNC;   /* SEGNUM is mandatory */
        out->segnum = rd_le16(p);
        p += 2;
        remain -= 2;
        if (remain > DNET_NSP_MAX_DATA)
            return DNET_NSP_EBADLEN;
        out->datalen = (uint16_t)remain;
        if (remain)
            memcpy(out->data, p, remain);
        p += remain;
        remain = 0;
        break;
    }
    case DNET_NSP_T_ACK: {
        /* Mandatory ACKNUM, optional second (other-data) ack; no data. */
        if (remain < 2)
            return DNET_NSP_ETRUNC;
        out->has_acknum = 1;
        out->acknum = rd_le16(p);
        p += 2;
        remain -= 2;
        if (remain >= 2) {
            out->has_ackoth = 1;
            out->ackoth = rd_le16(p);
            p += 2;
            remain -= 2;
        }
        if (remain != 0)
            return DNET_NSP_EBADLEN;  /* a data-ack carries no payload */
        break;
    }
    default:
        return DNET_NSP_EBADTYPE;
    }

    if (consumed)
        *consumed = (size_t)(p - buf);
    return DNET_NSP_OK;
}

int dnet_nsp_encode(const struct dnet_nsp_msg *msg,
                    uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!msg || !buf)
        return DNET_NSP_EINVAL;
    if (msg->datalen > DNET_NSP_MAX_DATA)
        return DNET_NSP_EBADLEN;

    /* Compute the exact encoded size for this message type first, so a short
     * buffer is rejected (ENOSPACE) before any byte is written. */
    size_t need = 5; /* MSGFLG + DSTADDR + SRCADDR */
    switch (msg->type) {
    case DNET_NSP_T_CI:
    case DNET_NSP_T_CC:
        need += 4 + msg->datalen;             /* SERVICES INFO SEGSIZE + data */
        break;
    case DNET_NSP_T_DI:
        need += 2 + msg->datalen;             /* REASON + data */
        break;
    case DNET_NSP_T_DATA:
        need += (msg->has_acknum ? 2 : 0) + 2 + msg->datalen; /* [ACK] SEGNUM data */
        break;
    case DNET_NSP_T_ACK:
        if (msg->datalen != 0)
            return DNET_NSP_EBADLEN;          /* a data-ack carries no payload */
        need += 2 + (msg->has_ackoth ? 2 : 0);/* ACKNUM [ACKOTH] */
        break;
    default:
        return DNET_NSP_EBADTYPE;
    }
    if (cap < need)
        return DNET_NSP_ENOSPACE;

    buf[0] = msg->msgflg;
    wr_le16(buf + 1, msg->dstaddr);
    wr_le16(buf + 3, msg->srcaddr);
    uint8_t *p = buf + 5;

    switch (msg->type) {
    case DNET_NSP_T_CI:
    case DNET_NSP_T_CC:
        p[0] = msg->services;
        p[1] = msg->info;
        wr_le16(p + 2, msg->segsize);
        p += 4;
        if (msg->datalen)
            memcpy(p, msg->data, msg->datalen);
        p += msg->datalen;
        break;
    case DNET_NSP_T_DI:
        wr_le16(p, msg->reason);
        p += 2;
        if (msg->datalen)
            memcpy(p, msg->data, msg->datalen);
        p += msg->datalen;
        break;
    case DNET_NSP_T_DATA:
        if (msg->has_acknum) {
            /* Preserve the caller's QUAL bit if set; the field is a raw ack. */
            wr_le16(p, msg->acknum);
            p += 2;
        }
        wr_le16(p, msg->segnum);
        p += 2;
        if (msg->datalen)
            memcpy(p, msg->data, msg->datalen);
        p += msg->datalen;
        break;
    case DNET_NSP_T_ACK:
        wr_le16(p, msg->acknum);
        p += 2;
        if (msg->has_ackoth) {
            wr_le16(p, msg->ackoth);
            p += 2;
        }
        break;
    default:
        return DNET_NSP_EBADTYPE;
    }

    if (outlen)
        *outlen = (size_t)(p - buf);
    return DNET_NSP_OK;
}
