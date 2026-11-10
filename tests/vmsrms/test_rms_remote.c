/*
 * test_rms_remote.c - RMS remote file access (rd vms-0a7 S1) against the
 * exact DAP replies a real OpenVMS VAX V7.3 FAL sent, with DCL's DIRECTORY /
 * TYPE output compared to what the VAX printed. Ported from
 * tests/vmsdecnet/test_dnet_fal_dirtype.c (rd vms-6a4): the same captures,
 * consoles and cases, now through $PARSE / $SEARCH / $OPEN / $GET / $CLOSE.
 *
 *   argv[1] tests/lab/captures/decnet-fal-verbs-20261008/vax-to-vax-sys-login/
 *           falverbs-v2v-wire.txt     (VAX1 -> VAX2's FAL, account DNTEST)
 *   argv[2] .../vax-to-vax-sys-login/falverbs-v2v-vax1-console.txt
 *   argv[3] .../decnet-fal-verbs-20261008/falverbs-wire.txt (SYSTEM, DNTEST)
 *   argv[4] .../decnet-fal-verbs-20261008/vax1-console.txt
 *
 * The logical link is RMS's link seam (rms_remote_set_link_ops) answering
 * with the VAX FAL's own segments for that command's links, in order. Where
 * the VAX client kept one link open for several files (each access opening
 * with its own CONFIGURATION exchange) and RMS opens one link per $OPEN, the
 * next link continues the same captured server stream. Missing capture = FAIL.
 *
 * NORMALISATION (only this): both captures are VAX<->VAX at DAP 7.2, so every
 * segment after the CONFIGURATION carries a 2-byte trailer DAP 5.6 does not
 * define; it is stripped (as test_dnet_fal_server.c does).
 *
 * Then the peer-receive bound: the NAME / ATTRIBUTES / DATA receive path is
 * fed mutated and random FAL replies (built with ASan/UBSan where the
 * toolchain has them; the RMS remote module and the codec are compiled into
 * this test so the sanitizer instruments them), and a FAL that sends more
 * names than RMS_REMOTE_MAXFILES.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "rms/rms.h"
#include "rms_remote.h"
#include "dcl/dcl_remote.h"
#include "dnet_dap.h"
#include "ssdef.h"
#include "rmsdef.h"

static int g_pass, g_fail;
#define CHECK(c, msg) do { if (c) { g_pass++; printf("  ok: %s\n", msg); } \
                           else { g_fail++; printf("  FAIL: %s\n", msg); } } while (0)

/* ---------------- the capture (the nspdump reader of the FAL tests) ------ */
#define MAXLINK 32
#define MAXLSEG 32
struct oseg { uint8_t b[1600]; size_t n; };
struct olink {
    unsigned id;
    struct oseg cli[MAXLSEG]; int ncli;
    struct oseg srv[MAXLSEG]; int nsrv;
};
static struct olink g_links[MAXLINK];
static int g_nlinks;

static struct olink *link_by_id(unsigned id)
{ for (int i = 0; i < g_nlinks; i++) if (g_links[i].id == id) return &g_links[i]; return NULL; }

static int load_wire(const char *path)
{
    g_nlinks = 0;
    FILE *f = fopen(path, "r");
    if (!f) { printf("  cannot open %s\n", path); return -1; }
    char line[8192];
    struct olink *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char dir[16], kind[16];
        double t;
        if (sscanf(line, "%lf %15s %15s", &t, dir, kind) != 3) continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        uint8_t b[1600]; size_t n = 0;
        for (char *p = colon + 1; *p && n < sizeof b; ) {
            while (*p == ' ') p++;
            unsigned v;
            if (p[0] == 'X' && p[1] == 'X') v = 'x';
            else if (sscanf(p, "%2x", &v) != 1) break;
            b[n++] = (uint8_t)v; p += 2;
        }
        int from_client = !strncmp(dir, "1.1->", 5);
        if (!strcmp(kind, "RCI:") && n >= 9 && g_nlinks < MAXLINK) {
            struct olink *L = &g_links[g_nlinks++];
            memset(L, 0, sizeof *L);
            L->id = b[3] | (b[4] << 8);
        } else if (!strcmp(kind, "CC:") && n >= 5) {
            cur = link_by_id(b[1] | (b[2] << 8));
        } else if (!strcmp(kind, "DATA") && cur) {
            int segno = 0;
            if (sscanf(line, "%*f %*s DATA seg%d", &segno) != 1) continue;
            if (segno >= 2) { if (n < 2) continue; n -= 2; }   /* DAP 7 trailer */
            int *cnt = from_client ? &cur->ncli : &cur->nsrv;
            if (*cnt >= MAXLSEG) continue;
            struct oseg *o = from_client ? &cur->cli[*cnt] : &cur->srv[*cnt];
            memcpy(o->b, b, n); o->n = n;
            (*cnt)++;
        }
    }
    fclose(f);
    return 0;
}

/* The block a console printed under `cmd`: every line after the first line
 * equal to `cmd`, up to the next DCL prompt line ("$ " / "$"). */
static char *console_block(const char *path, const char *cmd)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[1024];
    int in = 0;
    size_t cap = 8192, len = 0;
    char *out = calloc(1, cap);
    while (out && fgets(line, sizeof line, f)) {
        if (!in) {
            char c[1024];
            snprintf(c, sizeof c, "%s\n", cmd);
            if (!strcmp(line, c)) in = 1;
            continue;
        }
        if (!strcmp(line, "$ \n") || !strcmp(line, "$\n")) break;
        size_t l = strlen(line);
        if (len + l + 1 > cap) break;
        memcpy(out + len, line, l + 1);
        len += l;
    }
    fclose(f);
    if (out && !in) { free(out); out = NULL; }
    return out;
}


