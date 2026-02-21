/*
 * dcl_builtin.c - DCL Built-in Command Implementations
 *
 * Implements all the built-in DCL commands with authentic VMS
 * look and feel. This is the heart of the user experience.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/utsname.h>
#include <utmpx.h>
#include <limits.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/cdu.h"
#include "ssdef.h"
#include "opcdef.h"
#include "ovmx_accounting.h"
#include "starlet.h"

/* External functions */
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);
extern int dcl_resolve_path(struct dcl_context *ctx, const char *spec,
                            char *linux_path, size_t path_size);
extern int dcl_format_directory(const char *linux_path, char *vms_dir,
                                size_t dir_size);
extern int dcl_format_filespec(const char *linux_path, char *vms_spec,
                               size_t spec_size);
extern int dcl_translate_logical(const char *name, char *result,
                                 size_t result_size);
extern int dcl_eval_lexical(struct dcl_context *ctx, const char *expr,
                            char *result, size_t result_size);
extern int dcl_execute_line(const char *line);
extern int dcl_execute_script(const char *filename, int argc, char **argv);
extern int dcl_read_input(struct dcl_context *ctx, const char *prompt,
                          char *buffer, size_t bufsize);

/* VMS filesystem protection functions */
extern int      vmsfs_parse_protection(const char *str, uint16_t *prot);
extern int      vmsfs_format_protection(uint16_t prot, char *buf, size_t bufsize);
extern mode_t   vmsfs_protection_to_mode(uint16_t vms_prot);
extern uint16_t vmsfs_mode_to_protection(mode_t mode);

/* VMS month abbreviations */
static const char *vms_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

/* ================================================================== */
/*                          SHOW Commands                              */
/* ================================================================== */

/*
 * SHOW TIME - Display current date and time in VMS format.
 */
static int cmd_show_time(struct dcl_command *cmd)
{
    (void)cmd;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int centisec = (int)(ts.tv_nsec / 10000000);

    printf("  %2d-%s-%04d %02d:%02d:%02d.%02d\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec, centisec);

    return SS$_NORMAL;
}

/*
 * SHOW DEFAULT - Display current default directory.
 */
static int cmd_show_default(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();

    char vms_dir[512];
    dcl_format_directory(ctx->default_linux, vms_dir, sizeof(vms_dir));
    printf("  %s\n", vms_dir);

    return SS$_NORMAL;
}

/*
 * SHOW LOGICAL - Display logical name translations.
 */
static int cmd_show_logical(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* Well-known system logicals to display */
    static const struct {
        const char *name;
        const char *table;
    } known_logicals[] = {
        { "SYS$DISK",     "LNM$PROCESS_TABLE" },
        { "SYS$LOGIN",    "LNM$JOB" },
        { "SYS$SCRATCH",  "LNM$JOB" },
        { "SYS$SYSTEM",   "LNM$SYSTEM" },
        { "SYS$LIBRARY",  "LNM$SYSTEM" },
        { "SYS$HELP",     "LNM$SYSTEM" },
        { "SYS$INPUT",    "LNM$PROCESS_TABLE" },
        { "SYS$OUTPUT",   "LNM$PROCESS_TABLE" },
        { "SYS$ERROR",    "LNM$PROCESS_TABLE" },
        { "SYS$COMMAND",  "LNM$PROCESS_TABLE" },
        { NULL, NULL }
    };

    if (cmd->param_count >= 2) {
        /* SHOW LOGICAL specific-name: Look up in param[1] */
        /* Note: param[0] is the subcommand "LOGICAL" (from SHOW LOGICAL) */
        const char *logname = cmd->params[1];
        char value[256];
        if (dcl_translate_logical(logname, value, sizeof(value)) == 0) {
            /* Uppercase the name for display */
            char upper_name[256];
            size_t i;
            for (i = 0; i < sizeof(upper_name) - 1 && logname[i]; i++)
                upper_name[i] = (char)toupper((unsigned char)logname[i]);
            upper_name[i] = '\0';

            printf("   \"%s\" = \"%s\" (LNM$PROCESS_TABLE)\n", upper_name, value);
        } else {
            dcl_error("DCL", 0, "NOLOG",
                      "no logical name match");
            return SS$_NOLOGNAM;
        }
    } else {
        /* Show all known logicals */
        printf("(LNM$PROCESS_TABLE)\n\n");
        for (int i = 0; known_logicals[i].name; i++) {
            char value[256];
            if (dcl_translate_logical(known_logicals[i].name,
                                      value, sizeof(value)) == 0) {
                printf("   \"%s\" = \"%s\" (%s)\n",
                       known_logicals[i].name, value,
                       known_logicals[i].table);
            }
        }

        /* Also show SYS$DISK pointing to current default */
        char vms_dir[512];
        dcl_format_directory(ctx->default_linux, vms_dir, sizeof(vms_dir));
        (void)vms_dir;
    }

    return SS$_NORMAL;
}

/*
 * SHOW SYSTEM - Show process list (like VMS SHOW SYSTEM).
 */
static int cmd_show_system(struct dcl_command *cmd)
{
    (void)cmd;

    struct utsname uts;
    uname(&uts);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    printf("OpenVMS V7.3  on node %s  %2d-%s-%04d %02d:%02d:%02d.%02d"
           "  Uptime  ---\n",
           uts.nodename, tm.tm_mday, vms_months[tm.tm_mon],
           1900 + tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));
    printf("  Pid    Process Name    State  Pri      I/O       CPU"
           "       Page flts  Pages\n");
    printf(" %08X %-15s %s %3d %9d  %s  %9d  %5d\n",
           (unsigned)getpid(), "OVMX", "HIB", 4, 0,
           "0 00:00:00.00", 0, 0);

    /* Read /proc to list processes */
    DIR *proc_dir = opendir("/proc");
    if (proc_dir) {
        struct dirent *entry;
        int count = 0;
        while ((entry = readdir(proc_dir)) != NULL && count < 20) {
            /* Only process numeric directories */
            if (!isdigit((unsigned char)entry->d_name[0])) continue;

            int pid = atoi(entry->d_name);
            if (pid <= 0) continue;

            /* Read process name from /proc/pid/comm */
            char path[256];
            snprintf(path, sizeof(path), "/proc/%d/comm", pid);
            FILE *fp = fopen(path, "r");
            if (!fp) continue;

            char pname[64];
            if (!fgets(pname, sizeof(pname), fp)) {
                fclose(fp);
                continue;
            }
            fclose(fp);

            /* Remove newline */
            size_t plen = strlen(pname);
            if (plen > 0 && pname[plen - 1] == '\n') pname[plen - 1] = '\0';

            /* Truncate to 15 chars */
            if (strlen(pname) > 15) pname[15] = '\0';

            /* Uppercase */
            for (size_t i = 0; pname[i]; i++)
                pname[i] = (char)toupper((unsigned char)pname[i]);

            printf(" %08X %-15s %s %3d %9d  %s  %9d  %5d\n",
                   (unsigned)pid, pname, "COM", 4, 0,
                   "0 00:00:00.00", 0, 0);
            count++;
        }
        closedir(proc_dir);
    }

    return SS$_NORMAL;
}

/*
 * SHOW PROCESS - Show current process information.
 */
