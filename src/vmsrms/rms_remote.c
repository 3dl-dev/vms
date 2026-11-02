/*
 * rms_remote.c - RMS remote file access: node"user pw"::file over DAP to the
 * remote FAL (rd vms-0a7 slice S1). See rms_remote.h for what it is and why.
 *
 * The DAP sequences are the ones the OVMX FAL client already runs against a
 * real VAX V7.3 FAL (src/vmsdecnet/fal/dnet_fal.c, proven by
 * tests/lab/captures/vms-6a4-dir-type-copy-20261102/ and the VAX<->VAX oracle
 * tests/lab/captures/decnet-fal-verbs-20261008/):
 *
 *   every link   CONFIGURATION <-> CONFIGURATION
 *   $SEARCH      ACCESS(DIRECTORY LIST) -> NAME(volume) NAME(directory)
 *                NAME(file)... ACCESS COMPLETE(RESPONSE) | STATUS
 *   $OPEN        ATTRIBUTES + ACCESS(OPEN, FAC=GET) -> ATTRIBUTES.. ACK;
 *                CONTROL(CONNECT) -> ACK
 *   $GET         (first) CONTROL(GET, RAC=sequential file transfer); then
 *                DATA per record, STATUS(end of file) at the end
 *   $CREATE      ATTRIBUTES + ACCESS(CREATE, FAC=PUT) -> ACK;
 *                CONTROL(CONNECT) -> ACK; CONTROL(PUT, RAC=transfer)
 *   $PUT         DATA
 *   $CLOSE       (after a GET) ACCESS COMPLETE(END OF STREAM) -> RESPONSE;
 *                ACCESS COMPLETE(CLOSE) -> RESPONSE | STATUS
 *
 * CLEAN-ROOM (Rule 8): message layouts are the public DAP 5.6 specification
 * (AA-K177A-TK, via the vmsdecnet_dap codec); the sequences and the RMS status
 * a refusal maps to are observed from real VMS systems in the captures above.
 */
#include "rms_remote.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>      /* snprintf: string formatting only, no I/O */
#include <strings.h>

#include "rms/rms.h"
#include "dnet_dap.h"
#include "ssdef.h"
#include "rmsdef.h"
#include "starlet.h"
#include "descrip.h"
#include "iodef.h"
#include "iosbdef.h"

#define RR_BUFSIZ   1459   /* the NSP segment size OVMX's CONFIGURATION offers */
#define RR_SEG      4096   /* one received NSP segment (a VAX sends <= 1459)   */
#define RR_FAL_OBJ  17
#define RR_FLD      40     /* user / password / account field bound           */
#define RR_NODE     64
#define RR_MAXSTATE 16

/* ---- the logical link: the _NET: device ---------------------------------- */

struct net_link { uint16_t chan; };

static uint32_t net_qiow(uint16_t chan, uint32_t func, void *p1, uint32_t p2, size_t *xfer)
{
    struct _iosb iosb;
    memset(&iosb, 0, sizeof iosb);
    uint32_t st = sys$qiow(0, chan, func, &iosb, NULL, 0, p1, p2, 0, 0, 0, 0);
    if (st & 1)
        st = iosb.iosb$w_status;     /* how the I/O ENDED (NETACP's answer) */
    if (xfer)
        *xfer = (st & 1) ? iosb.iosb$w_bcnt : 0;
    return st;
}

static void *net_open(void *ctx, const char *ncb, size_t ncblen, uint32_t *st)
{
    (void)ctx;
    static char devnam[] = "_NET:";
    struct dsc$descriptor_s d;
    d.dsc$w_length = (uint16_t)(sizeof devnam - 1);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = devnam;
    uint16_t chan = 0;
    uint32_t s = sys$assign(&d, &chan, 0, NULL, 0);
    if (!(s & 1)) { *st = s; return NULL; }
    s = net_qiow(chan, IO$_ACCESS, (void *)ncb, (uint32_t)ncblen, NULL);
    if (!(s & 1)) { (void)sys$dassgn(chan); *st = s; return NULL; }
    struct net_link *L = calloc(1, sizeof *L);
    if (!L) {
        (void)net_qiow(chan, IO$_DEACCESS, NULL, 0, NULL);
        (void)sys$dassgn(chan);
        *st = SS$_INSFMEM;
        return NULL;
    }
    L->chan = chan;
    *st = SS$_NORMAL;
    return L;
}

static int net_send(void *link, const uint8_t *seg, size_t len)
{
    struct net_link *L = link;
    return (net_qiow(L->chan, IO$_WRITEVBLK, (void *)seg, (uint32_t)len, NULL) & 1) ? 0 : -1;
}

/* A read waits for the next segment (NETACP answers when one arrives, or
 * with the link's abort / disconnect). */
static int net_recv(void *link, uint8_t *buf, size_t cap, size_t *len)
{
    struct net_link *L = link;
    size_t x = 0;
    if (!(net_qiow(L->chan, IO$_READVBLK, buf, (uint32_t)cap, &x) & 1)) return -1;
    *len = x;
    return 0;
}

static void net_close(void *link)
{
    struct net_link *L = link;
    if (!L) return;
    (void)net_qiow(L->chan, IO$_DEACCESS, NULL, 0, NULL);
    (void)sys$dassgn(L->chan);
    free(L);
}

static const struct rms_remote_link_ops g_net_ops = { net_open, net_send, net_recv, net_close, NULL };
static const struct rms_remote_link_ops *g_ops = &g_net_ops;

void rms_remote_set_link_ops(const struct rms_remote_link_ops *ops)
{
    g_ops = ops ? ops : &g_net_ops;
}

/* ---- DAP over one link ---------------------------------------------------- */

struct rr_link {
    void   *h;
    uint8_t rx[RR_SEG];
    size_t  rxlen, rxoff;
};