/* DCL writes on SYS$OUTPUT (stdout): capture it around one command. */
static int g_saved_fd = -1;
static FILE *g_cap;
static void cap_begin(const char *path)
{
    fflush(stdout);
    g_saved_fd = dup(1);
    g_cap = path ? fopen(path, "w") : tmpfile();
    if (g_cap) dup2(fileno(g_cap), 1);
}
static char *cap_end(void)
{
    fflush(stdout);
    if (g_saved_fd >= 0) { dup2(g_saved_fd, 1); close(g_saved_fd); g_saved_fd = -1; }
    if (!g_cap) return NULL;
    long n = ftell(g_cap);
    char *buf = NULL;
    if (n >= 0 && (buf = calloc(1, (size_t)n + 1)) != NULL) {
        rewind(g_cap);
        if (fread(buf, 1, (size_t)n, g_cap) != (size_t)n) { free(buf); buf = NULL; }
    }
    fclose(g_cap);
    g_cap = NULL;
    return buf;
}

/* ---------------- the link seam: a replay of the VAX FAL's links --------- */
struct replay {
    const struct olink *links[4]; int nlinks, next;
    int pos[4];                         /* server segments served, per link */
    uint32_t refuse;                    /* !0: every connect fails with it  */
    int opens;
    char sent_spec[DNET_DAP_MAX_SPEC + 1];   /* the first ACCESS filespec sent */
    char ncb[4][256];                   /* each connect's NCB               */
};
struct rlink { struct replay *x; int li; };
static struct replay g_rp;
static struct rlink g_rl[64];

static void *rp_open(void *ctx, const char *ncb, size_t len, uint32_t *st)
{
    struct replay *x = ctx;
    if (x->opens < 4) snprintf(x->ncb[x->opens], sizeof x->ncb[0], "%.*s", (int)len, ncb);
    if (x->refuse) { *st = x->refuse; return NULL; }
    if (x->nlinks == 0 || x->opens >= 64) { *st = SS$_ABORT; return NULL; }
    struct rlink *r = &g_rl[x->opens++];
    r->x = x;
    r->li = x->next < x->nlinks ? x->next++ : x->nlinks - 1;   /* else: the same stream */
    *st = SS$_NORMAL;
    return r;
}
static int rp_send(void *l, const uint8_t *seg, size_t len)
{
    struct rlink *r = l;
    size_t off = 0;
    while (off < len) {
        struct dnet_dap_msg m; size_t used = 0;
        if (dnet_dap_decode(seg + off, len - off, &m, &used) != DNET_DAP_OK || !used) break;
        if (m.op == DNET_DAP_ACCESS && !r->x->sent_spec[0])
            snprintf(r->x->sent_spec, sizeof r->x->sent_spec, "%s", m.u.access.filespec);
        off += used;
    }
    return 0;
}
static int rp_recv(void *l, uint8_t *buf, size_t cap, size_t *len)
{
    struct rlink *r = l;
    const struct olink *L = r->x->links[r->li];
    if (r->x->pos[r->li] >= L->nsrv) return -1;
    const struct oseg *o = &L->srv[r->x->pos[r->li]++];
    if (o->n > cap) return -1;
    memcpy(buf, o->b, o->n);
    *len = o->n;
    return 0;
}
static void rp_close(void *l) { (void)l; }
static const struct rms_remote_link_ops g_rp_ops = { rp_open, rp_send, rp_recv, rp_close, &g_rp };

static char *run_cmd(int type, const char *spec, int l0, int nl, uint32_t refuse, uint32_t *st)
{
    memset(&g_rp, 0, sizeof g_rp);
    for (int i = 0; i < nl; i++) g_rp.links[i] = &g_links[l0 + i];
    g_rp.nlinks = nl;
    g_rp.refuse = refuse;
    rms_remote_set_link_ops(&g_rp_ops);
    cap_begin(NULL);
    *st = type == 2 ? dcl_remote_directory_full(spec)
        : type ? dcl_remote_type(spec) : dcl_remote_directory(spec);
    char *buf = cap_end();
    rms_remote_set_link_ops(NULL);
    return buf;
}

static void oracle_case(const char *what, const char *console, const char *cmd,
                        int type, const char *spec, int l0, int nl, uint32_t refuse,
                        const char *want_spec)
{
    uint32_t st = 0;
    char *want = console_block(console, cmd);
    char *got = run_cmd(type, spec, l0, nl, refuse, &st);
    int ok = want && got && !strcmp(want, got);
    if (!ok)
        printf("    --- VAX printed ---\n%s    --- OVMX printed ---\n%s    ---\n",
               want ? want : "(no block)\n", got ? got : "(nothing)\n");
    CHECK(ok, what);
    if (want_spec) {
        char m[300];
        snprintf(m, sizeof m, "%s: the DIRECTORY LIST names %s, as the VAX's did", what, want_spec);
        CHECK(!strcmp(g_rp.sent_spec, want_spec), m);
    }
    free(want); free(got);
}

/* ---------------- 1. RMS units: the spec, the NCB, the password ---------- */
static uint64_t q8(const uint8_t b[8])
{ uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | b[i]; return v; }

/* rd vms-5b9: a DAP A-18 date to VMS binary time (the expected values are
 * independent: Python datetime, ticks since 17-NOV-1858 00:00). */
static void ascii_dates(void)
{
    static const struct { const char *s; uint64_t want; } ok[] = {
        { " 2-NOV-26 23:40:25", 0xbc4eabff3a7a80ull },
        { "02-NOV-26 23:40:25", 0xbc4eabff3a7a80ull },
        { " 2-NOV-2026 23:40:25.81", 0xbc4eabffb61320ull },
        { "29-FEB-00 00:00:00", 0x9e65d062794000ull },
        { "31-DEC-99 23:59:59", 0x9e37739b816980ull },
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        uint8_t b[8] = { 0 };
        int r = rms_remote_ascii_time(ok[i].s, strlen(ok[i].s), b);
        char what[96];
        snprintf(what, sizeof what, "A-18 date \"%s\" is VMS time %llx", ok[i].s, (unsigned long long)ok[i].want);
        CHECK(r == 1 && q8(b) == ok[i].want, what);
    }
    static const char *bad[] = { "", "30-FEB-26 00:00:00", "02-XYZ-26 00:00:00", "02-NOV-26 24:00:00",
                                 "02-NOV-26 23:40", "02-NOV-26 23:40:25x", "-NOV-26 23:40:25" };
    int all = 1;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t b[8];
        if (rms_remote_ascii_time(bad[i], strlen(bad[i]), b)) all = 0;
    }
    CHECK(all, "A-18 dates that are not dates (Feb 30, bad month, 24:00, short, trailing junk) are refused");
}

