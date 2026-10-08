/*
 * sys_logical.c - Logical Name System Services: $CRELNM, $DELLNM, $TRNLNM.
 *
 * TABLES (rd vms-ef21). Every table these services reach is the executive's:
 *   LNM$PROCESS_TABLE  keyed by the VMS PID -- DCL and every image it activates
 *                      are ONE VMS process, so a name DCL (or LOGINOUT) created
 *                      is visible to the image, and a supervisor/executive-mode
 *                      name the image creates outlives it; user-mode names go
 *                      at image rundown (the executive's vms_lnm_rundown).
 *   LNM$JOB_xxxxxxxx   keyed by the job (vms-aba)
 *   LNM$GROUP_gggggg   keyed by the UIC group (vms-aba)
 *   LNM$SYSTEM_TABLE   node-wide (vms-d37)
 * Create/delete are /dev/vms ioctls; translation reads the read-only arena.
 *
 * TABLE NAMES. The table argument is itself a logical name, resolved the way
 * VMS does through the logical-name directories: LNM$PROCESS, LNM$JOB,
 * LNM$GROUP and LNM$SYSTEM name the four tables, LNM$FILE_DEV (and its alias
 * LNM$DCL_LOGICAL) is the search list PROCESS, JOB, GROUP, SYSTEM, and
 * LNM$PROCESS_DIRECTORY / LNM$SYSTEM_DIRECTORY / LNM$DIRECTORIES are the
 * directories themselves. A table name that resolves to nothing is
 * SS$_NOLOGTAB on $CRELNM and SS$_NOLOGNAM on $TRNLNM/$DELLNM.
 *
 * NAMES are case-sensitive (LNM$M_CASE_BLIND on $TRNLNM folds case) and exist
 * once per access mode: $CRELNM supersedes only the name at the same mode,
 * $TRNLNM ignores names at modes outer than the one asked for and returns the
 * outermost remaining one, $DELLNM deletes at the given mode and every outer
 * mode (all names when no name is given). Behaviour observed on OpenVMS VAX
 * V7.3 and Alpha V8.4: docs/oracle/semantics/lnm/ (rd vms-8d1).
 *
 * NO EXECUTIVE (host build/test tooling, no /dev/vms): LNM$JOB/GROUP/SYSTEM
 * fail SS$_NOSUCHDEV (CLAUDE.md Rule 9 / INV-6); LNM$PROCESS_TABLE is then
 * this process's own table, a small store below with the same rules -- with
 * no executive there is no other process to share a process table with.
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-PARTIAL: sys$crelnm (vms-ef21) -- exec: every table's storage is the
 *     executive's (vms_kif_lnm_define): PROCESS by VMS PID, JOB, GROUP, SYSTEM.
 * OVMX-LOCAL: sys$crelnm -- item-list parsing and table-name resolution are
 *     done here; with no /dev/vms LNM$PROCESS_TABLE is the process-local store.
 * OVMX-PARTIAL: sys$dellnm (vms-ef21) -- exec: deletes go to the executive
 *     (vms_kif_lnm_delete), which applies the access-mode rule.
 * OVMX-LOCAL: sys$dellnm -- table-name resolution here; with no /dev/vms
 *     LNM$PROCESS_TABLE is the process-local store.
 * OVMX-PARTIAL: sys$trnlnm (vms-ef21) -- exec: names are read from the
 *     executive's arena (vms_kif_lnm_lookup), mode- and case-filtered.
 * OVMX-LOCAL: sys$trnlnm -- table-name resolution, the directory tables'
 *     fixed entries and the item-list fill are done here; with no /dev/vms
 *     LNM$PROCESS_TABLE is the process-local store.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include "starlet.h"
#include "str_util.h"
#include "vms_kif.h"

#define LNM_MAXVAL   VMS_LNM_MAX_EQUIV   /* equivalence strings per name */

/* ---- table resolution ----------------------------------------------- */

enum lnm_tab {
    TAB_PROC = 1, TAB_JOB, TAB_GROUP, TAB_SYS,     /* real tables          */
    TAB_PDIR, TAB_SDIR                              /* the two directories  */
};

static uint32_t tab_exec_id(int t)
{
    switch (t) {
    case TAB_PROC:  return VMS_LNM_TBL_PROCESS;
    case TAB_JOB:   return VMS_LNM_TBL_JOB;
    case TAB_GROUP: return VMS_LNM_TBL_GROUP;
    case TAB_SYS:   return VMS_LNM_TBL_SYSTEM;
    default:        return 0;
    }
}