static int rr_send(struct rr_link *k, const struct dnet_dap_msg *m)
{
    uint8_t seg[DNET_DAP_MAX_MSG];
    size_t n = 0;
    if (dnet_dap_encode(m, 0, seg, sizeof seg, &n) != DNET_DAP_OK) return -1;
    return g_ops->send(k->h, seg, n);
}

/* The next DAP message: from what is left of the last segment (a VMS FAL
 * blocks several messages into one), else from a fresh one. A message the
 * bounded decoder refuses ends the access. */
static int rr_recv(struct rr_link *k, struct dnet_dap_msg *m)
{
    while (k->rxoff >= k->rxlen) {
        size_t n = 0;
        if (g_ops->recv(k->h, k->rx, sizeof k->rx, &n) < 0) return -1;
        if (n > sizeof k->rx) return -1;
        k->rxlen = n; k->rxoff = 0;
    }
    size_t used = 0;
    if (dnet_dap_decode(k->rx + k->rxoff, k->rxlen - k->rxoff, m, &used) != DNET_DAP_OK ||
        used == 0 || used > k->rxlen - k->rxoff) {
        k->rxoff = k->rxlen = 0;
        return -1;
    }
    k->rxoff += used;
    return 0;
}

static int rr_simple(struct rr_link *k, enum dnet_dap_op op, uint8_t sub)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = op;
    if (op == DNET_DAP_ACCESS_COMPLETE) m.u.complete.cmpfunc = sub;
    if (op == DNET_DAP_CONTROL) {
        m.u.control.ctlfunc = sub;
        if (sub == DNET_DAP_CTL_GET || sub == DNET_DAP_CTL_PUT) {
            m.u.control.menu = DNET_DAP_CTLM_RAC;
            m.u.control.rac = DNET_DAP_RAC_SEQFILE;
        }
    }
    return rr_send(k, &m);
}

static int rr_config(struct rr_link *k)
{
    struct dnet_dap_msg m;
    dnet_dap_ovmx_config(&m, RR_BUFSIZ);
    if (rr_send(k, &m) < 0) return -1;
    if (rr_recv(k, &m) < 0 || m.op != DNET_DAP_CONFIG) return -1;
    return 0;
}

/* The RMS condition a remote STATUS reports. FNF, PRV, DNF and ACC are RMS's
 * own; otherwise a real VMS FAL carries the system status in the STV. */
static uint32_t dap_rms(uint16_t stscode, uint64_t stv, uint32_t fallback)
{
    switch (DNET_DAP_MIC(stscode)) {
    case DNET_DAP_MIC_FNF: return RMS$_FNF;
    case DNET_DAP_MIC_PRV: return RMS$_PRV;
    case DNET_DAP_MIC_DNF: return RMS$_DNF;
    case DNET_DAP_MIC_ACC: return RMS$_ACC;
    default: break;
    }
    if (stv && stv <= 0xffffffffu) return (uint32_t)stv;
    return fallback;
}

/* The file's attributes as the remote's ATTRIBUTES gave them. */
struct rr_attr {
    int have;
    uint8_t org, rfm, rat, fsz;
    uint16_t mrs, lrl, ffb;
    uint32_t hbk, ebk;
};

static void attr_take(struct rr_attr *a, const struct dnet_dap_msg *m)
{
    uint64_t mn = m->u.attr.menu;
    a->have = 1;
    a->org = (mn & (1u << DNET_DAP_ATT_ORG)) ? m->u.attr.org : DNET_DAP_ORG_SEQ;
    a->rfm = (mn & (1u << DNET_DAP_ATT_RFM)) ? m->u.attr.rfm : DNET_DAP_RFM_FIX;
    a->rat = (mn & (1u << DNET_DAP_ATT_RAT)) ? (uint8_t)(m->u.attr.rat & 0x0F) : 0;
    a->fsz = (mn & (1u << DNET_DAP_ATT_FSZ)) ? m->u.attr.fsz : 2;
    a->mrs = (mn & (1u << DNET_DAP_ATT_MRS)) ? m->u.attr.mrs : 0;
    a->lrl = (mn & (1u << DNET_DAP_ATT_LRL)) ? m->u.attr.lrl : 0;
    a->ffb = (mn & (1u << DNET_DAP_ATT_FFB)) ? m->u.attr.ffb : 0;
    a->hbk = (mn & (1u << DNET_DAP_ATT_HBK)) && m->u.attr.hbk <= 0xffffffffu
             ? (uint32_t)m->u.attr.hbk : 0;
    a->ebk = (mn & (1u << DNET_DAP_ATT_EBK)) && m->u.attr.ebk <= 0xffffffffu
             ? (uint32_t)m->u.attr.ebk : 0;
}

/* Up to the end of a setup reply: ATTRIBUTES (kept) and the extended
 * attributes / NAME are passed over; ACK is the end (0); a STATUS is a
 * refusal (1, codes in *sc / *sv); anything else breaks the access (-1). */
static int await_ack(struct rr_link *k, struct rr_attr *a, uint16_t *sc, uint64_t *sv)
{
    struct dnet_dap_msg m;
    for (;;) {
        if (rr_recv(k, &m) < 0) return -1;
        switch (m.op) {
        case DNET_DAP_ACKNOWLEDGE: return 0;
        case DNET_DAP_STATUS:
            *sc = m.u.status.stscode;
            *sv = m.u.status.have_stv ? m.u.status.stv : 0;
            return 1;
        case DNET_DAP_ATTRIBUTES:
            if (a) attr_take(a, &m);
            continue;
        case DNET_DAP_NAME: case DNET_DAP_KEYDEF: case DNET_DAP_ALLOC:
        case DNET_DAP_SUMMARY: case DNET_DAP_DATETIME: case DNET_DAP_PROTECTION:
        case DNET_DAP_ACL:
            continue;
        default:
            return -1;
        }
    }
}

