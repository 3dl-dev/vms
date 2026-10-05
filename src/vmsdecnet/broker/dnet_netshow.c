/*
 * dnet_netshow.c - NETACP volatile-database snapshot record + NCP-layout
 *                  formatters (rd vms-30e). See dnet_netshow.h for the design,
 *                  the honest-scope rule and the bounds discipline.
 *
 * Pure logic: no socket, no mailbox, no allocation. The layouts below are copied
 * from observed real OpenVMS VAX V7.3 output (docs/oracle/vax-ncp-show/, Rule 8);
 * where OVMX prints something the oracle does not show (a populated KNOWN LINKS
 * table), that is said at the spot.
 */
#ifndef DNET_NETSHOW_H
#include "dnet_netshow.h"
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------ LE helpers ------------------------------ */
static void ns_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}
static void ns_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}
static uint16_t ns_get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t ns_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A counted string in a fixed slot: len byte, then `slot` bytes (zero-padded). */
static int ns_put_str(uint8_t *p, const char *s, size_t slot)
{
    size_t n = 0;
    while (n <= slot && s[n])       /* bounded: never reads past slot + 1 bytes */
        n++;
    if (n > slot)
        return DNET_NETSHOW_EBADLEN;
    p[0] = (uint8_t)n;
    memset(p + 1, 0, slot);
    memcpy(p + 1, s, n);
    return DNET_NETSHOW_OK;
}
/* Decode a counted string: bounded by its slot, printable ASCII only. */
static int ns_get_str(const uint8_t *p, char *out, size_t slot)
{
    size_t n = p[0];
    if (n > slot)
        return DNET_NETSHOW_EBADLEN;
    for (size_t i = 0; i < n; i++) {
        if (p[1 + i] < 0x20 || p[1 + i] > 0x7e)
            return DNET_NETSHOW_EINVAL;   /* no control bytes reach a terminal */
        out[i] = (char)p[1 + i];
    }
    out[n] = '\0';
    return DNET_NETSHOW_OK;
}

static int ns_entity_ok(uint8_t e)
{
    return e == DNET_NETSHOW_ENT_EXECUTOR || e == DNET_NETSHOW_ENT_NODES ||
           e == DNET_NETSHOW_ENT_LINKS;
}

/* ------------------------------ request ------------------------------ */
DNET_NETSHOW_API int dnet_netshow_req_encode(const struct dnet_netshow_req *q,
                                             uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!q || !buf || !ns_entity_ok(q->entity))
        return DNET_NETSHOW_EINVAL;
    if (cap < DNET_NETSHOW_REQ_LEN)
        return DNET_NETSHOW_ENOSPACE;
    buf[0] = q->version;
    buf[1] = q->entity;
    ns_put16(buf + 2, q->cursor);
    if (outlen)
        *outlen = DNET_NETSHOW_REQ_LEN;
    return DNET_NETSHOW_OK;
}

DNET_NETSHOW_API int dnet_netshow_req_decode(const uint8_t *buf, size_t len,
                                             struct dnet_netshow_req *out)
{
    if (!buf || !out)
        return DNET_NETSHOW_EINVAL;
    memset(out, 0, sizeof *out);
    if (len < DNET_NETSHOW_REQ_LEN)
        return DNET_NETSHOW_ETRUNC;
    if (len > DNET_NETSHOW_REQ_LEN)
        return DNET_NETSHOW_EBADLEN;
    if (buf[0] != DNET_NETSHOW_VERSION)
        return DNET_NETSHOW_EVERS;
    if (!ns_entity_ok(buf[1]))
        return DNET_NETSHOW_EINVAL;
    out->version = buf[0];
    out->entity  = buf[1];
    out->cursor  = ns_get16(buf + 2);
    return DNET_NETSHOW_OK;
}

/* ------------------------------ response ------------------------------ */
static size_t ns_entry_len(uint8_t entity)
{
    return entity == DNET_NETSHOW_ENT_NODES ? DNET_NETSHOW_NODE_LEN
         : entity == DNET_NETSHOW_ENT_LINKS ? DNET_NETSHOW_LINK_LEN : 0;
}
static unsigned ns_entry_max(uint8_t entity)
{
    return entity == DNET_NETSHOW_ENT_NODES ? DNET_NETSHOW_NODES_PER
         : entity == DNET_NETSHOW_ENT_LINKS ? DNET_NETSHOW_LINKS_PER : 0;
}

