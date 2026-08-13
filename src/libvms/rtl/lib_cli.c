/*
 * lib_cli.c - CLI$ callable interface + compiled-CLD support.
 *
 * Implements the CLI$ command-parsing routines used by DCL-derived utilities
 * (MMK, LIBRARIAN, ...) that were compiled against a command definition
 * (.CLD) table rather than argc/argv:
 *
 *     cli$dcl_parse   - parse a command string against a compiled command
 *                       table (the caller's "MMK_CLD"), establishing it as
 *                       the current command.
 *     cli$present     - test whether a qualifier / parameter / keyword
 *                       appeared, and how (present / absent / negated /
 *                       defaulted).
 *     cli$get_value   - retrieve successive values of a qualifier or
 *                       parameter.
 *
 * plus the OVMX compiled-CLD support:
 *
 *     cli$compile_cld - parse CLD source text into a command table.
 *     cli$free_cld    - release a compiled table.
 *
 * ------------------------------------------------------------------------
 * CLEAN-ROOM / PROVENANCE (project Rule 8)
 * ------------------------------------------------------------------------
 * All CLI$ semantics below are derived from the PUBLIC VSI OpenVMS DCL
 * Dictionary (CLI$PRESENT / CLI$GET_VALUE / CLI$DCL_PARSE) and the public
 * OpenVMS Command Definition Utility (CDU) manual, plus the observed call
 * pattern of the vendored MadGoat MMK sources we have the right to use
 * (tests/corpus/tier3-mmk). No VSI/HPE/DEC source or headers were copied or
 * paraphrased. The in-memory command-table byte layout is an OVMX design
 * choice (see clitable.h).
 *
 * The value-return contract is grounded directly in the DCL Dictionary and
 * confirmed against MMK's loops: repeated cli$get_value calls return
 * CLI$_COMMA (success) while a further list value follows and SS$_NORMAL on
 * the final value; a further call once the list is exhausted returns
 * CLI$_ABSENT (warning severity, so $VMS_STATUS_SUCCESS is false).
 * ------------------------------------------------------------------------
 *
 * THREADING: VMS keeps a single "current command" per process; cli$present /
 * cli$get_value operate on whatever cli$dcl_parse last established. This
 * module models that with one process-wide context, matching VMS behavior.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "descrip.h"
#include "ssdef.h"
#include "stsdef.h"
#include "libclidef.h"
#include "clitable.h"

/* ================================================================
 * Small helpers
 * ================================================================ */

static int ci_eq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return *a == *b;
}

/*
 * Abbreviation match: is ABBR (length alen) a case-insensitive prefix of
 * FULL? A zero-length abbreviation never matches.
 */
static int abbrev_of(const char *full, const char *abbr, size_t alen)
{
    if (alen == 0) return 0;
    if (strlen(full) < alen) return 0;
    return ci_eq_n(full, abbr, alen);
}

/* ================================================================
 * Compile-time arena (heap allocations owned by a compiled table)
 * ================================================================ */

struct cld_arena {
    void   **p;
    size_t   n;
    size_t   cap;
    int      oom;
};

static void *ar_alloc(struct cld_arena *a, size_t sz)
{
    void *m = calloc(1, sz ? sz : 1);
    if (!m) { a->oom = 1; return NULL; }
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 32;
        void **np = realloc(a->p, nc * sizeof(void *));
        if (!np) { free(m); a->oom = 1; return NULL; }
        a->p = np; a->cap = nc;
    }
    a->p[a->n++] = m;
    return m;
}

static char *ar_strup(struct cld_arena *a, const char *s, size_t n)
{
    char *d = ar_alloc(a, n + 1);
    if (d) { for (size_t i = 0; i < n; i++) d[i] = (char)toupper((unsigned char)s[i]); d[n] = '\0'; }
    return d;
}

static char *ar_str(struct cld_arena *a, const char *s, size_t n)
{
    char *d = ar_alloc(a, n + 1);
    if (d) { memcpy(d, s, n); d[n] = '\0'; }
    return d;
}

/* ================================================================
 * Parsed current-command context
 * ================================================================ */

struct cli_parsed {
    const char            *name;        /* canonical UPPERCASE name / label */
    int                    is_param;
    const struct cli_qual *qdef;        /* qualifier def, or NULL for a param */
    const struct cli_param *pdef;       /* param def, or NULL for a qualifier */
    int                    specified;   /* appeared on the command line */
    int                    negated;     /* /NOxxx */
    int                    is_default_on;/* DEFAULT attribute, not overridden */
    int                    defaulted;   /* value(s) from CLD VALUE(DEFAULT=) */
    char                 **values;      /* value list (heap, per-entity) */
    int                    nvalues;
    int                    getidx;      /* cursor for cli$get_value */
};

