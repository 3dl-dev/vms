/*
 * dnet_fal_search.c - file name resolution for the FAL server (rd vms-d85
 * inbound): RMS $PARSE + $SEARCH over the Files-11 ACP, so every NAME the FAL
 * server puts on the wire -- the directory list a real VMS COPY asks for and
 * the resultant spec of an OPEN -- is the RESULTANT string RMS returned, with
 * the real device, directory and version. Nothing is composed or guessed; with
 * no executive the $PARSE fails and the search reports "no files" honestly.
 */
#include <stdlib.h>
#include <string.h>

#include "dnet_fal.h"
#include "rms/rms.h"

struct fal_search {
    struct FAB fab;
    struct NAM nam;
    char spec[DNET_DAP_MAX_SPEC + 1];
    char esa[NAM$C_MAXESS + 1];
    char rsa[NAM$C_MAXRSS + 1];
};

int dnet_fal_search_begin(const char *spec, void **ctx)
{
    if (!spec || !ctx || strlen(spec) > DNET_DAP_MAX_SPEC) return -1;
    struct fal_search *s = calloc(1, sizeof *s);
    if (!s) return -1;
    strcpy(s->spec, spec);
    s->fab = cc$rms_fab;
    s->fab.fab$l_fna = s->spec;
    s->fab.fab$b_fns = (uint8_t)strlen(s->spec);
    s->nam = cc$rms_nam;
    s->nam.nam$l_esa = s->esa;
    s->nam.nam$b_ess = NAM$C_MAXESS;
    s->nam.nam$l_rsa = s->rsa;
    s->nam.nam$b_rss = NAM$C_MAXRSS;
    s->fab.fab$l_nam = &s->nam;
    if (sys$parse(&s->fab, 0, 0) != RMS$_NORMAL) {
        rms_search_end(&s->nam);
        free(s);
        return -1;
    }
    *ctx = s;
    return 0;
}

int dnet_fal_search_next(void *ctx, char *rsa, size_t cap)
{
    struct fal_search *s = ctx;
    if (!s || !rsa || cap == 0) return -1;
    if (sys$search(&s->fab, 0, 0) != RMS$_NORMAL) return -1;   /* RMS$_NMF / error */
    size_t n = s->nam.nam$b_rsl;
    if (n >= cap) return -1;
    memcpy(rsa, s->rsa, n);
    rsa[n] = '\0';
    return 0;
}

void dnet_fal_search_end(void *ctx)
{
    struct fal_search *s = ctx;
    if (!s) return;
    rms_search_end(&s->nam);
    free(s);
}

/* ---- record output for a FAL CREATE (one RMS stream for the whole access) --
 * The file is $CREATEd when the ACCESS(CREATE) arrives -- so the NAME the
 * accessor may ask for is the RESULTANT spec of the file actually created
 * (with its real version) -- and every DATA record is $PUT to that one open
 * stream, records verbatim (embedded NULs included). $CLOSE at ACCESS COMPLETE.
 */
struct fal_wfile {
    struct FAB fab;
    struct RAB rab;
    struct NAM nam;
    char spec[DNET_DAP_MAX_SPEC + 1];
    char esa[NAM$C_MAXESS + 1];
    char rsa[NAM$C_MAXRSS + 1];
};

int dnet_fal_wopen(const char *spec, uint8_t rfm, uint8_t rat, void **h,
                   char *rsa, size_t cap)
{
    if (!spec || !h || strlen(spec) > DNET_DAP_MAX_SPEC) return -1;
    struct fal_wfile *w = calloc(1, sizeof *w);
    if (!w) return -1;
    strcpy(w->spec, spec);
    w->fab = cc$rms_fab;
    w->fab.fab$l_fna = w->spec;
    w->fab.fab$b_fns = (uint8_t)strlen(w->spec);
    w->fab.fab$b_org = FAB$C_SEQ;
    w->fab.fab$b_rfm = rfm;
    w->fab.fab$b_rat = rat;
    w->fab.fab$b_fac = FAB$M_PUT;
    w->nam = cc$rms_nam;
    w->nam.nam$l_esa = w->esa;
    w->nam.nam$b_ess = NAM$C_MAXESS;
    w->nam.nam$l_rsa = w->rsa;
    w->nam.nam$b_rss = NAM$C_MAXRSS;
    w->fab.fab$l_nam = &w->nam;
    if (!(sys$create(&w->fab, 0, 0) & 1)) { free(w); return -1; }
    w->rab = cc$rms_rab;
    w->rab.rab$l_fab = &w->fab;
    if (!(sys$connect(&w->rab, 0, 0) & 1)) { sys$close(&w->fab, 0, 0); free(w); return -1; }
    if (rsa && cap) {
        size_t n = w->nam.nam$b_rsl ? w->nam.nam$b_rsl : 0;
        if (n >= cap) n = cap - 1;
        if (n) memcpy(rsa, w->rsa, n);
        rsa[n] = '\0';
    }
    *h = w;
    return 0;
}