static int cmd_show_process(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    const char *upper_user = ctx->username[0] ? ctx->username : "SYSTEM";

    printf("%2d-%s-%04d %02d:%02d:%02d.%02d   User: %-12s"
           "  Process ID:   %08X\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000),
           upper_user, (unsigned)getpid());
    printf("                          Process name: \"%s\"\n\n",
           ctx->process_name[0] ? ctx->process_name : "_FTA0:");
    printf("Terminal:          /dev/tty\n");
    printf("User Identifier:   [%03o,%03o]\n",
           ctx->uic_group ? (unsigned)ctx->uic_group : (unsigned)(getgid() & 0377),
           ctx->uic_member ? (unsigned)ctx->uic_member : (unsigned)(getuid() & 0377));
    printf("Base priority:     4\n");
    printf("Default file spec: ");

    char vms_dir[512];
    dcl_format_directory(ctx->default_linux, vms_dir, sizeof(vms_dir));
    printf("%s\n", vms_dir);

    return SS$_NORMAL;
}

/*
 * SHOW USERS - Show logged-in users.
 */
static int cmd_show_users(struct dcl_command *cmd)
{
    (void)cmd;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    struct utsname uts;
    uname(&uts);

    printf("      OpenVMS User Processes at %2d-%s-%04d %02d:%02d:%02d.%02d\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 10000000));
    printf("    Total number of users = ");

    /* Count logged-in users from utmpx */
    int user_count = 0;
    struct utmpx *ut;
    setutxent();
    printf("\n    Username     Node         Interactive  Subprocess   Batch\n");
    while ((ut = getutxent()) != NULL) {
        if (ut->ut_type == USER_PROCESS) {
            user_count++;
            char upper_name[64];
            size_t i;
            for (i = 0; i < sizeof(upper_name) - 1 && ut->ut_user[i]; i++)
                upper_name[i] = (char)toupper((unsigned char)ut->ut_user[i]);
            upper_name[i] = '\0';

            printf("    %-12s %-12s      %d\n", upper_name, uts.nodename, 1);
        }
    }
    endutxent();

    if (user_count == 0) {
        /* Show at least the current user */
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            char upper_name[64];
            size_t i;
            for (i = 0; i < sizeof(upper_name) - 1 && pw->pw_name[i]; i++)
                upper_name[i] = (char)toupper((unsigned char)pw->pw_name[i]);
            upper_name[i] = '\0';
            printf("    %-12s %-12s      1\n", upper_name, uts.nodename);
            user_count = 1;
        }
    }

    /* Go back and fill in the total (we already printed the header) */
    printf("    Total: %d user%s\n", user_count, user_count != 1 ? "s" : "");

    return SS$_NORMAL;
}

/* Callbacks for SHOW SYMBOL enumeration */
static int show_local_sym_cb(const char *name, const char *value,
                              int scope, void *ctx)
{
    (void)scope; (void)ctx;
    printf("  %s = \"%s\"\n", name, value);
    return 0;
}

static int show_global_sym_cb(const char *name, const char *value,
                               int scope, void *ctx)
{
    (void)scope; (void)ctx;
    printf("  %s == \"%s\"\n", name, value);
    return 0;
}

/*
 * SHOW SYMBOL - Display symbol value(s).
 */
static int cmd_show_symbol(struct dcl_command *cmd)
{
    if (cmd->param_count >= 2) {
        /* SHOW SYMBOL specific-name */
        const char *name = cmd->params[1];
        const char *value = dcl_sym_get(name);
        if (value) {
            /* Uppercase symbol name for display */
            char upper_name[256];
            size_t i;
            for (i = 0; i < sizeof(upper_name) - 1 && name[i]; i++)
                upper_name[i] = (char)toupper((unsigned char)name[i]);
            upper_name[i] = '\0';

            /* Try to determine if integer */
            char *endp;
            long v = strtol(value, &endp, 0);
            if (*endp == '\0' && value[0] != '\0') {
                printf("  %s = %ld   Hex = %08lX  Octal = %012lo\n",
                       upper_name, v, v, v);
            } else {
                printf("  %s = \"%s\"\n", upper_name, value);
            }
        } else {
            dcl_error("DCL", 0, "NOLCL",
                      "no symbol \"%s\" found", name);
            return SS$_NOLOGNAM;
        }
    } else {
        /* Show all symbols */
        dcl_sym_enumerate(DCL_SYM_LOCAL, show_local_sym_cb, NULL);
        dcl_sym_enumerate(DCL_SYM_GLOBAL, show_global_sym_cb, NULL);
    }

    return SS$_NORMAL;
}

/*
 * SHOW VERIFY - Display verification state.
 */
static int cmd_show_verify(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();
    printf("  VERIFY = %s\n", ctx->verify ? "ON" : "OFF");
    return SS$_NORMAL;
}

/*
 * SHOW PROTECTION - Display process default protection.
 */
static int cmd_show_protection(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();
    char prot_buf[64];
    vmsfs_format_protection(ctx->default_protection, prot_buf, sizeof(prot_buf));
    printf("  SYSTEM default protection: %s\n", prot_buf);
    return SS$_NORMAL;
}

/* ================================================================== */
/*                          SHOW Dispatcher                            */
/* ================================================================== */

static int cmd_show(struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("DCL", 2, "NOKEYW", "missing keyword - supply what you want to show");
        return SS$_BADPARAM;
    }

    const char *subcmd = cmd->params[0];

    if (dcl_match_command(subcmd, "TIME", 1))
        return cmd_show_time(cmd);
    if (dcl_match_command(subcmd, "DEFAULT", 3))
        return cmd_show_default(cmd);
    if (dcl_match_command(subcmd, "LOGICAL", 3))
        return cmd_show_logical(cmd);
    if (dcl_match_command(subcmd, "SYSTEM", 3))
        return cmd_show_system(cmd);
    if (dcl_match_command(subcmd, "PROCESS", 3))
        return cmd_show_process(cmd);
    if (dcl_match_command(subcmd, "USERS", 1))
        return cmd_show_users(cmd);
    if (dcl_match_command(subcmd, "SYMBOL", 3))
        return cmd_show_symbol(cmd);
    if (dcl_match_command(subcmd, "VERIFY", 3))
        return cmd_show_verify(cmd);
    if (dcl_match_command(subcmd, "PROTECTION", 3))
        return cmd_show_protection(cmd);

    dcl_error("DCL", 2, "IVKEYW", "unrecognized SHOW keyword - \\%s\\", subcmd);
    return SS$_IVKEYW;
}

/* ================================================================== */
/*                          SET Commands                                */
/* ================================================================== */

/*
 * SET DEFAULT - Change default directory.
 */
static int cmd_set_default(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NODIR", "missing directory specification");
        return SS$_BADPARAM;
    }

    const char *dirspec = cmd->params[1];
    char linux_path[1024];

    dcl_resolve_path(ctx, dirspec, linux_path, sizeof(linux_path));

    /* Remove trailing slash for stat, but keep it for storage */
    char check_path[1024];
    strncpy(check_path, linux_path, sizeof(check_path) - 1);
    check_path[sizeof(check_path) - 1] = '\0';
    size_t cplen = strlen(check_path);
    if (cplen > 1 && check_path[cplen - 1] == '/') {
        check_path[cplen - 1] = '\0';
    }

    struct stat st;
    if (stat(check_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        dcl_error("DCL", 2, "DIRECT", "invalid directory - \\%s\\", dirspec);
        return SS$_NOSUCHFILE;
    }

    /* Get absolute path */
    char abs_path[PATH_MAX];
    if (realpath(check_path, abs_path) != NULL) {
        strncpy(ctx->default_linux, abs_path, sizeof(ctx->default_linux) - 1);
        ctx->default_linux[sizeof(ctx->default_linux) - 1] = '\0';
    } else {
        strncpy(ctx->default_linux, check_path, sizeof(ctx->default_linux) - 1);
        ctx->default_linux[sizeof(ctx->default_linux) - 1] = '\0';
    }

    /* Also update the VMS format default */
    dcl_format_directory(ctx->default_linux, ctx->default_dir,
                         sizeof(ctx->default_dir));

    /* Change the process working directory too */
    if (chdir(ctx->default_linux) != 0) {
        /* Non-fatal - VMS default and Linux CWD diverge */
    }

    return SS$_NORMAL;
}

