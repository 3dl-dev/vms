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
    for (long s = EDT_SCALE; s >= 1; s /= 10) {
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

/* ================================================================ */
/* keypad (screen) mode, CHANGE -- rd vms-c37                        */
/*                                                                  */
/* What the VAX V7.3 console shows on a VT100 (keystroke EDT.SCREEN,  */
/* EDT.KEYPAD) and this reproduces:                                   */
/*  - entry: ESC \ ESC < ESC[?6l ESC[m ESC)0 ESC[4l ESC[H ESC[?3l     */
/*    ESC[;22r ESC[H ESC= ESC[J, the lines from the top ("text CR LF",  */
/*    [EOB] last), then the cursor; rows 1-22 are the text, 23 the      */
/*    prompt line, 24 the message line;                                 */
/*  - a key that arrives as an escape sequence is processed between     */
/*    ESC[?8l and ESC[?8h (GOLD and the key it prefixes, once);         */
/*  - cursor motion is the shortest of: CR (to column 1), BS (one left),*/
/*    ESC[nC / ESC[nD on the row, LF (one down) or ESC[nA (up) in the   */
/*    column, CR LF (next row, column 1), else ESC[r;cH with r or c     */
/*    left out when 1; after ESC[t;br the cursor is home;               */
/*  - an edit rewrites the line from the change: the text, ESC[K, then  */
/*    back to the cursor; PASTE rewrites the whole line;                */
/*  - removing a row: ESC[r;22r ESC[22H LF, then what is below [EOB]    */
/*    erased (ESC[J), ESC[;22r; adding one: ESC[r;22r LF ESC M text     */
/*    ESC[;22r;                                                         */
/*  - GOLD shows ESC[7m, the next command ESC[m; a SELECT range is in    */
/*    reverse video; a message is BEL ESC[24H ESC[K ESC[7m text ESC[m,  */
/*    and the next key clears rows 23-24 (ESC[23H ESC[J).               */
/* ================================================================ */

#define WROWS 22

struct scr {
    char   out[16384];
    size_t on;
    int    r, c, known;               /* the terminal's cursor */
    int    rev;                       /* reverse video is on */
    int    top;                       /* buffer index on row 1 */
    int    msg;                       /* rows 23-24 hold something */
    int    eob_row;                   /* row [EOB] is shown on */
};

static void so(struct scr *s, const char *t)
{
    size_t l = strlen(t);
    if (s->on + l >= sizeof s->out) { dcl_tt_raw_write(s->out, s->on); s->on = 0; }
    memcpy(s->out + s->on, t, l);
    s->on += l;
}

static void sflush(struct scr *s)
{
    dcl_tt_raw_write(s->out, s->on);
    s->on = 0;
}

/* text written at the cursor moves it */
static void stext(struct scr *s, const char *t, size_t n)
{
    char b[EDT_LINE_MAX + 1];
    if (n > EDT_LINE_MAX) n = EDT_LINE_MAX;
    memcpy(b, t, n);
    b[n] = '\0';
    so(s, b);
    s->c += (int)n;
}

static void srend(struct scr *s, int rev)
{
    if (rev == s->rev) return;
    so(s, rev ? "\033[7m" : "\033[m");
    s->rev = rev;
}

static void smove(struct scr *s, int r, int c)
{
    char b[32];
    if (s->known && r == s->r && c == s->c) return;
    if (s->known && r == s->r) {
        if (c > s->c) {
            if (c - s->c == 1) so(s, "\033[C");
            else { snprintf(b, sizeof b, "\033[%dC", c - s->c); so(s, b); }
        } else if (c == 1) {
            so(s, "\r");
        } else if (s->c - c == 1) {
            so(s, "\b");
        } else {
            snprintf(b, sizeof b, "\033[%dD", s->c - c); so(s, b);
        }
    } else if (s->known && c == s->c && r == s->r + 1) {
        so(s, "\n");
    } else if (s->known && c == s->c && r < s->r) {
        if (s->r - r == 1) so(s, "\033[A");
        else { snprintf(b, sizeof b, "\033[%dA", s->r - r); so(s, b); }
    } else if (s->known && c == 1 && r == s->r + 1) {
        so(s, "\r\n");
    } else {
        if (r == 1 && c == 1) snprintf(b, sizeof b, "\033[H");
        else if (c == 1) snprintf(b, sizeof b, "\033[%dH", r);
        else if (r == 1) snprintf(b, sizeof b, "\033[;%dH", c);
        else snprintf(b, sizeof b, "\033[%d;%dH", r, c);
        so(s, b);
    }
    s->r = r; s->c = c; s->known = 1;
}

static void sregion(struct scr *s, int t, int b)
{
    char x[32];
    if (t == 1) snprintf(x, sizeof x, "\033[;%dr", b);
    else snprintf(x, sizeof x, "\033[%d;%dr", t, b);
    so(s, x);
    s->r = 1; s->c = 1; s->known = 1;  /* DECSTBM homes the cursor */
}

struct kp {
    struct edt_buf *b;
    struct scr s;
    int li, off;                       /* the cursor in the buffer */
    int dir;                           /* 1 advance, -1 backup */
    int sel, sel_li, sel_off;          /* SELECT range start */
    char *delw, *delc, *dell, *paste;  /* the delete buffers, PASTE */
    char find[256];
};

static int row_of(const struct kp *k, int li) { return li - k->s.top + 1; }

static const char *ltext(const struct kp *k, int li)
{
    return li < k->b->n ? k->b->l[li].text : "";
}

/* rewrite row `li` from column `col`: the text, then ESC[K */
static void spaint(struct kp *k, int li, int col)
{
    const char *t = ltext(k, li);
    size_t len = strlen(t);
    smove(&k->s, row_of(k, li), col);
    if ((size_t)(col - 1) < len)
        stext(&k->s, t + col - 1, len - (size_t)(col - 1));
    so(&k->s, "\033[K");
}

static void scursor(struct kp *k)
{
    smove(&k->s, row_of(k, k->li), k->off + 1);
}

static void sclear_msg(struct kp *k)
{
    if (!k->s.msg) return;
    smove(&k->s, 23, 1);
    so(&k->s, "\033[J");
    k->s.msg = 0;
}

static void smessage(struct kp *k, const char *m)
{
    so(&k->s, "\a");
    smove(&k->s, 24, 1);
    so(&k->s, "\033[K");
    srend(&k->s, 1);
    stext(&k->s, m, strlen(m));
    srend(&k->s, 0);
    k->s.msg = 1;
}

/* below [EOB]: erased, from the row after it */
static void serase_below(struct kp *k)
{
    int er = row_of(k, k->b->n) + 1;
    if (er <= WROWS) {
        smove(&k->s, er, 1);
        so(&k->s, "\033[J");
    }
}

static void sfull(struct kp *k)
{
    so(&k->s, "\033\\\033<\033[?6l\033[m\033)0\033[4l\033[H\033[?3l");
    sregion(&k->s, 1, WROWS);
    so(&k->s, "\033[H\033=\033[J");
    k->s.r = 1; k->s.c = 1; k->s.known = 1;
    k->s.rev = 0;
    if (k->li < k->s.top || k->li >= k->s.top + WROWS)
        k->s.top = k->li;
    for (int i = k->s.top, row = 1; row <= WROWS; i++, row++) {
        if (i < k->b->n) {
            stext(&k->s, k->b->l[i].text, strlen(k->b->l[i].text));
            if (row < WROWS) { so(&k->s, "\r\n"); k->s.r++; k->s.c = 1; }
        } else {
            stext(&k->s, "[EOB]", 5);
            break;
        }
    }
    scursor(k);
}

/* the buffer lost the row for line `li` (still on the screen): close it up */
static void sdelete_row(struct kp *k, int li)
{
    int r = row_of(k, li);
    char x[32];
    if (r < 1 || r > WROWS) return;
    if (r == 1) snprintf(x, sizeof x, "\033[;%dr", WROWS);
    else snprintf(x, sizeof x, "\033[%d;%dr", r, WROWS);
    so(&k->s, x);
    k->s.known = 0;                    /* see below: ESC[22H, absolute */
    smove(&k->s, WROWS, 1);
    so(&k->s, "\n");
    /* the row that scrolled in at the bottom */
    int bl = k->s.top + WROWS - 1;
    if (bl < k->b->n) {
        stext(&k->s, k->b->l[bl].text, strlen(k->b->l[bl].text));
    } else if (bl == k->b->n) {
        stext(&k->s, "[EOB]", 5);
    } else {
        serase_below(k);
    }
    sregion(&k->s, 1, WROWS);
}

/* the buffer gained line `li`: open a row for it and write it */
static void sinsert_row(struct kp *k, int li)
{
    int r = row_of(k, li);
    if (r < 1 || r > WROWS) return;
    if (r > 1) {
        sregion(&k->s, r, WROWS);
        /* sregion homed the cursor to (1,1); the region's top is row r */
        smove(&k->s, r, 1);
    }
    so(&k->s, "\033M");
    stext(&k->s, ltext(k, li), strlen(ltext(k, li)));
    sregion(&k->s, 1, WROWS);
}

static int is_word_sep(int c) { return c == ' ' || c == '\t'; }

/* end of the word at `off`: through the word and the spaces after it */
static int word_end(const char *t, int off)
{
    int n = (int)strlen(t), i = off;
    if (i >= n) return n;
    while (i < n && !is_word_sep((unsigned char)t[i])) i++;
    while (i < n && is_word_sep((unsigned char)t[i])) i++;
    return i;
}

static void set_text(struct kp *k, int li, const char *t)
{
    char *d = strdup(t);
    if (!d) return;
    free(k->b->l[li].text);
    k->b->l[li].text = d;
}

static void str_set(char **dst, const char *t, size_t n)
{
    char *d = malloc(n + 1);
    if (!d) return;
    memcpy(d, t, n);
    d[n] = '\0';
    free(*dst);
    *dst = d;
}

/* insert `t` (may hold '\n') at the cursor; the cursor stays before it unless
 * `after` */
static void ins_text(struct kp *k, const char *t, int after, int whole_line)
{
    if (k->li >= k->b->n) {
        long first, step;
        if (number_run(k->b, k->b->n, 1, &first, &step) != 0) return;
        buf_insert(k->b, k->b->n, first, "");
        k->off = 0;
    }
    const char *cur = k->b->l[k->li].text;
    char head[EDT_LINE_MAX], tail[EDT_LINE_MAX];
    snprintf(head, sizeof head, "%.*s", k->off, cur);
    snprintf(tail, sizeof tail, "%s", cur + k->off);
    const char *nl = strchr(t, '\n');
    if (!nl) {
        char line[EDT_LINE_MAX * 2];
        snprintf(line, sizeof line, "%s%s%s", head, t, tail);
        set_text(k, k->li, line);
        int col = whole_line ? 1 : k->off + 1;
        spaint(k, k->li, col);
        if (after) k->off += (int)strlen(t);
        scursor(k);
        return;
    }
    /* one line break: head+first piece, then the rest + tail as a new line */
    char a[EDT_LINE_MAX * 2], bb[EDT_LINE_MAX * 2];
    snprintf(a, sizeof a, "%s%.*s", head, (int)(nl - t), t);
    snprintf(bb, sizeof bb, "%s%s", nl + 1, tail);
    set_text(k, k->li, a);
    long first, step;
    if (number_run(k->b, k->li + 1, 1, &first, &step) == 0)
        buf_insert(k->b, k->li + 1, first, bb);
    sinsert_row(k, k->li + 1);
    spaint(k, k->li, k->off + 1);
    if (after) { k->li++; k->off = (int)strlen(nl + 1); }
    scursor(k);
}

static void kp_move(struct kp *k, int li, int off)
{
    k->li = li;
    k->off = off;
}

/* one cursor step right/left, crossing line ends */
static int step_right(struct kp *k)
{
    int len = (int)strlen(ltext(k, k->li));
    if (k->li >= k->b->n) return 0;
    if (k->off < len) k->off++;
    else { k->li++; k->off = 0; }
    return 1;
}

static int step_left(struct kp *k)
{
    if (k->off > 0) { k->off--; return 1; }
    if (k->li == 0) return 0;
    k->li--;
    k->off = (int)strlen(ltext(k, k->li));
    return 1;
}

static void sel_paint_step(struct kp *k, int from_li, int from_off)
{
    /* moving right inside a SELECT range: the character passed is shown in
     * reverse (keystroke EDT.KEYPAD S2) */
    if (k->li == from_li && k->off == from_off + 1) {
        smove(&k->s, row_of(k, from_li), from_off + 1);
        srend(&k->s, 1);
        stext(&k->s, ltext(k, from_li) + from_off, 1);
    }
}

static int kp_search(struct kp *k, int dir, int *fli, int *foff)
{
    size_t sl = strlen(k->find);
    if (!sl) return 0;
    int li = k->li, off = k->off;
    if (dir > 0) {
        off++;
        for (; li < k->b->n; li++, off = 0) {
            const char *t = k->b->l[li].text;
            int n = (int)strlen(t);
            for (int j = off; j + (int)sl <= n; j++)
                if (strncasecmp(t + j, k->find, sl) == 0) { *fli = li; *foff = j; return 1; }
        }
    } else {
        off--;
        for (; li >= 0; li--) {
            const char *t = li < k->b->n ? k->b->l[li].text : "";
            int n = (int)strlen(t);
            if (li != k->li) off = n - (int)sl;
            for (int j = off; j >= 0; j--)
                if (j + (int)sl <= n && strncasecmp(t + j, k->find, sl) == 0) {
                    *fli = li; *foff = j; return 1;
                }
        }
    }
    return 0;
}

/* read a key: a character, or an escape sequence as 0x100 + its final byte
 * (CSI) or 0x200 + its final byte (SS3: the keypad) */
static int kp_key(void)
{
    int c = dcl_tt_getc(0);
    if (c != 0x1B) return c;
    int c2 = dcl_tt_getc(0);
    if (c2 == '[') {
        int f;
        do { f = dcl_tt_getc(0); } while (f >= 0 && f < 0x40);
        return 0x100 | (f & 0xFF);
    }
    if (c2 == 'O') {
        int f = dcl_tt_getc(0);
        return 0x200 | (f & 0xFF);
    }
    return c2 < 0 ? 0x1B : 0x300 | (c2 & 0xFF);
}

#define K_UP     (0x100 | 'A')
#define K_DOWN   (0x100 | 'B')
#define K_RIGHT  (0x100 | 'C')
#define K_LEFT   (0x100 | 'D')
#define K_PF1    (0x200 | 'P')
#define K_PF2    (0x200 | 'Q')
#define K_PF3    (0x200 | 'R')
#define K_PF4    (0x200 | 'S')
#define K_KP(n)  (0x200 | ('p' + (n)))
#define K_MINUS  (0x200 | 'm')
#define K_COMMA  (0x200 | 'l')
#define K_DOT    (0x200 | 'n')
#define K_ENTER  (0x200 | 'M')

/* GOLD FIND: "Search for: " on row 23, the string typed after it */
static int kp_find_prompt(struct kp *k)
{
    char str[256];
    size_t n = 0;
    int first = 1;
    smove(&k->s, 23, 1);
    so(&k->s, "\033[K");
    stext(&k->s, "Search for: ", 12);
    /* the ESC[?8h waits for the first echo (keystroke EDT.KEYPAD F, F2) */
    sflush(&k->s);
    for (;;) {
        int c = kp_key();
        if (c == K_ENTER || c == K_KP(4) || c == K_KP(5) || c == '\r') {
            if (c == K_KP(4)) k->dir = 1;
            if (c == K_KP(5)) k->dir = -1;
            break;
        }
        if (c < 0 || c == 0x1A) return 0;
        if (c >= 0x20 && c < 0x7F && n + 1 < sizeof str) {
            if (first) { so(&k->s, "\033[?8h"); first = 0; }
            char e[2] = { (char)c, 0 };
            stext(&k->s, e, 1);
            str[n++] = (char)c;
            sflush(&k->s);
        } else if (c == 0x7F && n) {
            n--;
            so(&k->s, "\b \b");
            k->s.c--;
            sflush(&k->s);
        }
    }
    if (first) so(&k->s, "\033[?8h");
    k->s.known = 0;                     /* EDT does not track the prompt line
                                         * (EDT.KEYPAD F3: ESC[23H, absolute) */
    str[n] = '\0';
    if (n) snprintf(k->find, sizeof k->find, "%s", str);
    so(&k->s, "\033[?8l");
    /* clear the prompt line, back to where the cursor was */
    smove(&k->s, 23, 1);
    so(&k->s, "\033[J");
    k->s.msg = 0;
    scursor(k);
    return 1;
}

static void kp_after_vertical(struct kp *k)
{
    int len = (int)strlen(ltext(k, k->li));
    if (k->off > len) k->off = len;
    if (k->li >= k->b->n) k->off = 0;
}

/* returns 0 to go back to line mode */
static int kp_command(struct kp *k, int key, int gold)
{
    struct edt_buf *b = k->b;
    const char *t = ltext(k, k->li);
    int len = (int)strlen(t);

    if (key == K_RIGHT || key == K_LEFT) {
        int fl = k->li, fo = k->off;
        if (key == K_RIGHT) step_right(k); else step_left(k);
        if (k->sel && key == K_RIGHT) sel_paint_step(k, fl, fo);
        scursor(k);
        return 1;
    }
    if (key == K_UP || key == K_DOWN) {
        if (key == K_UP && k->li > 0) k->li--;
        if (key == K_DOWN && k->li < b->n) k->li++;
        kp_after_vertical(k);
        scursor(k);
        return 1;
    }
    if (!(key == K_PF3 && gold))
        srend(&k->s, 0);                /* a command: GOLD's reverse ends;
                                         * FIND's after its prompt */
    if (key == K_KP(4)) { k->dir = 1; return 1; }
    if (key == K_KP(5)) { k->dir = -1; return 1; }
    if (key == K_DOT) {                 /* SELECT */
        k->sel = 1; k->sel_li = k->li; k->sel_off = k->off;
        return 1;
    }
    if (key == K_MINUS && !gold) {      /* DEL W */
        int e = word_end(t, k->off);
        if (k->off >= len) return 1;
        str_set(&k->delw, t + k->off, (size_t)(e - k->off));
        char nt[EDT_LINE_MAX];
        snprintf(nt, sizeof nt, "%.*s%s", k->off, t, t + e);
        set_text(k, k->li, nt);
        spaint(k, k->li, k->off + 1);
        scursor(k);
        return 1;
    }
    if (key == K_MINUS && gold) {       /* UND W */
        if (k->delw) ins_text(k, k->delw, 0, 0);
        return 1;
    }
    if (key == K_COMMA && !gold) {      /* DEL C */
        if (k->off >= len) return 1;
        str_set(&k->delc, t + k->off, 1);
        char nt[EDT_LINE_MAX];
        snprintf(nt, sizeof nt, "%.*s%s", k->off, t, t + k->off + 1);
        set_text(k, k->li, nt);
        spaint(k, k->li, k->off + 1);
        scursor(k);
        return 1;
    }
    if (key == K_COMMA && gold) {       /* UND C */
        if (k->delc) ins_text(k, k->delc, 0, 0);
        return 1;
    }
    if (key == K_PF4 && !gold) {        /* DEL L: to the end of the line, with its end */
        if (k->li >= b->n) return 1;
        char saved[EDT_LINE_MAX + 2];
        snprintf(saved, sizeof saved, "%s\n", t + k->off);
        str_set(&k->dell, saved, strlen(saved));
        if (k->li + 1 < b->n) {
            char nt[EDT_LINE_MAX * 2];
            snprintf(nt, sizeof nt, "%.*s%s", k->off, t, b->l[k->li + 1].text);
            set_text(k, k->li, nt);
            buf_delete(b, k->li + 1);
            sdelete_row(k, k->li + 1);
        } else {
            char nt[EDT_LINE_MAX];
            snprintf(nt, sizeof nt, "%.*s", k->off, t);
            set_text(k, k->li, nt);
        }
        spaint(k, k->li, k->off + 1);
        serase_below(k);
        scursor(k);
        return 1;
    }
    if (key == K_PF4 && gold) {         /* UND L */
        if (k->dell) ins_text(k, k->dell, 0, 0);
        return 1;
    }
    if (key == K_KP(6) && !gold) {      /* CUT */
        if (!k->sel) { smessage(k, "No select range active"); scursor(k); return 1; }
        int a_li = k->sel_li, a_off = k->sel_off, z_li = k->li, z_off = k->off;
        if (z_li < a_li || (z_li == a_li && z_off < a_off)) {
            int x = a_li; a_li = z_li; z_li = x; x = a_off; a_off = z_off; z_off = x;
        }
        k->sel = 0;
        if (a_li == z_li) {
            const char *lt = ltext(k, a_li);
            str_set(&k->paste, lt + a_off, (size_t)(z_off - a_off));
            char nt[EDT_LINE_MAX];
            snprintf(nt, sizeof nt, "%.*s%s", a_off, lt, lt + z_off);
            set_text(k, a_li, nt);
            k->li = a_li; k->off = a_off;
            scursor(k);
            spaint(k, a_li, a_off + 1);
            scursor(k);
        }
        return 1;
    }
    if (key == K_KP(6) && gold) {       /* PASTE */
        if (k->paste) ins_text(k, k->paste, 1, 1);
        return 1;
    }
    if (key == K_PF3) {                 /* FIND (GOLD), FNDNXT */
        if (gold && !kp_find_prompt(k)) return 1;
        int fli, foff;
        if (kp_search(k, k->dir, &fli, &foff)) {
            if (gold) srend(&k->s, 0);
            kp_move(k, fli, foff);
            scursor(k);
        } else {
            smessage(k, "String was not found");
            scursor(k);
        }
        return 1;
    }
    if (key == K_KP(0)) {               /* LINE */
        if (k->dir > 0) {
            if (k->li >= b->n) { smessage(k, "Advance past bottom of buffer"); scursor(k); return 1; }
            k->li++; k->off = 0;
        } else {
            if (k->off > 0) k->off = 0;
            else if (k->li > 0) k->li--;
            else { smessage(k, "Backup past top of buffer"); scursor(k); return 1; }
        }
        scursor(k);
        return 1;
    }
    if (key == K_KP(2)) {               /* EOL */
        if (k->dir > 0) {
            if (k->li >= b->n) { smessage(k, "Advance past bottom of buffer"); scursor(k); return 1; }
            if (k->off < len) k->off = len;
            else { k->li++; k->off = (int)strlen(ltext(k, k->li)); }
        } else {
            if (k->li == 0) { smessage(k, "Backup past top of buffer"); scursor(k); return 1; }
            k->li--; k->off = (int)strlen(ltext(k, k->li));
        }
        scursor(k);
        return 1;
    }
    if (key == K_KP(1)) {               /* WORD */
        if (k->dir > 0) {
            if (k->li >= b->n) { smessage(k, "Advance past bottom of buffer"); scursor(k); return 1; }
            if (k->off >= len) { k->li++; k->off = 0; }
            else k->off = word_end(t, k->off);
        } else {
            if (k->off == 0) {
                if (k->li == 0) { smessage(k, "Backup past top of buffer"); scursor(k); return 1; }
                k->li--; k->off = (int)strlen(ltext(k, k->li));
            } else {
                int i = k->off;
                while (i > 0 && is_word_sep((unsigned char)t[i - 1])) i--;
                while (i > 0 && !is_word_sep((unsigned char)t[i - 1])) i--;
                k->off = i;
            }
        }
        scursor(k);
        return 1;
    }
    if (key == K_KP(3)) {               /* CHAR */
        if (k->dir > 0) step_right(k); else step_left(k);
        scursor(k);
        return 1;
    }
    return 1;
}

/* CHANGE: keypad editing until CTRL/Z */
static void kp_run(struct edt_buf *b, struct kp *k)
{
    k->b = b;
    k->li = b->cur;
    k->off = 0;
    k->dir = 1;
    sfull(k);
    sflush(&k->s);
    for (;;) {
        int key = kp_key();
        if (key == -2) break;
        if (key < 0) continue;
        if (key == 0x1A) {              /* CTRL/Z: back to line mode */
            so(&k->s, "\033>");
            sregion(&k->s, 1, 24);
            k->s.known = 0;
            smove(&k->s, 24, 1);
            so(&k->s, "\n");
            sflush(&k->s);
            break;
        }
        int esc = key >= 0x100;
        if (esc) so(&k->s, "\033[?8l");
        int gold = 0;
        if (key == K_PF1) {
            srend(&k->s, 1);
            gold = 1;
            key = kp_key();
            if (key < 0) break;
        }
        if (k->s.msg) {                 /* the next key clears a message */
            sclear_msg(k);
            scursor(k);
        }
        if (key >= 0x20 && key < 0x7F) {          /* typed text */
            char c[2] = { (char)key, 0 };
            ins_text(k, c, 1, 0);
        } else if (key == 0x7F) {                 /* DELETE: the character before */
            if (k->off > 0) {
                const char *t = ltext(k, k->li);
                str_set(&k->delc, t + k->off - 1, 1);
                char nt[EDT_LINE_MAX];
                snprintf(nt, sizeof nt, "%.*s%s", k->off - 1, t, t + k->off);
                set_text(k, k->li, nt);
                k->off--;
                spaint(k, k->li, k->off + 1);
                scursor(k);
            }
        } else if (key == '\r') {
            ins_text(k, "\n", 1, 0);
        } else {
            kp_command(k, key, gold);
        }
        if (esc) so(&k->s, "\033[?8h");
        sflush(&k->s);
    }
    b->cur = k->li;
}

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

/* leaving EDT after keypad mode: the terminal put back (keystroke
 * EDT.SCREEN/EDT.KEYPAD Q) */
static void screen_reset(int used)
{
    static const char r[] = "\r\n\033[m\033)B\033[;r\033[24H\r\033>\r\n";
    if (used)
        dcl_tt_qio_write(r, sizeof r - 1);  /* its CR LF leaves nothing owed */
}

int edt_run(struct dcl_context *ctx, const char *spec, const char *output,
            int read_only)
{
    int screen_used = 0;
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

        if (kw(&p, "CHANGE", 1)) {
            struct dcl_context *c = ctx;
            if (!c || (strncasecmp(c->terminal.device_type, "VT1", 3) != 0 &&
                       strncasecmp(c->terminal.device_type, "VT2", 3) != 0 &&
                       strncasecmp(c->terminal.device_type, "VT3", 3) != 0 &&
                       strncasecmp(c->terminal.device_type, "VT4", 3) != 0 &&
                       strncasecmp(c->terminal.device_type, "VT5", 3) != 0)) {
                printf("Screen editing is supported only on a VT100-class terminal\n");
                continue;
            }
            static struct kp k;
            memset(&k, 0, sizeof k);
            kp_run(&b, &k);
            screen_used = 1;
            free(k.delw); free(k.delc); free(k.dell); free(k.paste);
            continue;
        }
        if (kw(&p, "EXIT", 2)) {
            const char *f = skip(p);
            if (read_only && !*f) {
                screen_reset(screen_used);
                buf_free(&b);
                return SS$_NORMAL;
            }
            screen_reset(screen_used);
            write_file(ctx, &b, *f ? f : (output && *output ? output : spec));
            buf_free(&b);
            return SS$_NORMAL;
        }
        if (kw(&p, "QUIT", 4)) {
            screen_reset(screen_used);
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
