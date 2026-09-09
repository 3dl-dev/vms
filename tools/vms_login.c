/*
 * vms_login.c - VMS-style login program for OVMX
 *
 * Displays a VMS-like login banner, prompts for username and
 * password, authenticates against SYSUAF.DAT, and then execs
 * the DCL shell with --login flag.
 *
 * Build: part of tools/ CMakeLists.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "sysuaf.h"
#include "str_util.h"
#include "vms/pcb.h"
#include "vms/privs.h"
#include "vms/logical.h"
#include "ovmx_accounting.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
/* LOGINOUT stamps the authenticated identity onto the executive's row. */
#include "vms_kif.h"
/* vms-3b0: distinguish a SYSUAF read fault (RMS$_PRV/...) from RMS$_RNF and
 * render the authentic %RMS- status text. */
#include "rmsdef.h"
#include "ovmx_status.h"
/* The OpenVMS-faithful post-authentication login-info block (vms-417), and the
 * pre-Username system-identification line (vms-3e9). */
#include "loginout_display.h"
/* The prompt reader: one line, with the LGI-style idle deadline, plus the
 * substrate-independent type-ahead drain (vms-3e9). */
#include "login_input.h"
/* New-mail count for the login notification (vms-417). */
#include "vms_mail_notify.h"

/* Maximum number of login attempts before disconnect */
#define MAX_ATTEMPTS   3

/* Paths */
#include "ovmx_layout.h"
#include "ovmx_identity.h"
#include "ovmx_banner.h"
#define LASTLOGIN_DIR      VMS_LASTLOGIN_DIR
#define DCL_SHELL_PATH     VMS_DCL_PATH

/* str_upcase() and str_trim() replaced by str_str_upcase()/str_trim() from str_util.h */

/* ------------------------------------------------------------------ */
/* Read one prompt response, with the LGI-style idle deadline          */
/* ------------------------------------------------------------------ */
/*
 * THE LOGIN SEQUENCE IS BOUNDED IN TIME (vms-3e9). Both prompts are read
 * through login_read_line_timed(), which gives up after
 * LOGIN_INPUT_TIMEOUT_SEC seconds of an unfinished response -- the public
 * OpenVMS LGI_PWD_TMO behaviour and its default value (see login_input.h for
 * the citation and for why the deadline is a compiled-in constant rather than
 * a fabricated SYSGEN row).
 *
 * BEFORE THIS, NEITHER PROMPT HAD A DEADLINE ON ANY ARCH: a console left at
 * "Username:" held the session open forever, and JOB_CONTROL -- which watches
 * the session it created and creates the next one when it ends -- had nothing
 * to observe. The read is the only place a deadline can live, because that is
 * where the session is idle.
 *
 * ON EXPIRY THE SESSION IS DISCONNECTED SILENTLY, which is the existing
 * MAX_ATTEMPTS behaviour and the same VMS rule: LOGINOUT prints no farewell
 * when it drops a connection, so an invented "login timed out" line would be a
 * self-certified VMS message (CLAUDE.md Rule 10, the same defect vms-417
 * deleted from the MAX_ATTEMPTS path). The disconnect is real, not cosmetic:
 * console_login() returns, main() returns, the image exits, and JOB_CONTROL
 * creates a fresh session on OPA0: -- which is what a real disconnect looks
 * like from the terminal.
 */
static int read_prompt_response(char *buf, size_t bufsiz, int hide)
{
    int rc = login_read_line_timed(STDIN_FILENO, buf, bufsiz, hide,
                                   STDOUT_FILENO, LOGIN_INPUT_TIMEOUT_SEC);
    if (rc != LOGIN_READ_OK)
        return rc;

    str_trim(buf);
    return LOGIN_READ_OK;
}

