/*
 * crtl_devfd.c - the C RTL's terminal and mailbox descriptors over VMS I/O
 * channels (rd vms-a66e, the device half of the C RTL file layer).
 *
 * On OpenVMS the C RTL is a client of the VMS I/O system for every descriptor
 * that is not a disk file:
 *
 *   a TERMINAL   stdin/stdout/stderr of a program run from DCL are SYS$INPUT /
 *                SYS$OUTPUT / SYS$ERROR, the process-permanent files on the
 *                process's terminal. The C RTL reads and writes them as $QIO
 *                IO$_READVBLK / IO$_WRITEVBLK on a channel $ASSIGNed to that
 *                terminal; the terminal driver does the line editing and echo.
 *   a MAILBOX    pipe() is a temporary mailbox ($CREMBX) with one channel for
 *                each end (VSI C RTL Reference, pipe: "a mailbox ... the
 *                parent and the child communicate through"); a write is one
 *                IO$_WRITEVBLK message, a read one IO$_READVBLK, close() of the
 *                last write end an IO$_WRITEOF, which the reader sees as end of
 *                file. A child created by exec*() with a pipe end as its
 *                stdin/stdout gets the mailbox as its SYS$INPUT / SYS$OUTPUT.
 *
 * HOW. src/vmsrms/crtl_rms_fd.c owns the hook in the musl system-call funnel;
 * it offers every call to this layer first (ovmx_crtl_devfd_call). A
 * descriptor of this layer is a kernel descriptor NUMBER only: a placeholder
 * (O_PATH "/", close-on-exec) reserves it, exactly as an RMS file's, so the
 * three kinds -- RMS files, VMS devices, and the substrate's own descriptors --
 * share one number space and dup2() between them works. No byte of a terminal
 * or mailbox descriptor's I/O passes through a Linux descriptor: read, write,
 * close, dup, fcntl, ioctl and fstat on one are served here from the channel.
 *
 * THE STANDARD STREAMS. At image start (ovmx_crtl_devfd_bind_stdio, from the
 * C RTL's decc$main) each of SYS$INPUT, SYS$OUTPUT and SYS$ERROR is translated
 * (a process-permanent file's ESC NUL IFI header dropped) to its device, and
 * $GETDVI decides what it is. A terminal or a mailbox becomes descriptor 0, 1
 * or 2 over a channel $ASSIGNed to it. A disk file (DEFINE/USER SYS$OUTPUT
 * file) is left as it is, and so is a stream the substrate has redirected
 * underneath the VMS name -- a terminal stream whose line is no longer a
 * terminal (a DCL PIPE segment, still a host pipe), or a mailbox stream the
 * creator pumps from a host pipe (LIB$SPAWN's DCL). Those
 * are tracked as their own items; this layer never sends a stream's bytes to a
 * device other than the one the stream already reaches.
 *
 * Single-threaded, as the rest of DECC$SHR (non-TLS producer).
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>              /* TCGETS / TIOCGWINSZ: the arch's numbers */
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>


#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif
#ifndef O_PATH
#define O_PATH 010000000
#endif

/* ------------------------------------------------- the VMS services used -- */

/* The system services, declared here (as crtl_process.c does) rather than
 * through <starlet.h>, whose types collide with the C RTL's own headers. All of
 * them are LIBVMS$SHR's (the RMS-backed DECC$SHR links --use LIBVMS$SHR). */
struct devfd_dsc {
    unsigned short len;
    unsigned char  dtype, cls;
    char          *ptr;
};
struct devfd_ile3 {
    unsigned short buflen, code;
    void          *buf;
    unsigned short *retlen;
};
/* A statically linked C image (OVMX_CRTLFD_STATIC: the x86_64 test corpus)
 * takes these WEAK: one that calls no system service of its own does not pull
 * the whole of LIBVMS into its image for them, and has no device layer (its
 * descriptors stay as the image's start-up found them); one that links LIBVMS
 * (test_syssvc_crtl_devfd) has it all. DECC$SHR always imports them. */
#if defined(OVMX_CRTLFD_STATIC)
#define DEVFD_SVC __attribute__((__weak__))
#else
#define DEVFD_SVC
#endif
extern DEVFD_SVC unsigned int sys$assign(const struct devfd_dsc *devnam, unsigned short *chan,
                               unsigned int acmode, const struct devfd_dsc *mbxnam,
                               unsigned int flags);
extern DEVFD_SVC unsigned int sys$dassgn(unsigned short chan);
extern DEVFD_SVC unsigned int sys$crembx(int prmflg, unsigned short *chan, unsigned int maxmsg,
                               unsigned int bufquo, unsigned int promsk,
                               unsigned int acmode, const struct devfd_dsc *lognam,
                               unsigned int flags, void *nullarg);
extern DEVFD_SVC unsigned int sys$qiow(unsigned int efn, unsigned short chan, unsigned int func,
                             void *iosb, void (*astadr)(unsigned int), unsigned int astprm,
                             void *p1, unsigned int p2, unsigned int p3,
                             uintptr_t p4, uintptr_t p5, unsigned int p6);