/* ACCESS COMPLETE(func) and its RESPONSE; a STATUS is the remote's failure. */
static uint32_t complete(struct rr_link *k, uint8_t func, uint32_t fallback, uint32_t *stv)
{
    struct dnet_dap_msg m;
    if (rr_simple(k, DNET_DAP_ACCESS_COMPLETE, func) < 0) { *stv = SS$_ABORT; return fallback; }
    if (rr_recv(k, &m) < 0) { *stv = SS$_ABORT; return fallback; }
    if (m.op == DNET_DAP_ACCESS_COMPLETE && m.u.complete.cmpfunc == DNET_DAP_CMP_RESPONSE)
        return RMS$_NORMAL;
    if (m.op == DNET_DAP_STATUS) {
        *stv = m.u.status.have_stv && m.u.status.stv <= 0xffffffffu ? (uint32_t)m.u.status.stv : 0;
        return dap_rms(m.u.status.stscode, m.u.status.stv, fallback);
    }
    *stv = SS$_ABORT;
    return fallback;
}

/* ---- the filespec ---------------------------------------------------------- */

struct rr_spec {
    char node[RR_NODE];
    int  has_access;
    char user[RR_FLD + 1], pw[RR_FLD + 1], acct[RR_FLD + 1];
    char file[DNET_DAP_MAX_SPEC + 1];
};

int rms_remote_is_spec(const char *s, size_t n)
{
    int q = 0;
    if (!s) return 0;
    for (size_t i = 0; i + 1 < n && s[i]; i++) {
        if (s[i] == '"') { q = !q; continue; }
        if (!q && s[i] == ':' && s[i + 1] == ':') return 1;
    }
    return 0;
}

/*
 * The passwords given this process, per node and user. A resultant string
 * shows a password as the word "password" (NAM$M_PWD clear), so a caller that
 * re-opens the name $SEARCH returned -- COPY of a node:: wildcard -- hands RMS
 * that word; RMS answers with the password the same image gave it for that
 * node and user. In process memory only, never written anywhere.
 */
struct rr_cred { int used; char node[RR_NODE], user[RR_FLD + 1], pw[RR_FLD + 1]; };
static struct rr_cred g_cred[8];
static unsigned g_cred_next;

static struct rr_cred *cred_find(const char *node, const char *user)
{
    for (unsigned i = 0; i < sizeof g_cred / sizeof g_cred[0]; i++)
        if (g_cred[i].used && !strcasecmp(g_cred[i].node, node) &&
            !strcasecmp(g_cred[i].user, user))
            return &g_cred[i];
    return NULL;
}

static void cred_keep(const char *node, const char *user, const char *pw)
{
    struct rr_cred *c = cred_find(node, user);
    if (!c) {
        c = &g_cred[g_cred_next++ % (sizeof g_cred / sizeof g_cred[0])];
        memset(c, 0, sizeof *c);
        c->used = 1;
        snprintf(c->node, sizeof c->node, "%s", node);
        snprintf(c->user, sizeof c->user, "%s", user);
    }
    memset(c->pw, 0, sizeof c->pw);
    snprintf(c->pw, sizeof c->pw, "%s", pw);
}

/* node["access"]::file -> parts. One node only (no poor-man's routing).
 * Returns 0, or -1 for a spec RMS cannot carry (RMS$_SYN). */
static int spec_split(const char *s, size_t n, struct rr_spec *o)
{
    memset(o, 0, sizeof *o);
    char buf[512];
    if (!s || n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    size_t i = 0;
    while (buf[i] && buf[i] != '"' && !(buf[i] == ':' && buf[i + 1] == ':')) i++;
    if (i == 0 || i >= sizeof o->node || !buf[i]) return -1;
    memcpy(o->node, buf, i);
    for (size_t j = 0; j < i; j++)
        if (o->node[j] == ':' || o->node[j] == ' ') return -1;
    if (buf[i] == '"') {
        char *q2 = strchr(buf + i + 1, '"');
        if (!q2 || q2[1] != ':' || q2[2] != ':') return -1;
        *q2 = '\0';
        char *p = buf + i + 1;
        char *fld[3] = { o->user, o->pw, o->acct };
        int nf = 0;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            char *e = p;
            while (*e && *e != ' ') e++;
            size_t l = (size_t)(e - p);
            if (nf >= 3 || l > RR_FLD) { memset(buf, 0, sizeof buf); return -1; }
            memcpy(fld[nf++], p, l);
            p = e;
        }
        o->has_access = 1;
        i = (size_t)(q2 - buf) + 3;
    } else {
        i += 2;
    }
    const char *file = buf + i;
    if (rms_remote_is_spec(file, strlen(file)) || strlen(file) > DNET_DAP_MAX_SPEC) {
        memset(buf, 0, sizeof buf);
        return -1;
    }
    snprintf(o->file, sizeof o->file, "%s", file);
    memset(buf, 0, sizeof buf);

    /* The word "password" stands for the one this image gave for the node. */
    if (o->has_access && o->user[0]) {
        if (!strcasecmp(o->pw, "password")) {
            struct rr_cred *c = cred_find(o->node, o->user);
            if (c) snprintf(o->pw, sizeof o->pw, "%s", c->pw);
        } else if (o->pw[0]) {
            cred_keep(o->node, o->user, o->pw);
        }
    }
    return 0;
}

/* The node part as VMS shows it in an expanded or resultant string. */
static size_t spec_display(const struct rr_spec *sp, char *out, size_t cap)
{
    int n;
    if (sp->has_access && sp->user[0])
        n = snprintf(out, cap, "%s\"%s%s\"::", sp->node, sp->user,
                     sp->pw[0] ? " password" : "");
    else
        n = snprintf(out, cap, "%s::", sp->node);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    return (size_t)n;
}

