/*
 * dnet_cterm.c - DECnet Phase IV CTERM (Command Terminal) protocol: the
 *                terminal-service layered product behind $ SET HOST.
 *                See dnet_cterm.h for the full clean-room provenance (Rule 8).
 *
 *                The FOUNDATION (session-setup) messages are now
 *                ORACLE-GROUNDED (rd vms-bd0, real VAX1<->VAX2 $ SET HOST
 *                capture on vaxlab-3) -- see the "FOUNDATION MESSAGE CODEC"
 *                block in dnet_cterm.h for the full provenance and the
 *                dnet_cterm_found_* functions below. The TERMINAL-I/O message
 *                set below (Characteristics/Start Read/Read Data/OOB/Write/
 *                Unbind) remains ENTIRELY SPEC-DERIVED -- no oracle specimen
 *                exists for it yet -- and is proven only by round-trip, never
 *                presented as oracle-verified bytes.
 *
 * On-wire CTERM PDU layouts (little-endian scalars; counted strings are a
 * 1-byte length followed by that many bytes). Every PDU begins with the 1-byte
 * message type (enum dnet_cterm_msgtype). Types 1/2 (the old invented Bind /
 * Bind Accept) are gone from this switch -- see dnet_cterm_found_* instead.
 *
 *   Unbind (3):          type, reason
 *   Characteristics (4): type, term_type, width(LE2), page(LE2), char_flags(LE4)
 *   Start Read (5):      type, rd_flags(LE2), maxlen(LE2), timeout(LE2),
 *                        prompt[counted]
 *   Read Data (6):       type, terminator, datalen(LE2), data[datalen]
 *   Out-of-Band (7):     type, oob_char
 *   Write (8):           type, wr_flags(LE2), datalen(LE2), data[datalen]
 *   Write Complete (9):  type
 *   Clear Input (10):    type
 *   Discard (11):        type
 */
#include "dnet_cterm.h"

#include <string.h>

/* ---- little-endian scalar helpers (self-contained, like dnet_nsp.c) ------ */
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Copy a counted string (len byte + bytes) into dst (NUL-terminated, capped).
 * Returns bytes consumed, or -1 on truncation. */
static long get_string(const uint8_t *buf, size_t len, size_t off,
                       char *dst, size_t dstcap)
{
    if (off >= len)
        return -1;
    size_t n = buf[off];
    if (off + 1 + n > len)
        return -1;
    size_t copy = n < dstcap - 1 ? n : dstcap - 1;
    memcpy(dst, buf + off + 1, copy);
    dst[copy] = '\0';
    return (long)(1 + n);
}

/* Emit a counted string; returns bytes written or -1 on overflow. */
static long put_string(uint8_t *buf, size_t cap, size_t off, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n > 255 || off + 1 + n > cap)
        return -1;
    buf[off] = (uint8_t)n;
    if (n)
        memcpy(buf + off + 1, s, n);
    return (long)(1 + n);
}

/* ---- codec ---------------------------------------------------------------- */

int dnet_cterm_encode(const struct dnet_cterm_msg *msg,
                      uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!msg || !buf)
        return DNET_CTERM_EINVAL;

    size_t off = 0;
    long r;

    if (cap < 1)
        return DNET_CTERM_ENOSPACE;
    buf[off++] = msg->type;

    switch (msg->type) {
    case DNET_CTERM_MSG_UNBIND:
        if (off + 1 > cap) return DNET_CTERM_ENOSPACE;
        buf[off++] = msg->reason;
        break;

    case DNET_CTERM_MSG_CHARACTERISTICS:
        if (off + 1 + 2 + 2 + 4 > cap) return DNET_CTERM_ENOSPACE;
        buf[off++] = msg->term_type;
        put_u16(buf + off, msg->width); off += 2;
        put_u16(buf + off, msg->page);  off += 2;
        put_u32(buf + off, msg->char_flags); off += 4;
        break;

    case DNET_CTERM_MSG_START_READ:
        if (off + 2 + 2 + 2 > cap) return DNET_CTERM_ENOSPACE;
        put_u16(buf + off, msg->rd_flags);   off += 2;
        put_u16(buf + off, msg->rd_maxlen);  off += 2;
        put_u16(buf + off, msg->rd_timeout); off += 2;
        r = put_string(buf, cap, off, msg->prompt);
        if (r < 0) return DNET_CTERM_ENOSPACE;
        off += (size_t)r;
        break;

    case DNET_CTERM_MSG_READ_DATA:
        if (msg->datalen > DNET_CTERM_MAX_DATA) return DNET_CTERM_EBADLEN;
        if (off + 1 + 2 + msg->datalen > cap) return DNET_CTERM_ENOSPACE;
        buf[off++] = msg->terminator;
        put_u16(buf + off, msg->datalen); off += 2;
        if (msg->datalen) memcpy(buf + off, msg->data, msg->datalen);
        off += msg->datalen;
        break;

    case DNET_CTERM_MSG_OOB:
        if (off + 1 > cap) return DNET_CTERM_ENOSPACE;
        buf[off++] = msg->oob_char;
        break;

    case DNET_CTERM_MSG_WRITE:
        if (msg->datalen > DNET_CTERM_MAX_DATA) return DNET_CTERM_EBADLEN;
        if (off + 2 + 2 + msg->datalen > cap) return DNET_CTERM_ENOSPACE;
        put_u16(buf + off, msg->wr_flags); off += 2;
        put_u16(buf + off, msg->datalen);  off += 2;
        if (msg->datalen) memcpy(buf + off, msg->data, msg->datalen);
        off += msg->datalen;
        break;

    case DNET_CTERM_MSG_WRITE_COMPLETE:
    case DNET_CTERM_MSG_CLEAR_INPUT:
    case DNET_CTERM_MSG_DISCARD:
        /* type-only messages */
        break;

    default:
        return DNET_CTERM_EBADTYPE;
    }

    if (outlen)
        *outlen = off;
    return DNET_CTERM_OK;
}

int dnet_cterm_decode(const uint8_t *buf, size_t len,
                      struct dnet_cterm_msg *out, size_t *consumed)
{
    if (!buf || !out)
        return DNET_CTERM_EINVAL;
    if (len < 1)
        return DNET_CTERM_ETRUNC;

    memset(out, 0, sizeof(*out));
    out->type = buf[0];
    size_t off = 1;
    long r;

