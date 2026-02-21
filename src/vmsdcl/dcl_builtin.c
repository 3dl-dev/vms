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
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <termios.h>
#include <mntent.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/cdu.h"
#include "ssdef.h"
#include "vms/logical.h"
#include "vms/privs.h"
#include "opcdef.h"
#include "ovmx_accounting.h"
#include "starlet.h"

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

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

/* VMS wildcard match (supports % as single-character wildcard) */
extern int vmsfs_wildcard_match(const char *pattern, const char *name);

/* VMS ODS-2 name validation */
extern int vmsfs_is_valid_ods2_name(const char *name);

/* VMS month abbreviations */
static const char *vms_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

/* ================================================================== */
/*                          SHOW Commands                              */
/* ================================================================== */

/* Forward declarations for helper functions used by cmd_show_process */
static int cmd_show_process_privileges(struct dcl_context *ctx);
static int cmd_show_process_quotas(struct dcl_context *ctx);

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

/* Callback context for enumerating logical names under SHOW LOGICAL */
struct show_lnm_ctx {
    const char *table_name;
};

static int show_lnm_callback(const char *name, const lnm_entry_t *entry, void *ctx)
{
    struct show_lnm_ctx *sctx = (struct show_lnm_ctx *)ctx;
    if (entry->num_translations > 0) {
        printf("   \"%s\" = \"%s\" (%s)\n",
               name, entry->translations[0].value, sctx->table_name);
    }
    return 0;
}

/*
 * SHOW LOGICAL - Display logical name translations.
 *
 * With a name argument: translate that specific name and show it.
 * Without arguments: enumerate all tables (process, job, group, system).
 */
static int cmd_show_logical(struct dcl_command *cmd)
{
    lnm_manager_t *mgr = lnm_get_manager();

    if (cmd->param_count >= 2) {
        /* SHOW LOGICAL <name> — look up and display a specific logical */
        /* Note: param[0] is the subcommand "LOGICAL" (from SHOW LOGICAL) */
        const char *logname = cmd->params[1];

        /* Uppercase for display */
        char upper_name[256];
        size_t i;
        for (i = 0; i < sizeof(upper_name) - 1 && logname[i]; i++)
            upper_name[i] = (char)toupper((unsigned char)logname[i]);
        upper_name[i] = '\0';

        char value[256];
        if (dcl_translate_logical(upper_name, value, sizeof(value)) == 0) {
            /*
             * Determine which table the name was found in so we can
             * show the correct table name in the output.
             */
            const char *found_table = LNM_PROCESS_TABLE;
            if (mgr) {
                static const struct { lnm_table_t **tbl; const char *tname; } tables[] = {
                    { NULL, LNM_PROCESS_TABLE },
                    { NULL, LNM_JOB_TABLE     },
                    { NULL, LNM_GROUP_TABLE   },
                    { NULL, LNM_SYSTEM_TABLE  },
                    { NULL, NULL              }
                };
                /* Search tables in order to find where the name lives */
                lnm_table_t *search[4];
                search[0] = mgr->process_table;
                search[1] = mgr->job_table;
                search[2] = mgr->group_table;
                search[3] = mgr->system_table;
                const char *tnames[4] = {
                    LNM_PROCESS_TABLE, LNM_JOB_TABLE,
                    LNM_GROUP_TABLE,   LNM_SYSTEM_TABLE
                };
                for (int t = 0; t < 4; t++) {
                    if (!search[t]) continue;
                    uint32_t st = lnm_translate(mgr, tnames[t], upper_name,
                                                value, sizeof(value), NULL, NULL);
                    if (st == SS$_NORMAL || st == SS$_SUPERSEDE) {
                        found_table = tnames[t];
                        break;
                    }
                }
                (void)tables;
            }
            printf("   \"%s\" = \"%s\" (%s)\n", upper_name, value, found_table);
        } else {
            dcl_error("DCL", 0, "NOLOG", "no logical name match");
            return SS$_NOLOGNAM;
        }
    } else {
        /* Show all logical names from all tables */
        if (mgr) {
            struct show_lnm_ctx sctx;

            printf("(LNM$PROCESS_TABLE)\n\n");
            sctx.table_name = LNM_PROCESS_TABLE;
            lnm_enumerate(mgr, LNM_PROCESS_TABLE, show_lnm_callback, &sctx);

            printf("\n(LNM$JOB)\n\n");
            sctx.table_name = LNM_JOB_TABLE;
            lnm_enumerate(mgr, LNM_JOB_TABLE, show_lnm_callback, &sctx);

            printf("\n(LNM$GROUP)\n\n");
            sctx.table_name = LNM_GROUP_TABLE;
            lnm_enumerate(mgr, LNM_GROUP_TABLE, show_lnm_callback, &sctx);

            printf("\n(LNM$SYSTEM)\n\n");
            sctx.table_name = LNM_SYSTEM_TABLE;
            lnm_enumerate(mgr, LNM_SYSTEM_TABLE, show_lnm_callback, &sctx);
        } else {
            /* LNM not available — show nothing (graceful degrade) */
            printf("(LNM$PROCESS_TABLE)\n\n");
            printf("   %%DCL-W-NOLOGNAM, logical name manager not available\n");
        }
    }

    return SS$_NORMAL;
}

/*
 * Helper: read CPU time from /proc/pid/stat and format as VMS HH:MM:SS.CC.
 * Fields 14 (utime) and 15 (stime) are in clock ticks.
 * Returns 1 on success, 0 on failure.  cpu_str must be >= 14 bytes.
 */
static int read_proc_cpu(int pid, char *cpu_str, size_t cpu_len)
{
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    /* /proc/pid/stat: pid (comm) state ppid ... utime stime ...
     * We need to skip past the comm field (may contain spaces/parens) */
    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    /* Find closing ')' of comm field */
    char *p = strrchr(line, ')');
    if (!p) return 0;
    p++; /* skip ')' */

    /* Now parse: state ppid pgrp session tty_nr tpgid flags
     * minflt cminflt majflt cmajflt utime stime
     * That's 12 more fields after ')' */
    unsigned long utime = 0, stime = 0;
    char state;
    long ppid, pgrp, session, tty_nr, tpgid;
    unsigned long flags, minflt, cminflt, majflt, cmajflt;

    if (sscanf(p, " %c %ld %ld %ld %ld %ld %lu %lu %lu %lu %lu %lu %lu",
               &state, &ppid, &pgrp, &session, &tty_nr, &tpgid,
               &flags, &minflt, &cminflt, &majflt, &cmajflt,
               &utime, &stime) < 13) {
        /* Fallback: scan as individual fields */
        utime = 0; stime = 0;
    }

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;

    unsigned long total_ticks = utime + stime;
    unsigned long total_sec   = total_ticks / (unsigned long)hz;
    unsigned long centisec    = (total_ticks % (unsigned long)hz) * 100UL /
                                (unsigned long)hz;
    unsigned long hh = total_sec / 3600;
    unsigned long mm = (total_sec % 3600) / 60;
    unsigned long ss = total_sec % 60;

    snprintf(cpu_str, cpu_len, "%lu %02lu:%02lu:%02lu.%02lu",
             hh / 24,          /* days (usually 0) */
             hh % 24, mm, ss, centisec);

    return 1;
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

    /* Show the current process first */
    char self_cpu[32] = "0 00:00:00.00";
    read_proc_cpu((int)getpid(), self_cpu, sizeof(self_cpu));
    printf(" %08X %-15s %s %3d %9d  %s  %9d  %5d\n",
           (unsigned)getpid(), "OVMX", "HIB", 4, 0, self_cpu, 0, 0);

    /* Read /proc to list processes */
    DIR *proc_dir = opendir("/proc");
    if (proc_dir) {
        struct dirent *entry;
        int count = 0;
        while ((entry = readdir(proc_dir)) != NULL && count < 20) {
            /* Only process numeric directories */
            if (!isdigit((unsigned char)entry->d_name[0])) continue;

            int pid = atoi(entry->d_name);
            if (pid <= 0 || pid == (int)getpid()) continue;

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

            /* Read CPU time */
            char cpu_str[32] = "0 00:00:00.00";
            read_proc_cpu(pid, cpu_str, sizeof(cpu_str));

            printf(" %08X %-15s %s %3d %9d  %s  %9d  %5d\n",
                   (unsigned)pid, pname, "COM", 4, 0, cpu_str, 0, 0);
            count++;
        }
        closedir(proc_dir);
    }

    return SS$_NORMAL;
}

/*
 * SHOW PROCESS - Show current process information.
 * Supports /PRIVILEGES and /QUOTAS qualifiers.
 */
static int cmd_show_process(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* Check for /PRIVILEGES qualifier */
    if (dcl_has_qualifier(cmd, "PRIVILEGES"))
        return cmd_show_process_privileges(ctx);

    /* Check for /QUOTAS qualifier */
    if (dcl_has_qualifier(cmd, "QUOTAS"))
        return cmd_show_process_quotas(ctx);

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
    /* Show VMS-style terminal name; fall back to _FTA0: */
    const char *prcnam = ctx->process_name[0] ? ctx->process_name : "_FTA0:";
    printf("Terminal:          %s\n", prcnam);
    printf("User Identifier:   [%03o,%03o]\n",
           ctx->uic_group ? (unsigned)ctx->uic_group : (unsigned)(getgid() & 0377),
           ctx->uic_member ? (unsigned)ctx->uic_member : (unsigned)(getuid() & 0377));
    printf("Base priority:     4\n");
    printf("Default file spec: ");

    char vms_dir[512];
    dcl_format_directory(ctx->default_linux, vms_dir, sizeof(vms_dir));
    printf("%s\n", vms_dir);

    /* Privileges — read from VMS_PRIVILEGES env var or PCB */
    const char *privs = getenv("VMS_PRIVILEGES");
    if (privs && privs[0]) {
        printf("Privileges:        %s\n", privs);
    } else {
        printf("Privileges:        TMPMBX NETMBX\n");
    }

    /* Quotas — display standard VMS quota fields */
    printf("\nProcess quotas:\n");
    printf(" CPU limit:                    (none)  Direct I/O limit:          18\n");
    printf(" Buffered I/O byte count quota: 20480  Buffered I/O limit:        18\n");
    printf(" Timer queue entry quota:           10  Open file quota:           28\n");
    printf(" Paging file quota:              65536  Subprocess quota:          8\n");
    printf(" Default page fault cluster:        64  AST limit:                 23\n");
    printf(" Enqueue quota:                    100  Shared file limit:          0\n");
    printf(" Max detached processes:             0  Max active jobs:            0\n");

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

/*
 * SHOW DEVICE - List mounted filesystems as VMS devices.
 */
static int cmd_show_device(struct dcl_command *cmd)
{
    (void)cmd;

    printf("Device                  Device           Error    Volume"
           "         Free  Trans Mnt\n");
    printf(" Name                   Status           Count     Label"
           "        Blocks Count Cnt\n");

    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        /* Fallback: show at least root */
        struct statvfs svfs;
        if (statvfs("/", &svfs) == 0) {
            unsigned long long free_blocks =
                (unsigned long long)svfs.f_bavail *
                (svfs.f_bsize / 512 ? svfs.f_bsize / 512 : 1);
            printf("$1$DGA0:              Mounted              0  %-14s%9llu     1   1\n",
                   "OVMXSYS", free_blocks);
        }
        return SS$_NORMAL;
    }

    char line[512];
    int dev_index = 0;
    while (fgets(line, sizeof(line), fp) && dev_index < 16) {
        char dev[256], mntpt[256], fstype[64], opts[256];
        int freq, passno;
        if (sscanf(line, "%255s %255s %63s %255s %d %d",
                   dev, mntpt, fstype, opts, &freq, &passno) < 3)
            continue;

        /* Skip pseudo filesystems */
        if (strcmp(fstype, "proc") == 0 || strcmp(fstype, "sysfs") == 0 ||
            strcmp(fstype, "devtmpfs") == 0 || strcmp(fstype, "tmpfs") == 0 ||
            strcmp(fstype, "cgroup") == 0 || strcmp(fstype, "cgroup2") == 0 ||
            strcmp(fstype, "devpts") == 0 || strcmp(fstype, "mqueue") == 0 ||
            strcmp(fstype, "hugetlbfs") == 0 || strcmp(fstype, "pstore") == 0 ||
            strcmp(fstype, "securityfs") == 0 || strcmp(fstype, "debugfs") == 0 ||
            strcmp(fstype, "bpf") == 0 || strcmp(fstype, "tracefs") == 0)
            continue;

        /* Build VMS device name */
        char vms_dev[32];
        snprintf(vms_dev, sizeof(vms_dev), "$1$DGA%d:", dev_index);

        /* Build a short label from mount point */
        char label[16];
        if (strcmp(mntpt, "/") == 0) {
            strncpy(label, "OVMXSYS", sizeof(label) - 1);
        } else {
            /* Use last path component, uppercased, max 12 chars */
            char *last = strrchr(mntpt, '/');
            const char *base = last ? last + 1 : mntpt;
            size_t li;
            for (li = 0; li < sizeof(label) - 1 && base[li]; li++)
                label[li] = (char)toupper((unsigned char)base[li]);
            label[li] = '\0';
            if (li == 0) {
                strncpy(label, "DISK", sizeof(label) - 1);
                label[sizeof(label) - 1] = '\0';
            }
        }

        /* Get free blocks (512-byte) */
        unsigned long long free_512 = 0;
        struct statvfs svfs;
        if (statvfs(mntpt, &svfs) == 0) {
            unsigned long bsize = svfs.f_bsize ? svfs.f_bsize : 512;
            free_512 = (unsigned long long)svfs.f_bavail * (bsize / 512 + (bsize % 512 ? 1 : 0));
        }

        printf("%-24s Mounted              0  %-14s%9llu     1   1\n",
               vms_dev, label, free_512);
        dev_index++;
    }
    fclose(fp);

    if (dev_index == 0) {
        /* Nothing printed — show a stub */
        printf("$1$DGA0:              Mounted              0  %-14s%9d     1   1\n",
               "OVMXSYS", 0);
    }

    return SS$_NORMAL;
}

