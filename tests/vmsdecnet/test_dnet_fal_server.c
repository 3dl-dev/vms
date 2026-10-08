/*
 * test_dnet_fal_server.c - the OVMX FAL SERVER (dnet_fal_server_run) driven by
 * the EXACT DAP segments real OpenVMS VAX V7.3 clients sent.
 *
 *  (1)-(3) A VAX COPY into and out of OVMX (rd vms-d85 inbound,
 *          tests/lab/captures/decnet-fal-inbound-20261004/).
 *  (4)     rd vms-277a: every DIRECTORY/FULL, TYPE, DELETE and RENAME session
 *          of tests/lab/captures/decnet-fal-verbs-20261008/falverbs-wire.txt
 *          (VAX1 client -> VAX2's own FAL), read from the committed capture
 *          at run time. Each link's CLIENT segments are replayed through the
 *          OVMX server and its replies are compared, segment for segment and
 *          message for message, with what the VAX FAL answered on that link.
 *
 * File I/O is a stub standing in for RMS over the ACP (the shipped hooks are
 * dnet_fal_search.c; the real ACP verdicts are proven on the booted executive
 * by decnetd --fal-proc-accept-test). The stub's "ACP" refuses DNTEST exactly
 * where the real VAX refused it, so what this pins is the PROTOCOL: given that
 * refusal, the STATUS bytes OVMX sends are the VAX FAL's.
 *
 * NORMALISATION (stated once, applied below, nothing else is relaxed):
 *   - VAX<->VAX is DAP 7.2 and every segment after the CONFIGURATION ends with
 *     2 bytes the DAP 5.6 spec does not define; with OVMX advertising 5.6 the
 *     VAX sends none (decnet-fal-dap-20261004/README.md sec. 4). They are
 *     stripped from both directions before replay / comparison.
 *   - DAP 7-only content OVMX does not speak: the type-18 message, ATTRIBUTES
 *     menu bit 21, DATE AND TIME binary-time bits 7/8 (OVMX sends the 5.6 A-18
 *     CDT/RDT instead, checked against the VAX console's own dates).
 *   - ATTRIBUTES fields OVMX does not read from the header (FOP, DEV, SBN) are
 *     omitted, never invented (INV-6); every field it sends must equal the
 *     VAX's.
 *   - The CONFIGURATION reply is OVMX's own.
 *   - The DIRECTORY LIST per-file ACKNOWLEDGE is DAP 7's: OVMX advertises
 *     DAP 7.2 (rd vms-b2f) and sends it to a DAP 7 client byte for byte; a
 *     DAP 5.6 FAL must not (BUG_DAP 0001A006, live bracket, test 5).
 * Everything else -- every NAME, ACK, ACCESS COMPLETE, SUMMARY, PROTECTION and
 * every STATUS -- must be byte-identical, FLAGS/LENGTH framing included.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "dnet_fal.h"
#include "dnet_cterm.h"
#include "ssdef.h"
#include "rmsdef.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s\n", m); } } while (0)

/* ======================= stub RMS (stands in for the ACP) ================= */
static char g_put_spec[256], g_put_data[4096]; static size_t g_put_len;
static int g_put_closed;
static int g_wopen_no_rsa;   /* 1 = RMS $CREATE left the NAM resultant empty */
static const char *g_src_lines[] = { "Served by the OVMX FAL server code", "second record" };

/* The capture's files, as the VAX listed them. */
static const char *g_user = "SYSTEM";     /* identity of the "FAL process"  */
static int g_delme = 1, g_renme = 1, g_renamed = 0;
static int g_erased_delme, g_renamed_ok;

/* name part ("NAME.TYP") of a spec, version dropped */
static void namepart(const char *spec, char *out, size_t cap)
{
    const char *rb = strrchr(spec, ']');
    if (!rb) rb = strrchr(spec, ':');
    snprintf(out, cap, "%s", rb ? rb + 1 : spec);
    char *sc = strchr(out, ';'); if (sc) *sc = '\0';
}
static int exists(const char *nm)
{
    if (!strcmp(nm, "DELME.TXT")) return g_delme;
    if (!strcmp(nm, "RENME.TXT")) return g_renme;
    if (!strcmp(nm, "RENAMED.TXT")) return g_renamed;
    if (!strcmp(nm, "PROT.TXT")) return 1;
    return 0;
}

/* The VAX<->VAX SYS$LOGIN capture's directory (VAX2 SYS$SYSDEVICE:[SYSMGR],
 * default account DNTEST [200,201]): BRK1, BRK2 owned by DNTEST, BRKP by
 * [1,4] with (S:RWED,O:RWED,G,W). g_v2v selects it for SYS$LOGIN: specs. */
static int g_v2v;
static struct { const char *nm; int exists, prot; } g_brk[] = {
    { "BRK1.TXT", 1, 0 }, { "BRK2.TXT", 1, 0 }, { "BRK3.TXT", 0, 0 }, { "BRKP.TXT", 1, 1 },
};
static int brk_find(const char *nm)
{ for (int i = 0; i < 4; i++) if (!strcmp(g_brk[i].nm, nm)) return i; return -1; }
static int nonsystem(void) { return strcmp(g_user, "SYSTEM") != 0; }