    switch (out->type) {
    case DNET_CTERM_MSG_UNBIND:
        if (off + 1 > len) return DNET_CTERM_ETRUNC;
        out->reason = buf[off++];
        break;

    case DNET_CTERM_MSG_CHARACTERISTICS:
        if (off + 1 + 2 + 2 + 4 > len) return DNET_CTERM_ETRUNC;
        out->term_type  = buf[off++];
        out->width      = get_u16(buf + off); off += 2;
        out->page       = get_u16(buf + off); off += 2;
        out->char_flags = get_u32(buf + off); off += 4;
        break;

    case DNET_CTERM_MSG_START_READ:
        if (off + 2 + 2 + 2 > len) return DNET_CTERM_ETRUNC;
        out->rd_flags   = get_u16(buf + off); off += 2;
        out->rd_maxlen  = get_u16(buf + off); off += 2;
        out->rd_timeout = get_u16(buf + off); off += 2;
        r = get_string(buf, len, off, out->prompt, sizeof(out->prompt));
        if (r < 0) return DNET_CTERM_ETRUNC;
        off += (size_t)r;
        break;

    case DNET_CTERM_MSG_READ_DATA:
        if (off + 1 + 2 > len) return DNET_CTERM_ETRUNC;
        out->terminator = buf[off++];
        out->datalen    = get_u16(buf + off); off += 2;
        if (out->datalen > DNET_CTERM_MAX_DATA) return DNET_CTERM_EBADLEN;
        if (off + out->datalen > len) return DNET_CTERM_ETRUNC;
        if (out->datalen) memcpy(out->data, buf + off, out->datalen);
        off += out->datalen;
        break;

    case DNET_CTERM_MSG_OOB:
        if (off + 1 > len) return DNET_CTERM_ETRUNC;
        out->oob_char = buf[off++];
        break;

    case DNET_CTERM_MSG_WRITE:
        if (off + 2 + 2 > len) return DNET_CTERM_ETRUNC;
        out->wr_flags = get_u16(buf + off); off += 2;
        out->datalen  = get_u16(buf + off); off += 2;
        if (out->datalen > DNET_CTERM_MAX_DATA) return DNET_CTERM_EBADLEN;
        if (off + out->datalen > len) return DNET_CTERM_ETRUNC;
        if (out->datalen) memcpy(out->data, buf + off, out->datalen);
        off += out->datalen;
        break;

    case DNET_CTERM_MSG_WRITE_COMPLETE:
    case DNET_CTERM_MSG_CLEAR_INPUT:
    case DNET_CTERM_MSG_DISCARD:
        break;

    default:
        return DNET_CTERM_EBADTYPE;
    }

    if (consumed)
        *consumed = off;
    return DNET_CTERM_OK;
}

/* ---- FOUNDATION message codec (rd vms-bd0) -------------------------------- */
/*
 * See the "FOUNDATION MESSAGE CODEC" block in dnet_cterm.h for the full
 * oracle provenance of every constant below (vaxlab-3, real-cterm-ci.pcap /
 * cterm-oracle-wid8.pcap). Every byte array here was copied verbatim off the
 * wire; nothing below is a guessed or spec-derived value.
 */

int dnet_cterm_found_short_build(uint8_t msg_code, uint8_t param_code,
                                 const uint8_t *value, uint8_t value_len,
                                 size_t total_len,
                                 uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!buf || (value_len && !value))
        return DNET_CTERM_EINVAL;
    if (value_len > DNET_CTERM_FOUND_VALUE_MAX)
        return DNET_CTERM_EBADLEN;
    size_t meaningful = 3u + value_len;
    size_t total = total_len > meaningful ? total_len : meaningful;
    if (total > cap)
        return DNET_CTERM_ENOSPACE;

    buf[0] = msg_code;
    buf[1] = param_code;
    buf[2] = value_len;
    if (value_len)
        memcpy(buf + 3, value, value_len);
    if (total > meaningful)
        memset(buf + meaningful, 0, total - meaningful);

    if (outlen)
        *outlen = total;
    return DNET_CTERM_OK;
}

int dnet_cterm_found_short_parse(const uint8_t *buf, size_t len,
                                 uint8_t *msg_code, uint8_t *param_code,
                                 uint8_t *value, size_t value_cap,
                                 uint8_t *value_len, size_t *consumed)
{
    if (!buf || !msg_code || !param_code || !value_len)
        return DNET_CTERM_EINVAL;
    if (len < 3)
        return DNET_CTERM_ETRUNC;

    uint8_t vl = buf[2];
    if (3u + vl > len)
        return DNET_CTERM_ETRUNC;
    if (vl > value_cap)
        return DNET_CTERM_EBADLEN;   /* refuse, never clip */

    *msg_code   = buf[0];
    *param_code = buf[1];
    *value_len  = vl;
    if (vl && value)
        memcpy(value, buf + 3, vl);
    if (consumed)
        *consumed = 3u + vl;
    return DNET_CTERM_OK;
}

int dnet_cterm_found_envelope_build(uint16_t len_field,
                                    const uint8_t *body, size_t body_len,
                                    uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!buf || (body_len && !body))
        return DNET_CTERM_EINVAL;
    if (4u + body_len > cap)
        return DNET_CTERM_ENOSPACE;

    buf[0] = 0x09;
    buf[1] = 0x00;
    put_u16(buf + 2, len_field);
    if (body_len)
        memcpy(buf + 4, body, body_len);

    if (outlen)
        *outlen = 4u + body_len;
    return DNET_CTERM_OK;
}

int dnet_cterm_found_envelope_parse(const uint8_t *buf, size_t len,
                                    uint16_t *len_field,
                                    const uint8_t **body, size_t *body_len,
                                    int *len_field_matches_body)
{
    if (!buf || !body || !body_len)
        return DNET_CTERM_EINVAL;
    if (len < 4)
        return DNET_CTERM_ETRUNC;
    if (buf[0] != 0x09 || buf[1] != 0x00)
        return DNET_CTERM_EBADTYPE;

    uint16_t lf = get_u16(buf + 2);
    if (len_field)
        *len_field = lf;
    *body = buf + 4;
    *body_len = len - 4;
    if (len_field_matches_body)
        *len_field_matches_body = (lf == (uint16_t)(len - 4)) ? 1 : 0;
    return DNET_CTERM_OK;
}

/* The host's seg-1 Start: msg-code 1, param-code 0x02, value 00 07 00 10,
 * zero-padded to 8 bytes total (oracle: both real-cterm-ci.pcap sessions and
 * cterm-oracle-wid8.pcap, byte-identical). */
