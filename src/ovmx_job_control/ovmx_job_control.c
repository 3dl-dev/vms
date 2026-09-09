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
 *
 * ============================================================================
 * HOW THE SESSION IS CREATED (vms-3e9; design record docs/design/
 * faithful-sessions-and-network-subsystems.md §3.1/§6-P1)
 * ============================================================================
 * Through $CREPRC, and through nothing else. This program used to reach
 * LOGINOUT.EXE by a raw fork() + execl() -- a disclosed Linux leak, flagged in
 * its own comment as "the authentic-shape follow-up" -- with the terminal
 * arriving by inherited descriptors and the SYSTEM identity established by the
 * forked child itself. All of that is now ONE VMS system service call:
 *
 *     $CREPRC image=SYS$SYSTEM:LOGINOUT.EXE, input/output/error=OPA0:,
 *             stsflg=PRC$M_INTER|PRC$M_LOGINOUT
 *
 * -- "create a process running LOGINOUT.EXE bound to terminal-device OPA0:".
 * The fork(), the execve(), the open() of the terminal's backing, the dup2()s
 * and the controlling-terminal claim all live INSIDE that service
 * (src/libvms/syssvc/sys_process.c), below the VMS layer, where the SSH (P3)
 * and DECnet CTERM (P4) paths will share them by making the SAME call with an
 * RTAn: device instead of OPA0:. Nothing in this file forks, execs, opens a
 * pty or dup2s any more, and nothing in it stamps the session's identity:
 * LOGINOUT.EXE is the sole authenticator and re-personas its own process after
 * it authenticates, so the job controller needs no identity-forging privilege
 * of its own to start a session for an arbitrary user.
 *
 * WAITING, WITHOUT A LINUX CHILD TO WAIT ON. A VMS interactive process is
 * OWNERLESS -- the top of its own job, not a subprocess of the job controller
 * -- so $CREPRC's session-creation mode creates it detached and this program
 * has no child to waitpid() for. It waits the way one VMS process watches
 * another: $GETJPI on the process id the EXECUTIVE assigned, until the
 * executive no longer carries it (SS$_NONEXPR, after vms_proc_reap_dead()
 * reclaims the row of a process that has ceased to exist). That is a read of
 * the real process table, not a Linux wait status, and it keeps the retry
 * accounting below honest -- a session that dies instantly still measures as
 * an instant session.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include "vmsfs/filespec.h"
#include "ovmx_layout.h"
#include "ssdef.h"
/* $CREPRC + the descriptor and PRC$M_ definitions its session-creation mode
 * takes: the ONE way this program starts a login session (vms-3e9). */
