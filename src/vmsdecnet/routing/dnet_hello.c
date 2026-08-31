/*
 * dnet_hello.c - DECnet Phase IV Ethernet Endnode Hello codec (rd vms-851).
 *
 * See dnet_hello.h for the full clean-room provenance statement and the wire
 * layout. The field-by-field map below annotates specimen #1 from
 * docs/decnet-provenance-register.md sec 4.6 (rd vms-3be / PR #665), the
 * ground-truth capture this codec round-trips:
 *
 *   off  bytes            field        value      decode
 *   ---  ---------------  -----------  ---------  ---------------------------
 *    0   22 00            DATA LENGTH  0x0022=34  routing message length (LE)
 *    2   0d               RFLAGS       0x0d       control, endnode hello
 *    3   02               VERSION      2          vers 2
 *    4   00               ECO          0          eco 0
 *    5   00               USER ECO     0          ueco 0
 *    6   aa 00 04 00 01 04 ID          -          src node 1.1 (LE 0x0401)
 *   12   03               IINFO        0x03       node type 3 = endnode
 *   13   da 05            BLKSIZE      0x05da     blksize 1498 (LE)
 *   15   00               AREA         0
 *   16   00*8             SEED         0
 *   24   aa 00 04 00 00 00 NEIGHBOR    -          rtr 0.0 (LE 0x0000)
 *   30   0f 00            TIMER        0x000f=15  hello 15 (LE)
 *   32   00               MPD          0
 *   33   02               DATALEN      2          data 2
 *   34   aa aa            TEST DATA    -          2 bytes
 *   36   00*10            (eth pad)               to the 46-byte minimum frame
 *
 * All little-endian scalar accesses are done byte-wise so the codec is
 * endian-neutral and freestanding (no <endian.h>, no unaligned casts).
 */
#include "dnet_hello.h"

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

uint16_t dnet_addr_from_id(const uint8_t id[DNET_ADDR_LEN])
{
    /* AA-00-04-00-nn-nn: the low two bytes are the DECnet address, LE. */
    return (uint16_t)(id[4] | ((uint16_t)id[5] << 8));
}

int dnet_hello_decode(const uint8_t *buf, size_t len,
                      struct dnet_endnode_hello *out, size_t *consumed)
{
    if (!buf || !out)
        return DNET_HELLO_EINVAL;
    if (len < DNET_HELLO_LENPREFIX)
        return DNET_HELLO_ETRUNC;

    uint16_t msglen = rd_le16(buf);
    /* The routing message must at least cover the fixed part, and its declared
     * TEST DATA must fit our cap and the input buffer. */
    if (msglen < DNET_HELLO_FIXED_MSG)
        return DNET_HELLO_EBADLEN;
    size_t datalen = (size_t)msglen - DNET_HELLO_FIXED_MSG;
    if (datalen > DNET_HELLO_MAX_DATA)
        return DNET_HELLO_EBADLEN;
    if (len < (size_t)DNET_HELLO_LENPREFIX + msglen)
        return DNET_HELLO_ETRUNC;

    const uint8_t *p = buf + DNET_HELLO_LENPREFIX;
    memset(out, 0, sizeof(*out));
    out->rflags   = p[0];
    out->version  = p[1];
    out->eco      = p[2];
    out->user_eco = p[3];
    memcpy(out->id, p + 4, DNET_ADDR_LEN);          /* off 6 */
    out->iinfo    = p[10];                          /* off 12 */
    out->blksize  = rd_le16(p + 11);                /* off 13 */
    out->area     = p[13];                          /* off 15 */
    memcpy(out->seed, p + 14, DNET_SEED_LEN);       /* off 16 */
    memcpy(out->neighbor, p + 22, DNET_ADDR_LEN);   /* off 24 */
    out->timer    = rd_le16(p + 28);                /* off 30 */
    out->mpd      = p[30];                          /* off 32 */
    out->datalen  = p[31];                          /* off 33 */
    /* DATALEN byte must agree with the DATA LENGTH-derived count. */
    if (out->datalen != datalen)
        return DNET_HELLO_EBADLEN;
    if (datalen)
        memcpy(out->data, p + 32, datalen);         /* off 34 */

    if (consumed)
        *consumed = (size_t)DNET_HELLO_LENPREFIX + msglen;
    return DNET_HELLO_OK;
}

int dnet_hello_encode(const struct dnet_endnode_hello *msg,
                      uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!msg || !buf)
        return DNET_HELLO_EINVAL;
    if (msg->datalen > DNET_HELLO_MAX_DATA)
        return DNET_HELLO_EBADLEN;

    uint16_t msglen = (uint16_t)(DNET_HELLO_FIXED_MSG + msg->datalen);
    size_t body = (size_t)DNET_HELLO_LENPREFIX + msglen;
    /* Pad to the Ethernet minimum data-field length (data-link behaviour,
     * OVMX encoder choice -- see dnet_hello.h). */
    size_t total = body < DNET_ETH_MIN_PAYLOAD ? DNET_ETH_MIN_PAYLOAD : body;
    if (cap < total)
        return DNET_HELLO_ENOSPACE;

    wr_le16(buf, msglen);
    uint8_t *p = buf + DNET_HELLO_LENPREFIX;
    p[0] = msg->rflags;
    p[1] = msg->version;
    p[2] = msg->eco;
    p[3] = msg->user_eco;
    memcpy(p + 4, msg->id, DNET_ADDR_LEN);
    p[10] = msg->iinfo;
    wr_le16(p + 11, msg->blksize);
    p[13] = msg->area;
    memcpy(p + 14, msg->seed, DNET_SEED_LEN);
    memcpy(p + 22, msg->neighbor, DNET_ADDR_LEN);
    wr_le16(p + 28, msg->timer);
    p[30] = msg->mpd;
    p[31] = msg->datalen;
    if (msg->datalen)
        memcpy(p + 32, msg->data, msg->datalen);

    /* Zero-fill the minimum-frame pad. */
    if (total > body)
        memset(buf + body, 0, total - body);

    if (outlen)
        *outlen = total;
    return DNET_HELLO_OK;
}