struct sctx { char found[8][256]; char esa[256]; int pos, nfound; uint32_t sts; };
int dnet_fal_search_begin(const char *spec, void **ctx)
{
    static struct sctx c;
    memset(&c, 0, sizeof c);
    char nm[128]; namepart(spec, nm, sizeof nm);
    if (strstr(spec, "GREET.TXT")) { snprintf(c.found[0], sizeof c.found[0], "DKA0:[SRV]GREET.TXT;1"); c.nfound = 1; }
    else if (strstr(spec, "PUTNAME.TXT") && g_put_spec[0]) { snprintf(c.found[0], sizeof c.found[0], "DKA0:[SRV]PUTNAME.TXT;1"); c.nfound = 1; }
    else if (!g_v2v && strstr(spec, "SYS$LOGIN:BRK")) {
        /* the live bracket's two files, as OVMX RMS resolved them */
        c.nfound = 2;
        snprintf(c.found[0], sizeof c.found[0], "VDA0:[SYS0.SYSCOMMON.SYSMGR]BRK1.TXT;1");
        snprintf(c.found[1], sizeof c.found[1], "VDA0:[SYS0.SYSCOMMON.SYSMGR]BRK2.TXT;1");
    }
    else if (g_v2v && (strstr(spec, "SYS$LOGIN:") || strstr(spec, "SYS$SYSDEVICE:[SYSMGR]"))) {
        /* VMS RMS: SYS$LOGIN translates to SYS$SYSDEVICE:[SYSMGR]. */
        int wild = strchr(nm, '*') != NULL;
        for (int i = 0; i < 4; i++) {
            if (!g_brk[i].exists) continue;
            if (wild ? !strncmp(g_brk[i].nm, nm, (size_t)(strchr(nm, '*') - nm)) : !strcmp(g_brk[i].nm, nm))
                snprintf(c.found[c.nfound++], sizeof c.found[0], "SYS$SYSDEVICE:[SYSMGR]%s;1", g_brk[i].nm);
        }
        if (!c.nfound) {
            const char *p = strrchr(spec, ']'); if (!p) p = strrchr(spec, ':');
            snprintf(c.esa, sizeof c.esa, "SYS$SYSDEVICE:[SYSMGR]%s", p + 1);
        }
    }
    else if (strstr(spec, "SYS$SYSROOT:[SYSMGR]")) {
        if (exists(nm)) { snprintf(c.found[0], sizeof c.found[0], "SYS$SYSROOT:[SYSMGR]%s;1", nm); c.nfound = 1; }
        else {
            /* What VMS RMS leaves in the NAM after the search list
             * SYS$SYSROOT (SYS$SPECIFIC, SYS$COMMON) is exhausted. */
            const char *rb = strrchr(spec, ']');
            snprintf(c.esa, sizeof c.esa, "SYS$COMMON:[SYSMGR]%s", rb + 1);
        }
    }
    else return -1;                                /* $PARSE refused, no ctx */
    c.sts = c.nfound ? RMS$_NMF : RMS$_FNF;
    *ctx = &c; return 0;
}
int dnet_fal_search_next(void *ctx, char *rsa, size_t cap)
{ struct sctx *c = ctx; if (c->pos >= c->nfound) return -1; c->pos++;
  snprintf(rsa, cap, "%s", c->found[c->pos - 1]);
  return 0; }
void dnet_fal_search_end(void *ctx) { (void)ctx; }
uint32_t dnet_fal_search_status(void *ctx, uint32_t *stv, char *esa, size_t cap)
{ struct sctx *c = ctx; if (stv) *stv = 0;
  if (esa && cap) snprintf(esa, cap, "%s", c ? c->esa : "");
  return c ? c->sts : RMS$_FNF; }

/* VMS 64-bit time for 8-OCT-2026 hh:mm:ss.cc (the VAX console's dates). */
static void vmstime(uint8_t q[8], int hh, int mm, int ss, int cc)
{
    /* 8-OCT-2026 is MJD 61321 (days since 17-NOV-1858). */
    uint64_t v = ((uint64_t)61321 * 86400u + (uint64_t)hh * 3600u + (uint64_t)mm * 60u + (uint64_t)ss)
                 * 10000000u + (uint64_t)cc * 100000u;
    for (int i = 0; i < 8; i++) q[i] = (uint8_t)(v >> (8 * i));
}
int dnet_fal_fileattr(const char *spec, struct dnet_fal_fattr *out, uint32_t *sts)
{
    memset(out, 0, sizeof *out);
    if (strstr(spec, "BRK")) {
        char nm[128]; namepart(spec, nm, sizeof nm);
        int i = brk_find(nm);
        if (i < 0 || (g_v2v && !g_brk[i].exists)) { if (sts) *sts = RMS$_FNF; return -1; }
        if (g_brk[i].prot && nonsystem()) { if (sts) *sts = RMS$_PRV; return -1; }
        /* BRK1/BRK2 as VAX2's DIRECTORY/FULL printed them: 1/9 blocks, owner
         * [200,201], VFC 2-byte header, max 0 longest 16, print carriage
         * control, created 11:22:12.72 / 11:22:17.10, revised 11:22:23.72 (2). */
        out->org = 0; out->rfm = 3; out->rat = 4; out->mrs = 0; out->lrl = 16; out->deq = 0;
        out->alq = 9; out->ebk = 1; out->ffb = (i == 0) ? 38 : 20; out->fsz = 2;
        out->fileprot = 0xfa00; out->uic_group = 0200; out->uic_member = 0201; out->revision = 2;
        if (i == 0) vmstime(out->credate, 11, 22, 12, 72); else vmstime(out->credate, 11, 22, 17, 10);
        vmstime(out->revdate, 11, 22, 23, 72);
        if (sts) *sts = RMS$_NORMAL;
        return 0;
    }
    if (!strstr(spec, "DELME.TXT") || !g_delme) { if (sts) *sts = RMS$_FNF; return -1; }
    /* DELME.TXT's header as the VAX DIRECTORY/FULL printed it: 1/9 blocks,
     * owner [1,4], VAR max 0 longest 9, CR, extend 0, S:RWED,O:RWED,G:RE,W:,
     * created 06:29:07.02, revised 06:29:36.37 (1). */
    out->org = 0; out->rfm = 2; out->rat = 2; out->mrs = 0; out->lrl = 9; out->deq = 0;
    out->alq = 9; out->ebk = 1; out->ffb = 12; out->fileprot = 0xfa00;
    out->uic_group = 1; out->uic_member = 4; out->revision = 1;
    vmstime(out->credate, 6, 29, 7, 2);
    vmstime(out->revdate, 6, 29, 36, 37);
    if (sts) *sts = RMS$_NORMAL;
    return 0;
}
int dnet_fal_erase(const char *spec, uint32_t *sts, uint32_t *stv)
{
    char nm[128]; namepart(spec, nm, sizeof nm);
    *stv = 0;
    if (g_v2v && brk_find(nm) >= 0) {
        int i = brk_find(nm);
        if (!g_brk[i].exists) { *sts = RMS$_FNF; return -1; }
        if (g_brk[i].prot && nonsystem()) { *sts = RMS$_PRV; *stv = SS$_NOPRIV; return -1; }
        g_brk[i].exists = 0; *sts = RMS$_NORMAL; return 0;
    }
    if (!exists(nm)) { *sts = RMS$_FNF; return -1; }
    if (!strcmp(nm, "PROT.TXT") && strcmp(g_user, "SYSTEM")) { *sts = RMS$_PRV; *stv = SS$_NOPRIV; return -1; }
    if (!strcmp(nm, "DELME.TXT")) { g_delme = 0; g_erased_delme = 1; }
    *sts = RMS$_NORMAL; return 0;
}
int dnet_fal_rename(const char *oldspec, const char *newspec, uint32_t *sts, uint32_t *stv)
{
    char nm[128]; namepart(oldspec, nm, sizeof nm);
    *stv = 0;
    if (g_v2v && brk_find(nm) >= 0) {
        char nn[128]; namepart(newspec, nn, sizeof nn);
        int i = brk_find(nm), j = brk_find(nn);
        if (!g_brk[i].exists) { *sts = RMS$_FNF; return -1; }
        if (g_brk[i].prot && nonsystem()) { *sts = RMS$_PRV; *stv = SS$_NOPRIV; return -1; }
        if (j < 0) { *sts = RMS$_SYN; return -1; }
        g_brk[i].exists = 0; g_brk[j].exists = 1; *sts = RMS$_NORMAL; return 0;
    }
    if (!exists(nm)) { *sts = RMS$_FNF; return -1; }
    if (!strcmp(nm, "PROT.TXT") && strcmp(g_user, "SYSTEM")) { *sts = RMS$_PRV; *stv = SS$_NOPRIV; return -1; }
    if (!strcmp(nm, "RENME.TXT") && strstr(newspec, "RENAMED.TXT")) { g_renme = 0; g_renamed = 1; g_renamed_ok = 1; }
    *sts = RMS$_NORMAL; return 0;
}

