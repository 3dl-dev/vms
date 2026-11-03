/*
 * dcl_mbx.c - bind DCL's SYS$OUTPUT to a mailbox device (vms-786, vms-1b6a)
 *
 * SYS$INPUT is no longer bound here: DCL reads a SYS$INPUT mailbox itself,
 * $QIOW IO$_READVBLK on its own channel, and a SYS$INPUT file through RMS
 * (dcl_in.c, rd vms-4d27). The reader threads and the pipe on fd 0 that the
 * history below describes for input are retired.
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

#include "dcl/dcl_out.h"
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "iodef.h"
#include "vms_kif.h"
#include "dcl/dcl_mbx.h"
#include "dcl/dcl_in.h"

/* One mailbox message never exceeds the executive's per-ioctl cap
 * (VMS_MBX_IOCTL_MAXLEN == 4096, src/kernel/vms_mbx.h); a DCL command line is
 * DCL_MAX_LINE (4096) too, so this buffer holds either. */
#define DCL_MBX_BUF 4096

static uint32_t  g_out_chan    = 0;   /* SYS$OUTPUT executive mailbox channel */
static int       g_out_pipe_r  = -1;  /* DCL fd 1 -> writer thread (read end) */
static pthread_t g_writer;
static int       g_writer_up   = 0;

/*
 * SYS$OUTPUT A MAILBOX: TWO WRITERS, ONE ORDER (rd vms-1b6a).
 *
 * DCL's OWN output -- every record its sink (dcl_out.c) produces -- goes to the
 * mailbox as a $QIO IO$_WRITEVBLK on a channel DCL $ASSIGNed on its main thread
 * (dcl_mbx_put_record). What an IMAGE DCL runs writes still arrives on fd 1, a
 * pipe, and is relayed to the mailbox a line at a time (line_relay_feed): an
 * image's run-time library writing its descriptor is the image's I/O, not
 * DCL's, and stays the labelled bridge it was (rd vms-786).
 *
 * Both must reach the mailbox in the order they were produced: an image's
 * last line before the "MMK____status=" record DCL writes after it (rd
 * vms-d26f). So every read of the pipe and every relayed message happens under
 * g_out_mtx, with the pipe's read end non-blocking, and DCL's own record first
 * relays whatever the pipe holds -- everything an image wrote before it --
 * under the same lock. A blocking read outside the lock could hold bytes it had
 * taken but not yet sent while DCL's record overtook them. The relay thread
 * therefore never blocks in read(): it drains, releases the lock, and sleeps a
 * moment (shorter while output flows, longer while it does not) -- the cost of
 * having no poll()/select() in the DECC$SHR surface the VMS-native link
 * resolves.
 *
 * vms-195's prompt/echo race (the demo's console: SYS$INPUT the terminal,
 * SYS$OUTPUT a mailbox): dcl_mbx_output_drain_sync relays, under the lock,
 * everything then in the pipe before DCL arms its read, so nothing written
 * before the read can arrive after the echo.
 */
static pthread_mutex_t g_out_mtx     = PTHREAD_MUTEX_INITIALIZER;
static int             g_out_stop    = 0;   /* shutdown: the relay thread leaves */
static uint16_t        g_out_qchan   = 0;   /* DCL's own $QIO channel to the mailbox */

/* Toggle O_NONBLOCK on a fd (fcntl is a DECC$SHR universal; poll is not). */
static void set_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return;
    int nf = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    (void)fcntl(fd, F_SETFL, nf);
}

/*
 * The writer's sink. Production writes each drained chunk to the SYS$OUTPUT
 * mailbox via the executive ($QIO IO$_WRITEVBLK). A hermetic unit test that has
 * no /dev/vms overrides this with a plain fd so the drain barrier itself can be
 * proven without the executive. Default (-1) = the real mailbox path.
 */
static int             g_out_sink_fd = -1; /* test-only sink; -1 = mailbox */

#ifdef DCL_MBX_TEST_HOOKS
/* Artificial writer lag (microseconds) so a hermetic test can make the
 * prompt/echo race deterministic. Zero (and absent) in every shipping build. */
static unsigned        g_out_sink_delay_us = 0;
#endif

static size_t          g_relayed = 0;       /* bytes relayed (the relay thread's pacing) */