/*
 * SET PROMPT - Change the interactive prompt.
 */
static int cmd_set_prompt(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW", "missing prompt string");
        return SS$_BADPARAM;
    }

    strncpy(ctx->prompt, cmd->params[1], sizeof(ctx->prompt) - 1);
    ctx->prompt[sizeof(ctx->prompt) - 1] = '\0';

    return SS$_NORMAL;
}

/*
 * SET VERIFY / SET NOVERIFY
 */
static int cmd_set_verify(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count >= 1) {
        if (strcasecmp(cmd->params[0], "VERIFY") == 0) {
            ctx->verify = 1;
        } else if (strcasecmp(cmd->params[0], "NOVERIFY") == 0) {
            ctx->verify = 0;
        }
    }

    return SS$_NORMAL;
}

/*
 * SET TERMINAL - Stub (acknowledge but no-op).
 */
static int cmd_set_terminal(struct dcl_command *cmd)
{
    (void)cmd;
    /* Silently succeed - terminal settings are managed by Linux */
    return SS$_NORMAL;
}

/*
 * SET PROTECTION - Set file protection.
 * Format: SET PROTECTION (S:RWED,O:RW,G:R,W:) filespec
 */
static int cmd_set_protection(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 3) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing protection string and/or file specification");
        return SS$_BADPARAM;
    }

    /* param[0] = "PROTECTION", param[1] = protection string, param[2] = filespec */
    const char *prot_str = cmd->params[1];
    const char *filespec = cmd->params[2];

    uint16_t vprot;
    if (vmsfs_parse_protection(prot_str, &vprot) != SS$_NORMAL) {
        dcl_error("DCL", 2, "BADPROT", "invalid protection string - %s", prot_str);
        return SS$_BADPARAM;
    }

    mode_t new_mode = vmsfs_protection_to_mode(vprot);

    char linux_path[1024];
    dcl_resolve_path(ctx, filespec, linux_path, sizeof(linux_path));

    if (chmod(linux_path, new_mode) != 0) {
        dcl_error("RMS", 2, "PRV", "failed to set protection - %s", strerror(errno));
        return SS$_NOPRIV;
    }

    return SS$_NORMAL;
}

/*
 * SET PASSWORD - Change user password (interactive prompts).
 */
static int cmd_set_password(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();

    printf("%%SET-I-PASSWORD, password change not fully implemented\n");
    printf("  User: %s\n", ctx->username);
    printf("  Full SYSUAF.DAT rewrite is planned for a future release.\n");

    return SS$_NORMAL;
}

/*
 * SET Dispatcher
 */
static int cmd_set(struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("DCL", 2, "NOKEYW", "missing keyword - supply what you want to set");
        return SS$_BADPARAM;
    }

    const char *subcmd = cmd->params[0];

    if (dcl_match_command(subcmd, "DEFAULT", 3))
        return cmd_set_default(cmd);
    if (dcl_match_command(subcmd, "PROMPT", 3))
        return cmd_set_prompt(cmd);
    if (dcl_match_command(subcmd, "VERIFY", 3) ||
        dcl_match_command(subcmd, "NOVERIFY", 3))
        return cmd_set_verify(cmd);
    if (dcl_match_command(subcmd, "TERMINAL", 3))
        return cmd_set_terminal(cmd);
    if (dcl_match_command(subcmd, "PROTECTION", 3))
        return cmd_set_protection(cmd);
    if (dcl_match_command(subcmd, "PASSWORD", 3))
        return cmd_set_password(cmd);

    dcl_error("DCL", 2, "IVKEYW", "unrecognized SET keyword - \\%s\\", subcmd);
    return SS$_IVKEYW;
}

/* ================================================================== */
/*                       File Operations                               */
/* ================================================================== */

/*
 * DIRECTORY - List files in VMS format.
 */
static int cmd_directory(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* Determine the directory to list */
    char linux_dir[1024];
    const char *pattern = NULL;

    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        dcl_resolve_path(ctx, cmd->params[0], linux_dir, sizeof(linux_dir));
        /* Check if this is a directory or a file pattern */
        struct stat st;
        if (stat(linux_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* It's a directory */
        } else {
            /* Might be a wildcard pattern - split dir and pattern */
            char *last_slash = strrchr(linux_dir, '/');
            if (last_slash) {
                pattern = strdup(last_slash + 1);
                *(last_slash + 1) = '\0';
            } else {
                pattern = strdup(linux_dir);
                strncpy(linux_dir, ctx->default_linux,
                        sizeof(linux_dir) - 1);
            }
        }
    } else {
        strncpy(linux_dir, ctx->default_linux, sizeof(linux_dir) - 1);
        linux_dir[sizeof(linux_dir) - 1] = '\0';
    }

    /* Ensure trailing slash */
    size_t dlen = strlen(linux_dir);
    if (dlen > 0 && linux_dir[dlen - 1] != '/') {
        if (dlen < sizeof(linux_dir) - 1) {
            linux_dir[dlen] = '/';
            linux_dir[dlen + 1] = '\0';
        }
    }

    /* Check qualifiers */
    int show_size = dcl_has_qualifier(cmd, "SIZE");
    int show_date = dcl_has_qualifier(cmd, "DATE");
    int show_full = dcl_has_qualifier(cmd, "FULL");
    int show_brief = dcl_has_qualifier(cmd, "BRIEF");
    int columns = 4;
    const char *col_val = dcl_qualifier_value(cmd, "COLUMNS");
    if (col_val && col_val[0]) columns = atoi(col_val);
    if (columns < 1) columns = 1;
    if (columns > 8) columns = 8;

    if (show_full) {
        show_size = 1;
        show_date = 1;
    }

    /* Display header */
    char vms_dir[512];
    /* Remove trailing slash for display */
    char display_dir[1024];
    strncpy(display_dir, linux_dir, sizeof(display_dir) - 1);
    display_dir[sizeof(display_dir) - 1] = '\0';
    size_t ddlen = strlen(display_dir);
    if (ddlen > 1 && display_dir[ddlen - 1] == '/') {
        display_dir[ddlen - 1] = '\0';
    }
    dcl_format_directory(display_dir, vms_dir, sizeof(vms_dir));
    printf("\nDirectory %s\n\n", vms_dir);

    /* Read directory entries */
    DIR *dir = opendir(linux_dir);
    if (!dir) {
        dcl_error("RMS", 2, "DNF",
                  "directory not found - %s", linux_dir);
        if (pattern) free((void *)pattern);
        return SS$_NOSUCHFILE;
    }

    struct dirent *entry;
    int file_count = 0;
    long total_blocks = 0;
    int col = 0;
    int col_width = (show_size || show_date) ? 0 : (80 / columns);

    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;

        /* Apply wildcard filter if pattern specified */
        if (pattern) {
            if (fnmatch(pattern, entry->d_name, FNM_CASEFOLD) != 0) continue;
        }

        /* Stat the file */
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s%s", linux_dir, entry->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        /* Format the filename in VMS style (uppercase, add version) */
        char vms_name[256];
        size_t ni = 0;
        for (size_t i = 0; entry->d_name[i] && ni < sizeof(vms_name) - 1; i++) {
            vms_name[ni++] = (char)toupper((unsigned char)entry->d_name[i]);
        }
        vms_name[ni] = '\0';

        /* Add ;1 version number for regular files */
        if (S_ISREG(st.st_mode)) {
            strncat(vms_name, ";1", sizeof(vms_name) - strlen(vms_name) - 1);
        }

        /* Add .DIR;1 suffix for directories */
        if (S_ISDIR(st.st_mode)) {
            strncat(vms_name, ".DIR;1", sizeof(vms_name) - strlen(vms_name) - 1);
        }

        /* Ensure the name has an extension dot for regular files */
        if (S_ISREG(st.st_mode) && !strchr(entry->d_name, '.')) {
            /* Insert .  before ;1 */
            char *semi = strrchr(vms_name, ';');
            if (semi) {
                memmove(semi + 1, semi, strlen(semi) + 1);
                *semi = '.';
            }
        }

        long blocks = (st.st_size + 511) / 512;
        total_blocks += blocks;
        file_count++;

        if (show_full) {
            /* Full listing: one file per line with all info */
            printf("%-39s", vms_name);
            printf(" %6ld", blocks);

            struct tm tm;
            localtime_r(&st.st_mtime, &tm);
            printf("  %2d-%s-%04d %02d:%02d:%02d.00",
                   tm.tm_mday, vms_months[tm.tm_mon],
                   1900 + tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec);

            /* Protection: use vmsfs functions for proper VMS format */
            uint16_t vprot = vmsfs_mode_to_protection(st.st_mode);
            char prot_buf[64];
            vmsfs_format_protection(vprot, prot_buf, sizeof(prot_buf));
            printf(" %s", prot_buf);
            printf("\n");
        } else if (show_size || show_date) {
            /* Size and/or date */
            printf("%-39s", vms_name);
            if (show_size) {
                printf(" %6ld", blocks);
            }
            if (show_date) {
                struct tm tm;
                localtime_r(&st.st_mtime, &tm);
                printf("  %2d-%s-%04d %02d:%02d:%02d.00",
                       tm.tm_mday, vms_months[tm.tm_mon],
                       1900 + tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec);
            }
            printf("\n");
        } else if (show_brief) {
            /* Brief: just filename */
            printf("%s\n", vms_name);
        } else {
            /* Columnar output */
            if (col_width < 1) col_width = 20;
            printf("%-*s", col_width, vms_name);
            col++;
            if (col >= columns) {
                printf("\n");
                col = 0;
            }
        }
    }
    closedir(dir);

    /* Finish last line of columnar output */
    if (col > 0 && !show_size && !show_date && !show_full && !show_brief) {
        printf("\n");
    }

    /* Footer */
    printf("\nTotal of %d file%s, %ld block%s.\n",
           file_count, file_count != 1 ? "s" : "",
           total_blocks, total_blocks != 1 ? "s" : "");

    if (pattern) free((void *)pattern);
    return SS$_NORMAL;
}