static const uint8_t k_found_host_start_value[4] = { 0x00, 0x07, 0x00, 0x10 };

int dnet_cterm_found_host_start_build(uint8_t *buf, size_t cap, size_t *outlen)
{
    return dnet_cterm_found_short_build(0x01, DNET_CTERM_FOUND_PARAM_OBSERVED,
                                        k_found_host_start_value, 4, 8,
                                        buf, cap, outlen);
}

/* The client's seg-1 response: msg-code 4, param-code 0x02, value
 * 00 07 00 00, zero-padded to 17 bytes total (same three captures). */
static const uint8_t k_found_client_start_value[4] = { 0x00, 0x07, 0x00, 0x00 };

int dnet_cterm_found_client_start_build(uint8_t *buf, size_t cap, size_t *outlen)
{
    return dnet_cterm_found_short_build(0x04, DNET_CTERM_FOUND_PARAM_OBSERVED,
                                        k_found_client_start_value, 4, 17,
                                        buf, cap, outlen);
}

/* The host's seg-2 envelope: msgtype 9, len field 0x0017=23 (NOT equal to the
 * 31-byte body that follows -- the discrepancy documented in dnet_cterm.h),
 * body byte-identical across every capture examined (no session-variable
 * content resolved in it). */
static const uint8_t k_found_host_seg2_body[31] = {
    0x01, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x02, 0x10, 0x1e, 0x03, 0x04, 0xfe, 0xff, 0xef,
    0x00, 0x06, 0x00, 0x0b, 0x00, 0x08, 0x02, 0x02, 0x00
};

int dnet_cterm_found_host_seg2_build(uint8_t *buf, size_t cap, size_t *outlen)
{
    return dnet_cterm_found_envelope_build(0x0017, k_found_host_seg2_body,
                                           sizeof(k_found_host_seg2_body),
                                           buf, cap, outlen);
}

/* The client's seg-2 envelope body template: msgtype 9, len field 0x0035=53,
 * EQUAL to its 53-byte body (unlike the host's). WIDTH lives at body offset
 * 31-32 (2B LE, 0x0084=132 in every capture -- not diff-confirmed, see
 * dnet_cterm.h); PAGE at body offset 36-37 (2B LE), DIFF-CONFIRMED 24 vs 48.
 * The template below carries the default (132/24) values at those offsets;
 * dnet_cterm_found_client_termchar_build() overwrites them. */
static const uint8_t k_found_client_seg2_body_tmpl[53] = {
    0x01, 0x00, 0x01, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x02, 0xf2, 0x03, 0x02, 0x02, 0xc0, 0x03, 0x03,
    0x04, 0xfe, 0xff, 0xef, 0x00, 0x04, 0x18, 0x42, 0x20, 0x84, 0x00,
    0xa0, 0x02, 0x00, 0x18, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    /* [31-32] WIDTH (LE, 2 bytes); [36-37] PAGE (LE, 2 bytes) -- overwritten
     * by dnet_cterm_found_client_termchar_build() below. */
};
#define DNET_CTERM_FOUND_CLIENT_WIDTH_OFF  31
#define DNET_CTERM_FOUND_CLIENT_PAGE_OFF   36

int dnet_cterm_found_client_termchar_build(uint16_t width, uint16_t page,
                                           uint8_t *buf, size_t cap,
                                           size_t *outlen)
{
    uint8_t body[sizeof(k_found_client_seg2_body_tmpl)];

    memcpy(body, k_found_client_seg2_body_tmpl, sizeof(body));
    put_u16(body + DNET_CTERM_FOUND_CLIENT_WIDTH_OFF, width);
    put_u16(body + DNET_CTERM_FOUND_CLIENT_PAGE_OFF, page);

    return dnet_cterm_found_envelope_build(0x0035, body, sizeof(body),
                                           buf, cap, outlen);
}

int dnet_cterm_found_client_termchar_parse(const uint8_t *buf, size_t len,
                                           uint16_t *width, uint16_t *page)
{
    uint16_t lf;
    const uint8_t *body;
    size_t body_len;
    int rc = dnet_cterm_found_envelope_parse(buf, len, &lf, &body, &body_len, NULL);
    if (rc != DNET_CTERM_OK)
        return rc;
    if (body_len < DNET_CTERM_FOUND_CLIENT_PAGE_OFF + 2u)
        return DNET_CTERM_ETRUNC;
    if (width)
        *width = get_u16(body + DNET_CTERM_FOUND_CLIENT_WIDTH_OFF);
    if (page)
        *page = get_u16(body + DNET_CTERM_FOUND_CLIENT_PAGE_OFF);
    return DNET_CTERM_OK;
}

/* ---- Session Control connect message (SET HOST -> CTERM object) ---------- */
/*
 * The DNA Session Control CONNECT message, in the shape the ORACLE captured
 * (docs/oracle/vax-sethost-cterm.pcap frame 5; rd vms-558 / vms-f40). The full
 * provenance, the twenty specimen bytes and the security argument they settle
 * are in dnet_cterm.h -- read that block before touching anything here.
 *
 * ATTACKER-CONTROLLED INPUT. dnet_cterm_sc_connect_parse() runs on bytes that
 * arrived off the wire from an UNAUTHENTICATED peer. Every read below is
 * bounded against `len`; every counted string is refused rather than clipped
 * when it exceeds DNET_SC_MAX_STR; an unknown descriptor format is refused.
 * A malformed connect returns a negative DNET_CTERM_E* and the caller
 * disconnects -- it never over-reads, never allocates, never bugchecks.
 */

/* The MENUVER byte the oracle specimen carried. Recorded, not interpreted:
 * OVMX reads the access-control fields that follow if (and only if) bytes
 * remain, which is behaviourally identical for every value we have observed
 * and cannot over-read for any value we have not. */
#define SC_MENUVER_OBSERVED  0x27

/* Copy a counted string, REFUSING (not clipping) one longer than the cap.
 * Returns bytes consumed, DNET_CTERM_ETRUNC if the field runs off the end of
 * the message, or DNET_CTERM_EBADLEN if it is longer than dst can hold. */
