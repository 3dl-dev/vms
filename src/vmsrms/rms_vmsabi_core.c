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

/* ======================================================================
 * File and record operations (vms-8b5): $CREATE/$OPEN/$CLOSE/$ERASE and the
 * record services on OVMX's internal FAB/RAB/NAM, behind IFI/ISI handles.
 * ====================================================================== */

#define ABI_MAXFILE 63
#define ABI_MAXSTRM 63

struct abi_file {
    struct FAB fab;
    struct NAM nam;
    char fna[256];
    char dna[256];
    char esa[256];
    char rsa[256];
};

struct abi_strm {
    struct RAB rab;
    unsigned   file;                 /* slot of its file */
};

static struct abi_file *g_file[ABI_MAXFILE];
static struct abi_strm *g_strm[ABI_MAXSTRM];
static uint16_t g_file_gen[ABI_MAXFILE], g_strm_gen[ABI_MAXSTRM];

/* handle = generation << 6 | (slot + 1); 0 is never a handle */
static uint16_t handle(unsigned slot, uint16_t gen) { return (uint16_t)((gen << 6) | (slot + 1u)); }

static int file_slot(uint16_t ifi)
{
    unsigned s = (ifi & 63u);
    if (s == 0 || s > ABI_MAXFILE || !g_file[s - 1] || handle(s - 1, g_file_gen[s - 1]) != ifi)
        return -1;
    return (int)s - 1;
}

static int strm_slot(uint16_t isi)
{
    unsigned s = (isi & 63u);
    if (s == 0 || s > ABI_MAXSTRM || !g_strm[s - 1] || handle(s - 1, g_strm_gen[s - 1]) != isi)
        return -1;
    return (int)s - 1;
}

static void strm_free(int s)
{
    free(g_strm[s]);
    g_strm[s] = NULL;
    g_strm_gen[s] = (uint16_t)((g_strm_gen[s] + 1u) & 0x3FFu);
}

static void file_free(int s)
{
    for (int k = 0; k < ABI_MAXSTRM; k++)
        if (g_strm[k] && g_strm[k]->file == (unsigned)s)
            strm_free(k);                    /* $CLOSE ends every stream */
    free(g_file[s]);
    g_file[s] = NULL;
    g_file_gen[s] = (uint16_t)((g_file_gen[s] + 1u) & 0x3FFu);
}

/* Where a native NAM component pointer lies: 1 expanded, 2 resultant. */
static void comp(struct ovmx_rmsabi_namout *o, int k, const struct abi_file *f,
                 const char *p, unsigned len)
{
    o->which[k] = 0;
    o->off[k] = 0;
    o->len[k] = (uint8_t)len;
    if (!p)
        return;
    if (o->rsl && p >= f->rsa && p <= f->rsa + o->rsl) {
        o->which[k] = 2;
        o->off[k] = (uint8_t)(p - f->rsa);
    } else if (o->esl && p >= f->esa && p <= f->esa + o->esl) {
        o->which[k] = 1;
        o->off[k] = (uint8_t)(p - f->esa);
    }
}

static void nam_out(struct ovmx_rmsabi_namout *o, const struct abi_file *f)
{
    const struct NAM *n = &f->nam;
    o->esl = n->nam$b_esl;
    memcpy(o->esa, f->esa, o->esl);
    o->rsl = n->nam$b_rsl;
    memcpy(o->rsa, f->rsa, o->rsl);
    o->fnb = n->nam$l_fnb;
    comp(o, 0, f, n->nam$l_node, n->nam$b_node);
    comp(o, 1, f, n->nam$l_dev, n->nam$b_dev);
    comp(o, 2, f, n->nam$l_dir, n->nam$b_dir);
    comp(o, 3, f, n->nam$l_name, n->nam$b_name);
    comp(o, 4, f, n->nam$l_type, n->nam$b_type);
    comp(o, 5, f, n->nam$l_ver, n->nam$b_ver);
    memcpy(o->fid, n->nam$w_fid, sizeof o->fid);
    memcpy(o->did, n->nam$w_did, sizeof o->did);
    memcpy(o->dvi, n->nam$t_dvi, sizeof o->dvi);
}