/*
 * TYPE - Display file contents.
 */
static int cmd_type(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    FILE *fp = fopen(linux_path, "r");
    if (!fp) {
        dcl_error("RMS", 2, "FNF",
                  "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* Check for /PAGE qualifier */
    int paged = dcl_has_qualifier(cmd, "PAGE");
    int line_count = 0;
    int page_size = 24;

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        fputs(line, stdout);
        line_count++;

        if (paged && line_count >= page_size) {
            printf("Press RETURN to continue...");
            fflush(stdout);
            char buf[64];
            if (!fgets(buf, sizeof(buf), stdin)) break;
            line_count = 0;
        }
    }

    fclose(fp);
    return SS$_NORMAL;
}

/*
 * COPY - Copy a file.
 */
static int cmd_copy(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE", "missing source and/or destination");
        return SS$_BADPARAM;
    }

    char src_path[1024], dst_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], src_path, sizeof(src_path));
    dcl_resolve_path(ctx, cmd->params[1], dst_path, sizeof(dst_path));

    /* Check if destination is a directory */
    struct stat st;
    if (stat(dst_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* Append source filename to destination directory */
        const char *basename = strrchr(src_path, '/');
        if (basename) basename++; else basename = src_path;
        size_t dlen = strlen(dst_path);
        if (dlen > 0 && dst_path[dlen - 1] != '/') {
            strncat(dst_path, "/", sizeof(dst_path) - dlen - 1);
        }
        strncat(dst_path, basename, sizeof(dst_path) - strlen(dst_path) - 1);
    }

    FILE *src = fopen(src_path, "rb");
    if (!src) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    FILE *dst = fopen(dst_path, "wb");
    if (!dst) {
        fclose(src);
        dcl_error("RMS", 2, "CRE", "cannot create - %s", cmd->params[1]);
        return SS$_FILACCERR;
    }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }

    fclose(src);
    fclose(dst);

    return SS$_NORMAL;
}

/*
 * DELETE - Delete a file.
 */
static int cmd_delete(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    int no_confirm = dcl_has_qualifier(cmd, "NOCONFIRM") ||
                     dcl_has_qualifier(cmd, "CONFIRM");
    /* Note: /CONFIRM with negation check handled by has_qualifier */

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    /* Check if the path contains wildcards */
    if (strchr(linux_path, '*') || strchr(linux_path, '?')) {
        /* Wildcard delete - expand and delete matching files */
        char dir[1024];
        strncpy(dir, linux_path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *last_slash = strrchr(dir, '/');
        const char *pat;
        if (last_slash) {
            pat = last_slash + 1;
            *(last_slash + 1) = '\0';
        } else {
            pat = dir;
            strncpy(dir, ctx->default_linux, sizeof(dir) - 1);
        }

        DIR *dp = opendir(dir);
        if (!dp) {
            dcl_error("RMS", 2, "DNF", "directory not found");
            return SS$_NOSUCHFILE;
        }

        struct dirent *entry;
        int deleted = 0;
        while ((entry = readdir(dp)) != NULL) {
            if (fnmatch(pat, entry->d_name, FNM_CASEFOLD) == 0) {
                char full[2048];
                snprintf(full, sizeof(full), "%s%s", dir, entry->d_name);

                if (!no_confirm) {
                    char upper_name[256];
                    size_t i;
                    for (i = 0; i < sizeof(upper_name) - 1 && entry->d_name[i]; i++)
                        upper_name[i] = (char)toupper((unsigned char)entry->d_name[i]);
                    upper_name[i] = '\0';
                    printf("Delete %s? [N]: ", upper_name);
                    fflush(stdout);
                    char resp[64];
                    if (!fgets(resp, sizeof(resp), stdin)) break;
                    if (toupper((unsigned char)resp[0]) != 'Y') continue;
                }

                if (unlink(full) == 0) deleted++;
            }
        }
        closedir(dp);

        if (deleted == 0) {
            dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
            return SS$_NOSUCHFILE;
        }
    } else {
        /* Single file delete */
        if (!no_confirm && ctx->interactive) {
            const char *bn = strrchr(linux_path, '/');
            if (bn) bn++; else bn = linux_path;
            char upper[256];
            size_t i;
            for (i = 0; i < sizeof(upper) - 1 && bn[i]; i++)
                upper[i] = (char)toupper((unsigned char)bn[i]);
            upper[i] = '\0';
            /* Actually, VMS DELETE does not prompt by default.
             * Only prompt if /CONFIRM is specified (not /NOCONFIRM). */
        }

        if (unlink(linux_path) != 0) {
            dcl_error("RMS", 2, "FNF",
                      "file not found - %s", cmd->params[0]);
            return SS$_NOSUCHFILE;
        }
    }

    return SS$_NORMAL;
}

/*
 * RENAME - Rename a file.
 */
static int cmd_rename(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE", "missing source and/or destination");
        return SS$_BADPARAM;
    }

    char src_path[1024], dst_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], src_path, sizeof(src_path));
    dcl_resolve_path(ctx, cmd->params[1], dst_path, sizeof(dst_path));

    if (rename(src_path, dst_path) != 0) {
        dcl_error("RMS", 2, "RNF",
                  "rename failed - %s", strerror(errno));
        return SS$_FILACCERR;
    }

    return SS$_NORMAL;
}

