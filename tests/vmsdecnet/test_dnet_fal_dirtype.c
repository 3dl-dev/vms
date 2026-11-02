/*
 * test_dnet_fal_dirtype.c - `$ DIRECTORY node::` and `$ TYPE node::` (rd
 * vms-6a4), the OVMX FAL CLIENT against the exact DAP replies a real OpenVMS
 * VAX V7.3 FAL sent, with its output compared to what the VAX printed.
 *
 *   argv[1] tests/lab/captures/decnet-fal-verbs-20261008/vax-to-vax-sys-login/
 *           falverbs-v2v-wire.txt     (VAX1 -> VAX2's FAL, account DNTEST)
 *   argv[2] .../vax-to-vax-sys-login/falverbs-v2v-vax1-console.txt
 *   argv[3] .../decnet-fal-verbs-20261008/falverbs-wire.txt (SYSTEM, DNTEST)
 *   argv[4] .../decnet-fal-verbs-20261008/vax1-console.txt
 *
 * Each command runs dnet_fal_directory / dnet_fal_type over a scripted
 * transport that answers with the VAX FAL's own segments for that command's
 * links, in order, and the text written must equal the block the VAX's
 * console printed under that command, byte for byte. Missing capture = FAIL.
 *
 * NORMALISATION (only this): both captures are VAX<->VAX at DAP 7.2, so every
 * segment after the CONFIGURATION carries a 2-byte trailer DAP 5.6 does not
 * define; it is stripped (as test_dnet_fal_server.c does). With OVMX's SYSCAP a
 * VAX sends none (live-bracket/README.md).
 *
 * Then the peer-receive bound: the NAME / DATA parsing path is fed mutated and
 * random FAL replies (built with ASan/UBSan where the toolchain has them), and
 * a FAL that sends more names than DNET_FAL_DIR_MAXFILES.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnet_fal.h"
#include "dnet_fal_dirtype.h"
#include "dnet_dap.h"
#include "dnet_cterm.h"
#include "ssdef.h"
#include "rmsdef.h"

static int g_pass, g_fail;
#define CHECK(c, msg) do { if (c) { g_pass++; printf("  ok: %s\n", msg); } \
                           else { g_fail++; printf("  FAIL: %s\n", msg); } } while (0)

/* ---------------- the capture (same nspdump reader as the server test) ---- */
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

/* ---------------- a linker that replays the VAX FAL's links -------------- */
struct replay_link {
    struct dnet_dap_transport t;
    const struct olink *L;
    int pos;
    char sent_spec[DNET_DAP_MAX_SPEC + 1];   /* the first ACCESS filespec sent */
};
struct replay {
    const struct olink *links[4]; int nlinks, next;
    uint32_t refuse;                          /* !0: every open fails with it */
    struct replay_link rl[4];
};

static int r_send(void *c, const uint8_t *seg, size_t len)
{
    struct replay_link *r = c;
    size_t off = 0;
    while (off < len) {
        struct dnet_dap_msg m; size_t used = 0;
        if (dnet_dap_decode(seg + off, len - off, &m, &used) != DNET_DAP_OK || !used) break;
        if (m.op == DNET_DAP_ACCESS && !r->sent_spec[0])
            snprintf(r->sent_spec, sizeof r->sent_spec, "%s", m.u.access.filespec);
        off += used;
    }
    return 0;
}
static int r_recv(void *c, uint8_t *buf, size_t cap, size_t *len)
{
    struct replay_link *r = c;
    if (r->pos >= r->L->nsrv) return -1;
    const struct oseg *o = &r->L->srv[r->pos++];
    if (o->n > cap) return -1;
    memcpy(buf, o->b, o->n);
    *len = o->n;
    return 0;
}
static struct dnet_dap_transport *r_open(void *v, uint32_t *st)
{
    struct replay *x = v;
    if (x->refuse) { *st = x->refuse; return NULL; }
    if (x->next >= x->nlinks) { *st = SS$_ABORT; return NULL; }
    struct replay_link *r = &x->rl[x->next];
    memset(r, 0, sizeof *r);
    r->L = x->links[x->next++];
    r->t.send = r_send; r->t.recv = r_recv; r->t.ctx = r;
    *st = SS$_NORMAL;
    return &r->t;
}
static void r_close(void *v, struct dnet_dap_transport *t) { (void)v; (void)t; }