static void fab_in(struct abi_file *f, const struct ovmx_rmsabi_fab *io)
{
    unsigned fns = io->fns < sizeof f->fna ? io->fns : sizeof f->fna - 1;
    unsigned dns = io->dns < sizeof f->dna ? io->dns : sizeof f->dna - 1;
    if (io->fna) memcpy(f->fna, io->fna, fns);
    if (io->dna) memcpy(f->dna, io->dna, dns);
    f->fab = cc$rms_fab;
    f->nam = cc$rms_nam;
    f->fab.fab$l_fna = f->fna;
    f->fab.fab$b_fns = (uint8_t)fns;
    if (dns) {
        f->fab.fab$l_dna = f->dna;
        f->fab.fab$b_dns = (uint8_t)dns;
    }
    f->fab.fab$l_fop = io->fop;
    f->fab.fab$l_alq = io->alq;
    f->fab.fab$w_deq = io->deq;
    f->fab.fab$b_fac = io->fac;
    f->fab.fab$b_shr = io->shr;
    f->fab.fab$b_org = io->org;
    f->fab.fab$b_rat = io->rat;
    f->fab.fab$b_rfm = io->rfm;
    f->fab.fab$w_mrs = io->mrs;
    f->fab.fab$l_mrn = io->mrn;
    f->fab.fab$b_fsz = io->fsz;
    if (io->nam) {
        f->fab.fab$l_nam = &f->nam;
        f->nam.nam$l_esa = f->esa;
        f->nam.nam$b_ess = (uint8_t)(io->ess < 255 ? io->ess : 255);
        f->nam.nam$l_rsa = f->rsa;
        f->nam.nam$b_rss = (uint8_t)(io->rss < 255 ? io->rss : 255);
        f->nam.nam$b_nop = (uint8_t)io->nop;
    }
}

static void fab_out(struct ovmx_rmsabi_fab *io, const struct abi_file *f)
{
    io->stv = f->fab.fab$l_stv;
    io->fop = f->fab.fab$l_fop;
    io->alq = f->fab.fab$l_alq;
    io->deq = f->fab.fab$w_deq;
    io->fac = f->fab.fab$b_fac;
    io->shr = f->fab.fab$b_shr;
    io->org = f->fab.fab$b_org;
    io->rat = f->fab.fab$b_rat;
    io->rfm = f->fab.fab$b_rfm;
    io->mrs = f->fab.fab$w_mrs;
    io->mrn = f->fab.fab$l_mrn;
    io->fsz = f->fab.fab$b_fsz;
    if (io->nam)
        nam_out(&io->n, f);
}

uint32_t ovmx_rmsabi_open(int create, uint16_t *ifi, struct ovmx_rmsabi_fab *io)
{
    if (file_slot(*ifi) >= 0)
        return RMS$_IFI;                     /* the FAB already has a file open */
    int s = -1;
    for (int i = 0; i < ABI_MAXFILE; i++)
        if (!g_file[i]) { s = i; break; }
    if (s < 0)
        return RMS$_IFI;
    struct abi_file *f = calloc(1, sizeof *f);
    if (!f)
        return RMS$_BUG;
    fab_in(f, io);
    uint32_t st = (create ? sys$create : sys$open)(&f->fab, 0, 0);
    fab_out(io, f);
    if (!(st & 1)) {
        free(f);
        return st;
    }
    g_file[s] = f;
    *ifi = handle((unsigned)s, g_file_gen[s]);
    return st;
}

uint32_t ovmx_rmsabi_close(uint16_t *ifi, uint32_t *stv)
{
    int s = file_slot(*ifi);
    if (s < 0)
        return RMS$_IFI;
    uint32_t st = sys$close(&g_file[s]->fab, 0, 0);
    *stv = g_file[s]->fab.fab$l_stv;
    file_free(s);
    *ifi = 0;
    return st;
}