int dnet_fal_wopen(const char *spec, uint8_t rfm, uint8_t rat, void **h, char *rsa, size_t cap)
{ (void)rfm; (void)rat; snprintf(g_put_spec, sizeof g_put_spec, "%s", spec); g_put_len = 0; g_put_closed = 0;
  if (rsa && cap) { if (g_wopen_no_rsa) rsa[0] = '\0'; else snprintf(rsa, cap, "DKA0:[SRV]%s1", spec); }
  *h = g_put_spec; return 0; }
int dnet_fal_wput(void *h, const uint8_t *rec, size_t len)
{ (void)h; if (g_put_len + len + 1 > sizeof g_put_data) return -1;
  memcpy(g_put_data + g_put_len, rec, len); g_put_len += len; g_put_data[g_put_len++] = '\n'; return 0; }
int dnet_fal_wclose(void *h) { (void)h; g_put_closed = 1; return 0; }
static int g_rpos;
int dnet_fal_ropen_st(const char *spec, void **h, uint8_t *rfm, uint8_t *rat, uint32_t *sts)
{
  /* VMS RMS through the search list SYS$SYSROOT: the first member has no
   * PROT.TXT, so the $OPEN completes FNF (the VAX FAL's 0x4032 for it). */
  if (strstr(spec, "PROT.TXT") && strcmp(g_user, "SYSTEM")) { if (sts) *sts = RMS$_FNF; return -1; }
  if (g_v2v && strstr(spec, "BRK")) {
      char nm[128]; namepart(spec, nm, sizeof nm);
      int i = brk_find(nm);
      if (i < 0 || !g_brk[i].exists) { if (sts) *sts = RMS$_FNF; return -1; }
      if (g_brk[i].prot && nonsystem()) { if (sts) *sts = RMS$_PRV; return -1; }
      g_rpos = (i == 0) ? 10 : 20;
      if (rfm) *rfm = 3;
      if (rat) *rat = 4;
      if (sts) *sts = RMS$_NORMAL;
      *h = &g_rpos; return 0;
  }
  if (!strstr(spec, "GREET.TXT")) { if (sts) *sts = RMS$_FNF; return -1; }
  g_rpos = 0;
  if (rfm) *rfm = 2;
  if (rat) *rat = 2;
  if (sts) *sts = RMS$_NORMAL;
  *h = &g_rpos; return 0; }
int dnet_fal_ropen(const char *spec, void **h, uint8_t *rfm, uint8_t *rat)
{ return dnet_fal_ropen_st(spec, h, rfm, rat, NULL); }
/* The second record carries an embedded NUL: records are length-delimited,
 * never C strings (a VAR file read back must be record-for-record). */
int dnet_fal_rget(void *h, uint8_t *rec, size_t cap, size_t *len)
{ int *p = h;
  if (*p >= 10) {                       /* BRK1: 2 records, BRK2: 1 record */
      static const char *b1[] = { "bracket file one", "second record" }, *b2[] = { "bracket file two" };
      int k = *p % 10, nrec = (*p >= 20) ? 1 : 2;
      if (k >= nrec) return 0;
      const char *l = (*p >= 20) ? b2[k] : b1[k];
      size_t n = strlen(l); if (n > cap) return -1;
      memcpy(rec, l, n); *len = n; (*p)++; return 1; }
  if (*p >= 2) return 0;
  const char *l = g_src_lines[*p]; size_t n = strlen(l);
  if (*p == 1) { if (n + 2 > cap) return -1; memcpy(rec, l, n); rec[n] = 0; rec[n + 1] = 'Z'; *len = n + 2; }
  else { if (n > cap) return -1; memcpy(rec, l, n); *len = n; }
  (*p)++; return 1; }
