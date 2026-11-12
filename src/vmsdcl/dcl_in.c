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
 * input in DCL (tests/dcl/test_no_libc_io.sh pins it). A command procedure's
 * in-stream data block is never read from it: that block is a mailbox (dcl_in_block_open, rd vms-07ae).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "iodef.h"
#include "dvidef.h"
#include "psldef.h"
#include "ssdef.h"
#include "rms_textfile.h"
#include "vms_kif.h"
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
static char           *g_unget;              /* a record handed back (dcl_in_unget) */

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
    return g_route == DCL_IN_ROUTE_NONE;
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

static int mem_get(char *buf, size_t bufsz);   /* a data block DCL reads */

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
    if (g_unget) {
        snprintf(buf, bufsz, "%s", g_unget);
        free(g_unget);
        g_unget = NULL;
        g_eof = 0;
        return (int)strlen(buf);
    }
    int n = g_route == DCL_IN_ROUTE_MBX ? mbx_get(buf, bufsz)
          : g_route == DCL_IN_ROUTE_MEM ? mem_get(buf, bufsz) : file_get(buf, bufsz);
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

void dcl_in_unget(const char *rec)
{
    free(g_unget);
    g_unget = rec ? strdup(rec) : NULL;
}

/* ================================================================
 * A PROCEDURE'S DATA BLOCK AS SYS$INPUT (rd vms-07ae).
 *
 * On VMS an image RUN from a command procedure reads the procedure's data
 * lines as its SYS$INPUT, record by record, however many there are. OVMX runs
 * the image in a task of its own, so DCL hands it the block through a
 * temporary mailbox, then end of file. SYS$INPUT is defined to the mailbox at
 * user mode -- an image's name, run down by the executive when the image exits
 * (DEFINE/USER) -- so the C RTL of every image class binds descriptor 0 to it.
 *
 * FLOW CONTROL. The mailbox holds one buffer quota of records. DCL writes what
 * fits at once (IO$M_NORSWAIT, before the image starts: a small block needs
 * nothing more). The rest goes in from a feeder thread of DCL's process whose
 * writes wait for room in the mailbox the way any mailbox writer does
 * (resource wait, VSI I/O User's Reference, Mailbox Driver): each record the
 * image reads makes room for the next, so a block of any size reaches it whole.
 * The thread writes on the executive channel (vms_kif_mbx_*), as dcl_mbx.c's
 * bridge threads do, because the PCB's channel table is the main thread's.
 * When the image is done, DCL reads off whatever it left unread (VMS's DCL
 * skips those lines too), which lets the feeder finish, and ends the mailbox
 * with end of file again for an image that is only stopped (CTRL/Y).
 * ================================================================ */
#define DCL_IN_BLOCK_QUOTA 4096u     /* the executive's mailbox buffer-quota cap */

static const struct dsc$descriptor_s blk_ptab =
    { 17, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"LNM$PROCESS_TABLE" };
static const struct dsc$descriptor_s blk_lnm =
    { 9, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"SYS$INPUT" };

/* DCL's own input, set aside while the block is SYS$INPUT */
static struct {
    int       active, route, eof, msg_more;
    uint16_t  chan;
    size_t    msg_len, msg_off;
    char     *unget;
    char      msg[DCL_IN_MSG];
} g_saved;

static uint32_t blk_status(uint32_t st, const uint16_t *iosb)
{
    return (st & 1) ? iosb[0] : st;
}

static char  *g_blk;                 /* DCL's own read of a data block */
static size_t g_blk_len, g_blk_off;

static void save_and_route(int route, uint16_t chan)
{
    g_saved.active   = 1;
    g_saved.route    = g_route;
    g_saved.chan     = g_chan;
    g_saved.eof      = g_eof;
    g_saved.msg_more = g_msg_more;
    g_saved.msg_len  = g_msg_len;
    g_saved.msg_off  = g_msg_off;
    g_saved.unget    = g_unget;
    if (g_msg_more)
        memcpy(g_saved.msg, g_msg, g_msg_len);
    g_route = route;
    g_chan = chan;
    g_eof = 0;
    g_msg_more = 0;
    g_unget = NULL;
}

