/*
 * ovmx_job_control.c - JOB_CONTROL.EXE, the OVMX console-session process
 * (vms-8d2, docs/design-init-scope.md §2/§5, docs/design-boot-faithful.md §4.5)
 *
 * ============================================================================
 * WHY THIS IMAGE EXISTS
 * ============================================================================
 * On OpenVMS, JOB_CONTROL is a DETACHED process, created during startup,
 * whose job is to create an interactive process running LOGINOUT.EXE on a
 * terminal (design-init-scope.md §1, row 6). It is one of the ~20 detached
 * system processes STARTUP.COM creates (§1 row 5; docs/oracle/vax73-show-
 * system-process.md lists it at PID 0000010B on the VAX 7.3 oracle;
 * docs/design-boot-faithful.md §3.6 lists it again on the Alpha 8.4 oracle at
 * PID 0000010B). Running the boot chain is emphatically NOT JOB_CONTROL's
 * job (§1 row 6's "does not" column).
 *
 * OVMX used to run this loop inline in PID 1 (src/ovmx_init/ovmx_init.c,
 * before this item) -- the exact "wrong component" defect
 * docs/design-init-scope.md §2 named it: "Real function, wrong owner."
 * PID 1 is SYSBOOT + EXEC_INIT + SYSINIT and nothing else; it hands off to
 * STARTUP.COM and its job ends there. Creating the interactive session is
 * JOB_CONTROL's job, and JOB_CONTROL is a service like any other -- created
 * from SYS$MANAGER:SYSTARTUP_VMS.COM via SYS$STARTUP:JOB_CONTROL_STARTUP.COM,
 * through RUN/DETACHED/PROCESS_NAME=JOB_CONTROL (vms-47b's mechanism), never
 * forked by STARTUP.EXE itself -- see the NOTE ON SERVICES in
 * src/ovmx_init/ovmx_init.c.
 *
 * WHAT MOVED, AND WHAT DID NOT. The retry/backoff logic below (the
 * consecutive_failures counter, the 5-failure diagnostic dump, the sleep(5)
 * backoff) is the SAME logic that used to run in PID 1's login loop, carried
 * over UNCHANGED (CLAUDE.md Rule 10: it is not independently oracle-pinned as
 * "what JOB_CONTROL does on repeated LOGINOUT failure" -- the ~/vax lab was
 * unavailable when the original behavior was written -- so it is neither
 * invented anew nor dropped, only relocated). Likewise the OPA0: channel
 * assign/setterm sequence (vms-d0b) and the "no DCL fallback" refusal
 * (vms-72c) are moved verbatim; their own comments, which explain WHY each
 * exists, move with them.
 *
 * WHAT THE CONSOLE CONNECTION LOOKS LIKE FOR A DETACHED PROCESS. A detached
 * process has no controlling terminal by definition ($CREPRC's PRC$M_DETACH,
 * src/libvms/syssvc/sys_process.c) -- stdin/stdout/stderr default to
 * /dev/null unless the creator supplies /INPUT, /OUTPUT, /ERROR. JOB_CONTROL
 * needs the REAL physical console to do its job, so SYS$STARTUP:
 * JOB_CONTROL_STARTUP.COM creates it with all three explicitly pointed at
 * the physical console device -- see that procedure's own comment for why it
 * spells the device as a Linux path (/dev/console) rather than the VMS
 * filespec OPA0: (dcl_resolve_path's VMS-filespec branch resolves disk-backed
 * specs through vmsfs; the OPA0: terminal alias src/libvms/syssvc/
 * sys_assign.c's $ASSIGN implements is a SEPARATE resolution path RUN
 * /DETACHED's /INPUT=/OUTPUT=/ERROR= qualifiers do not go through -- this
 * item does not extend dcl_resolve_path to close that gap, it only relies on
 * dcl_resolve_path's existing Linux-path passthrough, which sys_assign.c's
 * own comment already names /dev/console as the physical device behind
 * OPA0:).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>

#include "vmsfs/filespec.h"
#include "ovmx_layout.h"
#include "ssdef.h"
/* JOB_CONTROL's identity for OPA0: is established THROUGH the executive,
 * not declared -- the same vms_kif channel/setterm calls PID 1's login
 * loop used before this item (vms-d0b). */
#include "vms_kif.h"

static volatile sig_atomic_t shutdown_requested = 0;

static void sigterm_handler(int sig)
{
    (void)sig;
    shutdown_requested = 1;
}

/*
 * Translate a VMS filespec to a Linux path. Wrapper that returns a static
 * buffer -- use immediately or copy. Falls back to the spec itself if
 * translation fails (mirrors ovmx_init.c's vms_to_linux(), which this
 * function was copied from verbatim: JOB_CONTROL, like PID 1 before it,
 * resolves SYS$SYSTEM:LOGINOUT.EXE and SYS$SYSTEM:DCL.EXE before any
 * session exists to do it for it).
 */
