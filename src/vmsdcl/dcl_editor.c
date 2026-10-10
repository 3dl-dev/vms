/*
 * dcl_editor.c - EDT, line mode (rd vms-f1e / vms-d442).
 *
 * Clean-room: built from the public EDT Reference Manual and the observed
 * behaviour of the VAX V7.3 console (keystroke cases EDT.LINE, EDT.LINE2;
 * goldens docs/oracle/keystroke/EDT.LINE*). Nothing here comes from VSI/HPE
 * source or binaries.
 *
 * What the console shows, and this reproduces:
 *   - EDIT/EDT of a new file: "Input file does not exist", then [EOB]; of an
 *     existing file, its first line, numbered. The prompt is "*" on a new line.
 *   - A line is shown as its number -- the whole part right-justified in five
 *     columns, then any fraction ".n" -- padded to column 12, then the text
 *     ("    1       ONE", "    0.1     NEW"); the end of the buffer is [EOB].
 *   - INSERT inserts before the current line; with ";text" it inserts that one
 *     line, otherwise it prompts with twelve spaces until CTRL/Z. Lines added
 *     at the end of the buffer are numbered on from the last whole number;
 *     lines added between two others take tenths (then hundredths ...).
 *     Afterwards the current line is shown.
 *   - RETURN at "*" moves to the next line and shows it; a range alone shows
 *     that range (TYPE); TYPE leaves the current line at the range's first.
 *   - FIND moves; "String was not found" when a search fails.
 *   - SUBSTITUTE/old/new/ [range] shows each changed line, then
 *     "n substitutions".
 *   - DELETE [range]: "n line(s) deleted", then the new current line.
 *   - An unknown command: " ^" under it, then "Unrecognized command".
 *   - EXIT writes a new version and says "DEV:[DIR]NAME.TYP;v n lines";
 *     QUIT leaves without writing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <strings.h>

#include "ssdef.h"
#include "dcl/context.h"
#include "dcl/dcl_rms.h"
#include "dcl/terminal.h"

#define EDT_LINE_MAX  4096
#define EDT_SCALE     100000L        /* line numbers carry five decimals */

struct edt_line {
    long  num;                      /* line number * EDT_SCALE */
    char *text;
};

struct edt_buf {
    struct edt_line *l;
    int    n, cap;
    int    cur;                     /* current line index; n = [EOB] */
};

/* ---------------------------------------------------------------- */

static int buf_insert(struct edt_buf *b, int at, long num, const char *text)
{
    if (b->n == b->cap) {
        int nc = b->cap ? b->cap * 2 : 64;
        struct edt_line *nl = realloc(b->l, (size_t)nc * sizeof *nl);
        if (!nl) return -1;
        b->l = nl;
        b->cap = nc;
    }
    char *t = strdup(text);
    if (!t) return -1;
    memmove(b->l + at + 1, b->l + at, (size_t)(b->n - at) * sizeof *b->l);
    b->l[at].num = num;
    b->l[at].text = t;
    b->n++;
    return 0;
}

static void buf_delete(struct edt_buf *b, int at)
{
    free(b->l[at].text);
    memmove(b->l + at, b->l + at + 1, (size_t)(b->n - at - 1) * sizeof *b->l);
    b->n--;
}

static void buf_free(struct edt_buf *b)
{
    for (int i = 0; i < b->n; i++) free(b->l[i].text);
    free(b->l);
    memset(b, 0, sizeof *b);
}

/* "    1       ONE", "    0.1     NEW", "[EOB]" */
static void show_line(const struct edt_buf *b, int i)
{
    if (i >= b->n) {
        printf("[EOB]\n");
        return;
    }
    char field[32];
    long w = b->l[i].num / EDT_SCALE, f = b->l[i].num % EDT_SCALE;
    int k = snprintf(field, sizeof field, "%5ld", w);
    if (f) {
        char fr[8];
        snprintf(fr, sizeof fr, "%05ld", f);
        for (int j = 4; j > 0 && fr[j] == '0'; j--) fr[j] = '\0';
        k += snprintf(field + k, sizeof field - (size_t)k, ".%s", fr);
    }
    printf("%-12s%s\n", field, b->l[i].text);
}