/*
 * SHOW MEMORY - Display physical memory usage.
 */
static int cmd_show_memory(struct dcl_command *cmd)
{
    (void)cmd;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    /* Read /proc/meminfo */
    long total_kb = 0, free_kb = 0, available_kb = 0;
    long buffers_kb = 0, cached_kb = 0;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            long val = 0;
            if (sscanf(line, "MemTotal: %ld", &val) == 1)      total_kb = val;
            else if (sscanf(line, "MemFree: %ld", &val) == 1)  free_kb = val;
            else if (sscanf(line, "MemAvailable: %ld", &val) == 1) available_kb = val;
            else if (sscanf(line, "Buffers: %ld", &val) == 1)  buffers_kb = val;
            else if (sscanf(line, "Cached: %ld", &val) == 1)   cached_kb = val;
        }
        fclose(fp);
    }

    /* VMS uses 512-byte pages */
    long page_size_bytes = 512;
    long total_pages  = (total_kb  * 1024L) / page_size_bytes;
    long free_pages   = (free_kb   * 1024L) / page_size_bytes;
    long avail_pages  = (available_kb * 1024L) / page_size_bytes;
    long buf_pages    = (buffers_kb * 1024L) / page_size_bytes;
    long cached_pages = (cached_kb  * 1024L) / page_size_bytes;
    long inuse_pages  = total_pages - free_pages;
    double total_mb   = (double)total_kb / 1024.0;

    printf("\n              System Memory Resources on %2d-%s-%04d"
           " %02d:%02d:%02d.%02d\n\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));
    printf("Physical Memory Usage (pages):\n");
    printf("    Total Physical Pages   %9ld   Main Memory (MB)   %9.2f\n",
           total_pages, total_mb);
    printf("    Free List Size         %9ld\n", free_pages);
    printf("    Modified List Size     %9ld\n", buf_pages + cached_pages);
    printf("    Available              %9ld\n", avail_pages);
    printf("    In Use                 %9ld\n", inuse_pages);

    return SS$_NORMAL;
}

/*
 * SHOW STATUS - Display last command exit status.
 */
static int cmd_show_status(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    uint32_t status = ctx->last_status ? ctx->last_status : SS$_NORMAL;

    /* Determine severity and message */
    int severity = status & 7;
    const char *sev_str;
    const char *fac_str = "SYSTEM";
    const char *msg;

    switch (severity) {
    case 1: sev_str = "S"; break;
    case 0: sev_str = "W"; break;
    case 2: sev_str = "E"; break;
    case 4: sev_str = "F"; break;
    default: sev_str = "I"; break;
    }

    if (status == SS$_NORMAL)
        msg = "normal successful completion";
    else if (status == 0)
        msg = "image exit";
    else
        msg = "condition";

    printf("  Status at %2d-%s-%04d %02d:%02d:%02d.%02d\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));
    printf("  Condition value: %%X%08X\n", status);
    printf("  Message: %%%s-%s-NORMAL, %s\n", fac_str, sev_str, msg);

    return SS$_NORMAL;
}

/*
 * SHOW TERMINAL - Display terminal characteristics.
 */
static int cmd_show_terminal(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();

    int width = 80, height = 24;
    int is_vt100 = 1;

    /* Try to get terminal size */
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) width  = (int)ws.ws_col;
        if (ws.ws_row > 0) height = (int)ws.ws_row;
    }

    /* Terminal name */
    const char *term_name = "_FTA0:";
    char *term_env = getenv("TERM");
    if (term_env && (strncasecmp(term_env, "vt", 2) == 0 ||
                     strncasecmp(term_env, "xterm", 5) == 0))
        is_vt100 = 1;
    else if (term_env && strncasecmp(term_env, "ansi", 4) == 0)
        is_vt100 = 1;

    (void)is_vt100;

    const char *owner = ctx->username[0] ? ctx->username : "SYSTEM";

    printf("Terminal: %-12s Device_Type: VT100         Owner: %s\n\n",
           term_name, owner);
    printf("Terminal Characteristics:\n");
    printf("  Interactive         Echo               Type_ahead          Hostsync\n");
    printf("  TTsync              Lowercase          Tab                 Wrap\n");
    printf("  Width: %3d          Page: %3d\n", width, height);

    return SS$_NORMAL;
}

/*
 * SHOW PROCESS /PRIVILEGES - Display process privilege mask.
 */
static int cmd_show_process_privileges(struct dcl_context *ctx)
{
    /* Known VMS privileges in approximate display order */
    static const struct {
        const char *name;
        uint64_t    bit;
        const char *desc;
    } privs[] = {
        { "TMPMBX",  (1ULL << 0),  "may create temporary mailbox"   },
        { "NETMBX",  (1ULL << 1),  "may create network device"      },
        { "GRPNAM",  (1ULL << 2),  "may insert in group logical name table" },
        { "SYSNAM",  (1ULL << 3),  "may insert in system logical name table" },
        { "OPER",    (1ULL << 4),  "operator privilege"             },
        { "SYSPRV",  (1ULL << 5),  "may access objects via system protection" },
        { "BYPASS",  (1ULL << 6),  "may bypass object access control" },
        { "CMKRNL",  (1ULL << 7),  "may change mode to kernel"      },
        { "CMEXEC",  (1ULL << 8),  "may change mode to executive"   },
        { "SYSNAM",  (1ULL << 9),  "may insert in system logical name table" },
        { "MOUNT",   (1ULL << 10), "may execute mount volume QIO"   },
        { "VOLPRO",  (1ULL << 11), "may override volume protection" },
        { "PHY_IO",  (1ULL << 12), "may issue physical I/O"         },
        { "LOG_IO",  (1ULL << 13), "may issue logical I/O"          },
        { "PSWAPM",  (1ULL << 14), "may change process swap mode"   },
        { "DETACH",  (1ULL << 15), "may create detached processes"  },
        { "ACNT",    (1ULL << 16), "may disable accounting"         },
        { "PRMCEB",  (1ULL << 17), "may create permanent common event flag" },
        { "PRMGBL",  (1ULL << 18), "may create permanent global sections" },
        { "PRMMBX",  (1ULL << 19), "may create permanent mailbox"   },
        { "EXQUOTA", (1ULL << 20), "may exceed disk quota"          },
        { "ALTPRI",  (1ULL << 21), "may set any base priority"      },
        { "SETPRV",  (1ULL << 22), "may set any privilege"          },
        { "WORLD",   (1ULL << 23), "may affect other processes in system" },
        { "SHARE",   (1ULL << 24), "may assign channel to non-shared device" },
        { NULL, 0, NULL }
    };

    /* Read privileges: from context (set via VMS_PRIVILEGES env var), else default */
    uint64_t privmask = ctx->privileges;
    if (privmask == 0) {
        /* Default: give TMPMBX and NETMBX */
        privmask = (1ULL << 0) | (1ULL << 1);
    }

    printf("Process privileges:\n");
    int found = 0;
    for (int i = 0; privs[i].name; i++) {
        if (privmask & privs[i].bit) {
            printf(" %-16s %s\n", privs[i].name, privs[i].desc);
            found++;
        }
    }
    if (!found)
        printf(" (no privileges enabled)\n");

    return SS$_NORMAL;
}

/*
 * SHOW PROCESS /QUOTAS - Display process quotas.
 */
static int cmd_show_process_quotas(struct dcl_context *ctx)
{
    const char *acct = ctx->username[0] ? ctx->username : "SYSTEM";

    printf("Process Quotas:\n");
    printf(" Account name: %s\n", acct);
    printf(" CPU limit:                      Infinite"
           "  Direct I/O limit:        40\n");
    printf(" Buffered I/O byte count quota:    32768"
           "  Buffered I/O limit:      40\n");
    printf(" Timer queue entry quota:            30"
           "  Open file quota:         40\n");
    printf(" Paging file quota:              32768"
           "  Subprocess quota:         8\n");
    printf(" AST quota:                        40"
           "  Enqueue quota:          300\n");

    return SS$_NORMAL;
}

/*
 * SHOW TRANSLATION - Translate a logical name (show full chain).
 */
