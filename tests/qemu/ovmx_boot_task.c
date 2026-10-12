/*
 * ovmx_boot_task.c - the QEMU harness's boot task: every suite runs as a
 * process the executive created (rd vms-3fa, vms-9f32 design note PR3).
 *
 * On a booted OVMX system nothing runs that the executive did not create: the
 * boot task becomes SYSTEM, and every later process is $CREPRC'd by a VMS
 * process. The harness used to run each suite as a plain child of the busybox
 * PID 1, which registered itself with the executive on its first service call
 * -- the "fresh registration" path vms-2a5 removes. This program is the
 * harness's boot task instead: it becomes SYSTEM once (ESTABLISH_SYSTEM), then
 * creates each suite as a DETACHED process with SYSTEM's identity -- its own
 * job, as STARTUP starts independent work -- in $CREPRC's detached shape: an
 * intermediate forks the suite and holds it, VMS_IOCTL_CREPRC_PCB makes its
 * PCB, then it is released to exec. The PCB exists before the suite's first
 * instruction. Each suite being its own job keeps one suite's LNM$JOB names
 * out of the next (a subprocess of the boot task would share its job). The boot
 * task is a child subreaper only while a suite's intermediate exits, so the
 * suite becomes its child (it reaps it and reports its status) while anything
 * the suite itself orphans still goes to init.
 *
 * Protocol (init.sh holds both FIFOs open read-write):
 *   control FIFO, one line per suite:   RUN <program> <output-path>
 *   reply FIFO, two lines per suite:    PID <substrate-pid> <CREPRC_PCB status>
 *                                       RC <exit status as the shell reports it>
 * The program's stdout and stderr go to <output-path> (init.sh's per-suite
 * FIFO). A program whose PCB could not be made is not run: it is killed while
 * held and reported with its status, so a suite never runs as a process the
 * executive did not create.
 *
 * usage: ovmx_boot_task <control-fifo> <reply-fifo>
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ssdef.h"
#include "vms_kif.h"

static int write_all(int fd, const char *s)
{
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

/* One line from fd (no stdio: the control FIFO is shared with nothing). */
static int read_line(int fd, char *buf, size_t sz)
{
    size_t i = 0;
    while (i + 1 < sz) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return i ? (int)i : -1;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}

static struct vms_procinfo g_self;     /* the boot task's own (SYSTEM) row */

static void run_one(int reply, const char *prog, const char *out)
{
    char msg[160];
    int gp[2], hold[2];
    if (pipe(gp) != 0 || pipe(hold) != 0) {
        snprintf(msg, sizeof msg, "PID 0 %u\nRC 126\n", (unsigned)SS$_INSFMEM);
        (void)write_all(reply, msg);
        return;
    }
    (void)prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
    pid_t inter = fork();
    if (inter == 0) {
        char b = 0;
        close(gp[0]); close(hold[1]);
        pid_t g = fork();
        if (g == 0) {
            int fd = open(out, O_WRONLY);
            close(gp[1]);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > 2) close(fd);
            }
            /* Held until the boot task has made this task a VMS process. */
            while (read(hold[0], &b, 1) < 0 && errno == EINTR)
                ;
            close(hold[0]);
            if (b != 'g')
                _exit(125);
            execl(prog, prog, (char *)NULL);
            _exit(127);
        }
        int32_t gpid = (int32_t)g;
        (void)!write(gp[1], &gpid, sizeof gpid);
        close(gp[1]);
        while (read(hold[0], &b, 1) < 0 && errno == EINTR)   /* alive until released */
            ;
        _exit(0);
    }
    close(gp[1]); close(hold[0]);
    int32_t gpid = -1;
    ssize_t r;
    do { r = read(gp[0], &gpid, sizeof gpid); } while (r < 0 && errno == EINTR);
    close(gp[0]);
    uint32_t st = SS$_INSFMEM;
    if (inter > 0 && r == (ssize_t)sizeof gpid && gpid > 0)
        st = vms_kif_creprc_pcb((uint32_t)gpid, VMS_CREPRC_PCB_DETACHED, g_self.username,
                                g_self.uic, g_self.perm_privs, NULL);
    /* Released only as a VMS process; otherwise it exits held (125). */
    (void)write_all(hold[1], (st & 1) ? "gg" : "xx");
    close(hold[1]);
    if (inter > 0)
        while (waitpid(inter, NULL, 0) < 0 && errno == EINTR)
            ;
    /* The suite is this task's child now. Anything the suite orphans later (a
     * detached process it starts) goes on to init, as on a system with no
     * harness. */
    (void)prctl(PR_SET_CHILD_SUBREAPER, 0, 0, 0, 0);
    if (gpid <= 0) {
        snprintf(msg, sizeof msg, "PID 0 %u\nRC 126\n", (unsigned)st);
        (void)write_all(reply, msg);
        return;
    }
    snprintf(msg, sizeof msg, "PID %d %u\n", (int)gpid, (unsigned)st);
    (void)write_all(reply, msg);

    /* The intermediate is gone, so the suite is now this subreaper's child. */
    int ws = 0;
    while (waitpid((pid_t)gpid, &ws, 0) < 0 && errno == EINTR)
        ;
    int rc = WIFEXITED(ws) ? WEXITSTATUS(ws)
           : WIFSIGNALED(ws) ? 128 + WTERMSIG(ws) : 1;
    snprintf(msg, sizeof msg, "RC %d\n", rc);
    (void)write_all(reply, msg);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: ovmx_boot_task <control-fifo> <reply-fifo>\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    /* THE BOOT TASK BECOMES SYSTEM, once (the bootstrap exception of design
     * note 3.1: the one process the executive does not create through a
     * creator). Today its row comes from registration on open; vms-a2c makes
     * ESTABLISH_SYSTEM create it. */
    if (vms_kif_open() < 0) {
        fprintf(stderr, "%%OVMX-F-NOEXEC, boot task: /dev/vms not available\n");
        return 3;
    }
    uint32_t est = vms_kif_establish_system();
    if (!(est & 1)) {
        fprintf(stderr, "%%OVMX-F-NOSYSID, boot task: ESTABLISH_SYSTEM status %08X\n",
                (unsigned)est);
        return 3;
    }

    if (!(vms_kif_getjpi_self(&g_self) & 1)) {
        fprintf(stderr, "%%OVMX-F-NOSELF, boot task: cannot read its own row\n");
        return 3;
    }

    int ctl = open(argv[1], O_RDWR);
    int reply = open(argv[2], O_RDWR);
    if (ctl < 0 || reply < 0) {
        fprintf(stderr, "%%OVMX-F-NOFIFO, boot task: cannot open %s / %s\n", argv[1], argv[2]);
        return 3;
    }
    (void)write_all(reply, "READY\n");

    char line[1024];
    while (read_line(ctl, line, sizeof line) >= 0) {
        if (strcmp(line, "EXIT") == 0)
            break;
        if (strncmp(line, "RUN ", 4) != 0)
            continue;
        char *prog = line + 4;
        char *out = strchr(prog, ' ');
        if (!out)
            continue;
        *out++ = '\0';
        run_one(reply, prog, out);
    }
    return 0;
}
