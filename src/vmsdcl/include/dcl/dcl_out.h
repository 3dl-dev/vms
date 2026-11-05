/*
 * dcl_out.h - DCL's one output sink: SYS$OUTPUT and SYS$ERROR (rd vms-d4a0).
 *
 * Every message and every line of output DCL writes goes through these calls;
 * no other src/vmsdcl file writes the process's output or error stream
 * (tests/dcl/test_no_libc_io.sh). Each call mirrors the stdio call it
 * replaced -- same bytes, same buffering, same flush points -- so the sink is
 * the single place that later routes SYS$OUTPUT/SYS$ERROR to $QIO and RMS.
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
/* The SYS$OUTPUT stream itself, for the HELP renderer, which writes a
 * caller-chosen stream (a libc-drop: help_render/help_interactive). */
FILE  *dcl_out_stream(void);

/* SYS$ERROR */
int    dcl_err_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    dcl_err_vprintf(const char *fmt, va_list ap) __attribute__((format(printf, 1, 0)));
int    dcl_err_puts(const char *s);
int    dcl_err_fputs(const char *s);
int    dcl_err_putc(int c);
size_t dcl_err_write(const void *buf, size_t len);
int    dcl_err_flush(void);

#endif /* DCL_OUT_H */