static const char *vms_to_linux(const char *vms_spec, char *buf, size_t bufsz)
{
    if (vmsfs_to_linux_path(vms_spec, buf, bufsz) == 1)
        return buf;
    snprintf(buf, bufsz, "%s", vms_spec);
    return buf;
}

/*
 * Main: JOB_CONTROL creates login sessions on the console, forever, until
 * asked to stop. This IS the whole of JOB_CONTROL's job (design-init-
 * scope.md §1 row 6) -- there is no boot sequence here, no mounts, no
 * executive attach: SYSTARTUP_VMS.COM would not have run this image at all
 * if any of that had failed.
 */
int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Ignore SIGHUP so login children can terminate without killing us --
     * unchanged from PID 1's login loop (CLAUDE.md Rule 10: this behavior
     * was not independently oracle-pinned then and is not invented now). */
    signal(SIGHUP, SIG_IGN);

    char loginout_path[512], dcl_path[512];
    vms_to_linux(VMS_LOGINOUT_PATH, loginout_path, sizeof(loginout_path));
    vms_to_linux(VMS_DCL_PATH, dcl_path, sizeof(dcl_path));

    int console_interactive = isatty(STDIN_FILENO);
    int consecutive_failures = 0;

    while (!shutdown_requested) {
        /*
         * DEAD / NON-INTERACTIVE CONSOLE GUARD (vms-3ab8).
         *
         * The old guard here was `if (!console_interactive && feof(stdin))
         * break;` -- DEAD CODE. JOB_CONTROL never read()s stdin (its forked
         * LOGINOUT child does, on the inherited fd), so stdin's stdio EOF
         * indicator is never set and feof(stdin) is never true. A genuinely
         * dead or non-tty console therefore fell straight through: every
         * forked LOGINOUT hit EOF on the first fgets() and exited in well under
         * a second, and this loop respawned it as fast as the OS allowed. The
         * 5-fast-failures backoff below only throttles that spin to one burst
         * every five seconds -- it never stops it.
         *
         * JOB_CONTROL's whole job is to run login sessions ON THE OPERATOR
         * CONSOLE (OPA0:). If stdin is not an interactive terminal there is no
         * operator console to serve, so there is nothing to do and the process
         * exits honestly rather than respawning a login that can never read a
         * username. In normal operation JOB_CONTROL_STARTUP.COM binds stdin to
         * /dev/console -- a real terminal -- so console_interactive is true and
         * the loop runs exactly as before this change.
         */
        if (!console_interactive)
            break;

        struct timespec t_before;
        clock_gettime(CLOCK_MONOTONIC, &t_before);

        pid_t child = fork();
        if (child == 0) {
            /*
             * DELETED, NOT REPLACED (vms-fb9): setenv("VMS_TERMINAL",
             * "_OPA0:", 1) stood here once, in PID 1. A process told its
             * login child what terminal it was on through the environment
             * -- the rejected VMS_PRCNAM shape (CLAUDE.md rule 10, worked
             * example 2). It was not even a claim anything could check:
             * the child had no way to verify it and no other process
             * could see it.
             *
             * OPA0: IS real -- the executive creates it at module init
             * (src/kernel/vms_devtab.c) and every process on the node can
             * read it. So the console terminal does not need to be
             * announced; it needs to be LOOKED UP, with $ASSIGN and
             * $GETDVI on the resulting channel. JOB_CONTROL has no
             * business asserting it, and nothing downstream may be built
             * on this line being here.
             *
             * WHAT STANDS HERE INSTEAD (vms-d0b). The login session takes
             * a real channel to the console and asks the executive to
             * record that channel's device as this job's terminal. Three
             * properties, and each is the reason the environment variable
             * was not simply reinstated behind a function call:
             *
             *   - The name is not transmitted. $ASSIGN names the console
             *     because JOB_CONTROL is CREATING A SESSION ON IT -- that
             *     is system configuration, exactly as JOB_CONTROL_STARTUP.
             *     COM named the same device when IT created JOB_CONTROL --
             *     but VMS_IOCTL_SETTERM takes only the CHANNEL. The
             *     executive reads the device off the channel it issued and
             *     copies its own name. Nothing downstream receives a
             *     string it must trust.
             *   - The binding is in the executive, so a DIFFERENT process
             *     can read which terminal this job is on ($GETJPI), which
             *     is what makes it a fact rather than a self-description
             *     (CLAUDE.md Rule 11).
             *   - It survives the execl() below. The executive keys the
             *     process table on the thread-group id, which execve()
             *     does not change, so LOGINOUT.EXE and then DCL.EXE run
             *     with the binding their process already has, carrying
             *     nothing.
             *
             * Neither status is examined, deliberately, and this is the
             * same reasoning as cmd_show_device()'s untested
             * vms_kif_open(): the conditions they could report are ones
             * OVMX is not in. The executive is pinned open for the life
             * of the system (PID 1's executive_attach(), which halts the
             * whole boot if it is absent -- and JOB_CONTROL was created
             * by SYSTARTUP_VMS.COM, which cannot have run before that),
             * OPA0: is created at module init and vms.ko implements no
             * operation that removes a device, and the channel handed to
             * SETTERM is the one $ASSIGN just returned. A branch here
             * would be a handler for a state VMS is not in (Rule 10), and
             * the only thing it could usefully do is fabricate a binding.
             * If a call did fail, the executive records no terminal --
             * and SHOW TERMINAL then names none, which is the honest
             * outcome and the one the reader already renders.
             */
            uint32_t console_chan = 0;
            (void)vms_kif_assign(OVMX_CONSOLE_DEVICE, &console_chan);
            (void)vms_kif_setterm(console_chan);

            /* Child: exec vms_login (SYS$SYSTEM:LOGINOUT.EXE). */
            execl(loginout_path, "vms_login", (char *)NULL);

            /*
             * NO DCL FALLBACK (vms-72c). "exec vmsdcl directly" used to
             * stand here if the LOGINOUT.EXE exec above failed -- an
             * unauthenticated shell handed to whoever is at the console,
             * reached by nothing more than a missing or unexecutable
             * file. That is CLAUDE.md Rule 10's illegal third answer:
             * VMS has no state in which the console driver cannot run
             * LOGINOUT and responds by starting an interactive session
             * anyway with no username, no password and no SYSUAF check.
             *
             * MADE UNREACHABLE, NOT HANDLED, per Rule 10's other answer:
             * LOGINOUT.EXE is a required system file, present on every
             * installed system disk (the installer spine writes the whole
             * system tree; STARTUP.EXE's require_installed_system() halts
             * the boot before STARTUP.COM -- and therefore before
             * JOB_CONTROL -- can ever run on a volume that is not
             * installed), so failing to exec it here is the same class of
             * condition as vms.ko or /dev/vms being absent -- OVMX's one
             * runtime does not come up in that state. Unlike the
             * executive gate, the response here is not to halt the whole
             * boot: this is a per-login-attempt failure, not a per-system
             * one, and the outer loop already retries with backoff (see
             * "consecutive_failures" below) instead of surrendering the
             * console -- NOT independently oracle-pinned here as "what
             * VMS's console driver does on an image activation failure";
             * it is the behavior this loop already had before this item
             * moved it, kept unchanged. So the child reports why (OVMX
             * facility, not a VMS one -- a Linux exec(2) failure has no
             * VMS analogue) and exits, and the loop tries again; what it
             * may not do is substitute an unauthenticated shell for the
             * login it could not run.
             */
            fprintf(stderr, "%%OVMX-E-NOLOGIN, cannot exec %s: %s\n",
                    VMS_LOGINOUT_PATH, strerror(errno));
            _exit(1);
        } else if (child > 0) {
            /* Parent: wait for login session to end */
            int wstatus;
            waitpid(child, &wstatus, 0);

            struct timespec t_after;
            clock_gettime(CLOCK_MONOTONIC, &t_after);
            long elapsed_ms = (t_after.tv_sec - t_before.tv_sec) * 1000
                            + (t_after.tv_nsec - t_before.tv_nsec) / 1000000;

            /* Track consecutive fast failures (< 1 second) */
            if (elapsed_ms < 1000) {
                consecutive_failures++;
                if (consecutive_failures >= 5) {
                    fprintf(stderr,
                        "%%STARTUP-F-LOGINFAIL, login process failing repeatedly\n");
                    if (WIFEXITED(wstatus))
                        fprintf(stderr,
                            "%%STARTUP-F-LOGINFAIL, exit status %d\n",
                            WEXITSTATUS(wstatus));
                    else if (WIFSIGNALED(wstatus))
                        fprintf(stderr,
                            "%%STARTUP-F-LOGINFAIL, killed by signal %d\n",
                            WTERMSIG(wstatus));

                    /* Check if the binaries actually exist (VMS specs in messages) */
                    struct stat chk;
                    fprintf(stderr, "%%STARTUP-I-DIAG, %s: %s\n",
                            VMS_LOGINOUT_PATH,
                            stat(loginout_path, &chk) == 0 ?
                                "exists" : strerror(errno));
                    fprintf(stderr, "%%STARTUP-I-DIAG, %s: %s\n",
                            VMS_DCL_PATH,
                            stat(dcl_path, &chk) == 0 ?
                                "exists" : strerror(errno));

                    /* Back off instead of spinning */
                    sleep(5);
                    consecutive_failures = 0;
                }
            } else {
                consecutive_failures = 0;
            }

            if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
                usleep(100000);
            }

            /* Print blank line between sessions (like real VMS console) */
            printf("\n");
            fflush(stdout);
        } else {
            /* fork failed */
            perror("fork");
            sleep(1);
        }
    }

    return 0;
}