DNET_NETSHOW_API int dnet_netshow_rsp_encode(const struct dnet_netshow_rsp *r,
                                             uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!r || !buf || !ns_entity_ok(r->entity))
        return DNET_NETSHOW_EINVAL;
    if (r->count > ns_entry_max(r->entity) ||
        (uint32_t)r->first + r->count > r->total ||
        (r->entity == DNET_NETSHOW_ENT_EXECUTOR && (r->total || r->first)))
        return DNET_NETSHOW_EBADLEN;
    size_t total = DNET_NETSHOW_HDR_LEN + DNET_NETSHOW_EXEC_LEN +
                   (size_t)r->count * ns_entry_len(r->entity);
    if (cap < total)
        return DNET_NETSHOW_ENOSPACE;

    uint8_t *p = buf;
    p[0] = DNET_NETSHOW_VERSION;
    p[1] = r->entity;
    ns_put16(p + 2, r->total);
    ns_put16(p + 4, r->first);
    p[6] = r->count;
    p[7] = 0;
    ns_put32(p + 8, r->as_of);
    p += DNET_NETSHOW_HDR_LEN;

    const struct dnet_netshow_exec *x = &r->exec;
    ns_put16(p, x->addr);                                      p += 2;
    p[0] = x->state_on ? 1 : 0;                                p += 1;
    p[0] = x->type;                                            p += 1;
    if (ns_put_str(p, x->name, DNET_NETSHOW_NAMEMAX)) return DNET_NETSHOW_EBADLEN;
    p += 1 + DNET_NETSHOW_NAMEMAX;
    if (ns_put_str(p, x->ident, DNET_NETSHOW_IDENTMAX)) return DNET_NETSHOW_EBADLEN;
    p += 1 + DNET_NETSHOW_IDENTMAX;
    if (ns_put_str(p, x->circuit, DNET_NETSHOW_CIRCMAX)) return DNET_NETSHOW_EBADLEN;
    p += 1 + DNET_NETSHOW_CIRCMAX;
    memcpy(p, x->nsp_ver, 3);                                  p += 3;
    memcpy(p, x->rtg_ver, 3);                                  p += 3;
    ns_put16(p, x->max_links);                                 p += 2;
    ns_put16(p, x->max_links_active);                          p += 2;
    ns_put16(p, x->active_links);                              p += 2;
    p[0] = x->have_dr ? 1 : 0;                                 p += 1;
    ns_put16(p, x->dr_addr);                                   p += 2;

    for (unsigned i = 0; i < r->count; i++) {
        if (r->entity == DNET_NETSHOW_ENT_NODES) {
            const struct dnet_netshow_node *n = &r->u.node[i];
            ns_put16(p, n->addr);                              p += 2;
            if (ns_put_str(p, n->name, DNET_NETSHOW_NAMEMAX)) return DNET_NETSHOW_EBADLEN;
            p += 1 + DNET_NETSHOW_NAMEMAX;
            p[0] = n->flags;                                   p += 1;
            p[0] = n->adj_state;                               p += 1;
            ns_put16(p, n->active_links);                      p += 2;
            ns_put16(p, n->next_node);                         p += 2;
        } else {
            const struct dnet_netshow_link *l = &r->u.link[i];
            ns_put16(p, l->local_link);                        p += 2;
            ns_put16(p, l->remote_link);                       p += 2;
            ns_put16(p, l->node);                              p += 2;
            if (ns_put_str(p, l->name, DNET_NETSHOW_NAMEMAX)) return DNET_NETSHOW_EBADLEN;
            p += 1 + DNET_NETSHOW_NAMEMAX;
            ns_put32(p, l->pid);                               p += 4;
            p[0] = l->object;                                  p += 1;
            p[0] = l->outbound ? 1 : 0;                        p += 1;
            p[0] = l->running ? 1 : 0;                         p += 1;
        }
    }
    if (outlen)
        *outlen = (size_t)(p - buf);
    return DNET_NETSHOW_OK;
}

DNET_NETSHOW_API int dnet_netshow_rsp_decode(const uint8_t *buf, size_t len,
                                             struct dnet_netshow_rsp *out)
{
    if (!buf || !out)
        return DNET_NETSHOW_EINVAL;
    memset(out, 0, sizeof *out);            /* nothing half-filled on refusal */