struct cli_context {
    int                             valid;
    const struct cli_command_table *table;
    const struct cli_verb          *verb;
    struct cli_parsed              *ents; /* nquals + nparams entries */
    int                             nents;
};

static struct cli_context g_ctx; /* the process-wide current command */

static void ctx_reset(void)
{
    if (g_ctx.ents) {
        for (int i = 0; i < g_ctx.nents; i++) {
            struct cli_parsed *e = &g_ctx.ents[i];
            if (e->values) {
                for (int v = 0; v < e->nvalues; v++) free(e->values[v]);
                free(e->values);
            }
        }
        free(g_ctx.ents);
    }
    memset(&g_ctx, 0, sizeof(g_ctx));
}

static int ent_add_value(struct cli_parsed *e, const char *s, size_t n)
{
    char **nv = realloc(e->values, (size_t)(e->nvalues + 1) * sizeof(char *));
    if (!nv) return 0;
    e->values = nv;
    char *dup = malloc(n + 1);
    if (!dup) return 0;
    memcpy(dup, s, n); dup[n] = '\0';
    e->values[e->nvalues++] = dup;
    return 1;
}

/* ================================================================
 * cli$compile_cld -- CLD source text -> command table
 * ================================================================ */

/* ---- CLD tokenizer ---- */
struct cld_toks { char **v; int n; int cap; };

static int toks_push(struct cld_toks *t, const char *s, size_t n)
{
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 64;
        char **nv = realloc(t->v, (size_t)nc * sizeof(char *));
        if (!nv) return 0;
        t->v = nv; t->cap = nc;
    }
    char *d = malloc(n + 1);
    if (!d) return 0;
    memcpy(d, s, n); d[n] = '\0';
    t->v[t->n++] = d;
    return 1;
}

static void toks_free(struct cld_toks *t)
{
    for (int i = 0; i < t->n; i++) free(t->v[i]);
    free(t->v);
}

static int is_punct(char c) { return c == ',' || c == '(' || c == ')' || c == '='; }

static int cld_tokenize(const char *src, size_t len, struct cld_toks *out)
{
    size_t i = 0;
    while (i < len) {
        char c = src[i];
        if (c == '!') {                       /* comment to end of line */
            while (i < len && src[i] != '\n') i++;
        } else if (isspace((unsigned char)c)) {
            i++;
        } else if (is_punct(c)) {
            if (!toks_push(out, &src[i], 1)) return 0;
            i++;
        } else if (c == '"') {                 /* quoted string -> bare token */
            size_t j = i + 1;
            while (j < len && src[j] != '"') j++;
            if (!toks_push(out, &src[i + 1], j - (i + 1))) return 0;
            i = (j < len) ? j + 1 : j;
        } else {                               /* word */
            size_t j = i;
            while (j < len && !isspace((unsigned char)src[j]) &&
                   !is_punct(src[j]) && src[j] != '!' && src[j] != '"')
                j++;
            if (!toks_push(out, &src[i], j - i)) return 0;
            i = j;
        }
    }
    return 1;
}

/* Statement-starting keywords -- an attribute scan stops at any of these. */
static int is_stmt_kw(const char *t)
{
    return ci_eq(t, "MODULE") || ci_eq(t, "IDENT") || ci_eq(t, "DEFINE") ||
           ci_eq(t, "PARAMETER") || ci_eq(t, "QUALIFIER") || ci_eq(t, "KEYWORD") ||
           ci_eq(t, "DISALLOW") || ci_eq(t, "IMAGE") || ci_eq(t, "ROUTINE") ||
           ci_eq(t, "SYNONYM") || ci_eq(t, "PARAMETERS");
}

/* Growable temp vectors used during a compile. */
struct qvec { struct cli_qual *v; int n, cap; char **type; };
struct pvec { struct cli_param *v; int n, cap; };
struct kvec { struct cli_keyword *v; int n, cap; };
struct tvec { char **name; struct cli_keyword **kw; int n, cap; }; /* DEFINE TYPEs */

