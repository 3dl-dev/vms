/*
 * dnet_ncb.c - parse a DECnet task-to-task Network Connect Block (rd vms-22c,
 *              a1-2). See dnet_ncb.h: a LOCAL API structure, SPEC-DERIVED from
 *              the public DECnet-VAX connect-string form (Rule 8), never a wire
 *              field. Pure, bounds-checked, no allocation.
 */
#include "dnet_ncb.h"

#include <string.h>
#include <ctype.h>

/* Case-insensitive prefix test over a bounded buffer (no NUL assumed). */
static int has_prefix_ci(const char *s, size_t len, const char *pfx)
{
    size_t n = strlen(pfx);
    if (len < n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)s[i]) != toupper((unsigned char)pfx[i]))
            return 0;
    return 1;
}

/* Copy src[0..n-1] into dst (cap includes the NUL). Returns 0 on success,
 * DNET_NCB_EBADLEN if it would not fit, ETRUNC if empty. */
static int copy_bounded(char *dst, size_t cap, const char *src, size_t n)
{
    if (n == 0)
        return DNET_NCB_ETRUNC;
    if (n >= cap)
        return DNET_NCB_EBADLEN;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return DNET_NCB_OK;
}

/* Split the quoted access-control string q[0..n-1] ("user password account",
 * blank-separated, each optional after the first) into *out. */
static int parse_access(const char *q, size_t n, struct dnet_ncb *out)
{
    char *dst[3] = { out->user, out->password, out->account };
    size_t i = 0;
    for (int f = 0; f < 3; f++) {
        while (i < n && q[i] == ' ')
            i++;
        size_t b = i;
        while (i < n && q[i] != ' ')
            i++;
        if (i == b)
            break;                        /* no more fields */
        int r = copy_bounded(dst[f], DNET_NCB_MAXACC + 1, q + b, i - b);
        if (r != DNET_NCB_OK)
            return r;
    }
    while (i < n && q[i] == ' ')
        i++;
    if (i != n)
        return DNET_NCB_EBADLEN;          /* a fourth field: not an access string */
    if (out->user[0] == '\0')
        return DNET_NCB_ETRUNC;           /* "" -- empty access control */
    out->has_access = 1;
    return DNET_NCB_OK;
}

int dnet_ncb_parse(const char *ncb, size_t len, struct dnet_ncb *out)
{
    if (!ncb || !out)
        return DNET_NCB_EINVAL;

    memset(out, 0, sizeof(*out));

    /* Find the first "::" OUTSIDE the access-control quotes -- a password may
     * legitimately contain ':' -- which separates node from the object spec. */
    size_t sep = (size_t)-1;
    int inq = 0;
    for (size_t i = 0; i < len; i++) {
        if (ncb[i] == '"') {
            inq = !inq;
            continue;
        }
        if (!inq && i + 1 < len && ncb[i] == ':' && ncb[i + 1] == ':') {
            sep = i;
            break;
        }
    }
    if (sep == (size_t)-1)
        return DNET_NCB_ETRUNC;          /* no "::" -- not an NCB */

    /* Node part: <node> or <node>"<access control>". */
    size_t nodelen = sep;
    for (size_t i = 0; i < sep; i++) {
        if (ncb[i] == '"') {
            nodelen = i;
            if (sep - i < 2 || ncb[sep - 1] != '"')
                return DNET_NCB_ETRUNC;  /* unterminated access-control quote */
            int r = parse_access(ncb + i + 1, sep - i - 2, out);
            if (r != DNET_NCB_OK) {
                memset(out, 0, sizeof(*out));
                return r;
            }
            break;
        }
    }
    int r = copy_bounded(out->node, sizeof out->node, ncb, nodelen);
    if (r != DNET_NCB_OK) {
        memset(out, 0, sizeof(*out));
        return r;                         /* empty or over-long node */
    }

    /* The object spec is everything after "::", with optional surrounding "". */
    const char *rest = ncb + sep + 2;
    size_t restlen = len - (sep + 2);
    if (restlen >= 2 && rest[0] == '"' && rest[restlen - 1] == '"') {
        rest += 1;
        restlen -= 2;
    }
    /* Ignore an optional "/<connect data>" tail. */
    for (size_t i = 0; i < restlen; i++) {
        if (rest[i] == '/') {
            restlen = i;
            break;
        }
    }
    if (restlen == 0) {
        memset(out, 0, sizeof(*out));
        return DNET_NCB_ETRUNC;           /* empty object spec */
    }

    /* TASK=<name> or 0=<name> -> a NAMED task; bare <name> -> named; all-digits
     * (optionally followed by "=") -> an object NUMBER. */
    const char *name = NULL;
    size_t namelen = 0;
    if (has_prefix_ci(rest, restlen, "TASK=")) {
        name = rest + 5; namelen = restlen - 5;
    } else if (restlen > 2 && rest[0] == '0' && rest[1] == '=') {
        name = rest + 2; namelen = restlen - 2;
    } else {
        size_t digits = 0;
        while (digits < restlen && isdigit((unsigned char)rest[digits]))
            digits++;
        if (digits > 0 && (digits == restlen ||
                           (digits + 1 == restlen && rest[digits] == '='))) {
            unsigned val = 0;
            for (size_t i = 0; i < digits; i++) {
                if (val > (0xffffffffu - 9) / 10u) {   /* overflow guard */
                    memset(out, 0, sizeof(*out));
                    return DNET_NCB_EBADLEN;
                }
                val = val * 10u + (unsigned)(rest[i] - '0');
            }
            out->is_named = 0;
            out->object = val;
            return DNET_NCB_OK;
        }
        /* a bare named object */
        name = rest; namelen = restlen;
    }

    r = copy_bounded(out->task, sizeof out->task, name, namelen);
    if (r != DNET_NCB_OK) {
        memset(out, 0, sizeof(*out));
        return r;                         /* empty or over-long task name */
    }
    out->is_named = 1;
    return DNET_NCB_OK;
}