static int cmd_show_translation(struct dcl_command *cmd)
{
    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing logical name - supply name to translate");
        return SS$_BADPARAM;
    }

    const char *logname = cmd->params[1];

    /* Uppercase for display */
    char upper_name[256];
    size_t i;
    for (i = 0; i < sizeof(upper_name) - 1 && logname[i]; i++)
        upper_name[i] = (char)toupper((unsigned char)logname[i]);
    upper_name[i] = '\0';

    char value[256];
    if (dcl_translate_logical(logname, value, sizeof(value)) != 0) {
        dcl_error("DCL", 0, "NOLOG", "no logical name match");
        return SS$_NOLOGNAM;
    }

    /* Print translation chain (resolve iteratively up to 8 levels) */
    printf("   \"%s\" = \"%s\"\n", upper_name, value);

    char current[256];
    strncpy(current, value, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    int depth = 0;
    while (depth < 8) {
        char next[256];
        if (dcl_translate_logical(current, next, sizeof(next)) != 0)
            break;
        /* Avoid cycles */
        if (strcmp(next, current) == 0) break;

        /* Uppercase current for display */
        char upper_cur[256];
        for (i = 0; i < sizeof(upper_cur) - 1 && current[i]; i++)
            upper_cur[i] = (char)toupper((unsigned char)current[i]);
        upper_cur[i] = '\0';

        printf("   \"%s\" = \"%s\"\n", upper_cur, next);
        strncpy(current, next, sizeof(current) - 1);
        current[sizeof(current) - 1] = '\0';
        depth++;
    }

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
    if (dcl_match_command(subcmd, "DEVICE", 3))
        return cmd_show_device(cmd);
    if (dcl_match_command(subcmd, "MEMORY", 3))
        return cmd_show_memory(cmd);
    if (dcl_match_command(subcmd, "STATUS", 4))
        return cmd_show_status(cmd);
    if (dcl_match_command(subcmd, "TERMINAL", 4))
        return cmd_show_terminal(cmd);
    if (dcl_match_command(subcmd, "TRANSLATION", 5))
        return cmd_show_translation(cmd);

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
 * SET TERMINAL /WIDTH= /PAGE= /ECHO /NOECHO /WRAP /NOWRAP
 *
 * Stores settings in context and applies them to the real terminal
 * via ioctl(TIOCSWINSZ) for width/page.  ECHO is toggled via termios.
 */
static int cmd_set_terminal(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* /WIDTH=n */
    const char *width_val = dcl_qualifier_value(cmd, "WIDTH");
    if (width_val && *width_val) {
        int w = atoi(width_val);
        if (w < 1 || w > 32767) {
            dcl_error("SET", 2, "INVWIDTH",
                      "invalid terminal width - \\%s\\", width_val);
            return SS$_BADPARAM;
        }
        ctx->term_width = w;
    }

    /* /PAGE=n */
    const char *page_val = dcl_qualifier_value(cmd, "PAGE");
    if (page_val && *page_val) {
        int p = atoi(page_val);
        if (p < 0 || p > 32767) {
            dcl_error("SET", 2, "INVPAGE",
                      "invalid terminal page length - \\%s\\", page_val);
            return SS$_BADPARAM;
        }
        ctx->term_page = p;
    }

    /* /ECHO vs /NOECHO */
    if (dcl_has_qualifier(cmd, "NOECHO")) {
        ctx->term_echo = 0;
    } else if (dcl_has_qualifier(cmd, "ECHO")) {
        ctx->term_echo = 1;
    }

    /* /WRAP vs /NOWRAP */
    if (dcl_has_qualifier(cmd, "NOWRAP")) {
        ctx->term_wrap = 0;
    } else if (dcl_has_qualifier(cmd, "WRAP")) {
        ctx->term_wrap = 1;
    }

    /* Apply width/page to terminal window size if stdout is a tty */
    if (isatty(STDOUT_FILENO)) {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
            if (width_val && *width_val)
                ws.ws_col = (unsigned short)ctx->term_width;
            if (page_val && *page_val)
                ws.ws_row = (unsigned short)ctx->term_page;
            ioctl(STDOUT_FILENO, TIOCSWINSZ, &ws);
        }
    }

    /* Apply echo setting via termios */
    if (isatty(STDIN_FILENO)) {
        struct termios tio;
        if (tcgetattr(STDIN_FILENO, &tio) == 0) {
            if (ctx->term_echo)
                tio.c_lflag |= ECHO;
            else
                tio.c_lflag &= ~(tcflag_t)ECHO;
            tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        }
    }

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
 * SET MESSAGE /FACILITY /NOFACILITY /SEVERITY /NOSEVERITY
 *             /IDENTIFICATION /NOIDENTIFICATION /TEXT /NOTEXT
 *
 * Controls which components of VMS error messages are displayed.
 * On OpenVMS, dcl_error() respects these flags when formatting output.
 */
static int cmd_set_message(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* /[NO]FACILITY */
    if (dcl_has_qualifier(cmd, "NOFACILITY"))
        ctx->msg_facility = 0;
    else if (dcl_has_qualifier(cmd, "FACILITY"))
        ctx->msg_facility = 1;

    /* /[NO]SEVERITY */
    if (dcl_has_qualifier(cmd, "NOSEVERITY"))
        ctx->msg_severity = 0;
    else if (dcl_has_qualifier(cmd, "SEVERITY"))
        ctx->msg_severity = 1;

    /* /[NO]IDENTIFICATION */
    if (dcl_has_qualifier(cmd, "NOIDENTIFICATION") ||
        dcl_has_qualifier(cmd, "NOIDENT"))
        ctx->msg_ident = 0;
    else if (dcl_has_qualifier(cmd, "IDENTIFICATION") ||
             dcl_has_qualifier(cmd, "IDENT"))
        ctx->msg_ident = 1;

    /* /[NO]TEXT */
    if (dcl_has_qualifier(cmd, "NOTEXT"))
        ctx->msg_text = 0;
    else if (dcl_has_qualifier(cmd, "TEXT"))
        ctx->msg_text = 1;

    return SS$_NORMAL;
}

/*
 * SET CONTROL[=(item,...)] / SET NOCONTROL[=(item,...)]
 *
 * Enables or disables Ctrl-Y (and Ctrl-C) trapping.
 * SET CONTROL=Y   — enable Ctrl-Y interrupt
 * SET NOCONTROL=Y — disable Ctrl-Y interrupt
 *
 * The value is in cmd->params[1] for "SET CONTROL=Y" style,
 * or parsed from qualifiers. VMS also allows SET CONTROL=(Y,T).
 */
static int cmd_set_control(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /*
     * Determine enable vs disable from the subcommand word.
     * "CONTROL"   => enable
     * "NOCONTROL" => disable
     */
    int enable = 1;
    if (cmd->param_count >= 1 &&
        dcl_match_command(cmd->params[0], "NOCONTROL", 9))
        enable = 0;

    /*
     * The item list may be in params[1] (bare word after =) or in
     * the qualifier value when parsed as /CONTROL=Y.  Accept both.
     * Default target is Y when no item specified.
     */
    const char *item = (cmd->param_count >= 2) ? cmd->params[1] : "Y";

    /* Parse item list — may be "(Y)" or "Y" or "(Y,T)" */
    char item_buf[64];
    strncpy(item_buf, item, sizeof(item_buf) - 1);
    item_buf[sizeof(item_buf) - 1] = '\0';

    /* Strip surrounding parens */
    size_t ilen = strlen(item_buf);
    if (ilen > 0 && item_buf[0] == '(') {
        memmove(item_buf, item_buf + 1, ilen);
        ilen--;
    }
    if (ilen > 0 && item_buf[ilen - 1] == ')') {
        item_buf[ilen - 1] = '\0';
    }

    /* Walk comma-separated tokens */
    char *saveptr = NULL;
    char *tok = strtok_r(item_buf, ",", &saveptr);
    while (tok) {
        while (*tok == ' ') tok++;
        if (strcasecmp(tok, "Y") == 0) {
            ctx->ctrl_y_enabled = enable;
        }
        /* T = Ctrl-T (broadcast) — track but not implemented beyond flag */
        tok = strtok_r(NULL, ",", &saveptr);
    }

    return SS$_NORMAL;
}

/*
 * SET PROCESS /NAME=procname /PRIORITY=n /PRIVILEGES=(priv,...)
 *
 * /NAME=    — rename the process name (stored in context; no OS rename)
 * /PRIORITY= — set base priority; requires ALTPRI privilege
 * /PRIVILEGES= — set process privileges; requires SETPRV or OPER
 */
static int cmd_set_process(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* /NAME=name */
    const char *name_val = dcl_qualifier_value(cmd, "NAME");
    if (name_val && *name_val) {
        strncpy(ctx->process_name, name_val, sizeof(ctx->process_name) - 1);
        ctx->process_name[sizeof(ctx->process_name) - 1] = '\0';
        /* Upper-case the name, VMS style */
        for (size_t i = 0; ctx->process_name[i]; i++)
            ctx->process_name[i] = (char)toupper((unsigned char)ctx->process_name[i]);
    }

    /* /PRIORITY=n — requires ALTPRI */
    const char *pri_val = dcl_qualifier_value(cmd, "PRIORITY");
    if (pri_val && *pri_val) {
        if (!(ctx->privileges & PRV$M_ALTPRI) &&
            !(ctx->privileges & PRV$M_SYSPRV) &&
            !(ctx->privileges & PRV$M_BYPASS)) {
            dcl_error("SET", 2, "NOPRIV",
                      "no privilege for SET PROCESS /PRIORITY");
            return SS$_NOPRIV;
        }
        int pri = atoi(pri_val);
        if (pri < 0 || pri > 31) {
            dcl_error("SET", 2, "INVPRI",
                      "invalid priority \\%d\\ - must be 0-31", pri);
            return SS$_BADPARAM;
        }
        ctx->process_priority = pri;
        /* Best-effort: try to set Linux scheduling niceness proportionally */
        /* VMS pri 0 = lowest, 15 = normal, 31 = highest
         * Linux nice: -20 (highest) to +19 (lowest) */
        int nice_val = 19 - (pri * 39) / 31;
        setpriority(PRIO_PROCESS, 0, nice_val);
    }

    /* /PRIVILEGES=(priv,...) — requires SETPRV or OPER */
    const char *privs_val = dcl_qualifier_value(cmd, "PRIVILEGES");
    if (privs_val && *privs_val) {
        if (!(ctx->privileges & PRV$M_SETPRV) &&
            !(ctx->privileges & PRV$M_SYSPRV) &&
            !(ctx->privileges & PRV$M_BYPASS)) {
            dcl_error("SET", 2, "NOPRIV",
                      "no privilege for SET PROCESS /PRIVILEGES");
            return SS$_NOPRIV;
        }
        /* Strip outer parens if present */
        char pv[512];
        strncpy(pv, privs_val, sizeof(pv) - 1);
        pv[sizeof(pv) - 1] = '\0';
        size_t pvlen = strlen(pv);
        if (pvlen > 0 && pv[0] == '(') {
            memmove(pv, pv + 1, pvlen);
            pvlen--;
        }
        if (pvlen > 0 && pv[pvlen - 1] == ')') {
            pv[pvlen - 1] = '\0';
        }
        ctx->privileges = parse_privilege_string(pv);
    }

    return SS$_NORMAL;
}

/*
 * SET FILE /VERSION_LIMIT=n /EXPIRATION_DATE=ddmmyyyy
 *
 * /VERSION_LIMIT — sets ODS-2 version limit on a file.
 *   Under Linux this is advisory (no direct FS support); we record
 *   the intent but cannot enforce it at the kernel level.
 * /EXPIRATION_DATE — sets the file expiration date (utimes).
 */
static int cmd_set_file(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* Need at least a filespec after SET FILE */
    if (cmd->param_count < 2) {
        dcl_error("SET", 2, "NOFILES",
                  "missing file specification");
        return SS$_BADPARAM;
    }

    const char *filespec = cmd->params[1];
    char linux_path[1024];
    dcl_resolve_path(ctx, filespec, linux_path, sizeof(linux_path));

    struct stat st;
    if (stat(linux_path, &st) != 0) {
        dcl_error("SET", 2, "NOSUCHFILE",
                  "file not found - \\%s\\", filespec);
        return SS$_NOSUCHFILE;
    }

    /* /VERSION_LIMIT=n — advisory; just validate and acknowledge */
    const char *vl = dcl_qualifier_value(cmd, "VERSION_LIMIT");
    if (vl && *vl) {
        int vlim = atoi(vl);
        if (vlim < 0 || vlim > 32767) {
            dcl_error("SET", 2, "INVVLIM",
                      "invalid version limit \\%s\\", vl);
            return SS$_BADPARAM;
        }
        /* On Linux/ext4 there is no native version-limit support.
         * We acknowledge the setting without error. */
    }

    /* /EXPIRATION_DATE=dd-mmm-yyyy or absolute quadword in decimal.
     * Accept VMS date string format: dd-MMM-yyyy[:hh:mm:ss] */
    const char *exp_date = dcl_qualifier_value(cmd, "EXPIRATION_DATE");
    if (exp_date && *exp_date) {
        struct tm exp_tm;
        memset(&exp_tm, 0, sizeof(exp_tm));

        /* Parse VMS date: dd-MMM-yyyy or dd-MMM-yyyy:hh:mm:ss */
        static const char *mon_names[] = {
            "JAN","FEB","MAR","APR","MAY","JUN",
            "JUL","AUG","SEP","OCT","NOV","DEC"
        };

        char date_buf[64];
        strncpy(date_buf, exp_date, sizeof(date_buf) - 1);
        date_buf[sizeof(date_buf) - 1] = '\0';
        /* Upper-case for comparison */
        for (size_t i = 0; date_buf[i]; i++)
            date_buf[i] = (char)toupper((unsigned char)date_buf[i]);

        int day = 0, mon = -1, year = 0;
        int hr = 0, mi = 0, sc = 0;
        char mon_str[4] = {0};

        /* Try dd-MMM-yyyy:hh:mm:ss then dd-MMM-yyyy */
        int parsed = 0;
        if (sscanf(date_buf, "%d-%3s-%d:%d:%d:%d",
                   &day, mon_str, &year, &hr, &mi, &sc) >= 3)
            parsed = 1;
        else if (sscanf(date_buf, "%d-%3s-%d", &day, mon_str, &year) == 3)
            parsed = 1;

        if (parsed) {
            for (int m = 0; m < 12; m++) {
                if (strncmp(mon_str, mon_names[m], 3) == 0) {
                    mon = m;
                    break;
                }
            }
        }

        if (!parsed || mon < 0) {
            dcl_error("SET", 2, "IVTIME",
                      "invalid expiration date - \\%s\\", exp_date);
            return SS$_IVTIME;
        }

        exp_tm.tm_mday = day;
        exp_tm.tm_mon  = mon;
        exp_tm.tm_year = year - 1900;
        exp_tm.tm_hour = hr;
        exp_tm.tm_min  = mi;
        exp_tm.tm_sec  = sc;
        exp_tm.tm_isdst = -1;

        time_t exp_t = mktime(&exp_tm);
        if (exp_t == (time_t)-1) {
            dcl_error("SET", 2, "IVTIME",
                      "cannot convert expiration date - \\%s\\", exp_date);
            return SS$_IVTIME;
        }

        /* Set atime = now, mtime = expiration date */
        struct timeval tv[2];
        gettimeofday(&tv[0], NULL);
        tv[1].tv_sec  = exp_t;
        tv[1].tv_usec = 0;
        if (utimes(linux_path, tv) != 0) {
            dcl_error("SET", 2, "PRV",
                      "cannot set expiration date - %s", strerror(errno));
            return SS$_NOPRIV;
        }
    }

    return SS$_NORMAL;
}

/*
 * SET UIC [uic]
 *
 * Changes the current process UIC (user identification code).
 * On OpenVMS: SET UIC [group,member]
 * Requires SETPRV, SYSPRV, or BYPASS privilege to change to another UIC.
 * Without privilege, only reports the current UIC.
 */
static int cmd_set_uic(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        /* No argument: display current UIC */
        printf("  Current UIC: [%03o,%03o]\n", ctx->uic_group, ctx->uic_member);
        return SS$_NORMAL;
    }

    const char *uic_str = cmd->params[1];

    /* Check privilege */
    if (!(ctx->privileges & PRV$M_SETPRV) &&
        !(ctx->privileges & PRV$M_SYSPRV) &&
        !(ctx->privileges & PRV$M_BYPASS)) {
        dcl_error("SET", 2, "NOPRIV",
                  "no privilege for SET UIC");
        return SS$_NOPRIV;
    }

    /* Parse [group,member] in octal — strip brackets */
    char uic_buf[64];
    strncpy(uic_buf, uic_str, sizeof(uic_buf) - 1);
    uic_buf[sizeof(uic_buf) - 1] = '\0';

    size_t ulen = strlen(uic_buf);
    if (ulen > 0 && uic_buf[0] == '[') {
        memmove(uic_buf, uic_buf + 1, ulen);
        ulen--;
    }
    if (ulen > 0 && uic_buf[ulen - 1] == ']') {
        uic_buf[ulen - 1] = '\0';
    }

    unsigned int grp = 0, mem = 0;
    if (sscanf(uic_buf, "%o,%o", &grp, &mem) != 2) {
        dcl_error("SET", 2, "IVUIC",
                  "invalid UIC format - \\%s\\ (expected [group,member])", uic_str);
        return SS$_BADPARAM;
    }

    ctx->uic_group  = grp;
    ctx->uic_member = mem;

    return SS$_NORMAL;
}