static int qvec_push(struct qvec *q, struct cli_qual qd, char *typ)
{
    if (q->n == q->cap) {
        int nc = q->cap ? q->cap * 2 : 16;
        struct cli_qual *nv = realloc(q->v, (size_t)nc * sizeof(*nv));
        char **nt = realloc(q->type, (size_t)nc * sizeof(char *));
        if (!nv || !nt) { free(nv == q->v ? NULL : nv); return 0; }
        q->v = nv; q->type = nt; q->cap = nc;
    }
    q->v[q->n] = qd; q->type[q->n] = typ; q->n++;
    return 1;
}

static int pvec_push(struct pvec *p, struct cli_param pd)
{
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 8;
        struct cli_param *nv = realloc(p->v, (size_t)nc * sizeof(*nv));
        if (!nv) return 0;
        p->v = nv; p->cap = nc;
    }
    p->v[p->n++] = pd;
    return 1;
}

static int kvec_push(struct kvec *k, struct cli_keyword kd)
{
    if (k->n == k->cap) {
        int nc = k->cap ? k->cap * 2 : 8;
        struct cli_keyword *nv = realloc(k->v, (size_t)nc * sizeof(*nv));
        if (!nv) return 0;
        k->v = nv; k->cap = nc;
    }
    k->v[k->n++] = kd;
    return 1;
}

/*
 * Parse the attributes that can follow a QUALIFIER name. *pi points just
 * after the qualifier name; advances it past the attributes. Fills flags,
 * *deflt, and (for TYPE=xxx) *typename.
 */
static void parse_qual_attrs(struct cld_toks *t, int *pi, struct cld_arena *ar,
                             uint32_t *flags, const char **deflt, char **typename)
{
    int i = *pi;
    while (i < t->n) {
        const char *tk = t->v[i];
        if (ci_eq(tk, ",")) { i++; continue; }
        if (is_stmt_kw(tk)) break;
        if (ci_eq(tk, "NEGATABLE")) { *flags |= CLI_A_NEGATABLE; i++; continue; }
        if (ci_eq(tk, "NONNEGATABLE")) { *flags &= ~CLI_A_NEGATABLE; i++; continue; }
        if (ci_eq(tk, "DEFAULT")) {
            /* Standalone DEFAULT = qualifier present-by-default. A "DEFAULT="
             * form inside VALUE(...) is handled in the VALUE branch. */
            if (i + 1 < t->n && ci_eq(t->v[i + 1], "=")) {
                /* qualifier-level default value (rare) */
                if (i + 2 < t->n) { *deflt = ar_str(ar, t->v[i + 2], strlen(t->v[i + 2])); i += 3; }
                else i++;
            } else {
                *flags |= CLI_A_DEFAULT; i++;
            }
            continue;
        }
        if (ci_eq(tk, "VALUE")) {
            i++;
            *flags |= CLI_A_VALUE;
            if (i < t->n && ci_eq(t->v[i], "(")) {
                i++;
                while (i < t->n && !ci_eq(t->v[i], ")")) {
                    const char *a = t->v[i];
                    if (ci_eq(a, ",")) { i++; continue; }
                    if (ci_eq(a, "LIST")) { *flags |= CLI_A_LIST; i++; continue; }
                    if (ci_eq(a, "REQUIRED")) { *flags |= CLI_A_VALREQ; i++; continue; }
                    if (ci_eq(a, "IMPCAT") || ci_eq(a, "CONCATENATED") ||
                        ci_eq(a, "NOCONCATENATED")) { i++; continue; }
                    if (ci_eq(a, "TYPE")) {
                        i++;
                        if (i < t->n && ci_eq(t->v[i], "=")) i++;
                        if (i < t->n) {
                            const char *ty = t->v[i];
                            if (ci_eq(ty, "$FILE") || ci_eq(ty, "$INFILE") ||
                                ci_eq(ty, "$OUTFILE")) {
                                *flags |= CLI_A_FILE;
                            } else {
                                *flags |= CLI_A_KEYWORD;
                                *typename = ar_strup(ar, ty, strlen(ty));
                            }
                            i++;
                        }
                        continue;
                    }
                    if (ci_eq(a, "DEFAULT")) {
                        i++;
                        if (i < t->n && ci_eq(t->v[i], "=")) i++;
                        if (i < t->n) { *deflt = ar_str(ar, t->v[i], strlen(t->v[i])); i++; }
                        continue;
                    }
                    i++; /* unknown VALUE sub-attribute -- skip */
                }
                if (i < t->n && ci_eq(t->v[i], ")")) i++;
            }
            continue;
        }
        /* unknown attribute -- skip a single token */
        i++;
    }
    *pi = i;
}

