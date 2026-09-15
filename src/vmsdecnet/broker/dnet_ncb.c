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

int dnet_ncb_parse(const char *ncb, size_t len, struct dnet_ncb *out)
{
    if (!ncb || !out)
        return DNET_NCB_EINVAL;

    memset(out, 0, sizeof(*out));

    /* Find the first "::" -- a DECnet node name/address never contains it, so
     * the first one separates node from the object spec. */
    size_t sep = (size_t)-1;
    for (size_t i = 0; i + 1 < len; i++) {
        if (ncb[i] == ':' && ncb[i + 1] == ':') {
            sep = i;
            break;
        }
    }
    if (sep == (size_t)-1)
        return DNET_NCB_ETRUNC;          /* no "::" -- not an NCB */

    int r = copy_bounded(out->node, sizeof out->node, ncb, sep);
    if (r != DNET_NCB_OK)
        return r;                         /* empty or over-long node */

    /* The object spec is everything after "::", with optional surrounding "". */
    const char *rest = ncb + sep + 2;
    size_t restlen = len - (sep + 2);
    if (restlen >= 2 && rest[0] == '"' && rest[restlen - 1] == '"') {
        rest += 1;
        restlen -= 2;
    }
    if (restlen == 0)
        return DNET_NCB_ETRUNC;           /* empty object spec */

    /* TASK=<name> or 0=<name> -> a NAMED task; bare <name> -> named; all-digits
     * -> an object NUMBER. */
    const char *name = NULL;
    size_t namelen = 0;
    if (has_prefix_ci(rest, restlen, "TASK=")) {
        name = rest + 5; namelen = restlen - 5;
    } else if (restlen > 2 && rest[0] == '0' && rest[1] == '=') {
        name = rest + 2; namelen = restlen - 2;
    } else {
        /* all-digits => object number */
        int all_digits = 1;
        for (size_t i = 0; i < restlen; i++) {
            if (!isdigit((unsigned char)rest[i])) { all_digits = 0; break; }
        }
        if (all_digits) {
            unsigned val = 0;
            for (size_t i = 0; i < restlen; i++) {
                if (val > (0xffffffffu - 9) / 10u)   /* overflow guard */
                    return DNET_NCB_EBADLEN;
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
    if (r != DNET_NCB_OK)
        return r;                         /* empty or over-long task name */
    out->is_named = 1;
    return DNET_NCB_OK;
}