static long sc_get_string(const uint8_t *buf, size_t len, size_t off,
                          char *dst, size_t dstcap)
{
    if (off >= len)
        return DNET_CTERM_ETRUNC;
    size_t n = buf[off];
    if (off + 1 + n > len)
        return DNET_CTERM_ETRUNC;
    if (n >= dstcap)
        return DNET_CTERM_EBADLEN;
    if (n)
        memcpy(dst, buf + off + 1, n);
    dst[n] = '\0';
    return (long)(1 + n);
}

/* Decode one end-user descriptor (DSTNAME / SRCNAME). Returns bytes consumed
 * or a negative DNET_CTERM_E*. */
static long sc_get_descriptor(const uint8_t *buf, size_t len, size_t off,
                              uint8_t *fmt, uint8_t *objtype,
                              uint16_t *grpcode, uint16_t *usrcode,
                              char *name, size_t namecap)
{
    long r;
    size_t start = off;

    if (off + 2 > len)
        return DNET_CTERM_ETRUNC;
    *fmt     = buf[off++];
    *objtype = buf[off++];

    switch (*fmt) {
    case DNET_SC_FMT_OBJECT:
        /* Object number only. A format-0 descriptor with objtype 0 names no
         * object at all -- refuse it rather than dispatch object 0. */
        if (*objtype == 0)
            return DNET_CTERM_EINVAL;
        break;

    case DNET_SC_FMT_CODED:
        if (off + 4 > len)
            return DNET_CTERM_ETRUNC;
        /* grpcode/usrcode are optional OUTPUTS (the destination descriptor has
         * no slot for them); the bytes are consumed either way. */
        if (grpcode) *grpcode = get_u16(buf + off);
        off += 2;
        if (usrcode) *usrcode = get_u16(buf + off);
        off += 2;
        /* fall through to the counted name */
        /* FALLTHROUGH */
    case DNET_SC_FMT_NAMED:
        r = sc_get_string(buf, len, off, name, namecap);
        if (r < 0)
            return r;
        off += (size_t)r;
        break;

    default:
        return DNET_CTERM_EINVAL;   /* unknown descriptor format */
    }

    return (long)(off - start);
}

/* Emit an end-user descriptor. Returns bytes written or -1 on overflow. */
static long sc_put_descriptor(uint8_t *buf, size_t cap, size_t off,
                              uint8_t fmt, uint8_t objtype,
                              uint16_t grpcode, uint16_t usrcode,
                              const char *name)
{
    size_t start = off;
    long r;

    if (off + 2 > cap)
        return -1;
    buf[off++] = fmt;
    buf[off++] = objtype;

    if (fmt == DNET_SC_FMT_CODED) {
        if (off + 4 > cap)
            return -1;
        put_u16(buf + off, grpcode); off += 2;
        put_u16(buf + off, usrcode); off += 2;
    }
    if (fmt == DNET_SC_FMT_CODED || fmt == DNET_SC_FMT_NAMED) {
        r = put_string(buf, cap, off, name);
        if (r < 0)
            return -1;
        off += (size_t)r;
    }
    return (long)(off - start);
}

int dnet_cterm_sc_connect_build(uint8_t dst_object,
                                const char *src_user,
                                uint16_t src_grpcode, uint16_t src_usrcode,
                                const char *username, const char *password,
                                const char *account,
                                uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!buf || dst_object == 0)
        return DNET_CTERM_EINVAL;
    if (src_user && strlen(src_user) > DNET_SC_MAX_STR)
        return DNET_CTERM_EBADLEN;

    size_t off = 0;
    long r;

    /* DSTNAME: format 0, the object number. The oracle's byte is 0x00 0x2a --
     * format ZERO, not one (the #1013 cut had this wrong). */
    r = sc_put_descriptor(buf, cap, off, DNET_SC_FMT_OBJECT, dst_object, 0, 0, NULL);
    if (r < 0) return DNET_CTERM_ENOSPACE;
    off += (size_t)r;

    /* SRCNAME: format 2, objtype 0, group/user codes, counted source user. */
    r = sc_put_descriptor(buf, cap, off, DNET_SC_FMT_CODED, 0,
                          src_grpcode, src_usrcode, src_user ? src_user : "");
    if (r < 0) return DNET_CTERM_ENOSPACE;
    off += (size_t)r;

    /* MENUVER + the counted access-control strings RQSTRID, PASSWRD, ACCOUNT --
     * then, for CTERM (object 42) ONLY, a fourth counted USRDATA field. The two
     * services carry a DIFFERENT number of connect-data fields on the real wire:
     *   - FAL (object 17): MENUVER 0x27 + RQSTRID + PASSWRD + ACCOUNT (THREE),
     *     the real COPY capture in docs/oracle/vax-copy-fal-dap.md §1, accepted.
     *   - CTERM (object 42): MENUVER 0x27 + RQSTRID + PASSWRD + ACCOUNT + USRDATA
     *     (FOUR), the real SET HOST Sample A + the k_oracle_sc_connect specimen.
     * OVMX emitted only three for CTERM too -- one byte short of every accepted
     * CTERM form; real OpenVMS session control read past end-of-data on the
     * missing USRDATA field and SILENTLY DISCARDED the Connect Initiate: no
     * reject, no counter, no OPCOM event, no LOGINOUT (proven on the isolated
     * lab -- VAX1 answered a disconnect in 165us and registered 1.42 as a fully
     * reachable adjacency, yet gave the 3-field CI zero response; vms-a70 dirA).
     * Clean-room: the connect-data layout is the uncopyrightable protocol fact;
     * the C is OVMX's own. */
    if (off + 1 > cap) return DNET_CTERM_ENOSPACE;
    buf[off++] = SC_MENUVER_OBSERVED;

    r = put_string(buf, cap, off, username); if (r < 0) return DNET_CTERM_ENOSPACE; off += (size_t)r;
    r = put_string(buf, cap, off, password); if (r < 0) return DNET_CTERM_ENOSPACE; off += (size_t)r;
    r = put_string(buf, cap, off, account);  if (r < 0) return DNET_CTERM_ENOSPACE; off += (size_t)r;
    if (dst_object == DNET_CTERM_OBJECT) {
        r = put_string(buf, cap, off, NULL); if (r < 0) return DNET_CTERM_ENOSPACE; off += (size_t)r;  /* CTERM USRDATA, empty */
    }

    if (outlen)
        *outlen = off;
    return DNET_CTERM_OK;
}

int dnet_cterm_sc_connect_parse(const uint8_t *buf, size_t len,
                                struct dnet_cterm_sc_connect *out)
{
    if (!buf || !out)
        return DNET_CTERM_EINVAL;

