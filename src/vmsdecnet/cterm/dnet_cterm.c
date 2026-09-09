/*
 * dnet_cterm.c - DECnet Phase IV CTERM (Command Terminal) protocol: the
 *                terminal-service layered product behind $ SET HOST.
 *                See dnet_cterm.h for the full clean-room provenance (Rule 8):
 *                CTERM is ENTIRELY SPEC-DERIVED -- no oracle specimen exists
 *                (the vms-3be capture never completed a logical link), so the
 *                message-type codes and body layouts here are OVMX-assigned from
 *                the public DNA CTERM functional description and proven only by
 *                round-trip, never presented as oracle-verified bytes.
 *
 * On-wire CTERM PDU layouts (little-endian scalars; counted strings are a
 * 1-byte length followed by that many bytes). Every PDU begins with the 1-byte
 * message type (enum dnet_cterm_msgtype).
 *
 *   Bind (1):            type, ver_v, ver_eco, ver_user, mode, name[counted]
 *   Bind Accept (2):     type, ver_v, ver_eco, ver_user, status, name[counted]
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
    case DNET_CTERM_MSG_BIND:
        if (off + 4 > cap) return DNET_CTERM_ENOSPACE;
        buf[off++] = msg->ver_v;
        buf[off++] = msg->ver_eco;
        buf[off++] = msg->ver_user;
        buf[off++] = msg->mode;
        r = put_string(buf, cap, off, msg->name);
        if (r < 0) return DNET_CTERM_ENOSPACE;
        off += (size_t)r;
        break;

    case DNET_CTERM_MSG_BIND_ACCEPT:
        if (off + 4 > cap) return DNET_CTERM_ENOSPACE;
        buf[off++] = msg->ver_v;
        buf[off++] = msg->ver_eco;
        buf[off++] = msg->ver_user;
        buf[off++] = msg->status;
        r = put_string(buf, cap, off, msg->name);
        if (r < 0) return DNET_CTERM_ENOSPACE;
        off += (size_t)r;
        break;

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
    case DNET_CTERM_MSG_BIND:
        if (off + 4 > len) return DNET_CTERM_ETRUNC;
        out->ver_v    = buf[off++];
        out->ver_eco  = buf[off++];
        out->ver_user = buf[off++];
        out->mode     = buf[off++];
        r = get_string(buf, len, off, out->name, sizeof(out->name));
        if (r < 0) return DNET_CTERM_ETRUNC;
        off += (size_t)r;
        break;

    case DNET_CTERM_MSG_BIND_ACCEPT:
        if (off + 4 > len) return DNET_CTERM_ETRUNC;
        out->ver_v    = buf[off++];
        out->ver_eco  = buf[off++];
        out->ver_user = buf[off++];
        out->status   = buf[off++];
        r = get_string(buf, len, off, out->name, sizeof(out->name));
        if (r < 0) return DNET_CTERM_ETRUNC;
        off += (size_t)r;
        break;

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

    /* MENUVER + the three access-control strings. A plain SET HOST sends them
     * empty, exactly as the specimen does. */
    if (off + 1 > cap) return DNET_CTERM_ENOSPACE;
    buf[off++] = SC_MENUVER_OBSERVED;

    r = put_string(buf, cap, off, username); if (r < 0) return DNET_CTERM_ENOSPACE; off += (size_t)r;
    r = put_string(buf, cap, off, password); if (r < 0) return DNET_CTERM_ENOSPACE; off += (size_t)r;
    r = put_string(buf, cap, off, account);  if (r < 0) return DNET_CTERM_ENOSPACE; off += (size_t)r;

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

    struct dnet_cterm_msg m;
    memset(&m, 0, sizeof(m));
    m.type     = DNET_CTERM_MSG_BIND;
    m.ver_v    = DNET_CTERM_VER_V;
    m.ver_eco  = DNET_CTERM_VER_ECO;
    m.ver_user = DNET_CTERM_VER_USER;
    m.mode     = DNET_CTERM_MODE_COMMAND;
    if (term_name) {
        strncpy(m.name, term_name, sizeof(m.name) - 1);
        m.name[sizeof(m.name) - 1] = '\0';
    }
    int rc = dnet_cterm_encode(&m, out, cap, outlen);
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
    /* HOST accepts only after it has seen the inbound Bind (last.type == BIND). */
    if (s->role != DNET_CTERM_ROLE_HOST || s->state != DNET_CTERM_S_CLOSED ||
        s->last.type != DNET_CTERM_MSG_BIND)
        return DNET_CTERM_ESTATE;

    struct dnet_cterm_msg m;
    memset(&m, 0, sizeof(m));
    m.type     = DNET_CTERM_MSG_BIND_ACCEPT;
    m.ver_v    = DNET_CTERM_VER_V;
    m.ver_eco  = DNET_CTERM_VER_ECO;
    m.ver_user = DNET_CTERM_VER_USER;
    m.status   = 0;   /* accepted */
    if (host_name) {
        strncpy(m.name, host_name, sizeof(m.name) - 1);
        m.name[sizeof(m.name) - 1] = '\0';
    }
    int rc = dnet_cterm_encode(&m, out, cap, outlen);
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

    struct dnet_cterm_msg m;
    int rc = dnet_cterm_decode(buf, len, &m, NULL);
    if (rc != DNET_CTERM_OK)
        return rc;

    enum dnet_cterm_event ev = DNET_CTERM_EV_NONE;

    switch (m.type) {
    case DNET_CTERM_MSG_BIND:
        /* Only the HOST accepts an inbound Bind, and only while CLOSED. */
        if (s->role == DNET_CTERM_ROLE_HOST && s->state == DNET_CTERM_S_CLOSED) {
            s->last = m;   /* record so bind_accept() can validate + reply */
            strncpy(s->peer_name, m.name, sizeof(s->peer_name) - 1);
            s->peer_name[sizeof(s->peer_name) - 1] = '\0';
            ev = DNET_CTERM_EV_BIND_IND;
        }
        break;

    case DNET_CTERM_MSG_BIND_ACCEPT:
        /* Only the TERMINAL that sent a Bind accepts a Bind Accept. */
        if (s->role == DNET_CTERM_ROLE_TERMINAL && s->state == DNET_CTERM_S_BINDING) {
            s->last = m;
            strncpy(s->peer_name, m.name, sizeof(s->peer_name) - 1);
            s->peer_name[sizeof(s->peer_name) - 1] = '\0';
            s->state = DNET_CTERM_S_BOUND;
            ev = DNET_CTERM_EV_BOUND;
        }
        break;

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
    case DNET_CTERM_MSG_BIND:           return "BIND";
    case DNET_CTERM_MSG_BIND_ACCEPT:    return "BIND-ACCEPT";
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