/* The name of table `t` as $TRNLNM's LNM$_TABLE reports it. */
static void tab_name(int t, char *out, size_t outsz)
{
    uint32_t key = 0;
    switch (t) {
    case TAB_PROC: snprintf(out, outsz, "LNM$PROCESS_TABLE"); break;
    case TAB_SYS:  snprintf(out, outsz, "LNM$SYSTEM_TABLE"); break;
    case TAB_PDIR: snprintf(out, outsz, "LNM$PROCESS_DIRECTORY"); break;
    case TAB_SDIR: snprintf(out, outsz, "LNM$SYSTEM_DIRECTORY"); break;
    case TAB_JOB:
        (void)vms_kif_lnm_scope_key(VMS_LNM_TBL_JOB, &key);
        snprintf(out, outsz, "LNM$JOB_%08X", (unsigned)key);
        break;
    case TAB_GROUP:
        (void)vms_kif_lnm_scope_key(VMS_LNM_TBL_GROUP, &key);
        snprintf(out, outsz, "LNM$GROUP_%06o", (unsigned)key);
        break;
    default: out[0] = '\0'; break;
    }
}

/* Table-level attributes: the shareable tables carry LNM$M_SHAREABLE, which
 * $TRNLNM folds into a name's LNM$_ATTRIBUTES (observed: a LNM$JOB or
 * LNM$SYSTEM name reports %X10400, a LNM$PROCESS one %X400). */
static uint32_t tab_attr(int t)
{
    return (t == TAB_JOB || t == TAB_GROUP || t == TAB_SYS || t == TAB_SDIR)
               ? LNM$M_SHAREABLE : 0;
}

/*
 * Resolve a table-name argument into the ordered list of tables it denotes.
 * Returns the count (0 = no such table).
 */
static int resolve_tables(const char *t, int out[4])
{
    char buf[64];
    uint32_t key = 0;

    if (!strcmp(t, "LNM$PROCESS_TABLE") || !strcmp(t, "LNM$PROCESS")) {
        out[0] = TAB_PROC; return 1;
    }
    if (!strcmp(t, "LNM$SYSTEM_TABLE") || !strcmp(t, "LNM$SYSTEM")) {
        out[0] = TAB_SYS; return 1;
    }
    if (!strcmp(t, "LNM$JOB")) { out[0] = TAB_JOB; return 1; }
    if (!strcmp(t, "LNM$GROUP")) { out[0] = TAB_GROUP; return 1; }
    if (!strncmp(t, "LNM$JOB_", 8)) {
        tab_name(TAB_JOB, buf, sizeof buf);
        if (!strcmp(t, buf)) { out[0] = TAB_JOB; return 1; }
        return 0;
    }
    if (!strncmp(t, "LNM$GROUP_", 10)) {
        tab_name(TAB_GROUP, buf, sizeof buf);
        if (!strcmp(t, buf)) { out[0] = TAB_GROUP; return 1; }
        return 0;
    }
    if (!strcmp(t, "LNM$FILE_DEV") || !strcmp(t, "LNM$DCL_LOGICAL")) {
        out[0] = TAB_PROC; out[1] = TAB_JOB; out[2] = TAB_GROUP; out[3] = TAB_SYS;
        return 4;
    }
    if (!strcmp(t, "LNM$PROCESS_DIRECTORY")) { out[0] = TAB_PDIR; return 1; }
    if (!strcmp(t, "LNM$SYSTEM_DIRECTORY")) { out[0] = TAB_SDIR; return 1; }
    if (!strcmp(t, "LNM$DIRECTORIES")) {
        out[0] = TAB_PDIR; out[1] = TAB_SDIR; return 2;
    }
    (void)key;
    return 0;
}

/* One found name, whatever table it came from. */
struct lnm_hit {
    int      tab;
    uint32_t attributes;
    uint8_t  acmode;
    uint8_t  nvals;
    char     vals[LNM_MAXVAL][LNM$C_NAMLENGTH + 1];
    uint16_t vlen[LNM_MAXVAL];    /* byte lengths: a value may hold NUL */
};

/*
 * The directory tables' entries. OVMX's directories are not a store a
 * program can add to ($CRELNT is not provided); they hold exactly the table
 * names and table-name logicals resolve_tables() implements, so this is that
 * function's own content seen through $TRNLNM. Attributes and modes as
 * observed for the two probed entries (LNM$PROCESS_TABLE in
 * LNM$PROCESS_DIRECTORY: a table, %X9, kernel mode, no equivalence string;
 * LNM$FILE_DEV in LNM$SYSTEM_DIRECTORY: supervisor mode).
 */
