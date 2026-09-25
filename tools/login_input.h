/*
 * login_input.h - the LOGINOUT prompt reader: one line, with an IDLE DEADLINE,
 * and a substrate-independent type-ahead drain (vms-3e9).
 *
 * WHY THIS EXISTS
 * ---------------
 * LOGINOUT's Username:/Password: prompts used to be plain fgets() calls with no
 * deadline of any kind. A console left sitting at "Username:" -- or a session
 * whose operator walked away half way through a password -- held that session
 * open forever. OpenVMS does not: LOGINOUT's read of each login prompt is
 * bounded, and when it expires LOGINOUT says so on the terminal and ends the
 * login.
 *
 * WHAT VMS DOES, MEASURED (rd vms-29e, real OpenVMS VAX V7.3 console, isolated
 * vaxlab replica, 2026-09-25; captures under
 * tests/lab/captures/vms-29e-login-read-oracle-20260925/):
 *
 *   - THE DEADLINE IS LGI_RETRY_TMO, NOT LGI_PWD_TMO. With the defaults
 *     (LGI_PWD_TMO 30, LGI_RETRY_TMO 20) both the Username: and the Password:
 *     read expired after ~20 s. Raising LGI_RETRY_TMO to 60 moved BOTH to
 *     ~60 s; raising LGI_PWD_TMO to 60 (RETRY_TMO back at 20) left both at
 *     ~20 s. The deadline below is therefore the LGI_RETRY_TMO default. (This
 *     header used to cite LGI_PWD_TMO and use 30 s -- a documentation reading
 *     the oracle does not bear out.)
 *
 *   - IT IS NOT SILENT. On expiry the terminal shows, after the prompt and
 *     whatever had been typed on it:
 *
 *         Username: SYS<CR><LF>
 *         Error reading command input<CR><LF>
 *         Timeout period expired<CR><LF>
 *
 *     and the same two lines after a Password: prompt (whose RETURN-less,
 *     echo-off line is ended by the one CR/LF). This header used to drop the
 *     session with NOTHING printed, on the belief that VMS prints no farewell;
 *     the oracle shows it does. That silence is what made rd vms-29e look like
 *     "the executive's operator lines abandon LOGINOUT's read": a console left
 *     at Username: for minutes had long since timed out without a word, so the
 *     SYSTEM a harness (or a demo visitor) typed next only woke a fresh session
 *     -- whose banner and empty Username: prompt followed, exactly as they do
 *     on the real VAX in the same situation (the wake keystroke line is not a
 *     username there either).
 *
 *   - A BROADCAST DOES NOT END THE READ. A REPLY/TERMINAL message and an OPCOM
 *     message arriving while "Username: SYS" was half typed were displayed and
 *     the read carried on; the typed rest completed the username. (VMS also
 *     redisplays the prompt and the typed characters after the message; OVMX's
 *     $BRKTHRU is not yet executive-mediated and cannot: rd vms-42f.)
 *
 * OVMX HAS NO LGI_* SYSGEN PARAMETER YET, and this header does not invent a
 * parameter database entry to look one up in (that would be a facade: SYSGEN
 * would not list it and SET/SHOW could not change it). The deadline is
 * therefore OVMX's own compiled-in default, DISCLOSED as such -- its VALUE is
 * the measured LGI_RETRY_TMO default so the behaviour a user sees matches VMS.
 * Making it a real, settable SYSGEN parameter is tracked separately; when it
 * lands, LOGIN_INPUT_TIMEOUT_SEC becomes that parameter's default and this
 * comment goes away.
 *
 * HEADER-ONLY, static inline, BY DESIGN. LOGINOUT.EXE is built four different
 * ways (host CMake, the netbsd-vax cross build, the Linux-Alpha cross build and
 * the VMS-native self-host link), and each one names the translation units it
 * compiles. A new .c file therefore has to be registered in every one of them
 * or a rail goes red on an undefined symbol ("new X -> N places"); a new header
 * has to be registered nowhere. There is exactly one caller today
 * (tools/vms_login.c) and a unit test (tests/tools/test_loginout_display.c),
 * so there is no duplicated code to speak of.
 *
 * NO stdio ON THE READ PATH. tools/vms_login.c deliberately runs stdin
 * unbuffered (setvbuf _IONBF) so that input typed ahead stays in the terminal's
 * own queue and survives the execl() into DCL. These helpers read the
 * descriptor directly for the same reason -- and because a deadline cannot be
 * applied to fgets() at all.
 *
 * Clean-room (CLAUDE.md Rule 8): behaviour from public OpenVMS documentation;
 * the poll()/read() mechanics are OVMX's own and are not claimed as VMS's.
 */