int dnet_fal_rclose(void *h) { (void)h; return 0; }

/* ======================= scripted transport =============================== */
#define MAXSEG 64
struct script { const char **in; int nin, pos; char out[MAXSEG][3300]; int nout;
                const uint8_t *bin[MAXSEG]; size_t binlen[MAXSEG]; };
static size_t unhex(const char *h, uint8_t *o, size_t cap)
{ size_t n = 0; while (h[0] && h[1] && n < cap) { unsigned v; sscanf(h, "%2x", &v); o[n++] = (uint8_t)v; h += 2; } return n; }
static int s_send(void *c, const uint8_t *seg, size_t len)
{ struct script *s = c; if (s->nout >= MAXSEG) return -1; char *o = s->out[s->nout++];
  o[0] = '\0';
  for (size_t i = 0; i < len && 2 * i + 2 < sizeof s->out[0]; i++) sprintf(o + 2 * i, "%02x", seg[i]);
  return 0; }
static int s_recv(void *c, uint8_t *buf, size_t cap, size_t *len)
{ struct script *s = c; if (s->pos >= s->nin) return -1;
  if (s->in) { *len = unhex(s->in[s->pos++], buf, cap); return 0; }
  size_t n = s->binlen[s->pos]; if (n > cap) return -1;
  memcpy(buf, s->bin[s->pos++], n); *len = n; return 0; }

static int saw(struct script *s, const char *prefix)
{ for (int i = 0; i < s->nout; i++) if (!strncmp(s->out[i], prefix, strlen(prefix))) return i; return -1; }

/* ======================= the vms-277a oracle replay ======================= */
#define MAXLINK 32
#define MAXLSEG 32
struct oseg { uint8_t b[1600]; size_t n; };
struct olink {
    unsigned id;
    char user[40];
    uint8_t conn[256]; size_t connlen;
    struct oseg cli[MAXLSEG]; int ncli;
    struct oseg srv[MAXLSEG]; int nsrv;
};
static struct olink g_links[MAXLINK];
static int g_nlinks;

static struct olink *link_by_id(unsigned id)
{ for (int i = 0; i < g_nlinks; i++) if (g_links[i].id == id) return &g_links[i]; return NULL; }

static int g_strip_trailer;   /* 1 for the VAX<->VAX DAP 7 capture only */
/* Parse nspdump output: "<t> 1.1->1.2 KIND...: hex hex ..." */
static int load_wire(const char *path)
{
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
            if (p[0] == 'X' && p[1] == 'X') v = 'x';   /* masked password byte */
            else if (sscanf(p, "%2x", &v) != 1) break;
            b[n++] = (uint8_t)v; p += 2;
        }
        int from_client = !strncmp(dir, "1.1->", 5);
        if (!strcmp(kind, "RCI:") && n >= 9 && g_nlinks < MAXLINK) {
            struct olink *L = &g_links[g_nlinks++];
            memset(L, 0, sizeof *L);
            L->id = b[3] | (b[4] << 8);
            L->connlen = n - 9;                    /* session control data */
            memcpy(L->conn, b + 9, L->connlen);
            char pw[80], acct[80];
            if (dnet_fal_access_decode(L->conn, L->connlen, L->user, sizeof L->user,
                                       pw, sizeof pw, acct, sizeof acct) != 0)
                L->user[0] = '\0';
        } else if (!strcmp(kind, "CC:") && n >= 5) {
            cur = link_by_id(b[1] | (b[2] << 8));
        } else if (!strcmp(kind, "DATA") && cur) {
            int segno = 0;
            if (sscanf(line, "%*f %*s DATA seg%d", &segno) != 1) continue;
            if (g_strip_trailer && segno >= 2) { if (n < 2) continue; n -= 2; }   /* DAP 7 trailer */
            struct oseg *o = from_client ? &cur->cli[cur->ncli] : &cur->srv[cur->nsrv];
            if ((from_client ? cur->ncli : cur->nsrv) >= MAXLSEG) continue;
            memcpy(o->b, b, n); o->n = n;
            if (from_client) cur->ncli++; else cur->nsrv++;
        }
    }
    fclose(f);
    return 0;
}

struct dmsg { struct dnet_dap_msg m; const uint8_t *raw; size_t len; };
static int g_drop_ack;   /* VAX side of a DIRLIST link: DAP 7 per-file ACK */
/* OVMX advertises DAP 7.2 (rd vms-b2f): against a DAP 7 client it ACKs each
 * DIRLIST file as the VAX FAL does, so no ACK is normalised away. */
static int g_ack_normalise = 0;
/* The dates the VAX console printed, in DATE AND TIME message order. */
static const char *g_dates[4][2] = { { "08-OCT-26 06:29:07", "08-OCT-26 06:29:36" } };
static int g_ndate;
static int split(const uint8_t *b, size_t n, struct dmsg *out, int max)
{
    int k = 0; size_t off = 0;
    while (off < n && k < max) {
        size_t used = 0;
        if (dnet_dap_decode(b + off, n - off, &out[k].m, &used) != DNET_DAP_OK || !used) return -1;
        out[k].raw = b + off; out[k].len = used;
        off += used;
        if (out[k].m.type == 18) continue;         /* DAP 7 only: not served */
        if (g_drop_ack && out[k].m.op == DNET_DAP_ACKNOWLEDGE) continue;
        k++;
    }
    return k;
}

static void hexs(const uint8_t *b, size_t n, char *o, size_t cap)
{ o[0] = '\0'; for (size_t i = 0; i < n && 3 * i + 4 < cap; i++) sprintf(o + 3 * i, "%02x ", b[i]); }