/*
 * SET WORKING_SET /QUOTA=n /EXTENT=n /LIMIT=n
 *
 * Controls process working set size.  On Linux we map /QUOTA to
 * RLIMIT_AS (virtual address space) as the closest approximation.
 * VMS page size is 512 bytes; values are in pages.
 */
static int cmd_set_working_set(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

#define VMS_PAGE_SIZE 512

    /* /QUOTA=n pages */
    const char *quota_val = dcl_qualifier_value(cmd, "QUOTA");
    if (quota_val && *quota_val) {
        int pages = atoi(quota_val);
        if (pages < 0) {
            dcl_error("SET", 2, "INVQUO",
                      "invalid working set quota \\%s\\", quota_val);
            return SS$_BADPARAM;
        }
        ctx->ws_quota = pages;

        /* Best-effort: adjust RLIMIT_DATA */
        if (pages > 0) {
            struct rlimit rl;
            if (getrlimit(RLIMIT_DATA, &rl) == 0) {
                rlim_t new_limit = (rlim_t)pages * VMS_PAGE_SIZE;
                if (rl.rlim_max == RLIM_INFINITY || new_limit <= rl.rlim_max) {
                    rl.rlim_cur = new_limit;
                    setrlimit(RLIMIT_DATA, &rl);
                }
            }
        }
    }

    /* /EXTENT=n and /LIMIT=n are also valid — acknowledge silently */
    /* (EXTENT = maximum working set, LIMIT = minimum guaranteed pages) */

    return SS$_NORMAL;
#undef VMS_PAGE_SIZE
}

/*
 * SET TIME [dd-mmm-yyyy:hh:mm:ss] — set system clock (privileged).
 *
 * Requires OPER or SYSPRV privilege.  Uses settimeofday(2).
 */