#include "starlet.h"
#include "descrip.h"
#include "prcdef.h"
/* JOB_CONTROL's OWN SYSTEM identity is established THROUGH the executive, not
 * declared (vms-d31d). The session's identity is NOT this program's business
 * any more -- $CREPRC/LOGINOUT own it -- and neither is the OPA0: channel:
 * the $ASSIGN/SETTERM pair that used to stand in the forked login child moved
 * inside $CREPRC's terminal binding (vms-d0b's calls, relocated by vms-3e9). */
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

    /* ATOMIC FLIP (vms-5f0): JOB_CONTROL execve's LOGINOUT.EXE for the console
     * login; the Linux kernel maps its PT_LOAD + PT_INTERP by POSIX path. With
     * the /vms passthrough retired, LOGINOUT.EXE is execve'd from the boot-
     * staging tmpfs PID 1 filled off the ODS-2 volume THROUGH the executive
     * ACP. Self-guarding: use the staged copy only if present, so a substrate
     * that did not stage (NetBSD-vax, vms-d5d) keeps the original path. */
    {
        char staged[512];
        if (ovmx_boot_stage_exec_path(loginout_path, staged, sizeof(staged)) &&
            access(staged, X_OK) == 0)
            snprintf(loginout_path, sizeof(loginout_path), "%s", staged);
    }

    /*
     * ESTABLISH JOB_CONTROL'S SYSTEM IDENTITY IN THE EXECUTIVE (vms-d31d).
     *
     * JOB_CONTROL is a SYSTEM-owned detached process. Each login session it
     * creates below is a DISTINCT executive process (vms-d4ef) that
     * independently establishes this SAME SYSTEM identity on its own fresh PCB
     * -- but that call is no longer made HERE: vms-3e9 moved it inside
     * $CREPRC's PRC$M_LOGINOUT path (src/libvms/syssvc/sys_process.c), where it
     * belongs, because the authority to read SYSUAF is the LOGINOUT IMAGE's,
     * not its creator's (design record §2). The two processes still hold system
     * identity by the same privilege-gated primitive at different vms_pids; the
     * difference is that JOB_CONTROL no longer establishes it FOR the session.
     *
     * WHY JOB_CONTROL STILL ESTABLISHES ITS OWN. Two live reasons, neither of
     * them the session's SYSUAF read any more:
     *   - JOB_CONTROL genuinely IS a SYSTEM [1,4] detached process on VMS, and
     *     SHOW SYSTEM must be able to say so from another process.
     *   - It reads the session's row back through $GETJPI to wait for it (see
     *     the loop below). Once LOGINOUT re-personas that session to an
     *     ordinary account, its UIC is in another group, and the executive
     *     grants a cross-group row read only to a caller holding WORLD
     *     (vms_proc_may_read). Without a system identity here, the wait would
     *     be refused SS$_NOPRIV on the first non-SYSTEM login and the loop
     *     would read a live session as a finished one.
     * The SYSUAF protection rules the rest of this comment recites are
     * unchanged and still describe what the SESSION needs -- they are now
     * satisfied inside $CREPRC, on the session's own fresh PCB, before the
     * image is activated (the Wall-6 ordering guard).
     *
     * WHY THIS CALL, AND WHY $CREPRC DID NOT ALREADY DO IT. JOB_CONTROL is
     * created by SYS$STARTUP:JOB_CONTROL_STARTUP.COM's
     * RUN/DETACHED/UIC=[1,4]/PRIVILEGES=(...), and vms-d31d's $CREPRC stamps a
     * created process's UIC/privileges onto its executive row from the CREATOR's
     * identity (src/libvms/syssvc/sys_process.c). But that stamp is gated on the
     * created process inheriting an executive USER NAME, and the boot procedure
     * that issues the RUN/DETACHED (the STARTUP/STDRV DCL) has none on its
     * executive row -- so the stamp is skipped and JOB_CONTROL registers with
     * the fresh, credential-derived seed (VMS_PRV_M_ENFORCED|DEFAULT: SETPRV but
     * NOT SYSPRV/BYPASS, measured cur=0x13c00f on VAX). On x86_64 that seed
     * lands at UIC group 0 (root), which is <= MAXSYSGROUP and reads SYSUAF by
     * luck of the environment -- the root->group-0 crutch this program is
     * excising -- so the gap is invisible there. On NetBSD/VAX the seed is a
     * non-system group and the read is denied: the crutch was the only thing
     * making x86_64 login work, and VAX has no crutch.
     *
     * vms_ioctl_establish_system() (src/kernel-core/vms_proctab.c) stamps the
     * fixed SYSTEM identity -- UIC [1,4], the enforced SYSTEM privilege set,
     * user name "SYSTEM" -- and is GATED on the caller's real host privilege
     * (exec_current_is_privileged()); it is the same executive primitive
     * PROVISION.EXE uses to become SYSTEM without a SYSUAF read. JOB_CONTROL
     * genuinely holds that host privilege (it registered with the enforced set),
     * so this is a privilege-checked establishment of a real system identity,
     * NOT a blanket grant -- and it makes the AUTHENTIC identity load-bearing on
     * every substrate rather than the group-0 crutch.
     *
     * INV-6 / fail-honest: if the executive refuses (no host privilege) or is
     * absent, JOB_CONTROL is left non-system and the SYSUAF read then fails
     * honestly, exactly as it does today -- nothing here fabricates the
     * identity. The status is checked and a diagnostic printed so a regression
     * is never silent (the swallowed-privilege-error class this item names).
     */
    {
        uint32_t est = vms_kif_establish_system();
        if (!(est & 1))
            fprintf(stderr,
                    "%%JBC-W-NOSYSID, JOB_CONTROL could not establish its "
                    "SYSTEM identity (status %08X); console logins will be "
                    "refused SYS$SYSTEM:SYSUAF.DAT\n", (unsigned)est);
    }

    /*
     * THE SESSION-CREATION ARGUMENTS, built once.
     *
     * The IMAGE is named as a filespec the loader can map, exactly as DCL's
     * RUN/DETACHED names one (src/vmsdcl/dcl_cmd_process.c run_detached): the
     * caller resolves SYS$SYSTEM:LOGINOUT.EXE through vmsfs + the boot-staging
     * bridge above, and hands $CREPRC the result.
     *
     * The TERMINAL is named as a VMS DEVICE -- OPA0:, from the one constant
     * that names it (ovmx_layout.h) -- and NEVER as a substrate path. That is
     * the whole point of the mode: $CREPRC resolves the device to its backing
     * behind the VMS layer (ovmx_console_terminal_path, the same mapping
     * $ASSIGN uses), so a device this node does not have fails the creation
     * honestly with SS$_NOSUCHDEV instead of a caller here opening something.
     * The same three descriptors carry it, because a terminal is one device
     * for input, output and error alike.
     */
    struct dsc$descriptor_s img_d  = dsc$init(loginout_path);
    struct dsc$descriptor_s term_d = dsc$init(OVMX_CONSOLE_DEVICE);

    int console_interactive = isatty(STDIN_FILENO);
    int consecutive_failures = 0;

    while (!shutdown_requested) {
        /*
         * DEAD / NON-INTERACTIVE CONSOLE GUARD (vms-3ab8).
         *
         * The old guard here was `if (!console_interactive && feof(stdin))
         * break;` -- DEAD CODE. JOB_CONTROL never read()s stdin (the session
         * it creates does, on the terminal device $CREPRC bound it to), so
         * stdin's stdio EOF indicator is never set and feof(stdin) is never
         * true. A genuinely dead or non-tty console therefore fell straight
         * through: every LOGINOUT hit EOF on the first fgets() and exited in
         * well under a second, and this loop respawned it as fast as the OS
         * allowed. The 5-fast-failures backoff below only throttles that spin
         * to one burst every five seconds -- it never stops it.
         *
         * JOB_CONTROL's whole job is to run login sessions ON THE OPERATOR
         * CONSOLE (OPA0:). If stdin is not an interactive terminal there is no
         * operator console to serve, so there is nothing to do and the process
         * exits honestly rather than respawning a login that can never read a
         * username. In normal operation JOB_CONTROL_STARTUP.COM binds stdin to
         * the console -- a real terminal -- so console_interactive is true and
         * the loop runs exactly as before this change.
         */
        if (!console_interactive)
            break;

        struct timespec t_before;
        clock_gettime(CLOCK_MONOTONIC, &t_before);

        /*
         * CREATE THE INTERACTIVE SESSION (vms-3e9). One VMS system service,
         * no Linux mechanics: "create a process running LOGINOUT.EXE bound to
         * terminal-device OPA0:".
         *
         * NO IDENTITY IS PASSED. uic = 0 and prvadr = NULL mean "the creator's
         * defaults" to $CREPRC, and PRC$M_LOGINOUT then tells it not to stamp
         * even those: the created process establishes its own system identity
         * through the privilege-gated executive primitive and LOGINOUT
         * RE-PERSONAS it to the authenticated user afterwards. JOB_CONTROL
         * therefore never names, never forges and never sees the identity of
         * the user who logs in -- which is exactly why the same call will
         * serve NETACP and the SSH daemon, neither of which may hold the
         * authority to forge one (design record §2/§3.1).
         *
         * NO PROCESS NAME IS PASSED either: the session is named by LOGINOUT's
         * own $SETPRN once it knows the account (tools/vms_login.c), which is
         * what makes SHOW SYSTEM list the session under the username.
         */
        uint32_t session_pid = 0;
        uint32_t cst = sys$creprc(&session_pid, &img_d,
                                  &term_d, &term_d, &term_d,
                                  NULL, NULL, NULL,
                                  0, 0, 0,
                                  PRC$M_INTER | PRC$M_LOGINOUT);

        if (!(cst & 1)) {
            /*
             * THE SESSION WAS NOT CREATED, AND NOTHING PRETENDS OTHERWISE.
             *
             * This replaces the exec-failure branch the forked child used to
             * carry, and it keeps that branch's ruling intact (vms-72c): there
             * is NO DCL FALLBACK. VMS has no state in which the console cannot
             * run LOGINOUT and responds by starting an interactive session
             * anyway with no username, no password and no SYSUAF check. The
             * loop reports the executive's own status and retries with the
             * backoff below -- what it may not do is substitute an
             * unauthenticated shell for the login it could not create.
             *
             * The message is an OVMX facility, not a VMS one: $CREPRC failing
             * to create the console session is OVMX's condition to report, and
             * the status printed is the one the service returned (an executive
             * status such as SS$_NOSUCHDEV for a console this node does not
             * have, or OVMX$_PRCLOST for a process that died before it
             * registered), never a value invented here.
             */
            fprintf(stderr,
                    "%%OVMX-E-NOLOGIN, cannot create a console session running "
                    "%s on %s (status %08X)\n",
                    VMS_LOGINOUT_PATH, OVMX_CONSOLE_DEVICE, (unsigned)cst);
        } else {
            /*
             * WAIT FOR THE SESSION TO END, THROUGH THE EXECUTIVE.
             *
             * The interactive process is ownerless (see the file header), so
             * there is no Linux child to wait on and no wait status to read.
             * $GETJPI on the process id the executive assigned is the VMS way
             * to ask whether a process still exists, and the executive answers
             * SS$_NONEXPR once it does not: vms_proc_reap_dead() reclaims the
             * row of a process whose task the kernel has released, and PID 1
             * reaps everything reparented to it, so the row really does go
             * away when the session logs out.
             *
             * The poll interval is OVMX's own (CLAUDE.md Rule 8), chosen small
             * enough that the next "Username:" follows a logout without a
             * visible pause and large enough to cost nothing while an operator
             * is typing. It is not a claimed VMS behaviour: real VMS wakes the
             * job controller from the process's termination mailbox, which
             * OVMX's $CREPRC does not implement (the mbxunt argument is
             * ignored) -- so this waits honestly instead of pretending to be
             * notified.
             */
            struct vms_procinfo info;
            for (;;) {
                uint32_t gst = vms_kif_getjpi_pid(session_pid, &info);
                if (!(gst & 1))
                    break;                      /* SS$_NONEXPR: session gone */
                if (shutdown_requested)
                    break;
                {
                    struct timespec pause_for = { 0, 200 * 1000 * 1000 };
                    nanosleep(&pause_for, NULL);
                }
            }
        }

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
                fprintf(stderr,
                    "%%STARTUP-F-LOGINFAIL, last $CREPRC status %08X\n",
                    (unsigned)cst);

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

        if (!(cst & 1))
            usleep(100000);

        /* Print blank line between sessions (like real VMS console) */
        printf("\n");
        fflush(stdout);
    }

    return 0;
}