uint32_t cli$compile_cld(const struct dsc$descriptor_s *cld_source,
                         struct cli_command_table **table_out)
{
    if (!cld_source || !cld_source->dsc$a_pointer || !table_out)
        return SS$_BADPARAM;
    *table_out = NULL;

    struct cld_toks toks; memset(&toks, 0, sizeof(toks));
    if (!cld_tokenize(cld_source->dsc$a_pointer, cld_source->dsc$w_length, &toks)) {
        toks_free(&toks);
        return SS$_INSFMEM;
    }

    struct cld_arena *ar = calloc(1, sizeof(*ar));
    if (!ar) { toks_free(&toks); return SS$_INSFMEM; }

    struct qvec quals; memset(&quals, 0, sizeof(quals));
    struct pvec params; memset(&params, 0, sizeof(params));
    struct tvec types; memset(&types, 0, sizeof(types));
    char *verb_name = NULL;
    uint32_t rc = SS$_NORMAL;

    int i = 0;
    while (i < toks.n) {
        const char *tk = toks.v[i];

        if (ci_eq(tk, "MODULE") || ci_eq(tk, "IDENT")) {
            i++; if (i < toks.n) i++;            /* skip its operand */
            continue;
        }
        if (ci_eq(tk, "DEFINE")) {
            i++;
            if (i >= toks.n) break;
            if (ci_eq(toks.v[i], "VERB")) {
                i++;
                if (i < toks.n) { verb_name = ar_strup(ar, toks.v[i], strlen(toks.v[i])); i++; }
                continue;
            }
            if (ci_eq(toks.v[i], "TYPE")) {
                i++;
                char *tyname = NULL;
                if (i < toks.n) { tyname = ar_strup(ar, toks.v[i], strlen(toks.v[i])); i++; }
                struct kvec kws; memset(&kws, 0, sizeof(kws));
                while (i < toks.n && ci_eq(toks.v[i], "KEYWORD")) {
                    i++;
                    struct cli_keyword kd; memset(&kd, 0, sizeof(kd));
                    if (i < toks.n) { kd.name = ar_strup(ar, toks.v[i], strlen(toks.v[i])); i++; }
                    /* keyword attributes */
                    while (i < toks.n) {
                        const char *a = toks.v[i];
                        if (ci_eq(a, ",")) { i++; continue; }
                        if (ci_eq(a, "DEFAULT")) { kd.flags |= CLI_A_DEFAULT; i++; continue; }
                        if (ci_eq(a, "NEGATABLE")) { kd.flags |= CLI_A_NEGATABLE; i++; continue; }
                        if (ci_eq(a, "NONNEGATABLE")) { i++; continue; }
                        break;
                    }
                    if (!kvec_push(&kws, kd)) { rc = SS$_INSFMEM; goto done_kws; }
                }
              done_kws:
                if (rc == SS$_NORMAL) {
                    /* materialise a NULL-terminated keyword array in the arena */
                    struct cli_keyword *arr = ar_alloc(ar, (size_t)(kws.n + 1) * sizeof(*arr));
                    if (arr) {
                        for (int k = 0; k < kws.n; k++) arr[k] = kws.v[k];
                        /* register the type */
                        int tn = types.n;
                        if (tn == types.cap) {
                            int nc = types.cap ? types.cap * 2 : 8;
                            char **nn = realloc(types.name, (size_t)nc * sizeof(char *));
                            struct cli_keyword **nk = realloc(types.kw, (size_t)nc * sizeof(void *));
                            if (nn) types.name = nn;
                            if (nk) types.kw = nk;
                            if (!nn || !nk) { free(kws.v); rc = SS$_INSFMEM; goto finish; }
                            types.cap = nc;
                        }
                        types.name[tn] = tyname;
                        types.kw[tn] = arr;
                        types.n++;
                    } else {
                        rc = SS$_INSFMEM;
                    }
                }
                free(kws.v);
                if (rc != SS$_NORMAL) goto finish;
                continue;
            }
            /* DEFINE of something we don't model -- skip its name */
            if (i < toks.n) i++;
            continue;
        }
        if (ci_eq(tk, "PARAMETER")) {
            i++;
            struct cli_param pd; memset(&pd, 0, sizeof(pd));
            const char *pn = (i < toks.n) ? toks.v[i] : "P1";
            if (i < toks.n) i++;
            char *label = ar_strup(ar, pn, strlen(pn)); /* default label = Pn */
            uint32_t pflags = 0; const char *pdef = NULL; char *ptype = NULL;
            /* parameter attributes: LABEL=xxx and VALUE(...) */
            while (i < toks.n) {
                const char *a = toks.v[i];
                if (ci_eq(a, ",")) { i++; continue; }
                if (is_stmt_kw(a)) break;
                if (ci_eq(a, "LABEL")) {
                    i++;
                    if (i < toks.n && ci_eq(toks.v[i], "=")) i++;
                    if (i < toks.n) { label = ar_strup(ar, toks.v[i], strlen(toks.v[i])); i++; }
                    continue;
                }
                if (ci_eq(a, "VALUE")) {
                    int save = i;
                    parse_qual_attrs(&toks, &save, ar, &pflags, &pdef, &ptype);
                    i = save; continue;
                }
                if (ci_eq(a, "PROMPT")) {
                    i++;
                    if (i < toks.n && ci_eq(toks.v[i], "=")) i++;
                    if (i < toks.n) i++;
                    continue;
                }
                i++;
            }
            pd.label = label;
            pd.flags = pflags;
            if (!pvec_push(&params, pd)) { rc = SS$_INSFMEM; goto finish; }
            continue;
        }
        if (ci_eq(tk, "QUALIFIER")) {
            i++;
            struct cli_qual qd; memset(&qd, 0, sizeof(qd));
            if (i < toks.n) { qd.name = ar_strup(ar, toks.v[i], strlen(toks.v[i])); i++; }
            uint32_t flags = 0; const char *deflt = NULL; char *typ = NULL;
            parse_qual_attrs(&toks, &i, ar, &flags, &deflt, &typ);
            qd.flags = flags; qd.deflt = deflt;
            if (!qvec_push(&quals, qd, typ)) { rc = SS$_INSFMEM; goto finish; }
            continue;
        }
        /* anything else -- skip one token */
        i++;
    }