/* Return 1 on success, 0 on failure -- the writer breaks its loop on 0. */
static int out_sink_write(const char *buf, size_t n)
{
    g_relayed += n;
#ifdef DCL_MBX_TEST_HOOKS
    if (g_out_sink_delay_us)
        usleep(g_out_sink_delay_us);
#endif
    if (g_out_sink_fd >= 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(g_out_sink_fd, buf + off, n - off);
            if (w <= 0) {
                if (w < 0 && errno == EINTR) continue;
                return 0;
            }
            off += (size_t)w;
        }
        return 1;
    }
    return (vms_kif_mbx_write(g_out_chan, buf, (uint32_t)n) & 1) ? 1 : 0;
}

/*
 * LINE-ORIENTED RELAY (vms-d26f). A Unix pipe carries no message boundaries,
 * so a single read() of DCL's stdout pipe can return bytes from TWO logically
 * distinct writes concatenated -- e.g. a foreign command's own diagnostic
 * print (its exit-time stdio flush) immediately followed by DCL's own
 * end-of-command status echo (a driven build's "MMK____status=" marker,
 * tests/corpus/tier3-mmk/build_target.c's send_cmd_and_wait/echo_ast
 * protocol). The OLD code forwarded each read() verbatim as ONE mailbox
 * message, so that concatenation became ONE message whose FIRST bytes were
 * the diagnostic print, not the marker -- and echo_ast only recognizes the
 * marker when it is the first thing in a message (a strncmp at offset 0),
 * exactly the real VMS record-oriented mailbox semantics (one $QIO WRITE is
 * one message) this relay exists to preserve. The marker then silently
 * fails to match, is echoed as ordinary output instead of consumed, and the
 * command that owned it never completes -- MMK's send_cmd_and_wait $HIBERs
 * forever waiting for a wake that was already missed, and the spawned DCL
 * has nothing further queued to send it (test_syssvc_mmk_build's "pristine
 * run" flake: LINK never runs because the PRECEDING command's own
 * completion marker was the one lost this way).
 *
 * The fix: buffer across read()s and cut a mailbox message at each '\n', so
 * a message boundary lines up with a DCL-level LINE boundary -- the same
 * granularity a real VMS SYS$OUTPUT write would have produced -- regardless
 * of how the underlying pipe happened to batch the bytes. A trailing PARTIAL
 * line (no '\n' yet) is held and flushed once the pipe runs momentarily dry
 * (line_relay_flush_partial), which is still needed for an interactive
 * prompt ("$ ", vms-195) that never ends in a newline and must still reach
 * the mailbox promptly via dcl_mbx_output_drain_sync.
 */
static char   g_linebuf[DCL_MBX_BUF];
static size_t g_linelen = 0;

/*
 * line_relay_feed - append `n` new bytes and emit one mailbox message per
 * complete ('\n'-terminated) line found. Returns 1 on success, 0 on a fatal
 * sink failure (caller treats exactly like a failed out_sink_write used to).
 * A single "line" that alone fills the buffer with no '\n' is flushed as-is
 * rather than grown unboundedly or left to stall a pathological producer.
 */
static int line_relay_feed(const char *data, size_t n)
{
    size_t off = 0;

    while (off < n) {
        size_t room = sizeof(g_linebuf) - g_linelen;
        size_t take = n - off;
        if (take > room)
            take = room;
        memcpy(g_linebuf + g_linelen, data + off, take);
        g_linelen += take;
        off += take;

        for (;;) {
            char *nl = memchr(g_linebuf, '\n', g_linelen);
            if (!nl)
                break;
            size_t reclen = (size_t)(nl - g_linebuf) + 1;
            if (!out_sink_write(g_linebuf, reclen))
                return 0;
            memmove(g_linebuf, g_linebuf + reclen, g_linelen - reclen);
            g_linelen -= reclen;
        }

        if (room == 0 && g_linelen == sizeof(g_linebuf)) {
            /* No newline anywhere in a full buffer: flush it whole rather
             * than stall waiting for one that may never come. */
            if (!out_sink_write(g_linebuf, g_linelen))
                return 0;
            g_linelen = 0;
        }
    }
    return 1;
}

/*
 * line_relay_flush_partial - forward a held-back partial line (no trailing
 * '\n' yet). Called once the pipe runs momentarily dry, so a newline-less
 * prompt still reaches the mailbox instead of waiting forever for one.
 */