static int dir_lookup(int dir, const char *name, int case_blind,
                      uint8_t maxmode, struct lnm_hit *h)
{
    int (*eq)(const char *, const char *) = case_blind ? strcasecmp : strcmp;
    char jobt[32], grpt[32];

    memset(h, 0, sizeof *h);
    h->tab = dir;
    tab_name(TAB_JOB, jobt, sizeof jobt);
    tab_name(TAB_GROUP, grpt, sizeof grpt);

    if (dir == TAB_PDIR) {
        if (!eq(name, "LNM$PROCESS_TABLE")) {
            h->attributes = LNM$M_TABLE | LNM$M_NO_ALIAS; h->acmode = PSL$C_KERNEL;
        } else if (!eq(name, "LNM$PROCESS")) {
            h->acmode = PSL$C_KERNEL; h->nvals = 1;
            strcpy(h->vals[0], "LNM$PROCESS_TABLE");
        } else if (!eq(name, "LNM$JOB")) {
            h->acmode = PSL$C_KERNEL; h->nvals = 1; strcpy(h->vals[0], jobt);
        } else if (!eq(name, "LNM$GROUP")) {
            h->acmode = PSL$C_KERNEL; h->nvals = 1; strcpy(h->vals[0], grpt);
        } else
            return 0;
    } else {
        if (!eq(name, "LNM$SYSTEM_TABLE") || !eq(name, jobt) || !eq(name, grpt)) {
            h->attributes = LNM$M_TABLE | LNM$M_NO_ALIAS; h->acmode = PSL$C_KERNEL;
        } else if (!eq(name, "LNM$SYSTEM")) {
            h->acmode = PSL$C_KERNEL; h->nvals = 1;
            strcpy(h->vals[0], "LNM$SYSTEM_TABLE");
        } else if (!eq(name, "LNM$FILE_DEV")) {
            h->acmode = PSL$C_SUPER; h->nvals = 4;
            strcpy(h->vals[0], "LNM$PROCESS"); strcpy(h->vals[1], "LNM$JOB");
            strcpy(h->vals[2], "LNM$GROUP");   strcpy(h->vals[3], "LNM$SYSTEM");
        } else if (!eq(name, "LNM$DCL_LOGICAL")) {
            h->acmode = PSL$C_SUPER; h->nvals = 1; strcpy(h->vals[0], "LNM$FILE_DEV");
        } else if (!eq(name, "LNM$DIRECTORIES")) {
            h->acmode = PSL$C_KERNEL; h->nvals = 2;
            strcpy(h->vals[0], "LNM$PROCESS_DIRECTORY");
            strcpy(h->vals[1], "LNM$SYSTEM_DIRECTORY");
        } else
            return 0;
    }
    for (int k = 0; k < h->nvals; k++)
        h->vlen[k] = (uint16_t)strlen(h->vals[k]);
    return h->acmode <= maxmode;
}

/* ---- the no-executive LNM$PROCESS_TABLE store ------------------------ */

#define LOCAL_MAX 256
static struct vms_kif_lnm_enum_rec local_tab[LOCAL_MAX];
static uint8_t local_used[LOCAL_MAX];
static pthread_mutex_t local_mx = PTHREAD_MUTEX_INITIALIZER;

static int local_mode(uint8_t m) { return m > PSL$C_USER ? PSL$C_USER : m; }

static uint32_t local_define(const char *name, const char *const *vals,
                             const uint16_t *lens, uint8_t n, uint32_t attr, uint8_t mode)
{
    int i, freei = -1;
    uint32_t st = SS$_NORMAL;

    mode = (uint8_t)local_mode(mode);
    pthread_mutex_lock(&local_mx);
    for (i = 0; i < LOCAL_MAX; i++) {
        if (!local_used[i]) { if (freei < 0) freei = i; continue; }
        if (local_tab[i].acmode == mode && !strcmp(local_tab[i].name, name)) {
            freei = i; st = SS$_SUPERSEDE; break;
        }
    }
    if (freei < 0) { pthread_mutex_unlock(&local_mx); return SS$_EXQUOTA; }
    memset(&local_tab[freei], 0, sizeof local_tab[freei]);
    snprintf(local_tab[freei].name, sizeof local_tab[freei].name, "%s", name);
    for (i = 0; i < n; i++) {
        size_t vl = lens[i];
        if (vl >= sizeof local_tab[freei].values[i])
            vl = sizeof local_tab[freei].values[i] - 1;
        memcpy(local_tab[freei].values[i], vals[i], vl);
        local_tab[freei].values[i][vl] = '\0';
        local_tab[freei].value_len[i] = (uint16_t)vl;
    }
    local_tab[freei].num_values = n;
    local_tab[freei].attributes = attr;
    local_tab[freei].acmode = mode;
    local_used[freei] = 1;
    pthread_mutex_unlock(&local_mx);
    return st;
}

