/*
 * dnet_nodespec.c - split "NODE\"user pw acct\"::dev:[dir]file" into parts.
 * See dnet_nodespec.h for the contract and the clean-room note.
 */
#include "dnet_nodespec.h"

#include <ctype.h>
#include <string.h>

/* A printable field carries no control bytes -- the access-control strings and
 * the file spec are user text bound for the wire and for RMS; a control byte is
 * a malformed field, refused (INV-6), never passed through. */
static int field_is_clean(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7f)
            return 0;
    return 1;
}

/* Copy [b,e) into dst (capacity cap incl. NUL), rejecting overflow / control
 * bytes. Returns DNET_CTERM_OK, DNET_CTERM_EBADLEN, or DNET_CTERM_EINVAL. */
static int take(char *dst, size_t cap, const char *b, const char *e)
{
    size_t n = (size_t)(e - b);
    if (n + 1 > cap) return DNET_CTERM_EBADLEN;
    if (!field_is_clean(b, n)) return DNET_CTERM_EINVAL;
    memcpy(dst, b, n);
    dst[n] = '\0';
    return DNET_CTERM_OK;
}

/*
 * Split an access-control body (the text between the quotes, "" already
 * unescaped) into up to three whitespace-separated fields:
 *   username [password [account]]
 * A fourth field is malformed (refused). Empty body -> all fields empty.
 */
static int split_access(const char *body, size_t len,
                        struct dnet_nodespec *out)
{
    const char *p = body;
    const char *end = body + len;
    char *dst[3] = { out->username, out->password, out->account };
    size_t cap[3] = { sizeof out->username, sizeof out->password,
                      sizeof out->account };
    int field = 0;

    for (;;) {
        while (p < end && isspace((unsigned char)*p)) p++;   /* skip blanks */
        if (p >= end) break;
        const char *tok = p;
        while (p < end && !isspace((unsigned char)*p)) p++;
        if (field >= 3) return DNET_CTERM_EINVAL;            /* > 3 fields   */
        int rc = take(dst[field], cap[field], tok, p);
        if (rc != DNET_CTERM_OK) return rc;
        field++;
    }
    return DNET_CTERM_OK;
}