/* Run one command against `nl` captured links; return what it printed. */
static char *run_cmd(int type, const char *pfx, const char *spec, int l0, int nl,
                     uint32_t refuse, uint32_t *st, struct replay *x)
{
    memset(x, 0, sizeof *x);
    for (int i = 0; i < nl; i++) x->links[i] = &g_links[l0 + i];
    x->nlinks = nl;
    x->refuse = refuse;
    struct dnet_fal_linker lk = { r_open, r_close, x };
    char *buf = NULL; size_t bl = 0;
    FILE *out = open_memstream(&buf, &bl);
    if (!out) return NULL;
    *st = type ? dnet_fal_type(out, &lk, pfx, spec) : dnet_fal_directory(out, &lk, pfx, spec);
    fclose(out);
    return buf;
}

static void show_diff(const char *want, const char *got)
{
    printf("    --- VAX printed ---\n%s    --- OVMX printed ---\n%s    ---\n",
           want ? want : "(no block)\n", got ? got : "(nothing)\n");
}

static void oracle_case(const char *what, const char *console, const char *cmd,
                        int type, const char *pfx, const char *spec, int l0, int nl,
                        uint32_t refuse, const char *want_spec)
{
    struct replay x;
    uint32_t st = 0;
    char *want = console_block(console, cmd);
    char *got = run_cmd(type, pfx, spec, l0, nl, refuse, &st, &x);
    int ok = want && got && !strcmp(want, got);
    if (!ok) show_diff(want, got);
    CHECK(ok, what);
    if (want_spec) {
        char m[300];
        snprintf(m, sizeof m, "%s: the DIRECTORY LIST names %s, as the VAX's did", what, want_spec);
        CHECK(!strcmp(x.rl[0].sent_spec, want_spec), m);
    }
    free(want); free(got);
}

/* ---------------- 1. units -------------------------------------------------- */
static void units(void)
{
    char o[300];
    dnet_fal_default_spec("SYS$LOGIN:BRK*.TXT", DNET_FAL_VERB_DIRECTORY, o, sizeof o);
    CHECK(!strcmp(o, "SYS$LOGIN:BRK*.TXT;*"), "DIRECTORY default: a missing version is ;*");
    dnet_fal_default_spec("[SYSMGR]", DNET_FAL_VERB_DIRECTORY, o, sizeof o);
    CHECK(!strcmp(o, "[SYSMGR]*.*;*"), "DIRECTORY default: an empty name is *.*;*");
    dnet_fal_default_spec("LOGIN", DNET_FAL_VERB_DIRECTORY, o, sizeof o);
    CHECK(!strcmp(o, "LOGIN.*;*"), "DIRECTORY default: a missing type is .*");
    dnet_fal_default_spec("A.B;3", DNET_FAL_VERB_DIRECTORY, o, sizeof o);
    CHECK(!strcmp(o, "A.B;3"), "DIRECTORY default: an explicit version stays");
    dnet_fal_default_spec("SYS$LOGIN:BRK*.TXT", DNET_FAL_VERB_TYPE, o, sizeof o);
    CHECK(!strcmp(o, "SYS$LOGIN:BRK*.TXT;"), "TYPE default: a missing version is ;");
    char big[400]; memset(big, 'A', sizeof big - 1); big[sizeof big - 1] = '\0';
    CHECK(dnet_fal_default_spec(big, DNET_FAL_VERB_TYPE, o, sizeof o) == -1 && o[0] == '\0',
          "an over-long spec is refused, not clipped");

    dnet_fal_display_prefix("VAX2", "SYSTEM", 1, 1, o, sizeof o);
    CHECK(!strcmp(o, "VAX2\"SYSTEM password\"::"), "prefix: the password is printed as the word password");
    dnet_fal_display_prefix("VAX2", "SYSTEM", 1, 0, o, sizeof o);
    CHECK(!strcmp(o, "VAX2\"SYSTEM\"::"), "prefix: a proxy user alone");
    dnet_fal_display_prefix("1.43", "", 0, 0, o, sizeof o);
    CHECK(!strcmp(o, "1.43::"), "prefix: no access string");

    CHECK(dnet_fal_dap_rms(0x4055, 0x24, SS$_ABORT) == RMS$_PRV, "DAP PRV (STV NOPRIV) reports RMS$_PRV");
    CHECK(dnet_fal_dap_rms(0x4032, 0x910, SS$_ABORT) == RMS$_FNF, "DAP FNF reports RMS$_FNF");
    CHECK(dnet_fal_dap_rms(0x405f, 0, SS$_ABORT) == SS$_ABORT, "an unmapped MIC with no STV is the fallback");
}