extern DEVFD_SVC unsigned int sys$getdviw(unsigned int efn, unsigned short chan,
                                const struct devfd_dsc *devnam, void *itmlst, void *iosb,
                                void (*astadr)(unsigned int), unsigned int astprm,
                                unsigned int nullarg);
extern DEVFD_SVC unsigned int sys$trnlnm(const unsigned int *attr, const struct devfd_dsc *tabnam,
                               const struct devfd_dsc *lognam, const unsigned char *acmode,
                               const struct devfd_ile3 *itmlst);

#define SS_NORMAL       1u
#define SS_ENDOFFILE    2160u
#define SS_MBFULL       2264u
#define SS_NOSUCHDEV    2312u
#define SS_IVCHAN       316u
#define SS_DEVOFFLINE   0x84u
#define SS_EXQUOTA      0x1Cu
#define SS_NOPRIV       0x24u
#define SS_BUFFEROVF    1537u
#define SS_NOLOGNAM     444u
#define IO_WRITEOF      40u
#define IO_WRITEVBLK    48u
#define IO_READVBLK     49u
#define IO_SENSEMODE    39u
#define IO_M_NOW        0x40u
#define DVI_DEVCLASS    0x04u
#define DVI_DEVBUFSIZ   0x08u
#define DVI_UNIT        0x0Cu
#define DVI_DEVNAM      0x20u
#define DC_TERM         66u
#define DC_MAILBOX      160u
#define LNM_STRING      2u

/* The pipe mailbox: DEC C's DECC$PIPE_BUFFER_SIZE default of 512 bytes for
 * the largest message; the quota lets a writer queue several before a reader
 * runs (a writer that finds it full waits for room). */
#define PIPE_MAXMSG     512u
#define PIPE_BUFQUO     8192u
#define TT_RDMAX        512u        /* one terminal read (a line) */

/* --------------------------------------------------------- the raw trap --- */

#if defined(__alpha__)
extern long long __vms_alpha_syscall_raw(long long, long long, long long,
                                         long long, long long, long long,
                                         long long);
#define devfd_raw __vms_alpha_syscall_raw
#else
extern long long __ovmx_syscall_raw(long long, long long, long long,
                                    long long, long long, long long,
                                    long long);
#define devfd_raw __ovmx_syscall_raw
#endif

static long long rawsys(long long n, long long a1, long long a2, long long a3)
{
    return devfd_raw(n, a1, a2, a3, 0, 0, 0);
}

/* ------------------------------------------------------------ the table --- */

#define DFD_MAX 1024

enum { DF_TT = 1, DF_MBX = 2 };

/* An open device description: shared by every descriptor dup()ed from it. */
struct dfile {
    int            refs;
    int            kind;            /* DF_TT / DF_MBX                          */
    int            oflags;          /* O_ACCMODE | O_NONBLOCK                  */
    int            stdbound;        /* a standard stream (process-permanent)    */
    int            handed;          /* a pipe write end given to a subprocess   */
    unsigned short chan;
    char           dev[64];         /* "_MBA12:", "_OPA0:"                      */
    /* read side: the message / line not yet consumed */
    char          *rbuf;
    unsigned int   rlen, roff;
    int            reof;            /* end of file seen                         */
    /* write side of a terminal: the line not yet ended by a newline */
    char          *obuf;
    unsigned int   olen;
};

static struct dfile *dfd[DFD_MAX];
static unsigned char dfd_cloexec[DFD_MAX];

static struct dfile *dget(long long fd)
{
    return (fd >= 0 && fd < DFD_MAX) ? dfd[fd] : NULL;
}

int ovmx_crtl_devfd_is(long long fd)
{
    return dget(fd) != NULL;
}

static int st_errno(unsigned int st)
{
    switch (st) {
    case SS_NOSUCHDEV: case SS_NOLOGNAM: return ENOENT;
    case SS_IVCHAN:                      return EBADF;
    case SS_EXQUOTA:                     return EMFILE;
    case SS_NOPRIV:                      return EACCES;
    case SS_DEVOFFLINE:                  return ENXIO;
    default:                             return EIO;
    }
}

static void dsc(struct devfd_dsc *d, const char *s, size_t n)
{
    d->len = (unsigned short)n;
    d->dtype = 14;                  /* DSC$K_DTYPE_T */
    d->cls = 1;                     /* DSC$K_CLASS_S */
    d->ptr = (char *)s;
}

/* $GETDVI one longword item on a channel (or, chan 0, a device name). */
static int dvi_long(unsigned short chan, const char *name, unsigned short code,
                    unsigned int *out)
{
    struct devfd_dsc d;
    struct devfd_ile3 il[2];
    unsigned short iosb[4] = { 0, 0, 0, 0 };
    unsigned int v = 0;
    memset(il, 0, sizeof il);
    il[0].buflen = sizeof v;
    il[0].code = code;
    il[0].buf = &v;
    if (name)
        dsc(&d, name, strlen(name));
    unsigned int st = sys$getdviw(0, chan, name ? &d : 0, il, iosb, 0, 0, 0);
    if (!(st & 1))
        return 0;
    *out = v;
    return 1;
}

