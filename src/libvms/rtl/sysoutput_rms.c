/*
 * sysoutput_rms.c - an image's SYS$OUTPUT / SYS$ERROR named by a VMS file is
 * written through RMS (rd vms-c89).
 *
 * WHAT VMS DOES. $ RUN /DETACHED /OUTPUT=SYS$MANAGER:NETACP.LOG image gives
 * the new process the logical names SYS$OUTPUT (and SYS$ERROR, from /ERROR)
 * in LNM$PROCESS. The process has no terminal; when its image starts, the
 * run-time library opens SYS$OUTPUT -- RMS $CREATE of a NEW version of that
 * file -- and every line the image writes becomes a record in it. TYPE reads
 * the file like any other.
 *
 * WHAT OVMX DOES. $CREPRC (src/libvms/syssvc/sys_process.c) defines those
 * logical names for a DETACHED process whose /OUTPUT or /ERROR names a VMS
 * file, and leaves the substrate stream on the null device. This image-start
 * routine (OVMX's LIB$INITIALIZE, a .init_array constructor) is the run-time
 * library's half: when SYS$OUTPUT translates to a file spec AND the stream is
 * the null device -- so nothing else already carries it -- it $CREATEs the
 * file through RMS (over the Files-11 ACP, rms_textfile_create) and points the
 * stream at a pipe whose reader $PUTs each line as one record
 * (rms_textfile_append_line, RMS $PUT at EOF). SYS$ERROR naming the same file
 * shares the record stream; naming another file gets its own.
 *
 * FAIL-HONEST (INV-6). If the file cannot be created through RMS (no ACP
 * volume, no such directory, no LIBVMSRMS in this image) the stream stays on
 * the null device -- the output is discarded, never written somewhere it was
 * not sent (a host file, a terminal). No POSIX fallback.
 *
 * WHICH IMAGES. This TU is compiled into an image as a primary object (as the
 * *_bind.c anchors are), so its constructor runs in that image only: DECNETD
 * (NETACP, the process STARTNET creates with /OUTPUT=SYS$MANAGER:NETACP.LOG)
 * and the facility suite that proves it. It needs LIBVMSRMS in the image's
 * link closure (rms_textfile reaches RMS through a weak seam).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rms_textfile.h"
#include "vms_kif.h"

#define SYSOUT_SPEC_MAX  256
#define SYSOUT_REC_MAX   512      /* a longer line is filed as several records */
#define SYSOUT_DRAIN_MS  2000     /* how long image exit waits for the last lines */

struct sysout_sink {
    int  rfd;                     /* pipe read end, -1 when unused */
    char spec[SYSOUT_SPEC_MAX];
    char line[SYSOUT_REC_MAX + 1];
    size_t len;
};

static struct sysout_sink sinks[2] = { { .rfd = -1 }, { .rfd = -1 } };
static int done_pipe[2] = { -1, -1 };
static int active = 0;

/* A name that is a FILE (directory part, or a name after the device), not a
 * bare device such as NL:, OPA0: or MBA3:, and not a process-permanent file's
 * ESC-NUL equivalence. */
static int is_file_spec(const char *v, uint16_t len)
{
    if (len == 0 || vms_lnm_is_ppf(v, len) || v[0] == '/')
        return 0;
    if (strpbrk(v, "[<"))
        return 1;
    const char *col = strrchr(v, ':');
    return col && col[1] != '\0';
}

static int translate_file(const char *lognam, char *out, size_t outsz)
{
    uint16_t len = 0;
    uint32_t attrs = 0;
    if (vms_kif_lnm_translate(VMS_LNM_TBL_PROCESS, lognam, 0, out,
                              (uint32_t)outsz, &len, &attrs, NULL) != 1)
        return 0;
    if (len >= outsz)
        len = (uint16_t)(outsz - 1);
    out[len] = '\0';
    return is_file_spec(out, len);
}

/* Is this descriptor the null device? */
static int is_null_device(int fd)
{
    struct stat s, n;
    if (fstat(fd, &s) != 0 || stat("/dev/null", &n) != 0)
        return 0;
    return S_ISCHR(s.st_mode) && s.st_rdev == n.st_rdev;
}