static uint32_t local_delete(const char *name, uint8_t mode)
{
    int i, n = 0;
    mode = (uint8_t)local_mode(mode);
    pthread_mutex_lock(&local_mx);
    for (i = 0; i < LOCAL_MAX; i++) {
        if (!local_used[i] || local_tab[i].acmode < mode)
            continue;
        if (name && strcmp(local_tab[i].name, name))
            continue;
        local_used[i] = 0; n++;
    }
    pthread_mutex_unlock(&local_mx);
    return (n || !name) ? SS$_NORMAL : SS$_NOLOGNAM;
}

static int local_lookup(const char *name, int case_blind, uint8_t maxmode,
                        struct vms_kif_lnm_enum_rec *out)
{
    int i, best = -1;
    pthread_mutex_lock(&local_mx);
    for (i = 0; i < LOCAL_MAX; i++) {
        if (!local_used[i] || local_tab[i].acmode > maxmode)
            continue;
        if (case_blind ? strcasecmp(local_tab[i].name, name)
                       : strcmp(local_tab[i].name, name))
            continue;
        if (best < 0 || local_tab[i].acmode > local_tab[best].acmode)
            best = i;
    }
    if (best >= 0)
        *out = local_tab[best];
    pthread_mutex_unlock(&local_mx);
    return best >= 0;
}

/* ---- storage dispatch ------------------------------------------------ */

static int proc_is_local(void) { return !vms_kif_lnm_present(); }

static uint32_t tab_define(int t, const char *name, const char *const *vals,
                           const uint16_t *lens, uint8_t n, uint32_t attr, uint8_t mode)
{
    if (t == TAB_PROC && proc_is_local())
        return local_define(name, vals, lens, n, attr, mode);
    if (!tab_exec_id(t))
        return SS$_IVLOGTAB;              /* a directory: no $CRELNT here */
    return vms_kif_lnm_define_n(tab_exec_id(t), name, vals, lens, n, attr, mode);
}

static uint32_t tab_delete(int t, const char *name, uint8_t mode)
{
    if (t == TAB_PROC && proc_is_local())
        return local_delete(name, mode);
    if (!tab_exec_id(t))
        return SS$_NOLOGNAM;
    return vms_kif_lnm_delete(tab_exec_id(t), name, mode);
}

/* 1 found, 0 not found, or a failure status (> 1, even) to return. */
static uint32_t tab_lookup(int t, const char *name, int case_blind,
                           uint8_t maxmode, struct lnm_hit *h)
{
    struct vms_kif_lnm_enum_rec rec;
    int r, k;

    if (t == TAB_PDIR || t == TAB_SDIR)
        return (uint32_t)dir_lookup(t, name, case_blind, maxmode, h);
    if (t == TAB_PROC && proc_is_local())
        r = local_lookup(name, case_blind, maxmode, &rec);
    else
        r = vms_kif_lnm_lookup(tab_exec_id(t), name, case_blind, maxmode, &rec);
    if (r < 0)
        return SS$_NOSUCHDEV;                       /* executive absent */
    if (r == 0)
        return 0;
    memset(h, 0, sizeof *h);
    h->tab = t;
    h->attributes = rec.attributes;
    h->acmode = rec.acmode;
    h->nvals = rec.num_values > LNM_MAXVAL ? LNM_MAXVAL : rec.num_values;
    for (k = 0; k < h->nvals; k++) {
        size_t vl = rec.value_len[k];
        if (vl >= sizeof h->vals[k])
            vl = sizeof h->vals[k] - 1;
        memcpy(h->vals[k], rec.values[k], vl);
        h->vals[k][vl] = '\0';
        h->vlen[k] = (uint16_t)vl;
    }
    return 1;
}

/* ---- argument helpers ------------------------------------------------ */