    if (ar->oom) { rc = SS$_INSFMEM; goto finish; }
    if (!verb_name) { rc = SS$_ABORT; goto finish; } /* no DEFINE VERB found */

    /* Resolve qualifier TYPE=xxx references to their keyword sets. */
    for (int q = 0; q < quals.n; q++) {
        if (quals.type[q]) {
            for (int ty = 0; ty < types.n; ty++) {
                if (types.name[ty] && ci_eq(types.name[ty], quals.type[q])) {
                    quals.v[q].keywords = types.kw[ty];
                    break;
                }
            }
        }
    }

    /* Materialise the arena arrays and the table. */
    {
        struct cli_command_table *tab = ar_alloc(ar, sizeof(*tab));
        struct cli_verb *verb = ar_alloc(ar, sizeof(*verb));
        struct cli_qual *qarr = ar_alloc(ar, (size_t)(quals.n ? quals.n : 1) * sizeof(*qarr));
        struct cli_param *parr = ar_alloc(ar, (size_t)(params.n ? params.n : 1) * sizeof(*parr));
        if (!tab || !verb || !qarr || !parr || ar->oom) { rc = SS$_INSFMEM; goto finish; }
        for (int q = 0; q < quals.n; q++) qarr[q] = quals.v[q];
        for (int p = 0; p < params.n; p++) parr[p] = params.v[p];
        verb->name = verb_name;
        verb->params = parr; verb->nparams = params.n;
        verb->quals = qarr;  verb->nquals = quals.n;
        tab->magic = CLI_TABLE_MAGIC;
        tab->verbs = verb; tab->nverbs = 1;
        tab->ovmx_arena = ar;
        *table_out = tab;
    }

finish:
    free(quals.v); free(quals.type);
    free(params.v);
    free(types.name); free(types.kw);
    toks_free(&toks);
    if (rc != SS$_NORMAL) {
        /* free the arena on failure */
        for (size_t k = 0; k < ar->n; k++) free(ar->p[k]);
        free(ar->p); free(ar);
    }
    return rc;
}

void cli$free_cld(struct cli_command_table *table)
{
    if (!table || !table->ovmx_arena) return;
    struct cld_arena *ar = table->ovmx_arena;
    for (size_t k = 0; k < ar->n; k++) free(ar->p[k]); /* includes `table` */
    free(ar->p);
    free(ar);
}

/* ================================================================
 * cli$dcl_parse
 * ================================================================ */