uint32_t ovmx_rmsabi_erase(struct ovmx_rmsabi_fab *io)
{
    struct abi_file *f = calloc(1, sizeof *f);
    if (!f)
        return RMS$_BUG;
    fab_in(f, io);
    uint32_t st = sys$erase(&f->fab, 0, 0);
    fab_out(io, f);
    free(f);
    return st;
}

static void rab_in(struct RAB *r, const struct ovmx_rmsabi_rab *io)
{
    r->rab$l_rop = io->rop;
    r->rab$b_rac = io->rac;
    r->rab$b_krf = io->krf;
    r->rab$b_ksz = io->ksz;
    r->rab$l_kbf = (char *)(uintptr_t)io->kbf;
    r->rab$l_ubf = io->ubf;
    r->rab$w_usz = (uint16_t)io->usz;
    r->rab$l_rbf = (char *)(uintptr_t)io->rbf_in;
    r->rab$w_rsz = (uint16_t)io->rsz_in;
}

static void rab_out(struct ovmx_rmsabi_rab *io, const struct RAB *r)
{
    io->rbf = r->rab$l_rbf;
    io->rsz = r->rab$w_rsz;
    io->stv = r->rab$l_stv;
    io->rfa[0] = r->rab$w_rfa.rfa$w_area;
    io->rfa[1] = r->rab$w_rfa.rfa$w_page;
    io->rfa[2] = r->rab$w_rfa.rfa$w_offset;
}

uint32_t ovmx_rmsabi_connect(uint16_t ifi, uint16_t *isi, struct ovmx_rmsabi_rab *io)
{
    int fs = file_slot(ifi);
    if (fs < 0)
        return RMS$_IFI;
    if (strm_slot(*isi) >= 0)
        return RMS$_ISI;                     /* the RAB is already connected */
    int s = -1;
    for (int i = 0; i < ABI_MAXSTRM; i++)
        if (!g_strm[i]) { s = i; break; }
    if (s < 0)
        return RMS$_ISI;
    struct abi_strm *t = calloc(1, sizeof *t);
    if (!t)
        return RMS$_BUG;
    t->rab = cc$rms_rab;
    t->rab.rab$l_fab = &g_file[fs]->fab;
    t->file = (unsigned)fs;
    rab_in(&t->rab, io);
    uint32_t st = sys$connect(&t->rab, 0, 0);
    rab_out(io, &t->rab);
    if (!(st & 1)) {
        free(t);
        return st;
    }
    g_strm[s] = t;
    *isi = handle((unsigned)s, g_strm_gen[s]);
    return st;
}

uint32_t ovmx_rmsabi_record(int op, uint16_t *isi, struct ovmx_rmsabi_rab *io)
{
    int s = strm_slot(*isi);
    if (s < 0)
        return RMS$_ISI;
    struct RAB *r = &g_strm[s]->rab;
    rab_in(r, io);
    uint32_t st;
    switch (op) {
    case OVMX_RMSABI_GET:        st = sys$get(r, 0, 0); break;
    case OVMX_RMSABI_PUT:        st = sys$put(r, 0, 0); break;
    case OVMX_RMSABI_FIND:       st = sys$find(r, 0, 0); break;
    case OVMX_RMSABI_UPDATE:     st = sys$update(r, 0, 0); break;
    case OVMX_RMSABI_DELETE:     st = sys$delete(r, 0, 0); break;
    case OVMX_RMSABI_REWIND:     st = sys$rewind(r, 0, 0); break;
    case OVMX_RMSABI_FLUSH:      st = sys$flush(r, 0, 0); break;
    case OVMX_RMSABI_DISCONNECT:
        st = sys$disconnect(r, 0, 0);
        rab_out(io, r);
        strm_free(s);
        *isi = 0;
        return st;
    default:                     return RMS$_BUG;
    }
    rab_out(io, r);
    return st;
}