/* Compare one OVMX reply segment with the VAX FAL's (see NORMALISATION). */
static int seg_matches(const char *label, const uint8_t *ov, size_t ovn, const uint8_t *vx, size_t vxn,
                       int dirlist)
{
    static struct dmsg a[32], v[32];
    int na = split(ov, ovn, a, 32);
    g_drop_ack = dirlist && g_ack_normalise;
    int nv = split(vx, vxn, v, 32);
    g_drop_ack = 0;
    char h1[2048], h2[2048];
    if (na < 0 || nv < 0 || na != nv) {
        hexs(ov, ovn, h1, sizeof h1); hexs(vx, vxn, h2, sizeof h2);
        printf("  %s: %d vs %d messages\n    ovmx %s\n    vax  %s\n", label, na, nv, h1, h2);
        return 0;
    }
    for (int i = 0; i < na; i++) {
        const struct dnet_dap_msg *x = &a[i].m, *y = &v[i].m;
        int ok;
        if (x->type != y->type || x->flags != y->flags) ok = 0;
        else if (x->op == DNET_DAP_ATTRIBUTES) {
            uint64_t mask = (1u << 21) - 1;
            mask &= ~((1u << DNET_DAP_ATT_FOP) | (1u << DNET_DAP_ATT_DEV) | (1u << DNET_DAP_ATT_SBN));
            ok = x->u.attr.menu == (y->u.attr.menu & mask) &&
                 x->u.attr.org == y->u.attr.org && x->u.attr.rfm == y->u.attr.rfm &&
                 x->u.attr.rat == y->u.attr.rat && x->u.attr.mrs == y->u.attr.mrs &&
                 x->u.attr.alq == y->u.attr.alq && x->u.attr.deq == y->u.attr.deq &&
                 x->u.attr.lrl == y->u.attr.lrl && x->u.attr.hbk == y->u.attr.hbk &&
                 x->u.attr.ebk == y->u.attr.ebk && x->u.attr.ffb == y->u.attr.ffb &&
                 x->u.attr.fsz == y->u.attr.fsz;
        } else if (x->op == DNET_DAP_DATETIME) {
            /* VAX (DAP 7): RVN + binary times; OVMX (DAP 5.6): CDT, RDT, RVN.
             * The dates are the ones the VAX console printed for the file. */
            const char *ec = g_dates[g_ndate][0], *er = g_dates[g_ndate][1];
            if (g_dates[g_ndate + 1][0]) g_ndate++;
            ok = x->u.datetime.menu == (DNET_DAP_DAT_CDT | DNET_DAP_DAT_RDT | DNET_DAP_DAT_RVN) &&
                 (y->u.datetime.menu & DNET_DAP_DAT_RVN) && x->u.datetime.rvn == y->u.datetime.rvn &&
                 !strcmp(x->u.datetime.cdt, ec) && !strcmp(x->u.datetime.rdt, er);
        } else {
            ok = a[i].len == v[i].len && !memcmp(a[i].raw, v[i].raw, a[i].len);
        }
        if (!ok) {
            hexs(a[i].raw, a[i].len, h1, sizeof h1); hexs(v[i].raw, v[i].len, h2, sizeof h2);
            printf("  %s: message %d (%s) differs\n    ovmx %s\n    vax  %s\n", label, i,
                   dnet_dap_op_name(y->op), h1, h2);
            return 0;
        }
    }
    return 1;
}

/* Replay one captured link; 1 = every server segment matched. */
static int replay_link(const struct olink *L, const char *label, uint32_t *st_out)
{
    static struct script s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < L->ncli; i++) { s.bin[i] = L->cli[i].b; s.binlen[i] = L->cli[i].n; }
    s.nin = L->ncli;
    static struct dnet_dap_transport t;
    memset(&t, 0, sizeof t);
    t.send = s_send; t.recv = s_recv; t.ctx = &s;
    g_user = L->user;
    uint32_t st = dnet_fal_server_run(&t);
    if (st_out) *st_out = st;
    if (s.nout < 1 || strncmp(s.out[0], "0100", 4) != 0 || s.nout != L->nsrv) {
        printf("  %s: OVMX sent %d segments, the VAX FAL %d\n", label, s.nout, L->nsrv);
        for (int i = 0; i < s.nout; i++) printf("    ovmx[%d] %s\n", i, s.out[i]);
        return 0;
    }
    /* A DIRECTORY LIST link: the client's ACCESS has ACCFUNC 6. */
    int dirlist = L->ncli > 1 && L->cli[1].n > 2 && L->cli[1].b[0] == 3 &&
                  L->cli[1].b[(L->cli[1].b[1] & DNET_DAP_FLAG_LENGTH) ? 3 : 2] == DNET_DAP_ACC_DIRLIST;
    for (int i = 1; i < s.nout; i++) {
        uint8_t ob[1600];
        size_t on = unhex(s.out[i], ob, sizeof ob);
        if (!seg_matches(label, ob, on, L->srv[i].b, L->srv[i].n, dirlist)) return 0;
    }
    return 1;
}