/*
 * CREATE - Create a new file.
 */
static int cmd_create(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    FILE *fp = fopen(linux_path, "w");
    if (!fp) {
        dcl_error("RMS", 2, "CRE", "cannot create - %s", cmd->params[0]);
        return SS$_FILACCERR;
    }

    /* Read lines from SYS$INPUT until Ctrl-Z (EOF) */
    if (ctx->interactive) {
        char line[4096];
        while (1) {
            if (!fgets(line, sizeof(line), stdin)) break;
            fputs(line, fp);
        }
    }

    fclose(fp);
    return SS$_NORMAL;
}

/*
 * SEARCH - Search file for a string (like grep).
 */
static int cmd_search(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing file specification and/or search string");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    const char *search_str = cmd->params[1];
    int exact = dcl_has_qualifier(cmd, "EXACT");

    FILE *fp = fopen(linux_path, "r");
    if (!fp) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* Display header */
    char vms_name[256];
    const char *basename = strrchr(linux_path, '/');
    if (basename) basename++; else basename = linux_path;
    size_t i;
    for (i = 0; i < sizeof(vms_name) - 1 && basename[i]; i++)
        vms_name[i] = (char)toupper((unsigned char)basename[i]);
    vms_name[i] = '\0';

    char line[4096];
    int found = 0;
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        int match;

        if (exact) {
            match = (strstr(line, search_str) != NULL);
        } else {
            /* Case-insensitive search */
            char lower_line[4096], lower_search[1024];
            for (i = 0; line[i] && i < sizeof(lower_line) - 1; i++)
                lower_line[i] = (char)tolower((unsigned char)line[i]);
            lower_line[i] = '\0';
            for (i = 0; search_str[i] && i < sizeof(lower_search) - 1; i++)
                lower_search[i] = (char)tolower((unsigned char)search_str[i]);
            lower_search[i] = '\0';
            match = (strstr(lower_line, lower_search) != NULL);
        }

        if (match) {
            if (!found) {
                printf("\n******************************\n%s\n", vms_name);
                found = 1;
            }
            /* Remove trailing newline for cleaner output */
            size_t llen = strlen(line);
            if (llen > 0 && line[llen - 1] == '\n') line[llen - 1] = '\0';
            printf("%s\n", line);
        }
    }

    fclose(fp);

    if (!found) {
        dcl_error("SEARCH", 0, "NOMATCHES",
                  "no strings matched");
        return SS$_NORMAL; /* Warning, not error */
    }

    return SS$_NORMAL;
}

/*
 * PURGE - Delete all but highest version of files.
 */
static int cmd_purge(struct dcl_command *cmd)
{
    (void)cmd;
    /* In our Linux-based system, we don't have true file versions.
     * This is a stub that acknowledges the command. */
    printf("%%PURGE-I-NOPURGE, no file versions to purge on this system\n");
    return SS$_NORMAL;
}

/* ================================================================== */
/*                    Logical Name Operations                          */
/* ================================================================== */

/*
 * DEFINE - Define a logical name.
 */
static int cmd_define(struct dcl_command *cmd)
{
    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing logical name and/or equivalence string");
        return SS$_BADPARAM;
    }

    const char *logname = cmd->params[0];
    const char *equiv = cmd->params[1];

    /* For now, store as a symbol (a real implementation would use LNM) */
    char upper_name[256];
    size_t i;
    for (i = 0; i < sizeof(upper_name) - 1 && logname[i]; i++)
        upper_name[i] = (char)toupper((unsigned char)logname[i]);
    upper_name[i] = '\0';

    dcl_sym_set(upper_name, equiv, DCL_SYM_GLOBAL);

    return SS$_NORMAL;
}

/*
 * DEASSIGN - Remove a logical name.
 */
static int cmd_deassign(struct dcl_command *cmd)
{
    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOKEYW", "missing logical name");
        return SS$_BADPARAM;
    }

    dcl_sym_delete(cmd->params[0], DCL_SYM_GLOBAL);
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     I/O Channel Operations                          */
/* ================================================================== */

/*
 * OPEN - Open a file for READ, WRITE, or APPEND.
 */
static int cmd_open(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing channel name and/or file specification");
        return SS$_BADPARAM;
    }

    const char *channel_name = cmd->params[0];
    const char *filespec = cmd->params[1];

    /* Determine mode */
    int mode = 0; /* default: read */
    if (dcl_has_qualifier(cmd, "WRITE")) mode = 1;
    else if (dcl_has_qualifier(cmd, "APPEND")) mode = 2;
    else if (dcl_has_qualifier(cmd, "READ")) mode = 0;

    /* Find a free channel slot */
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (ctx->channels[i].fp == NULL) {
            slot = i;
            break;
        }
        if (strcasecmp(ctx->channels[i].name, channel_name) == 0) {
            /* Already open - close and reuse */
            fclose(ctx->channels[i].fp);
            ctx->channels[i].fp = NULL;
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        dcl_error("DCL", 2, "MAXCHAN", "maximum channels exceeded");
        return SS$_BADPARAM;
    }

    /* Resolve filespec */
    char linux_path[1024];
    dcl_resolve_path(ctx, filespec, linux_path, sizeof(linux_path));

    const char *fmode;
    switch (mode) {
        case 0: fmode = "r"; break;
        case 1: fmode = "w"; break;
        case 2: fmode = "a"; break;
        default: fmode = "r"; break;
    }

    FILE *fp = fopen(linux_path, fmode);
    if (!fp) {
        dcl_error("RMS", 2, "FNF",
                  "error opening %s", filespec);
        return SS$_NOSUCHFILE;
    }

    ctx->channels[slot].fp = fp;
    ctx->channels[slot].mode = mode;
    strncpy(ctx->channels[slot].name, channel_name,
            sizeof(ctx->channels[0].name) - 1);
    ctx->channels[slot].name[sizeof(ctx->channels[0].name) - 1] = '\0';
    /* Uppercase channel name */
    for (size_t i = 0; ctx->channels[slot].name[i]; i++) {
        ctx->channels[slot].name[i] =
            (char)toupper((unsigned char)ctx->channels[slot].name[i]);
    }

    return SS$_NORMAL;
}

/*
 * CLOSE - Close a file channel.
 */
static int cmd_close(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOKEYW", "missing channel name");
        return SS$_BADPARAM;
    }

    for (int i = 0; i < 16; i++) {
        if (ctx->channels[i].fp &&
            strcasecmp(ctx->channels[i].name, cmd->params[0]) == 0) {
            fclose(ctx->channels[i].fp);
            ctx->channels[i].fp = NULL;
            ctx->channels[i].name[0] = '\0';
            return SS$_NORMAL;
        }
    }

    dcl_error("DCL", 2, "IVLOGNAM",
              "channel %s is not open", cmd->params[0]);
    return SS$_BADPARAM;
}

/*
 * READ - Read a line from a file channel into a symbol.
 */
