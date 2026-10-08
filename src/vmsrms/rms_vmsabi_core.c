/*
 * rms_vmsabi_core.c - the RMS-engine half of the VMS-ABI entry points
 * (vms-692): $PARSE / $SEARCH on OVMX's internal control blocks, keyed by the
 * NAM$L_WCC context handle the VMS-layout NAM carries, as VMS RMS keys its
 * internal structures by IFI/ISI/WCC. See rms_vmsabi_core.h.
 */
#include <stdlib.h>
#include <string.h>

#include "rms/rms.h"
#include "rmsdef.h"
#include "rms_vmsabi_core.h"

#define ABI_MAXCTX 64

struct abi_ctx {
    struct FAB fab;
    struct NAM nam;
    char fna[256];
    char dna[256];
    char esa[256];
    char rsa[256];
};

static struct abi_ctx *g_ctx[ABI_MAXCTX];

static struct abi_ctx *ctx_get(uint32_t wcc)
{
    if (wcc == 0 || wcc > ABI_MAXCTX)
        return NULL;
    return g_ctx[wcc - 1];
}

static void ctx_release(uint32_t *wcc)
{
    struct abi_ctx *c = ctx_get(*wcc);
    if (c) {
        rms_search_end(&c->nam);
        free(c);
        g_ctx[*wcc - 1] = NULL;
    }
    *wcc = 0;
}

static uint8_t off_of(const char *base, const char *p, unsigned len)
{
    if (!p || p < base || p > base + len)
        return 0;
    return (uint8_t)(p - base);
}

/* Split a full filespec into its components (node::dev:[dir]name.type;ver). */
static void components(struct ovmx_rmsabi_name *io)
{
    const char *s = io->str, *e = io->str + io->len, *p = s;
    io->node_off = io->node_len = io->dev_off = io->dev_len = 0;
    io->dir_off = io->dir_len = io->name_off = io->name_len = 0;
    io->type_off = io->type_len = io->ver_off = io->ver_len = 0;
    for (const char *q = p; q + 1 < e; q++) {
        if (q[0] == ':' && q[1] == ':') {
            io->node_off = 0;
            io->node_len = (uint8_t)(q + 2 - s);
            p = q + 2;
            break;
        }
    }
    const char *lb = memchr(p, '[', (size_t)(e - p));
    if (!lb)
        lb = memchr(p, '<', (size_t)(e - p));
    const char *colon = memchr(p, ':', (size_t)(e - p));
    if (colon && (!lb || colon < lb)) {
        io->dev_off = (uint8_t)(p - s);
        io->dev_len = (uint8_t)(colon + 1 - p);
        p = colon + 1;
    }
    if (p < e && (*p == '[' || *p == '<')) {
        char close = *p == '[' ? ']' : '>';
        const char *rb = memchr(p, close, (size_t)(e - p));
        if (rb) {
            io->dir_off = (uint8_t)(p - s);
            io->dir_len = (uint8_t)(rb + 1 - p);
            p = rb + 1;
        }
    }
    const char *dot = memchr(p, '.', (size_t)(e - p));
    const char *semi = memchr(p, ';', (size_t)(e - p));
    const char *nend = dot ? dot : semi ? semi : e;
    io->name_off = (uint8_t)(p - s);
    io->name_len = (uint8_t)(nend - p);
    if (dot) {
        const char *tend = semi ? semi : e;
        io->type_off = (uint8_t)(dot - s);
        io->type_len = (uint8_t)(tend - dot);
    }
    if (semi) {
        io->ver_off = (uint8_t)(semi - s);
        io->ver_len = (uint8_t)(e - semi);
    }
}

static void dvi_from_dev(struct ovmx_rmsabi_name *io)
{
    unsigned n = io->dev_len ? io->dev_len - 1u : 0;     /* drop the ':' */
    if (n > 15)
        n = 15;
    memset(io->dvi, 0, sizeof io->dvi);
    io->dvi[0] = (char)n;
    memcpy(io->dvi + 1, io->str + io->dev_off, n);
}

uint32_t ovmx_rmsabi_parse(uint32_t *wcc, struct ovmx_rmsabi_name *io)
{
    ctx_release(wcc);
    int slot = -1;
    for (int i = 0; i < ABI_MAXCTX; i++)
        if (!g_ctx[i]) { slot = i; break; }
    if (slot < 0)
        return RMS$_WCC;                     /* no free wildcard context */
    struct abi_ctx *c = calloc(1, sizeof *c);
    if (!c)
        return RMS$_BUG;

    unsigned fns = io->fns < sizeof c->fna ? io->fns : sizeof c->fna - 1;
    unsigned dns = io->dns < sizeof c->dna ? io->dns : sizeof c->dna - 1;
    if (io->fna) memcpy(c->fna, io->fna, fns);
    if (io->dna) memcpy(c->dna, io->dna, dns);
    c->fab = cc$rms_fab;
    c->nam = cc$rms_nam;
    c->fab.fab$l_fna = c->fna;
    c->fab.fab$b_fns = (uint8_t)fns;
    if (dns) {
        c->fab.fab$l_dna = c->dna;
        c->fab.fab$b_dns = (uint8_t)dns;
    }
    c->fab.fab$l_nam = &c->nam;
    c->nam.nam$l_esa = c->esa;
    c->nam.nam$b_ess = 255;
    c->nam.nam$l_rsa = c->rsa;
    c->nam.nam$b_rss = 255;
    c->nam.nam$b_nop = (uint8_t)io->nop;

    uint32_t st = sys$parse(&c->fab, 0, 0);
    io->stv = c->fab.fab$l_stv;
    if (!(st & 1)) {
        free(c);
        return st;
    }
    g_ctx[slot] = c;
    *wcc = (uint32_t)slot + 1u;

    io->len = c->nam.nam$b_esl;
    memcpy(io->str, c->esa, io->len);
    io->fnb = c->nam.nam$l_fnb;
    components(io);
    dvi_from_dev(io);
    memset(io->fid, 0, sizeof io->fid);
    memset(io->did, 0, sizeof io->did);
    (void)off_of;
    return st;
}

uint32_t ovmx_rmsabi_search(uint32_t *wcc, struct ovmx_rmsabi_name *io)
{
    struct abi_ctx *c = ctx_get(*wcc);
    if (!c)
        return RMS$_WCC;
    uint32_t st = sys$search(&c->fab, 0, 0);
    io->stv = c->fab.fab$l_stv;
    if (!(st & 1)) {
        ctx_release(wcc);                    /* end of search: context gone */
        return st;
    }
    io->len = c->nam.nam$b_rsl;
    memcpy(io->str, c->rsa, io->len);
    io->fnb = c->nam.nam$l_fnb;
    components(io);
    dvi_from_dev(io);
    uint16_t n = 0, sq = 0;
    uint8_t rvn = 0, nmx = 0;
    if (rms_search_fid(&c->nam, &n, &sq, &rvn, &nmx)) {
        io->fid[0] = n;
        io->fid[1] = sq;
        io->fid[2] = (uint16_t)(rvn | (nmx << 8));
    }
    if (rms_search_did(&c->nam, &n, &sq, &rvn, &nmx)) {
        io->did[0] = n;
        io->did[1] = sq;
        io->did[2] = (uint16_t)(rvn | (nmx << 8));
    }
    return st;
}