/* Connect to the FAL. The NCB carries the access control; it is wiped. */
static int link_open(const struct rr_spec *sp, struct rr_link *k, uint32_t *st)
{
    memset(k, 0, sizeof *k);
    char ncb[256];
    int n;
    if (sp->acct[0] && !sp->pw[0]) { *st = SS$_BADPARAM; return -1; }
    if (sp->has_access)
        n = snprintf(ncb, sizeof ncb, "%s\"%s%s%s%s%s\"::\"%d=\"", sp->node, sp->user,
                     sp->pw[0] ? " " : "", sp->pw, sp->acct[0] ? " " : "", sp->acct,
                     RR_FAL_OBJ);
    else
        n = snprintf(ncb, sizeof ncb, "%s::\"%d=\"", sp->node, RR_FAL_OBJ);
    if (n < 0 || (size_t)n >= sizeof ncb) {
        memset(ncb, 0, sizeof ncb);
        *st = SS$_BADPARAM;
        return -1;
    }
    uint32_t s = SS$_ABORT;
    k->h = g_ops->open(g_ops->ctx, ncb, (size_t)n, &s);
    memset(ncb, 0, sizeof ncb);
    if (!k->h) { *st = (s & 1) ? SS$_ABORT : s; return -1; }
    if (rr_config(k) < 0) {
        g_ops->close(k->h);
        k->h = NULL;
        *st = SS$_ABORT;
        return -1;
    }
    *st = SS$_NORMAL;
    return 0;
}

static void link_close(struct rr_link *k)
{
    if (k->h) g_ops->close(k->h);
    k->h = NULL;
}

/* ---- per control block state -------------------------------------------- */

enum { RR_SEARCH = 1, RR_FILE = 2 };
enum { RR_GET = 1, RR_PUT = 2 };

struct rr_state {
    int used, kind;
    const void *fab, *nam;
    struct rr_spec sp;
    char disp[RR_NODE + 2 * RR_FLD + 16];
    size_t displen;
    /* RR_SEARCH */
    int listed;
    char **names; size_t nnames, pos;
    uint32_t end_sts, end_stv;
    char end_what[3 * (DNET_DAP_MAX_SPEC + 1)];
    /* RR_FILE */
    struct rr_link link;
    int mode, xfer, eof;
    struct rr_attr attr;
};

static struct rr_state g_st[RR_MAXSTATE];

static struct rr_state *st_by_fab(const void *fab)
{
    if (!fab) return NULL;
    for (int i = 0; i < RR_MAXSTATE; i++)
        if (g_st[i].used && g_st[i].fab == fab) return &g_st[i];
    return NULL;
}

static void st_free(struct rr_state *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->nnames; i++) free(s->names[i]);
    free(s->names);
    link_close(&s->link);
    memset(s, 0, sizeof *s);         /* the password with it */
}

static struct rr_state *st_new(const void *fab)
{
    struct rr_state *s = st_by_fab(fab);
    if (s) st_free(s);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < RR_MAXSTATE; i++)
            if (!g_st[i].used) { g_st[i].used = 1; g_st[i].fab = fab; return &g_st[i]; }
        /* Full: a search context its caller never ended is the one to go
         * (it holds no link); an open file is never taken from its caller. */
        for (int i = 0; i < RR_MAXSTATE; i++)
            if (g_st[i].kind == RR_SEARCH) { st_free(&g_st[i]); break; }
    }
    return NULL;
}

static int fab_spec(const struct FAB *fab, const char **s, size_t *n)
{
    if (!fab || !fab->fab$l_fna || fab->fab$b_fns == 0) return 0;
    *s = fab->fab$l_fna;
    *n = fab->fab$b_fns;
    return 1;
}

/* A FAB is remote while its file name names a node. State left under a FAB
 * address whose name no longer does (a caller freed the block without
 * $CLOSE and the address was reused) is dropped, never applied to it. */
int rms_remote_owns_fab(const void *fab)
{
    const struct FAB *f = fab;
    if (!f || f->fab$b_bid != FAB$C_BID) return 0;
    const char *s; size_t n;
    if (fab_spec(f, &s, &n) && rms_remote_is_spec(s, n)) return 1;
    struct rr_state *st = st_by_fab(f);
    if (st) st_free(st);
    return 0;
}

int rms_remote_owns_rab(const void *rab)
{
    const struct RAB *r = rab;
    if (!r || r->rab$b_bid != RAB$C_BID || !rms_remote_owns_fab(r->rab$l_fab)) return 0;
    struct rr_state *s = st_by_fab(r->rab$l_fab);
    return s && s->kind == RR_FILE;
}

/* Fill a NAM string area and its component pointers from `str`, whose first
 * `nodelen` bytes are the node part. 0, or -1 when the area is too short. */
static int nam_put(char *area, uint8_t cap, uint8_t *outlen, struct NAM *nam,
                   const char *str, size_t nodelen)
{
    size_t L = strlen(str);
    if (!area || L > cap) return -1;
    memcpy(area, str, L);
    *outlen = (uint8_t)L;
    if (!nam) return 0;
    const char *b = area, *e = area + L;
    const char *p = b + nodelen;
    nam->nam$l_node = (char *)b; nam->nam$b_node = (uint8_t)nodelen;
    /* device: up to a ':' that comes before any '[' / '<' */
    const char *dev_end = NULL;
    for (const char *q = p; q < e && *q != '[' && *q != '<'; q++)
        if (*q == ':') dev_end = q + 1;
    nam->nam$l_dev = (char *)p;
    nam->nam$b_dev = (uint8_t)(dev_end ? dev_end - p : 0);
    if (dev_end) p = dev_end;
    nam->nam$l_dir = (char *)p;
    nam->nam$b_dir = 0;
    if (p < e && (*p == '[' || *p == '<')) {
        char close = (*p == '[') ? ']' : '>';
        const char *q = p;
        while (q < e && *q != close) q++;
        if (q < e) q++;
        nam->nam$b_dir = (uint8_t)(q - p);
        p = q;
    }
    const char *ver = memchr(p, ';', (size_t)(e - p));
    const char *nend = ver ? ver : e;
    const char *dot = NULL;
    for (const char *q = p; q < nend; q++) if (*q == '.') dot = q;
    const char *tstart = dot ? dot : nend;
    nam->nam$l_name = (char *)p;     nam->nam$b_name = (uint8_t)(tstart - p);
    nam->nam$l_type = (char *)tstart; nam->nam$b_type = (uint8_t)(nend - tstart);
    nam->nam$l_ver = (char *)nend;   nam->nam$b_ver = (uint8_t)(e - nend);
    return 0;
}