    if (len < DNET_NETSHOW_HDR_LEN + DNET_NETSHOW_EXEC_LEN)
        return DNET_NETSHOW_ETRUNC;
    if (buf[0] != DNET_NETSHOW_VERSION)
        return DNET_NETSHOW_EVERS;
    uint8_t entity = buf[1];
    if (!ns_entity_ok(entity))
        return DNET_NETSHOW_EINVAL;
    uint16_t total = ns_get16(buf + 2);
    uint16_t first = ns_get16(buf + 4);
    uint8_t  count = buf[6];
    if (count > ns_entry_max(entity) || (uint32_t)first + count > total ||
        (entity == DNET_NETSHOW_ENT_EXECUTOR && (total || first)))
        return DNET_NETSHOW_EBADLEN;
    size_t need = DNET_NETSHOW_HDR_LEN + DNET_NETSHOW_EXEC_LEN +
                  (size_t)count * ns_entry_len(entity);
    if (len < need)
        return DNET_NETSHOW_ETRUNC;          /* never read past buf[len-1] */
    if (len > need)
        return DNET_NETSHOW_EBADLEN;         /* trailing bytes: not this record */

    struct dnet_netshow_rsp r;
    memset(&r, 0, sizeof r);
    r.version = buf[0];
    r.entity  = entity;
    r.total   = total;
    r.first   = first;
    r.count   = count;
    r.as_of   = ns_get32(buf + 8);
    const uint8_t *p = buf + DNET_NETSHOW_HDR_LEN;

    struct dnet_netshow_exec *x = &r.exec;
    int rc;
    x->addr = ns_get16(p);                                     p += 2;
    x->state_on = p[0] ? 1 : 0;                                p += 1;
    x->type = p[0];                                            p += 1;
    if (x->type > DNET_NETSHOW_TYPE_ROUTING)
        return DNET_NETSHOW_EINVAL;
    if ((rc = ns_get_str(p, x->name, DNET_NETSHOW_NAMEMAX)) != 0) return rc;
    p += 1 + DNET_NETSHOW_NAMEMAX;
    if ((rc = ns_get_str(p, x->ident, DNET_NETSHOW_IDENTMAX)) != 0) return rc;
    p += 1 + DNET_NETSHOW_IDENTMAX;
    if ((rc = ns_get_str(p, x->circuit, DNET_NETSHOW_CIRCMAX)) != 0) return rc;
    p += 1 + DNET_NETSHOW_CIRCMAX;
    memcpy(x->nsp_ver, p, 3);                                  p += 3;
    memcpy(x->rtg_ver, p, 3);                                  p += 3;
    x->max_links = ns_get16(p);                                p += 2;
    x->max_links_active = ns_get16(p);                         p += 2;
    x->active_links = ns_get16(p);                             p += 2;
    x->have_dr = p[0] ? 1 : 0;                                 p += 1;
    x->dr_addr = ns_get16(p);                                  p += 2;

    for (unsigned i = 0; i < count; i++) {
        if (entity == DNET_NETSHOW_ENT_NODES) {
            struct dnet_netshow_node *n = &r.u.node[i];
            n->addr = ns_get16(p);                             p += 2;
            if ((rc = ns_get_str(p, n->name, DNET_NETSHOW_NAMEMAX)) != 0) return rc;
            p += 1 + DNET_NETSHOW_NAMEMAX;
            n->flags = p[0];                                   p += 1;
            n->adj_state = p[0];                               p += 1;
            if (n->adj_state > DNET_NETSHOW_ADJ_UP)
                return DNET_NETSHOW_EINVAL;
            n->active_links = ns_get16(p);                     p += 2;
            n->next_node = ns_get16(p);                        p += 2;
        } else {
            struct dnet_netshow_link *l = &r.u.link[i];
            l->local_link = ns_get16(p);                       p += 2;
            l->remote_link = ns_get16(p);                      p += 2;
            l->node = ns_get16(p);                             p += 2;
            if ((rc = ns_get_str(p, l->name, DNET_NETSHOW_NAMEMAX)) != 0) return rc;
            p += 1 + DNET_NETSHOW_NAMEMAX;
            l->pid = ns_get32(p);                              p += 4;
            l->object = p[0];                                  p += 1;
            l->outbound = p[0] ? 1 : 0;                        p += 1;
            l->running = p[0] ? 1 : 0;                         p += 1;
        }
    }
    *out = r;
    return DNET_NETSHOW_OK;
}