/* Copy a descriptor's text; returns its length, or -1 for a missing one. */
static int dsc_text(const struct dsc$descriptor_s *d, char *out, size_t outsz)
{
    size_t n;
    if (!d || !d->dsc$a_pointer)
        return -1;
    n = d->dsc$w_length;
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, d->dsc$a_pointer, n);
    out[n] = '\0';
    return (int)d->dsc$w_length;
}

static uint8_t req_mode(const uint8_t *acmode)
{
    if (!acmode)
        return PSL$C_USER;        /* the caller's mode: user, for an image */
    return *acmode > PSL$C_USER ? PSL$C_USER : *acmode;
}

/* ---- the services ---------------------------------------------------- */

uint32_t sys$crelnm(const uint32_t *attr,
                    const struct dsc$descriptor_s *tabnam,
                    const struct dsc$descriptor_s *lognam,
                    const uint8_t *acmode,
                    const struct item_list_3 *itmlst)
{
    char table[LNM$C_TABNAMLEN + 1], name[LNM$C_NAMLENGTH + 1];
    char vbuf[LNM_MAXVAL][LNM$C_NAMLENGTH + 1];
    const char *vals[LNM_MAXVAL];
    uint16_t vlens[LNM_MAXVAL];
    uint32_t eattr = 0, cattr = 0;
    int tabs[4], nt, nl;
    uint8_t nv = 0;

    if (dsc_text(tabnam, table, sizeof table) < 0)
        return SS$_BADPARAM;
    nl = dsc_text(lognam, name, sizeof name);
    if (nl < 0)
        return SS$_BADPARAM;
    if (nl == 0 || nl > LNM$C_NAMLENGTH)
        return SS$_IVLOGNAM;

    /* Equivalence strings, each with the LNM$_ATTRIBUTES that precede it.
     * The arena keeps one attribute word per name: the first string's
     * (CONCEALED, TERMINAL) plus the name's own (attr argument). */
    for (const struct item_list_3 *it = itmlst;
         it && (it->buflen != 0 || it->item_code != 0); it++) {
        if (it->item_code == LNM$_ATTRIBUTES && it->bufaddr &&
            it->buflen >= sizeof(uint32_t)) {
            cattr = *(const uint32_t *)it->bufaddr &
                    (LNM$M_CONCEALED | LNM$M_TERMINAL);
        } else if (it->item_code == LNM$_STRING) {
            if (it->buflen == 0 || it->buflen > LNM$C_NAMLENGTH || !it->bufaddr)
                return SS$_IVLOGNAM;
            if (nv >= LNM_MAXVAL)
                return SS$_EXQUOTA;
            memcpy(vbuf[nv], it->bufaddr, it->buflen);
            vbuf[nv][it->buflen] = '\0';
            vals[nv] = vbuf[nv];
            vlens[nv] = it->buflen;     /* the bytes as given, NUL and all */
            if (nv == 0)
                eattr = cattr;
            nv++;
        }
    }
    eattr |= attr ? (*attr & (LNM$M_NO_ALIAS | LNM$M_CONFINE)) : 0;

    nt = resolve_tables(table, tabs);
    if (nt == 0)
        return SS$_NOLOGTAB;

    if (attr && (*attr & LNM$M_CREATE_IF)) {
        struct lnm_hit h;
        uint8_t m = req_mode(acmode);
        if (tab_lookup(tabs[0], name, 0, m, &h) == 1 && h.acmode == m)
            return SS$_NORMAL;
    }
    /* A search list (LNM$FILE_DEV): the name goes into its first table. */
    return tab_define(tabs[0], name, vals, vlens, nv, eattr, req_mode(acmode));
}

uint32_t sys$dellnm(const struct dsc$descriptor_s *tabnam,
                    const struct dsc$descriptor_s *lognam,
                    const uint8_t *acmode)
{
    char table[LNM$C_TABNAMLEN + 1], name[LNM$C_NAMLENGTH + 1];
    int tabs[4], nl;

    if (dsc_text(tabnam, table, sizeof table) < 0)
        return SS$_BADPARAM;
    nl = dsc_text(lognam, name, sizeof name);
    if (nl == 0 || nl > LNM$C_NAMLENGTH)
        return SS$_IVLOGNAM;
    if (resolve_tables(table, tabs) == 0)
        return SS$_NOLOGNAM;
    /* No logical name: every name in the table at the mode and outer ones. */
    return tab_delete(tabs[0], nl < 0 ? NULL : name, req_mode(acmode));
}