    memset(out, 0, sizeof(*out));

    size_t off = 0;
    long r;

    r = sc_get_descriptor(buf, len, off, &out->dst_format, &out->dst_object,
                          NULL /* no grpcode slot for dst */, NULL,
                          out->dst_task, sizeof(out->dst_task));
    if (r < 0) { memset(out, 0, sizeof(*out)); return (int)r; }
    off += (size_t)r;

    r = sc_get_descriptor(buf, len, off, &out->src_format, &out->src_object,
                          &out->src_grpcode, &out->src_usrcode,
                          out->src_user, sizeof(out->src_user));
    if (r < 0) { memset(out, 0, sizeof(*out)); return (int)r; }
    off += (size_t)r;

    /* MENUVER + access control. A connect that stops after the descriptors is
     * still well-formed (nothing to authenticate WITH -- which is the normal
     * case, per the oracle); it simply carries no access-control fields. */
    if (off >= len)
        return DNET_CTERM_OK;
    out->menuver = buf[off++];

    if (off >= len)
        return DNET_CTERM_OK;
    out->have_access_control = 1;

    r = sc_get_string(buf, len, off, out->rqstrid, sizeof(out->rqstrid));
    if (r < 0) { memset(out, 0, sizeof(*out)); return (int)r; }
    off += (size_t)r;

    /* PASSWRD: measured, NEVER RETAINED. Nothing in OVMX authenticates from a
     * wire-supplied password (the oracle proves real VMS does not either), so
     * the bytes are validated for length and then dropped -- there is no field
     * on this struct that could carry them into a login decision. */
    if (off < len) {
        size_t plen = buf[off];
        if (off + 1 + plen > len) { memset(out, 0, sizeof(*out)); return DNET_CTERM_ETRUNC; }
        if (plen > DNET_SC_MAX_STR) { memset(out, 0, sizeof(*out)); return DNET_CTERM_EBADLEN; }
        out->password_present = 1;
        out->password_len = (uint8_t)plen;
        off += 1 + plen;
    }

    if (off < len) {
        r = sc_get_string(buf, len, off, out->account, sizeof(out->account));
        if (r < 0) { memset(out, 0, sizeof(*out)); return (int)r; }
        off += (size_t)r;
    }

    /* Any trailing USRDATA is not interpreted; it is not read either. */
    return DNET_CTERM_OK;
}

int dnet_fal_access_decode(const uint8_t *buf, size_t len,
                           char *userid, size_t useridcap,
                           char *password, size_t passwordcap,
                           char *account, size_t accountcap)
{
    /*
     * THE FAL DIFFERENCE (rd vms-8c2, oracle docs/oracle/vax-copy-fal-dap.md
     * §1). Unlike CTERM/SET HOST -- where the access-control fields are EMPTY
     * and dnet_cterm_sc_connect_parse deliberately DROPS the password so no
     * wire-supplied credential can reach a decision -- an inbound FAL (object
     * 17) connect CARRIES the username AND password the server must
     * authenticate. So this decoder RETAINS the password, into a CALLER-OWNED
     * buffer the FAL server wipes the instant sysuaf_authenticate has consumed
     * it (dnet_fal.c). It is a SEPARATE, explicitly-named entry so the CTERM
     * codec's "never retains a password" invariant (and its test) stays intact:
     * only FAL, which must, ever sees the bytes.
     *
     * FULLY BOUNDED. These are attacker-controlled bytes on an unauthenticated
     * inbound connect. The walk reuses the same bounded descriptor/string
     * helpers the CTERM parse trusts (sc_get_descriptor / sc_get_string): it
     * never reads past buf[len-1] and refuses (does not clip) an over-long
     * counted field. On ANY malformation every output buffer is left empty and
     * a negative code is returned, so a caller cannot authenticate from a
     * half-parsed identity. "OVMX never crashes a peer": a hostile client
     * cannot fault FAL here.
     *
     * FIELD ORDER (oracle §1 + DNA Session Control): after the DSTNAME and
     * SRCNAME descriptors and the MENUVER byte come three counted access-
     * control strings -- RQSTRID (the username to authenticate), PASSWRD, and
     * ACCOUNT -- exactly the "06 'SYSTEM' 06 <pw> 00" the capture shows.
     */
    if (!buf || !userid || !password || !account ||
        useridcap == 0 || passwordcap == 0 || accountcap == 0)
        return DNET_CTERM_EINVAL;

    userid[0] = password[0] = account[0] = '\0';

    uint8_t  dfmt, dobj, sfmt, sobj;
    uint16_t grp, usr;
    char     dtask[DNET_SC_MAX_STR + 1], suser[DNET_SC_MAX_STR + 1];
    size_t   off = 0;
    long     r;

    r = sc_get_descriptor(buf, len, off, &dfmt, &dobj, NULL, NULL,
                          dtask, sizeof(dtask));
    if (r < 0) return (int)r;
    off += (size_t)r;

    r = sc_get_descriptor(buf, len, off, &sfmt, &sobj, &grp, &usr,
                          suser, sizeof(suser));
    if (r < 0) return (int)r;
    off += (size_t)r;

    /* MENUVER byte. A connect with no access-control area (nothing to
     * authenticate WITH) is well-formed but leaves every field empty -- the
     * FAL server then refuses the connect, it does not admit an empty-credential
     * session (that was the CTERM hole; FAL must not repeat it). */
    if (off >= len) return DNET_CTERM_OK;
    off++;  /* MENUVER, not interpreted here */

    /* RQSTRID = the username. */
    if (off >= len) return DNET_CTERM_OK;
    r = sc_get_string(buf, len, off, userid, useridcap);
    if (r < 0) { userid[0] = '\0'; return (int)r; }
    off += (size_t)r;

    /* PASSWRD = the password -- RETAINED (the FAL difference). */
    if (off < len) {
        r = sc_get_string(buf, len, off, password, passwordcap);
        if (r < 0) { userid[0] = password[0] = '\0'; return (int)r; }
        off += (size_t)r;
    }

    /* ACCOUNT (usually empty). */
    if (off < len) {
        r = sc_get_string(buf, len, off, account, accountcap);
        if (r < 0) { userid[0] = password[0] = account[0] = '\0'; return (int)r; }
        off += (size_t)r;
    }

    return DNET_CTERM_OK;
}