/* ---------------- 3. peer-receive fuzz ------------------------------------ */
struct fz { uint8_t seg[8][DNET_FAL_SEG_MAX]; size_t n[8]; int nseg, pos; };
static int fz_send(void *c, const uint8_t *s, size_t l) { (void)c; (void)s; (void)l; return 0; }
static int fz_recv(void *c, uint8_t *buf, size_t cap, size_t *len)
{
    struct fz *f = c;
    if (f->pos >= f->nseg) return -1;
    size_t n = f->n[f->pos]; if (n > cap) n = cap;
    memcpy(buf, f->seg[f->pos++], n); *len = n;
    return 0;
}
struct fzlk { struct fz *f[2]; int next; struct dnet_dap_transport t[2]; };
static struct dnet_dap_transport *fz_open(void *v, uint32_t *st)
{
    struct fzlk *k = v;
    if (k->next >= 2) { *st = SS$_ABORT; return NULL; }
    struct dnet_dap_transport *t = &k->t[k->next];
    memset(t, 0, sizeof *t);
    t->send = fz_send; t->recv = fz_recv; t->ctx = k->f[k->next++];
    *st = SS$_NORMAL;
    return t;
}

static uint32_t rnd_state = 0x6a4u;
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
            for (size_t i = 0; i < add && f->n[s] < DNET_FAL_SEG_MAX; i++)
                f->seg[s][f->n[s]++] = (uint8_t)rnd();
            break; }
        }
    }
}

static void fuzz(void)
{
    static struct fz a, b;
    struct fzlk k;
    FILE *devnull = fopen("/dev/null", "w");
    if (!devnull) { CHECK(0, "fuzz: /dev/null opens"); return; }
    const struct olink *dir = &g_links[1], *tdl = &g_links[2], *topen = &g_links[3];
    int runs = 0;
    for (int it = 0; it < 20000; it++) {
        memset(&k, 0, sizeof k);
        fill_from(&a, dir); mutate(&a);
        k.f[0] = &a; k.f[1] = &b;
        struct dnet_fal_linker lk = { fz_open, r_close, &k };
        (void)dnet_fal_directory(devnull, &lk, "VAX2::", "SYS$LOGIN:BRK*.TXT");
        memset(&k, 0, sizeof k);
        fill_from(&a, tdl); fill_from(&b, topen);
        if (rnd() & 1) mutate(&a); else mutate(&b);
        k.f[0] = &a; k.f[1] = &b;
        (void)dnet_fal_type(devnull, &lk, "VAX2::", "SYS$LOGIN:BRK*.TXT");
        runs += 2;
    }
    /* Pure noise after a good CONFIGURATION. */
    for (int it = 0; it < 5000; it++) {
        memset(&k, 0, sizeof k);
        fill_from(&a, dir);
        a.n[1] = rnd() % DNET_FAL_SEG_MAX;
        for (size_t i = 0; i < a.n[1]; i++) a.seg[1][i] = (uint8_t)rnd();
        k.f[0] = &a; k.f[1] = &b;
        struct dnet_fal_linker lk = { fz_open, r_close, &k };
        (void)dnet_fal_directory(devnull, &lk, "VAX2::", "X");
        runs++;
    }
    fclose(devnull);
    char m[120];
    snprintf(m, sizeof m, "fuzz: %d mutated / random FAL replies parsed without a fault", runs);
    CHECK(runs == 45000, m);
}

