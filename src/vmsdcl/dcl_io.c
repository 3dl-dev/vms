/*
 * dcl_io.c - DCL I/O Operations
 *
 * Where SYS$OUTPUT/SYS$ERROR go, and VMS-style error message formatting.
 */

#include "dcl/dcl_out.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/vms_messages.h"
#include "dcl/dcl_cmd.h"
#include "ssdef.h"
#include "dcl/terminal.h"
#include "dcl/dcl_mbx.h"
#include "dcl/dcl_rms.h"
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>


/*
 * DCL's input -- its command lines and READ SYS$INPUT / INQUIRE data -- is
 * read from SYS$INPUT by dcl_in.c (mailbox by $QIO, file by RMS) and the
 * terminal driver (dcl_terminal.c); there is no stdio read here.
 */

/*
 * Write a line to SYS$OUTPUT (stdout).
 */
void dcl_write_output(struct dcl_context *ctx, const char *text)
{
    (void)ctx;
    if (text) {
        dcl_out_fputs(text);
    }
}

/*
 * Write a line to SYS$ERROR (stderr).
 */
void dcl_write_error(struct dcl_context *ctx, const char *text)
{
    (void)ctx;
    if (text) {
        dcl_err_fputs(text);
    }
}

/*
 * Format a VMS-style error/status message.
 *
 * VMS format: %FACILITY-S-IDENT, text
 * Where S is severity letter: S(uccess), I(nfo), W(arning), E(rror), F(atal)
 */
void dcl_format_error(const char *facility, int severity, const char *ident,
                      const char *text, char *buf, size_t bufsize)
{
    char sev_char = vms_severity_char(severity);

    snprintf(buf, bufsize, "%%%s-%c-%s, %s",
             facility ? facility : "DCL",
             sev_char,
             ident ? ident : "ERROR",
             text ? text : "");
}

/*
 * Print a VMS-style error message to stderr.
 */
void dcl_error(const char *facility, int severity, const char *ident,
               const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    char msg[1280];
    dcl_format_error(facility, severity, ident, text, msg, sizeof(msg));
    dcl_err_printf("%s\n", msg);
}

/*
 * A continuation line of the message just shown ("-RMS-E-FNF, file not
 * found"), through the same message sink as dcl_error.
 */
void dcl_error_cont(const char *facility, int severity, const char *ident,
                    const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    char msg[1280];
    dcl_format_error(facility, severity, ident, text, msg, sizeof(msg));
    msg[0] = '-';
    dcl_err_printf("%s\n", msg);
}

/*
 * The one honest refusal for an operation OVMX does not perform (vms-916):
 * the REAL condition SS$_UNSUPPORTED, %X00000E4A, displayed with VMS's own
 * text (OpenVMS Alpha V8.4 $GETMSG: "%SYSTEM-W-UNSUPPORTED, unsupported
 * operation or function", docs/oracle/messages/alpha84-msgcat.txt, shown at
 * the value's own E severity). It replaces a family of invented
 * %DCL-W-NOTIMPL / %SET-W-NOTIMPL / %PCSI-W-NOTIMPL messages: VMS has no
 * NOTIMPL ident in any of those facilities. Callers return its value and
 * change no state.
 */
uint32_t dcl_unsupported(void)
{
    dcl_error("SYSTEM", 2, "UNSUPPORTED", "unsupported operation or function");
    return SS$_UNSUPPORTED;
}

/* ================================================================
 * SYS$OUTPUT AND SYS$ERROR: WHERE DCL'S RECORDS GO (rd vms-1b6a)
 *
 * The process-permanent files name the destination: the terminal (the
 * default, "TT:", or this process's own terminal device), a mailbox (MBAn:,
 * a parent driving this DCL), or a disk file (a detached or batch-style
 * process's log). DCL's sink (dcl_out.c) sends each record there: the
 * terminal through its driver ($QIO IO$_WRITEVBLK with carriage control), the
 * mailbox through $QIO IO$_WRITEVBLK (dcl_mbx_put_record), the file through
 * RMS $PUT. What is routed is only what really is that destination: output
 * the substrate already carries somewhere else -- a pipe a PIPE segment or a
 * test harness reads -- keeps the process stream (dcl_out.c's fallback).
 * ================================================================ */
enum { PPF_TERM = 1, PPF_MBX, PPF_FILE };

static int route_tt(void *arg, const char *rec, size_t n, int open)
{
    (void)arg;
    return dcl_tt_put_text(rec, n, open);
}

/* a mailbox message is one line, its '\n' with it, as the image-output relay
 * delivers each line an image writes (dcl_mbx.c line_relay_feed) */
static int route_mbx(void *arg, const char *rec, size_t n, int open)
{
    char msg[4096 + 1];
    (void)arg;
    if (n > sizeof msg - 1)
        n = sizeof msg - 1;
    memcpy(msg, rec, n);
    if (!open)
        msg[n++] = '\n';
    return dcl_mbx_put_record(msg, n);
}