int dnet_cterm_sc_connect_object(const uint8_t *buf, size_t len)
{
    struct dnet_cterm_sc_connect sc;
    int rc = dnet_cterm_sc_connect_parse(buf, len, &sc);

    if (rc != DNET_CTERM_OK)
        return rc;
    if (sc.dst_format != DNET_SC_FMT_OBJECT)
        return DNET_CTERM_EINVAL;   /* a named task, not a well-known object */
    return sc.dst_object;
}

int dnet_cterm_remote_port_info(const struct dnet_cterm_sc_connect *sc,
                                uint16_t src_addr, char *out, size_t cap)
{
    if (!sc || !out || cap == 0)
        return DNET_CTERM_EINVAL;

    /* "<addr>::<user>" -- the oracle's SHOW TERMINAL form, e.g. 1025::SYSTEM.
     * The ADDRESS comes from the caller (the routing header the engine
     * decoded), never from the connect message: a peer must not be able to
     * name itself something it is not on the accounting surface. */
    unsigned n = 0;
    char digits[8];
    unsigned v = src_addr;
    unsigned d = 0;

    do { digits[d++] = (char)('0' + (v % 10)); v /= 10; } while (v && d < sizeof(digits));
    while (d) {
        if (n + 1 >= cap) return DNET_CTERM_ENOSPACE;
        out[n++] = digits[--d];
    }
    if (n + 2 >= cap) return DNET_CTERM_ENOSPACE;
    out[n++] = ':'; out[n++] = ':';

    /* The source user is UNTRUSTED text destined for a human surface: copy only
     * printable ASCII, so a peer cannot inject control characters (or an
     * escape sequence) into a console line or an accounting record. */
    for (const char *p = sc->src_user; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7e)
            continue;
        if (n + 1 >= cap) return DNET_CTERM_ENOSPACE;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return DNET_CTERM_OK;
}

/* Copy at most DNET_SC_MAX_STR printable-ASCII characters of `src` into `dst`
 * (dst is DNET_SC_MAX_STR + 1 bytes). Control characters and 8-bit bytes are
 * dropped, so nothing a peer put in a Session Control name can carry a control
 * or escape sequence past this boundary onto a human/accounting surface. */
static void desc_copy_printable(char *dst, const char *src)
{
    size_t n = 0;
    for (const char *p = src; *p && n < DNET_SC_MAX_STR; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7e)
            continue;
        dst[n++] = (char)c;
    }
    dst[n] = '\0';
}

int dnet_conn_descriptor_from_wire(const uint8_t *conn_data, size_t conn_len,
                                   uint16_t peer_addr,
                                   struct dnet_conn_descriptor *out)
{
    struct dnet_cterm_sc_connect sc;
    int rc;

    if (!out)
        return DNET_CTERM_EINVAL;

    /* Zero FIRST: a failure below leaves validated == 0 and no half-filled
     * identity, so a caller that ignores the return value still cannot hand a
     * usable descriptor to the privileged path. */
    memset(out, 0, sizeof(*out));

    /* THE bounded parse. Every field-length is checked, an over-long counted
     * string is refused (not clipped), an unknown descriptor format is refused,
     * and the password bytes are measured and dropped -- all inside here, on
     * the low-privilege side. */
    rc = dnet_cterm_sc_connect_parse(conn_data, conn_len, &sc);
    if (rc != DNET_CTERM_OK)
        return rc;                 /* out stays all-zero / unvalidated */

    out->dst_is_object = (sc.dst_format == DNET_SC_FMT_OBJECT) ? 1 : 0;
    out->dst_object    = sc.dst_object;
    out->peer_addr     = peer_addr;   /* engine state, NOT the wire */
    desc_copy_printable(out->proxy_user, sc.src_user);
    desc_copy_printable(out->proxy_task, sc.dst_task);

    /* Deliberately NOT copied: password_present/password_len (already dropped
     * by the parser), rqstrid/account, src_grpcode/usrcode, menuver -- none of
     * it may reach a session-creation decision. */

    out->validated = 1;
    return DNET_CTERM_OK;
}

int dnet_conn_descriptor_port_info(const struct dnet_conn_descriptor *desc,
                                   char *out, size_t cap)
{
    if (!desc || !out || cap == 0)
        return DNET_CTERM_EINVAL;

    unsigned n = 0;
    char digits[8];
    unsigned v = desc->peer_addr;
    unsigned d = 0;

    do { digits[d++] = (char)('0' + (v % 10)); v /= 10; } while (v && d < sizeof(digits));
    while (d) {
        if (n + 1 >= cap) return DNET_CTERM_ENOSPACE;
        out[n++] = digits[--d];
    }
    if (n + 2 >= cap) return DNET_CTERM_ENOSPACE;
    out[n++] = ':'; out[n++] = ':';

    /* proxy_user is already printable-filtered at from_wire time; copy as-is. */
    for (const char *p = desc->proxy_user; *p; p++) {
        if (n + 1 >= cap) return DNET_CTERM_ENOSPACE;
        out[n++] = *p;
    }
    out[n] = '\0';
    return DNET_CTERM_OK;
}

/* ---- session FSM --------------------------------------------------------- */

int dnet_cterm_session_init(struct dnet_cterm_session *s, enum dnet_cterm_role role)
{
    if (!s || (role != DNET_CTERM_ROLE_TERMINAL && role != DNET_CTERM_ROLE_HOST))
        return DNET_CTERM_EINVAL;
    memset(s, 0, sizeof(*s));
    s->role  = role;
    s->state = DNET_CTERM_S_CLOSED;
    return DNET_CTERM_OK;
}

int dnet_cterm_bind(struct dnet_cterm_session *s, const char *term_name,
                    uint8_t *out, size_t cap, size_t *outlen)
{
    if (!s || !out)
        return DNET_CTERM_EINVAL;
    if (s->role != DNET_CTERM_ROLE_TERMINAL || s->state != DNET_CTERM_S_CLOSED)
        return DNET_CTERM_ESTATE;

    /* rd vms-bd0: the real wire's foundation phase carries no terminal-name
     * field at all -- term_name is accepted for call-site compatibility and
     * ignored. See dnet_cterm.h for the vms-6165 host-speaks-first gap this
     * still does not close. */
    (void)term_name;
    int rc = dnet_cterm_found_client_start_build(out, cap, outlen);
    if (rc != DNET_CTERM_OK)
        return rc;
    s->state = DNET_CTERM_S_BINDING;
    return DNET_CTERM_OK;
}