static int find_qual(const struct cli_verb *v, const char *name, size_t nlen)
{
    /* exact case-insensitive match first */
    for (int i = 0; i < v->nquals; i++)
        if (strlen(v->quals[i].name) == nlen && ci_eq_n(v->quals[i].name, name, nlen))
            return i;
    /* unique abbreviation */
    int hit = -1, count = 0;
    for (int i = 0; i < v->nquals; i++) {
        if (abbrev_of(v->quals[i].name, name, nlen)) { hit = i; count++; }
    }
    if (count == 1) return hit;
    if (count > 1) return -2; /* ambiguous */
    return -1;
}

/* Parse one value token (bare or quoted already stripped) into an entity. */
static int add_values_from_spec(struct cli_parsed *e, const char *spec, size_t len)
{
    /* spec is the text after '=' : either "(a,b,c)" or a bare value. */
    if (len == 0) return 1;
    if (spec[0] == '(') {
        size_t i = 1;
        while (i < len && spec[i] != ')') {
            size_t start = i;
            int depth = 0;
            while (i < len) {
                char c = spec[i];
                if (c == '(') depth++;
                else if (c == ')') { if (depth == 0) break; depth--; }
                else if (c == ',' && depth == 0) break;
                i++;
            }
            if (!ent_add_value(e, &spec[start], i - start)) return 0;
            if (i < len && spec[i] == ',') i++;
        }
    } else {
        if (!ent_add_value(e, spec, len)) return 0;
    }
    return 1;
}

uint32_t cli$dcl_parse(const struct dsc$descriptor_s *command,
                       const struct cli_command_table *table,
                       cli_prompt_rtn param_rtn,
                       cli_prompt_rtn prompt_rtn)
{
    (void)param_rtn; (void)prompt_rtn; /* accepted for signature compatibility;
                                        * interactive continuation for an omitted
                                        * REQUIRED parameter is not reached on the
                                        * non-interactive path (see item report). */

    if (!command || !table) return SS$_BADPARAM;
    if (table->magic != CLI_TABLE_MAGIC || table->nverbs < 1)
        return SS$_BADPARAM;

    ctx_reset();

    /* Copy the command string into a private NUL-terminated buffer. */
    size_t clen = command->dsc$a_pointer ? command->dsc$w_length : 0;
    char *cmd = malloc(clen + 1);
    if (!cmd) return SS$_INSFMEM;
    if (clen) memcpy(cmd, command->dsc$a_pointer, clen);
    cmd[clen] = '\0';

    const char *p = cmd;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Verb token. */
    const char *vstart = p;
    while (*p && !isspace((unsigned char)*p) && *p != '/') p++;
    size_t vlen = (size_t)(p - vstart);

    const struct cli_verb *verb = &table->verbs[0];
    if (vlen) {
        for (int i = 0; i < table->nverbs; i++) {
            if (strlen(table->verbs[i].name) == vlen &&
                ci_eq_n(table->verbs[i].name, vstart, vlen)) {
                verb = &table->verbs[i];
                break;
            }
        }
    }

    /* Build the entity table: one slot per qualifier then one per parameter. */
    int nents = verb->nquals + verb->nparams;
    struct cli_parsed *ents = calloc((size_t)(nents ? nents : 1), sizeof(*ents));
    if (!ents) { free(cmd); return SS$_INSFMEM; }
    for (int i = 0; i < verb->nquals; i++) {
        ents[i].name = verb->quals[i].name;
        ents[i].qdef = &verb->quals[i];
    }
    for (int i = 0; i < verb->nparams; i++) {
        ents[verb->nquals + i].name = verb->params[i].label;
        ents[verb->nquals + i].is_param = 1;
        ents[verb->nquals + i].pdef = &verb->params[i];
    }

    g_ctx.table = table;
    g_ctx.verb = verb;
    g_ctx.ents = ents;
    g_ctx.nents = nents;
    g_ctx.valid = 1;

    uint32_t rc = SS$_NORMAL;
    int param_cursor = 0;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        if (*p == '/') {
            p++;
            const char *qs = p;
            while (*p && *p != '=' && *p != '/' && !isspace((unsigned char)*p)) p++;
            size_t qlen = (size_t)(p - qs);
            if (qlen == 0) continue;

            /* Optional value. */
            const char *vs = NULL; size_t vl = 0;
            if (*p == '=') {
                p++;
                vs = p;
                if (*p == '(') {
                    int depth = 0;
                    while (*p) {
                        if (*p == '(') depth++;
                        else if (*p == ')') { depth--; if (depth == 0) { p++; break; } }
                        p++;
                    }
                } else {
                    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p == '"') p++; }
                    else while (*p && *p != '/' && !isspace((unsigned char)*p)) p++;
                }
                vl = (size_t)(p - vs);
            }

            int qi = find_qual(verb, qs, qlen);
            int negated = 0;
            if (qi == -1 && qlen > 2 && ci_eq_n(qs, "NO", 2)) {
                int q2 = find_qual(verb, qs + 2, qlen - 2);
                if (q2 >= 0 && (verb->quals[q2].flags & CLI_A_NEGATABLE)) {
                    qi = q2; negated = 1;
                }
            }
            if (qi == -2) { rc = SS$_IVQUAL; goto parsed; } /* ambiguous */
            if (qi < 0)   { rc = SS$_IVQUAL; goto parsed; } /* unknown qualifier */

            struct cli_parsed *e = &ents[qi];
            e->specified = 1;
            e->negated = negated;
            if (vl) {
                /* strip one surrounding quote pair for a bare quoted value */
                if (vl >= 2 && vs[0] == '"' && vs[vl - 1] == '"') { vs++; vl -= 2; }
                if (!add_values_from_spec(e, vs, vl)) { rc = SS$_INSFMEM; goto parsed; }
            }
            /* REQUIRED value but none supplied (and not negated). */
            if (!negated && (e->qdef->flags & CLI_A_VALREQ) && e->nvalues == 0) {
                rc = SS$_ABORT; /* %CLI-W-VALREQ analogue */
                goto parsed;
            }
        } else {
            /* Positional parameter. */
            const char *ps = p;
            if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p == '"') p++; }
            else while (*p && *p != '/' && !isspace((unsigned char)*p)) p++;
            size_t pl = (size_t)(p - ps);
            if (pl >= 2 && ps[0] == '"' && ps[pl - 1] == '"') { ps++; pl -= 2; }

            if (param_cursor >= verb->nparams) { rc = SS$_BADPARAM; goto parsed; }
            struct cli_parsed *e = &ents[verb->nquals + param_cursor];
            e->specified = 1;
            if (!ent_add_value(e, ps, pl)) { rc = SS$_INSFMEM; goto parsed; }
            /* A LIST parameter consumes all remaining positional tokens. */
            if (!(e->pdef->flags & CLI_A_LIST)) param_cursor++;
        }
    }

