/*
 * test_kmod_tt.c - the executive terminal CLASS DRIVER against a real vms.ko
 * (rd vms-f8c, epic vms-4eba; src/kernel-core/vms_tt.c + the Linux port
 * src/kernel/vms_tt_linux.c).
 *
 * WHAT THIS PROVES. A substrate pty is put under the executive's line
 * discipline and bound to a terminal unit the executive minted (RTAn:), and
 * then every property the keystroke oracle measured on a real VAX
 * (docs/oracle/keystroke/) is checked AT THE BYTE LEVEL on the pty's master
 * side -- what a person at that terminal would see:
 *
 *   - binding needs CMKRNL, decided by the executive (vms_prot.h), not by a
 *     substrate capability: with CMKRNL disabled the bind is SS$_NOPRIV;
 *   - a character typed while no read is outstanding is NOT echoed; it waits
 *     in the type-ahead buffer and is echoed when a read consumes it, AFTER
 *     that read's prompt (vms-d732, TA.WAIT);
 *   - IO$M_NOECHO reads echo nothing typed; IO$M_TIMED ends SS$_TIMEOUT;
 *     IO$M_PURGE discards type-ahead; ^X purges it as it is typed (TA.CTRLX);
 *     DELETE rubs out; ^Z terminates and the driver echoes *EXIT*;
 *   - $QIO writes go out through the driver byte for byte; read(2) on the
 *     line is a VMS terminal read too;
 *   - VMS_IOCTL_TT_SENSE reports a no-echo read as not echoing (what the
 *     DECnet CTERM host keys a Password: prompt on);
 *   - a terminal unit with no port answers SS$_DEVOFFLINE (Rule 9), and a
 *     read outstanding when the line goes away ends SS$_HANGUP.
 *
 * NEGATIVE CONTROLS live in tests/qemu/facility_defects.sh (tt-*): each
 * mutates vms_tt.c / vms_tt_linux.c and names this suite as the one that must
 * go red.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "vms_kif.h"

#define SS_NORMAL     1
#define SS_NOPRIV     0x24
#define SS_DEVOFFLINE 132
#define SS_DEVALLOC   2112
#define SS_TIMEOUT    556
#define SS_HANGUP     716

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
    fflush(stdout); \
} while (0)

static volatile int sigs_seen;
static void on_usr1(int sig) { (void)sig; sigs_seen++; }

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Everything the master side (the "screen") can read within `ms`. */
static size_t screen(int m, char *buf, size_t cap, int ms)
{
    size_t n = 0;

    for (;;) {
        struct pollfd p = { m, POLLIN, 0 };
        ssize_t k;

        if (poll(&p, 1, ms) <= 0)
            break;
        k = read(m, buf + n, cap - 1 - n);
        if (k <= 0)
            break;
        n += (size_t)k;
        ms = 100;                   /* drain what follows promptly */
        if (n >= cap - 1)
            break;
    }
    buf[n] = '\0';
    return n;
}

static void type(int m, const char *s)
{
    (void)!write(m, s, strlen(s));
}

/* A $QIO read run on a thread, so the test can type while it is outstanding. */
struct rd {
    pthread_t th;
    struct vms_tt_read_args a;
    char data[256];
    char prompt[64];
    uint32_t st;
};

static void *rd_main(void *arg)
{
    struct rd *r = arg;

    r->st = vms_kif_tt_read(&r->a);
    return NULL;
}

/* Every read is bounded (IO$M_TIMED): a driver that loses input must turn this
 * suite red, never hang it. BOUND is far above anything a working driver
 * needs here; a read the test means to time out passes its own, shorter one. */
#define BOUND 5

