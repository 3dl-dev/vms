/*
 * dcl_out.h - DCL's one output sink: SYS$OUTPUT and SYS$ERROR (rd vms-d4a0).
 *
 * Every message and every line of output DCL writes goes through these calls;
 * no other src/vmsdcl file writes the process's output or error stream
 * (tests/dcl/test_no_libc_io.sh). Each call mirrors the stdio call it
 * replaced. Lines become records, routed (rd vms-1b6a) to where SYS$OUTPUT /
 * SYS$ERROR name: the terminal through $QIO, a mailbox through $QIO, a file
 * through RMS $PUT (dcl_out_bind_ppf, dcl_io.c). A stream with no route is the
 * process's stdio stream, the one libc path left (rd vms-f6ac removes it).
 */
#ifndef DCL_OUT_H
#define DCL_OUT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

/* SYS$OUTPUT */
int    dcl_out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    dcl_out_vprintf(const char *fmt, va_list ap) __attribute__((format(printf, 1, 0)));
int    dcl_out_puts(const char *s);          /* s and a newline (puts) */
int    dcl_out_fputs(const char *s);         /* s alone (fputs) */
int    dcl_out_putc(int c);
size_t dcl_out_write(const void *buf, size_t len);
int    dcl_out_flush(void);
int    dcl_out_error(void);                  /* the stream's error flag */
void   dcl_out_clearerr(void);
void   dcl_out_line_buffered(void);          /* SYS$OUTPUT became a pipe */

/* /PAGE: SYS$OUTPUT is gathered into `f` until dcl_out_capture(NULL). */
void   dcl_out_capture(FILE *f);
/* What an image run in this process left in its stdio buffers goes to fd 1/2
 * now, ahead of DCL's next record. */
void   dcl_out_stdio_sync(void);

/* SYS$ERROR */
int    dcl_err_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    dcl_err_vprintf(const char *fmt, va_list ap) __attribute__((format(printf, 1, 0)));
int    dcl_err_puts(const char *s);
int    dcl_err_fputs(const char *s);
int    dcl_err_putc(int c);
size_t dcl_err_write(const void *buf, size_t len);
int    dcl_err_flush(void);

/* ROUTES (rd vms-1b6a). `emit` takes one record (no '\n'), or with `open`
 * the start of one the next text continues; 0 when the destination took it.
 * which: 0 SYS$OUTPUT, 1 SYS$ERROR. emit NULL: back to the stdio stream.
 * `close` (may be NULL) runs when the route is replaced or at exit, after the
 * last held text has gone. */
typedef int (*dcl_out_emit_fn)(void *arg, const char *rec, size_t n, int open);
#define DCL_OUT_ROUTE_OPEN 0x1u       /* the destination can leave a record open */
void   dcl_out_route(int which, dcl_out_emit_fn emit, void *arg, unsigned flags,
                     void (*close)(void *arg));
int    dcl_out_routed(int which);
/* Route SYS$OUTPUT/SYS$ERROR from the process-permanent files they name
 * (dcl_io.c): at startup, and again once a mailbox SYS$OUTPUT is bound. */
void   dcl_out_bind_ppf(void);

#endif /* DCL_OUT_H */
