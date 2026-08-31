/*
 * dnet_router_hello.c - DECnet Phase IV Ethernet Router Hello codec
 *                       (rd vms-0aba, sibling of dnet_hello.c).
 *
 * See dnet_router_hello.h for the full clean-room provenance statement
 * (SPEC-DERIVED, not oracle-captured -- no committed router-hello wire
 * specimen exists in docs/decnet-provenance-register.md) and the wire
 * layout. The field-by-field map below documents the fixed-part offsets used
 * by the decoder/encoder:
 *
 *   off  bytes   field        decode
 *   ---  ------  -----------  ---------------------------------------------
 *    0   LE16    DATA LENGTH  routing message length (framing prefix)
 *    2   u8      RFLAGS       0x0b = control, router hello (msg type 5)
 *    3   u8      VERSION
 *    4   u8      ECO
 *    5   u8      USER ECO
 *    6   id[6]   ID           sender Ethernet id AA-00-04-00-nn-nn
 *   12   u8      IINFO        low 2 bits = node type (L1/L2 router)
 *   13   LE16    BLKSIZE
 *   15   u8      PRIORITY     designated-router election priority
 *   16   u8      AREA         reserved (spec-derived)
 *   17   LE16    TIMER        hello timer, seconds
 *   19   u8      MPD          reserved / must-be-zero
 *   20   var     ELIST        opaque trailing router-list bytes
 *
 * All little-endian scalar accesses are done byte-wise so the codec is
 * endian-neutral and freestanding (no <endian.h>, no unaligned casts) --
 * same discipline as dnet_hello.c.
 */
#include "dnet_router_hello.h"

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

int dnet_router_hello_decode(const uint8_t *buf, size_t len,
                             struct dnet_router_hello *out, size_t *consumed)
{
    if (!buf || !out)
        return DNET_ROUTER_HELLO_EINVAL;
    if (len < DNET_ROUTER_HELLO_LENPREFIX)
        return DNET_ROUTER_HELLO_ETRUNC;

    uint16_t msglen = rd_le16(buf);
    /* The routing message must at least cover the fixed part, and its
     * implied E-list must fit our cap and the input buffer. */
    if (msglen < DNET_ROUTER_HELLO_FIXED_MSG)
        return DNET_ROUTER_HELLO_EBADLEN;
    size_t elistlen = (size_t)msglen - DNET_ROUTER_HELLO_FIXED_MSG;
    if (elistlen > DNET_ROUTER_HELLO_MAX_ELIST)
        return DNET_ROUTER_HELLO_EBADLEN;
    if (len < (size_t)DNET_ROUTER_HELLO_LENPREFIX + msglen)
        return DNET_ROUTER_HELLO_ETRUNC;

    const uint8_t *p = buf + DNET_ROUTER_HELLO_LENPREFIX;
    memset(out, 0, sizeof(*out));
    out->rflags   = p[0];
    out->version  = p[1];
    out->eco      = p[2];
    out->user_eco = p[3];
    memcpy(out->id, p + 4, DNET_ADDR_LEN);   /* off 6 */
    out->iinfo    = p[10];                    /* off 12 */
    out->blksize  = rd_le16(p + 11);          /* off 13 */
    out->priority = p[13];                    /* off 15 */
    out->area     = p[14];                    /* off 16 */
    out->timer    = rd_le16(p + 15);          /* off 17 */
    out->mpd      = p[17];                    /* off 19 */
    out->elist_len = (uint8_t)elistlen;
    if (elistlen)
        memcpy(out->elist, p + 18, elistlen); /* off 20 */

    if (consumed)
        *consumed = (size_t)DNET_ROUTER_HELLO_LENPREFIX + msglen;
    return DNET_ROUTER_HELLO_OK;
}

int dnet_router_hello_encode(const struct dnet_router_hello *msg,
                             uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!msg || !buf)
        return DNET_ROUTER_HELLO_EINVAL;
    if (msg->elist_len > DNET_ROUTER_HELLO_MAX_ELIST)
        return DNET_ROUTER_HELLO_EBADLEN;

    uint16_t msglen = (uint16_t)(DNET_ROUTER_HELLO_FIXED_MSG + msg->elist_len);
    size_t body = (size_t)DNET_ROUTER_HELLO_LENPREFIX + msglen;
    /* Pad to the Ethernet minimum data-field length (data-link behaviour,
     * OVMX encoder choice -- see dnet_hello.c's identical convention). */
    size_t total = body < DNET_ETH_MIN_PAYLOAD ? DNET_ETH_MIN_PAYLOAD : body;
    if (cap < total)
        return DNET_ROUTER_HELLO_ENOSPACE;

    wr_le16(buf, msglen);
    uint8_t *p = buf + DNET_ROUTER_HELLO_LENPREFIX;
    p[0] = msg->rflags;
    p[1] = msg->version;
    p[2] = msg->eco;
    p[3] = msg->user_eco;
    memcpy(p + 4, msg->id, DNET_ADDR_LEN);
    p[10] = msg->iinfo;
    wr_le16(p + 11, msg->blksize);
    p[13] = msg->priority;
    p[14] = msg->area;
    wr_le16(p + 15, msg->timer);
    p[17] = msg->mpd;
    if (msg->elist_len)
        memcpy(p + 18, msg->elist, msg->elist_len);

    /* Zero-fill the minimum-frame pad. */
    if (total > body)
        memset(buf + body, 0, total - body);

    if (outlen)
        *outlen = total;
    return DNET_ROUTER_HELLO_OK;
}