uint32_t sys$trnlnm(const uint32_t *attr,
                    const struct dsc$descriptor_s *tabnam,
                    const struct dsc$descriptor_s *lognam,
                    const uint8_t *acmode,
                    const struct item_list_3 *itmlst)
{
    char table[LNM$C_TABNAMLEN + 1], name[LNM$C_NAMLENGTH + 1];
    int tabs[4], nt, nl, i;
    uint32_t req_index = 0;
    struct lnm_hit h;
    int found = 0;

    nl = dsc_text(lognam, name, sizeof name);
    if (nl < 0)
        return SS$_BADPARAM;
    if (nl == 0 || nl > LNM$C_NAMLENGTH)
        return SS$_IVLOGNAM;

    for (const struct item_list_3 *it = itmlst;
         it && (it->buflen != 0 || it->item_code != 0); it++) {
        if (it->item_code == LNM$_INDEX && it->bufaddr &&
            it->buflen >= sizeof(uint32_t))
            req_index = *(const uint32_t *)it->bufaddr;
    }
    /* LNM$_INDEX is 0..127 (observed: 127 is a valid, empty index; 128 is
     * SS$_BADPARAM). */
    if (req_index > 127)
        return SS$_BADPARAM;

    if (dsc_text(tabnam, table, sizeof table) < 0)
        strcpy(table, "LNM$FILE_DEV");
    nt = resolve_tables(table, tabs);
    if (nt == 0)
        return SS$_NOLOGNAM;

    for (i = 0; i < nt && !found; i++) {
        uint32_t r = tab_lookup(tabs[i], name,
                                attr && (*attr & LNM$M_CASE_BLIND),
                                req_mode(acmode), &h);
        if (r == 1)
            found = 1;
        else if (r != 0)
            return r;                              /* SS$_NOSUCHDEV */
    }
    if (!found)
        return SS$_NOLOGNAM;

    /* Fill the item list. A string longer than its buffer is truncated and the
     * service returns SS$_BUFFEROVF; an index past the last equivalence string
     * reports an empty string and no attributes (observed, rd vms-3c4). */
    {
        const char *val = req_index < h.nvals ? h.vals[req_index] : "";
        uint16_t vlen = req_index < h.nvals ? h.vlen[req_index] : 0;
        uint32_t ra = req_index < h.nvals
                          ? (h.attributes | LNM$M_EXISTS | tab_attr(h.tab))
                          : (h.nvals == 0 ? h.attributes : 0);
        char tname[40];
        int truncated = 0;

        tab_name(h.tab, tname, sizeof tname);
        for (const struct item_list_3 *it = itmlst;
             it && (it->buflen != 0 || it->item_code != 0); it++) {
            switch (it->item_code) {
            case LNM$_STRING:
                if (it->bufaddr) {
                    uint16_t len = vlen;
                    if (len > it->buflen) { len = it->buflen; truncated = 1; }
                    memcpy(it->bufaddr, val, len);
                    if (it->retlen) *it->retlen = len;
                }
                break;
            case LNM$_LENGTH:
                if (it->bufaddr && it->buflen >= sizeof(uint32_t))
                    *(uint32_t *)it->bufaddr = (uint32_t)vlen;
                if (it->retlen) *it->retlen = sizeof(uint32_t);
                break;
            case LNM$_ATTRIBUTES:
                if (it->bufaddr && it->buflen >= sizeof(uint32_t))
                    *(uint32_t *)it->bufaddr = ra;
                if (it->retlen) *it->retlen = sizeof(uint32_t);
                break;
            case LNM$_MAX_INDEX:
                if (it->bufaddr && it->buflen >= sizeof(uint32_t))
                    *(int32_t *)it->bufaddr = (int32_t)h.nvals - 1;
                if (it->retlen) *it->retlen = sizeof(uint32_t);
                break;
            case LNM$_ACMODE:
                if (it->bufaddr && it->buflen >= 1)
                    *(uint8_t *)it->bufaddr = h.acmode;
                if (it->retlen) *it->retlen = 1;
                break;
            case LNM$_TABLE:
                if (it->bufaddr) {
                    uint16_t len = (uint16_t)strlen(tname);
                    if (len > it->buflen) { len = it->buflen; truncated = 1; }
                    memcpy(it->bufaddr, tname, len);
                    if (it->retlen) *it->retlen = len;
                }
                break;
            default:
                break;
            }
        }
        return truncated ? SS$_BUFFEROVF : SS$_NORMAL;
    }
}