static int is_wild(const char *s)
{
    return strpbrk(s, "*%") != NULL || strstr(s, "...") != NULL;
}

/* ---- $PARSE ----------------------------------------------------------------- */

uint32_t rms_remote_parse(void *fabp)
{
    struct FAB *fab = fabp;
    const char *s; size_t n;
    if (!fab_spec(fab, &s, &n)) { fab->fab$l_sts = RMS$_SYN; return RMS$_SYN; }
    struct rr_state *st = st_new(fab);
    if (!st) { fab->fab$l_sts = RMS$_DME; return RMS$_DME; }
    if (spec_split(s, n, &st->sp) != 0) {
        st_free(st);
        fab->fab$l_sts = RMS$_SYN;
        return RMS$_SYN;
    }
    st->kind = RR_SEARCH;
    st->displen = spec_display(&st->sp, st->disp, sizeof st->disp);
    struct NAM *nam = fab->fab$l_nam;
    st->nam = nam;
    if (nam && nam->nam$b_bid == NAM$C_BID) {
        char esa[600];
        snprintf(esa, sizeof esa, "%s%s", st->disp, st->sp.file);
        if (nam->nam$l_esa &&
            nam_put(nam->nam$l_esa, nam->nam$b_ess, &nam->nam$b_esl, nam, esa, st->displen) < 0) {
            st_free(st);
            fab->fab$l_sts = RMS$_ESS;
            nam->nam$l_sts = RMS$_ESS;
            return RMS$_ESS;
        }
        nam->nam$l_fnb = NAM$M_NODE;
        if (is_wild(st->sp.file)) nam->nam$l_fnb |= NAM$M_WILDCARD;
        nam->nam$b_rsl = 0;
        nam->nam$l_sts = RMS$_NORMAL;
    }
    fab->fab$l_sts = RMS$_NORMAL;
    fab->fab$l_stv = 0;
    return RMS$_NORMAL;
}

/* ---- $SEARCH: one DIRECTORY LIST, then a name per call ------------------- */

struct dl {
    struct rr_state *st;
    char vol[DNET_DAP_MAX_SPEC + 1], dir[DNET_DAP_MAX_SPEC + 1];
    char pending[DNET_DAP_MAX_SPEC + 1];
    int have_pending, oom;
    size_t cap;
};

static void dl_commit(struct dl *d)
{
    struct rr_state *st = d->st;
    if (!d->have_pending) return;
    d->have_pending = 0;
    if (st->nnames >= RMS_REMOTE_MAXFILES) return;          /* the bound */
    if (st->nnames == d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 16;
        char **g = realloc(st->names, nc * sizeof *g);
        if (!g) { d->oom = 1; return; }
        st->names = g; d->cap = nc;
    }
    size_t l = strlen(d->vol) + strlen(d->dir) + strlen(d->pending) + 1;
    char *nm = malloc(l);
    if (!nm) { d->oom = 1; return; }
    snprintf(nm, l, "%s%s%s", d->vol, d->dir, d->pending);
    st->names[st->nnames++] = nm;
}

/* A file NAME is held until the next message: one the search's STATUS
 * follows is the spec that matched nothing (a VAX names NOSUCH.TXT;* before
 * its FNF), not a file. */
static void dl_name(struct dl *d, unsigned type, const char *spec)
{
    size_t n = strnlen(spec, DNET_DAP_MAX_SPEC);
    char *to = NULL;
    switch (type) {
    case DNET_DAP_NT_VOLUME:    dl_commit(d); to = d->vol; break;
    case DNET_DAP_NT_DIRECTORY: dl_commit(d); to = d->dir; break;
    case DNET_DAP_NT_FILENAME:  dl_commit(d); to = d->pending; d->have_pending = 1; break;
    default: return;             /* a whole FILESPEC NAME: not asked for */
    }
    memcpy(to, spec, n);
    to[n] = '\0';
}

static void search_list(struct rr_state *st)
{
    struct dl d;
    memset(&d, 0, sizeof d);
    d.st = st;
    st->listed = 1;
    st->end_sts = RMS$_NMF;
    st->end_stv = 0;
    snprintf(st->end_what, sizeof st->end_what, "%s", st->sp.file);

    uint32_t lst;
    struct rr_link k;
    if (link_open(&st->sp, &k, &lst) < 0) {
        st->end_sts = RMS_REMOTE_FND;          /* the link did not open */
        st->end_stv = lst;
        return;
    }
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ACCESS;
    m.u.access.accfunc = DNET_DAP_ACC_DIRLIST;
    snprintf(m.u.access.filespec, sizeof m.u.access.filespec, "%s", st->sp.file);
    uint16_t sc = 0; uint64_t sv = 0;
    int ok = 0, broke = 0;
    if (rr_send(&k, &m) < 0) broke = 1;
    while (!broke && !ok) {
        if (rr_recv(&k, &m) < 0) { broke = 1; break; }
        switch (m.op) {
        case DNET_DAP_NAME:
            m.u.name.namespec[DNET_DAP_MAX_SPEC] = '\0';
            dl_name(&d, (unsigned)m.u.name.nametype, m.u.name.namespec);
            if (d.oom) broke = 1;
            break;
        case DNET_DAP_ACKNOWLEDGE: case DNET_DAP_ATTRIBUTES:
            break;
        case DNET_DAP_ACCESS_COMPLETE:
            if (m.u.complete.cmpfunc == DNET_DAP_CMP_RESPONSE) ok = 1;
            else broke = 1;
            break;
        case DNET_DAP_STATUS:
            sc = m.u.status.stscode;
            sv = m.u.status.have_stv ? m.u.status.stv : 0;
            broke = 1;
            break;
        default:
            broke = 1;
            break;
        }
    }
    link_close(&k);
    if (ok) {
        dl_commit(&d);
        return;
    }
    /* The search ended in a STATUS (or the link broke): say what the remote
     * had expanded when it stopped. */
    snprintf(st->end_what, sizeof st->end_what, "%s%s%s", d.vol, d.dir,
             d.have_pending ? d.pending : (d.vol[0] || d.dir[0] ? "" : st->sp.file));
    if (d.oom) { st->end_sts = RMS$_DME; return; }
    if (sc && DNET_DAP_MIC(sc) == DNET_DAP_MIC_FNF && st->nnames == 0) {
        st->end_sts = RMS$_FNF;
        st->end_stv = sv <= 0xffffffffu ? (uint32_t)sv : 0;
        return;
    }
    st->end_sts = sc ? dap_rms(sc, sv, SS$_ABORT) : SS$_ABORT;
    st->end_stv = sc ? (sv <= 0xffffffffu ? (uint32_t)sv : 0) : SS$_ABORT;
}