static void live_bracket(const char *wire);
static void oracle_replay(const char *wire)
{
    g_strip_trailer = 1;
    int lw = load_wire(wire);
    g_strip_trailer = 0;
    if (lw != 0) { CHECK(0, "the vms-277a oracle capture loads"); return; }
    CHECK(g_nlinks == 17, "the capture holds the 17 links of the nine VAX commands");
    if (g_nlinks != 17) return;
    /* Link order = the console's command order (vax1-console.txt). */
    static const char *what[17] = {
        "DIRECTORY/FULL DELME.TXT: DIRLIST + MAIN/SUMMARY/DATE/PROTECTION",
        "TYPE NOSUCH.TXT: DIRLIST -> volume/dir/file NAMEs + STATUS FNF 0x4032 STV 0x0910",
        "TYPE with the default account: refused at connect (INVLOGIN)",
        "RENAME RENME.TXT: DIRLIST of the source",
        "RENAME RENME.TXT RENAMED.TXT: ACCESS RENAME + NAME -> NAME ACK NAME ACK ACCOMP",
        "DELETE DELME.TXT;1: DIRLIST (1)", "DELETE DELME.TXT;1: DIRLIST (2)",
        "DELETE DELME.TXT;1: ACCESS ERASE -> NAME ACK ACCOMP",
        "DELETE NOSUCH.TXT;1: DIRLIST (1) -> STATUS FNF", "DELETE NOSUCH.TXT;1: DIRLIST (2) -> STATUS FNF",
        "DNTEST TYPE PROT.TXT: DIRLIST",
        "DNTEST TYPE PROT.TXT: OPEN refused -> STATUS FNF 0x4032 STV 0x0910 (not PRV)",
        "DNTEST DELETE PROT.TXT;1: DIRLIST (1)", "DNTEST DELETE PROT.TXT;1: DIRLIST (2)",
        "DNTEST DELETE PROT.TXT;1: ERASE refused -> STATUS PRV 0x4055 STV 0x24",
        "DNTEST RENAME PROT.TXT: DIRLIST",
        "DNTEST RENAME PROT.TXT STOLEN.TXT: refused -> STATUS RMV 0x405f, no STV",
    };
    for (int i = 0; i < 17; i++) {
        const struct olink *L = &g_links[i];
        char label[200];
        snprintf(label, sizeof label, "link %04x %s", L->id, what[i]);
        if (i == 2) {
            /* No DAP at all: the VAX FAL disconnected before confirming. */
            uint32_t a = dnet_fal_connect_auth(L->conn, L->connlen, NULL, 0);
            CHECK(L->ncli == 0 && L->user[0] == '\0' && a == SS$_INVLOGIN, label);
            continue;
        }
        uint32_t st = 0;
        int ok = replay_link(L, label, &st);
        CHECK(ok, label);
        if (ok) printf("  ok: %s\n", label);
    }
    CHECK(g_erased_delme && !g_delme, "the ERASE reached $ERASE for DELME.TXT;1");
    CHECK(g_renamed_ok, "the RENAME reached $RENAME RENME.TXT -> RENAMED.TXT");
}

/* The live bracket (tests/lab/captures/decnet-fal-verbs-20261008/live-bracket/):
 * VAX1 talked DAP 5.6 to a booted OVMX (#1487 at 549c21d2). Its DIRECTORY of
 * SYS$LOGIN:BRK*.TXT listed only BRK1 and failed "RMS-F-BUG_DAP, DAP code =
 * 0001A006" (MAC 10 sync / MIC ACK): OVMX sent a per-file ACK; DIRECTORY/FULL
 * asked only for MAIN attributes and RENAME was refused "RMS-F-SUPPORT"
 * because OVMX's SYSCAP lacked SUMMARY/DATE/PROTECTION and RENAME. The VAX's
 * own segments are replayed and the fixed replies checked. */
static void live_bracket(const char *wire)
{
    g_nlinks = 0;
    if (load_wire(wire) != 0) { CHECK(0, "the live-bracket capture loads"); return; }
    CHECK(g_nlinks == 12, "the live bracket holds its 12 links");
    if (g_nlinks < 1) return;
    const struct olink *L = &g_links[0];            /* DIRECTORY/FULL BRK*.TXT */
    static struct script s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < L->ncli; i++) { s.bin[i] = L->cli[i].b; s.binlen[i] = L->cli[i].n; }
    s.nin = L->ncli;
    static struct dnet_dap_transport t;
    memset(&t, 0, sizeof t);
    t.send = s_send; t.recv = s_recv; t.ctx = &s;
    g_user = "SYSTEM";
    (void)dnet_fal_server_run(&t);
    uint8_t b[1600]; size_t n;
    struct dmsg m[32]; int k;
    n = unhex(s.out[0], b, sizeof b);
    struct dnet_dap_msg cfg; size_t used = 0;
    CHECK(s.nout >= 1 && dnet_dap_decode(b, n, &cfg, &used) == DNET_DAP_OK &&
          dnet_dap_syscap_has(&cfg, DNET_DAP_CAP_SUMMARY) && dnet_dap_syscap_has(&cfg, DNET_DAP_CAP_DATETIME) &&
          dnet_dap_syscap_has(&cfg, DNET_DAP_CAP_PROTECTION) && dnet_dap_syscap_has(&cfg, DNET_DAP_CAP_RENAME),
          "live bracket: OVMX's CONFIGURATION now advertises SUMMARY, DATE AND TIME, PROTECTION and RENAME");
    int had_ack = 0;
    k = (L->nsrv >= 2) ? split(L->srv[1].b, L->srv[1].n, m, 32) : -1;
    for (int i = 0; i < k; i++) if (m[i].m.op == DNET_DAP_ACKNOWLEDGE) had_ack = 1;
    CHECK(had_ack && cfg.u.config.vernum == 7,
          "live bracket: the captured OVMX (then DAP 5.6) DIRLIST reply carried the ACK the VAX rejected; OVMX now advertises DAP 7.2");
    int files = 0, acks = 0, last = -1;
    n = (s.nout >= 2) ? unhex(s.out[1], b, sizeof b) : 0;
    k = n ? split(b, n, m, 32) : -1;
    for (int i = 0; i < k; i++) {
        if (m[i].m.op == DNET_DAP_NAME && m[i].m.u.name.nametype == DNET_DAP_NT_FILENAME) files++;
        if (m[i].m.op == DNET_DAP_ACKNOWLEDGE) acks++;
        last = m[i].m.op;
    }
    CHECK(s.nout == 2 && files == 2 && acks == 2 && last == DNET_DAP_ACCESS_COMPLETE,
          "live bracket: the same DIRLIST (a DAP 7.2 client) now lists BRK1 AND BRK2, an ACK after each, then ACCESS COMPLETE");
}