static int cmd_read(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing channel name and/or symbol name");
        return SS$_BADPARAM;
    }

    const char *channel_name = cmd->params[0];
    const char *symbol_name = cmd->params[1];

    /* Find the channel */
    FILE *fp = NULL;
    for (int i = 0; i < 16; i++) {
        if (ctx->channels[i].fp &&
            strcasecmp(ctx->channels[i].name, channel_name) == 0) {
            fp = ctx->channels[i].fp;
            break;
        }
    }

    /* Check for /PROMPT qualifier (read from SYS$INPUT) */
    const char *prompt = dcl_qualifier_value(cmd, "PROMPT");
    if (prompt) {
        char buf[4096];
        if (prompt[0]) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if (!fgets(buf, sizeof(buf), stdin)) {
            return SS$_ENDOFFILE;
        }
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        dcl_sym_set(symbol_name, buf, DCL_SYM_LOCAL);
        return SS$_NORMAL;
    }

    if (!fp) {
        dcl_error("DCL", 2, "IVLOGNAM",
                  "channel %s is not open", channel_name);
        return SS$_BADPARAM;
    }

    char line[4096];
    if (!fgets(line, sizeof(line), fp)) {
        dcl_sym_set(symbol_name, "", DCL_SYM_LOCAL);
        return SS$_ENDOFFILE;
    }

    /* Remove trailing newline */
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

    dcl_sym_set(symbol_name, line, DCL_SYM_LOCAL);
    return SS$_NORMAL;
}

/*
 * WRITE - Write text to a file channel.
 */
static int cmd_write(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing channel name and/or text");
        return SS$_BADPARAM;
    }

    const char *channel_name = cmd->params[0];

    /* Check for SYS$OUTPUT special channel */
    if (strcasecmp(channel_name, "SYS$OUTPUT") == 0) {
        for (int i = 1; i < cmd->param_count; i++) {
            printf("%s", cmd->params[i]);
        }
        printf("\n");
        return SS$_NORMAL;
    }

    if (strcasecmp(channel_name, "SYS$ERROR") == 0) {
        for (int i = 1; i < cmd->param_count; i++) {
            fprintf(stderr, "%s", cmd->params[i]);
        }
        fprintf(stderr, "\n");
        return SS$_NORMAL;
    }

    /* Find the channel */
    FILE *fp = NULL;
    for (int i = 0; i < 16; i++) {
        if (ctx->channels[i].fp &&
            strcasecmp(ctx->channels[i].name, channel_name) == 0) {
            fp = ctx->channels[i].fp;
            break;
        }
    }

    if (!fp) {
        dcl_error("DCL", 2, "IVLOGNAM",
                  "channel %s is not open", channel_name);
        return SS$_BADPARAM;
    }

    /* Write all remaining params joined by spaces (but actually as-is) */
    for (int i = 1; i < cmd->param_count; i++) {
        if (i > 1) fprintf(fp, " ");
        fprintf(fp, "%s", cmd->params[i]);
    }
    fprintf(fp, "\n");
    fflush(fp);

    return SS$_NORMAL;
}

/* ================================================================== */
/*                     Process/Session Commands                        */
/* ================================================================== */

/*
 * RUN - Execute a program.
 */
static int cmd_run(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing image specification");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    /* Check if executable exists */
    if (access(linux_path, X_OK) != 0) {
        /* Try without .exe extension, or try with .EXE */
        char try_path[1024];
        snprintf(try_path, sizeof(try_path), "%s.exe", linux_path);
        if (access(try_path, X_OK) == 0) {
            strncpy(linux_path, try_path, sizeof(linux_path) - 1);
        } else {
            dcl_error("DCL", 2, "IVIMAGE",
                      "image not found - %s", cmd->params[0]);
            return SS$_NOSUCHFILE;
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child */
        execl(linux_path, linux_path, (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        /* Parent - wait for child */
        int wstatus;
        waitpid(pid, &wstatus, 0);
        if (WIFEXITED(wstatus)) {
            int exit_code = WEXITSTATUS(wstatus);
            return (exit_code == 0) ? SS$_NORMAL : SS$_ABORT;
        }
    } else {
        dcl_error("DCL", 4, "CREPRC", "cannot create process");
        return SS$_ABORT;
    }

    return SS$_NORMAL;
}

/*
 * SPAWN - Create a subprocess.
 */
static int cmd_spawn(struct dcl_command *cmd)
{
    /* If params given, execute as a shell command */
    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        /* Build the command string */
        char shell_cmd[DCL_MAX_LINE] = {0};
        for (int i = 0; i < cmd->param_count; i++) {
            if (i > 0) strncat(shell_cmd, " ",
                                sizeof(shell_cmd) - strlen(shell_cmd) - 1);
            strncat(shell_cmd, cmd->params[i],
                    sizeof(shell_cmd) - strlen(shell_cmd) - 1);
        }
        int ret = system(shell_cmd);
        return (ret == 0) ? SS$_NORMAL : SS$_ABORT;
    }

    /* No params - spawn an interactive shell */
    const char *shell = getenv("SHELL");
    if (!shell) shell = "/bin/sh";

    pid_t pid = fork();
    if (pid == 0) {
        execl(shell, shell, (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        int wstatus;
        waitpid(pid, &wstatus, 0);
    }

    return SS$_NORMAL;
}

/*
 * PIPE - Execute a command using the system shell (for piping, redirection).
 */
static int cmd_pipe(struct dcl_command *cmd)
{
    /* Reconstruct the entire command line after PIPE */
    char shell_cmd[DCL_MAX_LINE] = {0};
    for (int i = 0; i < cmd->param_count; i++) {
        if (i > 0) strncat(shell_cmd, " ",
                            sizeof(shell_cmd) - strlen(shell_cmd) - 1);
        strncat(shell_cmd, cmd->params[i],
                sizeof(shell_cmd) - strlen(shell_cmd) - 1);
    }
    if (cmd->rest[0]) {
        if (shell_cmd[0]) strncat(shell_cmd, " | ",
                                   sizeof(shell_cmd) - strlen(shell_cmd) - 1);
        strncat(shell_cmd, cmd->rest,
                sizeof(shell_cmd) - strlen(shell_cmd) - 1);
    }

    if (shell_cmd[0] == '\0') {
        dcl_error("DCL", 2, "NOKEYW", "missing command for PIPE");
        return SS$_BADPARAM;
    }

    int ret = system(shell_cmd);
    return (ret == 0) ? SS$_NORMAL : SS$_ABORT;
}

/*
 * EXIT - Exit from current procedure or session.
 */
static int cmd_exit(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* Optional status code */
    uint32_t exit_code = SS$_NORMAL;
    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        exit_code = (uint32_t)strtoul(cmd->params[0], NULL, 0);
    }

    ctx->exit_requested = 1;
    ctx->exit_status = (int)exit_code;

    return (int)exit_code;
}

/*
 * STOP - Forceful exit.
 */
static int cmd_stop(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();
    ctx->exit_requested = 1;
    ctx->exit_status = 0;
    return SS$_NORMAL;
}

/*
 * LOGOUT - End session.
 */
static int cmd_logout(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();
    const char *upper_user = ctx->username[0] ? ctx->username : "SYSTEM";

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    printf("  %s      logged out at %2d-%s-%04d %02d:%02d:%02d.%02d\n",
           upper_user, tm.tm_mday, vms_months[tm.tm_mon],
           1900 + tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));

    /* Write an OPCOM logout record to the operator log */
    {
        struct {
            struct opcdef hdr;
            char text[128];
        } msgbuf;
        memset(&msgbuf, 0, sizeof(msgbuf));
        msgbuf.hdr.opc$b_ms_type   = OPC$_RQ_RQST;
        msgbuf.hdr.opc$b_ms_target = OPC$M_NM_CENTRL;
        int n = snprintf(msgbuf.hdr.opc$l_ms_text, sizeof(msgbuf.text),
                         "logout: user %s at %02d-%s-%04d %02d:%02d:%02d",
                         upper_user,
                         tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
                         tm.tm_hour, tm.tm_min, tm.tm_sec);
        struct dsc$descriptor_s desc;
        desc.dsc$a_pointer = (char *)&msgbuf.hdr;
        desc.dsc$w_length  = (uint16_t)(8 + n);
        sys$sndopr(&desc, 0);
    }

    ctx->exit_requested = 1;
    ctx->logout_requested = 1;
    return SS$_NORMAL;
}

/*
 * INQUIRE - Prompt user for input, store in symbol.
 */
static int cmd_inquire(struct dcl_command *cmd)
{
    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOKEYW", "missing symbol name");
        return SS$_BADPARAM;
    }

    const char *symbol_name = cmd->params[0];
    const char *prompt_text = (cmd->param_count >= 2) ? cmd->params[1] : "";

    /* Display prompt */
    if (prompt_text[0]) {
        printf("%s: ", prompt_text);
    } else {
        /* Default prompt is symbol name */
        char upper_name[256];
        size_t i;
        for (i = 0; i < sizeof(upper_name) - 1 && symbol_name[i]; i++)
            upper_name[i] = (char)toupper((unsigned char)symbol_name[i]);
        upper_name[i] = '\0';
        printf("%s: ", upper_name);
    }
    fflush(stdout);

    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) {
        dcl_sym_set(symbol_name, "", DCL_SYM_LOCAL);
        return SS$_NORMAL;
    }

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    /* Unless /NOPUNCTUATION, upcase the input */
    if (!dcl_has_qualifier(cmd, "NOPUNCTUATION")) {
        for (size_t i = 0; buf[i]; i++) {
            buf[i] = (char)toupper((unsigned char)buf[i]);
        }
    }

    dcl_sym_set(symbol_name, buf, DCL_SYM_LOCAL);
    return SS$_NORMAL;
}

