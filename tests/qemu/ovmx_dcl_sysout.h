/*
 * ovmx_dcl_sysout.h - a QEMU suite reads what the DCL it drives writes from
 * that DCL's SYS$OUTPUT MAILBOX, never from a pipe on the DCL's substrate
 * stdout (rd vms-2f39).
 *
 * WHAT VMS DOES. A process DCL's output -- SHOW, WRITE, its messages, the
 * output of the images it runs -- is records written to the process-permanent
 * file SYS$OUTPUT; a parent that wants a subprocess's output names a mailbox
 * as that subprocess's SYS$OUTPUT (LIB$SPAWN OUTPUT=) and reads one message
 * per record (VSI OpenVMS DCL Dictionary, SYS$OUTPUT; I/O User's Reference,
 * Mailbox Driver). DCL routes its records there by $QIO IO$_WRITEVBLK and an
 * image's output through its relay (src/vmsdcl/dcl_mbx.c, rd vms-1b6a).
 *
 * THE DRIVE.
 *   parent, before the fork: ovmx_dcl_sysout_open() $CREMBXes a temporary
 *     mailbox (no logical name, so nothing system-wide changes) and starts a
 *     reader thread that takes each message, in order, into a buffer.
 *   child, between ovmx_fork_*() and exec: ovmx_dcl_sysout_child() defines
 *     SYS$OUTPUT and SYS$ERROR in the CHILD's own LNM$PROCESS_TABLE as that
 *     mailbox (supervisor mode, as DCL's DEFINE does, so the image rundown of
 *     the exec does not take them) and puts its substrate stdout/stderr on the
 *     null device. Nothing DCL writes can reach the suite through a
 *     descriptor: a record that is not in the mailbox is not seen at all.
 *   parent, after reaping the child: ovmx_dcl_sysout_collect() queues an
 *     end-of-file behind DCL's last message, joins the reader and hands back
 *     the text -- the records concatenated in arrival order, each as DCL wrote
 *     it (a record carries its own line terminator), truncated to the caller's
 *     buffer exactly as the pipe read it replaces was.
 *   ovmx_dcl_sysout_peek() copies what has arrived so far, for a suite that
 *     watches DCL's output while it still runs.
 *   ovmx_dcl_sysout_note() is the CHILD's own channel for what it reports
 *     before exec (a setup verdict, an exec failure): a close-on-exec pipe DCL
 *     never inherits; collect() puts those lines ahead of DCL's records.
 *   ovmx_dcl_sysout_open_flags(.., OVMX_DCL_SYSOUT_F_OUTPUT_ONLY) names the
 *     mailbox as SYS$OUTPUT alone, for a suite asserting the whole of
 *     SYS$OUTPUT without DCL's SYS$ERROR messages.
 *
 * SYS$INPUT is not this header's business: a suite that feeds DCL a command
 * stream keeps doing so.
 *
 * NO EXECUTIVE: $CREMBX fails SS$_NOSUCHDEV; open returns that status and the
 * suite reports it (every caller has already SKIPped on no /dev/vms).
 *
 * Header-only, like ovmx_test_fork.h, so every suite shares one copy.
 */
#ifndef OVMX_DCL_SYSOUT_H
#define OVMX_DCL_SYSOUT_H
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "vms_kif.h"

#define OVMX_DCL_SYSOUT_MAXMSG 4096u
#define OVMX_DCL_SYSOUT_BUFQUO 65536u

/* SYS$OUTPUT only: SYS$ERROR is left undefined, so what DCL sends to
 * SYS$ERROR alone (its messages) is not in the mailbox -- for a suite whose
 * assertion is "the whole of SYS$OUTPUT is exactly ..." */
#define OVMX_DCL_SYSOUT_F_OUTPUT_ONLY 0x1u

struct ovmx_dcl_sysout {
    uint32_t        chan;          /* the suite's executive channel */
    char            dev[32];       /* "MBAn:" */
    pthread_t       reader;
    int             reader_up;
    pthread_mutex_t mtx;
    char           *buf;
    size_t          used, cap;
    uint32_t        nmsg;          /* messages taken (records) */
    unsigned        flags;         /* OVMX_DCL_SYSOUT_F_* */
    int             note[2];       /* the child's pre-exec notes (O_CLOEXEC) */
};

static void *ovmx_dcl_sysout_reader_(void *arg)
{
    struct ovmx_dcl_sysout *o = (struct ovmx_dcl_sysout *)arg;
    static __thread char msg[OVMX_DCL_SYSOUT_MAXMSG];
    for (;;) {
        uint32_t actlen = 0;
        /* SS$_ENDOFFILE (even) is collect()'s end-of-file: stop there */
        if (!(vms_kif_mbx_read(o->chan, msg, sizeof msg, &actlen, 0) & 1))
            break;
        if (actlen > sizeof msg)
            actlen = sizeof msg;
        pthread_mutex_lock(&o->mtx);
        if (o->used + actlen + 1 > o->cap) {
            size_t ncap = o->cap ? o->cap : 16384;
            while (o->used + actlen + 1 > ncap)
                ncap *= 2;
            char *nb = (char *)realloc(o->buf, ncap);
            if (nb) { o->buf = nb; o->cap = ncap; }
        }
        if (o->used + actlen + 1 <= o->cap) {
            memcpy(o->buf + o->used, msg, actlen);
            o->used += actlen;
            o->buf[o->used] = '\0';
        }
        o->nmsg++;
        pthread_mutex_unlock(&o->mtx);
    }
    return NULL;
}