int dnet_fal_wput(void *h, const uint8_t *rec, size_t len)
{
    struct fal_wfile *w = h;
    if (!w || len > 0xffff) return -1;
    w->rab.rab$l_rbf = (char *)rec;
    w->rab.rab$w_rsz = (uint16_t)len;
    return (sys$put(&w->rab, 0, 0) & 1) ? 0 : -1;
}

int dnet_fal_wclose(void *h)
{
    struct fal_wfile *w = h;
    if (!w) return -1;
    uint32_t st = sys$close(&w->fab, 0, 0);
    free(w);
    return (st & 1) ? 0 : -1;
}

/* ---- record input for a FAL OPEN ---------------------------------------- */
struct fal_rfile {
    struct FAB fab;
    struct RAB rab;
    char spec[DNET_DAP_MAX_SPEC + 1];
};

int dnet_fal_ropen(const char *spec, void **h, uint8_t *rfm, uint8_t *rat)
{
    if (!spec || !h || strlen(spec) > DNET_DAP_MAX_SPEC) return -1;
    struct fal_rfile *r = calloc(1, sizeof *r);
    if (!r) return -1;
    strcpy(r->spec, spec);
    r->fab = cc$rms_fab;
    r->fab.fab$l_fna = r->spec;
    r->fab.fab$b_fns = (uint8_t)strlen(r->spec);
    r->fab.fab$b_fac = FAB$M_GET;
    r->fab.fab$b_shr = FAB$M_SHRGET;
    /* The file's own record format, from its Files-11 header (FAT). VMS $OPEN
     * fills these FAB fields itself; OVMX RMS $OPEN does not yet (rd vms-158)
     * and $GET frames by the FAB's rfm -- so a VAR file opened with the
     * default FAB read back as ONE record of raw length-prefixed bytes (seen
     * live: a real VAX COPY from a booted OVMX). Setting them from the header
     * before $OPEN is what VMS would leave there anyway. */
    {
        struct rms_fileattr fa;
        memset(&fa, 0, sizeof fa);
        if (rms_file_attr(r->spec, &fa) & 1) {
            r->fab.fab$b_rfm = fa.rfm;
            r->fab.fab$b_rat = fa.rat;
            r->fab.fab$w_mrs = fa.mrs;
        }
    }
    if (!(sys$open(&r->fab, 0, 0) & 1)) { free(r); return -1; }
    r->rab = cc$rms_rab;
    r->rab.rab$l_fab = &r->fab;
    if (!(sys$connect(&r->rab, 0, 0) & 1)) { sys$close(&r->fab, 0, 0); free(r); return -1; }
    if (rfm) *rfm = r->fab.fab$b_rfm;
    if (rat) *rat = r->fab.fab$b_rat;
    *h = r;
    return 0;
}

int dnet_fal_rget(void *h, uint8_t *rec, size_t cap, size_t *len)
{
    struct fal_rfile *r = h;
    if (!r || !rec || !len || cap == 0) return -1;
    r->rab.rab$l_ubf = (char *)rec;
    r->rab.rab$w_usz = (uint16_t)(cap > 0xffff ? 0xffff : cap);
    uint32_t st = sys$get(&r->rab, 0, 0);
    if (st == RMS$_EOF) return 0;
    if (!(st & 1)) return -1;
    *len = r->rab.rab$w_rsz;
    return 1;
}

int dnet_fal_rclose(void *h)
{
    struct fal_rfile *r = h;
    if (!r) return -1;
    uint32_t st = sys$close(&r->fab, 0, 0);
    free(r);
    return (st & 1) ? 0 : -1;
}