/* ------------------------------------------------------------------ */
/* Start a VMS session: banner, environment, exec DCL shell           */
/* ------------------------------------------------------------------ */
static void start_session(const sysuaf_record_t *rec, unsigned login_failures)
{
    /* SYS$WELCOME (a boot-defined logical), falling back to the built-in
     * badged OVMX identity when undefined -- as LOGINOUT does on VMS. */
    printf("\n");
    ovmx_banner_welcome(stdout);
    printf("\n");

    /*
     * THE OpenVMS LOGINOUT SESSION-INFORMATION BLOCK (vms-417).
     *
     * Order, wording, format and the omit-when-absent rule are pinned to
     * public OpenVMS docs -- see loginout_display.c. Every line is emitted
     * ONLY when its value is real; nothing is fabricated (CLAUDE.md Rule 9).
     *
     *   Last interactive login    -- REAL, from the accounting record
     *                                 (ovmx_accounting.c). Absent on a first
     *                                 login -> line omitted, as on VMS. This is
     *                                 the same accounting store SSH login uses;
     *                                 both are interactive/remote logins in VMS
     *                                 terms, so it is the interactive slot.
     *
     *   Last non-interactive login -- NO SOURCE YET (flagged gap, vms-417).
     *                                 OVMX has no batch/network login that
     *                                 records a non-interactive timestamp, so
     *                                 last_noninteractive is always 0 and the
     *                                 line is omitted -- exactly as VMS omits it
     *                                 for an account that has never had a
     *                                 non-interactive login. The emit path is
     *                                 wired and correct; it lights up the day a
     *                                 batch/DECnet login records into this slot.
     *
     *   N failures since last login -- REAL: the bad-password attempts this
     *                                 connection made before succeeding
     *                                 (login_failures, counted by
     *                                 console_login). Omitted at zero. Narrower
     *                                 than VMS's cross-connection UAF$W_LOGFAILS
     *                                 (OVMX does not persist a failure counter),
     *                                 but it is a true subset, never invented.
     *
     *   You have N new mail messages -- REAL, mail_count_unread() (the shared
     *                                 MAIL counter, vms_mail_notify.h). Omitted
     *                                 at zero.
     */
    time_t last_interactive = 0;
    (void)ovmx_accounting_get_lastlogin(rec->username, &last_interactive);

    time_t last_noninteractive = 0;   /* flagged gap: no recorder yet (above) */

    int new_mail = mail_count_unread(rec->username);

    loginout_display_session_info(stdout, last_interactive,
                                  last_noninteractive, login_failures,
                                  new_mail);
    printf("\n");

    /* Record this login now (after showing last, before launching DCL) */
    ovmx_accounting_record_login(rec->username);

    /*
     * ESTABLISH THE AUTHENTICATED IDENTITY IN THE EXECUTIVE (vms-2b8).
     *
     * This is the whole point of LOGINOUT: the password has just been
     * checked against SYSUAF, and what that proved has to be recorded
     * somewhere every other process can see it and no process can
     * rewrite it. VMS_IOCTL_SETIDENT writes it into the executive's
     * process table, and the executive refuses the call outright unless
     * this process holds SETPRV -- so LOGINOUT can establish an
     * identity, and the session it hands over to cannot widen it.
     *
     * The row survives the execl() below, because the executive keys it
     * on the thread-group id and exec does not change that. Nothing in
     * the DCL image reads an environment variable to learn who it is.
     *
     * WHAT THIS REPLACES: setenv("VMS_USERNAME"/"VMS_UIC_GROUP"/
     * "VMS_UIC_MEMBER"/"VMS_PRIVILEGES") -- ordinary environment
     * variables that DCL then believed. Any process could set them, so
     * "logging in" was a process choosing a username (CLAUDE.md
     * Rule 10's worked example, in its original habitat).
     *
     * FAILURE IS FATAL (round 6). It used to print a warning and hand
     * the user a session anyway, which is CLAUDE.md Rule 10's illegal
     * third answer twice over: VMS has no state in which LOGINOUT
     * checks a password against SYSUAF and then starts a session with
     * no identity, so there is no behaviour to reproduce and the
     * condition must be made UNREACHABLE rather than handled -- and the
     * warning wore the LOGINOUT facility with an invented code, which
     * is a self-certified VMS diagnostic. Worse, it degraded UPWARD:
     * an unstamped session kept the credentials LOGINOUT was started
     * with (root, UIC [0,0], the executive's whole enforced mask),
     * i.e. strictly MORE privilege than the SYSUAF record grants.
     *
     * Unreachable, not merely refused: PID 1 halts at boot if the
     * executive is absent (vms-0ff), so by the time any Username:
     * prompt exists /dev/vms answers. The message wears the OVMX
     * facility because a rejected ioctl is an OVMX event, not a VMS
     * one -- the same reasoning ovmx_init.c uses for %OVMX-I-EXEC.
     */
    {
        uint32_t login_uic = (rec->uic_group << 16) | rec->uic_member;
        /* vms-26a: build the persona mask from the binary $UAFDEF quadword,
         * NOT by re-parsing rec->privileges. The name-string re-parser drops
         * MOUNT (and other privileges outside its 17-name subset), which is
         * what produced %SYSTEM-F-NOPRIV on MOUNT after the SYSUAF flip.
         * See sysuaf_record_privileges() in sysuaf.h. */
        uint64_t login_privs = sysuaf_record_privileges(rec);
        /* Authorized JIB quota set (vms-14a): decode the account's [OVMX]
         * SYSUAF quota region and hand it to the executive with the identity,
         * exactly as LOGINOUT copies the SYSUAF quota cells into the JIB. If
         * the record carries no seeded quota (presence marker clear), pass NULL
         * so the executive omits VMS_PI_V_QUOTA -- honest omission, never a
         * fabricated block (INV-6). Field order matches struct vms_jib_quota. */
        sysuaf_quota_t uq;
        struct vms_jib_quota jq;
        const struct vms_jib_quota *jqp = NULL;
        if (sysuaf_quota_decode(&rec->raw, &uq)) {
            jq.astlm     = uq.astlm;     jq.biolm    = uq.biolm;
            jq.bytlm     = uq.bytlm;     jq.diolm    = uq.diolm;
            jq.enqlm     = uq.enqlm;     jq.fillm    = uq.fillm;
            jq.pgflquota = uq.pgflquota; jq.prclm    = uq.prclm;
            jq.tqelm     = uq.tqelm;     jq.wsdefault = uq.wsdefault;
            jq.wsquota   = uq.wsquota;   jq.wsextent = uq.wsextent;
            jqp = &jq;
        }
        uint32_t ist = vms_kif_setident_quota(rec->username, login_uic,
                                              login_privs, jqp);
        if (!(ist & 1)) {
            printf("%%OVMX-F-NOIDENT, the executive refused the "
                   "authenticated identity (status %u)\n", (unsigned)ist);
            fflush(stdout);
            _exit(1);
        }
    }

    /*
     * NAME THE SESSION IN THE EXECUTIVE (vms-72c).
     *
     * WHY THIS WAS MISSING. vms_kif_setprn() already existed and already
     * had a product caller (src/libvms/syssvc/sys_process.c, for a
     * $CREPRC caller that supplies an explicit prcnam) -- but nothing on
     * the interactive console login path ever called it, so a real,
     * authenticated, terminal-owning login session sat in the executive's
     * process table with prcnam == "" forever. Measured on a real QEMU
     * boot before this change: SHOW PROCESS on the SYSTEM console session
     * printed `Process name: ""`, and SHOW USERS (once vms-72c also fixed
     * it to read the process table instead of fabricating a row) could
     * name every OTHER field of the session but not this one.
     *
     * WHAT THE NAME IS, AND WHY: the SYSUAF username, unmodified.
     * ORACLE-PINNED, not chosen -- VAX1, OpenVMS VAX V7.3, docs/oracle/
     * vax73-show-system-process.md Section 1: the interactive SYSTEM
     * session in that capture's own SHOW SYSTEM table is named literally
     * `SYSTEM` (row "2020021C SYSTEM CUR ..."), i.e. its process name IS
     * its username, with no decoration. rec->username is already the
     * upcased SYSUAF key (sysuaf_lookup(), tools/vms_login.c above), so
     * no separate uppercasing is needed here.
     *
     * NOT vms-d0e. vms-d0e asks what the executive should name a process
     * created with NO explicit prcnam at all (e.g. a bare $CREPRC/RUN
     * with no /PROCESS_NAME) -- a question that oracle session explicitly
     * left unpinned (Section 1.3: the detached-process probe died before
     * answering it). This is a narrower, already-answered question:
     * LOGINOUT itself, on VMS, supplies an explicit name -- the
     * account's username -- to $SETPRN as part of establishing the
     * session, before $CREPRC's own "what if nobody names it" default
     * ever comes into play. Closing this does not touch vms-d0e's
     * question, and vms-d0e's tripwire in tests/uat/vms_session_qemu.sh
     * for an UNNAMED $CREPRC subprocess (SPAWN) is deliberately left in
     * place below, unchanged, because SPAWN still passes no explicit
     * prcnam.
     *
     * FAILURE IS NOT FATAL, and this is a narrower claim than the block
     * above it, not the same one reapplied. VMS_IOCTL_SETIDENT failing
     * means the account this password just authenticated has NO
     * identity the rest of the system can see -- an unreachable state on
     * VMS, so login must not hand over a session in it. A name collision
     * (SS$_DUPLNAM: this exact username is already logged in elsewhere
     * in the same UIC group) is a different, genuinely reachable VMS
     * state -- OpenVMS handles it by uniquifying the name
     * (SMITH, SMITH_1, ...), which OVMX does not implement here (no
     * concurrent-same-user login is exercised by any test on this
     * runtime, and inventing the suffix format without an oracle
     * transcript to pin it against would be exactly Rule 10's illegal
     * third answer). So on DUPLNAM the session proceeds unnamed rather
     * than refused outright: a nameless interactive session is a real,
     * already-disclosed OVMX divergence (vms-d0e) and strictly better
     * than refusing a login the password legitimately authenticated.
     * The failure is still reported, not swallowed silently.
     */
    {
        uint32_t nst = vms_kif_setprn(rec->username);
        if (!(nst & 1))
            printf("%%OVMX-I-NOPRCNAM, the executive did not name this "
                   "session (status %u)\n", (unsigned)nst);
    }

    /* Set up environment for session.
     * VMS_DEFAULT_DIR is a VMS directory spec from SYSUAF.
     *
     * VMS_USERNAME REMAINS, AND IT IS STILL A FACADE. Its last reader
     * in the product is tools/vms_mail.c, which uses it to pick whose
     * mailbox to open -- so a user can still read another user's mail
     * by setting it. Deleting it here without converting MAIL would
     * silently break MAIL instead of fixing the hole, and MAIL is
     * outside this item. It is left LOUD rather than silent (vms-2b8
     * scope note 3) and reported. The UIC and privilege variables are
     * gone: they have no readers left. */
    setenv("VMS_USERNAME",    rec->username,    1);
    setenv("VMS_DEFAULT_DIR", rec->default_dir, 1);

    /*
     * SYS$LOGIN / SYS$LOGIN_DEVICE ARE NO LONGER ENV VARS (vms-e48). SYS$LOGIN
     * used to be setenv'd here and getenv'd by DCL -- a facade, because
     * F$TRNLNM("SYS$LOGIN") never consulted the environment and so returned the
     * generic SYS$SYSDEVICE:[USERS] default while the user's real home hid in
     * an env var only the login-script code read (two answers for one logical).
     *
     * They are now established as REAL LNM$JOB logicals (job-wide, so the whole
     * login job -- DCL and every image it activates -- agrees on them, as on
     * OpenVMS), sourced from this SYSUAF record's default device/directory, by
     * lnm_define_login_logicals() in the DCL --login process (dcl_main.c). The
     * establishment runs there, not here: LNM$PROCESS state would not survive
     * the execl() below anyway, and -- unlike LOGINOUT -- that process runs as
     * the unprivileged user and cannot re-read the protected SYSUAF.DAT, so
     * LOGINOUT (running privileged, pre-setuid) reads the record and hands the
     * default device/directory over through VMS_DEFAULT_DIR (above) and the
     * login command file through --lgicmd (below). DCL then defines the
     * logicals and translates them via F$TRNLNM.
     *
     * SYS$SCRATCH is NOT touched: OVMX defines it system-wide to its
     * mastered-writable [SYSTMP] scratch (see lnm_define_login_logicals()).
     * The old setenv("SYS$SCRATCH", "SYS$SYSDEVICE:[SYSTMP]") here was dead
     * anyway -- F$TRNLNM never read it.
     */

    /*
     * chdir into the VMS tree so DCL inherits a VMS-rooted cwd.
     * Translate the VMS directory spec to Linux for the syscall.
     *
     * LOGINOUT DOES NOT CREATE OR RE-OWN SYS$LOGIN (vms-2b8 round 7).
     * Round 6 had it mkdir() the directory and chown() it to the SYSUAF
     * UIC, printing %OVMX-W-LOGINOWN and carrying on if that failed --
     * which is CLAUDE.md Rule 10's illegal third answer, added forty
     * lines below where the identical shape (%LOGINOUT-W-NOIDENT) was
     * being deleted for being it. VMS has no state in which LOGINOUT
     * authenticates a user and then hands them a SYS$LOGIN they do not
     * own; under the [gid,uid] protection the credential drop below
     * activates, such a session cannot write its own login directory.
     * A warning for that is a plausible-looking handler for a condition
     * VMS never faces.
     *
     * So the condition is made UNREACHABLE rather than handled: the
     * directory is created and owned when the ACCOUNT is provisioned,
     * on the startup path, before any login can happen
     * (provision_home_directories() and provision_ownership() in
     * src/ovmx_provision/ovmx_provision.c -- PROVISION.EXE, which PID 1
     * execs and which execs DCL.EXE on STARTUP.COM afterwards, vms-9b7).
     * That is also what OpenVMS does -- the
     * System Manager's Manual add-user procedure is AUTHORIZE ADD
     * followed by CREATE/DIRECTORY .../OWNER=[g,m]; LOGINOUT is not in
     * that sequence and has no fixup step of its own.
     */
    char home_linux[512];
    if (vmsfs_to_linux_path(rec->default_dir, home_linux, sizeof(home_linux)) == 1)
        chdir(home_linux);

    /* Initialize user PCB (lives until exec replaces address space).
     * Seeded from the row the executive just stamped -- a copy of the
     * executive's verdict, not a second, independent claim. */
    struct vms_procinfo linfo;
    memset(&linfo, 0, sizeof(linfo));
    if (vms_kif_getjpi_self(&linfo) & 1) {
        struct vms_pcb *pcb = vms_pcb_init(linfo.cur_privs);
        if (pcb) {
            char prcnam[16];
            snprintf(prcnam, sizeof(prcnam), "_FTA%d:", (int)(getpid() % 100));
            vms_pcb_set_identity(linfo.vms_pid, linfo.uic, linfo.username,
                                 prcnam);
            vms_pcb_set_default_dir(rec->default_dir);
        }
    }

    /*
     * BECOME THE AUTHENTICATED USER (vms-2b8 round 6).
     *
     * WHY THIS IS NOT OPTIONAL. The executive's identity row is keyed on
     * the thread group, and a NEW task derives its own authorized mask
     * at registration from capable(CAP_SYS_ADMIN) -- see
     * vms_proc_register() in src/kernel/vms_module.c. So until this
     * call, every DCL session AND EVERY PROCESS IT SPAWNS ran as Linux
     * root: each child registered holding CMKRNL|CMEXEC|SYSNAM|GRPNAM|SETPRV|WORLD
     * before it executed a single instruction, and SETPRV is exactly
     * what VMS_IOCTL_SETIDENT requires to establish an arbitrary
     * identity. It was proven by execution, not argued: an ordinary
     * FIELD/[200,10] session forked a child, the child re-registered,
     * and it stamped itself SYSTEM [1,4] with SYSUAF's privilege ALL. The
     * executive's refusal was real but it protected exactly one task,
     * and a privilege reduction survived only until the next fork.
     *
     * The UIC is [gid,uid] throughout OVMX -- the executive derives
     * proc->uic that way, and src/vmsrms/rms_core.c enforces file
     * protection against the same pair. Before this call those two
     * disagreed for every session: the executive reported the SYSUAF
     * UIC while RMS saw root's [0,0]. After it they are the same UIC by
     * construction, because there is only one.
     *
     * THIS IS NOT A NEW VMS BEHAVIOUR AND IS NOT PRESENTED AS ONE
     * (CLAUDE.md Rule 8/10). OpenVMS has no Linux credentials; the
     * uid/gid pair is OVMX's stand-in for the UIC. What changes here
     * is only that the stand-in is made to agree with the identity the
     * executive was already enforcing, so the enforcement is not
     * layered over a process that could sidestep it by forking.
     *
     * ORDER MATTERS. It runs AFTER VMS_IOCTL_SETIDENT (which needs the
     * SETPRV that root-derived registration granted), after the SYS$LOGIN
     * chown, and after the accounting write -- and BEFORE execl, so the
     * image the user drives never holds credentials it did not
     * authenticate for. The executive's row survives: it is keyed on the
     * thread group id, which neither setuid nor exec changes.
     *
     * FAILURE IS FATAL. "LOGINOUT authenticated a user and then ran the
     * session as root anyway" is not a VMS state and gets no handler
     * (Rule 10): the condition is made unreachable.
     */
    {
        uid_t want_uid = (uid_t)rec->uic_member;
        gid_t want_gid = (gid_t)rec->uic_group;

        /*
         * PER-USER PRIVATE IMAGE-STAGING DIRECTORY (vms-a86f).
         *
         * The atomic flip (vms-5f0) stages an image the Linux kernel must
         * execve() into OVMX_BOOT_STAGE_DIR ("/run/ovmx-boot"), which PID 1
         * creates root-owned 0755 and pre-fills with the boot images + a fixed
         * utility set. An image the boot bridge did NOT pre-stage (e.g.
         * SYS$SYSTEM:PARTS.EXE the first time `$ PARTS` runs) is staged lazily
         * by DCL's activation resolver -- but that resolver runs as THIS
         * session's UIC after the credential drop below, and a non-root session
         * cannot create a file in the root-owned shared directory (the PARTS
         * demo's EACCES). Making the shared directory world-writable would be a
         * plant hole (any user could drop an image others activate) and is
         * forbidden.
         *
         * So LOGINOUT -- still privileged here, the same window that stamped the
         * SYSUAF identity -- creates the session's OWN staging directory,
         * OVMX_BOOT_STAGE_DIR "/<uid>/", 0700 and owned by the authenticated
         * UIC. The resolver then stages the session's images into a directory
         * it owns; the genuine bytes still come off the ODS-2 volume over the
         * executive ACP (INV-6), only the Linux-exec handoff is per-user-owned.
         * Best-effort: PID 1 already made the parent, and a failure here is not
         * fatal -- an image that then cannot be staged fails honestly at
         * activation with %DCL-E-IVIMAGE, never a fabricated success.
         */
        {
            char stage_user_dir[512];
            (void)mkdir(OVMX_BOOT_STAGE_DIR, 0755);   /* PID 1 makes it; EEXIST fine */
            if (ovmx_boot_stage_user_dir(stage_user_dir, sizeof(stage_user_dir),
                                         (unsigned long)want_uid)) {
                if (mkdir(stage_user_dir, 0700) == 0 || errno == EEXIST)
                    (void)chown(stage_user_dir, want_uid, want_gid);
            }
        }

        if (setgroups(0, NULL) != 0 ||
            setgid(want_gid) != 0 ||
            setuid(want_uid) != 0 ||
            getuid()  != want_uid || geteuid() != want_uid ||
            getgid()  != want_gid || getegid() != want_gid) {
            printf("%%OVMX-F-NOUIC, could not become UIC [%o,%o] for user "
                   "%s: %s\n", (unsigned)rec->uic_group,
                   (unsigned)rec->uic_member, rec->username,
                   strerror(errno));
            fflush(stdout);
            _exit(1);
        }
    }

    /*
     * The login command file to run: the SYSUAF LGICMD field, or the
     * documented SYS$LOGIN:LOGIN.COM default when that field is empty
     * (vms-e48). LOGINOUT resolves it here (it is the process that read the
     * SYSUAF record) and hands the resulting VMS filespec to DCL through
     * --lgicmd; DCL translates it through the SYS$LOGIN logical it is about to
     * define. This replaces DCL's old hardcoded "<home>/LOGIN.COM".
     */
    char login_lgicmd[256];
    sysuaf_login_command_file(rec, login_lgicmd, sizeof(login_lgicmd));

    /*
     * CAPTIVE CONFINEMENT (vms-c8fa). A captive account is confined to its
     * login command procedure: it runs SYLOGIN.COM and the account's LGICMD
     * and is then logged out -- it never reaches the "$" DCL prompt, and
     * Ctrl/Y is disabled so the procedure cannot be interrupted to escape to
     * DCL (OpenVMS Guide to System Security, "Captive Accounts"). LOGINOUT
     * conveys the CAPTIVE flag to DCL with --captive; DCL disables Ctrl/Y and
     * logs out instead of entering the interactive REPL. A non-captive
     * account execs EXACTLY as before -- this is purely additive.
     */
    int captive = sysuaf_account_captive(rec);

    /* Exec the DCL shell with --login flag */
    char dcl_linux[1024];
    vmsfs_to_linux_path(DCL_SHELL_PATH, dcl_linux, sizeof(dcl_linux));
    /* ATOMIC FLIP (vms-5f0): LOGINOUT execve's DCL.EXE for the session; the
     * Linux kernel maps its PT_LOAD + PT_INTERP by POSIX path. With the /vms
     * passthrough retired, DCL.EXE is execve'd from the boot-staging tmpfs
     * PID 1 filled off the ODS-2 volume THROUGH the executive ACP. Self-
     * guarding: use the staged copy only if present. */
    {
        char staged[1024];
        if (ovmx_boot_stage_exec_path(dcl_linux, staged, sizeof(staged)) &&
            access(staged, X_OK) == 0)
            snprintf(dcl_linux, sizeof(dcl_linux), "%s", staged);
    }
    if (captive)
        execl(dcl_linux, "vmsdcl", "--login", "--captive",
              "--lgicmd", login_lgicmd, (char *)NULL);
    else
        execl(dcl_linux, "vmsdcl", "--login", "--lgicmd", login_lgicmd,
              (char *)NULL);

    /* If exec fails, fall back to sh */
    perror("vmsdcl");
    fprintf(stderr, "Falling back to /bin/sh\n");
    execl("/bin/sh", "sh", (char *)NULL);
    _exit(1);
}