parsed:
    /* Apply CLD defaults to qualifiers not given on the command line. */
    if (rc == SS$_NORMAL) {
        for (int i = 0; i < verb->nquals; i++) {
            struct cli_parsed *e = &ents[i];
            if (e->specified || e->negated) continue;
            if (e->qdef->flags & CLI_A_DEFAULT) e->is_default_on = 1;
            if (e->qdef->deflt) {
                e->defaulted = 1;
                add_values_from_spec(e, e->qdef->deflt, strlen(e->qdef->deflt));
            }
        }
    } else {
        g_ctx.valid = 0; /* a parse error leaves no usable current command */
    }

    free(cmd);
    return rc;
}

/* ================================================================
 * cli$present
 * ================================================================ */

static struct cli_parsed *ent_by_name(const char *name, size_t nlen)
{
    if (!g_ctx.valid) return NULL;
    /* exact */
    for (int i = 0; i < g_ctx.nents; i++)
        if (strlen(g_ctx.ents[i].name) == nlen && ci_eq_n(g_ctx.ents[i].name, name, nlen))
            return &g_ctx.ents[i];
    /* abbreviation (qualifiers/params both) */
    struct cli_parsed *hit = NULL; int count = 0;
    for (int i = 0; i < g_ctx.nents; i++)
        if (abbrev_of(g_ctx.ents[i].name, name, nlen)) { hit = &g_ctx.ents[i]; count++; }
    return (count == 1) ? hit : NULL;
}

/* classify a plain (non-dotted) entity */
static uint32_t classify(const struct cli_parsed *e)
{
    if (!e) return CLI$_ABSENT;
    if (e->negated) return CLI$_NEGATED;
    if (e->specified) return CLI$_PRESENT;
    if (e->is_default_on) return CLI$_PRESENT;
    if (e->defaulted) return CLI$_DEFAULTED;
    return CLI$_ABSENT;
}