/* 6. The VAX<->VAX SYS$LOGIN capture (vax-to-vax-sys-login/): DIRECTORY/FULL
 * with a protected file, wildcard TYPE (a DIRLIST, then CONFIG + OPEN per file
 * on one link), RENAME, DELETE ;* (a DIRLIST, then ERASE by name), a
 * refused DELETE, and a DIRECTORY of a missing file. */
static void v2v_replay(const char *wire)
{
    g_nlinks = 0;
    g_strip_trailer = 1;
    int lw = load_wire(wire);
    g_strip_trailer = 0;
    if (lw != 0) { CHECK(0, "the VAX<->VAX SYS$LOGIN capture loads"); return; }
    CHECK(g_nlinks == 15, "the VAX<->VAX SYS$LOGIN capture holds its 15 links");
    if (g_nlinks != 15) return;
    g_v2v = 1;
    g_dates[0][0] = "08-OCT-26 11:22:12"; g_dates[0][1] = "08-OCT-26 11:22:23";
    g_dates[1][0] = "08-OCT-26 11:22:17"; g_dates[1][1] = "08-OCT-26 11:22:23";
    g_dates[2][0] = NULL;
    g_ndate = 0;
    static const char *what[15] = {
        "DIRECTORY/FULL BRK*.TXT: two full entries, BRKP refused (NAME + STATUS PRV, CONTINUE skip, ACCOMP)",
        "DIRECTORY BRK*.TXT: three NAMEs, ACCOMP",
        "TYPE BRK*.TXT: DIRLIST of the wildcard",
        "TYPE BRK*.TXT: CONFIG+OPEN per file on one link; BRKP refused STATUS PRV 0x4055 STV 0x24",
        "RENAME BRK2 BRK3: DIRLIST", "RENAME BRK2 BRK3: NAME ACK NAME ACK ACCOMP",
        "DIRECTORY after RENAME",
        "DELETE BRK3.TXT;*: DIRLIST (1)", "DELETE BRK3.TXT;*: DIRLIST (2)",
        "DELETE BRK3.TXT;*: ERASE by name -> NAME ACK ACCOMP",
        "DELETE BRKP.TXT;*: DIRLIST (1)", "DELETE BRKP.TXT;*: DIRLIST (2)",
        "DELETE BRKP.TXT;*: ERASE refused STATUS PRV 0x4055 STV 0x24",
        "DIRECTORY NOSUCH.TXT: NAME volume/directory/file + STATUS FNF (VMS prints NOFILES)",
        "DIRECTORY after DELETE",
    };
    for (int i = 0; i < 15; i++) {
        const struct olink *L = &g_links[i];
        char label[240];
        snprintf(label, sizeof label, "v2v link %04x %s", L->id, what[i]);
        g_user = "DNTEST";                    /* the default DECnet account */
        if (i != 3) {
            int ok = replay_link(L, label, NULL);
            CHECK(ok, label);
            if (ok) printf("  ok: %s\n", label);
            continue;
        }
        /* TYPE's OPEN link: per-file segment grouping is the VAX's DAP 7
         * blocking; the checks are the protocol ones. */
        static struct script sc;
        memset(&sc, 0, sizeof sc);
        for (int k = 0; k < L->ncli; k++) { sc.bin[k] = L->cli[k].b; sc.binlen[k] = L->cli[k].n; }
        sc.nin = L->ncli;
        static struct dnet_dap_transport t;
        memset(&t, 0, sizeof t);
        t.send = s_send; t.recv = s_recv; t.ctx = &sc;
        (void)dnet_fal_server_run(&t);
        int configs = 0, r1 = 0, r2 = 0, r3 = 0;
        for (int k = 0; k < sc.nout; k++) {
            if (!strncmp(sc.out[k], "0100", 4)) configs++;
            if (strstr(sc.out[k], "627261636b65742066696c65206f6e65")) r1 = 1;   /* "bracket file one" */
            if (strstr(sc.out[k], "7365636f6e64207265636f7264")) r2 = 1;          /* "second record" */
            if (strstr(sc.out[k], "627261636b65742066696c652074776f")) r3 = 1;   /* "bracket file two" */
        }
        const struct oseg *lastv = &L->srv[L->nsrv - 1];
        char lv[64] = ""; for (size_t k = 0; k < lastv->n && k < 30; k++) sprintf(lv + 2 * k, "%02x", lastv->b[k]);
        int ok = configs == 3 && r1 && r2 && r3 && sc.nout > 0 && !strcmp(sc.out[sc.nout - 1], lv) &&
                 !strcmp(lv, "0900554000000124");
        if (!ok) {
            printf("  %s: configs %d records %d%d%d last ovmx %s vax %s\n", label, configs, r1, r2, r3,
                   sc.nout ? sc.out[sc.nout - 1] : "-", lv);
        }
        CHECK(ok, label);
        if (ok) printf("  ok: %s\n", label);
    }
    CHECK(!g_brk[1].exists && !g_brk[2].exists && g_brk[3].exists,
          "v2v: BRK2 was renamed to BRK3, BRK3 erased, BRKP refused and still there");
    g_v2v = 0;
}