static int cmd_set_time(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        /* No argument: display current time (same as SHOW TIME).
         * Reading the clock requires no privilege on VMS or Linux. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);
        static const char *mon_abbr[] = {
            "JAN","FEB","MAR","APR","MAY","JUN",
            "JUL","AUG","SEP","OCT","NOV","DEC"
        };
        printf("  %2d-%s-%04d %02d:%02d:%02d.%02d\n",
               tm.tm_mday, mon_abbr[tm.tm_mon], 1900 + tm.tm_year,
               tm.tm_hour, tm.tm_min, tm.tm_sec,
               (int)(ts.tv_nsec / 10000000));
        return SS$_NORMAL;
    }

    /* Privilege check — required when actually setting the clock */
    if (!(ctx->privileges & PRV$M_OPER) &&
        !(ctx->privileges & PRV$M_SYSPRV) &&
        !(ctx->privileges & PRV$M_BYPASS)) {
        dcl_error("SET", 2, "NOPRIV",
                  "no privilege for SET TIME");
        return SS$_NOPRIV;
    }

    const char *time_str = cmd->params[1];

    /* Parse VMS format: dd-MMM-yyyy:hh:mm:ss or hh:mm:ss (time only) */
    static const char *mon_names[] = {
        "JAN","FEB","MAR","APR","MAY","JUN",
        "JUL","AUG","SEP","OCT","NOV","DEC"
    };

    char ts_buf[64];
    strncpy(ts_buf, time_str, sizeof(ts_buf) - 1);
    ts_buf[sizeof(ts_buf) - 1] = '\0';
    for (size_t i = 0; ts_buf[i]; i++)
        ts_buf[i] = (char)toupper((unsigned char)ts_buf[i]);

    struct tm new_tm;
    memset(&new_tm, 0, sizeof(new_tm));

    /* Get current local time as base */
    time_t now = time(NULL);
    localtime_r(&now, &new_tm);

    int day = 0, mon = -1, year = 0;
    int hr = 0, mi = 0, sc = 0;
    char mon_str[4] = {0};
    int have_date = 0;

    /* Try full datetime first, then time-only */
    if (sscanf(ts_buf, "%d-%3s-%d:%d:%d:%d",
               &day, mon_str, &year, &hr, &mi, &sc) >= 3) {
        have_date = 1;
    } else if (sscanf(ts_buf, "%d:%d:%d", &hr, &mi, &sc) >= 2) {
        /* time only — keep current date */
    } else {
        dcl_error("SET", 2, "IVTIME",
                  "invalid time specification - \\%s\\", time_str);
        return SS$_IVTIME;
    }

    if (have_date) {
        for (int m = 0; m < 12; m++) {
            if (strncmp(mon_str, mon_names[m], 3) == 0) {
                mon = m;
                break;
            }
        }
        if (mon < 0) {
            dcl_error("SET", 2, "IVTIME",
                      "invalid month - \\%s\\", mon_str);
            return SS$_IVTIME;
        }
        new_tm.tm_mday = day;
        new_tm.tm_mon  = mon;
        new_tm.tm_year = year - 1900;
    }

    new_tm.tm_hour   = hr;
    new_tm.tm_min    = mi;
    new_tm.tm_sec    = sc;
    new_tm.tm_isdst  = -1;

    time_t new_t = mktime(&new_tm);
    if (new_t == (time_t)-1) {
        dcl_error("SET", 2, "IVTIME",
                  "cannot convert time - \\%s\\", time_str);
        return SS$_IVTIME;
    }

    struct timeval tv;
    tv.tv_sec  = new_t;
    tv.tv_usec = 0;

    if (settimeofday(&tv, NULL) != 0) {
        if (errno == EPERM) {
            dcl_error("SET", 2, "NOPRIV",
                      "cannot set system time - insufficient OS privilege");
            return SS$_NOPRIV;
        }
        dcl_error("SET", 2, "IVTIME",
                  "cannot set system time - %s", strerror(errno));
        return SS$_IVTIME;
    }

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
    if (dcl_match_command(subcmd, "TERMINAL", 4))
        return cmd_set_terminal(cmd);
    if (dcl_match_command(subcmd, "PROTECTION", 3))
        return cmd_set_protection(cmd);
    if (dcl_match_command(subcmd, "PASSWORD", 3))
        return cmd_set_password(cmd);
    if (dcl_match_command(subcmd, "NOON", 4)) {
        /* SET NOON — suppress ON ERROR handler at current level */
        struct dcl_context *noon_ctx = dcl_get_context();
        noon_ctx->noon_active = 1;
        return SS$_NORMAL;
    }
    if (dcl_match_command(subcmd, "ON", 2)) {
        /* SET ON — re-enable ON ERROR handler */
        struct dcl_context *on_ctx = dcl_get_context();
        on_ctx->noon_active = 0;
        return SS$_NORMAL;
    }
    if (dcl_match_command(subcmd, "MESSAGE", 3))
        return cmd_set_message(cmd);
    if (dcl_match_command(subcmd, "CONTROL", 4) ||
        dcl_match_command(subcmd, "NOCONTROL", 9))
        return cmd_set_control(cmd);
    if (dcl_match_command(subcmd, "PROCESS", 3))
        return cmd_set_process(cmd);
    if (dcl_match_command(subcmd, "FILE", 4))
        return cmd_set_file(cmd);
    if (dcl_match_command(subcmd, "UIC", 3))
        return cmd_set_uic(cmd);
    if (dcl_match_command(subcmd, "WORKING_SET", 4))
        return cmd_set_working_set(cmd);
    if (dcl_match_command(subcmd, "TIME", 4))
        return cmd_set_time(cmd);

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

    /* Collect all matching entries for sorting */
    struct dir_entry {
        char vms_name[256];  /* Formatted VMS name (UPPERCASE, with version) */
        char raw_name[256];  /* Original d_name for name part comparison */
        int  version;        /* Numeric version for sort (descending) */
        long blocks;
        struct stat st;
    };

    int capacity = 256;
    struct dir_entry *entries = malloc((size_t)capacity * sizeof(*entries));
    if (!entries) {
        closedir(dir);
        if (pattern) free((void *)pattern);
        return SS$_INSFMEM;
    }
    int entry_count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) continue;

        /* Apply wildcard filter if pattern specified.
         * Use vmsfs_wildcard_match() which handles VMS % (single-char) and * */
        if (pattern) {
            if (!vmsfs_wildcard_match(pattern, de->d_name)) continue;
        }

        /* Stat the file */
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s%s", linux_dir, de->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        /* Grow array if needed */
        if (entry_count >= capacity) {
            capacity *= 2;
            struct dir_entry *tmp = realloc(entries,
                                            (size_t)capacity * sizeof(*entries));
            if (!tmp) {
                free(entries);
                closedir(dir);
                if (pattern) free((void *)pattern);
                return SS$_INSFMEM;
            }
            entries = tmp;
        }

        struct dir_entry *e = &entries[entry_count];
        e->st = st;
        e->blocks = (st.st_size + 511) / 512;
        strncpy(e->raw_name, de->d_name, sizeof(e->raw_name) - 1);
        e->raw_name[sizeof(e->raw_name) - 1] = '\0';

        /* Format the filename in VMS style (uppercase) */
        size_t ni = 0;
        for (size_t i = 0; de->d_name[i] && ni < sizeof(e->vms_name) - 1; i++) {
            e->vms_name[ni++] = (char)toupper((unsigned char)de->d_name[i]);
        }
        e->vms_name[ni] = '\0';

        /* Determine version number.
         * If the filename already contains ;N, extract it.
         * Otherwise append ;1 for regular files. */
        char *semi = strrchr(e->vms_name, ';');
        if (semi && semi[1] != '\0') {
            /* Already has a version suffix — use it */
            e->version = atoi(semi + 1);
            /* Don't double-add: nothing to append */
        } else if (S_ISREG(st.st_mode)) {
            /* No version suffix — add ;1 */
            e->version = 1;
            strncat(e->vms_name, ";1",
                    sizeof(e->vms_name) - strlen(e->vms_name) - 1);
        } else {
            e->version = 0;  /* Directories don't have versions per se */
        }

        /* Add .DIR;1 suffix for subdirectories */
        if (S_ISDIR(st.st_mode)) {
            strncat(e->vms_name, ".DIR;1",
                    sizeof(e->vms_name) - strlen(e->vms_name) - 1);
        }

        /* Ensure a dot separator for regular files without one.
         * Insert '.' before ';1' so "FOO;1" becomes "FOO.;1". */
        if (S_ISREG(st.st_mode) && !strchr(de->d_name, '.')) {
            char *s = strrchr(e->vms_name, ';');
            if (s) {
                memmove(s + 1, s, strlen(s) + 1);
                *s = '.';
            }
        }

        entry_count++;
    }
    closedir(dir);

    /* Sort entries: name ascending (ignoring version), version descending.
     * VMS displays files alphabetically, with multiple versions of the same
     * file in descending version order.
     * Sort by vms_name up to (not including) ';', then version descending. */
    for (int i = 0; i < entry_count - 1; i++) {
        for (int j = i + 1; j < entry_count; j++) {
            /* Get name without version for comparison */
            char na[256], nb[256];
            strncpy(na, entries[i].vms_name, sizeof(na) - 1); na[sizeof(na)-1]='\0';
            strncpy(nb, entries[j].vms_name, sizeof(nb) - 1); nb[sizeof(nb)-1]='\0';
            char *sa = strrchr(na, ';'); if (sa) *sa = '\0';
            char *sb = strrchr(nb, ';'); if (sb) *sb = '\0';

            int cmp = strcasecmp(na, nb);
            if (cmp > 0 || (cmp == 0 && entries[i].version < entries[j].version)) {
                struct dir_entry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    int file_count = 0;
    long total_blocks = 0;
    int col = 0;
    int col_width = (show_size || show_date) ? 0 : (80 / columns);

    for (int idx = 0; idx < entry_count; idx++) {
        struct dir_entry *e = &entries[idx];
        const char *vms_name = e->vms_name;
        long blocks = e->blocks;
        struct stat *st = &e->st;

        total_blocks += blocks;
        file_count++;

        if (show_full) {
            /* Full listing: one file per line with all info */
            printf("%-39s", vms_name);
            printf(" %6ld", blocks);

            struct tm tm;
            localtime_r(&st->st_mtime, &tm);
            printf("  %2d-%s-%04d %02d:%02d:%02d.00",
                   tm.tm_mday, vms_months[tm.tm_mon],
                   1900 + tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec);

            /* Protection: use vmsfs functions for proper VMS format */
            uint16_t vprot = vmsfs_mode_to_protection(st->st_mode);
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
                localtime_r(&st->st_mtime, &tm);
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

    free(entries);

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
 * DELETE - Delete a file, or (with /SYMBOL) delete a symbol.
 */
static int cmd_delete(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* /SYMBOL qualifier: delete a symbol from the symbol table */
    if (dcl_has_qualifier(cmd, "SYMBOL")) {
        if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
            dcl_error("DCL", 2, "NOSYM", "missing symbol name");
            return SS$_BADPARAM;
        }

        int scope = DCL_SYM_LOCAL;
        if (dcl_has_qualifier(cmd, "GLOBAL")) {
            scope = DCL_SYM_GLOBAL;
        }

        int ret = dcl_sym_delete(cmd->params[0], scope);
        if (ret != 0) {
            dcl_error("DCL", 0, "NOSUCHSYM",
                      "no symbol \"%s\" found", cmd->params[0]);
        }
        return SS$_NORMAL;
    }

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
 * CREATE - Create a new file, or (with /DIRECTORY) create a directory.
 */
static int cmd_create(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* /DIRECTORY qualifier: create a directory instead of a file */
    if (dcl_has_qualifier(cmd, "DIRECTORY")) {
        if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
            dcl_error("DCL", 2, "NODIR", "missing directory specification");
            return SS$_BADPARAM;
        }

        char linux_path[1024];
        dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

        /* Remove trailing slash before mkdir */
        size_t plen = strlen(linux_path);
        if (plen > 1 && linux_path[plen - 1] == '/') {
            linux_path[plen - 1] = '\0';
        }

        if (mkdir(linux_path, 0755) != 0) {
            if (errno == EEXIST) {
                dcl_error("DCL", 0, "CREATED",
                          "directory already exists - %s", cmd->params[0]);
                return SS$_NORMAL;
            }
            dcl_error("RMS", 2, "CRE",
                      "cannot create directory - %s: %s",
                      cmd->params[0], strerror(errno));
            return SS$_FILACCERR;
        }

        return SS$_NORMAL;
    }

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    /* Validate against ODS-2 naming rules before resolving the path.
     * Strip any directory spec to get just the filename portion. */
    const char *fname_for_check = cmd->params[0];
    /* Skip past device: and [dir] if present */
    const char *bracket_end = strrchr(fname_for_check, ']');
    if (bracket_end) fname_for_check = bracket_end + 1;
    else {
        const char *col = strrchr(fname_for_check, ':');
        if (col) fname_for_check = col + 1;
    }
    /* Strip version ;N for ODS-2 check */
    char name_check[256];
    strncpy(name_check, fname_for_check, sizeof(name_check) - 1);
    name_check[sizeof(name_check) - 1] = '\0';
    char *semi_check = strrchr(name_check, ';');
    if (semi_check) *semi_check = '\0';

    if (name_check[0] != '\0' && !vmsfs_is_valid_ods2_name(name_check)) {
        dcl_error("RMS", 2, "SYN",
                  "invalid ODS-2 filename - %s", cmd->params[0]);
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

/* vmsfs version management API (from vmsfs/version.h) */
extern int vmsfs_purge_versions(const char *linux_dir, const char *basename,
                                const char *ext, int keep_count);
extern int vmsfs_list_versions(const char *linux_dir, const char *basename,
                               const char *ext, int *versions, int max_versions,
                               int *count);

/*
 * PURGE - Delete all but the highest N versions of files.
 *
 * Syntax: PURGE [filespec] [/KEEP=n]
 *   filespec  - VMS file specification (wildcards allowed); defaults to *.*
 *   /KEEP=n   - Number of versions to keep; default is 1
 *
 * Calls vmsfs_purge_versions() for each matching file base name found
 * in the target directory.
 */
static int cmd_purge(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* Determine keep count from /KEEP=n qualifier */
    int keep_count = 1;
    const char *keep_val = dcl_qualifier_value(cmd, "KEEP");
    if (keep_val && keep_val[0]) {
        keep_count = atoi(keep_val);
        if (keep_count < 1) keep_count = 1;
    }

    /* Determine the target directory and pattern */
    char linux_dir[1024];
    const char *pattern = NULL;

    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        char resolved[1024];
        dcl_resolve_path(ctx, cmd->params[0], resolved, sizeof(resolved));

        struct stat st;
        if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(linux_dir, resolved, sizeof(linux_dir) - 1);
            linux_dir[sizeof(linux_dir) - 1] = '\0';
        } else {
            /* Split path into directory + filename pattern */
            char *last_slash = strrchr(resolved, '/');
            if (last_slash) {
                pattern = strdup(last_slash + 1);
                *(last_slash + 1) = '\0';
                strncpy(linux_dir, resolved, sizeof(linux_dir) - 1);
                linux_dir[sizeof(linux_dir) - 1] = '\0';
            } else {
                pattern = strdup(resolved);
                strncpy(linux_dir, ctx->default_linux, sizeof(linux_dir) - 1);
                linux_dir[sizeof(linux_dir) - 1] = '\0';
            }
        }
    } else {
        /* Default: current directory, all files (*.*) */
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

    /* Scan the directory and collect unique base names (name without version).
     * For each unique base+ext, call vmsfs_purge_versions(). */
    DIR *dir = opendir(linux_dir);
    if (!dir) {
        dcl_error("RMS", 2, "DNF", "directory not found - %s", linux_dir);
        if (pattern) free((void *)pattern);
        return SS$_NOSUCHFILE;
    }

    /* Track processed bases to avoid duplicate purge calls */
    struct purge_base {
        char name[64];
        char ext[64];
    };
    int cap = 64;
    struct purge_base *bases = malloc((size_t)cap * sizeof(*bases));
    if (!bases) {
        closedir(dir);
        if (pattern) free((void *)pattern);
        return SS$_INSFMEM;
    }
    int base_count = 0;
    int total_deleted = 0;

    struct dirent *de;
    /* Remove trailing slash for opendir (already done) — iterate entries */
    /* Strip the trailing slash from linux_dir to use as dir arg to vmsfs */
    char dir_notrail[1024];
    strncpy(dir_notrail, linux_dir, sizeof(dir_notrail) - 1);
    dir_notrail[sizeof(dir_notrail) - 1] = '\0';
    size_t dtlen = strlen(dir_notrail);
    if (dtlen > 1 && dir_notrail[dtlen - 1] == '/') {
        dir_notrail[dtlen - 1] = '\0';
    }

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        /* Only process versioned files (must contain ;N) */
        const char *semi = strrchr(de->d_name, ';');
        if (!semi) continue;

        /* Apply wildcard pattern filter if one was given */
        if (pattern) {
            if (!vmsfs_wildcard_match(pattern, de->d_name)) continue;
        }

        /* Extract base (name) and ext from the portion before ';' */
        char base_ext[256];
        size_t belen = (size_t)(semi - de->d_name);
        if (belen >= sizeof(base_ext)) belen = sizeof(base_ext) - 1;
        memcpy(base_ext, de->d_name, belen);
        base_ext[belen] = '\0';

        /* Split base_ext into name and extension */
        char fname[64] = {0};
        char fext[64]  = {0};
        const char *dot = strrchr(base_ext, '.');
        if (dot) {
            size_t nlen = (size_t)(dot - base_ext);
            if (nlen >= sizeof(fname)) nlen = sizeof(fname) - 1;
            memcpy(fname, base_ext, nlen);
            fname[nlen] = '\0';
            strncpy(fext, dot + 1, sizeof(fext) - 1);
            fext[sizeof(fext) - 1] = '\0';
        } else {
            strncpy(fname, base_ext, sizeof(fname) - 1);
            fname[sizeof(fname) - 1] = '\0';
        }

        /* Check if we already processed this base */
        int already = 0;
        for (int k = 0; k < base_count; k++) {
            if (strcasecmp(bases[k].name, fname) == 0 &&
                strcasecmp(bases[k].ext,  fext)  == 0) {
                already = 1;
                break;
            }
        }
        if (already) continue;

        /* Record this base */
        if (base_count >= cap) {
            cap *= 2;
            struct purge_base *tmp = realloc(bases, (size_t)cap * sizeof(*bases));
            if (!tmp) { free(bases); closedir(dir);
                        if (pattern) free((void *)pattern);
                        return SS$_INSFMEM; }
            bases = tmp;
        }
        strncpy(bases[base_count].name, fname, sizeof(bases[0].name) - 1);
        bases[base_count].name[sizeof(bases[0].name) - 1] = '\0';
        strncpy(bases[base_count].ext,  fext,  sizeof(bases[0].ext)  - 1);
        bases[base_count].ext[sizeof(bases[0].ext) - 1] = '\0';
        base_count++;

        /* Purge old versions for this file */
        int deleted = vmsfs_purge_versions(dir_notrail, fname,
                                            fext[0] ? fext : NULL, keep_count);
        if (deleted > 0) {
            total_deleted += deleted;
        }
    }
    closedir(dir);
    free(bases);
    if (pattern) free((void *)pattern);

    if (total_deleted == 0) {
        printf("%%PURGE-I-NOPURGE, no file versions to purge\n");
    } else {
        printf("%%PURGE-I-PURGED, %d old version%s deleted\n",
               total_deleted, total_deleted != 1 ? "s" : "");
    }
    return SS$_NORMAL;
}

/*
 * APPEND - Append one file to another.
 * Format: APPEND source-filespec destination-filespec
 */
static int cmd_append(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing source and/or destination file specification");
        return SS$_BADPARAM;
    }

    char src_path[1024], dst_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], src_path, sizeof(src_path));
    dcl_resolve_path(ctx, cmd->params[1], dst_path, sizeof(dst_path));

    FILE *src = fopen(src_path, "rb");
    if (!src) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    FILE *dst = fopen(dst_path, "ab");
    if (!dst) {
        fclose(src);
        dcl_error("RMS", 2, "CRE", "cannot open for append - %s", cmd->params[1]);
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
 * WAIT - Wait for a specified time interval.
 * Format: WAIT delta-time (HH:MM:SS.cc or ::SS or :MM: etc.)
 * VMS accepts delta time: 0 00:00:30.00 or just 00:00:30
 */
static int cmd_wait(struct dcl_command *cmd)
{
    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "BADPARAM", "missing time specification");
        return SS$_BADPARAM;
    }

    const char *timestr = cmd->params[0];

    /* Parse delta time: [d ]HH:MM:SS[.cc]
     * VMS format: 0 00:00:30.00  (days HH:MM:SS.cc)
     * Common format: 00:00:30 (HH:MM:SS) or :30 (just seconds) */
    long days = 0, hours = 0, minutes = 0, seconds = 0;

    /* Check if there's a day component (contains space) */
    const char *p = timestr;
    const char *space = strchr(timestr, ' ');
    if (space) {
        days = strtol(p, NULL, 10);
        p = space + 1;
    }

    /* Parse HH:MM:SS */
    char hms[64];
    strncpy(hms, p, sizeof(hms) - 1);
    hms[sizeof(hms) - 1] = '\0';

    /* Remove fractional seconds */
    char *dot = strchr(hms, '.');
    if (dot) *dot = '\0';

    /* Count colons to determine format */
    int colon_count = 0;
    for (size_t i = 0; hms[i]; i++) {
        if (hms[i] == ':') colon_count++;
    }

    if (colon_count == 2) {
        /* HH:MM:SS */
        sscanf(hms, "%ld:%ld:%ld", &hours, &minutes, &seconds);
    } else if (colon_count == 1) {
        /* MM:SS or ::SS (leading colons) */
        if (hms[0] == ':') {
            /* :MM:SS or ::SS */
            sscanf(hms, ":%ld:%ld", &minutes, &seconds);
        } else {
            sscanf(hms, "%ld:%ld", &minutes, &seconds);
        }
    } else if (colon_count == 0) {
        seconds = strtol(hms, NULL, 10);
    }

    long total_seconds = days * 86400L + hours * 3600L + minutes * 60L + seconds;

    if (total_seconds < 0) {
        dcl_error("DCL", 2, "IVTIME", "invalid time specification - %s", timestr);
        return SS$_IVTIME;
    }

    if (total_seconds > 0) {
        sleep((unsigned int)total_seconds);
    }

    return SS$_NORMAL;
}

/*
 * ASSIGN - Assign a logical name.
 * Format: ASSIGN equivalence-name logical-name [/TABLE=table-name]
 * Unlike DEFINE, ASSIGN uses a different default table (LNM$PROCESS) and
 * has slightly different semantics for table placement.
 */
static int cmd_assign(struct dcl_command *cmd)
{
    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing equivalence name and/or logical name");
        return SS$_BADPARAM;
    }

    const char *equiv   = cmd->params[0];
    const char *logname = cmd->params[1];

    /* Uppercase the logical name (VMS convention) */
    char upper_name[256];
    size_t i;
    for (i = 0; i < sizeof(upper_name) - 1 && logname[i]; i++)
        upper_name[i] = (char)toupper((unsigned char)logname[i]);
    upper_name[i] = '\0';

    /* /TABLE qualifier selects target table — stub: we have one table */
    /* /PROCESS (default), /JOB, /GROUP, /SYSTEM — all map to global for now */
    int scope = DCL_SYM_GLOBAL;
    if (dcl_has_qualifier(cmd, "PROCESS")) scope = DCL_SYM_GLOBAL;

    dcl_sym_set(upper_name, equiv, scope);

    return SS$_NORMAL;
}