#ifndef LOGIN_INPUT_H
#define LOGIN_INPUT_H

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*
 * The idle deadline applied to each login prompt, in seconds: the OpenVMS
 * LGI_RETRY_TMO default, which the V7.3 oracle shows governs both login reads
 * (see the header note above, and for why this is a compiled-in constant
 * rather than a fabricated SYSGEN row).
 */
#define LOGIN_INPUT_TIMEOUT_SEC   20

/*
 * What LOGINOUT writes to the terminal when a login prompt's read expires --
 * the two lines the V7.3 oracle printed, verbatim (header note above). The
 * first is LOGINOUT's own read-failure text (the one its audit record names
 * %LOGIN-F-CMDINPUT), the second is the reason, the system message for a
 * timed-out read.
 */
#define LOGIN_MSG_CMDINPUT  "Error reading command input"
#define LOGIN_MSG_TIMEOUT   "Timeout period expired"

/* login_read_line_timed() results. */
#define LOGIN_READ_OK        0   /* a complete line was read */
#define LOGIN_READ_EOF     (-1)  /* end of input / unrecoverable read error */
#define LOGIN_READ_TIMEOUT (-2)  /* the deadline expired with the line unfinished */

/*
 * login_now_ms - a monotonic millisecond clock for the deadline. CLOCK_MONOTONIC
 * so a system-time step (NTP, SET TIME) cannot shorten or extend a login window.
 */
static inline long long login_now_ms(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return 0;
}

/*
 * login_drain_typeahead - discard everything already waiting on `fd', without
 * blocking.
 *
 * WHY NOT JUST tcflush(). tcflush(TCIFLUSH) empties the TERMINAL's input queue
 * and does nothing at all on a descriptor the substrate does not present as a
 * terminal -- which is exactly the case this rung had to fix on the VAX rail,
 * where the console is a SIMH serial line rather than a QEMU virtio console.
 * On that rail the queued RETURNs the operator struck during the (long) boot
 * survived the flush and were then read as a burst of empty usernames, each
 * reprinting "Username: " on the same line: the vms-3ab8 machine-gun, alive
 * again on one arch only. Draining with a zero-timeout poll() works on a tty,
 * a pipe and a serial line alike, so the guard is arch-uniform.
 *
 * Still calls tcflush() first when the descriptor IS a terminal: that empties
 * the kernel tty queue in one syscall, and the drain then mops up anything the
 * substrate delivered outside it. Bounded (max_bytes) so a descriptor that is
 * continuously readable -- a live paste, /dev/zero in a test -- cannot spin
 * here forever.
 */
static inline void login_drain_typeahead(int fd, size_t max_bytes)
{
    size_t drained = 0;
    char scratch[256];

    if (isatty(fd))
        (void)tcflush(fd, TCIFLUSH);

    while (drained < max_bytes) {
        struct pollfd pfd;
        ssize_t n;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, 0) <= 0)
            return;                    /* nothing waiting (or poll failed) */
        if (!(pfd.revents & POLLIN))
            return;

        n = read(fd, scratch, sizeof(scratch));
        if (n <= 0)
            return;                    /* EOF or error -- caller will see it */
        drained += (size_t)n;
    }
}

