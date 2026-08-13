/*
 * dcl_mbx.c - bind DCL's SYS$INPUT / SYS$OUTPUT to mailbox devices (vms-786)
 *
 * THE GAP THIS CLOSES. DCL's command-read loop (dcl_main.c) reads from stdin
 * with fgets()/readline() and writes results with printf() to stdout -- a Unix
 * fd/stdio model. OVMX mailboxes are executive-resident and have NO local fd
 * (src/libvms/syssvc/sys_mailbox.c: the message queue lives in vms.ko, reached
 * with IO$_READVBLK/IO$_WRITEVBLK through vms_kif_mbx_read/write). So a
 * persistent DCL a parent drives over mailboxes -- MMK's send_cmd_and_wait,
 * which keeps one DCL open and feeds it resolved command lines over one mailbox
 * while reading each command's results back over a second
 * (docs/design-mmk-exec-drive-ovmx.md, vms-b23) -- had no way to take its
 * SYS$INPUT from, or send its SYS$OUTPUT to, a mailbox.
 *
 * THE SEAM (option (a): an fd-like handle DCL's existing loop can use). Rather
 * than teach every one of DCL's ~hundreds of printf() sites and its three
 * command-read sites about mailboxes, this binds the mailbox to DCL's stdin/
 * stdout through a pipe whose FAR END is the real mailbox executive path:
 *
 *   SYS$INPUT mailbox --[IO$_READVBLK]--> reader thread --> pipe --> fd 0
 *   fd 1 --> pipe --> writer thread --[IO$_WRITEVBLK]--> SYS$OUTPUT mailbox
 *
 * Every command byte still ARRIVES over the mailbox (the reader thread does the
 * IO$_READVBLK) and every result byte still LEAVES over the mailbox (the writer
 * thread does the IO$_WRITEVBLK) -- real executive I/O, no monkeypatch, no
 * fabrication. DCL's own loop, unchanged, reads lines from fd 0 and writes to
 * fd 1.
 *
 * WHY vms_kif_mbx_* AND NOT sys$qiow. OVMX's userspace channel table (the PCB)
 * is THREAD-LOCAL (src/vmsprocess/vms_pcb.c, `__thread current_pcb`), so a
 * channel $ASSIGNed on DCL's main thread is not visible to a helper thread's
 * sys$qiow. The executive, by contrast, keys a process by its THREAD-GROUP id
 * (src/kernel/vms_module.c vms_proc_find_or_err uses current->tgid), so a
 * mailbox channel assigned to this process is reachable from ANY of its threads
 * through the kernel-interface client -- whose own bind path is written for
 * exactly this ("a sibling thread of the SAME process ... the register below is
 * adopted onto the process's existing PCB", vms_kif.c kif_bind). So the main
 * thread assigns the mailbox with vms_kif_mbx_assign() to get the executive
 * channel number, and each bridge thread issues vms_kif_mbx_read/write() on it
 * directly -- the same real ioctls sys$qiow's mailbox path (sys_qio.c
 * qio_mailbox_op) issues, minus the thread-local PCB translation that would
 * fail off the main thread.
 *
 * CLEAN-ROOM (Rule 8). That a DCL subprocess reads commands from a SYS$INPUT
 * mailbox and writes results to a SYS$OUTPUT mailbox is public VSI behaviour:
 * LIB$SPAWN's INPUT/OUTPUT arguments may name mailboxes and the subprocess's
 * SYS$INPUT/SYS$OUTPUT are then those mailboxes (VSI OpenVMS RTL Library (LIB$)
 * Manual, LIB$SPAWN), and a mailbox is a record device read/written with $QIO
 * IO$_READVBLK / IO$_WRITEVBLK (VSI OpenVMS I/O User's Reference, Mailbox
 * Driver). No VSI source or byte layout is used.
 *
 * ADDED SOURCE, NOT REPLACEMENT (Rule 4). Binding happens only when the
 * SYS$INPUT / SYS$OUTPUT logical name translates to a mailbox device (MBAn:).
 * A login DCL (SYS$INPUT the terminal, or undefined) and an @-procedure / -c
 * file DCL translate to something that is not a mailbox, so this binds nothing
 * for them and their fd/stdio path is untouched.
 *
 * NO EOF FROM THE MAILBOX. The executive's mailbox read blocks until a message
 * is queued and has no writers-gone / end-of-file condition yet
 * (src/kernel-core/vms_mbx.c, vms_ioctl_mbx_read waits on read_wq). A driven
 * DCL therefore leaves its loop the VMS way -- the parent sends a LOGOUT (or
 * EXIT) command down SYS$INPUT -- not by the mailbox signalling EOF. The reader
 * thread is left detached; the process exit that follows LOGOUT reclaims it and
 * the executive channel.
 *
 * HONEST FAILURE (Rule 9 / INV-6). Reaching the mailbox goes through the
 * executive; with no /dev/vms the vms_kif_mbx_* calls fail and this binds
 * nothing, leaving DCL on its ordinary stdin/stdout. It never fabricates a
 * per-process substitute for a mailbox.
 */