int dnet_nodespec_parse(const char *spec, struct dnet_nodespec *out)
{
    if (!spec || !out) return DNET_CTERM_EINVAL;
    memset(out, 0, sizeof *out);

    /* 1. Locate the TOP-LEVEL "::" -- one not inside a quoted access string
     *    (identical rule to copy_spec_has_node / $FILESCAN FSCN$_NODE). */
    int in_quote = 0;
    const char *dcolon = NULL;
    for (const char *p = spec; *p; p++) {
        if (*p == '"') { in_quote = !in_quote; continue; }
        if (!in_quote && p[0] == ':' && p[1] == ':') { dcolon = p; break; }
    }
    if (in_quote) return DNET_CTERM_EBADLEN;      /* unterminated "..."      */
    if (!dcolon)  return DNET_NODESPEC_NONODE;    /* no node -> local path   */

    /* From here every failure jumps to `fail`, which re-zeros *out so no
     * half-parsed field -- above all no partial credential -- ever survives a
     * non-OK return (INV-6, the same guarantee dnet_fal_access_decode makes). */
    int rc;

    /* 2. The file part is everything after "::"; it must be non-empty. */
    const char *fs = dcolon + 2;
    size_t fslen = strlen(fs);
    if (fslen == 0) { rc = DNET_CTERM_EINVAL; goto fail; }    /* "node::"    */
    rc = take(out->filespec, sizeof out->filespec, fs, fs + fslen);
    if (rc != DNET_CTERM_OK) goto fail;

    /* 3. The node part is [spec, dcolon); its node NAME runs up to the first
     *    quote (or to "::" if there is no access string). */
    const char *np = spec;
    const char *npend = dcolon;
    const char *q = NULL;
    for (const char *p = np; p < npend; p++)
        if (*p == '"') { q = p; break; }

    const char *nameend = q ? q : npend;
    if (nameend == np) { rc = DNET_CTERM_EINVAL; goto fail; } /* no node name */
    rc = take(out->node, sizeof out->node, np, nameend);
    if (rc != DNET_CTERM_OK) goto fail;

    /* 4. No access string: done. */
    if (!q) return DNET_CTERM_OK;
    out->has_access = 1;

    /* 5. Unescape the quoted access body ("" -> ") up to its closing quote,
     *    which must appear before "::". */
    char body[3 * (DNET_SC_MAX_STR + 1) + 8];
    size_t bi = 0;
    const char *p = q + 1;
    int closed = 0;
    while (p < npend) {
        if (*p == '"') {
            if (p + 1 < npend && p[1] == '"') {              /* escaped quote */
                if (bi + 1 >= sizeof body) { rc = DNET_CTERM_EBADLEN; goto fail; }
                body[bi++] = '"';
                p += 2;
                continue;
            }
            closed = 1;
            p++;
            break;
        }
        if (bi + 1 >= sizeof body) { rc = DNET_CTERM_EBADLEN; goto fail; }
        body[bi++] = *p++;
    }
    if (!closed) { rc = DNET_CTERM_EBADLEN; goto fail; } /* access not closed */

    /* Only whitespace may follow the closing quote before "::". */
    for (; p < npend; p++)
        if (!isspace((unsigned char)*p)) { rc = DNET_CTERM_EINVAL; goto fail; }

    rc = split_access(body, bi, out);
    if (rc != DNET_CTERM_OK) goto fail;
    return DNET_CTERM_OK;

fail:
    /* Wipe any partially-parsed field (node/filespec/username/...) so a failed
     * parse never hands a half-decoded credential back to the caller. */
    memset(out, 0, sizeof *out);
    return rc;
}

int dnet_copy_plan(const char *src, const char *dst, struct dnet_copy_plan *out)
{
    if (!src || !dst || !out) return DNET_CTERM_EINVAL;
    memset(out, 0, sizeof *out);

    struct dnet_nodespec s, d;
    int rs = dnet_nodespec_parse(src, &s);
    if (rs != DNET_CTERM_OK && rs != DNET_NODESPEC_NONODE) return rs;
    int rd = dnet_nodespec_parse(dst, &d);
    if (rd != DNET_CTERM_OK && rd != DNET_NODESPEC_NONODE) return rd;

    int src_remote = (rs == DNET_CTERM_OK);
    int dst_remote = (rd == DNET_CTERM_OK);
    /* Exactly one side must be remote: a local->local COPY is not this path, and
     * a node->node COPY is not the outbound-client path either. */
    if (src_remote == dst_remote) return DNET_CTERM_EINVAL;

    const struct dnet_nodespec *rem = src_remote ? &s : &d;
    const char *local_arg = src_remote ? dst : src;

    out->is_get = src_remote;                 /* remote source -> GET remote->local */
    out->has_access = rem->has_access;
    /* Same field capacities as dnet_nodespec, so these copies cannot overflow. */
    memcpy(out->node,        rem->node,     sizeof out->node);
    memcpy(out->username,    rem->username, sizeof out->username);
    memcpy(out->password,    rem->password, sizeof out->password);
    memcpy(out->account,     rem->account,  sizeof out->account);
    memcpy(out->remote_spec, rem->filespec, sizeof out->remote_spec);

    /* The local side is taken verbatim (the splitter does not parse a no-node
     * spec's filespec), bounds-checked against the same limit. */
    size_t ll = strlen(local_arg);
    if (ll + 1 > sizeof out->local_spec) return DNET_CTERM_EBADLEN;
    memcpy(out->local_spec, local_arg, ll + 1);
    return DNET_CTERM_OK;
}