static int line_relay_flush_partial(void)
{
    if (g_linelen == 0)
        return 1;
    int ok = out_sink_write(g_linebuf, g_linelen);
    g_linelen = 0;
    return ok;
}

/*
 * relay_locked - with g_out_mtx held: relay everything now in the image-output
 * pipe to the mailbox, a message per line, a held partial line last. Returns 1
 * while the pipe has a writer, 0 at end-of-file or a failed mailbox write.
 */
static int relay_locked(void)
{
    char buf[DCL_MBX_BUF];

    if (g_out_pipe_r < 0)
        return 0;
    for (;;) {
        ssize_t m = read(g_out_pipe_r, buf, sizeof(buf));
        if (m > 0) {
            if (!line_relay_feed(buf, (size_t)m))
                return 0;
            continue;
        }
        if (m < 0 && errno == EINTR)
            continue;
        int open = (m < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        return line_relay_flush_partial() && open;
    }
}

/*
 * writer_main - relay image output from the pipe to the SYS$OUTPUT mailbox
 * while DCL's main thread runs (an image's output must not wait for DCL's next
 * record). Leaves at end-of-file on the pipe or at shutdown.
 */
static void *writer_main(void *arg)
{
    (void)arg;
    long idle_ns = 1000000L;                    /* 1 ms */

    for (;;) {
        pthread_mutex_lock(&g_out_mtx);
        size_t before = g_relayed;
        int live = !g_out_stop && relay_locked();
        size_t moved = g_relayed - before;
        pthread_mutex_unlock(&g_out_mtx);
        if (!live)
            break;
        idle_ns = moved ? 1000000L : (idle_ns < 16000000L ? idle_ns * 2 : idle_ns);
        struct timespec ts = { 0, idle_ns };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/*
 * dcl_mbx_output_is_mailbox - has SYS$OUTPUT been bound to a mailbox?
 */
int dcl_mbx_output_is_mailbox(void)
{
    return g_writer_up;
}

/*
 * dcl_mbx_output_drain_sync - relay, now, everything in the image-output pipe
 * (and what this process's own stdio holds for it) to the mailbox. A no-op when
 * SYS$OUTPUT is not mailbox-bound. Never blocks on an empty pipe.
 */
void dcl_mbx_output_drain_sync(void)
{
    if (!g_writer_up)
        return;
    dcl_out_stdio_sync();
    pthread_mutex_lock(&g_out_mtx);
    (void)relay_locked();
    pthread_mutex_unlock(&g_out_mtx);
}

/*
 * dcl_mbx_put_record - one record of DCL's own SYS$OUTPUT to the mailbox: $QIO
 * IO$_WRITEVBLK on DCL's channel, after whatever an image wrote before it.
 * 0 when the mailbox took it, -1 when SYS$OUTPUT is not a bound mailbox or the
 * write failed.
 */
int dcl_mbx_put_record(const char *rec, size_t n)
{
    int rc = -1;

    if (!g_writer_up)
        return -1;
    pthread_mutex_lock(&g_out_mtx);
    (void)relay_locked();
#ifdef DCL_MBX_TEST_HOOKS
    if (g_out_sink_fd >= 0) {
        rc = out_sink_write(rec, n) ? 0 : -1;
        pthread_mutex_unlock(&g_out_mtx);
        return rc;
    }
#endif
    uint16_t iosb[4] = { 0, 0, 0, 0 };
    uint32_t st = sys$qiow(0, g_out_qchan, IO$_WRITEVBLK, iosb, NULL, 0,
                           (void *)rec, (uint32_t)n, 0, 0, 0, 0);
    if ((st & 1) && (iosb[0] & 1))
        rc = 0;
    pthread_mutex_unlock(&g_out_mtx);
    return rc;
}

int dcl_mbx_bind_std_streams(void)
{
    int bound = 0;
    char dev[256];

    /* SYS$OUTPUT: results leave here -- DCL's own records through its $QIO
     * channel (dcl_mbx_put_record), an image's output through the relay. */
    if (dcl_in_resolve_mailbox("SYS$OUTPUT", dev, sizeof(dev))) {
        uint32_t chan = 0;
        uint16_t qchan = 0;
        struct dsc$descriptor_s devdsc;
        vms_cstr_to_desc(&devdsc, dev);
        if (!(sys$assign(&devdsc, &qchan, 0, NULL, 0) & 1)) {
            qchan = 0;
        } else if (vms_kif_mbx_assign(dev, &chan) & 1) {
            int p[2];
            if (pipe(p) == 0) {
                dcl_out_flush();
                g_out_chan   = chan;
                g_out_qchan  = qchan;
                g_out_pipe_r = p[0];
                set_nonblock(g_out_pipe_r, 1);   /* read only under g_out_mtx */
                if (dup2(p[1], STDOUT_FILENO) >= 0 &&
                    pthread_create(&g_writer, NULL, writer_main, NULL) == 0) {
                    g_writer_up = 1;
                    bound |= DCL_MBX_BOUND_OUTPUT;
                    close(p[1]);
                    /* fd 1 is a pipe now, so stdio would fully buffer it;
                     * line-buffer so an in-process image's lines reach the
                     * relay as they are produced, not only at exit. */
                    dcl_out_line_buffered();
                } else {
                    close(p[0]);
                    close(p[1]);
                    g_out_pipe_r = -1;
                    g_out_chan   = 0;
                    g_out_qchan  = 0;
                    (void)vms_kif_dassgn(chan);
                    (void)sys$dassgn(qchan);
                }
            } else {
                (void)vms_kif_dassgn(chan);
                (void)sys$dassgn(qchan);
            }
        } else {
            (void)sys$dassgn(qchan);
        }
    }

    return bound;
}

void dcl_mbx_shutdown(void)
{
    if (g_writer_up) {
        /* DCL's last record, then whatever images left in the pipe, reach the
         * mailbox before this returns (the parent's last read must not race
         * process exit); the relay thread then leaves. A process still holding
         * the pipe's write end cannot keep DCL from exiting. */
        dcl_out_flush();
        dcl_out_stdio_sync();
        close(STDOUT_FILENO);
        pthread_mutex_lock(&g_out_mtx);
        (void)relay_locked();
        g_out_stop = 1;
        pthread_mutex_unlock(&g_out_mtx);
        pthread_join(g_writer, NULL);
        g_writer_up = 0;
        if (g_out_qchan) {
            (void)sys$dassgn(g_out_qchan);
            g_out_qchan = 0;
        }
        if (g_out_pipe_r >= 0) {
            close(g_out_pipe_r);
            g_out_pipe_r = -1;
        }
        if (g_out_chan) {
            (void)vms_kif_dassgn(g_out_chan);
            g_out_chan = 0;
        }
    }
}

#ifdef DCL_MBX_TEST_HOOKS
/*
 * Hermetic test hooks (vms-195). CI has no /dev/vms, so the mailbox bind path
 * cannot be exercised there. These start the SAME writer-thread + drain-barrier
 * machinery against a caller-provided plain fd sink (a pipe standing in for the
 * mailbox far end), so the ordering guarantee dcl_mbx_output_drain_sync() makes
 * -- the prompt is fully emitted before DCL reads input -- is provable without
 * the executive. Compiled only under -DDCL_MBX_TEST_HOOKS; absent from every
 * shipping build.
 */
int dcl_mbx__test_start_output(int sink_fd, int *out_write_fd)
{
    int p[2];
    if (pipe(p) != 0)
        return -1;
    g_out_pipe_r  = p[0];
    set_nonblock(g_out_pipe_r, 1);
    g_out_sink_fd = sink_fd;
    g_out_stop    = 0;
    g_linelen     = 0;      /* fresh line-relay state for this hermetic run */
    if (pthread_create(&g_writer, NULL, writer_main, NULL) != 0) {
        close(p[0]); close(p[1]);
        g_out_pipe_r  = -1;
        g_out_sink_fd = -1;
        return -1;
    }
    g_writer_up = 1;
    *out_write_fd = p[1];   /* the test writes DCL's "fd 1" output here */
    return 0;
}

void dcl_mbx__test_set_sink_delay(unsigned usec)
{
    g_out_sink_delay_us = usec;
}

void dcl_mbx__test_stop_output(void)
{
    /* The caller closes its write fd first; the writer then sees EOF. */
    if (g_writer_up) {
        pthread_join(g_writer, NULL);
        g_writer_up = 0;
    }
    if (g_out_pipe_r >= 0) { close(g_out_pipe_r); g_out_pipe_r = -1; }
    g_out_sink_fd = -1;
}
#endif /* DCL_MBX_TEST_HOOKS */