static void units(void)
{
    ascii_dates();
    /* $PARSE: the expanded string shows the password as the word. */
    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;
    char esa[256];
    const char *spec = "VAX2\"SYSTEM s3cr3t\"::SYS$LOGIN:BRK*.TXT;*";
    fab.fab$l_fna = (char *)spec; fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$l_nam = &nam;
    nam.nam$l_esa = esa; nam.nam$b_ess = sizeof esa - 1;
    uint32_t st = sys$parse(&fab, 0, 0);
    esa[nam.nam$b_esl] = '\0';
    CHECK(st == RMS$_NORMAL && !strcmp(esa, "VAX2\"SYSTEM password\"::SYS$LOGIN:BRK*.TXT;*"),
          "$PARSE of node\"user pw\"::spec: the expanded string carries the word password");
    CHECK((nam.nam$l_fnb & NAM$M_NODE) && (nam.nam$l_fnb & NAM$M_WILDCARD) &&
          nam.nam$b_node == strlen("VAX2\"SYSTEM password\"::") &&
          nam.nam$b_dev == strlen("SYS$LOGIN:") && nam.nam$b_name == 4 &&
          nam.nam$b_type == 4 && nam.nam$b_ver == 2,
          "$PARSE: NAM$M_NODE + wildcard, and the node/device/name/type/version fields");

    /* $SEARCH over a refused connect: the NCB carried the real password,
     * the result is RMS$_FND with the connect's failure as the STV. */
    memset(&g_rp, 0, sizeof g_rp);
    g_rp.refuse = SS$_INVLOGIN;
    rms_remote_set_link_ops(&g_rp_ops);
    st = sys$search(&fab, 0, 0);
    CHECK(st == RMS_REMOTE_FND && fab.fab$l_stv == SS$_INVLOGIN,
          "$SEARCH, connect refused: RMS$_FND, STV = the connect failure");
    CHECK(!strcmp(g_rp.ncb[0], "VAX2\"SYSTEM s3cr3t\"::\"17=\""),
          "the NCB is node\"user password\"::\"17=\" (FAL, object 17)");
    rms_search_end(&nam);

    /* A resultant name re-opened: "password" stands for the real one. */
    memset(&g_rp, 0, sizeof g_rp);
    g_rp.refuse = SS$_INVLOGIN;
    struct FAB of = cc$rms_fab;
    const char *rsa = "VAX2\"SYSTEM password\"::SYS$SYSDEVICE:[SYSMGR]BRK1.TXT;1";
    of.fab$l_fna = (char *)rsa; of.fab$b_fns = (uint8_t)strlen(rsa);
    st = sys$open(&of, 0, 0);
    CHECK(st == RMS_REMOTE_FND && !strcmp(g_rp.ncb[0], "VAX2\"SYSTEM s3cr3t\"::\"17=\""),
          "$OPEN of a resultant name: the password this image gave is the one sent");

    /* No access string; a proxy user alone. */
    memset(&g_rp, 0, sizeof g_rp);
    g_rp.refuse = SS$_NOSUCHNODE;
    const char *s2 = "1.43::X.TXT";
    of = cc$rms_fab; of.fab$l_fna = (char *)s2; of.fab$b_fns = (uint8_t)strlen(s2);
    (void)sys$open(&of, 0, 0);
    CHECK(!strcmp(g_rp.ncb[0], "1.43::\"17=\""), "no access string: the NCB is node::\"17=\"");
    /* A multi-hop spec and an unterminated access string are refused. */
    const char *bad[] = { "A::B::C.TXT", "A\"U P A X\"::C.TXT", "\"U\"::X" };
    int all = 1;
    for (int i = 0; i < 3; i++) {
        of = cc$rms_fab; of.fab$l_fna = (char *)bad[i]; of.fab$b_fns = (uint8_t)strlen(bad[i]);
        if (sys$open(&of, 0, 0) != RMS$_SYN) all = 0;
    }
    CHECK(all, "multi-hop, a four-field access string and a node-less spec: RMS$_SYN");
    /* A local spec never reaches the link. */
    memset(&g_rp, 0, sizeof g_rp);
    CHECK(!rms_remote_is_spec("SYS$LOGIN:A.TXT", 15) &&
          !rms_remote_is_spec("A\"x::y\"B", 8) && rms_remote_is_spec("N::", 3),
          "rms_remote_is_spec: \"::\" outside a quoted string only");
    rms_remote_set_link_ops(NULL);
}

/* ---------------- 3. peer-receive fuzz ------------------------------------ */
struct fz { uint8_t seg[8][2048]; size_t n[8]; int nseg, pos; };
static struct fz g_fa, g_fb;
static int g_fz_next;
static void *fz_open(void *c, const char *ncb, size_t l, uint32_t *st)
{
    (void)c; (void)ncb; (void)l;
    if (g_fz_next >= 2) { *st = SS$_ABORT; return NULL; }
    *st = SS$_NORMAL;
    return g_fz_next++ ? &g_fb : &g_fa;
}
static int fz_send(void *l, const uint8_t *s, size_t n) { (void)l; (void)s; (void)n; return 0; }
static int fz_recv(void *l, uint8_t *buf, size_t cap, size_t *len)
{
    struct fz *f = l;
    if (f->pos >= f->nseg) return -1;
    size_t n = f->n[f->pos]; if (n > cap) n = cap;
    memcpy(buf, f->seg[f->pos++], n); *len = n;
    return 0;
}
static void fz_close(void *l) { (void)l; }
static const struct rms_remote_link_ops g_fz_ops = { fz_open, fz_send, fz_recv, fz_close, NULL };