static int dvi_devnam(unsigned short chan, char *out, size_t outsz)
{
    struct devfd_ile3 il[2];
    unsigned short iosb[4] = { 0, 0, 0, 0 }, rl = 0;
    char b[64];
    memset(il, 0, sizeof il);
    il[0].buflen = sizeof b - 1;
    il[0].code = DVI_DEVNAM;
    il[0].buf = b;
    il[0].retlen = &rl;
    if (!(sys$getdviw(0, chan, 0, il, iosb, 0, 0, 0) & 1) || !rl || rl >= outsz)
        return 0;
    memcpy(out, b, rl);
    out[rl] = '\0';
    return 1;
}

static struct dfile *dfile_new(int kind, unsigned short chan, int oflags)
{
    struct dfile *df = calloc(1, sizeof *df);
    if (!df)
        return NULL;
    df->kind = kind;
    df->chan = chan;
    df->oflags = oflags;
    if (!dvi_devnam(chan, df->dev, sizeof df->dev))
        df->dev[0] = '\0';
    return df;
}

/* Reserve a kernel descriptor number (placeholder, close-on-exec) for df. */
static long long dfd_bind(struct dfile *df, int cloexec)
{
    long long k = rawsys(SYS_openat, AT_FDCWD, (long long)(uintptr_t)"/",
                         O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (k < 0)
        return k;
    if (k >= DFD_MAX) {
        rawsys(SYS_close, k, 0, 0);
        return -EMFILE;
    }
    dfd[k] = df;
    dfd_cloexec[k] = (unsigned char)(cloexec != 0);
    df->refs++;
    return k;
}

static unsigned int mbx_write_eof(unsigned short chan)
{
    unsigned short iosb[4] = { 0, 0, 0, 0 };
    unsigned int st = sys$qiow(0, chan, IO_WRITEOF | IO_M_NOW, iosb, 0, 0, 0, 0, 0, 0, 0, 0);
    if ((st & 1) && !(iosb[0] & 1))
        st = iosb[0];
    if (st == SS_MBFULL) {                        /* full: wait for room */
        st = sys$qiow(0, chan, IO_WRITEOF, iosb, 0, 0, 0, 0, 0, 0, 0, 0);
        if ((st & 1) && !(iosb[0] & 1))
            st = iosb[0];
    }
    return st;
}

static void tt_flush_line(struct dfile *df, int record);

static void dfile_release(struct dfile *df)
{
    if (--df->refs > 0)
        return;
    if (df->kind == DF_TT)
        tt_flush_line(df, 1);
    /* the last write end of a pipe this process still owns: end of file for
     * the reader. A write end handed to a subprocess is the subprocess's to end
     * (its C RTL ends it when the image runs down), and a standard stream is a
     * process-permanent file, which an image's rundown does not close. */
    if (df->kind == DF_MBX && !df->stdbound && !df->handed &&
        (df->oflags & O_ACCMODE) != O_RDONLY)
        (void)mbx_write_eof(df->chan);
    (void)sys$dassgn(df->chan);
    free(df->rbuf);
    free(df->obuf);
    free(df);
}

/* Every terminal descriptor's waiting text, as it is (a read is coming). */
static void tt_prompt_all(void)
{
    for (int fd = 0; fd < DFD_MAX; fd++)
        if (dfd[fd] && dfd[fd]->kind == DF_TT && dfd[fd]->olen)
            tt_flush_line(dfd[fd], 0);
}

void ovmx_crtl_devfd_unbind(long long fd)
{
    struct dfile *df = dget(fd);
    if (!df)
        return;
    dfd[fd] = NULL;
    dfd_cloexec[fd] = 0;
    dfile_release(df);
}

/* ------------------------------------------------------------ terminal ---- */

/* A program's bytes to its terminal, as the DEC C RTL writes a terminal: each
 * newline ends a RECORD, and a record goes out with the terminal driver's
 * implied carriage control (IO$_WRITEVBLK, P4 " ": a new line before it, a
 * carriage return after) -- so a program's lines and DCL's interleave on the
 * screen as they do on VMS. Text not yet ended by a newline waits in the line
 * buffer; it goes out as a record at close, at image rundown and when it fills,
 * and as it is (a prompt) before the program reads the terminal. */
#define TT_OBUF 512u

static unsigned int tt_qio(struct dfile *df, const char *b, unsigned int n, int record)
{
    unsigned short iosb[4] = { 0, 0, 0, 0 };
    unsigned int st = sys$qiow(0, df->chan, IO_WRITEVBLK, iosb, 0, 0, (void *)b, n, 0,
                               record ? 0x20u : 0u, 0, 0);
    if ((st & 1) && !(iosb[0] & 1))
        st = iosb[0];
    return st;
}

static unsigned int tt_put_record(struct dfile *df)
{
    unsigned int st;
    if (df->olen)
        st = tt_qio(df, df->obuf, df->olen, 1);
    else
        st = tt_qio(df, "\n", 1, 0);   /* an empty record: the line feed owed */
    df->olen = 0;
    return st;
}

/* Text waiting for its newline goes out: as a record (close, rundown), or as
 * it is (before a read: the prompt the program wrote). */
static void tt_flush_line(struct dfile *df, int record)
{
    if (!df->olen)
        return;
    if (record)
        (void)tt_put_record(df);
    else {
        (void)tt_qio(df, df->obuf, df->olen, 0);
        df->olen = 0;
    }
}

static long long tt_write(struct dfile *df, const char *buf, uint64_t n)
{
    if (!df->obuf && !(df->obuf = malloc(TT_OBUF)))
        return -ENOMEM;
    for (uint64_t i = 0; i < n; i++) {
        unsigned int st = 1;
        if (buf[i] == '\n')
            st = tt_put_record(df);
        else {
            df->obuf[df->olen++] = buf[i];
            if (df->olen == TT_OBUF)
                st = tt_put_record(df);  /* a line longer than the buffer */
        }
        if (!(st & 1))
            return i ? (long long)i : -st_errno(st);
    }
    return (long long)n;
}

/* One line from the terminal: the terminal driver's read (echo, line editing,
 * the terminator). A carriage return ends the line and reads as '\n'; CTRL/Z
 * is end of file, as the DEC C RTL reads it. */
static void tt_prompt_all(void);

static int tt_fill(struct dfile *df)
{
    if (!df->rbuf && !(df->rbuf = malloc(TT_RDMAX + 1)))
        return -ENOMEM;
    tt_prompt_all();                    /* a prompt written without a newline */
    unsigned short iosb[4] = { 0, 0, 0, 0 };
    unsigned int st = sys$qiow(0, df->chan, IO_READVBLK, iosb, 0, 0,
                               df->rbuf, TT_RDMAX, 0, 0, 0, 0);
    if ((st & 1) && !(iosb[0] & 1))
        st = iosb[0];
    if (st == SS_ENDOFFILE) {
        df->rlen = df->roff = 0;
        return 0;
    }
    if (!(st & 1))
        return -st_errno(st);
    unsigned int len = iosb[1], term = iosb[2], tsz = iosb[3];
    if (len > TT_RDMAX)
        len = TT_RDMAX;
    if (tsz && term == 0x1A && len == 0) {           /* CTRL/Z */
        df->rlen = df->roff = 0;
        return 0;
    }
    if (tsz && term != 0x1A)
        df->rbuf[len++] = '\n';
    df->rlen = len;
    df->roff = 0;
    return (int)len;
}

/* ------------------------------------------------------------- mailbox ---- */

static long long mbx_write(struct dfile *df, const char *buf, uint64_t n)
{
    uint64_t done = 0;
    while (done < n) {
        unsigned int k = (unsigned int)((n - done) > PIPE_MAXMSG ? PIPE_MAXMSG : (n - done));
        unsigned short iosb[4] = { 0, 0, 0, 0 };
        unsigned int st = sys$qiow(0, df->chan, IO_WRITEVBLK | IO_M_NOW, iosb, 0, 0,
                                   (void *)(buf + done), k, 0, 0, 0, 0);
        if ((st & 1) && !(iosb[0] & 1))
            st = iosb[0];
        if (st == SS_MBFULL) {
            if (df->oflags & O_NONBLOCK)
                return done ? (long long)done : -EAGAIN;
            st = sys$qiow(0, df->chan, IO_WRITEVBLK, iosb, 0, 0,
                          (void *)(buf + done), k, 0, 0, 0, 0);
            if ((st & 1) && !(iosb[0] & 1))
                st = iosb[0];
        }
        if (!(st & 1))
            return done ? (long long)done : -st_errno(st);
        done += k;
    }
    return (long long)n;
}

/* One message. An end-of-file message reads as end of file. */
static int mbx_fill(struct dfile *df)
{
    if (!df->rbuf && !(df->rbuf = malloc(PIPE_MAXMSG + 1)))
        return -ENOMEM;
    for (;;) {
        unsigned short iosb[4] = { 0, 0, 0, 0 };
        unsigned int fn = IO_READVBLK | ((df->oflags & O_NONBLOCK) ? IO_M_NOW : 0);
        unsigned int st = sys$qiow(0, df->chan, fn, iosb, 0, 0, df->rbuf, PIPE_MAXMSG,
                                   0, 0, 0, 0);
        if ((st & 1) && !(iosb[0] & 1))
            st = iosb[0];
        if (st == SS_ENDOFFILE) {
            if (df->oflags & O_NONBLOCK)
                return -EAGAIN;     /* IO$M_NOW on an empty mailbox */
            df->reof = 1;
            df->rlen = df->roff = 0;
            return 0;
        }
        if (!(st & 1) && st != SS_BUFFEROVF)
            return -st_errno(st);
        unsigned int len = iosb[1] > PIPE_MAXMSG ? PIPE_MAXMSG : iosb[1];
        if (len == 0)
            continue;               /* a zero-length message carries nothing */
        df->rlen = len;
        df->roff = 0;
        return (int)len;
    }
}

/* ---------------------------------------------------------------- I/O ---- */

static long long d_read(struct dfile *df, char *buf, uint64_t n)
{
    if ((df->oflags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (n == 0)
        return 0;
    if (df->roff >= df->rlen) {
        int r = df->kind == DF_TT ? tt_fill(df) : mbx_fill(df);
        if (r <= 0)
            return r;
    }
    uint64_t k = df->rlen - df->roff;
    if (k > n)
        k = n;
    memcpy(buf, df->rbuf + df->roff, k);
    df->roff += (unsigned int)k;
    return (long long)k;
}

static long long d_write(struct dfile *df, const char *buf, uint64_t n)
{
    if ((df->oflags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (n == 0)
        return 0;
    return df->kind == DF_TT ? tt_write(df, buf, n) : mbx_write(df, buf, n);
}

static long long d_iov(struct dfile *df, const struct iovec *iov, long long cnt, int wr)
{
    long long total = 0;
    for (long long i = 0; i < cnt; i++) {
        if (!iov[i].iov_len)
            continue;
        long long r = wr ? d_write(df, iov[i].iov_base, iov[i].iov_len)
                         : d_read(df, iov[i].iov_base, iov[i].iov_len);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len)
            break;
        if (!wr)
            break;                  /* one line / one message per read */
    }
    return total;
}

/* fstat()'s answer for a descriptor of this layer: a character device (a
 * terminal) or a FIFO (a pipe's mailbox), st_ino the unit number. */
int ovmx_crtl_devfd_stat(long long fd, unsigned int *mode, unsigned long long *ino)
{
    struct dfile *df = dget(fd);
    unsigned int unit = 0;
    if (!df)
        return 0;
    (void)dvi_long(df->chan, 0, DVI_UNIT, &unit);
    *mode = df->kind == DF_TT ? (S_IFCHR | 0620) : (S_IFIFO | 0600);
    *ino = unit;
    return 1;
}

/* isatty() and the stdio line-buffering decision ask TIOCGWINSZ: the terminal's
 * width and page length, as the terminal driver senses them (IO$_SENSEMODE). */
static long long d_ioctl(struct dfile *df, unsigned long req, void *arg)
{
    if (df->kind != DF_TT)
        return -ENOTTY;
    if (req == TIOCGWINSZ) {
        unsigned char b[12];
        unsigned short iosb[4] = { 0, 0, 0, 0 };
        unsigned int st = sys$qiow(0, df->chan, IO_SENSEMODE, iosb, 0, 0, b, sizeof b,
                                   0, 0, 0, 0);
        if ((st & 1) && !(iosb[0] & 1))
            st = iosb[0];
        if (!(st & 1))
            return -st_errno(st);
        unsigned short *ws = arg;
        ws[0] = b[7];                               /* ws_row: the page length */
        ws[1] = (unsigned short)(b[2] | (b[3] << 8)); /* ws_col: the width     */
        ws[2] = ws[3] = 0;
        return 0;
    }
    return -EINVAL;                         /* not a terminal-driver function here */
}

/* --------------------------------------------------------------- pipe ---- */

/* pipe(): a temporary mailbox, a channel for each end (the read end first). */
static long long d_pipe(int *fds, long long flags)
{
    if (!sys$crembx || !sys$assign || !sys$getdviw)
        return -ENOSYS;                 /* a static image without LIBVMS */
    unsigned short rc = 0, wc = 0;
    unsigned int st = sys$crembx(0, &rc, PIPE_MAXMSG, PIPE_BUFQUO, 0, 0, 0, 0, 0);
    if (!(st & 1))
        return -st_errno(st);
    struct dfile *r = dfile_new(DF_MBX, rc, O_RDONLY | (int)(flags & O_NONBLOCK));
    if (!r || !r->dev[0]) {
        free(r);
        (void)sys$dassgn(rc);
        return r ? -EIO : -ENOMEM;
    }
    struct devfd_dsc d;
    dsc(&d, r->dev, strlen(r->dev));
    st = sys$assign(&d, &wc, 0, 0, 0);
    if (!(st & 1)) {
        free(r);
        (void)sys$dassgn(rc);
        return -st_errno(st);
    }
    struct dfile *w = dfile_new(DF_MBX, wc, O_WRONLY | (int)(flags & O_NONBLOCK));
    if (!w) {
        free(r);
        (void)sys$dassgn(wc);
        (void)sys$dassgn(rc);
        return -ENOMEM;
    }
    long long a = dfd_bind(r, (flags & O_CLOEXEC) != 0);
    if (a < 0) {
        r->refs = 1;
        dfile_release(r);
        w->refs = 1;
        w->handed = 1;              /* nothing to end: no reader */
        dfile_release(w);
        return a;
    }
    long long b = dfd_bind(w, (flags & O_CLOEXEC) != 0);
    if (b < 0) {
        ovmx_crtl_devfd_unbind(a);
        rawsys(SYS_close, a, 0, 0);
        w->refs = 1;
        w->handed = 1;
        dfile_release(w);
        return b;
    }
    fds[0] = (int)a;
    fds[1] = (int)b;
    return 0;
}

/* ------------------------------------------------------- descriptors ---- */

/* The C RTL's descriptor-number placeholder for df at newfd (dup2/dup3). */
static long long dup_onto(long long oldfd, long long newfd, long long flags)
{
    struct dfile *df = dfd[oldfd];
    /* A standard descriptor's placeholder stays open across a subprocess's
     * exec (not close-on-exec): the number must not come free in the new image
     * for the first host file it opens; an O_PATH descriptor carries no I/O. */
    long long k = rawsys(SYS_dup3, oldfd, newfd, newfd < 3 ? 0 : O_CLOEXEC);
    if (k < 0)
        return k;
    dfd[k] = df;
    dfd_cloexec[k] = (unsigned char)((flags & O_CLOEXEC) != 0);
    df->refs++;
    return k;
}

static long long dup_any(long long oldfd, long long min, int cloexec)
{
    struct dfile *df = dfd[oldfd];
    long long k = rawsys(SYS_fcntl, oldfd, F_DUPFD_CLOEXEC, min);
    if (k < 0)
        return k;
    if (k >= DFD_MAX) {
        rawsys(SYS_close, k, 0, 0);
        return -EMFILE;
    }
    dfd[k] = df;
    dfd_cloexec[k] = (unsigned char)(cloexec != 0);
    df->refs++;
    return k;
}

/* The RMS file layer's own unbind (crtl_rms_fd.c): a dup2 onto a descriptor
 * that was an RMS file closes that file first. */
extern void ovmx_crtl_rms_unbind(long long fd);

/*
 * Offered every system call before the RMS file layer. Serves a call on a
 * descriptor of this layer and pipe()/pipe2(), sets *handled and returns the
 * result; leaves every other call alone. dup2/dup3 ONTO a descriptor of this
 * layer from another kind releases the device first and lets the other layer
 * (or the kernel) do the dup.
 */
long long ovmx_crtl_devfd_call(long long n, long long a1, long long a2, long long a3,
                               long long a4, long long a5, int *handled)
{
    struct dfile *df;
    (void)a4; (void)a5;
    switch (n) {
#ifdef SYS_pipe
    case SYS_pipe:
        *handled = 1;
        return d_pipe((int *)(uintptr_t)a1, 0);
#endif
    case SYS_pipe2:
        *handled = 1;
        return d_pipe((int *)(uintptr_t)a1, a2);

    case SYS_read:
        if (!(df = dget(a1))) return 0;
        *handled = 1;
        return d_read(df, (char *)(uintptr_t)a2, (uint64_t)a3);
    case SYS_write:
        if (!(df = dget(a1))) return 0;
        *handled = 1;
        return d_write(df, (const char *)(uintptr_t)a2, (uint64_t)a3);
    case SYS_readv:
        if (!(df = dget(a1))) return 0;
        *handled = 1;
        return d_iov(df, (const struct iovec *)(uintptr_t)a2, a3, 0);
    case SYS_writev:
        if (!(df = dget(a1))) return 0;
        *handled = 1;
        return d_iov(df, (const struct iovec *)(uintptr_t)a2, a3, 1);
    case SYS_pread64: case SYS_pwrite64: case SYS_lseek:
#ifdef SYS_preadv
    case SYS_preadv: case SYS_pwritev:
#endif
        if (!dget(a1)) return 0;
        *handled = 1;
        return -ESPIPE;             /* a terminal or a mailbox has no position */

    case SYS_close:
        if (!dget(a1)) return 0;
        *handled = 1;
        ovmx_crtl_devfd_unbind(a1);
        return rawsys(SYS_close, a1, 0, 0);
    case SYS_close_range: {
        if (a3 & 4)                 /* CLOSE_RANGE_CLOEXEC */
            return 0;
        for (unsigned long long fd = (unsigned long long)a1;
             fd <= (unsigned long long)a2 && fd < DFD_MAX; fd++)
            if (dfd[fd])
                ovmx_crtl_devfd_unbind((long long)fd);
        return 0;                   /* the RMS layer and the kernel do the rest */
    }
    case SYS_dup:
        if (!dget(a1)) return 0;
        *handled = 1;
        return dup_any(a1, 0, 0);
    case SYS_dup3:
        if (a1 == a2)
            return 0;
        if (a2 >= 0 && a2 < DFD_MAX && dfd[a2])
            ovmx_crtl_devfd_unbind(a2);         /* newfd's device goes away */
        if (!dget(a1)) return 0;
        *handled = 1;
        ovmx_crtl_rms_unbind(a2);               /* ... or newfd's RMS file */
        return dup_onto(a1, a2, a3);
    case SYS_fcntl:
        if (!(df = dget(a1))) return 0;
        *handled = 1;
        switch (a2) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
            return dup_any(a1, a3, a2 == F_DUPFD_CLOEXEC);
        case F_GETFD:
            return dfd_cloexec[a1] ? FD_CLOEXEC : 0;
        case F_SETFD:
            dfd_cloexec[a1] = (a3 & FD_CLOEXEC) != 0;
            return 0;
        case F_GETFL:
            return df->oflags;
        case F_SETFL:
            df->oflags = (df->oflags & O_ACCMODE) | (int)(a3 & O_NONBLOCK);
            return 0;
        default:
            return -EINVAL;
        }
    case SYS_ioctl:
        if (!(df = dget(a1))) return 0;
        *handled = 1;
        return d_ioctl(df, (unsigned long)a2, (void *)(uintptr_t)a3);
    case SYS_fsync: case SYS_fdatasync:
        if (!(df = dget(a1))) return 0;
        *handled = 1;
        if (df->kind == DF_TT)
            tt_flush_line(df, 1);
        return 0;                   /* every other write was a completed $QIOW */
    case SYS_ftruncate: case SYS_fchmod: case SYS_flock: case SYS_fallocate:
    case SYS_getdents64:
        if (!dget(a1)) return 0;
        *handled = 1;
        return n == SYS_getdents64 ? -ENOTDIR : -EINVAL;
    default:
        return 0;
    }
}

/* ------------------------------------------------- the standard streams -- */

/* SYS$INPUT / SYS$OUTPUT / SYS$ERROR's device: the logical name translated
 * (iteratively, as RMS does for a device) with a process-permanent file's
 * ESC NUL IFI header dropped. */
static int std_device(const char *lognam, char *out, size_t outsz)
{
    char cur[256];
    size_t n = strlen(lognam);
    memcpy(cur, lognam, n + 1);
    for (int depth = 0; depth < 10; depth++) {
        struct devfd_dsc d;
        struct devfd_ile3 il[2];
        unsigned short rl = 0;
        char eq[256];
        memset(il, 0, sizeof il);
        il[0].buflen = sizeof eq - 1;
        il[0].code = LNM_STRING;
        il[0].buf = eq;
        il[0].retlen = &rl;
        size_t cl = strlen(cur);
        if (cl && cur[cl - 1] == ':')
            cl--;
        dsc(&d, cur, cl);
        if (!(sys$trnlnm(0, 0, &d, 0, il) & 1))
            break;                              /* not a logical name: a device */
        if (rl >= sizeof eq)
            rl = sizeof eq - 1;
        eq[rl] = '\0';
        char *v = eq;
        if (rl >= 4 && eq[0] == 0x1B && eq[1] == 0) {
            v = eq + 4;                         /* process-permanent file */
            rl = (unsigned short)(rl - 4);
        }
        if (!rl || strlen(v) >= sizeof cur)
            return 0;
        /* an equivalence that is no VMS name (TT's host-line spelling in a
         * process without a terminal row) ends the chain: the name before it
         * is what $ASSIGN resolves (TT: -> the console terminal) */
        if (v[0] == '/')
            break;
        memmove(cur, v, strlen(v) + 1);
    }
    n = strlen(cur);
    /* a device: "ddcu:" with nothing after the colon */
    char *col = strchr(cur, ':');
    if (!col || col[1] != '\0' || n >= outsz)
        return 0;
    memcpy(out, cur, n + 1);
    return 1;
}

/* The substrate's own TCGETS: on Linux/Alpha it is _IOR('t', 19, struct
 * termios), not the generic number musl-arch's headers carry (the C RTL's own
 * TIOCGWINSZ/TCGETS are answered here, from the channel, never by the kernel). */
#if defined(__alpha__)
#define SUBSTRATE_TCGETS 0x402c7413
#else
#define SUBSTRATE_TCGETS TCGETS
#endif

static int line_is_terminal(int fd)
{
    char t[128];                                 /* struct termios, generously */
    return rawsys(SYS_ioctl, fd, SUBSTRATE_TCGETS, (long long)(uintptr_t)t) == 0;
}

/* A host pipe: the creator pumps this stream into the mailbox from a pipe of
 * its own (LIB$SPAWN's DCL). F_GETPIPE_SZ answers only on a pipe. */
static int line_is_host_pipe(int fd)
{
    return rawsys(SYS_fcntl, fd, 1032 /* F_GETPIPE_SZ */, 0) >= 0;
}

/*
 * Bind descriptors 0, 1 and 2 to SYS$INPUT, SYS$OUTPUT and SYS$ERROR when each
 * is a terminal or a mailbox (see the head comment for what stays as it is).
 * Called once, from the C RTL's image start, before main().
 */
void ovmx_crtl_devfd_bind_stdio(void)
{
    if (!sys$assign || !sys$trnlnm || !sys$qiow || !sys$getdviw)
        return;                         /* a static image without LIBVMS */
    static const char *const names[3] = { "SYS$INPUT", "SYS$OUTPUT", "SYS$ERROR" };
    for (int fd = 0; fd < 3; fd++) {
        char dev[256];
        unsigned int cls = 0;
        if (dfd[fd] || !std_device(names[fd], dev, sizeof dev))
            continue;
        struct devfd_dsc d;
        unsigned short chan = 0;
        dsc(&d, dev, strlen(dev));
        if (!(sys$assign(&d, &chan, 0, 0, 0) & 1))
            continue;
        if (!dvi_long(chan, 0, DVI_DEVCLASS, &cls)) {
            /* a terminal channel's class, as its driver senses it */
            unsigned char b[12];
            unsigned short iosb[4] = { 0, 0, 0, 0 };
            unsigned int st = sys$qiow(0, chan, IO_SENSEMODE, iosb, 0, 0, b, sizeof b,
                                       0, 0, 0, 0);
            cls = ((st & 1) && (iosb[0] & 1)) ? b[0] : 0;
        }
        if (cls == DC_TERM) {
            if (!line_is_terminal(fd))
                cls = 0;                        /* redirected underneath */
        } else if (cls == DC_MAILBOX) {
            if (line_is_host_pipe(fd))
                cls = 0;                        /* pumped from a host pipe */
        } else {
            cls = 0;                            /* a file: RMS's, not ours */
        }
        if (cls != DC_TERM && cls != DC_MAILBOX) {
            (void)sys$dassgn(chan);
            continue;
        }
        struct dfile *df = dfile_new(cls == DC_TERM ? DF_TT : DF_MBX, chan,
                                     fd == 0 ? O_RDONLY : O_WRONLY);
        if (!df) {
            (void)sys$dassgn(chan);
            continue;
        }
        df->stdbound = 1;
        if (!df->dev[0]) {
            size_t k = strlen(dev);
            if (k >= sizeof df->dev)
                k = sizeof df->dev - 1;
            memcpy(df->dev, dev, k);
            df->dev[k] = '\0';
        }
        /* the number stays the substrate's stream underneath (its children
         * that are not C RTL images still write there); the C RTL's I/O on it
         * is the channel's from here */
        dfd[fd] = df;
        dfd_cloexec[fd] = 0;
        df->refs = 1;
    }
}

/* ------------------------------------------------------ name, handing ---- */

/* DEC C getname(): the device a descriptor of this layer is a channel to. */
int ovmx_crtl_devfd_name(long long fd, char *buf, size_t sz)
{
    struct dfile *df = dget(fd);
    if (!df || !df->dev[0])
        return 0;
    size_t k = strlen(df->dev);
    if (k >= sz)
        return -1;
    memcpy(buf, df->dev, k + 1);
    return 1;
}

/* exec*(): the device behind standard descriptor fd, for the subprocess's
 * SYS$INPUT/SYS$OUTPUT/SYS$ERROR. A pipe write end given to the subprocess is
 * the subprocess's to end: closing it here does not send end of file. */
int ovmx_crtl_devfd_give(long long fd, char *dev, size_t devsz)
{
    struct dfile *df = dget(fd);
    if (!df || !df->dev[0] || strlen(df->dev) >= devsz)
        return 0;
    memcpy(dev, df->dev, strlen(df->dev) + 1);
    if (df->kind == DF_MBX && (df->oflags & O_ACCMODE) != O_RDONLY)
        df->handed = 1;
    return 1;
}

/*
 * Image rundown. A standard stream that is a mailbox of a pipe the creator
 * made (SYS$OUTPUT given to this subprocess by exec*()) is ended with end of
 * file, the way the DEC C RTL closes its files at exit. A mailbox SYS$OUTPUT a
 * DCL pumps from a host pipe is never bound, so never ended here.
 */
void ovmx_crtl_devfd_rundown(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    for (int fd = 0; fd < DFD_MAX; fd++)
        if (dfd[fd] && dfd[fd]->kind == DF_TT)
            tt_flush_line(dfd[fd], 1);  /* the last line, as a record */
    for (int fd = 1; fd < 3; fd++) {
        struct dfile *df = dfd[fd];
        if (!df || df->kind != DF_MBX || !df->stdbound || df->handed)
            continue;
        df->handed = 1;
        /* once per mailbox, though SYS$ERROR may name SYS$OUTPUT's */
        if (fd == 2 && dfd[1] && dfd[1]->kind == DF_MBX && dfd[1]->stdbound &&
            strcmp(dfd[1]->dev, df->dev) == 0)
            continue;
        (void)mbx_write_eof(df->chan);
    }
}

/*
 * Does the device field of an RMS file specification name a device or a
 * logical name? A UNIX name's first component is its device ("/proc/self/x"
 * is PROC:[SELF]X); when that is neither a logical name ($TRNLNM, the
 * LNM$FILE_DEV tables RMS searches) nor a device the executive has ($GETDVI),
 * the file cannot exist -- ENOENT, the "no such device" answer the DEC C RTL
 * gives -- and RMS is not asked to look it up.
 */
int ovmx_crtl_devfd_device_known(const char *spec)
{
    if (!sys$trnlnm || !sys$getdviw)
        return 1;                       /* a static image without LIBVMS: RMS decides */
    const char *col = strchr(spec, ':');
    if (!col || col == spec || (size_t)(col - spec) >= 64)
        return 1;                       /* no device field: RMS's default */
    char dev[64];
    size_t n = (size_t)(col - spec);
    memcpy(dev, spec, n);
    dev[n] = '\0';
    struct devfd_dsc d;
    struct devfd_ile3 il[2];
    unsigned short rl = 0;
    char eq[256];
    memset(il, 0, sizeof il);
    il[0].buflen = sizeof eq;
    il[0].code = LNM_STRING;
    il[0].buf = eq;
    il[0].retlen = &rl;
    dsc(&d, dev[0] == '_' ? dev + 1 : dev, dev[0] == '_' ? n - 1 : n);
    if (dev[0] != '_' && (sys$trnlnm(0, 0, &d, 0, il) & 1))
        return 1;                       /* a logical name */
    unsigned int cls = 0;
    dev[n] = ':';
    dev[n + 1 < sizeof dev ? n + 1 : n] = '\0';
    return dvi_long(0, dev, DVI_DEVCLASS, &cls);
}