/* ---- the inbound-accept NCB (rd vms-0a7 S3; OVMX's own form, see .h) ---- */

/* A node-name byte: printable, and none of the bytes the form uses as
 * delimiters (quote, colon, blank). */
static int acc_node_ch(char c)
{
    return c > ' ' && c < 0x7f && c != '"' && c != ':';
}

/* A user-name byte: as a node byte, and never the '/' that ends it. */
static int acc_user_ch(char c)
{
    return acc_node_ch(c) && c != '/';
}

static int acc_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int dnet_ncb_accept_parse(const char *ncb, size_t len, struct dnet_ncb_accept *out)
{
    if (!ncb || !out)
        return DNET_NCB_EINVAL;
    memset(out, 0, sizeof(*out));
    int r = DNET_NCB_ENOTACC;

    /* <node>:: -- every byte up to the first ':' is a node byte. */
    size_t i = 0;
    while (i < len && acc_node_ch(ncb[i]))
        i++;
    if (i == 0 || i + 1 >= len || ncb[i] != ':' || ncb[i + 1] != ':')
        goto bad;
    if (i > DNET_NCB_MAXNODE) { r = DNET_NCB_EBADLEN; goto bad; }
    memcpy(out->node, ncb, i);
    size_t p = i + 2;

    /* "...": the quoted body is the rest of the NCB, exactly. */
    if (p >= len || ncb[p] != '"' || len - p < 2 || ncb[len - 1] != '"')
        goto bad;
    size_t b = p + 1, e = len - 1;      /* body = ncb[b..e-1] */

    /* <object-number>= : 1..3 digits, 1..255, no leading zero (one spelling
     * per link: what dnet_ncb_accept_format writes). */
    unsigned obj = 0;
    size_t d = 0;
    if (b < e && ncb[b] == '0')
        goto bad;
    while (b + d < e && ncb[b + d] >= '0' && ncb[b + d] <= '9') {
        if (++d > 3) goto bad;
        obj = obj * 10u + (unsigned)(ncb[b + d - 1] - '0');
    }
    if (d == 0 || obj == 0 || obj > DNET_NCB_ACC_MAXOBJ || b + d >= e || ncb[b + d] != '=')
        goto bad;
    b += d + 1;

    /* <user>/ */
    size_t u = 0;
    while (b + u < e && acc_user_ch(ncb[b + u]))
        u++;
    if (u == 0 || b + u >= e || ncb[b + u] != '/')
        goto bad;
    if (u > DNET_NCB_MAXACC) { r = DNET_NCB_EBADLEN; goto bad; }
    memcpy(out->user, ncb + b, u);
    b += u + 1;

    /* #<8 hex digits>, then the closing quote and nothing else. */
    if (e - b != 9 || ncb[b] != '#')
        goto bad;
    uint32_t h = 0;
    for (size_t k = 1; k <= 8; k++) {
        int v = acc_hexval(ncb[b + k]);
        if (v < 0) goto bad;
        h = (h << 4) | (uint32_t)v;
    }
    if (h == 0)
        goto bad;                         /* 0 is never a link handle */
    out->object = obj;
    out->handle = h;
    return DNET_NCB_OK;
bad:
    memset(out, 0, sizeof(*out));
    return r;
}

int dnet_ncb_accept_format(const char *node, unsigned object, const char *user,
                           uint32_t handle, char *buf, size_t cap, size_t *outlen)
{
    static const char hx[] = "0123456789ABCDEF";
    if (!node || !user || !buf || !outlen)
        return DNET_NCB_EINVAL;
    *outlen = 0;
    size_t nl = strlen(node), ul = strlen(user);
    if (nl == 0 || ul == 0 || object == 0 || object > DNET_NCB_ACC_MAXOBJ || handle == 0)
        return DNET_NCB_ENOTACC;
    if (nl > DNET_NCB_MAXNODE || ul > DNET_NCB_MAXACC)
        return DNET_NCB_EBADLEN;
    for (size_t i = 0; i < nl; i++)
        if (!acc_node_ch(node[i])) return DNET_NCB_ENOTACC;
    for (size_t i = 0; i < ul; i++)
        if (!acc_user_ch(user[i])) return DNET_NCB_ENOTACC;
    char num[4];
    size_t dl = 0;
    if (object >= 100) num[dl++] = (char)('0' + object / 100);
    if (object >= 10)  num[dl++] = (char)('0' + (object / 10) % 10);
    num[dl++] = (char)('0' + object % 10);
    /* node :: " num = user / # 8hex " NUL */
    size_t need = nl + 2 + 1 + dl + 1 + ul + 1 + 1 + 8 + 1;
    if (need + 1 > cap)
        return DNET_NCB_EBADLEN;
    size_t o = 0;
    memcpy(buf + o, node, nl); o += nl;
    buf[o++] = ':'; buf[o++] = ':'; buf[o++] = '"';
    memcpy(buf + o, num, dl); o += dl;
    buf[o++] = '=';
    memcpy(buf + o, user, ul); o += ul;
    buf[o++] = '/'; buf[o++] = '#';
    for (int k = 7; k >= 0; k--)
        buf[o++] = hx[(handle >> (4 * k)) & 0xFu];
    buf[o++] = '"';
    buf[o] = '\0';
    *outlen = o;
    return DNET_NCB_OK;
}