/* A FAL that names more files than the client keeps. */
struct flood { int sent_cfg, names, done; uint8_t cfg[64]; size_t cfgn; };
static int fl_recv(void *c, uint8_t *buf, size_t cap, size_t *len)
{
    struct flood *f = c;
    if (!f->sent_cfg) { memcpy(buf, f->cfg, f->cfgn); *len = f->cfgn; f->sent_cfg = 1; return 0; }
    if (f->done) return -1;
    size_t n = 0;
    while (n + 40 < cap && f->names < DNET_FAL_DIR_MAXFILES + 100) {
        char nm[32];
        int l = snprintf(nm, sizeof nm, "F%06d.TXT;1", f->names++);
        buf[n++] = 0x0f; buf[n++] = 0x02;            /* NAME, FLAGS.LENGTH */
        buf[n++] = (uint8_t)(2 + l);
        buf[n++] = 0x02;                              /* NAMETYPE file name */
        buf[n++] = (uint8_t)l;
        memcpy(buf + n, nm, (size_t)l); n += (size_t)l;
    }
    if (f->names >= DNET_FAL_DIR_MAXFILES + 100) {
        buf[n++] = 0x07; buf[n++] = 0x00; buf[n++] = 0x02;    /* ACCESS COMPLETE */
        f->done = 1;
    }
    *len = n;
    return 0;
}
static struct flood g_flood;
static struct dnet_dap_transport g_flt;
static struct dnet_dap_transport *fl_open(void *v, uint32_t *st)
{
    (void)v;
    memset(&g_flt, 0, sizeof g_flt);
    g_flt.send = fz_send; g_flt.recv = fl_recv; g_flt.ctx = &g_flood;
    *st = SS$_NORMAL;
    return &g_flt;
}
static void flood(void)
{
    memset(&g_flood, 0, sizeof g_flood);
    if (g_links[1].nsrv < 1) { CHECK(0, "flood: a captured CONFIGURATION"); return; }
    memcpy(g_flood.cfg, g_links[1].srv[0].b, g_links[1].srv[0].n);
    g_flood.cfgn = g_links[1].srv[0].n;
    struct dnet_fal_linker lk = { fl_open, r_close, NULL };
    char *buf = NULL; size_t bl = 0;
    FILE *out = open_memstream(&buf, &bl);
    uint32_t st = out ? dnet_fal_directory(out, &lk, "N::", "*") : 0;
    if (out) fclose(out);
    char want[64];
    snprintf(want, sizeof want, "\nTotal of %d files.\n", DNET_FAL_DIR_MAXFILES);
    CHECK(st == SS$_NORMAL && buf && strstr(buf, want),
          "a FAL naming more files than DNET_FAL_DIR_MAXFILES: the listing stops at the bound");
    free(buf);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        printf("usage: %s v2v-wire v2v-console falverbs-wire vax1-console\n", argv[0]);
        return 1;
    }
    printf("1. units\n");
    units();

    printf("2. the real VAX's FAL, its console the oracle\n");
    if (load_wire(argv[1]) != 0 || g_nlinks != 15) {
        CHECK(0, "the VAX<->VAX SYS$LOGIN capture loads with its 15 links");
    } else {
        oracle_case("DIRECTORY VAX2::SYS$LOGIN:BRK*.TXT prints the VAX's listing",
                    argv[2], "DIRECTORY VAX2::SYS$LOGIN:BRK*.TXT", 0, "VAX2::",
                    "SYS$LOGIN:BRK*.TXT", 1, 1, 0, "SYS$LOGIN:BRK*.TXT;*");
        oracle_case("TYPE VAX2::SYS$LOGIN:BRK*.TXT: two files with headers, BRKP OPENIN -RMS-E-PRV",
                    argv[2], "TYPE VAX2::SYS$LOGIN:BRK*.TXT", 1, "VAX2::",
                    "SYS$LOGIN:BRK*.TXT", 2, 2, 0, "SYS$LOGIN:BRK*.TXT;");
        oracle_case("DIRECTORY VAX2::SYS$LOGIN:NOSUCH.TXT is %DIRECT-W-NOFILES",
                    argv[2], "DIRECTORY VAX2::SYS$LOGIN:NOSUCH.TXT", 0, "VAX2::",
                    "SYS$LOGIN:NOSUCH.TXT", 13, 1, 0, "SYS$LOGIN:NOSUCH.TXT;*");
        fuzz();
        flood();
    }
    if (load_wire(argv[3]) != 0 || g_nlinks != 17) {
        CHECK(0, "the VAX1->VAX2 FAL verbs capture loads with its 17 links");
    } else {
        oracle_case("TYPE VAX2\"SYSTEM pw\"::...NOSUCH.TXT is SEARCHFAIL on the names the FAL sent",
                    argv[4], "### TYPE VAX2\"SYSTEM xxxxxx\"::SYS$SYSROOT:[SYSMGR]NOSUCH.TXT", 1,
                    "VAX2\"SYSTEM password\"::", "SYS$SYSROOT:[SYSMGR]NOSUCH.TXT", 1, 1, 0, NULL);
        oracle_case("TYPE VAX2::...PROT.TXT with the link refused: SEARCHFAIL, -RMS-E-FND, -SYSTEM-F-INVLOGIN",
                    argv[4], "### TYPE VAX2::SYS$SYSROOT:[SYSMGR]PROT.TXT", 1, "VAX2::",
                    "SYS$SYSROOT:[SYSMGR]PROT.TXT", 2, 0, SS$_INVLOGIN, NULL);
        oracle_case("TYPE VAX2\"DNTEST pw\"::...PROT.TXT: OPENIN on the resultant, -RMS-E-FNF",
                    argv[4], "### TYPE VAX2\"DNTEST xxxxxxxxxx\"::SYS$SYSROOT:[SYSMGR]PROT.TXT", 1,
                    "VAX2\"DNTEST password\"::", "SYS$SYSROOT:[SYSMGR]PROT.TXT", 10, 2, 0, NULL);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