/* ================================================================== */
/*           OPCOM Commands: REPLY and REQUEST                         */
/* ================================================================== */

/*
 * REPLY /ENABLE - Enable the current terminal as an operator terminal.
 * REPLY /DISABLE - Disable operator terminal.
 *
 * On real VMS, REPLY /ENABLE marks the terminal as an operator console.
 * Messages sent via sys$sndopr are then written to enabled terminals.
 * In OVMX, we log the enable/disable event to OPERATOR.LOG and
 * print a confirmation message — operator messages go to the log.
 *
 * Usage:
 *   REPLY /ENABLE[=class]   - enable operator messages
 *   REPLY /DISABLE          - disable operator terminal
 *   REPLY /TO=rqid "text"   - reply to a pending request
 */
static int cmd_reply(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();
    const char *username = ctx->username[0] ? ctx->username : "SYSTEM";

    /* Build a minimal OPC message for the log */
    struct {
        struct opcdef hdr;
        char text[128];
    } msgbuf;
    memset(&msgbuf, 0, sizeof(msgbuf));

    struct dsc$descriptor_s desc;
    desc.dsc$a_pointer = (char *)&msgbuf.hdr;
    desc.dsc$w_length  = 0;  /* filled below */

    if (dcl_has_qualifier(cmd, "ENABLE")) {
        const char *cls = dcl_qualifier_value(cmd, "ENABLE");
        char detail[64] = "CENTRAL";
        if (cls && cls[0]) {
            strncpy(detail, cls, sizeof(detail) - 1);
        }
        printf("%%OPCOM-I-OPRENA, operator %s enabled for %s class messages\n",
               username, detail);

        msgbuf.hdr.opc$b_ms_type   = OPC$_RQ_ENABLE;
        msgbuf.hdr.opc$b_ms_target = OPC$M_NM_CENTRL;
        int n = snprintf(msgbuf.hdr.opc$l_ms_text,
                         sizeof(msgbuf.text),
                         "operator %s enabled (%s)", username, detail);
        desc.dsc$w_length = (uint16_t)(8 + n);
        sys$sndopr(&desc, 0);

    } else if (dcl_has_qualifier(cmd, "DISABLE")) {
        printf("%%OPCOM-I-OPRDIS, operator %s disabled\n", username);

        msgbuf.hdr.opc$b_ms_type   = OPC$_RQ_DISABLE;
        msgbuf.hdr.opc$b_ms_target = OPC$M_NM_CENTRL;
        int n = snprintf(msgbuf.hdr.opc$l_ms_text,
                         sizeof(msgbuf.text),
                         "operator %s disabled", username);
        desc.dsc$w_length = (uint16_t)(8 + n);
        sys$sndopr(&desc, 0);

    } else if (dcl_has_qualifier(cmd, "TO")) {
        /* REPLY /TO=rqid "reply text" */
        const char *to_val = dcl_qualifier_value(cmd, "TO");
        const char *reply_text = (cmd->param_count >= 1) ? cmd->params[0] : "";

        printf("%%OPCOM-I-REPLY, reply sent to request %s: %s\n",
               to_val ? to_val : "?", reply_text);

        msgbuf.hdr.opc$b_ms_type   = OPC$_RQ_REPLY;
        msgbuf.hdr.opc$b_ms_target = OPC$M_NM_CENTRL;
        int n = snprintf(msgbuf.hdr.opc$l_ms_text,
                         sizeof(msgbuf.text),
                         "reply to rqid %s: %s",
                         to_val ? to_val : "0", reply_text);
        desc.dsc$w_length = (uint16_t)(8 + n);
        sys$sndopr(&desc, 0);

    } else {
        dcl_error("DCL", 2, "SYNTAX",
                  "REPLY requires /ENABLE, /DISABLE, or /TO qualifier");
        return SS$_BADPARAM;
    }

    return SS$_NORMAL;
}

/*
 * REQUEST "message" - Send a request message to the operator.
 *
 * Usage:
 *   REQUEST "message text"
 *   REQUEST /REPLY "message text"   - wait for operator reply (not implemented)
 *
 * Sends a message to OPCOM (logs to OPERATOR.LOG).
 * Prints a confirmation showing the request was sent.
 */
static int cmd_request(struct dcl_command *cmd)
{
    const char *msg_text = "";
    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        msg_text = cmd->params[0];
    } else {
        dcl_error("DCL", 2, "SYNTAX",
                  "REQUEST requires a message string parameter");
        return SS$_BADPARAM;
    }

    /* Build OPC message buffer */
    struct {
        struct opcdef hdr;
        char text[128];
    } msgbuf;
    memset(&msgbuf, 0, sizeof(msgbuf));

    msgbuf.hdr.opc$b_ms_type   = OPC$_RQ_RQST;
    msgbuf.hdr.opc$b_ms_target = OPC$M_NM_CENTRL;

    int n = snprintf(msgbuf.hdr.opc$l_ms_text, sizeof(msgbuf.text),
                     "%s", msg_text);
    if (n > 127) n = 127;

    struct dsc$descriptor_s desc;
    desc.dsc$a_pointer = (char *)&msgbuf.hdr;
    desc.dsc$w_length  = (uint16_t)(8 + n);

    uint32_t status = sys$sndopr(&desc, 0);
    if (!(status & 1)) {
        dcl_error("OPCOM", 2, "SNDOPR", "failed to send operator message");
        return (int)status;
    }

    printf("%%OPCOM-I-RQSTPEND, request sent to operator: %s\n", msg_text);
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     ACCOUNTING Command                               */
/* ================================================================== */

/*
 * ACCOUNTING - Display login accounting information for the current user.
 *
 * Shows last login time read from /etc/ovmx/lastlogin/<USERNAME>.
 * Matches OpenVMS ACCOUNTING utility output style.
 */