static int mem_get(char *buf, size_t bufsz)
{
    if (!g_blk || g_blk_off >= g_blk_len)
        return DCL_IN_EOF;
    const char *p = g_blk + g_blk_off;
    size_t left = g_blk_len - g_blk_off;
    const char *nl = memchr(p, '\n', left);
    size_t ll = nl ? (size_t)(nl - p) : left;
    size_t take = ll < bufsz - 1 ? ll : bufsz - 1;
    memcpy(buf, p, take);
    buf[take] = '\0';
    g_blk_off += nl ? ll + 1 : ll;
    return (int)take;
}

/* the feeder: the records that did not fit at once, written as the image
 * makes room */
static struct {
    int        up;           /* the thread runs, or ran and is not joined */
    pthread_t  th;
    uint32_t   xchan;        /* the mailbox's executive channel */
    char      *blk;          /* the rest of the block */
    size_t     len, off;
    int        stop;         /* DCL is done with the image: end the block now */
    int        done;         /* the feeder wrote the end of file */
} g_feed;

static size_t rec_len(const char *p, size_t left)
{
    const char *nl = memchr(p, '\n', left);
    return nl ? (size_t)(nl - p) + 1 : left;
}

static void *feed_main(void *arg)
{
    (void)arg;
    while (g_feed.off < g_feed.len && !__atomic_load_n(&g_feed.stop, __ATOMIC_ACQUIRE)) {
        size_t rl = rec_len(g_feed.blk + g_feed.off, g_feed.len - g_feed.off);
        /* a write that waits for room: the image's reads make it */
        if (!(vms_kif_mbx_write_ex(g_feed.xchan, g_feed.blk + g_feed.off,
                                   (uint32_t)rl, 0) & 1))
            break;
        g_feed.off += rl;
    }
    (void)vms_kif_mbx_write_eof(g_feed.xchan);
    __atomic_store_n(&g_feed.done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* The image is done with the block: read off what it left so the feeder can
 * finish, through the end of file it writes; then end the mailbox again. */
static void feed_finish(void)
{
    if (!g_feed.up)
        return;
    __atomic_store_n(&g_feed.stop, 1, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&g_feed.done, __ATOMIC_ACQUIRE)) {
        static char drain[DCL_IN_MSG];
        uint32_t got = 0;
        /* only DCL holds the mailbox's read side now: the feeder's end of
         * file arrives, and the read that takes it fails SS$_ENDOFFILE */
        while (vms_kif_mbx_read(g_feed.xchan, drain, sizeof drain, &got, 0) & 1)
            ;
        (void)vms_kif_mbx_write_eof(g_feed.xchan);
    }
    (void)pthread_join(g_feed.th, NULL);
    (void)vms_kif_dassgn(g_feed.xchan);
    free(g_feed.blk);
    memset(&g_feed, 0, sizeof g_feed);
}

uint32_t dcl_in_block_open(const char *blk, size_t len, int image, uint16_t *chan_out)
{
    uint16_t chan = 0, iosb[4] = { 0, 0, 0, 0 };
    uint32_t st;

    *chan_out = 0;
    if (g_saved.active)
        return SS$_BADPARAM;
    if (!image) {
        /* DCL itself reads the block (CREATE): the procedure's own lines,
         * from the memory DCL gathered them into -- no device between */
        g_blk = malloc(len + 1);
        if (!g_blk)
            return SS$_INSFMEM;
        memcpy(g_blk, blk, len);
        g_blk_len = len;
        g_blk_off = 0;
        save_and_route(DCL_IN_ROUTE_MEM, 0);
        return SS$_NORMAL;
    }
    st = sys$crembx(0, &chan, DCL_IN_MSG, DCL_IN_BLOCK_QUOTA, 0, 0, NULL, 0);
    if (!(st & 1))
        return st;
    /* what fits now, whole records (IO$M_NORSWAIT); the feeder takes the
     * rest once the name is in place */
    size_t off = 0;
    while (off < len) {
        size_t rl = rec_len(blk + off, len - off);
        st = blk_status(sys$qiow(0, chan, IO$_WRITEVBLK | IO$M_NOW | IO$M_NORSWAIT, iosb,
                                 NULL, 0, (void *)(blk + off), rl, 0, 0, 0, 0), iosb);
        if (st == SS$_MBFULL)
            break;
        if (!(st & 1))
            goto fail;
        off += rl;
    }
    if (off == len) {
        st = blk_status(sys$qiow(0, chan, IO$_WRITEOF | IO$M_NOW | IO$M_NORSWAIT, iosb,
                                 NULL, 0, 0, 0, 0, 0, 0, 0), iosb);
        if (!(st & 1))
            goto fail;
    }

    char dev[64];
    uint16_t dl = 0;
    struct item_list_3 il[2];
    memset(il, 0, sizeof il);
    il[0].buflen    = sizeof dev - 1;
    il[0].item_code = DVI$_DEVNAM;
    il[0].bufaddr   = dev;
    il[0].retlen    = &dl;
    st = sys$getdviw(0, chan, NULL, il, NULL, NULL, 0, 0);
    if (!(st & 1) || dl == 0) {
        st = (st & 1) ? SS$_NOSUCHDEV : st;
        goto fail;
    }
    memset(il, 0, sizeof il);
    il[0].buflen    = dl;
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr   = dev;
    uint8_t um = PSL$C_USER;
    if (off < len) {
        dev[dl] = '\0';
        uint32_t xchan = 0;
        st = vms_kif_mbx_assign(dev, &xchan);
        if (!(st & 1))
            goto fail;
        g_feed.blk = malloc(len - off);
        if (!g_feed.blk) {
            (void)vms_kif_dassgn(xchan);
            st = SS$_INSFMEM;
            goto fail;
        }
        memcpy(g_feed.blk, blk + off, len - off);
        g_feed.len = len - off;
        g_feed.off = 0;
        g_feed.xchan = xchan;
        g_feed.stop = g_feed.done = 0;
        if (pthread_create(&g_feed.th, NULL, feed_main, NULL) != 0) {
            (void)vms_kif_dassgn(xchan);
            free(g_feed.blk);
            memset(&g_feed, 0, sizeof g_feed);
            st = SS$_INSFMEM;
            goto fail;
        }
        g_feed.up = 1;
    }
    st = sys$crelnm(NULL, &blk_ptab, &blk_lnm, &um, il);
    if (!(st & 1)) {
        feed_finish();
        goto fail;
    }

    save_and_route(DCL_IN_ROUTE_MBX, chan);
    *chan_out = chan;
    return SS$_NORMAL;

fail:
    (void)sys$dassgn(chan);
    return st;
}

void dcl_in_block_close(uint16_t chan)
{
    uint8_t um = PSL$C_USER;

    if (!g_saved.active)
        return;
    /* the image's exit ran the name down already; an image never entered
     * (or one DCL ran in its own task) leaves it to DCL */
    if (g_route == DCL_IN_ROUTE_MBX) {
        feed_finish();
        (void)sys$dellnm(&blk_ptab, &blk_lnm, &um);
        (void)sys$dassgn(chan);     /* the temporary mailbox goes with it */
    }
    free(g_blk);
    g_blk = NULL;
    free(g_unget);
    g_route    = g_saved.route;
    g_chan     = g_saved.chan;
    g_eof      = g_saved.eof;
    g_msg_more = g_saved.msg_more;
    g_msg_len  = g_saved.msg_len;
    g_msg_off  = g_saved.msg_off;
    g_unget    = g_saved.unget;
    if (g_msg_more)
        memcpy(g_msg, g_saved.msg, g_msg_len);
    g_saved.active = 0;
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