int dnet_cterm_bind_accept(struct dnet_cterm_session *s, const char *host_name,
                           uint8_t *out, size_t cap, size_t *outlen)
{
    if (!s || !out)
        return DNET_CTERM_EINVAL;
    /* HOST accepts only after it has seen the inbound (real, rd vms-bd0)
     * foundation Bind -- found_bind_seen, set by dnet_cterm_rx() below. */
    if (s->role != DNET_CTERM_ROLE_HOST || s->state != DNET_CTERM_S_CLOSED ||
        !s->found_bind_seen)
        return DNET_CTERM_ESTATE;

    /* host_name is accepted for call-site compatibility and ignored -- see
     * dnet_cterm_bind() above; same real-wire fact applies. */
    (void)host_name;
    int rc = dnet_cterm_found_host_start_build(out, cap, outlen);
    if (rc != DNET_CTERM_OK)
        return rc;
    s->state = DNET_CTERM_S_BOUND;
    return DNET_CTERM_OK;
}

int dnet_cterm_send_characteristics(struct dnet_cterm_session *s,
                                    uint8_t term_type, uint16_t width,
                                    uint16_t page, uint32_t char_flags,
                                    uint8_t *out, size_t cap, size_t *outlen)
{
    if (!s || !out)
        return DNET_CTERM_EINVAL;
    if (s->state != DNET_CTERM_S_BOUND)
        return DNET_CTERM_ESTATE;

    struct dnet_cterm_msg m;
    memset(&m, 0, sizeof(m));
    m.type       = DNET_CTERM_MSG_CHARACTERISTICS;
    m.term_type  = term_type;
    m.width      = width;
    m.page       = page;
    m.char_flags = char_flags;
    int rc = dnet_cterm_encode(&m, out, cap, outlen);
    if (rc != DNET_CTERM_OK)
        return rc;
    s->term_type  = term_type;
    s->width      = width;
    s->page       = page;
    s->char_flags = char_flags;
    return DNET_CTERM_OK;
}

int dnet_cterm_start_read(struct dnet_cterm_session *s, const char *prompt,
                          uint16_t rd_flags, uint16_t maxlen, uint16_t timeout,
                          uint8_t *out, size_t cap, size_t *outlen)
{
    if (!s || !out)
        return DNET_CTERM_EINVAL;
    if (s->role != DNET_CTERM_ROLE_HOST || s->state != DNET_CTERM_S_BOUND)
        return DNET_CTERM_ESTATE;

    struct dnet_cterm_msg m;
    memset(&m, 0, sizeof(m));
    m.type       = DNET_CTERM_MSG_START_READ;
    m.rd_flags   = rd_flags;
    m.rd_maxlen  = maxlen;
    m.rd_timeout = timeout;
    if (prompt) {
        strncpy(m.prompt, prompt, sizeof(m.prompt) - 1);
        m.prompt[sizeof(m.prompt) - 1] = '\0';
    }
    int rc = dnet_cterm_encode(&m, out, cap, outlen);
    if (rc != DNET_CTERM_OK)
        return rc;
    s->reads_sent++;
    return DNET_CTERM_OK;
}

int dnet_cterm_read_data(struct dnet_cterm_session *s, const uint8_t *data,
                         size_t len, uint8_t terminator,
                         uint8_t *out, size_t cap, size_t *outlen)
{
    if (!s || !out || (len && !data))
        return DNET_CTERM_EINVAL;
    if (s->role != DNET_CTERM_ROLE_TERMINAL || s->state != DNET_CTERM_S_BOUND)
        return DNET_CTERM_ESTATE;
    if (len > DNET_CTERM_MAX_DATA)
        return DNET_CTERM_EBADLEN;

    struct dnet_cterm_msg m;
    memset(&m, 0, sizeof(m));
    m.type       = DNET_CTERM_MSG_READ_DATA;
    m.terminator = terminator;
    m.datalen    = (uint16_t)len;
    if (len) memcpy(m.data, data, len);
    int rc = dnet_cterm_encode(&m, out, cap, outlen);
    if (rc != DNET_CTERM_OK)
        return rc;
    s->reads_sent++;
    return DNET_CTERM_OK;
}

int dnet_cterm_oob(struct dnet_cterm_session *s, uint8_t oob_char,
                   uint8_t *out, size_t cap, size_t *outlen)
{
    if (!s || !out)
        return DNET_CTERM_EINVAL;
    if (s->role != DNET_CTERM_ROLE_TERMINAL || s->state != DNET_CTERM_S_BOUND)
        return DNET_CTERM_ESTATE;

    struct dnet_cterm_msg m;
    memset(&m, 0, sizeof(m));
    m.type     = DNET_CTERM_MSG_OOB;
    m.oob_char = oob_char;
    int rc = dnet_cterm_encode(&m, out, cap, outlen);
    if (rc != DNET_CTERM_OK)
        return rc;
    s->oob_sent++;
    return DNET_CTERM_OK;
}

int dnet_cterm_write(struct dnet_cterm_session *s, const uint8_t *data,
                     size_t len, uint16_t wr_flags,
                     uint8_t *out, size_t cap, size_t *outlen)
{
    if (!s || !out || (len && !data))
        return DNET_CTERM_EINVAL;
    if (s->role != DNET_CTERM_ROLE_HOST || s->state != DNET_CTERM_S_BOUND)
        return DNET_CTERM_ESTATE;
    if (len > DNET_CTERM_MAX_DATA)
        return DNET_CTERM_EBADLEN;

    struct dnet_cterm_msg m;
    memset(&m, 0, sizeof(m));
    m.type    = DNET_CTERM_MSG_WRITE;
    m.wr_flags = wr_flags;
    m.datalen = (uint16_t)len;
    if (len) memcpy(m.data, data, len);
    int rc = dnet_cterm_encode(&m, out, cap, outlen);
    if (rc != DNET_CTERM_OK)
        return rc;
    s->writes_sent++;
    return DNET_CTERM_OK;
}

int dnet_cterm_unbind(struct dnet_cterm_session *s, uint8_t reason,
                      uint8_t *out, size_t cap, size_t *outlen)
{
    if (!s || !out)
        return DNET_CTERM_EINVAL;
    if (s->state != DNET_CTERM_S_BOUND)
        return DNET_CTERM_ESTATE;

    struct dnet_cterm_msg m;
    memset(&m, 0, sizeof(m));
    m.type   = DNET_CTERM_MSG_UNBIND;
    m.reason = reason;
    int rc = dnet_cterm_encode(&m, out, cap, outlen);
    if (rc != DNET_CTERM_OK)
        return rc;
    s->state = DNET_CTERM_S_UNBOUND;
    return DNET_CTERM_OK;
}

