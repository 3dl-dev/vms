/*
 * dcl_out.c - DCL's one output sink (rd vms-d4a0). See dcl/dcl_out.h.
 *
 * Formatted output is rendered into a buffer and written as one piece, the
 * shape a record write ($QIO IO$_WRITEVBLK, RMS $PUT) takes. Today the piece
 * goes to the process's stdio stream exactly as the call it replaced did:
 * the bytes, buffering and flush points are unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcl/dcl_out.h"

static int sink_vprintf(FILE *s, const char *fmt, va_list ap)
{
    char small[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap2);
    va_end(ap2);
    if (n < 0)
        return n;
    if ((size_t)n < sizeof small)
        return fwrite(small, 1, (size_t)n, s) == (size_t)n ? n : -1;
    char *big = malloc((size_t)n + 1);
    if (!big)
        return -1;
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    int r = fwrite(big, 1, (size_t)n, s) == (size_t)n ? n : -1;
    free(big);
    return r;
}

static int sink_fputs(FILE *s, const char *str)
{
    size_t n = strlen(str);
    return (n == 0 || fwrite(str, 1, n, s) == n) ? 1 : EOF;
}

static int sink_puts(FILE *s, const char *str)
{
    if (sink_fputs(s, str) == EOF || fputc('\n', s) == EOF)
        return EOF;
    return 1;
}

int dcl_out_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = sink_vprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int dcl_out_vprintf(const char *fmt, va_list ap) { return sink_vprintf(stdout, fmt, ap); }
int dcl_out_puts(const char *s)  { return sink_puts(stdout, s); }
int dcl_out_fputs(const char *s) { return sink_fputs(stdout, s); }
int dcl_out_putc(int c)          { return fputc(c, stdout); }
size_t dcl_out_write(const void *buf, size_t len)
{
    return len ? fwrite(buf, 1, len, stdout) : 0;
}
int dcl_out_flush(void)          { return fflush(stdout); }
int dcl_out_error(void)          { return ferror(stdout); }
void dcl_out_clearerr(void)      { clearerr(stdout); }
void dcl_out_line_buffered(void) { setvbuf(stdout, NULL, _IOLBF, 0); }
FILE *dcl_out_stream(void)       { return stdout; }

int dcl_err_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = sink_vprintf(stderr, fmt, ap);
    va_end(ap);
    return n;
}

int dcl_err_vprintf(const char *fmt, va_list ap) { return sink_vprintf(stderr, fmt, ap); }
int dcl_err_puts(const char *s)  { return sink_puts(stderr, s); }
int dcl_err_fputs(const char *s) { return sink_fputs(stderr, s); }
int dcl_err_putc(int c)          { return fputc(c, stderr); }
size_t dcl_err_write(const void *buf, size_t len)
{
    return len ? fwrite(buf, 1, len, stderr) : 0;
}
int dcl_err_flush(void)          { return fflush(stderr); }