/*
 * DIFFERENCES - Compare two files and show differences.
 * Format: DIFFERENCES file1 file2
 * Implements a simple line-by-line diff with VMS-style output.
 */
static int cmd_differences(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing file specification(s)");
        return SS$_BADPARAM;
    }

    char path1[1024], path2[1024];
    dcl_resolve_path(ctx, cmd->params[0], path1, sizeof(path1));
    dcl_resolve_path(ctx, cmd->params[1], path2, sizeof(path2));

    FILE *f1 = fopen(path1, "r");
    if (!f1) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }
    FILE *f2 = fopen(path2, "r");
    if (!f2) {
        fclose(f1);
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[1]);
        return SS$_NOSUCHFILE;
    }

    /* VMS DIFFERENCES header */
    char vms1[256], vms2[256];
    const char *b1 = strrchr(path1, '/'); if (b1) b1++; else b1 = path1;
    const char *b2 = strrchr(path2, '/'); if (b2) b2++; else b2 = path2;
    size_t ni;
    for (ni = 0; b1[ni] && ni < sizeof(vms1)-1; ni++)
        vms1[ni] = (char)toupper((unsigned char)b1[ni]);
    vms1[ni] = '\0';
    for (ni = 0; b2[ni] && ni < sizeof(vms2)-1; ni++)
        vms2[ni] = (char)toupper((unsigned char)b2[ni]);
    vms2[ni] = '\0';

    printf("\n");
    printf("*************************\n");
    printf("File SYS$DISK:[]%s;1\n", vms1);
    printf("File SYS$DISK:[]%s;1\n", vms2);
    printf("*************************\n\n");

    char line1[4096], line2[4096];
    int lineno = 0;
    int diffs = 0;

    while (1) {
        char *r1 = fgets(line1, sizeof(line1), f1);
        char *r2 = fgets(line2, sizeof(line2), f2);
        lineno++;

        if (!r1 && !r2) break;

        /* Remove trailing newlines for comparison */
        if (r1) {
            size_t l = strlen(line1);
            if (l > 0 && line1[l-1] == '\n') line1[l-1] = '\0';
        }
        if (r2) {
            size_t l = strlen(line2);
            if (l > 0 && line2[l-1] == '\n') line2[l-1] = '\0';
        }

        if (!r1 || !r2 || strcmp(line1, line2) != 0) {
            diffs++;
            printf("***\n");
            if (r1) printf("  (%d) %s\n", lineno, line1);
            else    printf("  (%d) <end of file>\n", lineno);
            printf("***\n");
            if (r2) printf("  (%d) %s\n", lineno, line2);
            else    printf("  (%d) <end of file>\n", lineno);
            printf("\n");

            if (!r1 || !r2) break;
        }
    }

    fclose(f1);
    fclose(f2);

    if (diffs == 0) {
        printf("Number of difference sections found: 0\n\n");
        printf("%%DIFF-I-IDENT, files are identical\n");
    } else {
        printf("Number of difference sections found: %d\n", diffs);
    }

    return SS$_NORMAL;
}