/* numbers for `count` lines inserted before index `at` */
static int number_run(const struct edt_buf *b, int at, int count, long *first,
                      long *step)
{
    long prev = at > 0 ? b->l[at - 1].num : 0;
    if (at >= b->n) {                         /* at the end: whole numbers */
        *first = (prev / EDT_SCALE + 1) * EDT_SCALE;
        *step = EDT_SCALE;
        return 0;
    }
    long next = b->l[at].num;
    for (long s = EDT_SCALE / 10; s >= 1; s /= 10) {
        long base = (prev / s) * s;
        if (base + (long)count * s < next) {
            *first = base + s;
            *step = s;
            return 0;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- */
/* ranges                                                             */

struct range { int a, b; int ok; };          /* indices; b inclusive */

static const char *skip(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int kw(const char **pp, const char *word, size_t minlen)
{
    const char *p = *pp;
    size_t n = 0;
    while (isalpha((unsigned char)p[n])) n++;
    if (n < minlen || n > strlen(word) || strncasecmp(p, word, n) != 0)
        return 0;
    *pp = p + n;
    return 1;
}

/* the index of line number `num` (or of the first line after it) */
static int index_of(const struct edt_buf *b, long num, int exact)
{
    for (int i = 0; i < b->n; i++) {
        if (b->l[i].num == num) return i;
        if (b->l[i].num > num) return exact ? -1 : i;
    }
    return b->n;
}

static int search(const struct edt_buf *b, int from, const char *s, size_t sl)
{
    for (int i = from; i < b->n; i++) {
        const char *t = b->l[i].text;
        size_t tl = strlen(t);
        for (size_t j = 0; j + sl <= tl; j++)
            if (strncasecmp(t + j, s, sl) == 0) return i;
    }
    return -1;
}

static int parse_number(const char **pp, long *num)
{
    const char *p = *pp;
    if (!isdigit((unsigned char)*p)) return 0;
    long w = 0, f = 0, scale = EDT_SCALE / 10;
    while (isdigit((unsigned char)*p)) w = w * 10 + (*p++ - '0');
    if (*p == '.' && isdigit((unsigned char)p[1])) {
        p++;
        while (isdigit((unsigned char)*p)) {
            if (scale) { f += (*p - '0') * scale; scale /= 10; }
            p++;
        }
    }
    *num = w * EDT_SCALE + f;
    *pp = p;
    return 1;
}

/* one end of a range: a number, ., BEGIN, END, "string" */
static int parse_point(const struct edt_buf *b, const char **pp, int *idx,
                       int *notfound)
{
    const char *p = skip(*pp);
    long num;
    if (parse_number(&p, &num)) {
        *idx = index_of(b, num, 0);
    } else if (*p == '.') {
        p++;
        *idx = b->cur;
    } else if (*p == '"' || *p == '\'') {
        char q = *p++;
        const char *e = strchr(p, q);
        size_t sl = e ? (size_t)(e - p) : strlen(p);
        int f = search(b, b->cur, p, sl);
        p += sl + (e ? 1 : 0);
        if (f < 0) { *notfound = 1; *idx = b->cur; }
        else *idx = f;
    } else if (kw(&p, "BEGIN", 1)) {
        *idx = 0;
    } else if (kw(&p, "END", 1)) {
        *idx = b->n;
    } else {
        return 0;
    }
    *pp = p;
    return 1;
}

/* a range, or the current line when none is given */
static struct range parse_range(const struct edt_buf *b, const char **pp,
                                int *notfound)
{
    struct range r = { b->cur, b->cur, 1 };
    const char *p = skip(*pp);
    const char *save = p;

    if (kw(&p, "WHOLE", 1)) {
        r.a = 0; r.b = b->n;                  /* through [EOB] */
    } else if (kw(&p, "REST", 1)) {
        r.a = b->cur; r.b = b->n;
    } else {
        p = save;
        int a;
        if (parse_point(b, &p, &a, notfound)) {
            r.a = r.b = a;
            const char *q = skip(p);
            const char *q2 = q;
            if (*q == ':' || (kw(&q2, "THRU", 3) && (q = q2))) {
                if (*q == ':') q++;
                int c;
                if (parse_point(b, &q, &c, notfound)) {
                    r.b = c;
                    p = q;
                } else {
                    r.ok = 0;
                }
            }
        } else {
            p = save;
        }
    }
    *pp = p;
    return r;
}

/* ---------------------------------------------------------------- */

static int read_cmd(char *buf, size_t sz)
{
    int r = dcl_tt_read_line("\r\n*", buf, sz);
    if (r != 0) return -1;
    if (buf[0] == 0x1A && buf[1] == '\0') return -1;
    return 0;
}

static void insert_mode(struct edt_buf *b)
{
    char line[EDT_LINE_MAX];
    char **acc = NULL;
    int na = 0, ca = 0;

    for (;;) {
        if (dcl_tt_read_line("\r\n            ", line, sizeof line) != 0)
            break;                           /* CTRL/Z */
        if (line[0] == 0x1A && line[1] == '\0')
            break;                           /* a CTRL/Z line from a procedure */
        if (na == ca) {
            ca = ca ? ca * 2 : 16;
            char **t = realloc(acc, (size_t)ca * sizeof *t);
            if (!t) break;
            acc = t;
        }
        acc[na] = strdup(line);
        if (!acc[na]) break;
        na++;
    }
    if (na) {
        long first, step;
        if (number_run(b, b->cur, na, &first, &step) != 0) {
            printf("Line number would exceed maximum\n");
        } else {
            for (int i = 0; i < na; i++)
                if (buf_insert(b, b->cur + i, first + step * i, acc[i]) != 0) break;
            b->cur += na;
        }
    }
    for (int i = 0; i < na; i++) free(acc[i]);
    free(acc);
    show_line(b, b->cur);
}

static int write_file(struct dcl_context *ctx, const struct edt_buf *b,
                      const char *spec)
{
    uint32_t st = 0;
    struct dcl_rms_writer *w = dcl_rms_write_create(ctx, spec, FAB$C_VAR,
                                                    FAB$M_CR, 0, &st);
    if (!w) {
        printf("Error opening output file %s\n", spec);
        return -1;
    }
    for (int i = 0; i < b->n; i++)
        if (dcl_rms_write_record(w, b->l[i].text, strlen(b->l[i].text)) != 0)
            break;
    dcl_rms_write_close(w);

    /* the new version's full name */
    char full[1100] = "", pat[1100];
    snprintf(pat, sizeof pat, "%s", spec);
    char *semi = strrchr(pat, ';');
    if (semi) *semi = '\0';
    strncat(pat, ";*", sizeof pat - strlen(pat) - 1);
    struct dcl_rms_dir *d = dcl_rms_dir_open(ctx, pat);
    if (d) {
        if (!dcl_rms_dir_next(d, full, sizeof full, NULL, NULL, NULL))
            full[0] = '\0';
        dcl_rms_dir_close(d);
    }
    printf("%s %d line%s\n", full[0] ? full : spec, b->n, b->n == 1 ? "" : "s");
    return 0;
}

int edt_run(struct dcl_context *ctx, const char *spec, const char *output,
            int read_only)
{
    struct edt_buf b;
    char line[EDT_LINE_MAX];
    uint32_t rst = 0;

    memset(&b, 0, sizeof b);
    struct dcl_rms_reader *r = dcl_rms_read_open(ctx, spec, &rst);
    if (r) {
        int eof = 0, len, k = 0;
        while ((len = dcl_rms_read_record(r, line, sizeof line, &eof)) >= 0)
            if (buf_insert(&b, b.n, (long)(++k) * EDT_SCALE, line) != 0) break;
        dcl_rms_read_close(r);
        b.cur = 0;
        show_line(&b, b.cur);
    } else {
        printf("Input file does not exist\n");
        show_line(&b, 0);
    }

    for (;;) {
        if (read_cmd(line, sizeof line) != 0) {
            if (feof(stdin)) {                /* input exhausted: as QUIT */
                buf_free(&b);
                return SS$_NORMAL;
            }
            continue;                         /* CTRL/Z at "*" does nothing */
        }
        const char *p = skip(line);

        if (*p == '\0') {                     /* RETURN: the next line */
            if (b.cur < b.n) b.cur++;
            show_line(&b, b.cur);
            continue;
        }

        const char *cmd = p;
        int nf = 0;

        if (kw(&p, "EXIT", 2)) {
            const char *f = skip(p);
            if (read_only && !*f) {
                buf_free(&b);
                return SS$_NORMAL;
            }
            write_file(ctx, &b, *f ? f : (output && *output ? output : spec));
            buf_free(&b);
            return SS$_NORMAL;
        }
        if (kw(&p, "QUIT", 4)) {
            buf_free(&b);
            return SS$_NORMAL;
        }
        if (kw(&p, "INSERT", 1)) {
            p = skip(p);
            if (*p && *p != ';') {
                struct range rg = parse_range(&b, &p, &nf);
                if (nf) { printf("String was not found\n"); continue; }
                b.cur = rg.a;
                p = skip(p);
            }
            if (*p == ';') {
                long first, step;
                if (number_run(&b, b.cur, 1, &first, &step) == 0 &&
                    buf_insert(&b, b.cur, first, p + 1) == 0)
                    b.cur++;
                show_line(&b, b.cur);
            } else {
                insert_mode(&b);
            }
            continue;
        }
        if (kw(&p, "FIND", 1)) {
            struct range rg = parse_range(&b, &p, &nf);
            if (nf) { printf("String was not found\n"); continue; }
            b.cur = rg.a;
            continue;
        }
        if (kw(&p, "DELETE", 1)) {
            struct range rg = parse_range(&b, &p, &nf);
            if (nf) { printf("String was not found\n"); continue; }
            int a = rg.a, z = rg.b < b.n ? rg.b : b.n - 1, cnt = 0;
            for (int i = z; i >= a && a < b.n; i--) { buf_delete(&b, i); cnt++; }
            b.cur = a <= b.n ? a : b.n;
            printf("%d line%s deleted\n", cnt, cnt == 1 ? "" : "s");
            show_line(&b, b.cur);
            continue;
        }
        if (kw(&p, "SUBSTITUTE", 1)) {
            p = skip(p);
            char dl = *p;
            if (!dl || isalnum((unsigned char)dl)) {
                printf(" ^\nUnrecognized command\n");
                continue;
            }
            const char *o = p + 1, *oe = strchr(o, dl);
            if (!oe) { printf("Invalid substitute command\n"); continue; }
            const char *nw = oe + 1, *ne = strchr(nw, dl);
            size_t ol = (size_t)(oe - o), nl = ne ? (size_t)(ne - nw) : strlen(nw);
            p = ne ? ne + 1 : nw + nl;
            struct range rg = parse_range(&b, &p, &nf);
            if (nf) { printf("String was not found\n"); continue; }
            int subs = 0, last = -1;
            for (int i = rg.a; i <= rg.b && i < b.n && ol; i++) {
                char out[EDT_LINE_MAX];
                size_t ok = 0;
                int here = 0;
                const char *t = b.l[i].text;
                while (*t && ok + nl + 1 < sizeof out) {
                    if (strncasecmp(t, o, ol) == 0) {
                        memcpy(out + ok, nw, nl);
                        ok += nl;
                        t += ol;
                        here++;
                    } else {
                        out[ok++] = *t++;
                    }
                }
                out[ok] = '\0';
                if (here) {
                    char *d = strdup(out);
                    if (d) { free(b.l[i].text); b.l[i].text = d; }
                    show_line(&b, i);
                    subs += here;
                    last = i;
                }
            }
            if (subs) {
                b.cur = last;
                printf("%d substitution%s\n", subs, subs == 1 ? "" : "s");
            } else {
                printf("String was not found\n");
            }
            continue;
        }
        if (kw(&p, "TYPE", 1) || (p = cmd, 1)) {
            const char *before = p;
            struct range rg = parse_range(&b, &p, &nf);
            if (*skip(p) != '\0' || (!rg.ok)) {
                /* not a command and not a range */
                printf("%*s^\nUnrecognized command\n", (int)(before - line) + 1, "");
                continue;
            }
            if (nf) {
                printf("String was not found\n");
                show_line(&b, b.cur);
                continue;
            }
            int z = rg.b > b.n ? b.n : rg.b;
            for (int i = rg.a; i <= z; i++) show_line(&b, i);
            b.cur = rg.a;
            continue;
        }
    }
}
