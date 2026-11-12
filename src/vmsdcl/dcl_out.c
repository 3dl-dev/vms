/*
 * dcl_out.c - DCL's one output sink (rd vms-d4a0, vms-1b6a). See dcl/dcl_out.h.
 *
 * WHAT VMS DOES. DCL's output and its messages are records written to the
 * process-permanent files SYS$OUTPUT and SYS$ERROR: on a terminal each is a
 * $QIO IO$_WRITEVBLK with carriage control (a new line before the text, a
 * carriage return after); to a mailbox each is one mailbox message; to a disk
 * file each is an RMS $PUT.
 *
 * WHAT THIS DOES. Every call below renders its text and appends it to the
 * stream's record buffer; each line ended by '\n' becomes one record and goes
 * to the stream's ROUTE -- set at startup from what SYS$OUTPUT/SYS$ERROR name
 * (dcl_out_bind_ppf, dcl_io.c): the terminal driver, the SYS$OUTPUT mailbox,
 * or an RMS file. A line not yet ended waits for its end, as a line-buffered
 * stream would; a flush sends it as an open record (the terminal's "prompt"
 * carriage control, the next text continuing it) where the route can take one.
 * SYS$ERROR is not buffered: on the terminal what is written goes at once.
 *
 * THE ONE LIBC PATH. A stream with no route -- DCL with no executive (the
 * host test harness), or output that is not the executive's terminal, a
 * mailbox DCL bound, or a file (a pipe a parent reads) -- writes the process's
 * stdio stream exactly as before. That fallback is the block marked below and
 * nothing else in this file touches stdio (step E, rd vms-f6ac, removes it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcl/dcl_out.h"

#define DCL_OUT_REC 4096              /* a longer line goes as open pieces */

struct sink {
    dcl_out_emit_fn  emit;            /* NULL: the stdio fallback */
    void            *arg;
    unsigned         flags;           /* DCL_OUT_ROUTE_* */
    void           (*close)(void *arg);
    int              is_err;
    int              err;             /* a record the route refused */
    size_t           len;
    char             rec[DCL_OUT_REC];
};

static struct sink g_out = { .is_err = 0 };
static struct sink g_err = { .is_err = 1 };
/* /PAGE: SYS$OUTPUT's records go to the pager while it gathers them */
static dcl_out_emit_fn g_capture;
static void *g_capture_arg;

/* ================================================================
 * THE STDIO FALLBACK -- the only libc I/O in DCL's output path.
 * ================================================================ */
static FILE *fb_stream(const struct sink *s)
{
    return s->is_err ? stderr : stdout;
}

static int fb_write(const struct sink *s, const char *p, size_t n)
{
    return (n == 0 || fwrite(p, 1, n, fb_stream(s)) == n) ? 0 : -1;
}

static int fb_flush(const struct sink *s)        { return fflush(fb_stream(s)); }
static int fb_error(const struct sink *s)        { return ferror(fb_stream(s)); }
static void fb_clearerr(const struct sink *s)    { clearerr(fb_stream(s)); }
/* an image run in this process writes stdio: what it holds goes out before
 * DCL's next record does */
void dcl_out_stdio_sync(void)                    { fflush(stdout); fflush(stderr); }
/* ================================================================ */

static int capturing(const struct sink *s)
{
    return !s->is_err && g_capture;
}

static int routed(const struct sink *s)
{
    return s->emit || capturing(s);
}

/* a pager takes whole records; a long line goes as several */
static unsigned route_flags(const struct sink *s)
{
    return capturing(s) ? 0 : s->flags;
}

/* Send the buffered text: a whole record, or (`open`) the start of one. */
static int sink_emit(struct sink *s, int open)
{
    char rec[DCL_OUT_REC];
    size_t n = s->len;

    /* an AST that writes while a record is going out (CTRL/T's status line)
     * starts on an empty buffer, never on the one being sent */
    memcpy(rec, s->rec, n);
    s->len = 0;
    dcl_out_stdio_sync();
    int r = capturing(s) ? g_capture(g_capture_arg, rec, n, 0)
                         : s->emit(s->arg, rec, n, open);
    if (r != 0)
        s->err = 1;
    return r;
}

static int sink_write(struct sink *s, const char *p, size_t n)
{
    if (!routed(s))
        return fb_write(s, p, n);
    int r = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') {
            r |= sink_emit(s, 0);
            continue;
        }
        if (s->len == sizeof s->rec) {
            if (route_flags(s) & DCL_OUT_ROUTE_OPEN)
                r |= sink_emit(s, 1);
            else
                r |= sink_emit(s, 0);       /* a file takes it as a record */
        }
        s->rec[s->len++] = p[i];
    }
    /* SYS$ERROR is unbuffered: on a route that can leave a record open, a
     * partial line goes now */
    if (s->is_err && s->len && (route_flags(s) & DCL_OUT_ROUTE_OPEN))
        r |= sink_emit(s, 1);
    return r ? -1 : 0;
}

