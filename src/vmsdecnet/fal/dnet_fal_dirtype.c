/*
 * dnet_fal_dirtype.c - `$ DIRECTORY node::spec` and `$ TYPE node::spec` over
 * the FAL client (rd vms-6a4). See dnet_fal_dirtype.h for what a VMS client
 * does and which capture shows it.
 *
 * PEER INPUT: every NAME and DATA here came from the remote FAL. The DAP
 * decoder bounds each field (a NAME's spec to DNET_DAP_MAX_SPEC, a record to
 * DNET_DAP_MAX_REC); this layer copies with explicit bounds, caps how many
 * names it keeps (DNET_FAL_DIR_MAXFILES), and never trusts a VFC control size
 * beyond the record it came with. tests/vmsdecnet/test_dnet_fal_dirtype.c
 * fuzzes it under ASan/UBSan.
 */
#include "dnet_fal_dirtype.h"
#include "dnet_dap.h"

#include <stdlib.h>
#include <string.h>

#include "ssdef.h"
#include "rmsdef.h"
#include "starlet.h"
#include "descrip.h"

/* RMS$_FND "ACP file or directory lookup failed", at the error severity the
 * VAX printed it with (-RMS-E-FND, vax1-console.txt); the message number is
 * the catalog's (ovmx_msgcat.inc 0x0001C028). rmsdef.h does not define it. */
#define DNET_RMS_FND 0x0001C02Au

/* ---- spec defaults --------------------------------------------------------- */

/* Index of the name part: just past the last ']', '>' or ':'. */
static size_t name_part(const char *s)
{
    size_t at = 0;
    for (size_t i = 0; s[i]; i++)
        if (s[i] == ']' || s[i] == '>' || s[i] == ':') at = i + 1;
    return at;
}

int dnet_fal_default_spec(const char *spec, int verb, char *out, size_t cap)
{
    if (!spec || !out || cap == 0) return -1;
    const char *name = spec + name_part(spec);
    int has_ver = strchr(name, ';') != NULL;
    int has_dot = strchr(name, '.') != NULL;
    int n;
    if (verb == DNET_FAL_VERB_DIRECTORY) {
        if (!name[0])
            n = snprintf(out, cap, "%s*.*;*", spec);
        else
            n = snprintf(out, cap, "%s%s%s", spec, has_dot ? "" : ".*", has_ver ? "" : ";*");
    } else {
        n = snprintf(out, cap, "%s%s", spec, has_ver ? "" : ";");
    }
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return -1; }
    return 0;
}

void dnet_fal_display_prefix(const char *node, const char *user, int has_access,
                             int has_password, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (has_access && user && user[0])
        snprintf(out, cap, "%s\"%s%s\"::", node ? node : "", user,
                 has_password ? " password" : "");
    else
        snprintf(out, cap, "%s::", node ? node : "");
}

uint32_t dnet_fal_dap_rms(uint16_t stscode, uint64_t stv, uint32_t fallback)
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

/* ---- messages -------------------------------------------------------------- */

/* One continuation line, "-FAC-S-IDENT, text", from the message catalog. */
static void put_reason(FILE *out, uint32_t cond)
{
    char txt[300];
    uint16_t tl = 0;
    struct dsc$descriptor_s d;
    d.dsc$w_length = (uint16_t)(sizeof txt - 1);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = txt;
    (void)sys$getmsg(cond, &tl, &d, 15, NULL);   /* NOMSG text when unknown */
    txt[tl < sizeof txt ? tl : sizeof txt - 1] = '\0';
    if (txt[0] == '%') txt[0] = '-';
    fprintf(out, "%s\n", txt);
}

/* A link the remote would not open: RMS's search failed with FND, and why. */
static void put_link_failure(FILE *out, const char *fac, const char *what, uint32_t st)
{
    fprintf(out, "%%%s-W-SEARCHFAIL, error searching for %s\n", fac, what);
    put_reason(out, DNET_RMS_FND);
    put_reason(out, st);
}

/* ---- the DIRECTORY LIST collector --------------------------------------- */

struct dl_group { char vol[DNET_DAP_MAX_SPEC + 1], dir[DNET_DAP_MAX_SPEC + 1]; };
struct dl_entry { size_t group; char *name; };