/*
 * SORT - Sort a file.
 * Format: SORT input-file output-file
 * Reads the input file line by line, sorts, writes to output.
 */
static int cmd_sort(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing input and/or output file specification");
        return SS$_BADPARAM;
    }

    char src_path[1024], dst_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], src_path, sizeof(src_path));
    dcl_resolve_path(ctx, cmd->params[1], dst_path, sizeof(dst_path));

    /* Read all lines */
    FILE *fp = fopen(src_path, "r");
    if (!fp) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* Collect lines into dynamic array */
    char **lines = NULL;
    size_t line_count = 0;
    size_t line_cap = 0;
    char buf[4096];

    while (fgets(buf, sizeof(buf), fp)) {
        if (line_count >= line_cap) {
            size_t new_cap = (line_cap == 0) ? 64 : line_cap * 2;
            char **new_lines = realloc(lines, new_cap * sizeof(char *));
            if (!new_lines) {
                dcl_error("DCL", 4, "INSFMEM", "insufficient memory for sort");
                fclose(fp);
                for (size_t i = 0; i < line_count; i++) free(lines[i]);
                free(lines);
                return SS$_INSFMEM;
            }
            lines = new_lines;
            line_cap = new_cap;
        }
        lines[line_count] = strdup(buf);
        if (!lines[line_count]) {
            dcl_error("DCL", 4, "INSFMEM", "insufficient memory for sort");
            fclose(fp);
            for (size_t i = 0; i < line_count; i++) free(lines[i]);
            free(lines);
            return SS$_INSFMEM;
        }
        line_count++;
    }
    fclose(fp);

    /* Sort: /REVERSE reverses, default ascending case-insensitive */
    int reverse = dcl_has_qualifier(cmd, "REVERSE");

    /* Simple qsort with strcmp (case-insensitive) */
    /* Use a comparison that respects /REVERSE */
    /* We need a static/global for qsort comparator — use a function */
    /* Since we can't pass extra args to qsort comparator, implement inline */
    if (!reverse) {
        /* Ascending */
        for (size_t i = 0; i < line_count - 1; i++) {
            for (size_t j = i + 1; j < line_count; j++) {
                if (strcasecmp(lines[i], lines[j]) > 0) {
                    char *tmp = lines[i];
                    lines[i] = lines[j];
                    lines[j] = tmp;
                }
            }
        }
    } else {
        /* Descending */
        for (size_t i = 0; i < line_count - 1; i++) {
            for (size_t j = i + 1; j < line_count; j++) {
                if (strcasecmp(lines[i], lines[j]) < 0) {
                    char *tmp = lines[i];
                    lines[i] = lines[j];
                    lines[j] = tmp;
                }
            }
        }
    }

    /* Write sorted output */
    FILE *out = fopen(dst_path, "w");
    if (!out) {
        dcl_error("RMS", 2, "CRE", "cannot create - %s", cmd->params[1]);
        for (size_t i = 0; i < line_count; i++) free(lines[i]);
        free(lines);
        return SS$_FILACCERR;
    }

    for (size_t i = 0; i < line_count; i++) {
        fputs(lines[i], out);
        free(lines[i]);
    }
    free(lines);
    fclose(out);

    return SS$_NORMAL;
}

/*
 * SUBMIT - Submit a command procedure for batch execution (stub).
 * Format: SUBMIT filespec
 * VMS SUBMIT queues a command procedure to a batch queue.
 * OVMX stub: acknowledges the command but executes synchronously.
 */
static int cmd_submit(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    /* Check file exists */
    struct stat st;
    if (stat(linux_path, &st) != 0) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* VMS output: Job <name> (queue SYS$BATCH, entry <n>) started on queue SYS$BATCH */
    const char *bn = strrchr(cmd->params[0], ']');
    if (!bn) bn = strrchr(cmd->params[0], ':');
    if (bn) bn++; else bn = cmd->params[0];

    char upper_name[256];
    size_t i;
    for (i = 0; bn[i] && bn[i] != '.' && bn[i] != ';' && i < sizeof(upper_name)-1; i++)
        upper_name[i] = (char)toupper((unsigned char)bn[i]);
    upper_name[i] = '\0';

    /* Simulate a job number */
    static int job_entry = 100;
    job_entry++;

    printf("Job %s (queue SYS$BATCH, entry %d) started on queue SYS$BATCH\n",
           upper_name, job_entry);

    /* Stub: in a real implementation, this would queue the job.
     * For OVMX, we silently succeed — batch queuing not implemented. */

    return SS$_NORMAL;
}

/*
 * PRINT - Queue a file for printing (stub).
 * Format: PRINT filespec[,...] [/QUEUE=queue-name] [/COPIES=n]
 * VMS PRINT sends files to the print queue.
 */
static int cmd_print(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    struct stat st;
    if (stat(linux_path, &st) != 0) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* Get queue name (/QUEUE=name, default SYS$PRINT) */
    const char *queue_name = dcl_qualifier_value(cmd, "QUEUE");
    if (!queue_name || !queue_name[0]) queue_name = "SYS$PRINT";

    /* Get copy count */
    const char *copies_str = dcl_qualifier_value(cmd, "COPIES");
    int copies = 1;
    if (copies_str && copies_str[0]) copies = atoi(copies_str);
    if (copies < 1) copies = 1;

    /* Format filename for display */
    const char *bn = strrchr(cmd->params[0], ']');
    if (!bn) bn = strrchr(cmd->params[0], ':');
    if (bn) bn++; else bn = cmd->params[0];

    char upper_name[256];
    size_t i;
    for (i = 0; bn[i] && bn[i] != ';' && i < sizeof(upper_name)-1; i++)
        upper_name[i] = (char)toupper((unsigned char)bn[i]);
    upper_name[i] = '\0';

    static int print_entry = 200;
    print_entry++;

    printf("Job %s (queue %s, entry %d) pending\n",
           upper_name, queue_name, print_entry);

    /* Stub: no actual printing implemented */
    (void)copies;

    return SS$_NORMAL;
}

/*
 * DUMP - Hex dump of a file (or device/virtual memory in VMS).
 * Format: DUMP filespec [/BLOCKS=n] [/RECORDS]
 * OVMX: implements hex+ASCII dump of a file.
 */
static int cmd_dump(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    FILE *fp = fopen(linux_path, "rb");
    if (!fp) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* /BLOCKS=n — limit to n 512-byte blocks (0 = all) */
    long max_blocks = 0;
    const char *blk_str = dcl_qualifier_value(cmd, "BLOCKS");
    if (blk_str && blk_str[0]) max_blocks = strtol(blk_str, NULL, 10);

    /* VMS DUMP header */
    char vms_name[256];
    const char *bn = strrchr(linux_path, '/');
    if (bn) bn++; else bn = linux_path;
    size_t i;
    for (i = 0; bn[i] && i < sizeof(vms_name)-1; i++)
        vms_name[i] = (char)toupper((unsigned char)bn[i]);
    vms_name[i] = '\0';

    printf("\nDump of file SYS$DISK:[]%s;1\n\n", vms_name);
    printf("File ID (0,0,0)  End of file block 0  Offset 0\n\n");

    unsigned char buf[16];
    long offset = 0;
    size_t n;
    long block = 0;
    int in_block_start = 1;

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        /* Print block header at start of each 512-byte block */
        if (in_block_start || (offset % 512 == 0)) {
            if (offset > 0 && offset % 512 == 0) {
                block++;
                if (max_blocks > 0 && block >= max_blocks) break;
                printf("\nVirtual block number %ld (00000%02lX), 512 (0200) bytes\n\n",
                       block + 1, block * 512);
            } else if (in_block_start) {
                printf("Virtual block number 1 (00000000), 512 (0200) bytes\n\n");
                in_block_start = 0;
            }
        }

        /* Print offset (VMS style: relative to start of current block) */
        long block_offset = offset % 512;
        printf("%08lX ", block_offset);

        /* Hex bytes (4 groups of 4, space between) */
        for (size_t j = 0; j < 16; j++) {
            if (j > 0 && j % 4 == 0) printf(" ");
            if (j < n)
                printf("%02X", buf[j]);
            else
                printf("  ");
        }

        printf("  ");

        /* ASCII */
        for (size_t j = 0; j < n; j++) {
            printf("%c", isprint(buf[j]) ? buf[j] : '.');
        }
        printf("\n");

        offset += (long)n;
    }

    fclose(fp);
    printf("\n");

    return SS$_NORMAL;
}

/* ================================================================== */
/*                    Logical Name Operations                          */
/* ================================================================== */

/*
 * DEFINE - Define a logical name.
 *
 * Qualifiers:
 *   /PROCESS   (default) — create in LNM$PROCESS_TABLE
 *   /JOB                 — create in LNM$JOB
 *   /GROUP               — create in LNM$GROUP
 *   /SYSTEM              — create in LNM$SYSTEM
 *
 * If the LNM manager is not available, fall back to storing as a
 * global DCL symbol so callers don't lose the value entirely.
 */
static int cmd_define(struct dcl_command *cmd)
{
    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing logical name and/or equivalence string");
        return SS$_BADPARAM;
    }

    const char *logname = cmd->params[0];
    const char *equiv   = cmd->params[1];

    /* Uppercase the logical name */
    char upper_name[256];
    size_t i;
    for (i = 0; i < sizeof(upper_name) - 1 && logname[i]; i++)
        upper_name[i] = (char)toupper((unsigned char)logname[i]);
    upper_name[i] = '\0';

    /* Determine target table from qualifiers (/PROCESS is the default) */
    const char *table = LNM_PROCESS_TABLE;
    if (dcl_has_qualifier(cmd, "SYSTEM"))
        table = LNM_SYSTEM_TABLE;
    else if (dcl_has_qualifier(cmd, "GROUP"))
        table = LNM_GROUP_TABLE;
    else if (dcl_has_qualifier(cmd, "JOB"))
        table = LNM_JOB_TABLE;

    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        uint32_t status = lnm_create(mgr, table, upper_name, equiv,
                                     LNM_ATTR_TERMINAL, LNM_MODE_USER);
        if (status != SS$_NORMAL && status != SS$_SUPERSEDE) {
            dcl_error("DCL", 2, "LNMFAIL",
                      "failed to create logical name \\%s\\", upper_name);
            return (int)status;
        }
    } else {
        /* Graceful fallback: store as global symbol */
        dcl_sym_set(upper_name, equiv, DCL_SYM_GLOBAL);
    }

    return SS$_NORMAL;
}

/*
 * DEASSIGN - Remove a logical name.
 *
 * Qualifiers:
 *   /PROCESS   (default) — delete from LNM$PROCESS_TABLE
 *   /JOB                 — delete from LNM$JOB
 *   /GROUP               — delete from LNM$GROUP
 *   /SYSTEM              — delete from LNM$SYSTEM
 *   /ALL                 — delete from all tables (searches in order)
 *
 * If the LNM manager is not available, attempt to remove from the
 * global symbol table as a fallback.
 */