/* ------------------------------ the client ------------------------------ */
DNET_NETSHOW_API uint32_t dnet_netshow_fetch(dnet_netshow_query_fn query, void *ctx,
                                             uint8_t entity, struct dnet_netshow_view *v)
{
    static struct dnet_netshow_rsp r;       /* ~1.5 KB: keep it off small stacks */
    uint8_t req[DNET_NETSHOW_REQ_LEN];
    uint8_t rsp[DNET_NETSHOW_RSP_MAX + 16];
    if (!query || !v || !ns_entity_ok(entity))
        return DNET_NETSHOW_ST_BADREC;
    memset(v, 0, sizeof *v);

    uint16_t cursor = 0;
    for (unsigned guard = 0; guard < 1024; guard++) {
        struct dnet_netshow_req q = { DNET_NETSHOW_VERSION, entity, cursor };
        size_t qn = 0, rn = 0;
        if (dnet_netshow_req_encode(&q, req, sizeof req, &qn) != DNET_NETSHOW_OK)
            return DNET_NETSHOW_ST_BADREC;
        uint32_t st = query(ctx, req, qn, rsp, sizeof rsp, &rn);
        if (!(st & 1))
            return st;
        if (dnet_netshow_rsp_decode(rsp, rn, &r) != DNET_NETSHOW_OK ||
            r.entity != entity || r.first != cursor)
            return DNET_NETSHOW_ST_BADREC;
        if (cursor == 0) {
            v->as_of = r.as_of;
            v->exec = r.exec;
        }
        if (entity == DNET_NETSHOW_ENT_EXECUTOR)
            return 1;
        for (unsigned i = 0; i < r.count; i++) {
            if (entity == DNET_NETSHOW_ENT_NODES) {
                if (v->nnodes >= DNET_NETSHOW_MAX_NODES)
                    return DNET_NETSHOW_ST_BADREC;
                v->nodes[v->nnodes++] = r.u.node[i];
            } else {
                if (v->nlinks >= DNET_NETSHOW_MAX_LINKS)
                    return DNET_NETSHOW_ST_BADREC;
                v->links[v->nlinks++] = r.u.link[i];
            }
        }
        cursor = (uint16_t)(cursor + r.count);
        if (cursor >= r.total)
            return 1;
        if (r.count == 0)
            return DNET_NETSHOW_ST_BADREC;    /* a page that does not advance */
    }
    return DNET_NETSHOW_ST_BADREC;
}

/* ------------------------------ formatters ------------------------------ */
DNET_NETSHOW_API void dnet_netshow_addr(uint16_t a, char *buf, size_t cap)
{
    snprintf(buf, cap, "%u.%u", (unsigned)((a >> 10) & 0x3f), (unsigned)(a & 0x3ff));
}