struct dirlist {
    char vol[DNET_DAP_MAX_SPEC + 1], dir[DNET_DAP_MAX_SPEC + 1];
    char pending[DNET_DAP_MAX_SPEC + 1];
    int  have_pending;
    struct dl_group *groups; size_t ngroups, gcap;
    struct dl_entry *ents;   size_t nents, ecap;
    int  oom, capped;
};

static void dl_free(struct dirlist *d)
{
    for (size_t i = 0; i < d->nents; i++) free(d->ents[i].name);
    free(d->ents);
    free(d->groups);
    memset(d, 0, sizeof *d);
}

static void dl_commit(struct dirlist *d)
{
    if (!d->have_pending) return;
    d->have_pending = 0;
    if (d->nents >= DNET_FAL_DIR_MAXFILES) { d->capped = 1; return; }
    if (d->ngroups == 0 || strcmp(d->groups[d->ngroups - 1].vol, d->vol) ||
        strcmp(d->groups[d->ngroups - 1].dir, d->dir)) {
        if (d->ngroups == d->gcap) {
            size_t nc = d->gcap ? d->gcap * 2 : 4;
            struct dl_group *g = realloc(d->groups, nc * sizeof *g);
            if (!g) { d->oom = 1; return; }
            d->groups = g; d->gcap = nc;
        }
        memcpy(d->groups[d->ngroups].vol, d->vol, sizeof d->vol);
        memcpy(d->groups[d->ngroups].dir, d->dir, sizeof d->dir);
        d->ngroups++;
    }
    if (d->nents == d->ecap) {
        size_t nc = d->ecap ? d->ecap * 2 : 16;
        struct dl_entry *e = realloc(d->ents, nc * sizeof *e);
        if (!e) { d->oom = 1; return; }
        d->ents = e; d->ecap = nc;
    }
    char *nm = strdup(d->pending);
    if (!nm) { d->oom = 1; return; }
    d->ents[d->nents].group = d->ngroups - 1;
    d->ents[d->nents].name = nm;
    d->nents++;
}

/* A file NAME is held until the next message: one followed by the search's
 * STATUS is the spec that matched nothing (the VAX names NOSUCH.TXT;* before
 * its FNF), not a file. */
static int dl_on_name(void *ctx, unsigned nametype, const char *spec)
{
    struct dirlist *d = ctx;
    size_t n = strnlen(spec, DNET_DAP_MAX_SPEC);
    switch (nametype) {
    case DNET_DAP_NT_VOLUME:
        dl_commit(d);
        memcpy(d->vol, spec, n); d->vol[n] = '\0';
        break;
    case DNET_DAP_NT_DIRECTORY:
        dl_commit(d);
        memcpy(d->dir, spec, n); d->dir[n] = '\0';
        break;
    case DNET_DAP_NT_FILENAME:
        dl_commit(d);
        memcpy(d->pending, spec, n); d->pending[n] = '\0';
        d->have_pending = 1;
        break;
    default:
        break;                    /* a full FILESPEC NAME: not asked for */
    }
    return d->oom ? -1 : 0;
}

/* Run the DIRECTORY LIST on a fresh link. *link_st is the connect failure (the
 * list did not run); otherwise the list's own status and STATUS codes. */
static uint32_t dl_run(const struct dnet_fal_linker *lk, const char *wire_spec,
                       struct dirlist *d, uint32_t *link_st,
                       uint16_t *stscode, uint64_t *stv)
{
    memset(d, 0, sizeof *d);
    *link_st = SS$_NORMAL;
    *stscode = 0; *stv = 0;
    uint32_t lst = SS$_ABORT;
    struct dnet_dap_transport *t = lk->open(lk->ctx, &lst);
    if (!t) { *link_st = (lst & 1) ? SS$_ABORT : lst; return *link_st; }
    uint32_t st = dnet_fal_client_dirlist_cb(wire_spec, t, dl_on_name, d, stscode, stv);
    lk->close(lk->ctx, t);
    if (st == SS$_NORMAL) dl_commit(d);
    return st;
}

static int is_wild(const char *s)
{
    return strpbrk(s, "*%") != NULL || strstr(s, "...") != NULL;
}

/* ---- DIRECTORY ----------------------------------------------------------- */

#define DIR_COLS   4
#define DIR_CELL  20