uint32_t rms_remote_search(void *fabp)
{
    struct FAB *fab = fabp;
    struct rr_state *st = st_by_fab(fab);
    if (!st || st->kind != RR_SEARCH) { fab->fab$l_sts = RMS$_IFI; return RMS$_IFI; }
    struct NAM *nam = fab->fab$l_nam;
    if (!st->listed) search_list(st);
    if (st->pos < st->nnames) {
        char rsa[1024];
        snprintf(rsa, sizeof rsa, "%s%s", st->disp, st->names[st->pos++]);
        if (nam && nam->nam$l_rsa &&
            nam_put(nam->nam$l_rsa, nam->nam$b_rss, &nam->nam$b_rsl, nam, rsa, st->displen) < 0) {
            fab->fab$l_sts = RMS$_RSS;
            return RMS$_RSS;
        }
        fab->fab$l_sts = RMS$_NORMAL;
        fab->fab$l_stv = 0;
        if (nam) nam->nam$l_sts = RMS$_NORMAL;
        return RMS$_NORMAL;
    }
    /* The end: no more files, or the search's failure on what the remote
     * had expanded (left in the expanded string, as RMS leaves it). */
    if (st->end_sts != RMS$_NMF && nam && nam->nam$l_esa) {
        char esa[1024];
        snprintf(esa, sizeof esa, "%s%s", st->disp, st->end_what);
        (void)nam_put(nam->nam$l_esa, nam->nam$b_ess, &nam->nam$b_esl, nam, esa, st->displen);
    }
    if (nam) { nam->nam$b_rsl = 0; nam->nam$l_sts = st->end_sts; }
    fab->fab$l_sts = st->end_sts;
    fab->fab$l_stv = st->end_stv;
    return st->end_sts;
}

void rms_remote_search_end(void *nam)
{
    for (int i = 0; i < RR_MAXSTATE; i++)
        if (g_st[i].used && g_st[i].nam == nam && g_st[i].kind == RR_SEARCH)
            st_free(&g_st[i]);
}

/* ---- $OPEN / $CREATE --------------------------------------------------------- */

static uint32_t fab_fail(struct FAB *fab, struct rr_state *st, uint32_t sts, uint32_t stv)
{
    if (st) st_free(st);
    fab->fab$l_sts = sts;
    fab->fab$l_stv = stv;
    return sts;
}

/* Link + ATTRIBUTES + ACCESS + CONNECT. 0, or the RMS failure (status, STV). */
static int file_access(struct rr_state *st, uint8_t accfunc, const struct FAB *fab,
                       uint32_t *sts, uint32_t *stv)
{
    uint32_t lst;
    if (link_open(&st->sp, &st->link, &lst) < 0) {
        *sts = RMS_REMOTE_FND; *stv = lst;
        return -1;
    }
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ATTRIBUTES;
    m.u.attr.menu = (1u << DNET_DAP_ATT_DATATYPE) | (1u << DNET_DAP_ATT_ORG) |
                    (1u << DNET_DAP_ATT_RFM) | (1u << DNET_DAP_ATT_RAT);
    m.u.attr.datatype = DNET_DAP_DT_ASCII;
    m.u.attr.org = DNET_DAP_ORG_SEQ;
    m.u.attr.rfm = DNET_DAP_RFM_VAR;
    m.u.attr.rat = DNET_DAP_RAT_CR;
    if (accfunc == DNET_DAP_ACC_CREATE) {
        /* The new file's record structure is the FAB's (FAB$C_* and FAB$M_*
         * use the DAP numbering). */
        m.u.attr.rfm = fab->fab$b_rfm;
        m.u.attr.rat = fab->fab$b_rat & 0x0F;
        if (fab->fab$w_mrs) {
            m.u.attr.menu |= 1u << DNET_DAP_ATT_MRS;
            m.u.attr.mrs = fab->fab$w_mrs;
        }
        if (fab->fab$b_rfm == FAB$C_VFC) {
            m.u.attr.menu |= 1u << DNET_DAP_ATT_FSZ;
            m.u.attr.fsz = fab->fab$b_fsz ? fab->fab$b_fsz : 2;
        }
    }
    if (rr_send(&st->link, &m) < 0) goto broke;

    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ACCESS;
    m.u.access.accfunc = accfunc;
    snprintf(m.u.access.filespec, sizeof m.u.access.filespec, "%s", st->sp.file);
    m.u.access.have_fac = 1;
    m.u.access.fac = (accfunc == DNET_DAP_ACC_CREATE) ? DNET_DAP_FB_PUT : DNET_DAP_FB_GET;
    m.u.access.have_shr = 1;
    m.u.access.shr = (accfunc == DNET_DAP_ACC_CREATE) ? 0 : DNET_DAP_FB_GET;
    m.u.access.have_display = 1;
    m.u.access.display = DNET_DAP_DSP_MAIN;
    if (rr_send(&st->link, &m) < 0) goto broke;

    uint16_t sc = 0; uint64_t sv = 0;
    int r = await_ack(&st->link, &st->attr, &sc, &sv);
    if (r == 0) {
        if (rr_simple(&st->link, DNET_DAP_CONTROL, DNET_DAP_CTL_CONNECT) < 0) goto broke;
        r = await_ack(&st->link, &st->attr, &sc, &sv);
    }
    if (r < 0) goto broke;
    if (r > 0) {
        uint32_t fb = (accfunc == DNET_DAP_ACC_CREATE) ? RMS$_CRE : RMS$_ACC;
        *sts = dap_rms(sc, sv, fb);
        *stv = sv <= 0xffffffffu ? (uint32_t)sv : 0;
        return -1;
    }
    return 0;
broke:
    *sts = (accfunc == DNET_DAP_ACC_CREATE) ? RMS$_CRE : RMS$_ACC;
    *stv = SS$_ABORT;
    return -1;
}