static uint32_t rnd_state = 0x0a7u;
static uint32_t rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }

static void fill_from(struct fz *f, const struct olink *L)
{
    memset(f, 0, sizeof *f);
    for (int i = 0; i < L->nsrv && i < 8; i++) {
        memcpy(f->seg[i], L->srv[i].b, L->srv[i].n);
        f->n[i] = L->srv[i].n;
        f->nseg++;
    }
}

static void mutate(struct fz *f)
{
    int k = (int)(rnd() % 4) + 1;
    while (k--) {
        int s = (int)(rnd() % (unsigned)f->nseg);
        switch (rnd() % 4) {
        case 0: if (f->n[s]) f->seg[s][rnd() % f->n[s]] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: if (f->n[s]) f->seg[s][rnd() % f->n[s]] = (uint8_t)rnd(); break;
        case 2: f->n[s] = f->n[s] ? rnd() % f->n[s] : 0; break;          /* truncate */
        case 3: {                                                          /* extend   */
            size_t add = rnd() % 64;
            for (size_t i = 0; i < add && f->n[s] < sizeof f->seg[0]; i++)
                f->seg[s][f->n[s]++] = (uint8_t)rnd();
            break; }
        }
    }
}

static void fuzz(void)
{
    rms_remote_set_link_ops(&g_fz_ops);
    cap_begin("/dev/null");
    const struct olink *full = &g_links[0], *dir = &g_links[1], *tdl = &g_links[2], *topen = &g_links[3];
    int runs = 0;
    for (int it = 0; it < 20000; it++) {
        g_fz_next = 0;
        fill_from(&g_fa, dir); mutate(&g_fa);
        (void)dcl_remote_directory("VAX2::SYS$LOGIN:BRK*.TXT");
        g_fz_next = 0;
        fill_from(&g_fa, tdl); fill_from(&g_fb, topen);
        if (rnd() & 1) mutate(&g_fa); else mutate(&g_fb);
        (void)dcl_remote_type("VAX2::SYS$LOGIN:BRK*.TXT");
        g_fz_next = 0;
        fill_from(&g_fa, full); mutate(&g_fa);       /* DIRECTORY/FULL: attributes, dates, protection */
        (void)dcl_remote_directory_full("VAX2::SYS$LOGIN:BRK*.TXT");
        runs += 3;
    }
    /* Pure noise after a good CONFIGURATION. */
    for (int it = 0; it < 5000; it++) {
        g_fz_next = 0;
        fill_from(&g_fa, dir);
        g_fa.n[1] = rnd() % sizeof g_fa.seg[1];
        for (size_t i = 0; i < g_fa.n[1]; i++) g_fa.seg[1][i] = (uint8_t)rnd();
        (void)dcl_remote_directory("VAX2::X");
        runs++;
    }
    free(cap_end());
    rms_remote_set_link_ops(NULL);
    char m[120];
    snprintf(m, sizeof m, "fuzz: %d mutated / random FAL replies parsed without a fault", runs);
    CHECK(runs == 65000, m);
}

/* A FAL that names more files than RMS keeps. */
struct flood { int sent_cfg, names, done; uint8_t cfg[64]; size_t cfgn; };
static struct flood g_flood;
static void *fl_open(void *c, const char *ncb, size_t l, uint32_t *st)
{ (void)c; (void)ncb; (void)l; *st = SS$_NORMAL; return &g_flood; }
static int fl_recv(void *c, uint8_t *buf, size_t cap, size_t *len)
{
    struct flood *f = c;
    if (!f->sent_cfg) { memcpy(buf, f->cfg, f->cfgn); *len = f->cfgn; f->sent_cfg = 1; return 0; }
    if (f->done) return -1;
    size_t n = 0;
    while (n + 40 < cap && f->names < RMS_REMOTE_MAXFILES + 100) {
        char nm[32];
        int l = snprintf(nm, sizeof nm, "F%06d.TXT;1", f->names++);
        buf[n++] = 0x0f; buf[n++] = 0x02;            /* NAME, FLAGS.LENGTH */
        buf[n++] = (uint8_t)(2 + l);
        buf[n++] = 0x02;                              /* NAMETYPE file name */
        buf[n++] = (uint8_t)l;
        memcpy(buf + n, nm, (size_t)l); n += (size_t)l;
    }
    if (f->names >= RMS_REMOTE_MAXFILES + 100) {
        buf[n++] = 0x07; buf[n++] = 0x00; buf[n++] = 0x02;    /* ACCESS COMPLETE */
        f->done = 1;
    }
    *len = n;
    return 0;
}
static const struct rms_remote_link_ops g_fl_ops = { fl_open, fz_send, fl_recv, fz_close, NULL };

static void flood(void)
{
    memset(&g_flood, 0, sizeof g_flood);
    if (g_links[1].nsrv < 1) { CHECK(0, "flood: a captured CONFIGURATION"); return; }
    memcpy(g_flood.cfg, g_links[1].srv[0].b, g_links[1].srv[0].n);
    g_flood.cfgn = g_links[1].srv[0].n;
    rms_remote_set_link_ops(&g_fl_ops);
    cap_begin(NULL);
    uint32_t st = dcl_remote_directory("N::*");
    char *buf = cap_end();
    rms_remote_set_link_ops(NULL);
    char want[64];
    snprintf(want, sizeof want, "\nTotal of %d files.\n", RMS_REMOTE_MAXFILES);
    CHECK(st == RMS$_NORMAL && buf && strstr(buf, want),
          "a FAL naming more files than RMS_REMOTE_MAXFILES: the listing stops at the bound");
    free(buf);
}


/* $STATUS: what DCL leaves after the command (the verb adds STS$M_INHIB_MSG
 * when the failure has been reported), against what the live VAX V7.3 left. */
