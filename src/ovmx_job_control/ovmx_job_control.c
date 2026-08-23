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
     * JOB_CONTROL is a SYSTEM-owned detached process, and every login session
     * it creates below inherits ITS executive identity via
     * vms_kif_register_continue() (the child's line, ~below). LOGINOUT then has
     * to read the World-denied SYS$SYSTEM:SYSUAF.DAT (fh2_fileprot 0xFF88;
     * acp_check_access() in src/kernel-core/vmsfs_acp.c grants that read only to
     * a caller in the SYSTEM protection category -- UIC group <= MAXSYSGROUP, or
     * SYSPRV/BYPASS/READALL). So JOB_CONTROL MUST hold a system identity, or the
     * login it spawns is refused its own authorization file (%RMS-E-PRV,
     * surfacing as LOGINOUT's "User authorization failure").
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
             * THE LOGIN SESSION CONTINUES JOB_CONTROL'S EXECUTIVE IDENTITY
             * (vms-d4ef, Wall 6). LOGINOUT must read SYS$SYSTEM:SYSUAF.DAT to
             * authenticate a not-yet-authenticated user, and SYSUAF is
             * World-denied (fh2_fileprot 0xFF88 -- S:RWE, O:RWE, G:none,
             * W:none; ods2_class_fileprot(), oracle vax73-authorize-privilege.
             * md). The Files-11 protection gate (acp_check_access() in
             * src/kernel-core/vmsfs_acp.c) grants that read only to a caller
             * that qualifies for the SYSTEM protection category -- a UIC group
             * <= MAXSYSGROUP, or SYSPRV/BYPASS/READALL.
             *
             * JOB_CONTROL already holds such an identity: it is created with
             * $CREPRC (SYS$STARTUP:JOB_CONTROL_STARTUP.COM's RUN/DETACHED),
             * which stamps the child's UIC/username/privileges through the
             * executive (sys$creprc -> vms_pcb_set_identity, src/libvms/syssvc/
             * sys_process.c), inheriting the system identity of the STARTUP.COM
             * process that created it. But LOGINOUT is NOT created through
             * $CREPRC -- it is reached by the fork()+execl() below -- so
             * WITHOUT this call it never inherits that identity. Instead its
             * first executive call lazily REGISTERs a FRESH process whose UIC
             * is derived from the substrate's OS credentials (vms_proc_register
             * / vms_proc_get, uic = (gid<<16)|uid). On the QEMU/Linux runtime
             * those credentials are root, so the fresh UIC is group 0, which is
             * <= MAXSYSGROUP and qualifies for SYSTEM by luck of the
             * environment -- which is why x86_64 reads SYSUAF today. On the
             * NetBSD/VAX substrate the login process's credentials are NOT
             * privileged, so the fresh PCB is a non-SYSTEM, non-privileged
             * process and the ACP denies the SYSUAF read (0xFF88 -> RMS$_PRV),
             * surfacing as the clean "User authorization failure" LOGINOUT
             * prints when sysuaf_lookup() cannot open the file.
             *
             * vms_kif_register_continue() -- the SAME executive call DCL's RUN
             * path makes in its forked child before execv (src/vmsdcl/
             * dcl_cmd_process.c, and what let SYSTEM's RUN AUTHORIZE open the
             * World-denied SYSUAF, vms-381) -- registers THIS task as a
             * continuation of its VMS parent (JOB_CONTROL) while it is still
             * JOB_CONTROL's child, so the executive reads JOB_CONTROL's row and
             * copies its genuine UIC and CURRENT privilege masks onto this
             * task. The PCB is keyed on the thread group and survives the
             * execl() below, so LOGINOUT.EXE runs with the continued system
             * identity and reads SYSUAF the same authentic way on every
             * substrate -- an executive-mediated inheritance of a real system
             * identity, never a blanket grant, and it weakens no protection:
             * the World-deny stands, and a caller the executive did not stamp
             * with a system identity is still refused.
             *
             * It runs FIRST, before the $ASSIGN/setterm below, so the console
             * channel and terminal are recorded on the continued PCB rather
             * than on a fresh one the continuation would then replace. Its
             * status is not examined for the same reason the setterm calls
             * below are not: the executive is pinned open for the life of the
             * system (PID 1's executive_attach()) and JOB_CONTROL, whose row
             * this continues, was created by that same executive; a failure
             * here leaves LOGINOUT with no continued identity and the SYSUAF
             * read then fails honestly, which is the outcome the reader already
             * renders.
             *
             * FAITHFULNESS NOTE (vms-d4ef follow-up). On real OpenVMS the
             * interactive session is a genuinely NEW process ($CREPRC'ing
             * LOGINOUT.EXE, an image INSTALLed /PRIVILEGED with SYSPRV), not a
             * continuation of JOB_CONTROL. OVMX reaches the console session by
             * fork()+execl() rather than $CREPRC, so this continues
             * JOB_CONTROL's identity instead of modelling installed-image
             * privilege -- consistent across substrates and enough to unblock
             * Wall 6, but the installed-privileged-LOGINOUT model (and a
             * $CREPRC'd console session with its own VMS PID) is the authentic
             * shape and is filed as follow-up, not done here.
             */
            (void)vms_kif_register_continue();

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