static int cmd_accounting(struct dcl_command *cmd)
{
    (void)cmd;

    struct dcl_context *ctx = dcl_get_context();
    const char *username = ctx->username[0] ? ctx->username : "SYSTEM";

    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    time_t last_login = 0;
    int found = (ovmx_accounting_get_lastlogin(username, &last_login) == 0);

    printf("\n  OVMX Accounting for user %s\n", username);
    printf("  %s\n\n", "----------------------------------------");

    if (found && last_login > 0) {
        struct tm tm;
        localtime_r(&last_login, &tm);
        printf("  Last interactive login: %02d-%s-%04d %02d:%02d:%02d\n",
               tm.tm_mday, months[tm.tm_mon], 1900 + tm.tm_year,
               tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        printf("  Last interactive login: (no previous login recorded)\n");
    }

    printf("\n");
    return SS$_NORMAL;
}

/* ================================================================== */
/*                         HELP Command                                */
/* ================================================================== */

static int cmd_help(struct dcl_command *cmd)
{
    /* If a specific topic is requested */
    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        /* Look up the command for its help text */
        const struct dcl_verb *verb = dcl_find_verb(cmd->params[0]);
        if (verb && verb->help) {
            printf("\n%s\n\n", verb->name);
            printf("  %s\n\n", verb->help);

            /* Provide some standard help structure */
            if (strcasecmp(verb->name, "SHOW") == 0) {
                printf("  Subcommands:\n\n");
                printf("    DEFAULT    Display current default directory\n");
                printf("    LOGICAL    Display logical name translations\n");
                printf("    PROCESS    Display current process information\n");
                printf("    PROTECTION Display process default protection\n");
                printf("    SYMBOL     Display symbol value(s)\n");
                printf("    SYSTEM     Display process list\n");
                printf("    TIME       Display current date/time\n");
                printf("    USERS      Display logged-in users\n");
                printf("    VERIFY     Display verification state\n");
                printf("\n");
            } else if (strcasecmp(verb->name, "SET") == 0) {
                printf("  Subcommands:\n\n");
                printf("    DEFAULT    Change default directory\n");
                printf("    PASSWORD   Change user password\n");
                printf("    PROMPT     Change DCL prompt\n");
                printf("    PROTECTION Set file protection\n");
                printf("    TERMINAL   Set terminal characteristics (stub)\n");
                printf("    [NO]VERIFY Set command verification on/off\n");
                printf("\n");
            } else if (strcasecmp(verb->name, "DIRECTORY") == 0) {
                printf("  Format:\n\n");
                printf("    DIRECTORY [filespec]\n\n");
                printf("  Qualifiers:\n\n");
                printf("    /BRIEF      Display filenames only\n");
                printf("    /COLUMNS=n  Display in n columns\n");
                printf("    /DATE       Display date/time\n");
                printf("    /FULL       Display all information\n");
                printf("    /SIZE       Display file sizes in blocks\n");
                printf("\n");
            }
            return SS$_NORMAL;
        }
        dcl_error("DCL", 0, "NOHELP",
                  "no help available for \"%s\"", cmd->params[0]);
        return SS$_NORMAL;
    }

    /* General help - list all commands */
    printf("\n");
    printf("Information available:\n\n");

    int count = 0;
    const struct dcl_verb *table = dcl_get_verb_table(&count);
    int col = 0;

    for (int i = 0; i < count; i++) {
        printf("  %-16s", table[i].name);
        col++;
        if (col >= 4) {
            printf("\n");
            col = 0;
        }
    }
    if (col > 0) printf("\n");

    printf("\nTopic? ");
    fflush(stdout);

    char topic[256];
    if (fgets(topic, sizeof(topic), stdin)) {
        size_t tlen = strlen(topic);
        if (tlen > 0 && topic[tlen - 1] == '\n') topic[tlen - 1] = '\0';
        if (topic[0] != '\0') {
            struct dcl_command help_cmd;
            memset(&help_cmd, 0, sizeof(help_cmd));
            strcpy(help_cmd.verb, "HELP");
            strncpy(help_cmd.params[0], topic, sizeof(help_cmd.params[0]) - 1);
            help_cmd.param_count = 1;
            return cmd_help(&help_cmd);
        }
    }

    printf("\n");
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     Command Table                                   */
/* ================================================================== */

static struct dcl_verb builtin_verbs[] = {
    { "ACCOUNTING",cmd_accounting,CDU_F_ABBREV | CDU_F_QUALIFIER, 4,
      "Display login accounting information for the current user" },
    { "CLOSE",     cmd_close,     CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Close a file that was opened for I/O" },
    { "COPY",      cmd_copy,      CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Copy a file" },
    { "CREATE",    cmd_create,    CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Create a new file" },
    { "DEASSIGN",  cmd_deassign,  CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Deassign (remove) a logical name" },
    { "DEFINE",    cmd_define,    CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Create a logical name definition" },
    { "DELETE",    cmd_delete,    CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Delete a file" },
    { "DIRECTORY", cmd_directory, CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "List files in a directory" },
    { "EXIT",      cmd_exit,      CDU_F_ABBREV, 2,
      "Terminate a command procedure or session" },
    { "HELP",      cmd_help,      CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Obtain information about DCL commands" },
    { "INQUIRE",   cmd_inquire,   CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Read input from SYS$INPUT and assign to a symbol" },
    { "LOGOUT",    cmd_logout,    CDU_F_ABBREV, 2,
      "Terminate an interactive session" },
    { "OPEN",      cmd_open,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Open a file for reading or writing" },
    { "PIPE",      cmd_pipe,      CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Execute a command using system shell (piping/redirection)" },
    { "PURGE",     cmd_purge,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Delete old versions of a file" },
    { "READ",      cmd_read,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Read a record from a file into a symbol" },
    { "RENAME",    cmd_rename,    CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Change the name and/or location of a file" },
    { "REPLY",     cmd_reply,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Send an operator reply or enable/disable operator terminal" },
    { "REQUEST",   cmd_request,   CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Send a request message to the operator" },
    { "RUN",       cmd_run,       CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Execute a program image" },
    { "SEARCH",    cmd_search,    CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Search a file for a text string" },
    { "SET",       cmd_set,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Set or modify system, process, or file characteristics" },
    { "SHOW",      cmd_show,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display information about the system, process, or files" },
    { "SPAWN",     cmd_spawn,     CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Create a subprocess" },
    { "STOP",      cmd_stop,      CDU_F_ABBREV, 2,
      "Stop the current process" },
    { "TYPE",      cmd_type,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display the contents of a file" },
    { "WRITE",     cmd_write,     CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Write a record to a file" },
};

static int builtin_count = (int)(sizeof(builtin_verbs) / sizeof(builtin_verbs[0]));

/*
 * Find a verb by name (with minimum-uniqueness abbreviation matching).
 */
const struct dcl_verb *dcl_find_verb(const char *name)
{
    if (!name || !name[0]) return NULL;

    for (int i = 0; i < builtin_count; i++) {
        if (dcl_match_command(name, builtin_verbs[i].name,
                              builtin_verbs[i].min_abbrev)) {
            return &builtin_verbs[i];
        }
    }
    return NULL;
}

/*
 * Get the full verb table (for HELP listing).
 */
const struct dcl_verb *dcl_get_verb_table(int *count)
{
    if (count) *count = builtin_count;
    return builtin_verbs;
}

/*
 * Register built-in commands (called during initialization).
 * Currently a no-op since we use a static table, but reserved
 * for future dynamic command registration.
 */
void dcl_register_builtins(void)
{
    /* Static table - nothing to do */
}