/*
 * login_read_line_timed - read one newline-terminated line from `fd' into
 * `buf', giving up after `timeout_sec' seconds.
 *
 * @fd           descriptor to read (the session's terminal).
 * @buf/@bufsiz  destination; always NUL-terminated on LOGIN_READ_OK.
 * @echo_off     nonzero to suppress terminal echo for the duration (the
 *               Password: prompt). Only attempted when `fd' is a terminal; the
 *               original settings are restored before returning, and a newline
 *               is emitted on `echo_fd' so the cursor leaves the prompt line.
 * @echo_fd      the terminal's output side (STDOUT_FILENO for LOGINOUT):
 *               where the newline above goes, and where the timeout report
 *               below is written.
 * @timeout_sec  the idle deadline in seconds. 0 means "no deadline" -- used by
 *               nothing in LOGINOUT today, but it keeps the helper usable for a
 *               prompt VMS does not time out.
 *
 * THE DEADLINE COVERS THE WHOLE RESPONSE, not each keystroke: it is the time
 * a user has to ENTER A RESPONSE, so a session that types one
 * character every 19 seconds must not hold the terminal open indefinitely.
 *
 * Returns LOGIN_READ_OK, LOGIN_READ_EOF or LOGIN_READ_TIMEOUT. Bytes read
 * before a timeout are discarded: a partial username is not a username.
 *
 * ON LOGIN_READ_TIMEOUT THE EXPIRY IS REPORTED ON `echo_fd', as VMS does: the
 * prompt line is ended (exactly once -- the echo-off path has already ended
 * it) and LOGIN_MSG_CMDINPUT and LOGIN_MSG_TIMEOUT follow, one per line.
 *
 * A line longer than the buffer is TRUNCATED and the remainder consumed
 * through the newline, so an over-long username cannot leave its tail to be
 * read as the password (which is what fgets() did here before).
 */
static inline int login_read_line_timed(int fd, char *buf, size_t bufsiz,
                                        int echo_off, int echo_fd,
                                        unsigned timeout_sec)
{
    struct termios old_term, new_term;
    int have_term = 0;
    size_t len = 0;
    int rc = LOGIN_READ_EOF;
    long long deadline;

    if (!buf || bufsiz == 0)
        return LOGIN_READ_EOF;
    buf[0] = '\0';

    if (echo_off && isatty(fd) && tcgetattr(fd, &old_term) == 0) {
        new_term = old_term;
        new_term.c_lflag &= ~(tcflag_t)ECHO;
        (void)tcsetattr(fd, TCSAFLUSH, &new_term);
        have_term = 1;
    }

    deadline = login_now_ms() + (long long)timeout_sec * 1000;

    for (;;) {
        struct pollfd pfd;
        char c;
        ssize_t n;
        int pr;
        int wait_ms = -1;

        if (timeout_sec > 0) {
            long long remaining = deadline - login_now_ms();
            if (remaining <= 0) {
                rc = LOGIN_READ_TIMEOUT;
                break;
            }
            wait_ms = (remaining > 60000) ? 60000 : (int)remaining;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR)
                continue;              /* a signal is not the operator leaving */
            rc = LOGIN_READ_EOF;
            break;
        }
        if (pr == 0) {
            rc = LOGIN_READ_TIMEOUT;   /* nothing typed within the window */
            break;
        }

        n = read(fd, &c, 1);
        if (n == 0) {
            /* End of input. A line already typed but not terminated is still a
             * line the user finished by closing the connection -- accept it, as
             * fgets() did; nothing typed at all is EOF. */
            rc = (len > 0) ? LOGIN_READ_OK : LOGIN_READ_EOF;
            break;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            rc = LOGIN_READ_EOF;
            break;
        }

        if (c == '\n') {
            rc = LOGIN_READ_OK;
            break;
        }
        if (c == '\r')
            continue;                  /* a CRLF terminal ends lines with both */
        if (len + 1 < bufsiz)
            buf[len++] = c;
        /* else: truncate, and keep consuming to the newline */
    }

    buf[(rc == LOGIN_READ_OK) ? len : 0] = '\0';

    if (have_term) {
        (void)tcsetattr(fd, TCSAFLUSH, &old_term);
        /* Echo was off, so the user's RETURN produced no newline. */
        (void)!write(echo_fd, "\n", 1);
    }

    if (rc == LOGIN_READ_TIMEOUT) {
        static const char report[] =
            LOGIN_MSG_CMDINPUT "\n" LOGIN_MSG_TIMEOUT "\n";
        /* A half-typed line still sitting in the terminal's own (canonical)
         * input queue is not a response either: discard it, so it cannot
         * become the head of whatever the next session reads. */
        if (isatty(fd))
            (void)tcflush(fd, TCIFLUSH);
        if (!have_term)                /* the prompt line is still open */
            (void)!write(echo_fd, "\n", 1);
        (void)!write(echo_fd, report, sizeof(report) - 1);
    }

    return rc;
}

#endif /* LOGIN_INPUT_H */
