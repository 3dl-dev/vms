/*
 * CRTLSHR.C - a C program linked against DECC$SHR, run from DCL by
 * tests/qemu/test_syssvc_crtl_shr.c (rd vms-569).
 *
 * It exercises the C RTL layer the x86_64 DECC$SHR turns on when the image
 * activator starts an image (src/vmsrms/crtl_rms_fd.c ovmx_crtl_fd_install,
 * called from src/imgact/imgact.c crtl_image_init) and reports what it saw,
 * one "CRTLSHR <key> ..." line per fact, through its own stdout -- which, on
 * OpenVMS and here, is SYS$OUTPUT: the parent reads the report off the
 * terminal SYS$OUTPUT names, not off the substrate line underneath.
 *
 *   GETNAME1  getname(1): the device descriptor 1 is open on.
 *   PIPE      getname() of pipe()'s read end. When it is a mailbox, the
 *             program waits for ONE message the parent writes into that
 *             mailbox with $QIO from its own process (PIPE-READ).
 *   PROC      open("/proc/self/status"): the return value and errno.
 *   TIME      time().
 *   ALARM     alarm(1) + pause(): whether the SIGALRM action ran, and whether
 *             the frame handler established here saw the condition the C RTL
 *             signals for a timer AST, SS$_ASTFLT.
 *   FORK      fork() with a pipe(): the child asks $GETJPI for its own PID and
 *             writes "<PID hex>,<status>" into the pipe; the parent reports
 *             what it read, then its own PID and $GETJPI status. A child that
 *             is a subprocess has a VMS PID of its own and still holds the pipe.
 *
 * Built by tests/qemu/crtlshr/mk_crtlshr_native.sh: cc + LINK.EXE --executable
 * --use DECC$SHR --use LIBVMS$SHR (lib$establish).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* DEC C getname() and alarm(), under the names DEC C binds them to (DECC$SHR
 * exports these two only as decc$getname / decc$alarm). */
extern char *decc_getname(int fd, char *buf, ...) __asm__("decc$getname");
extern unsigned int decc_alarm(unsigned int seconds) __asm__("decc$alarm");
extern void *lib$establish(void *handler);
extern unsigned int sys$getjpiw(unsigned int efn, unsigned int *pidadr, void *prcnam,
                                void *itmlst, void *iosb, void *astadr, unsigned int astprm);

#define SS_ASTFLT 1036u                  /* SSDEF SS$_ASTFLT (%X0000040C): %SYSTEM-F-ASTFLT */
#define SS_RESIGNAL 0x00000918u
#define JPI_PID 0x0319u                  /* prcdef.h JPI$_PID */

/* $GETJPI JPI$_PID of this process */
static unsigned int my_pid(unsigned int *pid)
{
    struct { unsigned short len, code; void *buf; unsigned short *ret; } il[2];
    unsigned short iosb[4] = { 0, 0, 0, 0 };
    il[0].len = 4; il[0].code = JPI_PID; il[0].buf = pid; il[0].ret = 0;
    il[1].len = 0; il[1].code = 0; il[1].buf = 0; il[1].ret = 0;
    *pid = 0;
    unsigned int st = sys$getjpiw(0, 0, 0, il, iosb, 0, 0);
    return (st & 1) ? iosb[0] : st;
}

struct sigarr { uint32_t args, name; };

static volatile int frame_astflt, alarm_ran;

static uint32_t frame_handler(struct sigarr *sa, void *ma)
{
    (void)ma;
    if (sa && (sa->name & 0x0FFFFFF8u) == (SS_ASTFLT & 0x0FFFFFF8u))
        frame_astflt = 1;
    return SS_RESIGNAL;
}

static void on_alarm(int s)
{
    (void)s;
    alarm_ran = 1;
}

static void say(const char *fmt, const char *a, long b, long c)
{
    printf(fmt, a, b, c);
    fflush(stdout);
}

int main(void)
{
    char nm[256];

    lib$establish((void *)frame_handler);

    /* descriptor 1 */
    nm[0] = '\0';
    say("CRTLSHR GETNAME1 %s %ld %ld\n", decc_getname(1, nm, 1) ? nm : "(none)", 0, 0);

    /* pipe(): a mailbox the parent can write into from its own process */
    {
        int p[2] = { -1, -1 };
        int pr = pipe(p);
        nm[0] = '\0';
        if (pr != 0 || !decc_getname(p[0], nm, 1))
            strcpy(nm, "(none)");
        say("CRTLSHR PIPE %s %ld %ld\n", nm, (long)pr, (long)errno);
        if (strncmp(nm + (nm[0] == '_'), "MBA", 3) == 0) {
            char b[64];
            ssize_t r = read(p[0], b, sizeof b - 1);
            b[r > 0 ? r : 0] = '\0';
            say("CRTLSHR PIPE-READ %s %ld %ld\n", r > 0 ? b : "(nothing)", (long)r, 0);
        }
        if (p[0] >= 0) close(p[0]);
        if (p[1] >= 0) close(p[1]);
    }

    /* a kernel namespace is not a C RTL file */
    {
        errno = 0;
        int fd = open("/proc/self/status", O_RDONLY);
        say("CRTLSHR PROC %s %ld %ld\n", fd < 0 ? "refused" : "opened", (long)fd, (long)errno);
        if (fd >= 0) close(fd);
    }

    /* the clock */
    say("CRTLSHR TIME %s %ld %ld\n", "unix", (long)time(NULL), 0);

    /* alarm(): a timer AST */
    {
        signal(SIGALRM, on_alarm);
        decc_alarm(1);
        pause();
        say("CRTLSHR ALARM %s %ld %ld\n", "ran", (long)alarm_ran, (long)frame_astflt);
    }

    /* fork(): the child is a VMS subprocess and holds the pipe */
    {
        int p[2] = { -1, -1 };
        unsigned int me = 0, st = my_pid(&me);
        char got[64] = "(none)";
        if (pipe(p) == 0) {
            pid_t c = fork();
            if (c == 0) {
                unsigned int cp = 0, cs = my_pid(&cp);
                char m[32];
                int k = snprintf(m, sizeof m, "%08X,%u", cp, cs);
                close(p[0]);
                (void)write(p[1], m, (size_t)k);
                close(p[1]);
                _exit(0);
            }
            close(p[1]);
            if (c > 0) {
                ssize_t r = read(p[0], got, sizeof got - 1);
                got[r > 0 ? r : 0] = '\0';
                if (r <= 0)
                    strcpy(got, "(nothing)");
            } else {
                strcpy(got, "(nofork)");
            }
            close(p[0]);
        }
        say("CRTLSHR FORK %s %ld %ld\n", got, (long)me, (long)st);
    }

    say("CRTLSHR DONE %s %ld %ld\n", "end", 0, 0);
    return 0;
}