/* ------------------------------------------------------------------ */
/* Is this LOGINOUT sitting at an operator's terminal?                 */
/* ------------------------------------------------------------------ */
/*
 * THE ASYMMETRY THIS FIXES (vms-3e9, operator-measured on a real VAX boot).
 * The OPA0: wake below used to be gated on isatty(STDIN_FILENO) alone. That
 * predicate is TRUE on the QEMU virtio console the x86_64 and aarch64 rails
 * boot on and FALSE on the SIMH serial line the OVMX/NetBSD-vax rail boots on,
 * so the wake -- and, with it, the type-ahead drain and the ECHO restore that
 * live inside the same guard -- was silently skipped on the VAX and nowhere
 * else. The VAX console printed "Username:" with no RETURN wait, and every
 * proof of the wake was written on an arch where it happened to work: the
 * exact x86_64-green/VAX-broken shape [[asymmetric-arch-red-is-real]].
 *
 * THE FIX IS TO ASK THE EXECUTIVE, NOT THE SUBSTRATE. "Am I an interactive
 * process?" is a VMS question with a VMS answer: an interactive process is one
 * BOUND TO A TERMINAL DEVICE, and since vms-3e9 that binding is established by
 * $CREPRC PRC$M_INTER and RECORDED in the executive's process table
 * ($ASSIGN + VMS_IOCTL_SETTERM, src/libvms/syssvc/sys_process.c
 * creprc_bind_terminal), where $GETJPI reads it back. So LOGINOUT reads its own
 * row: a non-empty JPI terminal name means the session was created ON a
 * terminal device, on every arch, whatever the substrate calls the descriptor.
 * This is the same rule src/vmsdcl/dcl_main.c's comment lays down for the
 * terminal identity -- "the fix is the executive-resident process/terminal
 * binding; anything else is this same defect wearing a different name".
 *
 * isatty() REMAINS AS A SECOND YES, never as the only one. An image started
 * outside $CREPRC's interactive mode (a developer running LOGINOUT.EXE at a
 * shell) has no executive terminal row but is genuinely at a terminal, and
 * must still get the console behaviour. A SCRIPTED LOGINOUT -- the VMS-native
 * login image test, src/imgact/test/run_login_native.sh, which feeds a session
 * file on stdin -- has NEITHER, so it still skips the wake and does not lose
 * its first input line to it, exactly as before.
 */