int main(int argc, char **argv)
{
    static struct dnet_dap_transport t;
    t.send = s_send; t.recv = s_recv;

    /* 1. VMS COPY local -> OVMX (PUT), with DISPLAY main+NAME. */
    static const char *put_in[] = {
        "01003c1007030702000500f7fbd9ffaeac8694e77f",
        "020210efa00401000202000001018080100e00030002010c5055544e414d452e5458543b53408102",
        "0400020840",
        "0402040409034008060f000048656c6c6f206c696e65206f6e6508060900006c696e652074776f070004",
        "07000100",
    };
    static struct script ps; memset(&ps, 0, sizeof ps); ps.in = put_in; ps.nin = 5;
    t.ctx = &ps; t.rxlen = t.rxoff = 0;
    uint32_t st = dnet_fal_server_run(&t);
    int i_att = saw(&ps, "0200"), i_name = saw(&ps, "0f0001"), i_ack = saw(&ps, "0600");
    CHECK(st == 1, "VMS PUT session completes (SS$_NORMAL)");
    CHECK(i_att >= 0 && i_name > i_att && i_ack > i_name,
          "CREATE reply = ATTRIBUTES, NAME(resultant), ACK -- the order a VMS COPY requires");
    CHECK(strcmp(g_put_spec, "PUTNAME.TXT;") == 0, "the file created is the spec VMS named");
    CHECK(g_put_closed && g_put_len == 24 && !memcmp(g_put_data, "Hello line one\nline two\n", 24),
          "both blocked DATA records stored verbatim and the file closed");
    CHECK(saw(&ps, "070002") >= 0, "END-OF-STREAM and CLOSE answered ACCESS COMPLETE(RESPONSE)");

    /* 1b. If $CREATE ever returned no resultant (rd vms-98e), the server
     * must REFUSE the access honestly -- an ACK without the NAME the VMS COPY
     * asked for is a DAP sync error at the peer (RMS-F-BUG_DAP 0001A006). */
    g_wopen_no_rsa = 1; g_put_spec[0] = '\0';
    static struct script ps2; memset(&ps2, 0, sizeof ps2); ps2.in = put_in; ps2.nin = 5;
    t.ctx = &ps2; t.rxlen = t.rxoff = 0;
    st = dnet_fal_server_run(&t);
    CHECK(saw(&ps2, "0600") < 0 && saw(&ps2, "0f0001") < 0 && saw(&ps2, "0900") >= 0,
          "no $CREATE resultant: STATUS refusal, never ACK without the NAME asked for");
    g_wopen_no_rsa = 0;

    /* 2. VMS COPY OVMX -> local: link 1 = DIRECTORY LIST, link 2 = OPEN + GET.
     * The DIRECTORY LIST reply is the VAX FAL's shape (rd vms-277a): one
     * blocked segment NAME(volume) NAME(directory) NAME(file) ACK ACCOMP. */
    static const char *dir_in[] = {
        "0100240407030702000500f7fbd9ffaeac8694e77f",
        "030006010a47524545542e5458543b",
    };
    static struct script ds; memset(&ds, 0, sizeof ds); ds.in = dir_in; ds.nin = 2;
    t.ctx = &ds; t.rxlen = t.rxoff = 0;
    st = dnet_fal_server_run(&t);
    CHECK(st == 1 && ds.nout == 2 &&
          !strcmp(ds.out[1], "0f02070805444b41303a0f020704055b5352565d0f020d020b47524545542e5458543b31060200070002"),
          "DIRECTORY LIST to a DAP 7 client = NAME(volume), NAME(directory), NAME(file), ACK, ACCESS COMPLETE(RESPONSE), blocked");

    static const char *get_in[] = {
        "01003c1007030702000500f7fbd9ffaeac8694e77f",
        "02020baf2001000202000080801003000101194c4142244449534b3a5b5352565d47524545542e5458543b3142028102",
        "0400020800",
        "040001090300",
        "070004",
        "07000100",
    };
    static struct script gs; memset(&gs, 0, sizeof gs); gs.in = get_in; gs.nin = 6;
    t.ctx = &gs; t.rxlen = t.rxoff = 0;
    st = dnet_fal_server_run(&t);
    int d1 = saw(&gs, "080000536572766564"), eof = saw(&gs, "09002750");
    CHECK(st == 1, "VMS GET session completes");
    CHECK(saw(&gs, "0f0001") >= 0, "OPEN with DISPLAY NAME returns the resultant NAME before ACK");
    CHECK(d1 >= 0 && eof > d1, "RAC=file transfer GET streams the records then STATUS EOF (MAC 5 / MIC 047)");
    CHECK(saw(&gs, "0800007365636f6e64207265636f7264005a") >= 0,
          "a record with an embedded NUL is sent whole (length-delimited, not truncated at the NUL)");

    /* 3. Honest misses: an unknown file in a DIRECTORY LIST and an OPEN. */
    static const char *miss_in[] = {
        "0100240407030702000500f7fbd9ffaeac8694e77f",
        "030006010a4e4f5045472e5458543b",
    };
    static struct script ms; memset(&ms, 0, sizeof ms); ms.in = miss_in; ms.nin = 2;
    t.ctx = &ms; t.rxlen = t.rxoff = 0;
    st = dnet_fal_server_run(&t);
    CHECK(st == SS$_NOSUCHFILE && ms.nout == 2 && !strcmp(ms.out[1], "090032400000021009"),
          "a DIRECTORY LIST of a missing file is STATUS FNF (MAC 4 / MIC 062) + STV 0x0910, the VAX FAL's bytes");

    /* 4. rd vms-277a: the real VAX DIRECTORY/FULL / TYPE / DELETE / RENAME
     * sessions, replayed and compared segment for segment. */
    if (argc < 2) CHECK(0, "the oracle capture path is given (ctest passes it)");
    else oracle_replay(argv[1]);

    /* 5. The 2026-10-08 live bracket: a real VAX against a booted OVMX. */
    if (argc < 3) CHECK(0, "the live-bracket capture path is given (ctest passes it)");
    else live_bracket(argv[2]);

    /* 6. The VAX<->VAX SYS$LOGIN capture (wildcards, VFC, refusals, NOFILES). */
    if (argc < 4) CHECK(0, "the VAX<->VAX SYS$LOGIN capture path is given (ctest passes it)");
    else v2v_replay(argv[3]);

    printf("test_dnet_fal_server: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