int dnet_cterm_rx(struct dnet_cterm_session *s, const uint8_t *buf, size_t len,
                  enum dnet_cterm_event *event)
{
    if (event)
        *event = DNET_CTERM_EV_NONE;
    if (!s || !buf)
        return DNET_CTERM_EINVAL;

    /* FOUNDATION PHASE (rd vms-bd0): before the session is BOUND, CTERM
     * speaks the real short-TLV foundation messages, not the general PDU set
     * decoded below -- the client's real msg-code (4) collides on the wire
     * with DNET_CTERM_MSG_CHARACTERISTICS, so which decoder applies is gated
     * on role+state, never guessed from the byte alone (see dnet_cterm.h). */
    if (s->role == DNET_CTERM_ROLE_HOST && s->state == DNET_CTERM_S_CLOSED) {
        uint8_t msg_code, param_code, value_len;
        uint8_t value[DNET_CTERM_FOUND_VALUE_MAX];
        int rc = dnet_cterm_found_short_parse(buf, len, &msg_code, &param_code,
                                              value, sizeof(value), &value_len, NULL);
        if (rc != DNET_CTERM_OK)
            return rc;
        if (msg_code == 0x04) {
            s->found_bind_seen = 1;
            if (event)
                *event = DNET_CTERM_EV_BIND_IND;
        }
        return DNET_CTERM_OK;
    }
    if (s->role == DNET_CTERM_ROLE_TERMINAL && s->state == DNET_CTERM_S_BINDING) {
        uint8_t msg_code, param_code, value_len;
        uint8_t value[DNET_CTERM_FOUND_VALUE_MAX];
        int rc = dnet_cterm_found_short_parse(buf, len, &msg_code, &param_code,
                                              value, sizeof(value), &value_len, NULL);
        if (rc != DNET_CTERM_OK)
            return rc;
        if (msg_code == 0x01) {
            s->state = DNET_CTERM_S_BOUND;
            if (event)
                *event = DNET_CTERM_EV_BOUND;
        }
        return DNET_CTERM_OK;
    }

    struct dnet_cterm_msg m;
    int rc = dnet_cterm_decode(buf, len, &m, NULL);
    if (rc != DNET_CTERM_OK)
        return rc;

    enum dnet_cterm_event ev = DNET_CTERM_EV_NONE;

    switch (m.type) {
    case DNET_CTERM_MSG_CHARACTERISTICS:
        if (s->state == DNET_CTERM_S_BOUND) {
            s->last = m;
            s->term_type  = m.term_type;
            s->width      = m.width;
            s->page       = m.page;
            s->char_flags = m.char_flags;
            ev = DNET_CTERM_EV_CHARACTERISTICS;
        }
        break;

    case DNET_CTERM_MSG_START_READ:
        if (s->role == DNET_CTERM_ROLE_TERMINAL && s->state == DNET_CTERM_S_BOUND) {
            s->last = m;
            ev = DNET_CTERM_EV_START_READ;
        }
        break;

    case DNET_CTERM_MSG_READ_DATA:
        if (s->role == DNET_CTERM_ROLE_HOST && s->state == DNET_CTERM_S_BOUND) {
            s->last = m;
            s->reads_recv++;
            ev = DNET_CTERM_EV_READ_DATA;
        }
        break;

    case DNET_CTERM_MSG_OOB:
        if (s->role == DNET_CTERM_ROLE_HOST && s->state == DNET_CTERM_S_BOUND) {
            s->last = m;
            s->oob_recv++;
            ev = DNET_CTERM_EV_OOB;
        }
        break;

    case DNET_CTERM_MSG_WRITE:
        if (s->role == DNET_CTERM_ROLE_TERMINAL && s->state == DNET_CTERM_S_BOUND) {
            s->last = m;
            s->writes_recv++;
            ev = DNET_CTERM_EV_WRITE;
        }
        break;

    case DNET_CTERM_MSG_WRITE_COMPLETE:
        if (s->role == DNET_CTERM_ROLE_HOST && s->state == DNET_CTERM_S_BOUND) {
            s->last = m;
            ev = DNET_CTERM_EV_WRITE_COMPLETE;
        }
        break;

    case DNET_CTERM_MSG_CLEAR_INPUT:
        if (s->role == DNET_CTERM_ROLE_TERMINAL && s->state == DNET_CTERM_S_BOUND) {
            s->last = m;
            ev = DNET_CTERM_EV_CLEAR_INPUT;
        }
        break;

    case DNET_CTERM_MSG_UNBIND:
        if (s->state == DNET_CTERM_S_BOUND) {
            s->last = m;
            s->state = DNET_CTERM_S_UNBOUND;
            ev = DNET_CTERM_EV_UNBOUND;
        }
        break;

    default:
        break;
    }

    if (event)
        *event = ev;
    return DNET_CTERM_OK;
}

const char *dnet_cterm_state_name(enum dnet_cterm_state st)
{
    switch (st) {
    case DNET_CTERM_S_CLOSED:  return "CLOSED";
    case DNET_CTERM_S_BINDING: return "BINDING";
    case DNET_CTERM_S_BOUND:   return "BOUND";
    case DNET_CTERM_S_UNBOUND: return "UNBOUND";
    default:                   return "?";
    }
}

const char *dnet_cterm_msgtype_name(enum dnet_cterm_msgtype t)
{
    switch (t) {
    case DNET_CTERM_MSG_UNBIND:         return "UNBIND";
    case DNET_CTERM_MSG_CHARACTERISTICS:return "CHARACTERISTICS";
    case DNET_CTERM_MSG_START_READ:     return "START-READ";
    case DNET_CTERM_MSG_READ_DATA:      return "READ-DATA";
    case DNET_CTERM_MSG_OOB:            return "OUT-OF-BAND";
    case DNET_CTERM_MSG_WRITE:          return "WRITE";
    case DNET_CTERM_MSG_WRITE_COMPLETE: return "WRITE-COMPLETE";
    case DNET_CTERM_MSG_CLEAR_INPUT:    return "CLEAR-INPUT";
    case DNET_CTERM_MSG_DISCARD:        return "DISCARD";
    default:                            return "?";
    }
}