static int loginout_at_operator_terminal(void)
{
    struct vms_procinfo pi;

    memset(&pi, 0, sizeof(pi));
    if ((vms_kif_getjpi_self(&pi) & 1) && pi.terminal[0] != '\0')
        return 1;

    return isatty(STDIN_FILENO) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Console mode: interactive login with username/password prompts      */
/* ------------------------------------------------------------------ */
static int console_login(void)
{
    char username[64];
    char password[128];
    sysuaf_record_t user_rec;
    int attempts = 0;

    /* Disable stdio buffering on stdin so that unread data remains
     * in the kernel pipe/tty buffer and is available after exec(). */
    setvbuf(stdin, NULL, _IONBF, 0);

    /*
     * OPA0: OPERATOR-CONSOLE WAKE (vms-2213). On the operator console, VMS
     * LOGINOUT does not present the login prompt the instant it starts: the
     * console carries the boot output, and the login session waits for the
     * operator to strike RETURN before it displays the announcement and
     * "Username:" -- the classic "press RETURN to log in" console behaviour
     * (VSI OpenVMS System Manager's Manual, Vol I, "Logging In to the System"
     * / the interactive login sequence on the operator console).
     *
     * This wait also fixes the ordering the boot exposes: JOB_CONTROL forks
     * this LOGINOUT while STARTUP.COM is still running, BEFORE STARTUP.EXE
     * prints its boot identification banner (display_boot_banner() in
     * src/ovmx_init/ovmx_init.c, emitted after run_startup() returns). Without
     * this wait the very first "Username:" raced ahead of that banner and the
     * console showed "Username:" before the banner. Blocking here until the
     * operator's RETURN guarantees the boot banner has finished printing
     * before the prompt appears -- banner first, then Username:, as on VMS.
     *
     * NOTE (vms-dec): that banner-ordering rationale is now STALE. vms-1fb
     * moved the banner to print from PID 1 (print_banner_once() in
     * src/ovmx_init/ovmx_init.c) BEFORE run_startup(), so the banner has
     * already been emitted by the time JOB_CONTROL forks this LOGINOUT. The
     * oracle (docs/design-boot-faithful.md sec3.5) boots straight to
     * "Username:" with no "press RETURN". Removing the wake entirely is the
     * faithful end state, but LOGINOUT runs concurrently with the tail of
     * STARTUP.COM, so a bare removal risks "Username:" interleaving a startup
     * line, and dropping the tcflush below would re-expose the "empty
     * username machine-gun" it guards. That is tracked as a follow-up; this
     * rung keeps the wake.
     *
     * THE NEWLINE-SPAM FIX (vms-dec) does NOT live here: the operator hammers
     * RETURN during the WHOLE slow boot, but LOGINOUT is not forked until
     * STARTUP.COM's tail, so by the time this function runs almost all the
     * mashing has already happened and been echoed by the console tty.
     * Silencing the echo therefore belongs in PID 1, which owns the console
     * for the whole boot (boot_console_disable_echo() in
     * src/ovmx_init/ovmx_init.c). This function's only role in that fix is to
     * RE-ENABLE ECHO just before "Username:" (below), so the operator's typed
     * username shows again.
     *
     * CONSOLE ONLY. The wait is gated on the session being at an operator's
     * TERMINAL -- loginout_at_operator_terminal() above, which asks the
     * EXECUTIVE whether this process is bound to a terminal device and only
     * then falls back to isatty(). $CREPRC PRC$M_INTER created this session on
     * OPA0: and recorded that binding, so the gate is true on the
     * operator-console login path on EVERY arch; the bare isatty() it replaces
     * was true only where the substrate happened to present the console as a
     * tty, which skipped this whole block on the VAX rail. A scripted/piped
     * LOGINOUT (e.g. the VMS-native login image test, src/imgact/test/
     * run_login_native.sh, which feeds a session file on stdin) has neither an
     * executive terminal row nor a tty, has no operator to wait for, and must
     * NOT consume its first input line as the wake keystroke -- so it is still
     * skipped there. A CR/RETURN (or any first line) wakes the session; EOF
     * before that means the connection closed with nobody there, so give up.
     *
     * NO DEADLINE ON THE WAKE, deliberately, unlike the prompts below: a VMS
     * operator console offered but not used is a system waiting for its
     * operator, not an idle login attempt, and timing it out would only make
     * JOB_CONTROL respawn the session forever.
     */
    if (loginout_at_operator_terminal()) {
        char c;
        ssize_t n;

        for (;;) {
            n = read(STDIN_FILENO, &c, 1);
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (n <= 0)
                return 1;              /* EOF: connection closed, nobody there */
            if (c == '\n')
                break;
        }
        /*
         * DISCARD TYPE-AHEAD QUEUED DURING THE (LONG) BOOT (vms-3ab8).
         *
         * The wake design above invites the operator to strike RETURN while
         * the slow console boot is still running -- and an operator naturally
         * hits it several times. Every RETURN typed before this image started
         * reading sits in the terminal's type-ahead buffer; the wake loop
         * consumes exactly one line (up to the first '\n'), leaving the rest.
         * Without this flush those leftover RETURNs are read by the fgets()
         * below as a burst of EMPTY usernames, each of which reprints
         * "Username: " with no wait -- machine-gunning ~20 prompts onto a
         * single line before the terminal finally blocks (the exact defect
         * the operator hit in the 0.4 demo boot). The queued RETURNs were
         * already echoed by the tty at type-time, during the boot output, so
         * the reprompts have no newline of their own and pile up on one line.
         *
         * Flushing the input queue after the single wake keystroke is consumed
         * makes the first real "Username:" prompt block for the operator's
         * NEXT keystroke, as a VMS operator console does. This is OVMX
         * console-handling behaviour (CLAUDE.md Rule 8), not a claimed
         * byte-level VMS terminal-driver detail: stdin is unbuffered
         * (setvbuf _IONBF above), so there is no stdio buffer to reconcile.
         *
         * WHY NOT tcflush() ALONE any more (vms-3e9). tcflush() empties the
         * TERMINAL's input queue and is a no-op on a descriptor the substrate
         * does not present as a terminal -- so on the VAX rail's SIMH serial
         * console the queued RETURNs survived it and the machine-gun came back
         * on one arch only. login_drain_typeahead() still issues the tcflush
         * where there is a tty to flush, and then drains whatever is
         * immediately readable with a zero-timeout poll(), which works the same
         * on a tty, a serial line and a pipe.
         */
        login_drain_typeahead(STDIN_FILENO, 64 * 1024);
        /*
         * RE-ENABLE ECHO for the real "Username:" prompt (vms-dec). PID 1
         * turned the console's ECHO OFF for the whole boot so the operator's
         * RETURN keystrokes, mashed while waiting on the slow console, were not
         * echoed as blank-line "newline spam" (boot_console_disable_echo() in
         * src/ovmx_init/ovmx_init.c). This is the point where the operator's
         * typing must show again, so turn ECHO back on. Idempotent and
         * console-only (inside the operator-terminal guard): a scripted/piped
         * LOGINOUT never reaches here, and on a descriptor with no termios at
         * all the tcgetattr() simply fails and nothing is changed. If the
         * console still had ECHO on (some substrate where PID 1's disable did
         * not take), this is a harmless no-op and the prompt still echoes.
         */
        {
            struct termios t;
            if (tcgetattr(STDIN_FILENO, &t) == 0) {
                t.c_lflag |= (tcflag_t)ECHO;
                tcsetattr(STDIN_FILENO, TCSANOW, &t);
            }
        }
    }

    /*
     * THE SYSTEM-IDENTIFICATION LINE, ONCE, IMMEDIATELY BEFORE "Username:"
     * (vms-3e9). The oracle console prints it exactly here -- identification,
     * blank line, prompt (docs/design-boot-faithful.md §3.5) -- and OVMX
     * printed NOTHING here on any arch: the only identity a user saw before
     * logging in was the boot banner PID 1 emitted minutes earlier, which has
     * usually scrolled away by the time the prompt appears, and on a
     * re-offered session (JOB_CONTROL creating the next one after a logout or
     * a timeout) it never appears at all.
     *
     * THREE DISTINCT EMISSIONS, one each: the boot banner (PID 1,
     * display_boot_banner()), this identification line (LOGINOUT, here) and
     * the post-authentication SYS$WELCOME (start_session(), below). See
     * loginout_display.h for the oracle citation, the INV-0 wording and why
     * this line must not be confusable with the SYS$WELCOME one.
     *
     * EVERY VALUE COMES FROM THE IDENTITY SSOT (INV-1): the product name, the
     * architecture this build actually runs on and the product version are
     * read from ovmx_identity.h's accessors. Nothing here knows a version.
     */
    loginout_display_system_identification(stdout, OVMX_PRODUCT_NAME,
                                           ovmx_hw_arch(),
                                           ovmx_product_version(),
                                           OVMX_COMPAT_BADGE);

    /* SYS$ANNOUNCE -- the SITE's own announcement, displayed once before the
     * first Username: prompt. Undefined by default, in which case nothing is
     * printed (VMS). */
    ovmx_banner_announce(stdout);

    while (attempts < MAX_ATTEMPTS) {
        /* Prompt for username. Bounded by the LGI-style idle deadline: on
         * expiry the session is disconnected silently (read_prompt_response). */
        printf("Username: ");
        fflush(stdout);
        if (read_prompt_response(username, sizeof(username), 0) != LOGIN_READ_OK)
            return 1;  /* EOF, or the idle deadline expired */
        str_upcase(username);

        if (username[0] == '\0')
            continue;

        /* Prompt for password (echo suppressed), same deadline. */
        printf("Password: ");
        fflush(stdout);
        if (read_prompt_response(password, sizeof(password), 1) != LOGIN_READ_OK)
            return 1;

        /*
         * "User authorization failure" -- SAME TEXT for an unknown
         * username and a wrong password, DELIBERATELY, on both branches
         * below. That is not this file's invention: VMS uses one
         * message for both causes precisely so a failed login cannot be
         * used to learn which half was wrong (username enumeration).
         *
         * PINNED, per Rule 10 ("do not invent ... verify it" -- this
         * item's own scope note), not carried over unexamined from
         * whoever wrote it first. The ~/vax/cluster lab was mid-use for
         * an unrelated cluster experiment for the whole of this item
         * (reset3.sh/nodedrv.py running against all three nodes,
         * `ps aux` checked before touching anything) and Rule 10
         * explicitly allows citing public documentation instead of
         * taking it. Verbatim Example 6 of the VMS Help "Login"
         * reference (a mirrored HP/Compaq OpenVMS documentation page,
         * https://marc.vos.net/books/vms/help/login/ -- OpenVMS Alpha
         * V6.2, node LSR), console transcript:
         *
         *     Username: JONES
         *     Password: <PASSWORD>
         *     User authorization failure
         *      <Return>
         *     Username: JONES
         *
         * -- the exact text, lowercase after the first letter, one line,
         * immediately after the failed Password: entry and before the
         * next Username: prompt. This is a WEB-SOURCED public-doc pin,
         * not a lab capture; if the lab is later free, re-verifying
         * against a live ~/vax console transcript is still worth doing
         * and would supersede this citation, not contradict it.
         */
        /* Look up user */
        memset(&user_rec, 0, sizeof(user_rec));
        uint32_t uaf_st = 0;
        if (sysuaf_lookup_st(username, &user_rec, &uaf_st) < 0) {
            /*
             * FAIL-HONESTY (vms-3b0). A privilege- or access-denied SYSUAF
             * read (RMS$_PRV, RMS$_FNF, ... -- the authorization file itself
             * could not be read) is a SYSTEM fault, not a per-user decision,
             * and collapsing it into the generic failure MASKED the real
             * cause during the vms-d31d/#766 VAX-login diagnosis. Surface the
             * AUTHENTIC %RMS- status on SYS$ERROR so a privilege denial is
             * diagnosable.
             *
             * A record-not-found (RMS$_RNF = no such user) is deliberately
             * NOT surfaced: it stays indistinguishable from a wrong password
             * (the generic "User authorization failure" below), so a failed
             * login cannot reveal whether the account exists (OpenVMS Guide to
             * System Security, "Disusering Accounts"). Only a whole-file read
             * fault -- identical for EVERY username -- is surfaced, so the
             * security invariant is not weakened.
             */
            if (uaf_st != 0 && uaf_st != RMS$_RNF) {
                char rmsmsg[256];
                vms_status_string(uaf_st, rmsmsg, sizeof(rmsmsg));
                fprintf(stderr, "%s\n", rmsmsg);
            }
            printf("\nUser authorization failure\n\n");
            attempts++;
            continue;
        }

        /* Authenticate */
        if (!sysuaf_authenticate(&user_rec, password)) {
            printf("\nUser authorization failure\n\n");
            attempts++;
            continue;
        }

        /*
         * LOGIN-FLAG ENFORCEMENT (vms-c8fa). The password is correct; that is
         * NOT sufficient. OpenVMS gates login on the SYSUAF login flags as a
         * step distinct from the password check, so a disabled account
         * (DISUSER / DISACNT) does not reach a session no matter what password
         * is typed. Refused with the SAME "User authorization failure" text
         * the wrong-password branch above uses -- deliberately, so a failed
         * login cannot be used to learn whether the account exists, is
         * disabled, or the password was wrong (OpenVMS Guide to System
         * Security, "Disusering Accounts"). The decision lives in the SYSUAF
         * library (sysuaf_interactive_login_permitted) and is fail-closed:
         * this gate stands BEFORE start_session(), so a disabled account
         * never sees the welcome banner, the identity stamp, or the "$"
         * prompt. CAPTIVE and expired-password are NOT denials -- they
         * constrain a permitted session and are handled in start_session().
         */
        if (!sysuaf_interactive_login_permitted(&user_rec)) {
            printf("\nUser authorization failure\n\n");
            attempts++;
            continue;
        }

        /* --- Authentication successful --- */
        /* 'attempts' is the number of failed tries before this success --
         * the real "failures since last successful login" for this
         * connection (vms-417). */
        start_session(&user_rec, (unsigned)attempts);
        return 1;  /* Should not reach here */
    }

    /*
     * MAX_ATTEMPTS reached: disconnect SILENTLY. VMS LOGINOUT prints no
     * "maximum attempts exceeded" farewell -- it simply drops the connection
     * after the last "User authorization failure". The invented sign-off line
     * that used to stand here was a self-certified VMS message (CLAUDE.md
     * Rule 10 / vms-417); removed, not reworded.
     */
    return 1;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* Bootstrap VMS namespace — each exec'd process needs its own
     * device table + LNM since these are in-process state. */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    return console_login();
}