/* Does keyword KW (possibly abbreviated) appear among an entity's values? */
static int keyword_in_values(const struct cli_parsed *e, const char *kw, size_t klen,
                             int *negated_out)
{
    *negated_out = 0;
    for (int i = 0; i < e->nvalues; i++) {
        const char *val = e->values[i];
        const char *vv = val; size_t vlen = strlen(val);
        int neg = 0;
        if (vlen > 2 && ci_eq_n(vv, "NO", 2)) { /* NOKEYWORD form */
            /* only treat as negation if the remainder matches the keyword */
            if (abbrev_of(kw, vv + 2, vlen - 2) || abbrev_of(vv + 2, kw, klen)) {
                neg = 1; vv += 2; vlen -= 2;
            }
        }
        if (abbrev_of(vv, kw, klen) || (klen == vlen && ci_eq_n(vv, kw, klen)) ||
            abbrev_of(kw, vv, vlen)) {
            *negated_out = neg;
            return 1;
        }
    }
    return 0;
}

uint32_t cli$present(const struct dsc$descriptor_s *label)
{
    if (!label || !label->dsc$a_pointer || !g_ctx.valid) return CLI$_ABSENT;
    const char *name = label->dsc$a_pointer;
    size_t len = label->dsc$w_length;

    /* Dotted "QUAL.KEYWORD" form. */
    const char *dot = memchr(name, '.', len);
    if (dot) {
        size_t qn = (size_t)(dot - name);
        const char *kw = dot + 1;
        size_t kl = len - qn - 1;
        struct cli_parsed *e = ent_by_name(name, qn);
        uint32_t base = classify(e);
        if (base != CLI$_PRESENT) return base; /* absent/negated/defaulted qual */

        int neg = 0;
        if (keyword_in_values(e, kw, kl, &neg))
            return neg ? CLI$_NEGATED : CLI$_PRESENT;

        /* No explicit keyword: a DEFAULT keyword in the set counts as present
         * when the qualifier itself is present but carried no keyword values. */
        if (e->nvalues == 0 && e->qdef && e->qdef->keywords) {
            for (const struct cli_keyword *k = e->qdef->keywords; k->name; k++) {
                if ((k->flags & CLI_A_DEFAULT) &&
                    (abbrev_of(k->name, kw, kl) || (strlen(k->name) == kl && ci_eq_n(k->name, kw, kl))))
                    return CLI$_PRESENT;
            }
        }
        return CLI$_ABSENT;
    }

    return classify(ent_by_name(name, len));
}

/* ================================================================
 * cli$get_value
 * ================================================================ */

uint32_t cli$get_value(const struct dsc$descriptor_s *label,
                       struct dsc$descriptor_s *value,
                       uint16_t *retlen)
{
    if (retlen) *retlen = 0;
    if (!label || !label->dsc$a_pointer || !value || !g_ctx.valid)
        return CLI$_ABSENT;

    const char *name = label->dsc$a_pointer;
    size_t len = label->dsc$w_length;

    struct cli_parsed *e = ent_by_name(name, len);
    if (!e) return CLI$_ABSENT;
    if (e->negated) return CLI$_ABSENT;
    if (!(e->specified || e->is_default_on || e->defaulted)) return CLI$_ABSENT;
    if (e->getidx >= e->nvalues) return CLI$_ABSENT;

    const char *val = e->values[e->getidx];
    size_t vlen = strlen(val);
    e->getidx++;

    /* Deliver into the caller's descriptor. */
    if (value->dsc$b_class == DSC$K_CLASS_D) {
        /* dynamic string: (re)allocate to the exact length */
        char *buf = malloc(vlen ? vlen : 1);
        if (!buf) return SS$_INSFMEM;
        memcpy(buf, val, vlen);
        if (value->dsc$a_pointer) free(value->dsc$a_pointer);
        value->dsc$a_pointer = buf;
        value->dsc$w_length = (uint16_t)vlen;
        if (retlen) *retlen = (uint16_t)vlen;
    } else {
        size_t cap = value->dsc$w_length;
        size_t cpy = (vlen < cap) ? vlen : cap;
        if (value->dsc$a_pointer && cpy)
            memcpy(value->dsc$a_pointer, val, cpy);
        /* fixed-length string class pads the remainder with blanks (VMS) */
        if (value->dsc$b_class == DSC$K_CLASS_S && value->dsc$a_pointer && cap > cpy)
            memset(value->dsc$a_pointer + cpy, ' ', cap - cpy);
        if (retlen) *retlen = (uint16_t)cpy;
    }

    return (e->getidx < e->nvalues) ? CLI$_COMMA : SS$_NORMAL;
}