static int cmd_deassign(struct dcl_command *cmd)
{
    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOKEYW", "missing logical name");
        return SS$_BADPARAM;
    }

    /* Uppercase the logical name */
    char upper_name[256];
    size_t i;
    for (i = 0; i < sizeof(upper_name) - 1 && cmd->params[0][i]; i++)
        upper_name[i] = (char)toupper((unsigned char)cmd->params[0][i]);
    upper_name[i] = '\0';

    /* Determine target table from qualifiers (/PROCESS is the default) */
    const char *table = LNM_PROCESS_TABLE;
    int all_tables = dcl_has_qualifier(cmd, "ALL");
    if (!all_tables) {
        if (dcl_has_qualifier(cmd, "SYSTEM"))
            table = LNM_SYSTEM_TABLE;
        else if (dcl_has_qualifier(cmd, "GROUP"))
            table = LNM_GROUP_TABLE;
        else if (dcl_has_qualifier(cmd, "JOB"))
            table = LNM_JOB_TABLE;
    }

    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        if (all_tables) {
            /* Try all tables; ignore "not found" errors */
            lnm_delete(mgr, LNM_PROCESS_TABLE, upper_name, LNM_MODE_USER);
            lnm_delete(mgr, LNM_JOB_TABLE,     upper_name, LNM_MODE_USER);
            lnm_delete(mgr, LNM_GROUP_TABLE,   upper_name, LNM_MODE_USER);
            lnm_delete(mgr, LNM_SYSTEM_TABLE,  upper_name, LNM_MODE_USER);
        } else {
            uint32_t status = lnm_delete(mgr, table, upper_name, LNM_MODE_USER);
            if (status == SS$_NOLOGNAM) {
                /* Not an error on VMS — deassigning a non-existent name is silent */
                return SS$_NORMAL;
            }
            if (status != SS$_NORMAL) {
                dcl_error("DCL", 1, "NOLOGNAM",
                          "no logical name match for \\%s\\", upper_name);
                return (int)status;
            }
        }
    } else {
        /* Graceful fallback: remove from global symbol table */
        dcl_sym_delete(upper_name, DCL_SYM_GLOBAL);
    }

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
 * MAIL - Launch VMS MAIL utility.
 */
static int cmd_mail(struct dcl_command *cmd)
{
    /* Build argv for exec: vms_mail [qualifiers/args] */
    char mail_path[PATH_MAX];

#ifndef OVMX_BIN_DIR
#define OVMX_BIN_DIR "/usr/local/bin"
#endif
    snprintf(mail_path, sizeof(mail_path), "%s/vms_mail", OVMX_BIN_DIR);

    /* Collect qualifiers and params into an argv */
    char *exec_argv[64];
    int exec_argc = 0;
    exec_argv[exec_argc++] = mail_path;

    /* Pass through qualifiers first */
    for (int i = 0; i < cmd->qualifier_count && exec_argc < 62; i++) {
        exec_argv[exec_argc++] = cmd->qualifiers[i].name;
    }

    /* Pass through params */
    for (int i = 0; i < cmd->param_count && exec_argc < 62; i++) {
        if (cmd->params[i][0] != '\0')
            exec_argv[exec_argc++] = cmd->params[i];
    }
    exec_argv[exec_argc] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        execv(mail_path, exec_argv);
        /* If exec fails, try just "vms_mail" on PATH */
        execvp("vms_mail", exec_argv);
        _exit(1);
    } else if (pid > 0) {
        int wstatus;
        waitpid(pid, &wstatus, 0);
        if (WIFEXITED(wstatus))
            return (WEXITSTATUS(wstatus) == 0) ? SS$_NORMAL : SS$_ABORT;
    } else {
        dcl_error("DCL", 4, "CREPRC", "cannot create process for MAIL");
        return SS$_ABORT;
    }
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
/*                        MONITOR Command                              */
/* ================================================================== */

/*
 * MONITOR - Real-time system activity display.
 * Executes the vms_monitor binary with the subcommand argument.
 */
static int cmd_monitor(struct dcl_command *cmd)
{
    /* Locate vms_monitor binary: try install path first, then build path */
    static const char *candidates[] = {
        "/usr/local/bin/vms_monitor",
        "/usr/bin/vms_monitor",
        NULL
    };

    const char *monitor_bin = NULL;
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], X_OK) == 0) {
            monitor_bin = candidates[i];
            break;
        }
    }

    if (!monitor_bin) {
        /* Fall back to PATH-based search */
        monitor_bin = "vms_monitor";
    }

    /* Build argv for exec: vms_monitor [subcommand] */
    const char *subcommand = (cmd->param_count >= 1 && cmd->params[0][0] != '\0')
                             ? cmd->params[0] : "SYSTEM";

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: exec vms_monitor with the subcommand */
        execlp(monitor_bin, monitor_bin, subcommand, (char *)NULL);
        /* If execlp fails, try absolute candidate paths */
        for (int i = 0; candidates[i]; i++) {
            execl(candidates[i], candidates[i], subcommand, (char *)NULL);
        }
        fprintf(stderr, "%%MONITOR-F-NOIMG, cannot execute vms_monitor\n");
        _exit(1);
    } else if (pid > 0) {
        int wstatus;
        waitpid(pid, &wstatus, 0);
        if (WIFEXITED(wstatus)) {
            return (WEXITSTATUS(wstatus) == 0) ? SS$_NORMAL : SS$_ABORT;
        }
    } else {
        dcl_error("DCL", 4, "CREPRC", "cannot create process for MONITOR");
        return SS$_ABORT;
    }

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
/*                     RECALL Command                                  */
/* ================================================================== */

/*
 * RECALL - Show or re-execute commands from history.
 *
 * RECALL          — show last command
 * RECALL /ALL     — numbered history list
 * RECALL n        — re-execute command number n
 * RECALL string   — find and re-execute most recent match
 */
static int cmd_recall(struct dcl_command *cmd)
{
#ifndef HAVE_READLINE
    (void)cmd;
    printf("%%DCL-W-RECALL, command recall requires readline support\n");
    return SS$_NORMAL;
#else
    int show_all = dcl_has_qualifier(cmd, "ALL");

    if (show_all) {
        /* RECALL /ALL — print numbered history */
        HIST_ENTRY **hist = history_list();
        if (!hist || !hist[0]) {
            printf("%%DCL-I-RECALL, no history available\n");
            return SS$_NORMAL;
        }
        int count = 0;
        while (hist[count]) count++;
        for (int i = 0; i < count; i++) {
            /* history_list() is 0-indexed; history_get() uses offset_history */
            printf("%5d  %s\n", history_base + i, hist[i]->line);
        }
        return SS$_NORMAL;
    }

    if (cmd->param_count == 0) {
        /* RECALL with no args — show most recent command */
        HIST_ENTRY *entry = current_history();
        /* Go to the most recent history entry */
        while (next_history() != NULL) { /* advance to end */ }
        entry = previous_history();
        if (!entry) {
            printf("%%DCL-I-RECALL, no history available\n");
            return SS$_NORMAL;
        }
        printf("%s\n", entry->line);
        return SS$_NORMAL;
    }

    /* Parameter given — check if it's a number */
    const char *param = cmd->params[0];
    int is_number = 1;
    for (size_t i = 0; param[i]; i++) {
        if (!isdigit((unsigned char)param[i])) { is_number = 0; break; }
    }

    if (is_number) {
        /* RECALL n — re-execute command number n */
        int n = atoi(param);
        HIST_ENTRY *entry = history_get(n);
        if (!entry) {
            printf("%%DCL-W-RECALL, no command number %d in history\n", n);
            return SS$_NORMAL;
        }
        printf("%s\n", entry->line);
        /* Feed the command back to the executor */
        return dcl_execute_line(entry->line);
    } else {
        /* RECALL string — find most recent command starting with string */
        HIST_ENTRY **hist = history_list();
        if (!hist) {
            printf("%%DCL-I-RECALL, no history available\n");
            return SS$_NORMAL;
        }
        int count = 0;
        while (hist[count]) count++;
        size_t plen = strlen(param);
        for (int i = count - 1; i >= 0; i--) {
            if (strncasecmp(hist[i]->line, param, plen) == 0) {
                printf("%s\n", hist[i]->line);
                return dcl_execute_line(hist[i]->line);
            }
        }
        printf("%%DCL-W-RECALL, no command matching \"%s\" in history\n", param);
        return SS$_NORMAL;
    }
#endif
}

/* ================================================================== */
/*                     Command Table                                   */
/* ================================================================== */

static struct dcl_verb builtin_verbs[] = {
    { "ACCOUNTING",  cmd_accounting,  CDU_F_ABBREV | CDU_F_QUALIFIER, 4,
      "Display login accounting information for the current user" },
    { "APPEND",      cmd_append,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Append source file to destination file" },
    { "ASSIGN",      cmd_assign,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Assign a logical name (equivalence name to logical name)" },
    { "CLOSE",       cmd_close,       CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Close a file that was opened for I/O" },
    { "COPY",        cmd_copy,        CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Copy a file" },
    { "CREATE",      cmd_create,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Create a new file (or directory with /DIRECTORY)" },
    { "DEASSIGN",    cmd_deassign,    CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Deassign (remove) a logical name" },
    { "DEFINE",      cmd_define,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Create a logical name definition" },
    { "DELETE",      cmd_delete,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Delete a file (or symbol with /SYMBOL)" },
    { "DIFFERENCES", cmd_differences, CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Compare two files and display differences" },
    { "DIRECTORY",   cmd_directory,   CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "List files in a directory" },
    { "DUMP",        cmd_dump,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display contents of a file in hexadecimal and ASCII" },
    { "EXIT",        cmd_exit,        CDU_F_ABBREV, 2,
      "Terminate a command procedure or session" },
    { "HELP",        cmd_help,        CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Obtain information about DCL commands" },
    { "INQUIRE",     cmd_inquire,     CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Read input from SYS$INPUT and assign to a symbol" },
    { "LOGOUT",      cmd_logout,      CDU_F_ABBREV, 2,
      "Terminate an interactive session" },
    { "MAIL",      cmd_mail,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Send and receive electronic mail messages" },
    { "MONITOR",   cmd_monitor,   CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Display real-time system activity statistics" },
    { "OPEN",        cmd_open,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Open a file for reading or writing" },
    { "PIPE",        cmd_pipe,        CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Execute a command using system shell (piping/redirection)" },
    { "PRINT",       cmd_print,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Queue a file for printing" },
    { "PURGE",       cmd_purge,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Delete old versions of a file" },
    { "RECALL",    cmd_recall,    CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Show or re-execute commands from command history" },
    { "READ",        cmd_read,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Read a record from a file into a symbol" },
    { "RENAME",      cmd_rename,      CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Change the name and/or location of a file" },
    { "REPLY",       cmd_reply,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Send an operator reply or enable/disable operator terminal" },
    { "REQUEST",     cmd_request,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Send a request message to the operator" },
    { "RUN",         cmd_run,         CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Execute a program image" },
    { "SEARCH",      cmd_search,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Search a file for a text string" },
    { "SET",         cmd_set,         CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Set or modify system, process, or file characteristics" },
    { "SHOW",        cmd_show,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display information about the system, process, or files" },
    { "SORT",        cmd_sort,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Sort records in a file" },
    { "SPAWN",       cmd_spawn,       CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Create a subprocess" },
    { "STOP",        cmd_stop,        CDU_F_ABBREV, 2,
      "Stop the current process" },
    { "SUBMIT",      cmd_submit,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Submit a command procedure to a batch queue" },
    { "TYPE",        cmd_type,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display the contents of a file" },
    { "WAIT",        cmd_wait,        CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Wait for a specified time interval" },
    { "WRITE",       cmd_write,       CDU_F_ABBREV | CDU_F_PARAM, 2,
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