static void dir_print_group(FILE *out, const struct dirlist *d, size_t g,
                            const char *pfx, size_t *count)
{
    fprintf(out, "\nDirectory %s%s%s\n\n", pfx, d->groups[g].vol, d->groups[g].dir);
    int col = 0;
    size_t n = 0;
    for (size_t i = 0; i < d->nents; i++) {
        if (d->ents[i].group != g) continue;
        const char *nm = d->ents[i].name;
        size_t len = strlen(nm);
        int cells = (int)(len / DIR_CELL) + 1;
        if (cells > DIR_COLS) cells = DIR_COLS;
        if (col > 0 && col + cells > DIR_COLS) { fputc('\n', out); col = 0; }
        /* Every name is padded to its column stop, the row's last included:
         * the VAX's DIRECTORY of a remote directory ends each row with the
         * blanks (falverbs-v2v-vax1-console.txt, "BRKP.TXT;1          "). */
        int pad = cells * DIR_CELL - (int)len;
        fprintf(out, "%s%*s", nm, pad > 0 ? pad : 1, "");
        col += cells;
        if (col >= DIR_COLS) { fputc('\n', out); col = 0; }
        n++;
    }
    if (col > 0) fputc('\n', out);
    fprintf(out, "\nTotal of %zu file%s.\n", n, n == 1 ? "" : "s");
    *count = n;
}

uint32_t dnet_fal_directory(FILE *out, const struct dnet_fal_linker *lk,
                            const char *nodepfx, const char *spec)
{
    if (!out || !lk || !lk->open || !lk->close || !nodepfx || !spec) return SS$_BADPARAM;
    char wire[DNET_DAP_MAX_SPEC + 1];
    if (dnet_fal_default_spec(spec, DNET_FAL_VERB_DIRECTORY, wire, sizeof wire) != 0)
        return SS$_BADPARAM;

    struct dirlist d;
    uint32_t link_st;
    uint16_t sc; uint64_t sv;
    uint32_t st = dl_run(lk, wire, &d, &link_st, &sc, &sv);
    if (!(link_st & 1)) {
        char what[600];
        snprintf(what, sizeof what, "%s%s", nodepfx, wire);
        put_link_failure(out, "DIRECT", what, link_st);
        dl_free(&d);
        return link_st;
    }

    size_t total = 0;
    for (size_t g = 0; g < d.ngroups; g++) {
        size_t n = 0;
        dir_print_group(out, &d, g, nodepfx, &n);
        total += n;
    }
    if (d.ngroups > 1)
        fprintf(out, "\nGrand total of %zu directories, %zu file%s.\n",
                d.ngroups, total, total == 1 ? "" : "s");

    uint32_t ret = SS$_NORMAL;
    if (st != SS$_NORMAL) {
        if (d.nents == 0 && DNET_DAP_MIC(sc) == DNET_DAP_MIC_FNF && sc) {
            fprintf(out, "%%DIRECT-W-NOFILES, no files found\n");
            ret = RMS$_FNF;
        } else {
            /* The search failed and said why (or the link broke). */
            char what[900];
            snprintf(what, sizeof what, "%s%s%s%s", nodepfx, d.vol, d.dir,
                     d.have_pending ? d.pending : (d.vol[0] || d.dir[0] ? "" : wire));
            fprintf(out, "%%DIRECT-W-SEARCHFAIL, error searching for %s\n", what);
            ret = sc ? dnet_fal_dap_rms(sc, sv, st) : st;
            put_reason(out, ret);
        }
    }
    fflush(out);
    dl_free(&d);
    return ret;
}

/* ---- TYPE ------------------------------------------------------------------ */

struct type_sink {
    FILE *out;
    const char *header;     /* NULL: no per-file header (not a wildcard) */
    uint8_t rfm, fsz;
    int began;
};

static int type_begin(void *ctx, uint8_t rfm, uint8_t fsz)
{
    struct type_sink *s = ctx;
    s->rfm = rfm; s->fsz = fsz;
    s->began = 1;
    /* The wildcard header, as the VAX printed it: a line holding one blank,
     * the file's full name, another one-blank line (falverbs-v2v-vax1-console
     * .txt and falverbs-live3-vax1-console.txt, both). */
    if (s->header) fprintf(s->out, " \n%s\n \n", s->header);
    return 0;
}