static uint32_t file_start(void *fabp, uint8_t accfunc)
{
    struct FAB *fab = fabp;
    const char *s; size_t n;
    if (!fab_spec(fab, &s, &n)) return fab_fail(fab, NULL, RMS$_SYN, 0);
    struct rr_state *st = st_new(fab);
    if (!st) return fab_fail(fab, NULL, RMS$_DME, 0);
    if (spec_split(s, n, &st->sp) != 0) return fab_fail(fab, st, RMS$_SYN, 0);
    if (is_wild(st->sp.file)) return fab_fail(fab, st, RMS$_SYN, 0);
    st->kind = RR_FILE;
    st->mode = (accfunc == DNET_DAP_ACC_CREATE) ? RR_PUT : RR_GET;
    st->displen = spec_display(&st->sp, st->disp, sizeof st->disp);
    uint32_t sts = 0, stv = 0;
    if (file_access(st, accfunc, fab, &sts, &stv) < 0) return fab_fail(fab, st, sts, stv);
    if (st->mode == RR_PUT &&
        rr_simple(&st->link, DNET_DAP_CONTROL, DNET_DAP_CTL_PUT) < 0)
        return fab_fail(fab, st, RMS$_CRE, SS$_ABORT);
    st->xfer = (st->mode == RR_PUT);

    if (st->mode == RR_GET && st->attr.have) {
        fab->fab$b_org = st->attr.org;
        fab->fab$b_rfm = st->attr.rfm;
        fab->fab$b_rat = st->attr.rat;
        fab->fab$b_fsz = st->attr.rfm == DNET_DAP_RFM_VFC ? st->attr.fsz : 0;
        fab->fab$w_mrs = st->attr.mrs;
    }
    struct NAM *nam = fab->fab$l_nam;
    if (nam && nam->nam$b_bid == NAM$C_BID && nam->nam$l_rsa) {
        char rsa[600];
        snprintf(rsa, sizeof rsa, "%s%s", st->disp, st->sp.file);
        (void)nam_put(nam->nam$l_rsa, nam->nam$b_rss, &nam->nam$b_rsl, nam, rsa, st->displen);
    }
    fab->fab$l_sts = RMS$_NORMAL;
    fab->fab$l_stv = 0;
    return RMS$_NORMAL;
}

uint32_t rms_remote_open(void *fab)   { return file_start(fab, DNET_DAP_ACC_OPEN); }
uint32_t rms_remote_create(void *fab) { return file_start(fab, DNET_DAP_ACC_CREATE); }

/* ---- $CONNECT / $GET / $PUT / $DISCONNECT -------------------------------- */

uint32_t rms_remote_connect(void *rabp)
{
    struct RAB *rab = rabp;
    rab->rab$l_sts = RMS$_NORMAL;
    rab->rab$l_stv = 0;
    return RMS$_NORMAL;
}

uint32_t rms_remote_disconnect(void *rabp)
{
    struct RAB *rab = rabp;
    rab->rab$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
}

static uint32_t rab_ret(struct RAB *rab, uint32_t sts, uint32_t stv)
{
    rab->rab$l_sts = sts;
    rab->rab$l_stv = stv;
    return sts;
}

uint32_t rms_remote_get(void *rabp)
{
    struct RAB *rab = rabp;
    struct rr_state *st = st_by_fab(rab->rab$l_fab);
    if (!st || st->kind != RR_FILE || st->mode != RR_GET) return rab_ret(rab, RMS$_FAC, 0);
    if (st->eof) return rab_ret(rab, RMS$_EOF, 0);
    if (!st->xfer) {
        if (rr_simple(&st->link, DNET_DAP_CONTROL, DNET_DAP_CTL_GET) < 0)
            return rab_ret(rab, RMS$_RER, SS$_ABORT);
        st->xfer = 1;
    }
    struct dnet_dap_msg m;
    if (rr_recv(&st->link, &m) < 0) return rab_ret(rab, RMS$_RER, SS$_ABORT);
    if (m.op == DNET_DAP_DATA) {
        size_t n = m.u.data.reclen <= sizeof m.u.data.rec ? m.u.data.reclen : sizeof m.u.data.rec;
        const uint8_t *rec = m.u.data.rec;
        if (st->attr.rfm == DNET_DAP_RFM_VFC) {      /* the fixed control area */
            size_t skip = st->attr.fsz;
            if (skip > n) skip = n;
            rec += skip; n -= skip;
        }
        rab->rab$l_rbf = rab->rab$l_ubf;
        if (!rab->rab$l_ubf && n) return rab_ret(rab, RMS$_RTB, 0);
        size_t c = n <= rab->rab$w_usz ? n : rab->rab$w_usz;
        if (c) memcpy(rab->rab$l_ubf, rec, c);
        rab->rab$w_rsz = (uint16_t)c;
        if (c < n) { rab->rab$w_rsz = (uint16_t)n; return rab_ret(rab, RMS$_RTB, (uint32_t)n); }
        return rab_ret(rab, RMS$_NORMAL, 0);
    }
    if (m.op == DNET_DAP_STATUS) {
        uint16_t sc = m.u.status.stscode;
        if (DNET_DAP_MAC(sc) == DNET_DAP_MAC_XFER && DNET_DAP_MIC(sc) == DNET_DAP_MIC_EOF) {
            st->eof = 1;
            rab->rab$w_rsz = 0;
            return rab_ret(rab, RMS$_EOF, 0);
        }
        uint64_t sv = m.u.status.have_stv ? m.u.status.stv : 0;
        st->eof = 1;                                 /* the transfer is over */
        return rab_ret(rab, dap_rms(sc, sv, RMS$_RER), sv <= 0xffffffffu ? (uint32_t)sv : 0);
    }
    st->eof = 1;
    return rab_ret(rab, RMS$_RER, SS$_ABORT);
}