static void status_case(const char *what, int type, const char *spec, int l0, int nl,
                        uint32_t refuse, uint32_t want)
{
    uint32_t st = 0;
    char *got = run_cmd(type, spec, l0, nl, refuse, &st);
    uint32_t dcl = (st & 1) ? 1u : (st | 0x10000000u);
    char m[300];
    snprintf(m, sizeof m, "%s: $STATUS %%X%08X (the VAX's %%X%08X)", what, dcl, want);
    CHECK(dcl == want, m);
    free(got);
}

/* ---------------- COPY/LOG: the block count -------------------------------- */
/* VAX2 is the real VAX FAL's captured replies (the TYPE links of the capture).
 * VAX3 is a destination FAL that accepts a PUT and, asked for the file's
 * attributes, answers with the ATTRIBUTES message the real VAX FAL sent for
 * BRK1.TXT (EBK 1, FFB 38, HBK 9: one block of nine allocated). */
struct mk { int open, src; uint8_t seg[16][1600]; size_t n[16]; int nq, pos; };
static struct mk g_mk[16];
static int g_mk_n;
static struct dnet_dap_msg g_brk1_attr;
static int g_have_brk1_attr;

static void mk_q(struct mk *k, const struct dnet_dap_msg *m)
{
    size_t n = 0;
    if (k->nq >= 16) return;
    if (dnet_dap_encode(m, 0, k->seg[k->nq], sizeof k->seg[0], &n) == DNET_DAP_OK) k->n[k->nq++] = n;
}
/* VAX4: a FAL serving one VFC print file (2-byte fixed control 01 8D, as
 * DCL OPEN/WRITE makes BIG.TXT on the VAX, S1 run 4) -- its records arrive in
 * DAP DATA with the fixed control in front, as on the wire (wire.txt run 3:
 * `00 01 8d 72 65 63 6f 72 64` = recnum 0, 01 8d, "record"). The DATA records
 * VAX3 is sent are kept, to check the control survives a COPY. */
#define VFC_NREC 3
static uint8_t g_sink[8][128]; static size_t g_sinkn[8]; static int g_nsink;
static void vfc_rec(int i, uint8_t *b, size_t *n)
{
    b[0] = 0x01; b[1] = 0x8d;
    *n = 2 + (size_t)sprintf((char *)b + 2, "record %04d of the S1 long copy", i + 1);
}
static void *cp_open(void *c, const char *ncb, size_t len, uint32_t *st)
{
    if (strstr(ncb, "VAX3") || strstr(ncb, "VAX4")) {
        if (g_mk_n >= 16) { *st = SS$_ABORT; return NULL; }
        struct mk *k = &g_mk[g_mk_n++];
        memset(k, 0, sizeof *k);
        k->open = 1;
        k->src = strstr(ncb, "VAX4") != NULL;
        *st = SS$_NORMAL;
        return k;
    }
    return rp_open(c, ncb, len, st);
}
static int is_mk(const void *l) { return l >= (void *)g_mk && l < (void *)(g_mk + 16); }
static int cp_send(void *l, const uint8_t *seg, size_t len)
{
    if (!is_mk(l)) return rp_send(l, seg, len);
    struct mk *k = l;
    struct dnet_dap_msg m, r;
    size_t used = 0;
    if (dnet_dap_decode(seg, len, &m, &used) != DNET_DAP_OK) return -1;
    memset(&r, 0, sizeof r);
    if (k->src && m.op == DNET_DAP_ACCESS && m.u.access.accfunc == DNET_DAP_ACC_DIRLIST) {
        static const struct { unsigned t; const char *s; } nm[] = {
            { DNET_DAP_NT_VOLUME, "SYS$SYSDEVICE:" }, { DNET_DAP_NT_DIRECTORY, "[SYSMGR]" },
            { DNET_DAP_NT_FILENAME, "BIG.TXT;1" } };
        for (unsigned i = 0; i < 3; i++) {
            memset(&r, 0, sizeof r);
            r.op = DNET_DAP_NAME; r.u.name.nametype = nm[i].t;
            snprintf(r.u.name.namespec, sizeof r.u.name.namespec, "%s", nm[i].s);
            mk_q(k, &r);
        }
        memset(&r, 0, sizeof r);
        r.op = DNET_DAP_ACCESS_COMPLETE; r.u.complete.cmpfunc = DNET_DAP_CMP_RESPONSE; mk_q(k, &r);
        return 0;
    }
    if (k->src && m.op == DNET_DAP_ACCESS && m.u.access.accfunc == DNET_DAP_ACC_OPEN) {
        r.op = DNET_DAP_ATTRIBUTES;
        r.u.attr.menu = (1u << DNET_DAP_ATT_ORG) | (1u << DNET_DAP_ATT_RFM) |
                        (1u << DNET_DAP_ATT_RAT) | (1u << DNET_DAP_ATT_FSZ);
        r.u.attr.org = DNET_DAP_ORG_SEQ; r.u.attr.rfm = DNET_DAP_RFM_VFC;
        r.u.attr.rat = DNET_DAP_RAT_PRN; r.u.attr.fsz = 2;
        mk_q(k, &r);
        memset(&r, 0, sizeof r); r.op = DNET_DAP_ACKNOWLEDGE; mk_q(k, &r);
        return 0;
    }
    if (k->src && m.op == DNET_DAP_CONTROL && m.u.control.ctlfunc == DNET_DAP_CTL_GET) {
        for (int i = 0; i < VFC_NREC; i++) {
            memset(&r, 0, sizeof r);
            r.op = DNET_DAP_DATA;
            size_t n = 0; vfc_rec(i, r.u.data.rec, &n); r.u.data.reclen = (uint16_t)n;
            mk_q(k, &r);
        }
        memset(&r, 0, sizeof r);
        r.op = DNET_DAP_STATUS;
        r.u.status.stscode = (uint16_t)((DNET_DAP_MAC_XFER << 12) | DNET_DAP_MIC_EOF);
        mk_q(k, &r);
        return 0;
    }
    if (!k->src && m.op == DNET_DAP_DATA && g_nsink < 8) {
        size_t n = m.u.data.reclen < sizeof g_sink[0] ? m.u.data.reclen : sizeof g_sink[0];
        memcpy(g_sink[g_nsink], m.u.data.rec, n); g_sinkn[g_nsink++] = n;
        return 0;
    }
    switch (m.op) {
    case DNET_DAP_CONFIG:
        if (k->nq < 16) { memcpy(k->seg[k->nq], g_links[1].srv[0].b, g_links[1].srv[0].n);
                         k->n[k->nq++] = g_links[1].srv[0].n; }
        break;
    case DNET_DAP_ACCESS:
        if (m.u.access.accfunc == DNET_DAP_ACC_OPEN && g_have_brk1_attr) mk_q(k, &g_brk1_attr);
        r.op = DNET_DAP_ACKNOWLEDGE; mk_q(k, &r);
        break;
    case DNET_DAP_CONTROL:
        if (m.u.control.ctlfunc == DNET_DAP_CTL_CONNECT) { r.op = DNET_DAP_ACKNOWLEDGE; mk_q(k, &r); }
        break;
    case DNET_DAP_ACCESS_COMPLETE:
        r.op = DNET_DAP_ACCESS_COMPLETE; r.u.complete.cmpfunc = DNET_DAP_CMP_RESPONSE; mk_q(k, &r);
        break;
    default: break;
    }
    return 0;
}
static int cp_recv(void *l, uint8_t *buf, size_t cap, size_t *len)
{
    if (!is_mk(l)) return rp_recv(l, buf, cap, len);
    struct mk *k = l;
    if (k->pos >= k->nq || k->n[k->pos] > cap) return -1;
    memcpy(buf, k->seg[k->pos], k->n[k->pos]);
    *len = k->n[k->pos++];
    return 0;
}
static void cp_close(void *l) { if (!is_mk(l)) rp_close(l); }
static const struct rms_remote_link_ops g_cp_ops = { cp_open, cp_send, cp_recv, cp_close, &g_rp };

