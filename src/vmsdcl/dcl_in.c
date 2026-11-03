/*
 * dcl_in.c - DCL's one input source (rd vms-4d27, vms-67a, vms-91fe,
 * vms-ee7). See dcl/dcl_in.h; the output twin is dcl_out.c.
 *
 * WHAT VMS DOES. DCL reads its command lines -- and the data of READ
 * SYS$INPUT and INQUIRE -- as records of the process-permanent file
 * SYS$INPUT: from a terminal a $QIO IO$_READPROMPT on the terminal channel;
 * from a mailbox (LIB$SPAWN INPUT=, a parent driving a subprocess) a $QIO
 * IO$_READVBLK, one message a record; from a disk file (LIB$SPAWN's command
 * file, RUN/DETACHED /INPUT=, a batch job) an RMS $GET per record (VSI
 * OpenVMS DCL Dictionary, SYS$INPUT; I/O User's Reference, mailbox driver).
 *
 * WHAT THIS DOES. dcl_in_bind_ppf() looks at what SYS$INPUT names, once, at
 * startup. A mailbox is $ASSIGNed on DCL's own (main) thread and each record
 * is a $QIOW IO$_READVBLK on that channel; a disk file is RMS $OPENed and
 * each record is a $GET. The terminal is not read here: its reads go to the
 * executive's terminal driver (dcl_terminal.c, IO$_READPROMPT with the
 * type-ahead, echo, ^Y/^C and ^Z it implements), which every caller tries
 * first. This replaces dcl_mbx.c's reader threads, which pumped the mailbox
 * or the file into a pipe on fd 0 for fgets() (rd vms-786, vms-003b).
 *
 * THE ONE LIBC PATH. A process whose SYS$INPUT is neither -- DCL with no
 * executive (the host test harness), a terminal with no executive driver
 * behind it, a pipe a PIPE segment feeds -- reads its substrate stdin exactly
 * as before. That fallback is the block marked below and is the only libc
 * input in DCL (tests/dcl/test_no_libc_io.sh pins it). It is also what is
 * read while a command procedure's in-stream data block stands on fd 0 for
 * CREATE or an image (dcl_sysinput_install, dcl_cmd_process.c; rd vms-1a9):
 * dcl_in_substrate() brackets that window.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "iodef.h"
#include "ssdef.h"
#include "rms_textfile.h"
#include "dcl/dcl_out.h"
#include "dcl/dcl_in.h"

#define DCL_IN_MSG 4096          /* one mailbox message: the executive's cap */

static int             g_route;              /* DCL_IN_ROUTE_* */
static char            g_spec[256];          /* the device or file bound */
static uint16_t        g_chan;               /* mailbox: DCL's channel */
static rms_textfile_t *g_file;               /* file: the RMS stream */
static char            g_msg[DCL_IN_MSG];    /* mailbox: the message being read */
static size_t          g_msg_len, g_msg_off;
static int             g_msg_more;           /* g_msg holds lines not yet read */
static int             g_eof;                /* the route's last read hit the end */
static int             g_substrate;          /* reads go to the substrate fd 0 */

/* ================================================================
 * THE STDIO FALLBACK -- the only libc input in DCL.
 * ================================================================ */
static int fb_get(char *buf, size_t bufsz)
{
    if (!fgets(buf, (int)bufsz, stdin))
        return DCL_IN_EOF;
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n')
        buf[--n] = '\0';
    return (int)n;
}

/* ^Z on a substrate terminal ends one read, not the stream */
static int fb_rearm(void)
{
    if (feof(stdin) && !ferror(stdin) && isatty(STDIN_FILENO)) {
        clearerr(stdin);
        return 1;
    }
    return 0;
}

static int fb_at_eof(void)      { return feof(stdin); }
static void fb_clear_eof(void)  { clearerr(stdin); }
/* ================================================================ */

static int substrate(void)
{
    return g_route == DCL_IN_ROUTE_NONE || g_substrate;
}