uint32_t rms_remote_put(void *rabp)
{
    struct RAB *rab = rabp;
    struct rr_state *st = st_by_fab(rab->rab$l_fab);
    if (!st || st->kind != RR_FILE || st->mode != RR_PUT) return rab_ret(rab, RMS$_FAC, 0);
    if (rab->rab$w_rsz > DNET_DAP_MAX_REC || (rab->rab$w_rsz && !rab->rab$l_rbf))
        return rab_ret(rab, RMS$_RTB, 0);
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_DATA;
    m.u.data.reclen = rab->rab$w_rsz;
    if (rab->rab$w_rsz) memcpy(m.u.data.rec, rab->rab$l_rbf, rab->rab$w_rsz);
    if (rr_send(&st->link, &m) < 0) return rab_ret(rab, RMS$_WER, SS$_ABORT);
    return rab_ret(rab, RMS$_NORMAL, 0);
}

/* ---- $CLOSE ------------------------------------------------------------------ */

uint32_t rms_remote_close(void *fabp)
{
    struct FAB *fab = fabp;
    struct rr_state *st = st_by_fab(fab);
    if (!st) { fab->fab$l_sts = RMS$_IFI; return RMS$_IFI; }
    if (st->kind != RR_FILE) {                       /* a $PARSE / $SEARCH context */
        st_free(st);
        fab->fab$l_sts = RMS$_NORMAL;
        return RMS$_NORMAL;
    }
    uint32_t sts = RMS$_NORMAL, stv = 0;
    if (st->mode == RR_GET && st->xfer && !st->eof) {
        /* Closed before the end: the remote is still sending. Take the rest
         * of the stream to its STATUS (bounded by what the remote sends). */
        struct dnet_dap_msg m;
        for (;;) {
            if (rr_recv(&st->link, &m) < 0) { sts = RMS$_RER; stv = SS$_ABORT; break; }
            if (m.op == DNET_DAP_DATA) continue;
            if (m.op == DNET_DAP_STATUS) { st->eof = 1; break; }
            sts = RMS$_RER; stv = SS$_ABORT;
            break;
        }
    }
    if (sts & 1) {
        if (st->mode == RR_GET && st->xfer)
            sts = complete(&st->link, DNET_DAP_CMP_EOS, RMS$_RER, &stv);
        if (sts & 1)
            sts = complete(&st->link, DNET_DAP_CMP_CLOSE,
                           st->mode == RR_PUT ? RMS$_WER : RMS$_RER, &stv);
    }
    st_free(st);
    fab->fab$l_sts = sts;
    fab->fab$l_stv = (sts & 1) ? 0 : stv;
    return sts;
}

/* ---- rms_file_attr of a remote file ----------------------------------------- */

uint32_t rms_remote_file_attr(const char *spec, struct rms_fileattr *out)
{
    if (!spec || !out) return RMS$_SYN;
    memset(out, 0, sizeof *out);
    struct rr_state st;
    memset(&st, 0, sizeof st);
    if (spec_split(spec, strlen(spec), &st.sp) != 0 || is_wild(st.sp.file)) {
        memset(&st, 0, sizeof st);
        return RMS$_SYN;
    }
    uint32_t sts = 0, stv = 0;
    uint32_t lst;
    if (link_open(&st.sp, &st.link, &lst) < 0) { memset(&st, 0, sizeof st); return RMS_REMOTE_FND; }
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ACCESS;
    m.u.access.accfunc = DNET_DAP_ACC_OPEN;
    snprintf(m.u.access.filespec, sizeof m.u.access.filespec, "%s", st.sp.file);
    m.u.access.have_fac = 1;     m.u.access.fac = DNET_DAP_FB_GET;
    m.u.access.have_shr = 1;     m.u.access.shr = DNET_DAP_FB_GET;
    m.u.access.have_display = 1; m.u.access.display = DNET_DAP_DSP_MAIN;
    uint16_t sc = 0; uint64_t sv = 0;
    int r = rr_send(&st.link, &m) < 0 ? -1 : await_ack(&st.link, &st.attr, &sc, &sv);
    if (r == 0) {
        sts = complete(&st.link, DNET_DAP_CMP_CLOSE, RMS$_ACC, &stv);
    } else {
        sts = r > 0 ? dap_rms(sc, sv, RMS$_ACC) : RMS$_ACC;
    }
    link_close(&st.link);
    if ((sts & 1) && st.attr.have) {
        out->org = st.attr.org;
        out->rfm = st.attr.rfm;
        out->rat = st.attr.rat;
        out->mrs = st.attr.mrs;
        out->lrl = st.attr.lrl;
        out->vfcsize = st.attr.rfm == DNET_DAP_RFM_VFC ? st.attr.fsz : 0;
        out->hiblk = st.attr.hbk;
        out->efblk = st.attr.ebk;
        out->ffbyte = st.attr.ffb;
    } else if (sts & 1) {
        sts = RMS$_ACC;
    }
    memset(&st, 0, sizeof st);
    return sts;
}