static int type_record(void *ctx, const uint8_t *rec, size_t len)
{
    struct type_sink *s = ctx;
    if (s->rfm == DNET_DAP_RFM_VFC) {          /* drop the fixed control area */
        size_t skip = s->fsz;
        if (skip > len) skip = len;
        rec += skip; len -= skip;
    }
    if (len) fwrite(rec, 1, len, s->out);
    fputc('\n', s->out);
    return ferror(s->out) ? -1 : 0;
}

uint32_t dnet_fal_type(FILE *out, const struct dnet_fal_linker *lk,
                       const char *nodepfx, const char *spec)
{
    if (!out || !lk || !lk->open || !lk->close || !nodepfx || !spec) return SS$_BADPARAM;
    char wire[DNET_DAP_MAX_SPEC + 1];
    if (dnet_fal_default_spec(spec, DNET_FAL_VERB_TYPE, wire, sizeof wire) != 0)
        return SS$_BADPARAM;

    struct dirlist d;
    uint32_t link_st;
    uint16_t sc; uint64_t sv;
    uint32_t st = dl_run(lk, wire, &d, &link_st, &sc, &sv);
    char what[900];
    if (!(link_st & 1)) {
        snprintf(what, sizeof what, "%s%s", nodepfx, wire);
        put_link_failure(out, "TYPE", what, link_st);
        fflush(out);
        dl_free(&d);
        return link_st;
    }
    if (st != SS$_NORMAL && d.nents == 0) {
        snprintf(what, sizeof what, "%s%s%s%s", nodepfx, d.vol, d.dir,
                 d.have_pending ? d.pending : (d.vol[0] || d.dir[0] ? "" : wire));
        fprintf(out, "%%TYPE-W-SEARCHFAIL, error searching for %s\n", what);
        uint32_t why = sc ? dnet_fal_dap_rms(sc, sv, st) : st;
        put_reason(out, why);
        fflush(out);
        dl_free(&d);
        return why;
    }

    int wild = is_wild(spec) || d.nents > 1;
    uint32_t ret = SS$_NORMAL;
    struct dnet_dap_transport *t = NULL;
    for (size_t i = 0; i < d.nents; i++) {
        const struct dl_group *g = &d.groups[d.ents[i].group];
        char full[3 * (DNET_DAP_MAX_SPEC + 1)];
        snprintf(full, sizeof full, "%s%s%s", g->vol, g->dir, d.ents[i].name);
        snprintf(what, sizeof what, "%s%s", nodepfx, full);
        if (strlen(full) > DNET_DAP_MAX_SPEC) {
            fprintf(out, "%%TYPE-W-OPENIN, error opening %s as input\n", what);
            put_reason(out, RMS$_FNF);
            ret = RMS$_FNF;
            continue;
        }
        if (!t) {
            uint32_t lst = SS$_ABORT;
            t = lk->open(lk->ctx, &lst);
            if (!t) {
                if (lst & 1) lst = SS$_ABORT;
                fprintf(out, "%%TYPE-W-OPENIN, error opening %s as input\n", what);
                put_reason(out, DNET_RMS_FND);
                put_reason(out, lst);
                ret = lst;
                break;
            }
        }
        struct type_sink s = { out, wild ? what : NULL, DNET_DAP_RFM_VAR, 0, 0 };
        struct dnet_fal_get_sink sink = { type_begin, type_record, &s };
        uint16_t fsc = 0; uint64_t fsv = 0;
        uint32_t fst = dnet_fal_client_get_cb(full, t, &sink, &fsc, &fsv);
        if (fst == SS$_NORMAL) continue;
        uint32_t why = fsc ? dnet_fal_dap_rms(fsc, fsv, fst) : fst;
        if (fst == SS$_NOSUCHFILE && !fsc) why = RMS$_FNF;
        fprintf(out, s.began ? "%%TYPE-W-READERR, error reading %s\n"
                             : "%%TYPE-W-OPENIN, error opening %s as input\n", what);
        put_reason(out, why);
        ret = why;
        if (!fsc) break;               /* the link itself failed: stop here */
    }
    if (t) lk->close(lk->ctx, t);
    fflush(out);
    dl_free(&d);
    return ret;
}