static void make_desc(struct dsc$descriptor_s *d, const char *s)
{
    d->dsc$w_length  = (uint16_t)strlen(s);
    d->dsc$b_dtype   = DSC$K_DTYPE_T;
    d->dsc$b_class   = DSC$K_CLASS_S;
    d->dsc$a_pointer = (char *)s;
}

/* One level of logical-name translation; 0 if `name` is not a logical name. */
static int translate_once(const char *name, char *out, size_t outsz)
{
    struct dsc$descriptor_s namdsc;
    struct item_list_3 itmlst[2];
    uint16_t rl = 0;

    make_desc(&namdsc, name);
    memset(itmlst, 0, sizeof itmlst);
    itmlst[0].buflen    = (uint16_t)(outsz - 1);
    itmlst[0].item_code = LNM$_STRING;
    itmlst[0].bufaddr   = out;
    itmlst[0].retlen    = &rl;
    out[0] = '\0';
    if (!(sys$trnlnm(NULL, NULL, &namdsc, NULL, itmlst) & 1) || rl == 0)
        return 0;
    if (rl >= outsz)
        rl = (uint16_t)(outsz - 1);
    out[rl] = '\0';
    return 1;
}

/* A VMS mailbox device name: MBA<unit>: */
static int is_mbx_device(const char *s)
{
    size_t n = strlen(s);
    if (n < 5 || toupper((unsigned char)s[0]) != 'M' ||
        toupper((unsigned char)s[1]) != 'B' || toupper((unsigned char)s[2]) != 'A' ||
        s[n - 1] != ':')
        return 0;
    for (size_t i = 3; i < n - 1; i++)
        if (!isdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

int dcl_in_resolve_mailbox(const char *name, char *out, size_t outsz)
{
    char cur[256], equiv[256];

    snprintf(cur, sizeof cur, "%s", name);
    for (int depth = 0; depth < 10; depth++) {
        if (is_mbx_device(cur)) {
            snprintf(out, outsz, "%s", cur);
            return 1;
        }
        if (!translate_once(cur, equiv, sizeof equiv) || strcmp(equiv, cur) == 0)
            return 0;
        snprintf(cur, sizeof cur, "%s", equiv);
    }
    return 0;
}

/* The final equivalence of `name` (up to ten levels); `name` itself when it
 * is not a logical name. Returns 1 when `name` was a logical name. */
static int resolve_final(const char *name, char *out, size_t outsz)
{
    char cur[256], equiv[256];
    int any = 0;

    snprintf(cur, sizeof cur, "%s", name);
    for (int depth = 0; depth < 10; depth++) {
        if (!translate_once(cur, equiv, sizeof equiv) || strcmp(equiv, cur) == 0)
            break;
        snprintf(cur, sizeof cur, "%s", equiv);
        any = 1;
    }
    snprintf(out, outsz, "%s", cur);
    return any;
}

/* A FILE specification -- a device field followed by a directory or file name
 * -- rather than a bare device such as a terminal or the null device. */
static int names_a_file(const char *spec)
{
    const char *c = strrchr(spec, ':');
    return c && c[1] != '\0';
}

int dcl_in_bind_ppf(void)
{
    char spec[256];

    if (g_route != DCL_IN_ROUTE_NONE)
        return g_route;

    /* A MAILBOX: commands arrive as its messages. */
    if (dcl_in_resolve_mailbox("SYS$INPUT", spec, sizeof spec)) {
        struct dsc$descriptor_s devdsc;
        uint16_t chan = 0;
        make_desc(&devdsc, spec);
        if (sys$assign(&devdsc, &chan, 0, NULL, 0) & 1) {
            g_chan = chan;
            snprintf(g_spec, sizeof g_spec, "%s", spec);
            g_route = DCL_IN_ROUTE_MBX;
            return g_route;
        }
    }

    /* A DISK FILE (rd vms-003b): a process $CREPRC'd with a VMS file as its
     * input has SYS$INPUT naming that file, and DCL reads its commands from
     * it through RMS. A substrate terminal on fd 0 is the terminal's. */
    if (!isatty(STDIN_FILENO) && resolve_final("SYS$INPUT", spec, sizeof spec) &&
        names_a_file(spec) && (g_file = rms_textfile_open(spec)) != NULL) {
        snprintf(g_spec, sizeof g_spec, "%s", spec);
        g_route = DCL_IN_ROUTE_FILE;
    }
    return g_route;
}

int dcl_in_route(void)
{
    return g_route;
}

int dcl_in_is_ppf(const char *equiv)
{
    char fin[256];

    if (g_route == DCL_IN_ROUTE_NONE || !equiv || !equiv[0])
        return 0;
    if (strcasecmp(equiv, g_spec) == 0)
        return 1;
    if (g_route == DCL_IN_ROUTE_MBX)
        return dcl_in_resolve_mailbox(equiv, fin, sizeof fin) &&
               strcasecmp(fin, g_spec) == 0;
    resolve_final(equiv, fin, sizeof fin);
    return strcasecmp(fin, g_spec) == 0;
}

/* The next line of the mailbox message being read; a new message ($QIOW
 * IO$_READVBLK, waiting for one) when it is used up. A message is a record;
 * one a writer sent holding several lines gives them one by one. */
static int mbx_get(char *buf, size_t bufsz)
{
    if (!g_msg_more) {
        uint16_t iosb[4] = { 0, 0, 0, 0 };
        uint32_t st = sys$qiow(0, g_chan, IO$_READVBLK, iosb, NULL, 0,
                               g_msg, sizeof g_msg, 0, 0, 0, 0);
        if (!(st & 1) || !(iosb[0] & 1))
            return DCL_IN_EOF;               /* SS$_ENDOFFILE, or no mailbox */
        size_t n = iosb[1];
        if (n > sizeof g_msg)
            n = sizeof g_msg;
        /* a writer that sent the record with its C string's NUL */
        if (n > 0 && g_msg[n - 1] == '\0')
            n--;
        if (n > 0 && g_msg[n - 1] == '\n')
            n--;                             /* the record's own terminator */
        g_msg_len = n;
        g_msg_off = 0;
        g_msg_more = 1;
    }
    const char *p = g_msg + g_msg_off;
    size_t left = g_msg_len - g_msg_off;
    const char *nl = memchr(p, '\n', left);
    size_t ll = nl ? (size_t)(nl - p) : left;
    size_t take = ll < bufsz - 1 ? ll : bufsz - 1;
    memcpy(buf, p, take);
    buf[take] = '\0';
    if (nl)
        g_msg_off += ll + 1;
    else
        g_msg_more = 0;
    return (int)take;
}

static int file_get(char *buf, size_t bufsz)
{
    int too_long = 0;
    if (!g_file || !rms_textfile_getline(g_file, buf, bufsz, &too_long))
        return DCL_IN_EOF;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return (int)n;
}

int dcl_in_get(const char *prompt, char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return DCL_IN_EOF;
    buf[0] = '\0';
    if (prompt && prompt[0]) {
        dcl_out_fputs(prompt);
        dcl_out_flush();
    }
    if (substrate())
        return fb_get(buf, bufsz);
    int n = g_route == DCL_IN_ROUTE_MBX ? mbx_get(buf, bufsz) : file_get(buf, bufsz);
    g_eof = n < 0;
    return n;
}

int dcl_in_rearm(void)
{
    return substrate() ? fb_rearm() : 0;
}

int dcl_in_at_eof(void)
{
    return substrate() ? fb_at_eof() : g_eof;
}

void dcl_in_clear_eof(void)
{
    if (substrate())
        fb_clear_eof();
    else
        g_eof = 0;
}

void dcl_in_substrate(int on)
{
    g_substrate = on;
}

void dcl_in_shutdown(void)
{
    if (g_route == DCL_IN_ROUTE_MBX && g_chan)
        (void)sys$dassgn(g_chan);
    if (g_file)
        rms_textfile_close(g_file);
    g_chan = 0;
    g_file = NULL;
    g_msg_more = 0;
    g_route = DCL_IN_ROUTE_NONE;
}