static void rd_start(struct rd *r, uint32_t chan, uint32_t flags,
                     const char *prompt, uint32_t timeout)
{
    if (!(flags & VMS_TT_RD_TIMED)) {
        flags |= VMS_TT_RD_TIMED;
        timeout = BOUND;
    }
    memset(r, 0, sizeof(*r));
    r->a.chan = chan;
    r->a.flags = flags;
    r->a.buf = (uint64_t)(uintptr_t)r->data;
    r->a.bufsz = sizeof(r->data) - 1;
    r->a.timeout = timeout;
    if (prompt) {
        snprintf(r->prompt, sizeof(r->prompt), "%s", prompt);
        r->a.prompt = (uint64_t)(uintptr_t)r->prompt;
        r->a.promptsz = (uint32_t)strlen(r->prompt);
    }
    pthread_create(&r->th, NULL, rd_main, r);
}

static void rd_wait(struct rd *r)
{
    pthread_join(r->th, NULL);
    r->data[r->a.count < sizeof(r->data) ? r->a.count : sizeof(r->data) - 1] = '\0';
}

int main(void)
{
    char devnam[32] = "", devnam2[32] = "", scr[512];
    uint32_t st, chan = 0, chan2 = 0, state = 0;
    uint64_t prev = 0;
    int m, s;
    struct rd r;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== test_kmod_tt: the executive terminal class driver (rd vms-f8c) ===\n");

    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m)) {
        printf("  FAIL: no pseudo-terminal\n");
        return 1;
    }
    s = open(ptsname(m), O_RDWR | O_NOCTTY);
    CHECK(s >= 0, "the pty's line side opens");

    st = vms_kif_terminal_create(ptsname(m) + 5, devnam, sizeof(devnam));
    CHECK((st & 1) && devnam[0], "the executive mints a terminal unit (RTAn:) for the pty");

    /* ---- Ruling 2: the bind is a VMS privilege, decided by the executive ---- */
    st = vms_kif_setprv(VMS_PRV_M_CMKRNL, 0, 0, &prev);
    CHECK(st & 1, "CMKRNL disabled for the negative control");
    st = vms_kif_tt_attach(s, devnam);
    /* negctl: tt-bind-privilege-ignored */
    CHECK(st == SS_NOPRIV, "binding a line to a terminal unit WITHOUT CMKRNL is SS$_NOPRIV");
    st = vms_kif_setprv(VMS_PRV_M_CMKRNL, 1, 0, &prev);
    CHECK(st & 1, "CMKRNL re-enabled");
    st = vms_kif_tt_attach(s, devnam);
    CHECK(st == SS_NORMAL, "with CMKRNL the line is bound to the unit (SS$_NORMAL)");
    st = vms_kif_tt_attach(s, devnam);
    CHECK(st == SS_DEVALLOC, "binding the same line twice is SS$_DEVALLOC");

    st = vms_kif_assign(devnam, &chan);
    CHECK(st & 1, "a channel is assigned to the unit");

    /* ---- TYPE-AHEAD: held unechoed, echoed when a read consumes it ---- */
    (void)screen(m, scr, sizeof(scr), 100);
    type(m, "abc");
    screen(m, scr, sizeof(scr), 300);
    /* negctl: tt-typeahead-echoed-on-receipt */
    CHECK(scr[0] == '\0', "characters typed with no read outstanding are NOT echoed (held in type-ahead)");
    rd_start(&r, chan, 0, "P> ", 0);
    msleep(200);
    type(m, "\r");
    rd_wait(&r);
    screen(m, scr, sizeof(scr), 300);
    /* negctl: tt-port-input-dropped */
    CHECK(r.st == SS_NORMAL && strcmp(r.data, "abc") == 0 && r.a.term == 13 && r.a.termsz == 1,
          "the read returns the type-ahead 'abc' and the RETURN terminator");
    CHECK(strcmp(scr, "P> abc\r\n") == 0,
          "the prompt is written FIRST, then the type-ahead is echoed as it is consumed, then CR LF");
    if (strcmp(scr, "P> abc\r\n") != 0)
        printf("      screen was [%s]\n", scr);

    /* ---- a signal does not end a read: it is suspended and resumed ---- */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;            /* no SA_RESTART: the kif re-enters */
        sigaction(SIGUSR1, &sa, NULL);
        rd_start(&r, chan, 0, "S> ", 0);
        msleep(200);
        type(m, "o");
        msleep(100);
        pthread_kill(r.th, SIGUSR1);
        msleep(200);
        type(m, "k\r");
        rd_wait(&r);
        screen(m, scr, sizeof(scr), 300);
        CHECK(sigs_seen == 1 && r.st == SS_NORMAL && strcmp(r.data, "ok") == 0,
              "a signal mid-read does not end it: the read resumes and returns the whole line 'ok'");
        CHECK(strcmp(scr, "S> ok\r\n") == 0,
              "the resumed read does not write its prompt a second time");
        if (strcmp(scr, "S> ok\r\n") != 0)
            printf("      screen was [%s]\n", scr);
    }

    /* ---- IO$M_NOECHO ---- */
    rd_start(&r, chan, VMS_TT_RD_NOECHO, NULL, 0);
    msleep(200);
    st = vms_kif_tt_sense(devnam, &state);
    CHECK((st & 1) && (state & VMS_TT_SENSE_READING) && !(state & VMS_TT_SENSE_ECHOING),
          "TT_SENSE reports a no-echo read outstanding as NOT echoing");
    type(m, "secret\r");
    rd_wait(&r);
    screen(m, scr, sizeof(scr), 300);
    CHECK(r.st == SS_NORMAL && strcmp(r.data, "secret") == 0, "a NOECHO read returns what was typed");
    CHECK(strstr(scr, "secret") == NULL, "a NOECHO read echoes none of it");

    /* ---- IO$M_TIMED ---- */
    rd_start(&r, chan, VMS_TT_RD_TIMED, NULL, 1);
    rd_wait(&r);
    CHECK(r.st == SS_TIMEOUT && r.a.count == 0, "a timed read with nothing typed ends SS$_TIMEOUT");

    /* ---- IO$M_PURGE ---- */
    type(m, "zzz");
    msleep(200);
    rd_start(&r, chan, VMS_TT_RD_PURGE, NULL, 0);
    msleep(200);
    type(m, "q\r");
    rd_wait(&r);
    screen(m, scr, sizeof(scr), 300);
    CHECK(r.st == SS_NORMAL && strcmp(r.data, "q") == 0, "IO$M_PURGE discards the type-ahead before reading");

    /* ---- ^X purges the type-ahead as it is typed (TA.CTRLX) ---- */
    type(m, "WRITE\r\x18");
    msleep(200);
    rd_start(&r, chan, VMS_TT_RD_TIMED, NULL, 1);
    rd_wait(&r);
    screen(m, scr, sizeof(scr), 200);
    CHECK(r.st == SS_TIMEOUT && r.a.count == 0, "^X purged the line typed ahead: the next read finds nothing");

    /* ---- DELETE rubs out ---- */
    rd_start(&r, chan, 0, NULL, 0);
    msleep(200);
    type(m, "ab\x7f" "c\r");
    rd_wait(&r);
    screen(m, scr, sizeof(scr), 300);
    CHECK(r.st == SS_NORMAL && strcmp(r.data, "ac") == 0, "DELETE rubs out the last character (data 'ac')");
    CHECK(strcmp(scr, "ab\b \bc\r\n") == 0, "DELETE is echoed as BS SP BS on a scope terminal");

    /* ---- TTSYNC: ^S / ^Q hold and release output, and are never data ---- */
    rd_start(&r, chan, 0, NULL, 0);
    msleep(200);
    type(m, "a\x13" "b\x11" "\r");
    rd_wait(&r);
    screen(m, scr, sizeof(scr), 300);
    CHECK(r.st == SS_NORMAL && strcmp(r.data, "ab") == 0,
          "^S and ^Q (TTSYNC) are flow control, not data: the read returns 'ab'");

    /* ---- a cursor key is one key, not data, and does not end the read ---- */
    rd_start(&r, chan, 0, NULL, 0);
    msleep(200);
    type(m, "ab\x1b[Ac\r");
    rd_wait(&r);
    screen(m, scr, sizeof(scr), 300);
    CHECK(r.st == SS_NORMAL && strcmp(r.data, "abc") == 0 && r.a.term == 13,
          "an escape sequence (up-arrow) neither ends the read nor lands in the line");

    /* ---- ^Z ---- */
    rd_start(&r, chan, 0, NULL, 0);
    msleep(200);
    type(m, "\x1a");
    rd_wait(&r);
    screen(m, scr, sizeof(scr), 300);
    CHECK(r.st == SS_NORMAL && r.a.term == 26 && r.a.count == 0, "^Z terminates the read");
    CHECK(strstr(scr, "*EXIT*") != NULL, "the driver echoes *EXIT* for ^Z");

    /* ---- $QIO write ---- */
    st = vms_kif_tt_write(chan, "xyz", 3);
    screen(m, scr, sizeof(scr), 300);
    CHECK((st & 1) && strcmp(scr, "xyz") == 0, "a $QIO write goes out through the driver byte for byte");

    /* ---- read(2) on the line is a terminal-driver read ---- */
    type(m, "hi\r");
    {
        char b[32] = "";
        struct pollfd p = { s, POLLIN, 0 };
        ssize_t k = -1;
        /* poll first: the line is readable once the type-ahead holds input */
        if (poll(&p, 1, BOUND * 1000) == 1)
            k = read(s, b, sizeof(b) - 1);
        b[k > 0 ? k : 0] = '\0';
        screen(m, scr, sizeof(scr), 300);
        CHECK(k == 3 && strcmp(b, "hi\n") == 0, "read(2) on the bound line returns the line with LF for the RETURN");
        CHECK(strcmp(scr, "hi\r\n") == 0, "read(2) echoes as it consumes, like any driver read");
    }

    /* ---- a unit with no port: SS$_DEVOFFLINE (Rule 9) ---- */
    st = vms_kif_terminal_create(ptsname(m) + 5, devnam2, sizeof(devnam2));
    if ((st & 1) && (vms_kif_assign(devnam2, &chan2) & 1)) {
        rd_start(&r, chan2, VMS_TT_RD_TIMED, NULL, 1);
        rd_wait(&r);
        CHECK(r.st == SS_DEVOFFLINE, "a terminal unit with no port attached answers SS$_DEVOFFLINE");
        (void)vms_kif_dassgn(chan2);
        (void)vms_kif_terminal_delete(devnam2);
    } else {
        CHECK(0, "a second, portless terminal unit is minted");
    }

    /* ---- hangup: an outstanding read ends SS$_HANGUP ---- */
    rd_start(&r, chan, 0, NULL, 0);
    msleep(200);
    close(m);
    rd_wait(&r);
    CHECK(r.st == SS_HANGUP, "a read outstanding when the line hangs up ends SS$_HANGUP");

    close(s);
    (void)vms_kif_dassgn(chan);
    (void)vms_kif_terminal_delete(devnam);

    /* ---- the console outlives its sessions ----
     * On Linux a session leader whose controlling terminal is the console
     * hangs the console up when it exits, and the console's line discipline is
     * closed and re-opened. OPA0: must stay bound through that, as a VMS
     * console does (STARTUP binds it once, at boot). */
    {
        int cfd = open("/dev/console", O_RDWR | O_NOCTTY);
        uint32_t ast = cfd >= 0 ? vms_kif_tt_attach(cfd, "OPA0:") : 0;
        pid_t c;

        CHECK(ast == SS_NORMAL || ast == SS_DEVALLOC, "the console is attached to OPA0: (as STARTUP attaches it)");
        c = fork();
        if (c == 0) {
            int fd;
            setsid();
            fd = open("/dev/console", O_RDWR);
            (void)ioctl(fd, TIOCSCTTY, 1);
            _exit(0);                       /* a session on the console ends */
        }
        waitpid(c, NULL, 0);
        msleep(300);
        state = 0;
        st = vms_kif_tt_sense("OPA0:", &state);
        /* negctl: tt-console-hangup-unbinds */
        CHECK((st & 1) && (state & VMS_TT_SENSE_BOUND),
              "OPA0: is still bound after a session whose controlling terminal it was ends (console hangup)");
    }

    printf("=== test_kmod_tt: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
