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

    /* Set up environment for session.
     * VMS_DEFAULT_DIR is a VMS directory spec from SYSUAF. */
    setenv("VMS_USERNAME",    rec->username,    1);

    char uic_group_str[16], uic_member_str[16];
    snprintf(uic_group_str, sizeof(uic_group_str), "%u", rec->uic_group);
    snprintf(uic_member_str, sizeof(uic_member_str), "%u", rec->uic_member);
    setenv("VMS_UIC_GROUP",   uic_group_str, 1);
    setenv("VMS_UIC_MEMBER",  uic_member_str, 1);
    setenv("VMS_DEFAULT_DIR", rec->default_dir, 1);
    setenv("VMS_PRIVILEGES",  rec->privileges, 1);

    /* Build logical name equivalences — VMS directory specs */
    setenv("SYS$LOGIN",   rec->default_dir, 1);
    setenv("SYS$SCRATCH", "SYS$SYSDEVICE:[SYSTMP]", 1);

    /* chdir into the VMS tree so DCL inherits a VMS-rooted cwd.
     * Translate the VMS directory spec to Linux for the syscall. */
    char home_linux[512];
    if (vmsfs_to_linux_path(rec->default_dir, home_linux, sizeof(home_linux)) == 1) {
        /* Ensure home directory exists */
        mkdir(home_linux, 0755);
        chdir(home_linux);
    }

    /* Initialize user PCB (lives until exec replaces address space) */
    uint64_t user_privs = parse_privilege_string(rec->privileges);
    struct vms_pcb *pcb = vms_pcb_init(user_privs);
    if (pcb) {
        uint32_t uic = (rec->uic_group << 16) | rec->uic_member;
        char prcnam[16];
        snprintf(prcnam, sizeof(prcnam), "_FTA%d:", (int)(getpid() % 100));
        vms_pcb_set_identity((uint32_t)getpid(), uic, rec->username, prcnam);
        vms_pcb_set_default_dir(rec->default_dir);
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