static void copy_blocks(void)
{
    /* BRK1's ATTRIBUTES as the real VAX FAL sent it (the DIRECTORY/FULL link). */
    g_have_brk1_attr = 0;
    const struct olink *L0 = &g_links[0];
    for (int i = 0; i < L0->nsrv && !g_have_brk1_attr; i++) {
        size_t off = 0;
        while (off < L0->srv[i].n) {
            struct dnet_dap_msg m; size_t used = 0;
            if (dnet_dap_decode(L0->srv[i].b + off, L0->srv[i].n - off, &m, &used) != DNET_DAP_OK || !used) break;
            if (m.op == DNET_DAP_ATTRIBUTES) {
                g_brk1_attr = m;
                /* the VAX sets menu bit 21 (a later DAP's field); the codec does not encode it */
                g_brk1_attr.u.attr.menu &= (1ull << (DNET_DAP_ATT_LAST_KNOWN + 1)) - 1;
                g_have_brk1_attr = 1;
                break;
            }
            off += used;
        }
    }
    CHECK(g_have_brk1_attr && g_brk1_attr.u.attr.ebk == 1 && g_brk1_attr.u.attr.ffb != 0 &&
          g_brk1_attr.u.attr.hbk == 9, "BRK1's captured ATTRIBUTES: EBK 1, FFB set, HBK 9");

    memset(&g_rp, 0, sizeof g_rp);
    g_rp.links[0] = &g_links[2]; g_rp.links[1] = &g_links[3]; g_rp.nlinks = 2;
    g_mk_n = 0;
    rms_remote_set_link_ops(&g_cp_ops);
    cap_begin(NULL);
    uint32_t st = dcl_remote_copy("VAX2::SYS$LOGIN:BRK*.TXT", "VAX3::DEST.TXT", 1);
    char *buf = cap_end();
    rms_remote_set_link_ops(NULL);
    int n1 = 0;
    for (const char *p = buf; p && (p = strstr(p, "%COPY-S-COPIED, ")) != NULL; p++) {
        const char *e = strchr(p, '\n');
        if (e && e - p > 11 && !strncmp(e - 11, " (1 block)", 10)) n1++;
        else if (e && e - p > 10 && !strncmp(e - 10, " (1 block)", 10)) n1++;
    }
    if (n1 != 2) printf("    --- OVMX printed ---\n%s    ---\n", buf ? buf : "(nothing)\n");
    CHECK(buf && n1 == 2, "COPY/LOG of two remote files: each %COPY-S-COPIED line ends \" (1 block)\", as the VAX's");
    CHECK(buf && strstr(buf, "BRK1.TXT;1 copied to VAX3::DEST.TXT (1 block)\n") != NULL,
          "COPY/LOG: the full line is \"... copied to <output> (1 block)\"");
    (void)st;
    free(buf);
}

/* A VFC print file keeps its fixed control through COPY node:: -> node::, and
 * TYPE shows the text after it (S1 run 4: BIGRT.TXT came back with each
 * record's first two bytes eaten -- "cord 0001" -- because $GET dropped the
 * control and $PUT then sent the text where the FAL takes the control). */
