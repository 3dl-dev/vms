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
#define SS_ABORT      44

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

static void rd_start_n(struct rd *r, uint32_t chan, uint32_t flags,
                       const char *prompt, size_t plen, uint32_t timeout);

static void rd_start(struct rd *r, uint32_t chan, uint32_t flags,
                     const char *prompt, uint32_t timeout)
{
    rd_start_n(r, chan, flags, prompt, prompt ? strlen(prompt) : 0, timeout);
}

static void rd_start_n(struct rd *r, uint32_t chan, uint32_t flags,
                       const char *prompt, size_t plen, uint32_t timeout)
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
    if (prompt && plen) {
        if (plen > sizeof(r->prompt))
            plen = sizeof(r->prompt);
        memcpy(r->prompt, prompt, plen);
        r->a.prompt = (uint64_t)(uintptr_t)r->prompt;
        r->a.promptsz = (uint32_t)plen;
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

    /* ---- carriage control (rd vms-fc4): records and prompts ----
     * A program's '\n'-terminated output is a run of RECORDS: a new line
     * before each, a carriage return after, the line feed owed. A read that
     * echoes pays the owed line feed before its prompt; DCL's prompt starts
     * with a new line and a fill NUL. Expected bytes are the VAX V7.3
     * console's for the same sequence (probes CC.EMPTY W2, CC.MIX). */
    rd_start(&r, chan, 0, NULL, 0);
    msleep(200);
    type(m, "\r");                          /* a command line, echoed: fresh line */
    rd_wait(&r);
    (void)screen(m, scr, sizeof(scr), 300);
    (void)!write(s, "A\nB\n", 4);
    screen(m, scr, sizeof(scr), 300);
    /* negctl: tt-newline-ignores-cursor */
    /* negctl-knockon: tt-port-input-dropped */
    CHECK(strcmp(scr, "\rA\r\nB\r") == 0,
          "two records after an echoed RETURN: <CR>A<CR> <LF>B<CR> (the line feed stays owed)");
    if (strcmp(scr, "\rA\r\nB\r") != 0)
        printf("      screen was [%s]\n", scr);
    rd_start_n(&r, chan, 0, "\r\n\0$ ", 5, 0);   /* the NUL is part of it */
    msleep(200);
    screen(m, scr, sizeof(scr), 300);
    /* negctl: tt-owed-linefeed-unpaid */
    /* negctl-knockon: tt-newline-ignores-cursor */
    CHECK(memcmp(scr, "\n\r", 2) == 0 && scr[2] == '\0' && memcmp(scr + 3, "$ ", 2) == 0,
          "DCL's prompt after a record: the owed <LF>, then <CR><NUL>$ ");
    type(m, "\r");
    rd_wait(&r);
    (void)screen(m, scr, sizeof(scr), 300);
    rd_start_n(&r, chan, 0, "\r\n\0$ ", 5, 0);
    msleep(200);
    screen(m, scr, sizeof(scr), 300);
    /* negctl-knockon: tt-newline-ignores-cursor */
    /* negctl-knockon: tt-port-input-dropped */
    CHECK(scr[0] == '\r' && scr[1] == '\0' && memcmp(scr + 2, "$ ", 2) == 0,
          "DCL's prompt after an echoed RETURN: <CR><NUL>$ (the line already advanced)");
    type(m, "\r");
    rd_wait(&r);
    (void)screen(m, scr, sizeof(scr), 300);

    /* ---- $QIO write ---- */
    st = vms_kif_tt_write(chan, "xyz", 3);
    screen(m, scr, sizeof(scr), 300);
    CHECK((st & 1) && strcmp(scr, "xyz") == 0, "a $QIO write goes out through the driver byte for byte");
    /* a write with P4 " " carriage control is one record: a new line (mid-
     * line here: CR LF), the text, a CR with the line feed owed (semantic
     * oracle TT.WRITE, rd vms-fc4) */
    st = vms_kif_tt_write_record(chan, "abc", 3);
    screen(m, scr, sizeof(scr), 300);
    CHECK((st & 1) && strcmp(scr, "\r\nabc\r") == 0,
          "a $QIO write with P4 carriage control \" \" is one record: <CR><LF>abc<CR>");
    (void)!write(s, "d\n", 2);
    screen(m, scr, sizeof(scr), 300);
    CHECK(strcmp(scr, "\nd\r") == 0, "the record after it pays the owed line feed: <LF>d<CR>");
    (void)vms_kif_tt_write(chan, "\r\n", 2);      /* back to a fresh line */
    (void)screen(m, scr, sizeof(scr), 300);

    /* ---- out-of-band ASTs (rd vms-f0fb) ----
     * Expected bytes and behaviour: VAX V7.3 console probes OB.PROMPT/OB.READ
     * (docs/oracle/keystroke-probes/OB.*). AST routine addresses are opaque
     * values to the executive; vms_kif_deliverast hands back what was armed. */
    {
        uint64_t aa = 0, ap = 0;
        uint8_t am = 0;
        int got;

        while (vms_kif_deliverast(&aa, &ap, &am) == 0)
            ;                                   /* start with an empty queue */

        /* no AST armed: CTRL/Y interrupts nothing -- *INTERRUPT* is shown and
         * the character ends the read like a terminator, its line intact */
        rd_start(&r, chan, 0, NULL, 0);
        msleep(200);
        type(m, "AB\x19");
        rd_wait(&r);
        screen(m, scr, sizeof(scr), 300);
        /* negctl: tt-ctrly-not-shown */
        CHECK(strcmp(scr, "AB\r\n*INTERRUPT*\r\n") == 0,
              "CTRL/Y is shown as <CR><LF>*INTERRUPT*<CR><LF> by the driver");
        CHECK(r.st == SS_NORMAL && strcmp(r.data, "AB") == 0 && r.a.term == 0x19,
              "with no CTRL/Y AST armed, CTRL/Y ends the read with its line ('AB'), not SS$_ABORT");

        /* CTRL/Y AST armed: the read ends SS$_ABORT, the AST is queued in the
         * executive for this process, and it is spent */
        st = vms_kif_tt_oobast(chan, VMS_TT_OOB_CTRLY, 0x1234, 77, 0, 3);
        CHECK(st & 1, "IO$M_CTRLYAST arms a CTRL/Y AST on the channel");
        rd_start_n(&r, chan, 0, "\r\n\0$ ", 5, 0);
        msleep(200);
        type(m, "AB\x19");
        rd_wait(&r);
        screen(m, scr, sizeof(scr), 300);
        CHECK(r.st == SS_ABORT, "with the CTRL/Y AST armed, CTRL/Y ends the read SS$_ABORT (the line is gone)");
        got = vms_kif_deliverast(&aa, &ap, &am) == 0;
        /* negctl: tt-oob-ast-not-queued */
        CHECK(got && aa == 0x1234 && ap == 77 && am == 3,
              "the CTRL/Y AST is in the executive's queue for this process (routine 1234, parameter 77, user mode)");
        CHECK(vms_kif_deliverast(&aa, &ap, &am) != 0, "exactly one AST was queued");

        /* spent: the next CTRL/Y finds no AST */
        rd_start(&r, chan, 0, NULL, 0);
        msleep(200);
        type(m, "\x19");
        rd_wait(&r);
        (void)screen(m, scr, sizeof(scr), 300);
        CHECK(r.st == SS_NORMAL && vms_kif_deliverast(&aa, &ap, &am) != 0,
              "a CTRL/Y AST fires once: the next CTRL/Y queues nothing until it is re-armed");

        /* CTRL/C with no CTRL/C AST fires the CTRL/Y AST */
        (void)vms_kif_tt_oobast(chan, VMS_TT_OOB_CTRLY, 0x2222, 5, 0, 3);
        rd_start(&r, chan, 0, NULL, 0);
        msleep(200);
        type(m, "\x03");
        rd_wait(&r);
        (void)screen(m, scr, sizeof(scr), 300);
        got = vms_kif_deliverast(&aa, &ap, &am) == 0;
        /* negctl-knockon: tt-oob-ast-not-queued */
        CHECK(r.st == SS_ABORT && got && aa == 0x2222,
              "CTRL/C with no CTRL/C AST armed fires the CTRL/Y AST");

        /* an OUT-OF-BAND character (CTRL/T): the read stays outstanding, the
         * reader is let go to deliver the AST (character as parameter), output
         * written meanwhile breaks through, and the read resumes with its line
         * shown again (OB.PROMPT T2) */
        st = vms_kif_tt_oobast(chan, VMS_TT_OOB_OUTBAND, 0x3333, 0, 1u << 0x14, 3);
        CHECK(st & 1, "IO$M_OUTBAND arms an out-of-band AST for CTRL/T");
        {
            struct vms_tt_read_args ra;
            char data[64];
            uint32_t rs;

            memset(&ra, 0, sizeof ra);
            ra.chan = chan;
            ra.flags = VMS_TT_RD_TIMED;
            ra.timeout = BOUND;
            ra.buf = (uint64_t)(uintptr_t)data;
            ra.bufsz = sizeof data - 1;
            ra.prompt = (uint64_t)(uintptr_t)"\r\n\0$ ";
            ra.promptsz = 5;
            {
                pid_t kid = fork();
                if (kid == 0) {                 /* the keyboard */
                    msleep(300);
                    type(m, "ABC\x14");
                    _exit(0);
                }
                rs = vms_kif_tt_read(&ra);
                waitpid(kid, NULL, 0);
            }
            got = vms_kif_deliverast(&aa, &ap, &am) == 0;
            /* negctl: tt-outband-ends-read */
            /* negctl-knockon: tt-oob-ast-not-queued */
            CHECK((ra.oflags & VMS_TT_RDO_ASTPEND) && got && aa == 0x3333 && ap == 0x14,
                  "CTRL/T lets the reader go with the read still outstanding (ASTPEND), its AST carrying the character");
            (void)screen(m, scr, sizeof(scr), 300);
            (void)!write(s, "STATUS\n", 7);      /* the AST's status line */
            screen(m, scr, sizeof(scr), 300);
            /* negctl: tt-breakthrough-no-redisplay */
            /* negctl-knockon: tt-oob-ast-not-queued */
            /* negctl-knockon: tt-outband-ends-read */
            CHECK(memcmp(scr, "\r\nSTATUS\r\n\r\0$ ABC", 17) == 0,
                  "output during the read breaks through and the read is shown again: <CR><LF>STATUS<CR><LF><CR><NUL>$ ABC");
            if (memcmp(scr, "\r\nSTATUS\r\n\r\0$ ABC", 17) != 0) {
                printf("      screen was [");
                for (size_t q = 0; q < 40 && (scr[q] || q < 22); q++)
                    printf(scr[q] < 32 ? "<%02X>" : "%c", (unsigned char)scr[q]);
                printf("]\n");
            }
            {
                pid_t kid = fork();
                if (kid == 0) {
                    msleep(300);
                    type(m, "D\r");
                    _exit(0);
                }
                ra.oflags = 0;
                rs = vms_kif_tt_read(&ra);
                waitpid(kid, NULL, 0);
            }
            /* negctl-knockon: tt-oob-ast-not-queued */
            /* negctl-knockon: tt-outband-ends-read */
            CHECK(rs == SS_NORMAL && ra.oflags == 0 && ra.count == 4 && memcmp(data, "ABCD", 4) == 0,
                  "the same read resumes and completes with the whole line 'ABCD'");
            (void)screen(m, scr, sizeof(scr), 300);
        }
        (void)vms_kif_tt_oobast(chan, VMS_TT_OOB_OUTBAND, 0, 0, 0, 3);
        (void)vms_kif_tt_oobast(chan, VMS_TT_OOB_CTRLY, 0, 0, 0, 3);

        /* the arming channel's deassign ends its AST */
        {
            uint32_t chan3 = 0;
            if (vms_kif_assign(devnam, &chan3) & 1) {
                (void)vms_kif_tt_oobast(chan3, VMS_TT_OOB_CTRLY, 0x4444, 0, 0, 3);
                (void)vms_kif_dassgn(chan3);
            }
            rd_start(&r, chan, 0, NULL, 0);
            msleep(200);
            type(m, "\x19");
            rd_wait(&r);
            (void)screen(m, scr, sizeof(scr), 300);
            /* negctl: tt-oob-survives-deassign */
            CHECK(chan3 && r.st == SS_NORMAL && vms_kif_deliverast(&aa, &ap, &am) != 0,
                  "an AST armed through a channel ends when that channel is deassigned");
        }
    }

    /* ---- CTRL/O discards output (OOB.CTRLO, OB.PROMPT O1) ---- */
    (void)screen(m, scr, sizeof(scr), 100);
    (void)!write(s, "A\n", 2);                 /* a record: its line feed owed */
    (void)screen(m, scr, sizeof(scr), 300);
    type(m, "\x0f");
    msleep(200);
    screen(m, scr, sizeof(scr), 300);
    CHECK(strcmp(scr, "\n*OUTPUT OFF*\r\n") == 0,
          "CTRL/O while output runs shows <LF>*OUTPUT OFF*<CR><LF> (the owed line feed first)");
    (void)!write(s, "B\n", 2);
    screen(m, scr, sizeof(scr), 300);
    /* negctl: tt-ctrlo-not-discarding */
    CHECK(scr[0] == '\0', "output written while CTRL/O is on is discarded");
    type(m, "\x0f");
    msleep(200);                                /* the receive path runs first */
    (void)!write(s, "C\n", 2);
    screen(m, scr, sizeof(scr), 300);
    /* negctl-knockon: tt-ctrlo-not-discarding */
    CHECK(strcmp(scr, "*OUTPUT ON*\r\nC\r") == 0,
          "a second CTRL/O shows *OUTPUT ON*<CR><LF> and output resumes on that line (OOB.CTRLO OW2)");
    rd_start(&r, chan, 0, NULL, 0);
    msleep(200);
    type(m, "a\x0f" "b\r");
    rd_wait(&r);
    screen(m, scr, sizeof(scr), 300);
    CHECK(r.st == SS_NORMAL && strcmp(r.data, "ab") == 0 && strcmp(scr, "\nab\r\n") == 0,
          "CTRL/O during a read is neither data nor shown, and does not end the read");
    if (strcmp(scr, "\nab\r\n") != 0)
        printf("      screen was [%s] data [%s] st %u\n", scr, r.data, r.st);

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
        /* a process blocked in read(2) on the console while the session
         * ends: the hangup must end ITS read (it holds the line discipline
         * the hangup re-opens), not wedge the exiting session */
        pid_t rdr = fork();
        if (rdr == 0) {
            char b[16];
            int rfd = open("/dev/console", O_RDONLY | O_NOCTTY);
            ssize_t k = read(rfd, b, sizeof(b));
            _exit(k == 0 ? 0 : 3);
        }
        msleep(300);
        c = fork();
        if (c == 0) {
            int fd;
            setsid();
            fd = open("/dev/console", O_RDWR);
            (void)ioctl(fd, TIOCSCTTY, 1);
            _exit(0);                       /* a session on the console ends */
        }
        {
            int w, rst = -1, cst = -1;
            for (w = 0; w < 50; w++) {             /* 5 s */
                if (cst < 0 && waitpid(c, &cst, WNOHANG) != c)
                    cst = -1;
                if (rst < 0 && waitpid(rdr, &rst, WNOHANG) != rdr)
                    rst = -1;
                if (cst >= 0 && rst >= 0)
                    break;
                msleep(100);
            }
            CHECK(cst >= 0, "the session leader's exit completes (the console hangup does not wedge on a blocked reader)");
            CHECK(rst >= 0 && WIFEXITED(rst) && WEXITSTATUS(rst) == 0,
                  "a read(2) blocked on the console ends with end-of-file when the session hangs up");
            if (cst < 0) kill(c, SIGKILL);
            if (rst < 0) kill(rdr, SIGKILL);
        }
        msleep(300);
        state = 0;
        st = vms_kif_tt_sense("OPA0:", &state);
        /* negctl: tt-console-hangup-unbinds */
        CHECK((st & 1) && (state & VMS_TT_SENSE_BOUND),
              "OPA0: is still bound after a session whose controlling terminal it was ends (console hangup)");
        /* Give the console line back to the substrate's own discipline when
         * this suite attached it (rd vms-0125): a console left on the
         * executive's discipline holds a module reference of its own, and
         * test_syssvc_pin must find the descriptor the ONLY thing pinning
         * vms.ko. A fresh descriptor: the hangup above killed cfd's. */
        if (ast == SS_NORMAL) {
            int gfd = open("/dev/console", O_RDWR | O_NOCTTY), n_tty = 0;
            if (gfd >= 0) {
                (void)ioctl(gfd, TIOCSETD, &n_tty);
                close(gfd);
            }
        }
    }

    printf("=== test_kmod_tt: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