/* Parent, before the fork. Returns the $CREMBX status (odd = ready). */
static inline uint32_t ovmx_dcl_sysout_open_flags(struct ovmx_dcl_sysout *o,
                                                  unsigned flags)
{
    uint32_t unit = 0;
    memset(o, 0, sizeof *o);
    o->flags = flags;
    o->note[0] = o->note[1] = -1;
    pthread_mutex_init(&o->mtx, NULL);
    if (pipe(o->note) == 0) {
        (void)fcntl(o->note[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(o->note[1], F_SETFD, FD_CLOEXEC);
        (void)fcntl(o->note[0], F_SETFL, O_NONBLOCK);
    } else {
        o->note[0] = o->note[1] = -1;
    }
    uint32_t st = vms_kif_mbx_create(0, OVMX_DCL_SYSOUT_MAXMSG,
                                     OVMX_DCL_SYSOUT_BUFQUO, &o->chan, &unit,
                                     o->dev, sizeof o->dev);
    if (!(st & 1)) {
        if (o->note[0] >= 0) { close(o->note[0]); close(o->note[1]); }
        o->note[0] = o->note[1] = -1;
        return st;
    }
    if (pthread_create(&o->reader, NULL, ovmx_dcl_sysout_reader_, o) != 0) {
        (void)vms_kif_mbx_delmbx(o->chan);
        (void)vms_kif_dassgn(o->chan);
        return 0x124; /* SS$_INSFMEM */
    }
    o->reader_up = 1;
    return st;
}

static inline uint32_t ovmx_dcl_sysout_open(struct ovmx_dcl_sysout *o)
{
    return ovmx_dcl_sysout_open_flags(o, 0);
}

static inline uint32_t ovmx_dcl_sysout_define_(const char *name, const char *dev)
{
    static const char tab[] = "LNM$PROCESS_TABLE";
    struct dsc$descriptor_s tabd = { (uint16_t)(sizeof tab - 1), DSC$K_DTYPE_T,
                                     DSC$K_CLASS_S, (char *)tab };
    struct dsc$descriptor_s nm = { (uint16_t)strlen(name), DSC$K_DTYPE_T,
                                   DSC$K_CLASS_S, (char *)name };
    struct item_list_3 it[2];
    uint8_t mode = PSL$C_SUPER;
    memset(it, 0, sizeof it);
    it[0].buflen    = (uint16_t)strlen(dev);
    it[0].item_code = LNM$_STRING;
    it[0].bufaddr   = (void *)dev;
    return sys$crelnm(NULL, &tabd, &nm, &mode, it);
}

/* Child, after ovmx_fork_*() returned 0 and before exec. A child that cannot
 * name the mailbox exits 126: it never runs a DCL whose output would go
 * nowhere the suite reads. */
static inline void ovmx_dcl_sysout_child(const struct ovmx_dcl_sysout *o)
{
    if (!(ovmx_dcl_sysout_define_("SYS$OUTPUT", o->dev) & 1) ||
        (!(o->flags & OVMX_DCL_SYSOUT_F_OUTPUT_ONLY) &&
         !(ovmx_dcl_sysout_define_("SYS$ERROR", o->dev) & 1)))
        _exit(126);
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) {
        dup2(nul, STDOUT_FILENO);
        dup2(nul, STDERR_FILENO);
        if (nul > STDERR_FILENO)
            close(nul);
    }
}

/* Child, before exec (or after a failed one): a line the SUITE's child itself
 * reports -- a setup verdict, an exec failure. It is not DCL's output and
 * never goes through DCL's streams: it travels on a close-on-exec pipe, so
 * DCL cannot inherit it, and collect() puts it ahead of DCL's records. */
static inline void ovmx_dcl_sysout_note(const struct ovmx_dcl_sysout *o,
                                        const char *text)
{
    size_t n = strlen(text), off = 0;
    if (o->note[1] < 0)
        return;
    while (off < n) {
        ssize_t w = write(o->note[1], text + off, n - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
}

/* What has arrived so far, NUL-terminated (truncated to outsz - 1). */
static inline size_t ovmx_dcl_sysout_peek(struct ovmx_dcl_sysout *o,
                                          char *out, size_t outsz)
{
    size_t n;
    pthread_mutex_lock(&o->mtx);
    n = o->used < outsz - 1 ? o->used : outsz - 1;
    if (n)
        memcpy(out, o->buf, n);
    out[n] = '\0';
    pthread_mutex_unlock(&o->mtx);
    return n;
}

/* Parent, after the child is reaped: end-of-file behind DCL's last record,
 * join the reader, copy out, deassign (the temporary mailbox goes with its
 * last channel). Returns the bytes copied. */
static inline size_t ovmx_dcl_sysout_collect(struct ovmx_dcl_sysout *o,
                                             char *out, size_t outsz)
{
    size_t n = 0;
    if (o->reader_up) {
        (void)vms_kif_mbx_write_eof(o->chan);
        pthread_join(o->reader, NULL);
        o->reader_up = 0;
    }
    if (o->note[1] >= 0) {
        close(o->note[1]);
        o->note[1] = -1;
    }
    if (out && outsz) {
        /* the child's notes first: they were written before DCL ran */
        size_t k = 0;
        ssize_t r;
        while (o->note[0] >= 0 && k < outsz - 1 &&
               (r = read(o->note[0], out + k, outsz - 1 - k)) > 0)
            k += (size_t)r;
        out[k] = '\0';
        n = k + ovmx_dcl_sysout_peek(o, out + k, outsz - k);
    }
    if (o->note[0] >= 0) {
        close(o->note[0]);
        o->note[0] = -1;
    }
    (void)vms_kif_mbx_delmbx(o->chan);
    (void)vms_kif_dassgn(o->chan);
    free(o->buf);
    o->buf = NULL;
    o->used = o->cap = 0;
    pthread_mutex_destroy(&o->mtx);
    return n;
}

#endif /* OVMX_DCL_SYSOUT_H */