#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "vms_kif.h"
#include "dcl/dcl_mbx.h"

/* One mailbox message never exceeds the executive's per-ioctl cap
 * (VMS_MBX_IOCTL_MAXLEN == 4096, src/kernel/vms_mbx.h); a DCL command line is
 * DCL_MAX_LINE (4096) too, so this buffer holds either. */
#define DCL_MBX_BUF 4096

static uint32_t  g_in_chan     = 0;   /* SYS$INPUT executive mailbox channel */
static uint32_t  g_out_chan    = 0;   /* SYS$OUTPUT executive mailbox channel */
static int       g_in_pipe_w   = -1;  /* reader thread -> DCL fd 0 (write end) */
static int       g_out_pipe_r  = -1;  /* DCL fd 1 -> writer thread (read end) */
static pthread_t g_reader;
static pthread_t g_writer;
static int       g_reader_up   = 0;
static int       g_writer_up   = 0;

/* A VMS mailbox device name is MBA<unit>: -- what $CREMBX publishes and what a
 * SYS$INPUT/SYS$OUTPUT logical must resolve to for us to bind it. */
static int is_mbx_device(const char *s)
{
    size_t n = strlen(s);
    if (n < 5) return 0;                    /* at least "MBA0:" */
    if (toupper((unsigned char)s[0]) != 'M' ||
        toupper((unsigned char)s[1]) != 'B' ||
        toupper((unsigned char)s[2]) != 'A')
        return 0;
    if (s[n - 1] != ':') return 0;
    for (size_t i = 3; i < n - 1; i++)
        if (!isdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

/*
 * resolve_to_mailbox - translate a logical name (SYS$INPUT / SYS$OUTPUT)
 * through the standard search order, following indirection, until it names a
 * mailbox device. Returns 1 with *out set to the "MBAn:" device name, 0 if the
 * name is undefined or resolves to something that is not a mailbox (a terminal,
 * a file, undefined -- all of which leave DCL on its fd/stdio path).
 */
static int resolve_to_mailbox(const char *name, char *out, size_t outsz)
{
    char cur[256];

    strncpy(cur, name, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';

    for (int depth = 0; depth < 10; depth++) {
        if (is_mbx_device(cur)) {
            strncpy(out, cur, outsz - 1);
            out[outsz - 1] = '\0';
            return 1;
        }

        struct dsc$descriptor_s namdsc;
        char equiv[256];
        uint16_t rl = 0;
        struct item_list_3 itmlst[2];

        vms_cstr_to_desc(&namdsc, cur);
        memset(itmlst, 0, sizeof(itmlst));
        itmlst[0].buflen    = (uint16_t)(sizeof(equiv) - 1);
        itmlst[0].item_code = LNM$_STRING;
        itmlst[0].bufaddr   = equiv;
        itmlst[0].retlen    = &rl;
        equiv[0] = '\0';

        uint32_t st = sys$trnlnm(NULL, NULL, &namdsc, NULL, itmlst);
        if (!(st & 1) || rl == 0)
            return 0;                        /* not a logical name / undefined */
        if (rl >= sizeof(equiv))
            rl = (uint16_t)(sizeof(equiv) - 1);
        equiv[rl] = '\0';
        if (strcmp(equiv, cur) == 0)
            return 0;                        /* no progress: not a mailbox */

        strncpy(cur, equiv, sizeof(cur) - 1);
        cur[sizeof(cur) - 1] = '\0';
    }
    return 0;
}

/*
 * reader_main - drain the SYS$INPUT mailbox into DCL's stdin pipe. Each
 * IO$_READVBLK returns one message (one command record the parent wrote); it is
 * forwarded to the pipe and terminated with a newline so DCL's fgets()-based
 * loop sees a complete line. A read that fails (e.g. the executive became
 * unreachable) closes the pipe, so DCL's stdin hits EOF and the REPL ends.
 */
static void *reader_main(void *arg)
{
    char buf[DCL_MBX_BUF];
    (void)arg;

    for (;;) {
        uint32_t actlen = 0;
        /* nowait=0: block until a command record arrives (VMS $QIO READVBLK on
         * a mailbox waits for a message; IO$M_NOW is vms-5df, not wanted here). */
        uint32_t st = vms_kif_mbx_read(g_in_chan, buf, (uint32_t)sizeof(buf),
                                       &actlen, 0);
        if (!(st & 1))
            break;

        uint32_t mlen = actlen;
        if (mlen > sizeof(buf))
            mlen = sizeof(buf);
        /* A writer that sent the record with a trailing NUL (a write of
         * strlen+1) should not push that NUL into the command line. */
        if (mlen > 0 && buf[mlen - 1] == '\0')
            mlen--;

        size_t off = 0;
        while (off < mlen) {
            ssize_t w = write(g_in_pipe_w, buf + off, mlen - off);
            if (w <= 0)
                goto done;
            off += (size_t)w;
        }
        /* Deliver a complete line even if the record carried no newline. */
        if (mlen == 0 || buf[mlen - 1] != '\n') {
            char nl = '\n';
            if (write(g_in_pipe_w, &nl, 1) != 1)
                break;
        }
    }

done:
    if (g_in_pipe_w >= 0) {
        close(g_in_pipe_w);
        g_in_pipe_w = -1;
    }
    return NULL;
}

/*
 * writer_main - drain DCL's stdout pipe to the SYS$OUTPUT mailbox. Each read()
 * of DCL's output becomes one IO$_WRITEVBLK message the parent reads back. Ends
 * when the pipe's last write end is closed (DCL exited and dcl_mbx_shutdown()
 * closed fd 1), which returns 0 from read().
 */
static void *writer_main(void *arg)
{
    char buf[DCL_MBX_BUF];
    (void)arg;

    for (;;) {
        ssize_t n = read(g_out_pipe_r, buf, sizeof(buf));
        if (n <= 0)
            break;
        uint32_t st = vms_kif_mbx_write(g_out_chan, buf, (uint32_t)n);
        if (!(st & 1))
            break;
    }
    return NULL;
}

int dcl_mbx_bind_std_streams(void)
{
    int bound = 0;
    char dev[256];

    /* SYS$INPUT: commands arrive here. */
    if (resolve_to_mailbox("SYS$INPUT", dev, sizeof(dev))) {
        uint32_t chan = 0;
        if (vms_kif_mbx_assign(dev, &chan) & 1) {
            int p[2];
            if (pipe(p) == 0) {
                g_in_chan   = chan;
                g_in_pipe_w = p[1];
                if (dup2(p[0], STDIN_FILENO) >= 0 &&
                    pthread_create(&g_reader, NULL, reader_main, NULL) == 0) {
                    pthread_detach(g_reader);
                    g_reader_up = 1;
                    bound |= DCL_MBX_BOUND_INPUT;
                    close(p[0]);
                } else {
                    close(p[0]);
                    close(p[1]);
                    g_in_pipe_w = -1;
                    g_in_chan   = 0;
                    (void)vms_kif_dassgn(chan);
                }
            } else {
                (void)vms_kif_dassgn(chan);
            }
        }
    }

    /* SYS$OUTPUT: results leave here. */
    if (resolve_to_mailbox("SYS$OUTPUT", dev, sizeof(dev))) {
        uint32_t chan = 0;
        if (vms_kif_mbx_assign(dev, &chan) & 1) {
            int p[2];
            if (pipe(p) == 0) {
                fflush(stdout);
                g_out_chan   = chan;
                g_out_pipe_r = p[0];
                if (dup2(p[1], STDOUT_FILENO) >= 0 &&
                    pthread_create(&g_writer, NULL, writer_main, NULL) == 0) {
                    g_writer_up = 1;
                    bound |= DCL_MBX_BOUND_OUTPUT;
                    close(p[1]);
                    /* fd 1 is a pipe now, so stdio would fully buffer it;
                     * line-buffer so each command's output reaches the parent
                     * as it is produced, not only at exit. */
                    setvbuf(stdout, NULL, _IOLBF, 0);
                } else {
                    close(p[0]);
                    close(p[1]);
                    g_out_pipe_r = -1;
                    g_out_chan   = 0;
                    (void)vms_kif_dassgn(chan);
                }
            } else {
                (void)vms_kif_dassgn(chan);
            }
        }
    }

    return bound;
}

void dcl_mbx_shutdown(void)
{
    if (g_writer_up) {
        /* Flush DCL's remaining SYS$OUTPUT into the pipe, then close the last
         * write end (fd 1) so writer_main() drains it and sees EOF; join so the
         * final IO$_WRITEVBLK completes before we return (the parent's last
         * read must not race process exit). */
        fflush(stdout);
        close(STDOUT_FILENO);
        pthread_join(g_writer, NULL);
        g_writer_up = 0;
        if (g_out_pipe_r >= 0) {
            close(g_out_pipe_r);
            g_out_pipe_r = -1;
        }
        if (g_out_chan) {
            (void)vms_kif_dassgn(g_out_chan);
            g_out_chan = 0;
        }
    }

    /* The reader thread is blocked in IO$_READVBLK on the (now command-drained)
     * input mailbox; it is detached and reclaimed by process exit. We do NOT
     * $DASSGN the input channel here -- tearing it down under an in-flight read
     * in another thread would be a race, and process exit reclaims the channel
     * cleanly. */
    g_reader_up = 0;
}