static void vfc_cases(void)
{
    memset(&g_rp, 0, sizeof g_rp);
    g_mk_n = 0; g_nsink = 0;
    rms_remote_set_link_ops(&g_cp_ops);
    cap_begin(NULL);
    uint32_t st = dcl_remote_copy("VAX4::SYS$SYSDEVICE:[SYSMGR]BIG.TXT", "VAX3::BIGRT.TXT", 0);
    char *buf = cap_end();
    int same = (st & 1) && g_nsink == VFC_NREC;
    for (int i = 0; i < g_nsink && same; i++) {
        uint8_t want[128]; size_t wn = 0;
        vfc_rec(i, want, &wn);
        same = g_sinkn[i] == wn && memcmp(g_sink[i], want, wn) == 0;
    }
    if (!same) printf("    --- COPY printed ---\n%s    --- st %08X, %d records ---\n",
                      buf ? buf : "", (unsigned)st, g_nsink);
    CHECK(same, "COPY of a VFC print file node:: -> node:: sends each record with its fixed control (01 8D + text), byte-identical");
    free(buf);

    /* COPY node:: -> local writes the VAX's own VFC records: the control in
     * the fixed-control area (RAB$L_RHB), not in the data (rd vms-36aa: the
     * 290-block BIG.TXT was 297 on OVMX). Each record: count 33 (2 + 31),
     * 01 8D, the 31-byte text, a pad byte = 36 bytes; 3 records = 108. */
    char lspec[128], ldisk[140], lmeta[160];
    snprintf(lspec, sizeof lspec, "/tmp/test_rms_remote_vfc_%d.TXT", (int)getpid());
    snprintf(ldisk, sizeof ldisk, "%s;1", lspec);
    snprintf(lmeta, sizeof lmeta, "%s.rms_meta", ldisk);
    g_mk_n = 0;
    cap_begin(NULL);
    st = dcl_remote_copy("VAX4::SYS$SYSDEVICE:[SYSMGR]BIG.TXT", lspec, 0);
    buf = cap_end();
    struct stat sb;
    unsigned char raw[36];
    int fd = open(ldisk, O_RDONLY);
    int ondisk = (st & 1) && stat(ldisk, &sb) == 0 && sb.st_size == 3 * 36 && fd >= 0 &&
                 read(fd, raw, sizeof raw) == (ssize_t)sizeof raw && raw[0] == 33 && raw[1] == 0 &&
                 raw[2] == 0x01 && raw[3] == 0x8d && !memcmp(raw + 4, "record 0001 of", 14);
    if (fd >= 0) close(fd);
    if (!ondisk) printf("    --- COPY printed ---\n%s    --- st %08X ---\n", buf ? buf : "", (unsigned)st);
    CHECK(ondisk, "COPY node:: -> local of a VFC file: 36 bytes a record (count, 01 8D control, text, pad), as the VAX's");
    free(buf);

    /* ... and back: the local file's records, $GET with RHB and $PUT with
     * RHB to node:: (what COPY local -> node:: does), are the VAX's records. */
    g_mk_n = 0; g_nsink = 0;
    {
        struct FAB in = cc$rms_fab, out = cc$rms_fab;
        in.fab$l_fna = ldisk; in.fab$b_fns = (uint8_t)strlen(ldisk);
        in.fab$b_fac = FAB$M_GET; in.fab$b_rfm = FAB$C_VFC; in.fab$b_fsz = 2;
        char ospec[] = "VAX3::BIGRT.TXT";
        out.fab$l_fna = ospec; out.fab$b_fns = (uint8_t)strlen(ospec);
        out.fab$b_fac = FAB$M_PUT; out.fab$b_org = FAB$C_SEQ;
        out.fab$b_rfm = FAB$C_VFC; out.fab$b_fsz = 2; out.fab$b_rat = FAB$M_PRN;
        int ok = (sys$open(&in, 0, 0) & 1) && (sys$create(&out, 0, 0) & 1);
        struct RAB ir = cc$rms_rab, orb = cc$rms_rab;
        char ub[128], hb[2];
        ir.rab$l_fab = &in; ir.rab$l_ubf = ub; ir.rab$w_usz = sizeof ub; ir.rab$l_rhb = hb;
        orb.rab$l_fab = &out; orb.rab$l_rhb = hb;
        ok = ok && (sys$connect(&ir, 0, 0) & 1) && (sys$connect(&orb, 0, 0) & 1);
        while (ok && (sys$get(&ir, 0, 0) & 1)) {
            orb.rab$l_rbf = ir.rab$l_rbf; orb.rab$w_rsz = ir.rab$w_rsz;
            ok = (sys$put(&orb, 0, 0) & 1);
        }
        ok = ok && (sys$close(&out, 0, 0) & 1);
        (void)sys$close(&in, 0, 0);
        same = ok && g_nsink == VFC_NREC;
        for (int i = 0; i < g_nsink && same; i++) {
            uint8_t want[128]; size_t wn = 0;
            vfc_rec(i, want, &wn);
            same = g_sinkn[i] == wn && memcmp(g_sink[i], want, wn) == 0;
        }
        if (!same) printf("    --- ok %d, %d records ---\n", ok, g_nsink);
        CHECK(same, "the local VFC file -> node:: ($GET/$PUT with RHB) sends each record as the VAX sent it (01 8D + text)");
    }
    unlink(ldisk); unlink(lmeta);

    /* $GET with RHB: the control in RHB, the text alone in UBF. */
    g_mk_n = 0;
    {
        struct FAB f = cc$rms_fab;
        char spec[] = "VAX4::SYS$SYSDEVICE:[SYSMGR]BIG.TXT";
        f.fab$l_fna = spec; f.fab$b_fns = (uint8_t)strlen(spec);
        f.fab$b_fac = FAB$M_GET;
        int ok = (sys$open(&f, 0, 0) & 1) && f.fab$b_rfm == FAB$C_VFC && f.fab$b_fsz == 2;
        struct RAB r = cc$rms_rab;
        char ub[128], hb[2] = { 0, 0 };
        r.rab$l_fab = &f; r.rab$l_ubf = ub; r.rab$w_usz = sizeof ub; r.rab$l_rhb = hb;
        ok = ok && (sys$connect(&r, 0, 0) & 1) && (sys$get(&r, 0, 0) & 1) &&
             r.rab$w_rsz == 31 && !memcmp(ub, "record 0001 of the S1 long copy", 31) &&
             hb[0] == 0x01 && hb[1] == (char)0x8d;
        r.rab$l_rhb = NULL;
        ok = ok && (sys$get(&r, 0, 0) & 1) && r.rab$w_rsz == 31 && !memcmp(ub, "record 0002", 11);
        (void)sys$close(&f, 0, 0);
        CHECK(ok, "$GET node:: of a VFC record: RHB = 01 8D, UBF = the text (RSZ 31); RHB 0 = the text alone");
    }

    g_mk_n = 0;
    cap_begin(NULL);
    st = dcl_remote_type("VAX4::SYS$SYSDEVICE:[SYSMGR]BIG.TXT");
    buf = cap_end();
    rms_remote_set_link_ops(NULL);
    int ok = buf && strstr(buf, "record 0001 of the S1 long copy\nrecord 0002 of the S1 long copy\n") &&
             !strchr(buf, 0x01) && !strchr(buf, (char)0x8d);
    if (!ok) printf("    --- TYPE printed ---\n%s    ---\n", buf ? buf : "(nothing)\n");
    CHECK(ok, "TYPE of a remote VFC print file shows the text after the fixed control");
    free(buf);
}