DNET_NETSHOW_API void dnet_netshow_asctime(uint32_t t, char *buf, size_t cap)
{
    static const char *const mon[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                         "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    time_t tt = (time_t)t;
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    if (!localtime_r(&tt, &tmv)) {
        snprintf(buf, cap, "?");
        return;
    }
    snprintf(buf, cap, "%2d-%s-%04d %02d:%02d:%02d", tmv.tm_mday,
             mon[(tmv.tm_mon >= 0 && tmv.tm_mon < 12) ? tmv.tm_mon : 0],
             tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* "1.2 (VAX2)" / "1.2". */
static void ns_node_id(uint16_t a, const char *name, char *buf, size_t cap)
{
    char as[12];
    dnet_netshow_addr(a, as, sizeof as);
    if (name && name[0])
        snprintf(buf, cap, "%s (%s)", as, name);
    else
        snprintf(buf, cap, "%s", as);
}

static void ns_header(const struct dnet_netshow_view *v, const char *title,
                      dnet_netshow_emit_fn emit, void *ctx)
{
    char t[24], line[96];
    dnet_netshow_asctime(v->as_of, t, sizeof t);
    emit(ctx, "");
    emit(ctx, "");
    snprintf(line, sizeof line, "%s as of %s", title, t);
    emit(ctx, line);
    emit(ctx, "");
}

static void ns_executor_line(const struct dnet_netshow_exec *x,
                             dnet_netshow_emit_fn emit, void *ctx)
{
    char id[24], line[64];
    ns_node_id(x->addr, x->name, id, sizeof id);
    snprintf(line, sizeof line, "Executor node = %s", id);
    emit(ctx, line);
    emit(ctx, "");
}

/* "Name                     = value" -- the value column is 25 (oracle). */
static void ns_param(const char *name, const char *val, dnet_netshow_emit_fn emit, void *ctx)
{
    char line[96];
    snprintf(line, sizeof line, "%-24s = %s", name, val);
    emit(ctx, line);
}

static void ns_state_ident(const struct dnet_netshow_exec *x,
                           dnet_netshow_emit_fn emit, void *ctx)
{
    ns_param("State", x->state_on ? "on" : "off", emit, ctx);
    if (x->ident[0])
        ns_param("Identification", x->ident, emit, ctx);
}

DNET_NETSHOW_API void dnet_netshow_fmt_executor(const struct dnet_netshow_view *v, int kind,
                                                dnet_netshow_emit_fn emit, void *ctx)
{
    const struct dnet_netshow_exec *x = &v->exec;
    char val[32];
    if (kind == DNET_NETSHOW_EXEC_CHAR) {
        ns_header(v, "Node Volatile Characteristics", emit, ctx);
        ns_executor_line(x, emit, ctx);
        if (x->ident[0])
            ns_param("Identification", x->ident, emit, ctx);
        snprintf(val, sizeof val, "V%u.%u.%u", x->nsp_ver[0], x->nsp_ver[1], x->nsp_ver[2]);
        ns_param("NSP version", val, emit, ctx);
        snprintf(val, sizeof val, "%u", (unsigned)x->max_links);
        ns_param("Maximum links", val, emit, ctx);
        snprintf(val, sizeof val, "V%u.%u.%u", x->rtg_ver[0], x->rtg_ver[1], x->rtg_ver[2]);
        ns_param("Routing version", val, emit, ctx);
        ns_param("Type", x->type == DNET_NETSHOW_TYPE_ROUTING ? "routing IV"
                                                              : "nonrouting IV", emit, ctx);
    } else if (kind == DNET_NETSHOW_EXEC_COUNTERS) {
        char line[64];
        ns_header(v, "Node Counters", emit, ctx);
        ns_executor_line(x, emit, ctx);
        snprintf(line, sizeof line, "%12u  Maximum logical links active",
                 (unsigned)x->max_links_active);
        emit(ctx, line);
    } else {
        ns_header(v, "Node Volatile Summary", emit, ctx);
        ns_executor_line(x, emit, ctx);
        ns_state_ident(x, emit, ctx);
    }
    emit(ctx, "");
    emit(ctx, "");
}

/* The KNOWN NODES table: column starts measured on the oracle header
 * "    Node           State      Active  Delay   Circuit     Next node"
 * (Node 4, State 19, Active/Links 30, Delay 38, Circuit 46) and its row
 * " 1.2 (VAX2)                                   QNA-0          0"
 * (node id at 1, circuit at 46, next node at 61 = the circuit in a 15-wide
 * field). Delay is not kept by NETACP and is left blank. */
static void ns_node_table_head(dnet_netshow_emit_fn emit, void *ctx)
{
    emit(ctx, "    Node           State      Active  Delay   Circuit     Next node");
    emit(ctx, "                              Links");
    emit(ctx, "");
}

static void ns_place(char *row, size_t rowcap, size_t col, const char *s)
{
    size_t n = strlen(s);
    if (col >= rowcap - 1)
        return;
    if (col + n > rowcap - 1)
        n = rowcap - 1 - col;
    memcpy(row + col, s, n);
}

static void ns_node_row(const struct dnet_netshow_view *v, const struct dnet_netshow_node *n,
                        dnet_netshow_emit_fn emit, void *ctx)
{
    char row[96], id[24], tmp[24];
    memset(row, ' ', sizeof row - 1);
    row[sizeof row - 1] = '\0';
    ns_node_id(n->addr, n->name, id, sizeof id);
    ns_place(row, sizeof row, 1, id);
    /* State: a ROUTING executor knows reachability from its adjacencies; a
     * nonrouting one keeps no per-node state (the oracle endnode prints none). */
    if (v->exec.type == DNET_NETSHOW_TYPE_ROUTING)
        ns_place(row, sizeof row, 19,
                 n->adj_state == DNET_NETSHOW_ADJ_UP ? "reachable" : "unreachable");
    if (n->active_links) {
        snprintf(tmp, sizeof tmp, "%5u", (unsigned)n->active_links);
        ns_place(row, sizeof row, 30, tmp);
    }
    if (v->exec.circuit[0]) {
        ns_place(row, sizeof row, 46, v->exec.circuit);
        if (n->next_node)
            dnet_netshow_addr(n->next_node, tmp, sizeof tmp);
        else
            snprintf(tmp, sizeof tmp, "0");
        ns_place(row, sizeof row, 61, tmp);
    }
    size_t end = sizeof row - 1;
    while (end > 0 && row[end - 1] == ' ')
        end--;
    row[end] = '\0';
    emit(ctx, row);
}

DNET_NETSHOW_API void dnet_netshow_fmt_known_nodes(const struct dnet_netshow_view *v,
                                                   dnet_netshow_emit_fn emit, void *ctx)
{
    ns_header(v, "Known Node Volatile Summary", emit, ctx);
    ns_executor_line(&v->exec, emit, ctx);
    ns_state_ident(&v->exec, emit, ctx);
    emit(ctx, "");
    emit(ctx, "");
    if (v->nnodes == 0)
        return;
    ns_node_table_head(emit, ctx);
    for (unsigned i = 0; i < v->nnodes; i++)
        ns_node_row(v, &v->nodes[i], emit, ctx);
}

DNET_NETSHOW_API void dnet_netshow_fmt_node(const struct dnet_netshow_view *v,
                                            const struct dnet_netshow_node *n,
                                            dnet_netshow_emit_fn emit, void *ctx)
{
    if (!n) {
        dnet_netshow_fmt_executor(v, DNET_NETSHOW_EXEC_SUMMARY, emit, ctx);
        return;
    }
    ns_header(v, "Node Volatile Summary", emit, ctx);
    ns_node_table_head(emit, ctx);
    ns_node_row(v, n, emit, ctx);
}

/* KNOWN LINKS. The empty form ("No information in database") is the oracle's.
 * The POPULATED table is not in the oracle capture (the reference node had no
 * links): its columns -- Link, Node, PID, Remote link, Remote user -- follow
 * the public NCP manual's SHOW KNOWN LINKS fields; the exact column positions
 * are OVMX's until a capture with a live link pins them. The process-name
 * column is not printed (NETACP's record carries the PID only). */
DNET_NETSHOW_API void dnet_netshow_fmt_known_links(const struct dnet_netshow_view *v,
                                                   dnet_netshow_emit_fn emit, void *ctx)
{
    ns_header(v, "Known Link Volatile Summary", emit, ctx);
    if (v->nlinks == 0) {
        emit(ctx, "No information in database");
        emit(ctx, "");
        return;
    }
    emit(ctx, "    Link         Node            PID       Remote link  Remote user");
    emit(ctx, "");
    for (unsigned i = 0; i < v->nlinks; i++) {
        const struct dnet_netshow_link *l = &v->links[i];
        char id[24], user[16], line[112];
        ns_node_id(l->node, l->name, id, sizeof id);
        if (l->object == 17)
            snprintf(user, sizeof user, "FAL");
        else if (l->object == 42)
            snprintf(user, sizeof user, "CTERM");
        else if (l->object)
            snprintf(user, sizeof user, "%u", (unsigned)l->object);
        else
            user[0] = '\0';
        snprintf(line, sizeof line, " %7u     %-16s %08X  %11u   %s", (unsigned)l->local_link,
                 id, (unsigned)l->pid, (unsigned)l->remote_link, user);
        size_t end = strlen(line);
        while (end > 0 && line[end - 1] == ' ')
            line[--end] = '\0';
        emit(ctx, line);
    }
    emit(ctx, "");
}

/* "Product:  DECNET        Node:  VAX1                 Address(es):  1.1"
 * (oracle: the product in a 14-wide field after "Product:  ", the node name in a
 * 21-wide field after "Node:  "). */
DNET_NETSHOW_API void dnet_netshow_fmt_network_line(const struct dnet_netshow_exec *ex,
                                                    char *buf, size_t cap)
{
    char as[12];
    dnet_netshow_addr(ex->addr, as, sizeof as);
    snprintf(buf, cap, "Product:  %-14sNode:  %-21sAddress(es):  %s", "DECNET",
             ex->name, as);
}