static int sink_flush(struct sink *s)
{
    if (!routed(s))
        return fb_flush(s);
    if (s->len && (route_flags(s) & DCL_OUT_ROUTE_OPEN))
        return sink_emit(s, 1) ? EOF : 0;
    return 0;
}

static int sink_vprintf(struct sink *s, const char *fmt, va_list ap)
{
    char small[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap2);
    va_end(ap2);
    if (n < 0)
        return n;
    if ((size_t)n < sizeof small)
        return sink_write(s, small, (size_t)n) == 0 ? n : -1;
    char *big = malloc((size_t)n + 1);
    if (!big)
        return -1;
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    int r = sink_write(s, big, (size_t)n) == 0 ? n : -1;
    free(big);
    return r;
}

static int sink_fputs(struct sink *s, const char *str)
{
    return sink_write(s, str, strlen(str)) == 0 ? 1 : EOF;
}

static int sink_puts(struct sink *s, const char *str)
{
    if (sink_fputs(s, str) == EOF || sink_write(s, "\n", 1) != 0)
        return EOF;
    return 1;
}

static void sink_close(struct sink *s)
{
    if (!s->emit)
        return;
    if (s->len)
        (void)sink_emit(s, (s->flags & DCL_OUT_ROUTE_OPEN) != 0);
    if (s->close)
        s->close(s->arg);
    s->emit = NULL;
    s->close = NULL;
}

static void dcl_out_atexit(void)
{
    sink_close(&g_err);
    sink_close(&g_out);
}

void dcl_out_route(int which, dcl_out_emit_fn emit, void *arg, unsigned flags,
                   void (*close)(void *arg))
{
    static int registered;
    struct sink *s = which ? &g_err : &g_out;

    if (s->emit)
        sink_close(s);
    else
        (void)fb_flush(s);              /* the fallback's text goes first */
    s->emit = emit;
    s->arg = arg;
    s->flags = flags;
    s->close = close;
    s->len = 0;
    s->err = 0;
    if (emit && !registered) {
        registered = 1;
        atexit(dcl_out_atexit);
    }
}

int dcl_out_routed(int which)
{
    return (which ? &g_err : &g_out)->emit != NULL;
}

/* ---- SYS$OUTPUT ---- */
int dcl_out_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = sink_vprintf(&g_out, fmt, ap);
    va_end(ap);
    return n;
}

int dcl_out_vprintf(const char *fmt, va_list ap) { return sink_vprintf(&g_out, fmt, ap); }
int dcl_out_puts(const char *s)  { return sink_puts(&g_out, s); }
int dcl_out_fputs(const char *s) { return sink_fputs(&g_out, s); }
int dcl_out_putc(int c)
{
    char ch = (char)c;
    return sink_write(&g_out, &ch, 1) == 0 ? (unsigned char)c : EOF;
}
size_t dcl_out_write(const void *buf, size_t len)
{
    return sink_write(&g_out, buf, len) == 0 ? len : 0;
}
int dcl_out_flush(void)          { return sink_flush(&g_out); }
int dcl_out_error(void)          { return routed(&g_out) ? g_out.err : fb_error(&g_out); }
void dcl_out_clearerr(void)
{
    if (routed(&g_out))
        g_out.err = 0;
    else
        fb_clearerr(&g_out);
}
void dcl_out_line_buffered(void) { setvbuf(stdout, NULL, _IOLBF, 0); }
void dcl_out_capture(dcl_out_emit_fn fn, void *arg)
{
    struct sink *s = &g_out;
    if (g_capture) {
        /* the gathered output's last line, even unended, is the pager's */
        if (s->len)
            (void)sink_emit(s, 0);
    } else {
        (void)sink_flush(s);
        if (!s->emit)
            (void)fb_flush(s);
    }
    g_capture = fn;
    g_capture_arg = arg;
    s->err = 0;
}

/* ---- SYS$ERROR ---- */
int dcl_err_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = sink_vprintf(&g_err, fmt, ap);
    va_end(ap);
    return n;
}

int dcl_err_vprintf(const char *fmt, va_list ap) { return sink_vprintf(&g_err, fmt, ap); }
int dcl_err_puts(const char *s)  { return sink_puts(&g_err, s); }
int dcl_err_fputs(const char *s) { return sink_fputs(&g_err, s); }
int dcl_err_putc(int c)
{
    char ch = (char)c;
    return sink_write(&g_err, &ch, 1) == 0 ? (unsigned char)c : EOF;
}
size_t dcl_err_write(const void *buf, size_t len)
{
    return sink_write(&g_err, buf, len) == 0 ? len : 0;
}
int dcl_err_flush(void)          { return sink_flush(&g_err); }