/* DIRECTORY/FULL (the real VAX FAL's replies to a DIRLIST asking 0x839) is
 * the VAX's own listing, byte for byte; the VAX's request is what is sent. */
static void full_cases(const char *console)
{
    uint32_t st = 0;
    char *want = console_block(console, "DIRECTORY/FULL VAX2::SYS$LOGIN:BRK*.TXT");
    char *got = run_cmd(2, "VAX2::SYS$LOGIN:BRK*.TXT", 0, 1, 0, &st);
    int ok = want && got && !strcmp(want, got);
    if (!ok)
        printf("    --- VAX printed ---\n%s    --- OVMX printed ---\n%s    ---\n",
               want ? want : "(no block)\n", got ? got : "(nothing)\n");
    CHECK(ok, "DIRECTORY/FULL VAX2::SYS$LOGIN:BRK*.TXT prints the VAX's listing (dates, protection, sizes, PRV line, totals)");
    CHECK((st & 1) == 1, "DIRECTORY/FULL: success status");
    free(want); free(got);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        printf("usage: %s v2v-wire v2v-console falverbs-wire vax1-console\n", argv[0]);
        return 1;
    }
    printf("1. RMS units\n");
    units();

    printf("2. the real VAX's FAL, its console the oracle\n");
    if (load_wire(argv[1]) != 0 || g_nlinks != 15) {
        CHECK(0, "the VAX<->VAX SYS$LOGIN capture loads with its 15 links");
    } else {
        oracle_case("DIRECTORY VAX2::SYS$LOGIN:BRK*.TXT prints the VAX's listing",
                    argv[2], "DIRECTORY VAX2::SYS$LOGIN:BRK*.TXT", 0,
                    "VAX2::SYS$LOGIN:BRK*.TXT", 1, 1, 0, "SYS$LOGIN:BRK*.TXT;*");
        oracle_case("TYPE VAX2::SYS$LOGIN:BRK*.TXT: two files with headers, BRKP OPENIN -RMS-E-PRV",
                    argv[2], "TYPE VAX2::SYS$LOGIN:BRK*.TXT", 1,
                    "VAX2::SYS$LOGIN:BRK*.TXT", 2, 2, 0, "SYS$LOGIN:BRK*.TXT;");
        oracle_case("DIRECTORY VAX2::SYS$LOGIN:NOSUCH.TXT is %DIRECT-W-NOFILES",
                    argv[2], "DIRECTORY VAX2::SYS$LOGIN:NOSUCH.TXT", 0,
                    "VAX2::SYS$LOGIN:NOSUCH.TXT", 13, 1, 0, "SYS$LOGIN:NOSUCH.TXT;*");
        printf("2b. DIRECTORY/FULL, COPY/LOG blocks, $STATUS\n");
        full_cases(argv[2]);
        copy_blocks();
        vfc_cases();
        /* $STATUS, from tests/lab/captures/vms-0a7-s1-20261102/events.log (VAXC L-cases). */
        status_case("DIRECTORY ...NOSUCH.TXT", 0, "VAX2::SYS$LOGIN:NOSUCH.TXT", 13, 1, 0, 0x10018290u);
        status_case("TYPE BRK*.TXT (BRKP OPENIN -RMS-E-PRV)", 1, "VAX2::SYS$LOGIN:BRK*.TXT", 2, 2, 0, 0x10951098u);
        status_case("DIRECTORY, link refused INVLOGIN", 0, "VAX2::SYS$LOGIN:BRK*.TXT", 1, 0, SS$_INVLOGIN, 0x1001C02Au);
        status_case("TYPE, link refused INVLOGIN (SEARCHFAIL)", 1, "VAX2::SYS$LOGIN:BRK1.TXT", 1, 0, SS$_INVLOGIN, 0x10951238u);
        printf("3. peer-receive bound\n");
        fuzz();
        flood();
    }
    if (load_wire(argv[3]) != 0 || g_nlinks != 17) {
        CHECK(0, "the VAX1->VAX2 FAL verbs capture loads with its 17 links");
    } else {
        oracle_case("TYPE VAX2\"SYSTEM pw\"::...NOSUCH.TXT is SEARCHFAIL on the names the FAL sent",
                    argv[4], "### TYPE VAX2\"SYSTEM xxxxxx\"::SYS$SYSROOT:[SYSMGR]NOSUCH.TXT", 1,
                    "VAX2\"SYSTEM xxxxxx\"::SYS$SYSROOT:[SYSMGR]NOSUCH.TXT", 1, 1, 0, NULL);
        oracle_case("TYPE VAX2::...PROT.TXT with the link refused: SEARCHFAIL, -RMS-E-FND, -SYSTEM-F-INVLOGIN",
                    argv[4], "### TYPE VAX2::SYS$SYSROOT:[SYSMGR]PROT.TXT", 1,
                    "VAX2::SYS$SYSROOT:[SYSMGR]PROT.TXT", 2, 0, SS$_INVLOGIN, NULL);
        oracle_case("TYPE VAX2\"DNTEST pw\"::...PROT.TXT: OPENIN on the resultant, -RMS-E-FNF",
                    argv[4], "### TYPE VAX2\"DNTEST xxxxxxxxxx\"::SYS$SYSROOT:[SYSMGR]PROT.TXT", 1,
                    "VAX2\"DNTEST xxxxxxxxxx\"::SYS$SYSROOT:[SYSMGR]PROT.TXT", 10, 2, 0, NULL);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