static void sink_put(struct sysout_sink *k)
{
    while (k->len > 0 && k->line[k->len - 1] == '\r')
        k->len--;
    k->line[k->len] = '\0';
    (void)rms_textfile_append_line(k->spec, k->line);
    k->len = 0;
}

static void sink_take(struct sysout_sink *k, const char *buf, ssize_t n)
{
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            sink_put(k);
            continue;
        }
        k->line[k->len++] = buf[i];
        if (k->len == SYSOUT_REC_MAX)
            sink_put(k);
    }
}

static void *sysout_reader(void *arg)
{
    (void)arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);   /* signals stay the image's */

    char buf[1024];
    for (;;) {
        struct pollfd p[2];
        int np = 0, idx[2];
        for (int i = 0; i < 2; i++)
            if (sinks[i].rfd >= 0) {
                p[np].fd = sinks[i].rfd;
                p[np].events = POLLIN;
                idx[np++] = i;
            }
        if (np == 0)
            break;
        if (poll(p, (nfds_t)np, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int j = 0; j < np; j++) {
            if (!(p[j].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            struct sysout_sink *k = &sinks[idx[j]];
            ssize_t n = read(k->rfd, buf, sizeof(buf));
            if (n > 0) {
                sink_take(k, buf, n);
            } else if (n == 0 || errno != EINTR) {
                if (k->len)
                    sink_put(k);
                close(k->rfd);
                k->rfd = -1;
            }
        }
    }
    if (done_pipe[1] >= 0)
        (void)write(done_pipe[1], "", 1);
    return NULL;
}

/* Image exit: flush the image's buffered output into the pipe, close the
 * write ends so the reader sees end-of-file, and give it a bounded time to
 * file the last lines (a child process may still hold a write end). */
static void sysout_atexit(void)
{
    if (!active)
        return;
    fflush(NULL);
    int nfd = open("/dev/null", O_WRONLY);
    if (nfd >= 0) {
        dup2(nfd, STDOUT_FILENO);
        dup2(nfd, STDERR_FILENO);
        close(nfd);
    }
    struct pollfd p = { .fd = done_pipe[0], .events = POLLIN };
    (void)poll(&p, 1, SYSOUT_DRAIN_MS);
}

/* Point `fd` at a new pipe whose read end is sink `k`. */
static int sink_open(struct sysout_sink *k, const char *spec, int fd)
{
    int pfd[2];
    if (rms_textfile_create(spec) != 0)
        return 0;                        /* fail honest: stays on NL: */
    if (pipe(pfd) != 0)
        return 0;
    (void)fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
    snprintf(k->spec, sizeof(k->spec), "%s", spec);
    k->rfd = pfd[0];
    dup2(pfd[1], fd);
    close(pfd[1]);
    return 1;
}

__attribute__((constructor))
static void ovmx_sysoutput_rms_init(void)
{
    char out[SYSOUT_SPEC_MAX], err[SYSOUT_SPEC_MAX];
    int want_out = is_null_device(STDOUT_FILENO) &&
                   translate_file("SYS$OUTPUT", out, sizeof(out));
    int want_err = is_null_device(STDERR_FILENO) &&
                   translate_file("SYS$ERROR", err, sizeof(err));
    if (!want_out && !want_err)
        return;

    int have_out = want_out && sink_open(&sinks[0], out, STDOUT_FILENO);
    if (want_err) {
        if (have_out && strcasecmp(err, out) == 0)
            dup2(STDOUT_FILENO, STDERR_FILENO);   /* one file, one stream */
        else
            (void)sink_open(&sinks[1], err, STDERR_FILENO);
    }
    if (sinks[0].rfd < 0 && sinks[1].rfd < 0)
        return;

    if (pipe(done_pipe) != 0) {
        done_pipe[0] = done_pipe[1] = -1;
    } else {
        (void)fcntl(done_pipe[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(done_pipe[1], F_SETFD, FD_CLOEXEC);
    }
    pthread_t t;
    if (pthread_create(&t, NULL, sysout_reader, NULL) != 0)
        return;
    pthread_detach(t);
    active = 1;
    atexit(sysout_atexit);
    /* The streams are pipes now, not terminals: line-buffer them so each
     * line reaches its record as it is written, as on VMS. */
    setvbuf(stdout, NULL, _IOLBF, 0);
}