struct ppf_file {
    struct dcl_rms_writer *w;
    char spec[256];
    int refs;
};
static struct ppf_file g_ppf_file[2];
static struct ppf_file *g_route_file[2];   /* the file each stream is routed to */

static int route_file(void *arg, const char *rec, size_t n, int open)
{
    (void)open;
    return dcl_rms_write_record(((struct ppf_file *)arg)->w, rec, n);
}

static void route_file_close(void *arg)
{
    struct ppf_file *f = arg;
    if (--f->refs <= 0 && f->w) {
        (void)dcl_rms_write_close(f->w);
        f->w = NULL;
        f->spec[0] = '\0';
        f->refs = 0;
    }
}

/* What `lognam` names: PPF_TERM, PPF_MBX or PPF_FILE (equiv: the name). */
static int ppf_kind(const char *lognam, char *equiv, size_t sz)
{
    equiv[0] = '\0';
    if (dcl_translate_logical(lognam, equiv, sz) != 0 || !equiv[0] ||
            equiv[0] == 0x1B)
        return PPF_TERM;                      /* undefined, or the PPF itself */
    const char *d = equiv + (equiv[0] == '_');
    if (strncasecmp(d, "MBA", 3) == 0) {
        d += 3;
        if (*d >= '0' && *d <= '9') {
            while (*d >= '0' && *d <= '9')
                d++;
            if (*d == '\0' || (*d == ':' && d[1] == '\0'))
                return PPF_MBX;
        }
    }
    return dcl_logical_is_terminal(equiv) ? PPF_TERM : PPF_FILE;
}

/* fd is the null device: nothing in the substrate carries this stream, so a
 * file SYS$OUTPUT is the only place it goes (as for an image, sysoutput_rms.c) */
static int is_null_device(int fd)
{
    struct stat st, nl;
    return fstat(fd, &st) == 0 && stat("/dev/null", &nl) == 0 &&
           S_ISCHR(st.st_mode) && st.st_rdev == nl.st_rdev;
}

void dcl_out_bind_ppf(void)
{
    static const char *const names[2] = { "SYS$OUTPUT", "SYS$ERROR" };
    char equiv[2][256];
    int kind[2];

    for (int which = 0; which < 2; which++) {
        int fd = which ? STDERR_FILENO : STDOUT_FILENO;
        kind[which] = ppf_kind(names[which], equiv[which], sizeof equiv[which]);
        g_route_file[which] = NULL;

        if (kind[which] == PPF_TERM) {
            if (isatty(fd) && dcl_tt_output_ready())
                dcl_out_route(which, route_tt, NULL, DCL_OUT_ROUTE_OPEN, NULL);
            else if (dcl_out_routed(which))
                dcl_out_route(which, NULL, NULL, 0, NULL);
        } else if (kind[which] == PPF_MBX) {
            /* SYS$ERROR goes there only when it is SYS$OUTPUT's mailbox */
            int ok = dcl_mbx_output_is_mailbox() &&
                     (which == 0 || (kind[0] == PPF_MBX &&
                                     strcasecmp(equiv[0], equiv[1]) == 0));
            if (ok)
                dcl_out_route(which, route_mbx, NULL, DCL_OUT_ROUTE_OPEN, NULL);
            else if (dcl_out_routed(which))
                dcl_out_route(which, NULL, NULL, 0, NULL);
        } else if (is_null_device(fd)) {
            struct ppf_file *f = &g_ppf_file[which];
            if (which == 1 && kind[0] == PPF_FILE && g_ppf_file[0].w &&
                strcasecmp(g_ppf_file[0].spec, equiv[1]) == 0)
                f = &g_ppf_file[0];           /* SYS$ERROR is SYS$OUTPUT's file */
            if (dcl_out_routed(which))
                dcl_out_route(which, NULL, NULL, 0, NULL);
            if (!f->w) {
                uint32_t st = 0;
                f->w = dcl_rms_write_append_shared(dcl_get_context(), equiv[which], &st);
                snprintf(f->spec, sizeof f->spec, "%s", equiv[which]);
            }
            if (f->w) {
                f->refs++;
                dcl_out_route(which, route_file, f, 0, route_file_close);
                g_route_file[which] = f;
            }
        }
    }
}

/* The file SYS$OUTPUT (which 0) or SYS$ERROR (1) is routed to, or NULL. A
 * subprocess DCL creates (a PIPE segment) is given it as its own SYS$OUTPUT /
 * SYS$ERROR -- the process-permanent file it inherits (rd vms-380f). */
const char *dcl_ppf_file_spec(int which)
{
    const struct ppf_file *f = g_route_file[which ? 1 : 0];
    return (f && f->w && dcl_out_routed(which ? 1 : 0)) ? f->spec : NULL;
}

/* Is SYS$OUTPUT (which 0) or SYS$ERROR (1) routed to the file `spec`? Then a
 * WRITE to it goes through that route, the file DCL already holds open. */
int dcl_ppf_routed_to_file(int which, const char *spec)
{
    const struct ppf_file *f = g_route_file[which ? 1 : 0];
    return f && f->w && dcl_out_routed(which) && strcasecmp(f->spec, spec) == 0;
}
