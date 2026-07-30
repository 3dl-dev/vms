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
/* Read password with echo disabled                                   */
/* ------------------------------------------------------------------ */
static int read_password(char *buf, size_t bufsiz)
{
    struct termios old_term, new_term;
    int have_term = 0;

    /* Turn off echo (only if stdin is a terminal) */
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old_term) == 0) {
        new_term = old_term;
        new_term.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_term);
        have_term = 1;
    }

    /* Read the line */
    if (fgets(buf, (int)bufsiz, stdin) == NULL) {
        if (have_term)
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
        return -1;
    }

    /* Restore terminal */
    if (have_term) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
        /* Print a newline (since echo was off) */
        putchar('\n');
    }

    str_trim(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Start a VMS session: banner, environment, exec DCL shell           */
/* ------------------------------------------------------------------ */
static void start_session(const sysuaf_record_t *rec)
{
    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    /* SYS$WELCOME (a boot-defined logical), falling back to the built-in
     * badged OVMX identity when undefined -- as LOGINOUT does on VMS. */
    printf("\n");
    ovmx_banner_welcome(stdout);

    /* Read the REAL last login time from accounting records */
    time_t last_login = 0;
    int has_last = (ovmx_accounting_get_lastlogin(rec->username, &last_login) == 0);

    if (has_last && last_login > 0) {
        struct tm *tm = localtime(&last_login);
        if (tm) {
            printf("\n   Last interactive login on %02d-%s-%04d %02d:%02d:%02d\n\n",
                   tm->tm_mday, months[tm->tm_mon], tm->tm_year + 1900,
                   tm->tm_hour, tm->tm_min, tm->tm_sec);
        } else {
            printf("\n   Last login time could not be determined.\n\n");
        }
    } else {
        printf("\n   No previous interactive login recorded.\n\n");
    }

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
        uint64_t login_privs = parse_privilege_string(rec->privileges);
        uint32_t ist = vms_kif_setident(rec->username, login_uic, login_privs);
        if (!(ist & 1)) {
            printf("%%OVMX-F-NOIDENT, the executive refused the "
                   "authenticated identity (status %u)\n", (unsigned)ist);
            fflush(stdout);
            _exit(1);
        }
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

    /* Build logical name equivalences — VMS directory specs */
    setenv("SYS$LOGIN",   rec->default_dir, 1);
    setenv("SYS$SCRATCH", "SYS$SYSDEVICE:[SYSTMP]", 1);

    /* chdir into the VMS tree so DCL inherits a VMS-rooted cwd.
     * Translate the VMS directory spec to Linux for the syscall. */
    char home_linux[512];
    if (vmsfs_to_linux_path(rec->default_dir, home_linux, sizeof(home_linux)) == 1) {
        /* Ensure home directory exists */
        mkdir(home_linux, 0755);
        /* SYS$LOGIN is owned by the user's UIC on VMS, and the UIC is
         * [gid,uid] here (the same mapping the executive derives and
         * that src/vmsrms/rms_core.c enforces protection against). This
         * must happen while LOGINOUT still has the privilege to do it,
         * i.e. before the credential drop below. */
        if (chown(home_linux, (uid_t)rec->uic_member,
                  (gid_t)rec->uic_group) != 0)
            printf("%%OVMX-W-LOGINOWN, %s could not be given to UIC "
                   "[%o,%o]: %s\n", rec->default_dir,
                   (unsigned)rec->uic_group, (unsigned)rec->uic_member,
                   strerror(errno));
        chdir(home_linux);
    }

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
     * root: each child registered holding CMKRNL|CMEXEC|SETPRV|WORLD
     * before it executed a single instruction, and SETPRV is exactly
     * what VMS_IOCTL_SETIDENT requires to establish an arbitrary
     * identity. It was proven by execution, not argued: an ordinary
     * FIELD/[200,10] session forked a child, the child re-registered,
     * and it stamped itself SYSTEM [1,4] with all 37 privileges. The
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
     * uid/gid pair is OVMX's substrate for the UIC. What changes here
     * is only that the substrate is made to agree with the identity the
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

    /* Exec the DCL shell with --login flag */
    char dcl_linux[1024];
    vmsfs_to_linux_path(DCL_SHELL_PATH, dcl_linux, sizeof(dcl_linux));
    execl(dcl_linux, "vmsdcl", "--login", (char *)NULL);

    /* If exec fails, fall back to sh */
    perror("vmsdcl");
    fprintf(stderr, "Falling back to /bin/sh\n");
    execl("/bin/sh", "sh", (char *)NULL);
    _exit(1);
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

    /* SYS$ANNOUNCE -- displayed once before the first Username: prompt.
     * Undefined by default, in which case nothing is printed (VMS). */
    ovmx_banner_announce(stdout);

    while (attempts < MAX_ATTEMPTS) {
        /* Prompt for username */
        printf("Username: ");
        fflush(stdout);
        if (fgets(username, sizeof(username), stdin) == NULL)
            return 1;  /* EOF */
        str_trim(username);
        str_upcase(username);

        if (username[0] == '\0')
            continue;

        /* Prompt for password */
        printf("Password: ");
        fflush(stdout);
        if (read_password(password, sizeof(password)) < 0)
            return 1;

        /* Look up user */
        memset(&user_rec, 0, sizeof(user_rec));
        if (sysuaf_lookup(username, &user_rec) < 0) {
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

        /* --- Authentication successful --- */
        start_session(&user_rec);
        return 1;  /* Should not reach here */
    }

    printf("\nMaximum login attempts exceeded. Disconnecting.\n");
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
