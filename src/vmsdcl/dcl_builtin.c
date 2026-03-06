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
#include <limits.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <termios.h>
#include <mntent.h>

#include "dcl/context.h"
#include "dcl/terminal.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/cdu.h"
#include "ssdef.h"
#include "vms/logical.h"
#include "vms/privs.h"
#include "opcdef.h"
#include "ovmx_accounting.h"
#include "starlet.h"
#include "vmsfs/filespec.h"
#include "dcl/vms_messages.h"
#include "vms/pcb.h"
#include "vmsqueue.h"

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

/* External functions */
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);
extern int dcl_resolve_path(struct dcl_context *ctx, const char *spec,
                            char *linux_path, size_t path_size);

/* BACKUP command (dcl_backup.c) */
extern int cmd_backup(struct dcl_command *cmd);

/* LIBRARY command (dcl_library.c) */
extern int cmd_library(struct dcl_command *cmd);

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
/*                     VMS Device Table                                */
/* ================================================================== */

#define VMS_MAX_DEVICES 64

struct vms_device {
    char vms_name[16];      /* DUA0:, DKA0:, DNA0: */
    char linux_path[256];   /* /dev/sda1, /mnt/data, etc. */
    char volume_label[16];  /* Volume label */
    int  mounted;           /* 1 = mounted, 0 = not mounted */
};

static struct vms_device vms_device_table[VMS_MAX_DEVICES];
static int vms_device_count = 0;

/*
 * Find a device in the table by VMS name (case-insensitive).
 * The name may or may not include trailing colon.
 */
static struct vms_device *vms_find_device(const char *name)
{
    char upper[16];
    size_t len = strlen(name);
    if (len >= sizeof(upper)) len = sizeof(upper) - 1;
    for (size_t i = 0; i < len; i++)
        upper[i] = (char)toupper((unsigned char)name[i]);
    upper[len] = '\0';
    /* Strip trailing colon for comparison */
    if (len > 0 && upper[len - 1] == ':')
        upper[--len] = '\0';

    for (int i = 0; i < vms_device_count; i++) {
        char dev[16];
        strncpy(dev, vms_device_table[i].vms_name, sizeof(dev) - 1);
        dev[sizeof(dev) - 1] = '\0';
        size_t dlen = strlen(dev);
        if (dlen > 0 && dev[dlen - 1] == ':')
            dev[--dlen] = '\0';
        if (strcasecmp(upper, dev) == 0)
            return &vms_device_table[i];
    }
    return NULL;
}

/* ================================================================== */
/*                          SHOW Commands                              */
/* ================================================================== */

/* Forward declarations for helper functions used by cmd_show_process */
static int cmd_show_process_privileges(struct dcl_context *ctx);
static int cmd_show_process_quotas(struct dcl_context *ctx);

/* Forward declarations for queue commands (defined after PRINT/SUBMIT) */
static int ensure_queue_init(void);
static int cmd_show_queue(struct dcl_command *cmd);
static int cmd_show_entry(struct dcl_command *cmd);
static int cmd_set_entry(struct dcl_command *cmd);
static int cmd_set_queue(struct dcl_command *cmd);
static int cmd_show_intrusion(struct dcl_command *cmd);

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

    printf("  %s\n", ctx->default_dir);

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
 *
 * Shows only VMS-managed processes from the PCB table.
 * No Linux process names (kworker, systemd, etc.) are exposed.
 */
static int cmd_show_system(struct dcl_command *cmd)
{
    (void)cmd;

    struct dcl_context *ctx = dcl_get_context();

    struct utsname uts;
    uname(&uts);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    /* Calculate uptime from /proc/uptime if available, else show --- */
    char uptime_str[32];
    {
        FILE *uf = fopen("/proc/uptime", "r");
        if (uf) {
            double up_secs = 0;
            if (fscanf(uf, "%lf", &up_secs) == 1 && up_secs > 0) {
                unsigned long up = (unsigned long)up_secs;
                unsigned long days = up / 86400;
                unsigned long hrs  = (up % 86400) / 3600;
                unsigned long mins = (up % 3600) / 60;
                unsigned long secs = up % 60;
                snprintf(uptime_str, sizeof(uptime_str),
                         "%lu %02lu:%02lu:%02lu", days, hrs, mins, secs);
            } else {
                snprintf(uptime_str, sizeof(uptime_str), "---");
            }
            fclose(uf);
        } else {
            snprintf(uptime_str, sizeof(uptime_str), "---");
        }
    }

    printf("OpenVMS V7.3  on node %s  %2d-%s-%04d %02d:%02d:%02d.%02d"
           "  Uptime  %s\n",
           uts.nodename, tm.tm_mday, vms_months[tm.tm_mon],
           1900 + tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000), uptime_str);
    printf("  Pid    Process Name    State  Pri      I/O       CPU"
           "       Page flts  Pages\n");

    /* Show VMS processes from PCB table only — no /proc scanning */
    struct vms_pcb *pcb = vms_pcb_get();

    if (pcb && pcb->vms_pid != 0) {
        /* Use PCB identity */
        const char *pname = pcb->prcnam[0] ? pcb->prcnam : "OVMX";
        char cpu_str[32] = "0 00:00:00.00";
        read_proc_cpu((int)getpid(), cpu_str, sizeof(cpu_str));
        printf(" %08X %-15s %s %3d %9d  %s  %9d  %5d\n",
               pcb->vms_pid, pname, "CUR", 4, 0, cpu_str, 0, 340);
    } else {
        /* PCB not initialized — fabricate current process entry */
        const char *pname = (ctx && ctx->process_name[0])
                            ? ctx->process_name : "OVMX";
        uint32_t vpid = (uint32_t)getpid();
        char cpu_str[32] = "0 00:00:00.00";
        read_proc_cpu((int)getpid(), cpu_str, sizeof(cpu_str));
        printf(" %08X %-15s %s %3d %9d  %s  %9d  %5d\n",
               vpid, pname, "CUR", 4, 0, cpu_str, 0, 340);
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
    printf("Default file spec: %s\n", ctx->default_dir);

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
 * SHOW USERS - Show logged-in users from the terminal device table.
 *
 * Output matches OpenVMS format:
 *   Username     Process Name      PID        Terminal
 */
static int cmd_show_users(struct dcl_command *cmd)
{
    (void)cmd;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    printf("      OpenVMS User Processes at %2d-%s-%04d %02d:%02d:%02d.%02d\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 10000000));

    struct terminal_device devs[100];
    int count = 0;
    vms_term_list(devs, 100, &count);

    if (count == 0) {
        /* No entries in device table — show at least the current user */
        struct dcl_context *ctx = dcl_get_context();
        char upper_name[64];
        size_t i;
        const char *src = ctx->username[0] ? ctx->username : "SYSTEM";
        for (i = 0; i < sizeof(upper_name) - 1 && src[i]; i++)
            upper_name[i] = (char)toupper((unsigned char)src[i]);
        upper_name[i] = '\0';

        printf("    Total number of users = 1, number of processes = 1\n\n");
        printf("      Username     Process Name      PID        Terminal\n");
        printf("      %-12s %-16s  %08X   %s\n",
               upper_name, ctx->process_name[0] ? ctx->process_name : upper_name,
               (unsigned)getpid(),
               ctx->terminal.device_name[0] ? ctx->terminal.device_name : "_FTA0:");
    } else {
        printf("    Total number of users = %d, number of processes = %d\n\n",
               count, count);
        printf("      Username     Process Name      PID        Terminal\n");

        for (int j = 0; j < count; j++) {
            char upper_name[64];
            size_t i;
            for (i = 0; i < sizeof(upper_name) - 1 && devs[j].owner_name[i]; i++)
                upper_name[i] = (char)toupper((unsigned char)devs[j].owner_name[i]);
            upper_name[i] = '\0';

            printf("      %-12s %-16s  %08X   %s\n",
                   upper_name, upper_name,
                   (unsigned)devs[j].owner_pid, devs[j].name);
        }
    }

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


    /* Show user-mounted devices from the device table */
    for (int i = 0; i < vms_device_count; i++) {
        const char *status = vms_device_table[i].mounted ? "Mounted" : "Dismounted";
        printf("%-24s %-14s       0  %-14s%9d     1   1\n",
               vms_device_table[i].vms_name, status,
               vms_device_table[i].volume_label, 0);
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
 *
 * Dynamically displays actual terminal state from the
 * vms_terminal characteristics model.
 */
static int cmd_show_terminal(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();

    /* Ensure owner is current */
    if (ctx->username[0] && !ctx->terminal.owner[0]) {
        strncpy(ctx->terminal.owner, ctx->username,
                sizeof(ctx->terminal.owner) - 1);
    }

    vms_terminal_show(&ctx->terminal, stdout);
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

/*
 * SHOW LICENSE - Display active product licenses.
 */
static int cmd_show_license(struct dcl_command *cmd)
{
    (void)cmd;
    printf("Active licenses on this node:\n\n");
    printf("------- Product ID --------    ---- Rating -----   -- Version --\n");
    printf("Product Name          Producer  Units  Avail  Actv  Version  Termination\n");
    printf("OVMX                  OVMX      0      0      100   V1.0     (none)\n");
    printf("OVMX-TCP/IP           OVMX      0      0      100   V0.1     (none)\n");
    return SS$_NORMAL;
}

/*
 * SHOW CLUSTER - Display VMScluster membership.
 */
static int cmd_show_cluster(struct dcl_command *cmd)
{
    (void)cmd;
    printf("%%SYSTEM-I-NOTMEMBER, this system is not a member of a VMScluster\n");
    return SS$_NORMAL;
}

/*
 * SHOW NETWORK - Display DECnet/TCP network configuration.
 */
static int cmd_show_network(struct dcl_command *cmd)
{
    (void)cmd;
    printf("Product: OVMX TCP/IP Services for OpenVMS V0.1\n");
    printf("Node: OVMX\n");
    return SS$_NORMAL;
}

/*
 * SHOW ERROR - Display device error summary.
 */
static int cmd_show_error(struct dcl_command *cmd)
{
    (void)cmd;
    printf("\n         Device Error Count Summary\n");
    printf("         Device   Error Count\n");
    printf("         ------   -----------\n");
    printf("No errors logged.\n");
    return SS$_NORMAL;
}

/*
 * SHOW WORKING_SET - Display working set quotas.
 */
static int cmd_show_working_set(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();
    int quota = ctx->ws_quota > 0 ? ctx->ws_quota : 8192;
    int extent = quota * 2;
    printf("  Working Set  [current,quota,extent] = [%d,%d,%d]\n",
           quota, quota, extent);
    printf("  Adjustment enabled  Authorized Quota = %d  Authorized Extent = %d\n",
           quota, extent);
    return SS$_NORMAL;
}

/*
 * SHOW ACCOUNTING - Display accounting status.
 */
static int cmd_show_accounting(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();
    if (ctx->accounting_enabled) {
        printf("Accounting is currently enabled.\n");
    } else {
        printf("Accounting is currently disabled.\n");
    }
    printf("Accounting file: SYS$MANAGER:ACCOUNTNG.DAT\n");
    return SS$_NORMAL;
}

/*
 * SHOW AUDIT - Display security auditing status.
 */
static int cmd_show_audit(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();
    if (ctx->audit_enabled) {
        printf("System security auditing is currently enabled.\n");
    } else {
        printf("System security auditing is currently disabled.\n");
    }
    printf("Audit log file: SYS$MANAGER:AUDIT.LOG\n");
    return SS$_NORMAL;
}

/*
 * SHOW QUOTA - Display disk quota for current user.
 */
static int cmd_show_quota(struct dcl_command *cmd)
{
    (void)cmd;
    printf("  User [200,1] has 0 blocks used, 0 available\n");
    return SS$_NORMAL;
}

/*
 * SHOW ROOT - Display system root directory.
 */
static int cmd_show_root(struct dcl_command *cmd)
{
    (void)cmd;
    printf("  System root is SYS$SYSDEVICE:[SYS0.SYSCOMMON.]\n");
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
    if (dcl_match_command(subcmd, "LICENSE", 3))
        return cmd_show_license(cmd);
    if (dcl_match_command(subcmd, "CLUSTER", 3))
        return cmd_show_cluster(cmd);
    if (dcl_match_command(subcmd, "NETWORK", 3))
        return cmd_show_network(cmd);
    if (dcl_match_command(subcmd, "ERROR", 3))
        return cmd_show_error(cmd);
    if (dcl_match_command(subcmd, "WORKING_SET", 4))
        return cmd_show_working_set(cmd);
    if (dcl_match_command(subcmd, "ACCOUNTING", 3))
        return cmd_show_accounting(cmd);
    if (dcl_match_command(subcmd, "AUDIT", 3))
        return cmd_show_audit(cmd);
    if (dcl_match_command(subcmd, "QUOTA", 3))
        return cmd_show_quota(cmd);
    if (dcl_match_command(subcmd, "ROOT", 3))
        return cmd_show_root(cmd);
    if (dcl_match_command(subcmd, "QUEUE", 3))
        return cmd_show_queue(cmd);
    if (dcl_match_command(subcmd, "ENTRY", 3))
        return cmd_show_entry(cmd);
    if (dcl_match_command(subcmd, "INTRUSION", 3))
        return cmd_show_intrusion(cmd);

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

    /* Remove trailing slash for stat */
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

    /* Store the VMS dirspec directly — don't round-trip through Linux.
     * If the spec has a device/logical, use as-is.
     * If relative ([DIR]), prepend the current default's device. */
    if (strchr(dirspec, ':')) {
        /* Full spec with device/logical — store directly */
        strncpy(ctx->default_dir, dirspec, sizeof(ctx->default_dir) - 1);
        ctx->default_dir[sizeof(ctx->default_dir) - 1] = '\0';
    } else if (dirspec[0] == '[') {
        /* Relative spec — prepend current device */
        char device[128] = "";
        const char *colon = strchr(ctx->default_dir, ':');
        if (colon) {
            size_t dlen = (size_t)(colon - ctx->default_dir);
            if (dlen < sizeof(device)) {
                memcpy(device, ctx->default_dir, dlen);
                device[dlen] = '\0';
            }
        }
        if (device[0])
            snprintf(ctx->default_dir, sizeof(ctx->default_dir),
                     "%s:%s", device, dirspec);
        else
            strncpy(ctx->default_dir, dirspec, sizeof(ctx->default_dir) - 1);
    } else {
        /* Bare name — treat as logical or directory */
        strncpy(ctx->default_dir, dirspec, sizeof(ctx->default_dir) - 1);
        ctx->default_dir[sizeof(ctx->default_dir) - 1] = '\0';
    }

    /* Change the process working directory too */
    if (chdir(check_path) != 0) {
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
 * SET TERMINAL - Modify terminal characteristics.
 *
 * SET TERMINAL /WIDTH=n /PAGE=n /ECHO /NOECHO /WRAP /NOWRAP
 *              /INSERT /OVERSTRIKE /BROADCAST /NOBROADCAST
 *              /LINE_EDITING /NOLINE_EDITING /DEVICE_TYPE=type
 *              /HOSTSYNC /NOHOSTSYNC /TTSYNC /NOTTSYNC
 *              /TYPEAHEAD /NOTYPEAHEAD /TAB /NOTAB
 *              /SCOPE /NOSCOPE /LOWERCASE /UPPERCASE
 *              /HOLDSCREEN /NOHOLDSCREEN /EIGHTBIT /NOEIGHTBIT
 *              /READSYNC /NOREADSYNC /PASTHRU /NOPASTHRU
 *              /ESCAPE /NOESCAPE /FORM /NOFORM
 *              /FULLDUP /HALFDUP /MODEM /NOMODEM
 *              /PAGE_CHAR /NOPAGE_CHAR /SECURE /NOSECURE
 *              /FALLBACK /NOFALLBACK /SPEED=n /PARITY=type
 *
 * Stores settings in the vms_terminal model and applies those
 * that map to real termios / ioctl.
 */
static int cmd_set_terminal(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();
    struct vms_terminal *term = &ctx->terminal;
    int changed = 0;

    /* /WIDTH=n */
    const char *width_val = dcl_qualifier_value(cmd, "WIDTH");
    if (width_val && *width_val) {
        int w = atoi(width_val);
        if (w < 1 || w > 32767) {
            dcl_error("SET", 2, "INVWIDTH",
                      "invalid terminal width - \\%s\\", width_val);
            return SS$_BADPARAM;
        }
        term->width = w;
        changed = 1;
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
        term->page = p;
        changed = 1;
    }

    /* /SPEED=n */
    const char *speed_val = dcl_qualifier_value(cmd, "SPEED");
    if (speed_val && *speed_val) {
        int s = atoi(speed_val);
        if (s < 0) {
            dcl_error("SET", 2, "INVSPEED",
                      "invalid terminal speed - \\%s\\", speed_val);
            return SS$_BADPARAM;
        }
        term->speed = s;
        changed = 1;
    }

    /* /PARITY=type (NONE, EVEN, ODD) */
    const char *parity_val = dcl_qualifier_value(cmd, "PARITY");
    if (parity_val && *parity_val) {
        if (strncasecmp(parity_val, "NONE", 4) == 0)
            term->parity = 0;
        else if (strncasecmp(parity_val, "EVEN", 4) == 0)
            term->parity = 1;
        else if (strncasecmp(parity_val, "ODD", 3) == 0)
            term->parity = 2;
        else {
            dcl_error("SET", 2, "INVPAR",
                      "invalid parity type - \\%s\\", parity_val);
            return SS$_BADPARAM;
        }
        changed = 1;
    }

    /* /DEVICE_TYPE=type */
    const char *devtype_val = dcl_qualifier_value(cmd, "DEVICE_TYPE");
    if (!devtype_val) devtype_val = dcl_qualifier_value(cmd, "DEVICE");
    if (devtype_val && *devtype_val) {
        strncpy(term->device_type, devtype_val, sizeof(term->device_type) - 1);
        term->device_type[sizeof(term->device_type) - 1] = '\0';
        /* Uppercase the device type */
        for (char *p = term->device_type; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        changed = 1;
    }

    /*
     * Boolean characteristic qualifiers.
     * Each pair: /NAME sets bit, /NONAME clears bit.
     * Check NO-form first so that if both are present, the positive wins.
     */
    static const struct { const char *on; const char *off; uint32_t bit; } quals[] = {
        { "ECHO",          "NOECHO",          TT_ECHO          },
        { "WRAP",          "NOWRAP",          TT_WRAP          },
        { "BROADCAST",     "NOBROADCAST",     TT_BROADCAST     },
        { "TYPEAHEAD",     "NOTYPEAHEAD",     TT_TYPEAHEAD     },
        { "HOSTSYNC",      "NOHOSTSYNC",      TT_HOSTSYNC      },
        { "TTSYNC",        "NOTTSYNC",        TT_TTSYNC        },
        { "LINE_EDITING",  "NOLINE_EDITING",  TT_LINE_EDITING  },
        { "INSERT",        "OVERSTRIKE",      TT_INSERT        },
        { "SCOPE",         "NOSCOPE",         TT_SCOPE         },
        { "LOWERCASE",     "UPPERCASE",       TT_LOWERCASE     },
        { "TAB",           "NOTAB",           TT_TAB           },
        { "MECHTAB",       "NOMECHTAB",       TT_MECHTAB       },
        { "HOLDSCREEN",    "NOHOLDSCREEN",    TT_HOLDSCREEN    },
        { "EIGHTBIT",      "NOEIGHTBIT",      TT_EIGHTBIT      },
        { "READSYNC",      "NOREADSYNC",      TT_READSYNC      },
        { "PASTHRU",       "NOPASTHRU",       TT_PASTHRU       },
        { "ESCAPE",        "NOESCAPE",        TT_ESCAPE        },
        { "FORM",          "NOFORM",          TT_FORM          },
        { "FULLDUP",       "HALFDUP",         TT_FULLDUP       },
        { "MODEM",         "NOMODEM",         TT_MODEM         },
        { "PAGE_CHAR",     "NOPAGE_CHAR",     TT_PAGE          },
        { "SECURE",        "NOSECURE",        TT_SECURE        },
        { "FALLBACK",      "NOFALLBACK",      TT_FALLBACK      },
        { "DIALUP",        "NODIALUP",        TT_DIALUP        },
        { "OPER",          "NOOPER",          TT_OPER          },
        { "ALTYPEAHD",     "NOALTYPEAHD",     TT_ALTYPEAHD     },
        { "RUNOUT",        "NORUNOUT",        TT_RUNOUT        },
    };

    for (unsigned i = 0; i < sizeof(quals)/sizeof(quals[0]); i++) {
        if (dcl_has_qualifier(cmd, quals[i].off)) {
            vms_terminal_set_char(term, quals[i].bit, 0);
            changed = 1;
        }
        if (dcl_has_qualifier(cmd, quals[i].on)) {
            vms_terminal_set_char(term, quals[i].bit, 1);
            changed = 1;
        }
    }

    /* Apply changes to real terminal */
    if (changed)
        vms_terminal_apply(term);

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
        dcl_error("RMS", 2, "PRV", "failed to set protection - %s", vms_strerror(errno));
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
                      "cannot set expiration date - %s", vms_strerror(errno));
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
                  "cannot set system time - %s", vms_strerror(errno));
        return SS$_IVTIME;
    }

    return SS$_NORMAL;
}

/*
 * SET HOST - Attempt DECnet connection (not available).
 */
static int cmd_set_host(struct dcl_command *cmd)
{
    (void)cmd;
    printf("%%SET-I-NOTAVAIL, DECnet is not available on this system\n");
    return SS$_NORMAL;
}

/*
 * SET AUDIT /ENABLE /DISABLE - Toggle security auditing.
 */
static int cmd_set_audit(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (dcl_has_qualifier(cmd, "ENABLE")) {
        ctx->audit_enabled = 1;
        printf("%%SET-I-INTSET, auditing enabled\n");
    } else if (dcl_has_qualifier(cmd, "DISABLE")) {
        ctx->audit_enabled = 0;
        printf("%%SET-I-INTSET, auditing disabled\n");
    } else {
        printf("%%SET-I-INTSET, security auditing is %s\n",
               ctx->audit_enabled ? "enabled" : "disabled");
    }
    return SS$_NORMAL;
}

/*
 * SET ACCOUNTING /ENABLE /DISABLE - Toggle accounting.
 */
static int cmd_set_accounting(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (dcl_has_qualifier(cmd, "ENABLE")) {
        ctx->accounting_enabled = 1;
        printf("%%SET-I-INTSET, accounting enabled\n");
    } else if (dcl_has_qualifier(cmd, "DISABLE")) {
        ctx->accounting_enabled = 0;
        printf("%%SET-I-INTSET, accounting disabled\n");
    } else {
        printf("%%SET-I-INTSET, accounting is %s\n",
               ctx->accounting_enabled ? "enabled" : "disabled");
    }
    return SS$_NORMAL;
}

/*
 * SET VOLUME - Set volume characteristics (requires mounted VMSFS).
 */
static int cmd_set_volume(struct dcl_command *cmd)
{
    (void)cmd;
    printf("%%SET-I-NOTIMPL, SET VOLUME requires a mounted VMSFS volume\n");
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
    if (dcl_match_command(subcmd, "HOST", 3))
        return cmd_set_host(cmd);
    if (dcl_match_command(subcmd, "AUDIT", 3))
        return cmd_set_audit(cmd);
    if (dcl_match_command(subcmd, "ACCOUNTING", 3))
        return cmd_set_accounting(cmd);
    if (dcl_match_command(subcmd, "VOLUME", 3))
        return cmd_set_volume(cmd);
    if (dcl_match_command(subcmd, "ENTRY", 3))
        return cmd_set_entry(cmd);
    if (dcl_match_command(subcmd, "QUEUE", 3))
        return cmd_set_queue(cmd);

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
                vmsfs_to_linux_path(ctx->default_dir, linux_dir, sizeof(linux_dir));
            }
        }
    } else {
        vmsfs_to_linux_path(ctx->default_dir, linux_dir, sizeof(linux_dir));
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

    /* /ENTRY=n qualifier: delete a queue entry */
    if (dcl_has_qualifier(cmd, "ENTRY")) {
        const char *entry_str = dcl_qualifier_value(cmd, "ENTRY");
        if (!entry_str || !entry_str[0]) {
            dcl_error("DCL", 2, "NOENTRY", "missing entry number with /ENTRY");
            return SS$_BADPARAM;
        }
        uint32_t entry_id = (uint32_t)atol(entry_str);
        if (entry_id == 0) {
            dcl_error("DCL", 2, "BADENTRY", "invalid entry number - %s", entry_str);
            return SS$_BADPARAM;
        }
        int qsts = ensure_queue_init();
        if (!(qsts & 1)) {
            dcl_error("DELETE", 2, "QMANERR", "queue manager initialization failed");
            return qsts;
        }
        qsts = vmsq_delete_entry(entry_id);
        if (!(qsts & 1)) {
            dcl_error("DELETE", 2, "ENTNOTFND", "entry %u not found", entry_id);
            return qsts;
        }
        printf("%%DELETE-S-DELETED, entry %u deleted\n", entry_id);
        return SS$_NORMAL;
    }

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
            vmsfs_to_linux_path(ctx->default_dir, dir, sizeof(dir));
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
                  "rename failed - %s", vms_strerror(errno));
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
                      cmd->params[0], vms_strerror(errno));
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
                vmsfs_to_linux_path(ctx->default_dir, linux_dir, sizeof(linux_dir));
            }
        }
    } else {
        /* Default: current directory, all files (*.*) */
        vmsfs_to_linux_path(ctx->default_dir, linux_dir, sizeof(linux_dir));
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
 * Queue initialization helper — ensures QMAN$MASTER.DAT exists and
 * default queues (SYS$BATCH, SYS$PRINT) are created.
 * Called lazily on first queue command.
 */
static int queue_initialized = 0;

static int ensure_queue_init(void)
{
    if (queue_initialized)
        return SS$_NORMAL;

    /* Use /tmp for testing, SYS$MANAGER: in production */
    const char *db_path = getenv("VMSQ_DB_PATH");
    if (!db_path)
        db_path = "/tmp/QMAN_MASTER.DAT";

    int sts = vmsq_init(db_path);
    if (sts & 1) {
        queue_initialized = 1;
        /* Create default queues if they don't exist (ignore DUPLNAM) */
        vmsq_create_queue("SYS$BATCH", VMSQ_TYPE_BATCH);
        vmsq_create_queue("SYS$PRINT", VMSQ_TYPE_PRINT);
    }
    return sts;
}

/*
 * SUBMIT - Submit a command procedure for batch execution.
 * Format: SUBMIT filespec [/QUEUE=name]
 * Queues a command procedure to a batch queue via vmsqueue.
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

    int sts = ensure_queue_init();
    if (!(sts & 1)) {
        dcl_error("SUBMIT", 2, "QMANERR", "queue manager initialization failed");
        return sts;
    }

    /* Get queue name (/QUEUE=name, default SYS$BATCH) */
    const char *queue_name = dcl_qualifier_value(cmd, "QUEUE");
    if (!queue_name || !queue_name[0]) queue_name = "SYS$BATCH";

    /* Format job name from filename (uppercase, strip path and extension) */
    const char *bn = strrchr(cmd->params[0], ']');
    if (!bn) bn = strrchr(cmd->params[0], ':');
    if (bn) bn++; else bn = cmd->params[0];

    char upper_name[256];
    size_t i;
    for (i = 0; bn[i] && bn[i] != '.' && bn[i] != ';' && i < sizeof(upper_name)-1; i++)
        upper_name[i] = (char)toupper((unsigned char)bn[i]);
    upper_name[i] = '\0';

    const char *user = ctx->username[0] ? ctx->username : "SYSTEM";

    uint32_t entry_id = 0;
    sts = vmsq_submit(queue_name, upper_name, user, &entry_id);
    if (!(sts & 1)) {
        dcl_error("SUBMIT", 2, "SUBMITERR", "failed to submit job to queue %s",
                  queue_name);
        return sts;
    }

    printf("%%SUBMIT-S-SUBMITTED, job %s (queue %s, entry %u) queued\n",
           upper_name, queue_name, entry_id);

    return SS$_NORMAL;
}

/*
 * PRINT - Queue a file for printing.
 * Format: PRINT filespec[,...] [/QUEUE=queue-name] [/COPIES=n]
 * Sends files to the print queue via vmsqueue.
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

    int sts = ensure_queue_init();
    if (!(sts & 1)) {
        dcl_error("PRINT", 2, "QMANERR", "queue manager initialization failed");
        return sts;
    }

    /* Get queue name (/QUEUE=name, default SYS$PRINT) */
    const char *queue_name = dcl_qualifier_value(cmd, "QUEUE");
    if (!queue_name || !queue_name[0]) queue_name = "SYS$PRINT";

    /* Format filename for display (uppercase, keep extension) */
    const char *bn = strrchr(cmd->params[0], ']');
    if (!bn) bn = strrchr(cmd->params[0], ':');
    if (bn) bn++; else bn = cmd->params[0];

    char upper_name[256];
    size_t i;
    for (i = 0; bn[i] && bn[i] != ';' && i < sizeof(upper_name)-1; i++)
        upper_name[i] = (char)toupper((unsigned char)bn[i]);
    upper_name[i] = '\0';

    const char *user = ctx->username[0] ? ctx->username : "SYSTEM";

    uint32_t entry_id = 0;
    sts = vmsq_submit(queue_name, upper_name, user, &entry_id);
    if (!(sts & 1)) {
        dcl_error("PRINT", 2, "PRINTERR", "failed to queue file to %s",
                  queue_name);
        return sts;
    }

    printf("%%PRINT-S-QUEUED, job %s (queue %s, entry %u) queued\n",
           upper_name, queue_name, entry_id);

    return SS$_NORMAL;
}

/*
 * SHOW QUEUE - Display queue status and entries.
 * Format: SHOW QUEUE [name] [/ALL] [/FULL]
 * Displays queue information matching VMS output format.
 */
static int cmd_show_queue(struct dcl_command *cmd)
{
    int sts = ensure_queue_init();
    if (!(sts & 1)) {
        dcl_error("SHOW", 2, "QMANERR", "queue manager initialization failed");
        return sts;
    }

    int show_all = dcl_has_qualifier(cmd, "ALL");
    int show_full = dcl_has_qualifier(cmd, "FULL");

    /* Queue name is params[1] if present (params[0] is "QUEUE") */
    const char *queue_name = NULL;
    if (cmd->param_count >= 2 && cmd->params[1][0] != '\0')
        queue_name = cmd->params[1];

    /* If a specific queue name given, show just that queue */
    if (queue_name) {
        struct vms_queue qinfo;
        sts = vmsq_show_queue(queue_name, &qinfo);
        if (!(sts & 1)) {
            dcl_error("SHOW", 2, "NOSUCHQUE", "no such queue - %s", queue_name);
            return sts;
        }

        const char *type_str = qinfo.type == VMSQ_TYPE_BATCH ? "Batch" :
                               qinfo.type == VMSQ_TYPE_PRINT ? "Printer" : "Generic";
        const char *status_str = qinfo.status == VMSQ_STATUS_STARTED ? "started" :
                                 qinfo.status == VMSQ_STATUS_STOPPED ? "stopped" :
                                 qinfo.status == VMSQ_STATUS_PAUSED  ? "paused" : "unknown";

        printf("  %s queue %s, %s\n", type_str, qinfo.name, status_str);

        if (show_full || show_all) {
            struct vms_queue_entry entries[64];
            int count = 0;
            vmsq_show_entries(queue_name, entries, 64, &count);
            for (int j = 0; j < count; j++) {
                const char *entry_status =
                    entries[j].status == VMSQ_ENTRY_PENDING   ? "Pending" :
                    entries[j].status == VMSQ_ENTRY_EXECUTING ? "Executing" :
                    entries[j].status == VMSQ_ENTRY_HOLDING   ? "Holding" :
                    entries[j].status == VMSQ_ENTRY_COMPLETED ? "Completed" : "Unknown";
                printf("    entry %-6u %-20s %-12s %-10s\n",
                       entries[j].entry_id, entries[j].job_name,
                       entries[j].username, entry_status);
            }
        }
    } else {
        /* Show all queues — iterate SYS$BATCH, SYS$PRINT, then any others */
        const char *default_queues[] = { "SYS$BATCH", "SYS$PRINT", NULL };

        for (int q = 0; default_queues[q]; q++) {
            struct vms_queue qinfo;
            sts = vmsq_show_queue(default_queues[q], &qinfo);
            if (!(sts & 1)) continue;

            const char *type_str = qinfo.type == VMSQ_TYPE_BATCH ? "Batch" :
                                   qinfo.type == VMSQ_TYPE_PRINT ? "Printer" : "Generic";
            const char *status_str = qinfo.status == VMSQ_STATUS_STARTED ? "started" :
                                     qinfo.status == VMSQ_STATUS_STOPPED ? "stopped" :
                                     qinfo.status == VMSQ_STATUS_PAUSED  ? "paused" : "unknown";

            printf("  %s queue %s, %s\n", type_str, qinfo.name, status_str);

            if (show_full || show_all) {
                struct vms_queue_entry entries[64];
                int count = 0;
                vmsq_show_entries(default_queues[q], entries, 64, &count);
                for (int j = 0; j < count; j++) {
                    const char *entry_status =
                        entries[j].status == VMSQ_ENTRY_PENDING   ? "Pending" :
                        entries[j].status == VMSQ_ENTRY_EXECUTING ? "Executing" :
                        entries[j].status == VMSQ_ENTRY_HOLDING   ? "Holding" :
                        entries[j].status == VMSQ_ENTRY_COMPLETED ? "Completed" : "Unknown";
                    printf("    entry %-6u %-20s %-12s %-10s\n",
                           entries[j].entry_id, entries[j].job_name,
                           entries[j].username, entry_status);
                }
            }
        }
    }

    return SS$_NORMAL;
}

/*
 * SET ENTRY - Modify a queued job entry.
 * Format: SET ENTRY n /HOLD or /RELEASE
 */
static int cmd_set_entry(struct dcl_command *cmd)
{
    int sts = ensure_queue_init();
    if (!(sts & 1)) {
        dcl_error("SET", 2, "QMANERR", "queue manager initialization failed");
        return sts;
    }

    /* Entry number is params[1] (params[0] is "ENTRY") */
    if (cmd->param_count < 2 || cmd->params[1][0] == '\0') {
        dcl_error("SET", 2, "NOENTRY", "missing entry number");
        return SS$_BADPARAM;
    }

    uint32_t entry_id = (uint32_t)atol(cmd->params[1]);
    if (entry_id == 0) {
        dcl_error("SET", 2, "BADENTRY", "invalid entry number - %s", cmd->params[1]);
        return SS$_BADPARAM;
    }

    if (dcl_has_qualifier(cmd, "HOLD")) {
        sts = vmsq_hold_entry(entry_id);
        if (!(sts & 1)) {
            dcl_error("SET", 2, "ENTNOTFND", "entry %u not found", entry_id);
            return sts;
        }
        printf("%%SET-S-MODIFIED, entry %u set to HOLD\n", entry_id);
    } else if (dcl_has_qualifier(cmd, "RELEASE")) {
        sts = vmsq_release_entry(entry_id);
        if (!(sts & 1)) {
            dcl_error("SET", 2, "ENTNOTFND", "entry %u not found", entry_id);
            return sts;
        }
        printf("%%SET-S-MODIFIED, entry %u released\n", entry_id);
    } else {
        dcl_error("SET", 2, "IVQUAL",
                  "specify /HOLD or /RELEASE with SET ENTRY");
        return SS$_BADPARAM;
    }

    return SS$_NORMAL;
}

/*
 * SHOW ENTRY - Display detailed info about a specific queue entry.
 * Format: SHOW ENTRY [entry-number]
 * If no entry number given, shows all entries across all queues.
 */
static int cmd_show_entry(struct dcl_command *cmd)
{
    int sts = ensure_queue_init();
    if (!(sts & 1)) {
        dcl_error("SHOW", 2, "QMANERR", "queue manager initialization failed");
        return sts;
    }

    /* VMS binary time conversion constants */
    #define SHOW_ENTRY_VMS_UNIX_DIFF 3506716800ULL
    #define SHOW_ENTRY_VMS_TICKS     10000000ULL

    /* Entry number is params[1] if present (params[0] is "ENTRY") */
    if (cmd->param_count >= 2 && cmd->params[1][0] != '\0') {
        uint32_t entry_id = (uint32_t)atol(cmd->params[1]);
        if (entry_id == 0) {
            dcl_error("SHOW", 2, "BADENTRY", "invalid entry number - %s",
                      cmd->params[1]);
            return SS$_BADPARAM;
        }

        struct vms_queue_entry entry;
        sts = vmsq_show_entry(entry_id, &entry);
        if (!(sts & 1)) {
            dcl_error("SHOW", 2, "ENTNOTFND", "entry %u not found", entry_id);
            return sts;
        }

        const char *status_str =
            entry.status == VMSQ_ENTRY_PENDING   ? "Pending" :
            entry.status == VMSQ_ENTRY_EXECUTING ? "Executing" :
            entry.status == VMSQ_ENTRY_HOLDING   ? "Holding" :
            entry.status == VMSQ_ENTRY_COMPLETED ? "Completed" : "Unknown";

        printf("  Entry  Jobname         Username     Status\n");
        printf("  -----  -------         --------     ------\n");
        printf("  %5u  %-15s %-12s %s\n",
               entry.entry_id, entry.job_name, entry.username, status_str);

        /* Show submission time and queue */
        if (entry.submit_time) {
            uint64_t unix_secs = entry.submit_time / SHOW_ENTRY_VMS_TICKS
                                 - SHOW_ENTRY_VMS_UNIX_DIFF;
            time_t t = (time_t)unix_secs;
            struct tm tm;
            localtime_r(&t, &tm);
            printf("         Submitted %2d-%s-%04d %02d:%02d on queue %s\n",
                   tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
                   tm.tm_hour, tm.tm_min, entry.queue_name);
        }
    } else {
        /* No entry number — show all entries across default queues */
        const char *queues[] = { "SYS$BATCH", "SYS$PRINT", NULL };
        int found = 0;

        for (int q = 0; queues[q]; q++) {
            struct vms_queue_entry entries[64];
            int count = 0;
            vmsq_show_entries(queues[q], entries, 64, &count);
            if (count == 0) continue;

            if (!found) {
                printf("  Entry  Jobname         Username     Status\n");
                printf("  -----  -------         --------     ------\n");
                found = 1;
            }

            for (int j = 0; j < count; j++) {
                const char *status_str =
                    entries[j].status == VMSQ_ENTRY_PENDING   ? "Pending" :
                    entries[j].status == VMSQ_ENTRY_EXECUTING ? "Executing" :
                    entries[j].status == VMSQ_ENTRY_HOLDING   ? "Holding" :
                    entries[j].status == VMSQ_ENTRY_COMPLETED ? "Completed" : "Unknown";
                printf("  %5u  %-15s %-12s %s\n",
                       entries[j].entry_id, entries[j].job_name,
                       entries[j].username, status_str);
                if (entries[j].submit_time) {
                    uint64_t unix_secs = entries[j].submit_time / SHOW_ENTRY_VMS_TICKS
                                         - SHOW_ENTRY_VMS_UNIX_DIFF;
                    time_t t = (time_t)unix_secs;
                    struct tm tm;
                    localtime_r(&t, &tm);
                    printf("         Submitted %2d-%s-%04d %02d:%02d on queue %s\n",
                           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
                           tm.tm_hour, tm.tm_min, entries[j].queue_name);
                }
            }
        }

        if (!found) {
            printf("%%SHOW-I-NOENTRY, no entries found\n");
        }
    }

    #undef SHOW_ENTRY_VMS_UNIX_DIFF
    #undef SHOW_ENTRY_VMS_TICKS

    return SS$_NORMAL;
}

/*
 * SET QUEUE - Modify queue state.
 * Format: SET QUEUE queue-name /STOP | /START | /PAUSE
 */
static int cmd_set_queue(struct dcl_command *cmd)
{
    int sts = ensure_queue_init();
    if (!(sts & 1)) {
        dcl_error("SET", 2, "QMANERR", "queue manager initialization failed");
        return sts;
    }

    /* Queue name is params[1] (params[0] is "QUEUE") */
    if (cmd->param_count < 2 || cmd->params[1][0] == '\0') {
        dcl_error("SET", 2, "NOQUNAM", "missing queue name");
        return SS$_BADPARAM;
    }

    const char *queue_name = cmd->params[1];

    /* Check queue exists */
    struct vms_queue qinfo;
    sts = vmsq_show_queue(queue_name, &qinfo);
    if (!(sts & 1)) {
        dcl_error("SET", 2, "NOSUCHQUE", "no such queue - %s", queue_name);
        return sts;
    }

    if (dcl_has_qualifier(cmd, "STOP")) {
        sts = vmsq_set_queue_status(queue_name, VMSQ_STATUS_STOPPED);
        if (!(sts & 1)) {
            dcl_error("SET", 2, "QMANERR", "failed to stop queue %s", queue_name);
            return sts;
        }
        printf("%%SET-S-QUEMOD, queue %s stopped\n", queue_name);
    } else if (dcl_has_qualifier(cmd, "START")) {
        sts = vmsq_set_queue_status(queue_name, VMSQ_STATUS_STARTED);
        if (!(sts & 1)) {
            dcl_error("SET", 2, "QMANERR", "failed to start queue %s", queue_name);
            return sts;
        }
        printf("%%SET-S-QUEMOD, queue %s started\n", queue_name);
    } else if (dcl_has_qualifier(cmd, "PAUSE")) {
        sts = vmsq_set_queue_status(queue_name, VMSQ_STATUS_PAUSED);
        if (!(sts & 1)) {
            dcl_error("SET", 2, "QMANERR", "failed to pause queue %s", queue_name);
            return sts;
        }
        printf("%%SET-S-QUEMOD, queue %s paused\n", queue_name);
    } else {
        dcl_error("SET", 2, "IVQUAL",
                  "specify /STOP, /START, or /PAUSE with SET QUEUE");
        return SS$_BADPARAM;
    }

    return SS$_NORMAL;
}

/*
 * SHOW INTRUSION - Display intrusion database.
 * Reads /vms/SYS0/SYSCOMMON/SYSMGR/INTRUSION.DAT
 * Format per line: timestamp|username|source|type|count
 */
static int cmd_show_intrusion(struct dcl_command *cmd)
{
    (void)cmd;

    const char *intrusion_path = "/vms/SYS0/SYSCOMMON/SYSMGR/INTRUSION.DAT";
    FILE *fp = fopen(intrusion_path, "r");
    if (!fp) {
        printf("%%SHOW-I-NOINTRUSION, no intrusion records found\n");
        return SS$_NORMAL;
    }

    char line[512];
    int found = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        /* Parse: timestamp|username|source|type|count */
        char *timestamp = line;
        char *username = strchr(timestamp, '|');
        if (!username) continue;
        *username++ = '\0';

        char *source = strchr(username, '|');
        if (!source) continue;
        *source++ = '\0';

        char *type = strchr(source, '|');
        if (!type) continue;
        *type++ = '\0';

        char *count_str = strchr(type, '|');
        if (!count_str) continue;
        *count_str++ = '\0';

        if (!found) {
            printf("Intrusion   Type    Count  Expiration          Source\n");
            found = 1;
        }

        printf("%-11s %-7s %5s  %-19s %s\n",
               type, username, count_str, timestamp, source);
    }

    fclose(fp);

    if (!found) {
        printf("%%SHOW-I-NOINTRUSION, no intrusion records found\n");
    }

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
        /* Parent - wait for child (WUNTRACED for Ctrl-Y stop support) */
        extern volatile sig_atomic_t dcl_running_child;
        dcl_running_child = (sig_atomic_t)pid;
        int wstatus;
        waitpid(pid, &wstatus, WUNTRACED);
        dcl_running_child = 0;
        if (WIFSTOPPED(wstatus)) {
            /* Child was stopped by Ctrl-Y — save for CONTINUE */
            printf("\nInterrupt\n");
            ctx->interrupted_pid = pid;
            return SS$_ABORT;
        }
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
 * SPAWN - Create a DCL subprocess.
 *
 * SPAWN                    — interactive DCL subprocess
 * SPAWN cmd                — execute single DCL command in subprocess
 * SPAWN /NOWAIT cmd        — run DCL subprocess in background
 * SPAWN /OUTPUT=file cmd   — redirect subprocess stdout to file
 */
static int cmd_spawn(struct dcl_command *cmd)
{
    /* Resolve our own binary path for re-exec */
    char self_exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
    if (len < 0)
        strncpy(self_exe, "vmsdcl", sizeof(self_exe) - 1);
    else
        self_exe[len] = '\0';

    /* Check qualifiers */
    int nowait = dcl_has_qualifier(cmd, "NOWAIT");
    const char *output_file = dcl_qualifier_value(cmd, "OUTPUT");

    /* Build command text from params if any */
    int has_command = (cmd->param_count >= 1 && cmd->params[0][0] != '\0');
    char command_text[DCL_MAX_LINE] = {0};
    if (has_command) {
        for (int i = 0; i < cmd->param_count; i++) {
            if (i > 0)
                strncat(command_text, " ",
                        sizeof(command_text) - strlen(command_text) - 1);
            strncat(command_text, cmd->params[i],
                    sizeof(command_text) - strlen(command_text) - 1);
        }
        /* Append rest-of-line if present (after pipe chars etc.) */
        if (cmd->rest[0]) {
            strncat(command_text, " ",
                    sizeof(command_text) - strlen(command_text) - 1);
            strncat(command_text, cmd->rest,
                    sizeof(command_text) - strlen(command_text) - 1);
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child process */

        /* Handle /OUTPUT=file redirection */
        if (output_file) {
            int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }

        if (has_command) {
            execl(self_exe, "vmsdcl", "-c", command_text, (char *)NULL);
        } else {
            execl(self_exe, "vmsdcl", (char *)NULL);
        }
        fprintf(stderr, "%%DCL-E-CREPRC, cannot create subprocess\n");
        _exit(127);
    } else if (pid > 0) {
        if (nowait) {
            /* /NOWAIT: print PID and return immediately */
            printf("%%DCL-I-SPAWNED, process id is %d\n", (int)pid);
        } else {
            extern volatile sig_atomic_t dcl_running_child;
            struct dcl_context *spawn_ctx = dcl_get_context();
            dcl_running_child = (sig_atomic_t)pid;
            int wstatus;
            waitpid(pid, &wstatus, WUNTRACED);
            dcl_running_child = 0;
            if (WIFSTOPPED(wstatus)) {
                printf("\nInterrupt\n");
                spawn_ctx->interrupted_pid = pid;
                return SS$_ABORT;
            }
            if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0)
                return SS$_ABORT;
        }
        return SS$_NORMAL;
    } else {
        dcl_error("DCL", 4, "CREPRC", "cannot create process");
        return SS$_ABORT;
    }
}

/*
 * pipe_split_segments - Split a command line on unquoted '|' characters.
 *
 * Populates segments[] with pointers into the mutable buffer 'line'.
 * Each '|' is replaced with '\0'.  Returns the number of segments,
 * or -1 if max_seg is exceeded.
 */
static int pipe_split_segments(char *line, char **segments, int max_seg)
{
    int count = 0;
    char *p = line;

    /* Skip leading whitespace */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return 0;

    segments[count++] = p;

    while (*p) {
        if (*p == '"') {
            /* Skip quoted string */
            p++;
            while (*p && *p != '"') p++;
            if (*p == '"') p++;
        } else if (*p == '|') {
            /* Segment boundary */
            *p = '\0';
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (count >= max_seg) return -1;
            segments[count++] = p;
        } else {
            p++;
        }
    }

    /* Trim trailing whitespace from each segment */
    for (int i = 0; i < count; i++) {
        char *end = segments[i] + strlen(segments[i]) - 1;
        while (end >= segments[i] && (*end == ' ' || *end == '\t'))
            *end-- = '\0';
    }

    return count;
}

/*
 * pipe_get_self_exe - Get the path of the current vmsdcl executable.
 *
 * Uses /proc/self/exe (Linux-specific).  Falls back to "vmsdcl" (PATH lookup).
 */
static const char *pipe_get_self_exe(void)
{
    static char exe_path[4096];
    static int resolved = 0;

    if (!resolved) {
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len > 0) {
            exe_path[len] = '\0';
        } else {
            /* Fallback: rely on PATH */
            strncpy(exe_path, "vmsdcl", sizeof(exe_path) - 1);
            exe_path[sizeof(exe_path) - 1] = '\0';
        }
        resolved = 1;
    }
    return exe_path;
}

/*
 * PIPE - Execute a DCL pipeline.
 *
 * Syntax: PIPE cmd1 | cmd2 | cmd3
 *
 * Each segment is executed as a child process running vmsdcl -c "segment".
 * Adjacent segments are connected via pipe(2), so stdout of cmd1 feeds
 * stdin of cmd2, etc.  The exit status of the last segment is propagated.
 *
 * A single segment (no '|') simply runs vmsdcl -c "segment" with inherited
 * stdin/stdout.
 */
static int cmd_pipe(struct dcl_command *cmd)
{
    /* Reconstruct the entire command line after PIPE */
    char pipeline[DCL_MAX_LINE] = {0};
    for (int i = 0; i < cmd->param_count; i++) {
        if (i > 0) strncat(pipeline, " ",
                            sizeof(pipeline) - strlen(pipeline) - 1);
        strncat(pipeline, cmd->params[i],
                sizeof(pipeline) - strlen(pipeline) - 1);
    }
    if (cmd->rest[0]) {
        if (pipeline[0]) strncat(pipeline, " | ",
                                  sizeof(pipeline) - strlen(pipeline) - 1);
        strncat(pipeline, cmd->rest,
                sizeof(pipeline) - strlen(pipeline) - 1);
    }

    if (pipeline[0] == '\0') {
        dcl_error("DCL", 2, "NOKEYW", "missing command for PIPE");
        return SS$_BADPARAM;
    }

    /* Split into segments on '|' */
#define PIPE_MAX_SEGMENTS 64
    char *segments[PIPE_MAX_SEGMENTS];
    int nseg = pipe_split_segments(pipeline, segments, PIPE_MAX_SEGMENTS);
    if (nseg <= 0) {
        dcl_error("DCL", 2, "NOKEYW", "missing command for PIPE");
        return SS$_BADPARAM;
    }
    if (nseg < 0) {
        dcl_error("DCL", 2, "IVPIPE", "too many pipe segments");
        return SS$_BADPARAM;
    }

    const char *dcl_exe = pipe_get_self_exe();

    /*
     * For N segments we need N-1 pipes.
     * pipefds[i] connects segment i's stdout to segment i+1's stdin.
     */
    int pipefds[PIPE_MAX_SEGMENTS - 1][2];
    pid_t pids[PIPE_MAX_SEGMENTS];

    /* Create all pipes first */
    for (int i = 0; i < nseg - 1; i++) {
        if (pipe(pipefds[i]) < 0) {
            dcl_error("DCL", 4, "CREPRC", "cannot create pipe");
            /* Close any already-created pipes */
            for (int j = 0; j < i; j++) {
                close(pipefds[j][0]);
                close(pipefds[j][1]);
            }
            return SS$_ABORT;
        }
    }

    /* Fork each segment */
    for (int i = 0; i < nseg; i++) {
        /* Skip empty segments */
        if (segments[i][0] == '\0') {
            dcl_error("DCL", 2, "IVPIPE", "empty pipe segment");
            /* Clean up pipes and already-forked children */
            for (int j = 0; j < nseg - 1; j++) {
                close(pipefds[j][0]);
                close(pipefds[j][1]);
            }
            for (int j = 0; j < i; j++) {
                waitpid(pids[j], NULL, 0);
            }
            return SS$_BADPARAM;
        }

        pid_t pid = fork();
        if (pid < 0) {
            dcl_error("DCL", 4, "CREPRC", "cannot create process");
            /* Clean up */
            for (int j = 0; j < nseg - 1; j++) {
                close(pipefds[j][0]);
                close(pipefds[j][1]);
            }
            for (int j = 0; j < i; j++) {
                waitpid(pids[j], NULL, 0);
            }
            return SS$_ABORT;
        }

        if (pid == 0) {
            /* Child process */

            /* Wire stdin from previous pipe (except first segment) */
            if (i > 0) {
                dup2(pipefds[i - 1][0], STDIN_FILENO);
            }

            /* Wire stdout to next pipe (except last segment) */
            if (i < nseg - 1) {
                dup2(pipefds[i][1], STDOUT_FILENO);
            }

            /* Close all pipe fds in child */
            for (int j = 0; j < nseg - 1; j++) {
                close(pipefds[j][0]);
                close(pipefds[j][1]);
            }

            /* Exec vmsdcl -c "segment" */
            execl(dcl_exe, "vmsdcl", "-c", segments[i], (char *)NULL);
            fprintf(stderr, "%%DCL-E-CREPRC, cannot execute - %s\n", dcl_exe);
            _exit(127);
        }

        pids[i] = pid;
    }

    /* Parent: close all pipe fds */
    for (int i = 0; i < nseg - 1; i++) {
        close(pipefds[i][0]);
        close(pipefds[i][1]);
    }

    /* Wait for all children, capture last segment's exit status */
    int last_status = SS$_NORMAL;
    for (int i = 0; i < nseg; i++) {
        int wstatus;
        waitpid(pids[i], &wstatus, 0);
        if (i == nseg - 1) {
            if (WIFEXITED(wstatus))
                last_status = (WEXITSTATUS(wstatus) == 0)
                              ? SS$_NORMAL : SS$_ABORT;
            else
                last_status = SS$_ABORT;
        }
    }

    return last_status;
#undef PIPE_MAX_SEGMENTS
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

/* ================================================================== */
/*           External Utility Executor                                 */
/* ================================================================== */

/*
 * dcl_exec_utility - Fork/exec a VMS utility and wait for completion.
 *
 * Searches for the binary in SYS$SYSTEM first, then standard paths.
 * Handles Ctrl-Y interruption and child process management.
 *
 * @exe_name:  Binary name (e.g. "SYSGEN.EXE", "MAIL.EXE")
 * @facility:  Error message facility (e.g. "SYSGEN", "MAIL")
 * @argv:      NULL-terminated argument vector (argv[0] is placeholder)
 * @argc:      Number of arguments (not counting NULL terminator)
 *
 * Returns VMS status code.
 */
static int dcl_exec_utility(const char *exe_name, const char *facility,
                            char *argv[], int argc)
{
    (void)argc;

    /* Search order: SYS$SYSTEM, /usr/local/bin, PATH */
    char sys_path[PATH_MAX];
    snprintf(sys_path, sizeof(sys_path),
             "/vms/SYS0/SYSCOMMON/SYSEXE/%s", exe_name);

    char usr_path[PATH_MAX];
    snprintf(usr_path, sizeof(usr_path), "/usr/local/bin/%s", exe_name);

    const char *bin = NULL;
    if (access(sys_path, X_OK) == 0)
        bin = sys_path;
    else if (access(usr_path, X_OK) == 0)
        bin = usr_path;

    /* Set argv[0] to resolved path or exe_name for PATH search */
    argv[0] = (char *)(bin ? bin : exe_name);

    pid_t pid = fork();
    if (pid == 0) {
        if (bin)
            execv(bin, argv);
        execvp(exe_name, argv);
        fprintf(stderr, "%%%s-F-NOIMG, cannot execute %s\n", facility, exe_name);
        _exit(1);
    } else if (pid > 0) {
        extern volatile sig_atomic_t dcl_running_child;
        struct dcl_context *ctx = dcl_get_context();
        dcl_running_child = (sig_atomic_t)pid;
        int wstatus;
        waitpid(pid, &wstatus, WUNTRACED);
        dcl_running_child = 0;
        if (WIFSTOPPED(wstatus)) {
            printf("\nInterrupt\n");
            ctx->interrupted_pid = pid;
            return SS$_ABORT;
        }
        if (WIFEXITED(wstatus))
            return (WEXITSTATUS(wstatus) == 0) ? SS$_NORMAL : SS$_ABORT;
    } else {
        dcl_error("DCL", 4, "CREPRC", "cannot create process for %s", facility);
        return SS$_ABORT;
    }
    return SS$_NORMAL;
}

/* ================================================================== */
/*                        ANALYZE Command                              */
/* ================================================================== */

static int cmd_analyze(struct dcl_command *cmd)
{
    const char *qualifier = NULL;
    const char *param = NULL;

    if (dcl_has_qualifier(cmd, "DISK_STRUCTURE"))
        qualifier = "/DISK_STRUCTURE";
    else if (dcl_has_qualifier(cmd, "SYSTEM"))
        qualifier = "/SYSTEM";
    else if (dcl_has_qualifier(cmd, "IMAGE"))
        qualifier = "/IMAGE";
    else if (dcl_has_qualifier(cmd, "OBJECT"))
        qualifier = "/OBJECT";

    if (!qualifier) {
        if (cmd->param_count >= 1 && cmd->params[0][0] == '/') {
            qualifier = cmd->params[0];
            param = (cmd->param_count >= 2) ? cmd->params[1] : NULL;
        } else {
            dcl_error("ANALYZE", 2, "NOKEYW",
                      "qualifier required (/DISK_STRUCTURE, /SYSTEM, /IMAGE, /OBJECT)");
            return SS$_BADPARAM;
        }
    } else {
        param = (cmd->param_count >= 1 && cmd->params[0][0] != '\0')
                ? cmd->params[0] : NULL;
    }

    char *argv[8] = {NULL};
    int argc = 0;
    argv[argc++] = NULL; /* placeholder for binary path */
    argv[argc++] = (char *)qualifier;
    if (param)
        argv[argc++] = (char *)param;
    argv[argc] = NULL;
    return dcl_exec_utility("ANALYZE.EXE", "ANALYZE", argv, argc);
}

/* MAIL — pass qualifiers and params through */
static int cmd_mail(struct dcl_command *cmd)
{
    char *argv[64] = {NULL};
    int argc = 0;
    argv[argc++] = NULL; /* placeholder */
    for (int i = 0; i < cmd->qualifier_count && argc < 62; i++)
        argv[argc++] = cmd->qualifiers[i].name;
    for (int i = 0; i < cmd->param_count && argc < 62; i++) {
        if (cmd->params[i][0] != '\0')
            argv[argc++] = cmd->params[i];
    }
    argv[argc] = NULL;
    return dcl_exec_utility("MAIL.EXE", "MAIL", argv, argc);
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

/* MONITOR — pass subcommand (default SYSTEM) */
static int cmd_monitor(struct dcl_command *cmd)
{
    const char *subcmd = (cmd->param_count >= 1 && cmd->params[0][0] != '\0')
                         ? cmd->params[0] : "SYSTEM";
    char *argv[4] = {NULL, (char *)subcmd, NULL};
    return dcl_exec_utility("MONITOR.EXE", "MONITOR", argv, 2);
}

/* SYSGEN — interactive, no args */
static int cmd_sysgen(struct dcl_command *cmd)
{
    (void)cmd;
    char *argv[2] = {NULL, NULL};
    return dcl_exec_utility("SYSGEN.EXE", "SYSGEN", argv, 1);
}

/* SYSMAN — interactive, no args */
static int cmd_sysman(struct dcl_command *cmd)
{
    (void)cmd;
    char *argv[2] = {NULL, NULL};
    return dcl_exec_utility("SYSMAN.EXE", "SYSMAN", argv, 1);
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
/*                     TCPIP Commands                                  */
/* ================================================================== */

/*
 * Map a Linux network interface name to a VMS device name.
 * eth*, ens*, enp* → SE0, SE1, ...
 * lo              → LO0
 * wlan*, wlp*     → EW0, EW1, ...
 * tun*            → TN0, TN1, ...
 * Everything else → XX0, XX1, ...
 */
static const char *tcpip_map_interface(const char *linux_name,
                                        int *se_idx, int *ew_idx,
                                        int *tn_idx, int *xx_idx)
{
    static char vms_name[16];

    if (strcmp(linux_name, "lo") == 0) {
        snprintf(vms_name, sizeof(vms_name), "LO0");
    } else if (strncmp(linux_name, "eth", 3) == 0 ||
               strncmp(linux_name, "ens", 3) == 0 ||
               strncmp(linux_name, "enp", 3) == 0) {
        snprintf(vms_name, sizeof(vms_name), "SE%d", (*se_idx)++);
    } else if (strncmp(linux_name, "wlan", 4) == 0 ||
               strncmp(linux_name, "wlp", 3) == 0) {
        snprintf(vms_name, sizeof(vms_name), "EW%d", (*ew_idx)++);
    } else if (strncmp(linux_name, "tun", 3) == 0) {
        snprintf(vms_name, sizeof(vms_name), "TN%d", (*tn_idx)++);
    } else {
        snprintf(vms_name, sizeof(vms_name), "XX%d", (*xx_idx)++);
    }

    return vms_name;
}

/*
 * Build a mapping table of Linux interface names → VMS device names.
 * Scans /sys/class/net/ to enumerate interfaces.
 */
#define TCPIP_MAX_IFACES 32

struct tcpip_ifmap {
    char linux_name[IFNAMSIZ];
    char vms_name[16];
};

static int tcpip_build_ifmap(struct tcpip_ifmap *map, int max_entries)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) return 0;

    /* First pass: collect interface names */
    char names[TCPIP_MAX_IFACES][IFNAMSIZ];
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < TCPIP_MAX_IFACES) {
        if (ent->d_name[0] == '.') continue;
        strncpy(names[count], ent->d_name, IFNAMSIZ - 1);
        names[count][IFNAMSIZ - 1] = '\0';
        count++;
    }
    closedir(d);

    /* Sort for stable ordering */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(names[i], names[j]) > 0) {
                char tmp[IFNAMSIZ];
                memcpy(tmp, names[i], IFNAMSIZ);
                memcpy(names[i], names[j], IFNAMSIZ);
                memcpy(names[j], tmp, IFNAMSIZ);
            }
        }
    }

    /* Map names */
    int se_idx = 0, ew_idx = 0, tn_idx = 0, xx_idx = 0;
    int n = (count < max_entries) ? count : max_entries;
    for (int i = 0; i < n; i++) {
        strncpy(map[i].linux_name, names[i], IFNAMSIZ - 1);
        map[i].linux_name[IFNAMSIZ - 1] = '\0';
        const char *vn = tcpip_map_interface(names[i],
                                              &se_idx, &ew_idx,
                                              &tn_idx, &xx_idx);
        strncpy(map[i].vms_name, vn, sizeof(map[i].vms_name) - 1);
        map[i].vms_name[sizeof(map[i].vms_name) - 1] = '\0';
    }
    return n;
}

/* Look up VMS name for a Linux interface name in the map */
static const char *tcpip_lookup_vms_name(const struct tcpip_ifmap *map,
                                          int count, const char *linux_name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(map[i].linux_name, linux_name) == 0)
            return map[i].vms_name;
    }
    return linux_name; /* fallback — should not happen */
}

/* Reverse lookup: VMS device name → Linux interface name */
static const char *tcpip_lookup_linux_name(const struct tcpip_ifmap *map,
                                            int count, const char *vms_name)
{
    for (int i = 0; i < count; i++) {
        if (strcasecmp(map[i].vms_name, vms_name) == 0)
            return map[i].linux_name;
    }
    return NULL;
}

/* Path for VMS TCPIP config files */
#define TCPIP_CONFIG_DIR "/vms/SYS0/SYSCOMMON/SYSEXE"
#define TCPIP_HOST_DAT    TCPIP_CONFIG_DIR "/TCPIP$HOST.DAT"
#define TCPIP_NS_DAT      TCPIP_CONFIG_DIR "/TCPIP$NAMESERVICE.DAT"
#define TCPIP_IF_DAT      TCPIP_CONFIG_DIR "/TCPIP$INTERFACE.DAT"
#define TCPIP_ROUTE_DAT   TCPIP_CONFIG_DIR "/TCPIP$ROUTE.DAT"

/*
 * TCPIP SHOW INTERFACE [/FULL] - Display network interfaces with VMS names.
 */
static int cmd_tcpip_show_interface(struct dcl_command *cmd)
{
    struct tcpip_ifmap ifmap[TCPIP_MAX_IFACES];
    int ifcount = tcpip_build_ifmap(ifmap, TCPIP_MAX_IFACES);

    int full = dcl_has_qualifier(cmd, "FULL");

    printf("\n");
    printf("%-12s%-17s%-17s%-7s%s\n",
           "Interface", "IP Address", "Network Mask", "MTU", "State");

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        printf("%%TCPIP-E-SOCKERR, cannot open socket\n");
        return SS$_BADPARAM;
    }

    for (int i = 0; i < ifcount; i++) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifmap[i].linux_name, IFNAMSIZ - 1);

        /* IP address */
        char ip_str[INET_ADDRSTRLEN] = "*";
        if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
            struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
            inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
        }

        /* Netmask */
        char mask_str[INET_ADDRSTRLEN] = "*";
        if (ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
            struct sockaddr_in *mask = (struct sockaddr_in *)&ifr.ifr_netmask;
            inet_ntop(AF_INET, &mask->sin_addr, mask_str, sizeof(mask_str));
        }

        /* MTU */
        int mtu = 0;
        if (ioctl(sock, SIOCGIFMTU, &ifr) == 0) {
            mtu = ifr.ifr_mtu;
        }

        /* Flags (up/down) */
        const char *state = "Down";
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
            if (ifr.ifr_flags & IFF_UP)
                state = "Up";
        }

        printf("%-12s%-17s%-17s%-7d%s\n",
               ifmap[i].vms_name, ip_str, mask_str, mtu, state);

        if (full) {
            /* Show hardware address */
            if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                unsigned char *hw = (unsigned char *)ifr.ifr_hwaddr.sa_data;
                printf("             Hardware address: %02X-%02X-%02X-%02X-%02X-%02X\n",
                       hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
            }
        }
    }

    close(sock);
    printf("\n");
    return SS$_NORMAL;
}

/*
 * TCPIP SHOW ROUTE - Display routing table with VMS device names.
 */
static int cmd_tcpip_show_route(struct dcl_command *cmd)
{
    (void)cmd;

    struct tcpip_ifmap ifmap[TCPIP_MAX_IFACES];
    int ifcount = tcpip_build_ifmap(ifmap, TCPIP_MAX_IFACES);

    FILE *fp = fopen("/proc/net/route", "r");
    if (!fp) {
        printf("%%TCPIP-E-NOROUTE, cannot read routing table\n");
        return SS$_BADPARAM;
    }

    printf("\n");
    printf("%-17s%-17s%-17s%s\n",
           "Destination", "Gateway", "Mask", "Interface");

    char line[256];
    /* Skip header */
    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return SS$_NORMAL;
    }

    while (fgets(line, sizeof(line), fp)) {
        char iface[IFNAMSIZ];
        unsigned long dest, gw, mask;
        unsigned int flags;
        int refs, use, metric, mtu, window, irtt;

        if (sscanf(line, "%s %lx %lx %x %d %d %d %lx %d %d %d",
                   iface, &dest, &gw, &flags, &refs, &use, &metric,
                   &mask, &mtu, &window, &irtt) < 8)
            continue;

        /* Skip non-UP routes */
        if (!(flags & 0x0001)) continue;

        /* Destination */
        char dest_str[32];
        if (dest == 0) {
            strcpy(dest_str, "default");
        } else {
            struct in_addr a;
            a.s_addr = (in_addr_t)dest;
            inet_ntop(AF_INET, &a, dest_str, sizeof(dest_str));
        }

        /* Gateway */
        char gw_str[32];
        if (gw == 0) {
            strcpy(gw_str, "*");
        } else {
            struct in_addr a;
            a.s_addr = (in_addr_t)gw;
            inet_ntop(AF_INET, &a, gw_str, sizeof(gw_str));
        }

        /* Mask */
        char mask_str[32];
        {
            struct in_addr a;
            a.s_addr = (in_addr_t)mask;
            inet_ntop(AF_INET, &a, mask_str, sizeof(mask_str));
        }

        /* VMS interface name */
        const char *vms_iface = tcpip_lookup_vms_name(ifmap, ifcount, iface);

        printf("%-17s%-17s%-17s%s\n", dest_str, gw_str, mask_str, vms_iface);
    }

    fclose(fp);
    printf("\n");
    return SS$_NORMAL;
}

/*
 * TCPIP SHOW HOST - Display host table entries.
 */
/* Track shown host entries to avoid duplicates across files */
#define TCPIP_MAX_HOST_ENTRIES 256

struct tcpip_host_entry {
    char addr[128];
    char name[256];
};

static int tcpip_host_already_shown(const struct tcpip_host_entry *shown,
                                     int count,
                                     const char *addr, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(shown[i].addr, addr) == 0 &&
            strcasecmp(shown[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static int tcpip_print_hosts_from_file(const char *path,
                                        struct tcpip_host_entry *shown,
                                        int count)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return count;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char addr[128], hostname[256];
        if (sscanf(p, "%127s %255s", addr, hostname) >= 2) {
            if (count < TCPIP_MAX_HOST_ENTRIES &&
                !tcpip_host_already_shown(shown, count, addr, hostname)) {
                printf("%-16s%s\n", addr, hostname);
                strncpy(shown[count].addr, addr, sizeof(shown[count].addr) - 1);
                strncpy(shown[count].name, hostname, sizeof(shown[count].name) - 1);
                count++;
            }
        }
    }
    fclose(fp);
    return count;
}

static int cmd_tcpip_show_host(struct dcl_command *cmd)
{
    (void)cmd;
    printf("\n");
    printf("%-16s%s\n", "Host address", "Host name");

    struct tcpip_host_entry *shown = calloc(TCPIP_MAX_HOST_ENTRIES,
                                             sizeof(struct tcpip_host_entry));
    int count = 0;
    if (shown) {
        count = tcpip_print_hosts_from_file("/etc/hosts", shown, count);
        tcpip_print_hosts_from_file(TCPIP_HOST_DAT, shown, count);
        free(shown);
    }

    printf("\n");
    return SS$_NORMAL;
}

/*
 * TCPIP SHOW VERSION - Display TCP/IP Services version.
 */
static int cmd_tcpip_show_version(struct dcl_command *cmd)
{
    (void)cmd;
    printf("OVMX TCP/IP Services for OpenVMS V0.1\n");
    return SS$_NORMAL;
}

/*
 * Ensure the TCPIP config directory exists.
 */
static void tcpip_ensure_config_dir(void)
{
    mkdir(TCPIP_CONFIG_DIR, 0755);
}

/*
 * TCPIP SET HOST hostname /ADDRESS=ip
 * Adds an entry to TCPIP$HOST.DAT and /etc/hosts.
 */
static int cmd_tcpip_set_host(struct dcl_command *cmd)
{
    if (cmd->param_count < 3) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing hostname - usage: TCPIP SET HOST name /ADDRESS=ip");
        return SS$_BADPARAM;
    }

    const char *hostname = cmd->params[2];
    const char *address = dcl_qualifier_value(cmd, "ADDRESS");

    if (!address) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing /ADDRESS qualifier");
        return SS$_BADPARAM;
    }

    /* Validate IP address */
    struct in_addr test_addr;
    if (inet_pton(AF_INET, address, &test_addr) != 1) {
        dcl_error("TCPIP", 2, "BADPARAM",
                  "invalid IP address - \\%s\\", address);
        return SS$_BADPARAM;
    }

    tcpip_ensure_config_dir();

    /* Write to TCPIP$HOST.DAT */
    FILE *fp = fopen(TCPIP_HOST_DAT, "a");
    if (fp) {
        fprintf(fp, "%-16s%s\n", address, hostname);
        fclose(fp);
    }

    /* Also append to /etc/hosts for Linux DNS resolution */
    fp = fopen("/etc/hosts", "a");
    if (fp) {
        fprintf(fp, "%-16s%s\n", address, hostname);
        fclose(fp);
    }

    printf("%%TCPIP-I-INFO, host \"%s\" added\n", hostname);
    return SS$_NORMAL;
}

/*
 * TCPIP SET NAME_SERVICE /SYSTEM /SERVER=ip [/DOMAIN=domain]
 * Configures DNS resolver.
 */
static int cmd_tcpip_set_name_service(struct dcl_command *cmd)
{
    const char *server = dcl_qualifier_value(cmd, "SERVER");

    if (!server) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing /SERVER qualifier");
        return SS$_BADPARAM;
    }

    /* Validate IP address */
    struct in_addr test_addr;
    if (inet_pton(AF_INET, server, &test_addr) != 1) {
        dcl_error("TCPIP", 2, "BADPARAM",
                  "invalid server IP address - \\%s\\", server);
        return SS$_BADPARAM;
    }

    const char *domain = dcl_qualifier_value(cmd, "DOMAIN");

    tcpip_ensure_config_dir();

    /* Write to TCPIP$NAMESERVICE.DAT */
    FILE *fp = fopen(TCPIP_NS_DAT, "w");
    if (fp) {
        fprintf(fp, "SERVER=%s\n", server);
        if (domain)
            fprintf(fp, "DOMAIN=%s\n", domain);
        fclose(fp);
    }

    /* Also write /etc/resolv.conf */
    fp = fopen("/etc/resolv.conf", "w");
    if (fp) {
        if (domain)
            fprintf(fp, "domain %s\n", domain);
        fprintf(fp, "nameserver %s\n", server);
        fclose(fp);
    }

    printf("%%TCPIP-I-INFO, name service configured\n");
    return SS$_NORMAL;
}

/*
 * TCPIP SET INTERFACE ifname /HOST=ip /NETWORK_MASK=mask
 * Configure a network interface (requires root/NET_ADMIN).
 */
static int cmd_tcpip_set_interface(struct dcl_command *cmd)
{
    if (cmd->param_count < 3) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing interface name - usage: TCPIP SET INTERFACE name /HOST=ip /NETWORK_MASK=mask");
        return SS$_BADPARAM;
    }

    const char *ifname = cmd->params[2];
    const char *host_ip = dcl_qualifier_value(cmd, "HOST");
    const char *netmask = dcl_qualifier_value(cmd, "NETWORK_MASK");

    if (!host_ip) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing /HOST qualifier");
        return SS$_BADPARAM;
    }

    /* Validate IP address */
    struct in_addr test_addr;
    if (inet_pton(AF_INET, host_ip, &test_addr) != 1) {
        dcl_error("TCPIP", 2, "BADPARAM",
                  "invalid IP address - \\%s\\", host_ip);
        return SS$_BADPARAM;
    }

    /* Map VMS device name to Linux interface */
    struct tcpip_ifmap ifmap[TCPIP_MAX_IFACES];
    int ifcount = tcpip_build_ifmap(ifmap, TCPIP_MAX_IFACES);
    const char *linux_if = tcpip_lookup_linux_name(ifmap, ifcount, ifname);

    if (!linux_if) {
        dcl_error("TCPIP", 2, "NOSUCHDEV",
                  "unknown interface - \\%s\\", ifname);
        return SS$_BADPARAM;
    }

    /* Check for root/NET_ADMIN privilege */
    if (geteuid() != 0) {
        printf("%%TCPIP-W-PRIVREQ, operation requires NET_ADMIN privilege\n");
        /* Still persist to config file */
    } else {
        /* Apply with ioctl */
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, linux_if, IFNAMSIZ - 1);

            /* Set IP address */
            struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
            addr->sin_family = AF_INET;
            inet_pton(AF_INET, host_ip, &addr->sin_addr);
            if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
                printf("%%TCPIP-W-IOERR, failed to set address: %s\n",
                       strerror(errno));
            }

            /* Set netmask if provided */
            if (netmask) {
                struct sockaddr_in *mask = (struct sockaddr_in *)&ifr.ifr_netmask;
                mask->sin_family = AF_INET;
                inet_pton(AF_INET, netmask, &mask->sin_addr);
                if (ioctl(sock, SIOCSIFNETMASK, &ifr) < 0) {
                    printf("%%TCPIP-W-IOERR, failed to set netmask: %s\n",
                           strerror(errno));
                }
            }

            close(sock);
        }
    }

    /* Persist to TCPIP$INTERFACE.DAT */
    tcpip_ensure_config_dir();
    FILE *fp = fopen(TCPIP_IF_DAT, "a");
    if (fp) {
        fprintf(fp, "%s %s", ifname, host_ip);
        if (netmask)
            fprintf(fp, " %s", netmask);
        fprintf(fp, "\n");
        fclose(fp);
    }

    printf("%%TCPIP-I-INFO, interface configured\n");
    return SS$_NORMAL;
}

/*
 * TCPIP SET ROUTE /GATEWAY=ip /DEFAULT
 *   or  /DESTINATION=dest /GATEWAY=gw /NETWORK_MASK=mask
 * Configure a network route (requires root/NET_ADMIN).
 */
static int cmd_tcpip_set_route(struct dcl_command *cmd)
{
    const char *gateway = dcl_qualifier_value(cmd, "GATEWAY");

    if (!gateway) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "missing /GATEWAY qualifier");
        return SS$_BADPARAM;
    }

    /* Validate gateway IP */
    struct in_addr test_addr;
    if (inet_pton(AF_INET, gateway, &test_addr) != 1) {
        dcl_error("TCPIP", 2, "BADPARAM",
                  "invalid gateway IP address - \\%s\\", gateway);
        return SS$_BADPARAM;
    }

    int is_default = dcl_has_qualifier(cmd, "DEFAULT");
    const char *destination = dcl_qualifier_value(cmd, "DESTINATION");
    const char *netmask = dcl_qualifier_value(cmd, "NETWORK_MASK");

    if (!is_default && !destination) {
        dcl_error("TCPIP", 2, "NOKEYW",
                  "specify /DEFAULT or /DESTINATION");
        return SS$_BADPARAM;
    }

    /* Check for root/NET_ADMIN privilege */
    if (geteuid() != 0) {
        printf("%%TCPIP-W-PRIVREQ, operation requires NET_ADMIN privilege\n");
        /* Still persist to config file */
    } else {
        /* Use ip route add command */
        char route_cmd[512];
        if (is_default) {
            snprintf(route_cmd, sizeof(route_cmd),
                     "ip route replace default via %s 2>/dev/null", gateway);
        } else {
            if (netmask) {
                /* Convert dotted netmask to CIDR prefix length */
                struct in_addr mask_addr;
                inet_pton(AF_INET, netmask, &mask_addr);
                uint32_t mask_val = ntohl(mask_addr.s_addr);
                int prefix = 0;
                while (mask_val & 0x80000000) {
                    prefix++;
                    mask_val <<= 1;
                }
                snprintf(route_cmd, sizeof(route_cmd),
                         "ip route replace %s/%d via %s 2>/dev/null",
                         destination, prefix, gateway);
            } else {
                snprintf(route_cmd, sizeof(route_cmd),
                         "ip route replace %s via %s 2>/dev/null",
                         destination, gateway);
            }
        }
        (void)system(route_cmd);
    }

    /* Persist to TCPIP$ROUTE.DAT */
    tcpip_ensure_config_dir();
    FILE *fp = fopen(TCPIP_ROUTE_DAT, "a");
    if (fp) {
        if (is_default) {
            fprintf(fp, "DEFAULT %s\n", gateway);
        } else {
            fprintf(fp, "%s %s", destination, gateway);
            if (netmask)
                fprintf(fp, " %s", netmask);
            fprintf(fp, "\n");
        }
        fclose(fp);
    }

    printf("%%TCPIP-I-INFO, route added\n");
    return SS$_NORMAL;
}

/*
 * TCPIP - TCP/IP Services command with SHOW and SET subcommands.
 */
static int cmd_tcpip(struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("TCPIP", 2, "NOKEYW", "missing keyword - supply a TCPIP subcommand");
        return SS$_BADPARAM;
    }

    const char *subcmd = cmd->params[0];

    if (dcl_match_command(subcmd, "SHOW", 2)) {
        /* TCPIP SHOW <what> */
        if (cmd->param_count < 2) {
            dcl_error("TCPIP", 2, "NOKEYW",
                      "missing SHOW keyword - supply what you want to show");
            return SS$_BADPARAM;
        }

        const char *what = cmd->params[1];

        if (dcl_match_command(what, "INTERFACE", 3))
            return cmd_tcpip_show_interface(cmd);
        if (dcl_match_command(what, "ROUTE", 3))
            return cmd_tcpip_show_route(cmd);
        if (dcl_match_command(what, "HOST", 3))
            return cmd_tcpip_show_host(cmd);
        if (dcl_match_command(what, "VERSION", 3))
            return cmd_tcpip_show_version(cmd);

        dcl_error("TCPIP", 2, "IVKEYW",
                  "unrecognized TCPIP SHOW keyword - \\%s\\", what);
        return SS$_IVKEYW;
    }

    if (dcl_match_command(subcmd, "SET", 2)) {
        /* TCPIP SET <what> */
        if (cmd->param_count < 2) {
            dcl_error("TCPIP", 2, "NOKEYW",
                      "missing SET keyword - supply what you want to set");
            return SS$_BADPARAM;
        }

        const char *what = cmd->params[1];

        if (dcl_match_command(what, "HOST", 3))
            return cmd_tcpip_set_host(cmd);
        if (dcl_match_command(what, "NAME_SERVICE", 4))
            return cmd_tcpip_set_name_service(cmd);
        if (dcl_match_command(what, "INTERFACE", 3))
            return cmd_tcpip_set_interface(cmd);
        if (dcl_match_command(what, "ROUTE", 3))
            return cmd_tcpip_set_route(cmd);

        dcl_error("TCPIP", 2, "IVKEYW",
                  "unrecognized TCPIP SET keyword - \\%s\\", what);
        return SS$_IVKEYW;
    }

    dcl_error("TCPIP", 2, "IVKEYW",
              "unrecognized TCPIP keyword - \\%s\\", subcmd);
    return SS$_IVKEYW;
}

/* ================================================================== */
/*                    MOUNT / DISMOUNT Commands                        */
/* ================================================================== */

/*
 * MOUNT - Mount a VMS device (virtual mapping to a directory).
 *
 * Syntax: MOUNT device: label [/SYSTEM]
 */
static int cmd_mount(struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("MOUNT", 2, "NODEVICE",
                  "no device specified");
        return SS$_BADPARAM;
    }

    const char *device = cmd->params[0];

    /* Validate device name */
    size_t dlen = strlen(device);
    if (dlen < 2) {
        dcl_error("MOUNT", 2, "IVDEVNAM",
                  "invalid device name - \\%s\\", device);
        return SS$_IVDEVNAM;
    }

    /* Build canonical device name (uppercase, with colon) */
    char dev_name[16];
    size_t nlen = dlen;
    if (nlen >= sizeof(dev_name) - 1) nlen = sizeof(dev_name) - 2;
    for (size_t i = 0; i < nlen; i++)
        dev_name[i] = (char)toupper((unsigned char)device[i]);
    dev_name[nlen] = '\0';
    if (dev_name[nlen - 1] != ':') {
        dev_name[nlen] = ':';
        dev_name[nlen + 1] = '\0';
    }

    /* Check if already mounted */
    struct vms_device *existing = vms_find_device(dev_name);
    if (existing && existing->mounted) {
        dcl_error("MOUNT", 2, "DEVMOUNT",
                  "device already mounted - _%s", dev_name);
        return SS$_DEVMOUNT;
    }

    /* Get volume label */
    char label[16] = "OVMX";
    if (cmd->param_count >= 2) {
        size_t llen = strlen(cmd->params[1]);
        if (llen >= sizeof(label)) llen = sizeof(label) - 1;
        for (size_t i = 0; i < llen; i++)
            label[i] = (char)toupper((unsigned char)cmd->params[1][i]);
        label[llen] = '\0';
    }

    /* Use current directory as mount path */
    char linux_path[256];
    if (!getcwd(linux_path, sizeof(linux_path))) {
        strncpy(linux_path, "/", sizeof(linux_path) - 1);
        linux_path[sizeof(linux_path) - 1] = '\0';
    }

    /* Add or update device table entry */
    struct vms_device *dev = existing;
    if (!dev) {
        if (vms_device_count >= VMS_MAX_DEVICES) {
            dcl_error("MOUNT", 2, "DEVFULL",
                      "device table full");
            return SS$_DEVALLOC;
        }
        dev = &vms_device_table[vms_device_count++];
    }
    strncpy(dev->vms_name, dev_name, sizeof(dev->vms_name) - 1);
    dev->vms_name[sizeof(dev->vms_name) - 1] = '\0';
    strncpy(dev->linux_path, linux_path, sizeof(dev->linux_path) - 1);
    dev->linux_path[sizeof(dev->linux_path) - 1] = '\0';
    strncpy(dev->volume_label, label, sizeof(dev->volume_label) - 1);
    dev->volume_label[sizeof(dev->volume_label) - 1] = '\0';
    dev->mounted = 1;

    /* Create logical name for device -> linux path */
    const char *table = LNM_PROCESS_TABLE;
    if (dcl_has_qualifier(cmd, "SYSTEM"))
        table = LNM_SYSTEM_TABLE;

    /* Strip trailing colon for logical name */
    char log_name[16];
    strncpy(log_name, dev_name, sizeof(log_name) - 1);
    log_name[sizeof(log_name) - 1] = '\0';
    size_t lnlen = strlen(log_name);
    if (lnlen > 0 && log_name[lnlen - 1] == ':')
        log_name[lnlen - 1] = '\0';

    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        lnm_create(mgr, table, log_name, linux_path,
                   LNM_ATTR_TERMINAL, LNM_MODE_USER);
    }

    printf("%%MOUNT-I-MOUNTED, %s mounted on _%s\n", label, dev_name);
    return SS$_NORMAL;
}

/*
 * DISMOUNT - Dismount a VMS device.
 *
 * Syntax: DISMOUNT device:
 */
static int cmd_dismount(struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("DISMOUNT", 2, "NODEVICE",
                  "no device specified");
        return SS$_BADPARAM;
    }

    const char *device = cmd->params[0];

    /* Build canonical device name */
    char dev_name[16];
    size_t nlen = strlen(device);
    if (nlen >= sizeof(dev_name) - 1) nlen = sizeof(dev_name) - 2;
    for (size_t i = 0; i < nlen; i++)
        dev_name[i] = (char)toupper((unsigned char)device[i]);
    dev_name[nlen] = '\0';
    if (dev_name[nlen - 1] != ':') {
        dev_name[nlen] = ':';
        dev_name[nlen + 1] = '\0';
    }

    struct vms_device *dev = vms_find_device(dev_name);
    if (!dev || !dev->mounted) {
        dcl_error("DISMOUNT", 2, "DEVNOTMNT",
                  "device is not mounted - _%s", dev_name);
        return SS$_DEVNOTMOUNT;
    }

    dev->mounted = 0;

    /* Remove logical name */
    char log_name[16];
    strncpy(log_name, dev_name, sizeof(log_name) - 1);
    log_name[sizeof(log_name) - 1] = '\0';
    size_t lnlen = strlen(log_name);
    if (lnlen > 0 && log_name[lnlen - 1] == ':')
        log_name[lnlen - 1] = '\0';

    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        lnm_delete(mgr, LNM_PROCESS_TABLE, log_name, LNM_MODE_USER);
        lnm_delete(mgr, LNM_SYSTEM_TABLE, log_name, LNM_MODE_USER);
    }

    printf("%%DISMOUNT-I-DISMOUNTED, _%s dismounted\n", dev_name);
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     CONTINUE Command                                */
/* ================================================================== */

/*
 * CONTINUE - Resume an interrupted process (stopped by Ctrl-Y).
 *
 * On VMS, Ctrl-Y interrupts the currently running image and returns
 * control to the DCL prompt.  CONTINUE resumes execution of the
 * interrupted image.  If no image is interrupted, an informational
 * message is displayed.
 */
static int cmd_continue(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();

    if (ctx->interrupted_pid <= 0) {
        printf("%%DCL-I-NOINTER, no interrupted image to continue\n");
        return SS$_NORMAL;
    }

    pid_t pid = ctx->interrupted_pid;

    /* Send SIGCONT to resume the stopped child */
    if (kill(pid, SIGCONT) != 0) {
        printf("%%DCL-W-NOPROC, process %d no longer exists\n", (int)pid);
        ctx->interrupted_pid = 0;
        return SS$_NONEXPR;
    }

    /* Wait for the child to finish (or be stopped again by another Ctrl-Y) */
    int wstatus;
    pid_t result;
    while ((result = waitpid(pid, &wstatus, WUNTRACED)) < 0 && errno == EINTR)
        ;

    if (result > 0 && WIFSTOPPED(wstatus)) {
        /* Child was stopped again (another Ctrl-Y during CONTINUE) */
        printf("\nInterrupt\n");
        /* Still interrupted — keep interrupted_pid */
    } else {
        /* Child exited or was signaled */
        ctx->interrupted_pid = 0;
    }

    return SS$_NORMAL;
}

/* ================================================================== */
/*                     EDIT Command                                    */
/* ================================================================== */

/* External EDT editor entry point (dcl_editor.c) */
extern int edt_run(const char *filepath);

/*
 * EDIT - Launch EDT line-mode editor on a file.
 *
 * Format: EDIT filespec
 */
static int cmd_edit(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("EDIT", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    /* Resolve filespec to Linux path (file may not exist yet) */
    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    return edt_run(linux_path);
}

/* ================================================================== */
/*                     ATTACH Command                                  */
/* ================================================================== */

/*
 * ATTACH - Transfer terminal control to another process.
 *
 * ATTACH [process-name]    — switch to named subprocess
 * ATTACH /ID=hex-pid       — switch by PID
 *
 * For now, uses the interrupted_pid from Ctrl-Y if available.
 */
static int cmd_attach(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* Check /ID=hex-pid qualifier */
    const char *id_val = dcl_qualifier_value(cmd, "ID");
    if (id_val && id_val[0]) {
        pid_t target = (pid_t)strtol(id_val, NULL, 16);
        if (target <= 0) {
            dcl_error("DCL", 4, "ATTFAIL", "invalid process id - %s", id_val);
            return SS$_BADPARAM;
        }
        /* Check if process exists */
        if (kill(target, 0) != 0) {
            dcl_error("DCL", 4, "ATTFAIL", "no such process");
            return SS$_NONEXPR;
        }
        /* Send SIGCONT and wait */
        kill(target, SIGCONT);
        extern volatile sig_atomic_t dcl_running_child;
        dcl_running_child = (sig_atomic_t)target;
        int wstatus;
        waitpid(target, &wstatus, WUNTRACED);
        dcl_running_child = 0;
        if (WIFSTOPPED(wstatus)) {
            printf("\nInterrupt\n");
            ctx->interrupted_pid = target;
            return SS$_ABORT;
        }
        ctx->interrupted_pid = 0;
        return SS$_NORMAL;
    }

    /* Check for process-name parameter */
    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        /* Named process attach — for now, only support interrupted process */
        if (ctx->interrupted_pid > 0) {
            pid_t pid = ctx->interrupted_pid;
            if (kill(pid, SIGCONT) != 0) {
                dcl_error("DCL", 4, "ATTFAIL", "no such process");
                ctx->interrupted_pid = 0;
                return SS$_NONEXPR;
            }
            extern volatile sig_atomic_t dcl_running_child;
            dcl_running_child = (sig_atomic_t)pid;
            int wstatus;
            waitpid(pid, &wstatus, WUNTRACED);
            dcl_running_child = 0;
            if (WIFSTOPPED(wstatus)) {
                printf("\nInterrupt\n");
                /* Keep interrupted_pid */
            } else {
                ctx->interrupted_pid = 0;
            }
            return SS$_NORMAL;
        }
        dcl_error("DCL", 4, "ATTFAIL", "no such process");
        return SS$_NONEXPR;
    }

    /* No parameter and no /ID — try interrupted process */
    if (ctx->interrupted_pid > 0) {
        pid_t pid = ctx->interrupted_pid;
        if (kill(pid, SIGCONT) != 0) {
            dcl_error("DCL", 4, "ATTFAIL", "no such process");
            ctx->interrupted_pid = 0;
            return SS$_NONEXPR;
        }
        extern volatile sig_atomic_t dcl_running_child;
        dcl_running_child = (sig_atomic_t)pid;
        int wstatus;
        waitpid(pid, &wstatus, WUNTRACED);
        dcl_running_child = 0;
        if (WIFSTOPPED(wstatus)) {
            printf("\nInterrupt\n");
        } else {
            ctx->interrupted_pid = 0;
        }
        return SS$_NORMAL;
    }

    dcl_error("DCL", 4, "ATTFAIL", "no process specified");
    return SS$_BADPARAM;
}

/* ================================================================== */
/*                     CONVERT Command                                 */
/* ================================================================== */

/*
 * CONVERT - Convert file format (basic file copy with record counting).
 *
 * CONVERT input-file output-file
 * /FDL=fdl-file — accepted but ignored with informational message
 */
static int cmd_convert(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("CONVERT", 2, "NOINPFIL", "missing input and/or output file");
        return SS$_BADPARAM;
    }

    /* Check /FDL qualifier — accepted but not implemented */
    if (dcl_has_qualifier(cmd, "FDL")) {
        printf("%%CONVERT-I-FDL, /FDL qualifier accepted but ignored in this implementation\n");
    }

    char src_path[1024], dst_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], src_path, sizeof(src_path));
    dcl_resolve_path(ctx, cmd->params[1], dst_path, sizeof(dst_path));

    FILE *src = fopen(src_path, "r");
    if (!src) {
        dcl_error("CONVERT", 2, "OPENIN", "error opening %s as input", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    FILE *dst = fopen(dst_path, "w");
    if (!dst) {
        fclose(src);
        dcl_error("CONVERT", 2, "OPENOUT", "error opening %s as output", cmd->params[1]);
        return SS$_FILACCERR;
    }

    /* Copy line by line, counting records */
    char line[4096];
    long records = 0;
    while (fgets(line, sizeof(line), src)) {
        fputs(line, dst);
        records++;
    }

    fclose(src);
    fclose(dst);

    printf("%%CONVERT-S-CONVERTED, %ld records converted\n", records);
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     INSTALL Command                                 */
/* ================================================================== */

/*
 * INSTALL - Manage known image list.
 *
 * INSTALL ADD image [/SHARED] [/OPEN] [/HEADER_RESIDENT]
 * INSTALL LIST [/FULL]
 * INSTALL REMOVE image
 *
 * Maintains list in /vms/SYS0/SYSCOMMON/SYSMGR/INSTALL_LIST.DAT
 */

#define INSTALL_LIST_PATH "/vms/SYS0/SYSCOMMON/SYSMGR/INSTALL_LIST.DAT"

static int cmd_install(struct dcl_command *cmd)
{
    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("INSTALL", 2, "NOCMD", "missing subcommand (ADD, LIST, or REMOVE)");
        return SS$_BADPARAM;
    }

    char subcmd[32];
    strncpy(subcmd, cmd->params[0], sizeof(subcmd) - 1);
    subcmd[sizeof(subcmd) - 1] = '\0';
    for (int i = 0; subcmd[i]; i++)
        subcmd[i] = (char)toupper((unsigned char)subcmd[i]);

    if (strcmp(subcmd, "ADD") == 0) {
        if (cmd->param_count < 2 || cmd->params[1][0] == '\0') {
            dcl_error("INSTALL", 2, "NOIMAGE", "missing image name");
            return SS$_BADPARAM;
        }

        /* Build flags string */
        char flags[128] = {0};
        if (dcl_has_qualifier(cmd, "OPEN"))
            strncat(flags, " Open", sizeof(flags) - strlen(flags) - 1);
        if (dcl_has_qualifier(cmd, "HEADER_RESIDENT"))
            strncat(flags, " Hdr", sizeof(flags) - strlen(flags) - 1);
        if (dcl_has_qualifier(cmd, "SHARED"))
            strncat(flags, " Shared", sizeof(flags) - strlen(flags) - 1);

        /* Ensure directory exists */
        mkdir("/vms/SYS0/SYSCOMMON/SYSMGR", 0755);

        /* Append to install list */
        FILE *fp = fopen(INSTALL_LIST_PATH, "a");
        if (!fp) {
            dcl_error("INSTALL", 2, "OPENERR", "cannot open install list");
            return SS$_FILACCERR;
        }

        /* Convert image name to uppercase */
        char image_upper[256];
        strncpy(image_upper, cmd->params[1], sizeof(image_upper) - 1);
        image_upper[sizeof(image_upper) - 1] = '\0';
        for (int i = 0; image_upper[i]; i++)
            image_upper[i] = (char)toupper((unsigned char)image_upper[i]);

        fprintf(fp, "%s%s\n", image_upper, flags);
        fclose(fp);

        printf("%%INSTALL-I-ADDED, %s added to known image list\n", image_upper);
        return SS$_NORMAL;

    } else if (strcmp(subcmd, "LIST") == 0) {
        FILE *fp = fopen(INSTALL_LIST_PATH, "r");
        if (!fp) {
            printf("%%INSTALL-I-NOIMAGES, no known images installed\n");
            return SS$_NORMAL;
        }

        int full = dcl_has_qualifier(cmd, "FULL");
        printf("\nINSTALL - Known Image List\n");
        if (full) {
            printf("%-50s %s\n", "Image Name", "Attributes");
            printf("%-50s %s\n",
                   "--------------------------------------------------",
                   "----------");
        }

        char line[512];
        int count = 0;
        while (fgets(line, sizeof(line), fp)) {
            /* Strip newline */
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n')
                line[--len] = '\0';
            if (len == 0) continue;

            /* Parse: "IMAGE FLAGS" */
            char *space = strchr(line, ' ');
            if (full && space) {
                char image[256];
                strncpy(image, line, (size_t)(space - line));
                image[space - line] = '\0';
                printf("DISK$SYSTEM:[SYSEXE]%s;1  %s\n", image, space + 1);
            } else if (space) {
                char image[256];
                strncpy(image, line, (size_t)(space - line));
                image[space - line] = '\0';
                printf("DISK$SYSTEM:[SYSEXE]%s;1  %s\n", image, space + 1);
            } else {
                printf("DISK$SYSTEM:[SYSEXE]%s;1\n", line);
            }
            count++;
        }
        fclose(fp);

        printf("\n%d known image%s\n", count, count != 1 ? "s" : "");
        return SS$_NORMAL;

    } else if (strcmp(subcmd, "REMOVE") == 0) {
        if (cmd->param_count < 2 || cmd->params[1][0] == '\0') {
            dcl_error("INSTALL", 2, "NOIMAGE", "missing image name");
            return SS$_BADPARAM;
        }

        char target[256];
        strncpy(target, cmd->params[1], sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
        for (int i = 0; target[i]; i++)
            target[i] = (char)toupper((unsigned char)target[i]);

        FILE *fp = fopen(INSTALL_LIST_PATH, "r");
        if (!fp) {
            dcl_error("INSTALL", 2, "NOTKNOWN", "image %s is not a known image", target);
            return SS$_NOSUCHFILE;
        }

        /* Read all lines, write back those that don't match */
        char lines[256][512];
        int count = 0;
        int found = 0;
        char line[512];
        while (fgets(line, sizeof(line), fp) && count < 256) {
            /* Check if this line starts with target */
            size_t tlen = strlen(target);
            if (strncasecmp(line, target, tlen) == 0 &&
                (line[tlen] == ' ' || line[tlen] == '\n' || line[tlen] == '\0')) {
                found = 1;
                continue;
            }
            strncpy(lines[count], line, sizeof(lines[count]) - 1);
            lines[count][sizeof(lines[count]) - 1] = '\0';
            count++;
        }
        fclose(fp);

        if (!found) {
            dcl_error("INSTALL", 2, "NOTKNOWN", "image %s is not a known image", target);
            return SS$_NOSUCHFILE;
        }

        fp = fopen(INSTALL_LIST_PATH, "w");
        if (fp) {
            for (int i = 0; i < count; i++)
                fputs(lines[i], fp);
            fclose(fp);
        }

        printf("%%INSTALL-I-REMOVED, %s removed from known image list\n", target);
        return SS$_NORMAL;

    } else {
        dcl_error("INSTALL", 2, "INVCMD", "invalid subcommand - %s", subcmd);
        return SS$_BADPARAM;
    }
}

/* ================================================================== */
/*                     LINK Command                                    */
/* ================================================================== */

/*
 * LINK - Link object modules into an executable image.
 *
 * LINK file1[,file2,...]
 * /EXECUTABLE=name — output executable name
 * /MAP — produce link map
 *
 * Wraps the system linker (cc).
 */
static int cmd_link(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("LINK", 2, "NOFILES", "no input files specified");
        return SS$_BADPARAM;
    }

    /* Collect input files — params may be comma-separated */
    char *input_files[64];
    int input_count = 0;
    char resolved[64][1024];

    for (int i = 0; i < cmd->param_count && input_count < 64; i++) {
        if (cmd->params[i][0] == '\0') continue;

        /* Split on commas */
        char temp[DCL_MAX_LINE];
        strncpy(temp, cmd->params[i], sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';

        char *tok = strtok(temp, ",");
        while (tok && input_count < 64) {
            while (*tok == ' ') tok++;
            if (*tok) {
                dcl_resolve_path(ctx, tok, resolved[input_count],
                                 sizeof(resolved[input_count]));
                input_files[input_count] = resolved[input_count];
                input_count++;
            }
            tok = strtok(NULL, ",");
        }
    }

    if (input_count == 0) {
        dcl_error("LINK", 2, "NOFILES", "no input files specified");
        return SS$_BADPARAM;
    }

    /* Determine output name */
    char output_name[1024];
    const char *exe_val = dcl_qualifier_value(cmd, "EXECUTABLE");
    if (exe_val && exe_val[0]) {
        dcl_resolve_path(ctx, exe_val, output_name, sizeof(output_name));
    } else {
        /* Default: first input name without extension + .EXE */
        strncpy(output_name, input_files[0], sizeof(output_name) - 1);
        output_name[sizeof(output_name) - 1] = '\0';
        char *dot = strrchr(output_name, '.');
        if (dot) *dot = '\0';
        strncat(output_name, ".EXE", sizeof(output_name) - strlen(output_name) - 1);
    }

    /* Check /MAP qualifier */
    int want_map = dcl_has_qualifier(cmd, "MAP");
    char map_name[1024] = {0};
    if (want_map) {
        strncpy(map_name, output_name, sizeof(map_name) - 1);
        map_name[sizeof(map_name) - 1] = '\0';
        char *dot = strrchr(map_name, '.');
        if (dot) *dot = '\0';
        strncat(map_name, ".MAP", sizeof(map_name) - strlen(map_name) - 1);
    }

    /* Print linking message */
    printf("%%LINK-I-LINK, linking %s...\n", output_name);

    /* Build cc command: cc -o output input1 input2 ... [-Wl,-Map,mapfile] */
    char *argv[128];
    int argc = 0;
    argv[argc++] = "cc";
    argv[argc++] = "-o";
    argv[argc++] = output_name;
    for (int i = 0; i < input_count && argc < 120; i++)
        argv[argc++] = input_files[i];
    if (want_map) {
        static char map_flag[1100];
        snprintf(map_flag, sizeof(map_flag), "-Wl,-Map,%s", map_name);
        argv[argc++] = map_flag;
    }
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        execvp("cc", argv);
        _exit(127);
    } else if (pid > 0) {
        int wstatus;
        waitpid(pid, &wstatus, 0);
        if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
            printf("%%LINK-I-DONE, %s linked successfully\n", output_name);
            return SS$_NORMAL;
        } else {
            dcl_error("LINK", 2, "FAILED", "error linking image");
            return SS$_ABORT;
        }
    } else {
        dcl_error("LINK", 4, "CREPRC", "cannot create linker process");
        return SS$_ABORT;
    }
}

/* ================================================================== */
/*                     PHONE Command                                   */
/* ================================================================== */

/*
 * PHONE - Phone utility for interactive conversation.
 * Not available in OVMX — stub only.
 */
static int cmd_phone(struct dcl_command *cmd)
{
    (void)cmd;
    dcl_error("PHONE", 0, "NOTAVAIL", "PHONE facility is not available");
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     PRODUCT Command                                 */
/* ================================================================== */

/*
 * PRODUCT - Software product management (minimal PCSI emulation).
 *
 * PRODUCT SHOW PRODUCT — list installed products
 * PRODUCT SHOW HISTORY — show installation date
 */
static int cmd_product(struct dcl_command *cmd)
{
    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("PCSI", 0, "NOTIMPL", "operation not implemented");
        return SS$_NORMAL;
    }

    char subcmd[32];
    strncpy(subcmd, cmd->params[0], sizeof(subcmd) - 1);
    subcmd[sizeof(subcmd) - 1] = '\0';
    for (int i = 0; subcmd[i]; i++)
        subcmd[i] = (char)toupper((unsigned char)subcmd[i]);

    if (strcmp(subcmd, "SHOW") != 0) {
        dcl_error("PCSI", 0, "NOTIMPL", "operation not implemented");
        return SS$_NORMAL;
    }

    /* Need a second parameter for SHOW sub-subcommand */
    if (cmd->param_count < 2 || cmd->params[1][0] == '\0') {
        dcl_error("PCSI", 0, "NOTIMPL", "operation not implemented");
        return SS$_NORMAL;
    }

    char showwhat[32];
    strncpy(showwhat, cmd->params[1], sizeof(showwhat) - 1);
    showwhat[sizeof(showwhat) - 1] = '\0';
    for (int i = 0; showwhat[i]; i++)
        showwhat[i] = (char)toupper((unsigned char)showwhat[i]);

    if (strcmp(showwhat, "PRODUCT") == 0) {
        printf("----------------------------------- ----------- -----------\n");
        printf("PRODUCT                             KIT TYPE    STATE\n");
        printf("----------------------------------- ----------- -----------\n");
        printf("OVMX V1.0                          Full LP     Installed\n");
        printf("----------------------------------- ----------- -----------\n");
        printf("1 product found\n");
        return SS$_NORMAL;
    } else if (strcmp(showwhat, "HISTORY") == 0) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        printf("----------------------------------- ----------- ----------- -----------\n");
        printf("PRODUCT                             KIT TYPE    STATE       DATE\n");
        printf("----------------------------------- ----------- ----------- -----------\n");
        printf("OVMX V1.0                          Full LP     Installed   %2d-%s-%04d\n",
               tm->tm_mday, vms_months[tm->tm_mon], 1900 + tm->tm_year);
        printf("----------------------------------- ----------- ----------- -----------\n");
        printf("1 item found\n");
        return SS$_NORMAL;
    } else {
        dcl_error("PCSI", 0, "NOTIMPL", "operation not implemented");
        return SS$_NORMAL;
    }
}

/* ================================================================== */
/*                     Command Table                                   */
/* ================================================================== */

static struct dcl_verb builtin_verbs[] = {
    { "ACCOUNTING",  cmd_accounting,  CDU_F_ABBREV | CDU_F_QUALIFIER, 4,
      "Display login accounting information for the current user" },
    { "ANALYZE",     cmd_analyze,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Analyze system components" },
    { "APPEND",      cmd_append,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Append source file to destination file" },
    { "ASSIGN",      cmd_assign,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Assign a logical name (equivalence name to logical name)" },
    { "ATTACH",      cmd_attach,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Transfer terminal control to another process" },
    { "BACKUP",      cmd_backup,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Create, restore, or list a saveset file" },
    { "CLOSE",       cmd_close,       CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Close a file that was opened for I/O" },
    { "CONTINUE",    cmd_continue,    CDU_F_ABBREV, 4,
      "Resume execution of an interrupted image" },
    { "CONVERT",     cmd_convert,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Convert file format" },
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
    { "DISMOUNT",    cmd_dismount,    CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Dismount a volume from a device" },
    { "DUMP",        cmd_dump,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display contents of a file in hexadecimal and ASCII" },
    { "EDIT",        cmd_edit,        CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Invoke the EDT text editor" },
    { "EXIT",        cmd_exit,        CDU_F_ABBREV, 2,
      "Terminate a command procedure or session" },
    { "HELP",        cmd_help,        CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Obtain information about DCL commands" },
    { "INQUIRE",     cmd_inquire,     CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Read input from SYS$INPUT and assign to a symbol" },
    { "INSTALL",     cmd_install,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Manage known images" },
    { "LIBRARY",     cmd_library,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Manage text, help, and object libraries" },
    { "LINK",        cmd_link,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Link object modules into executable image" },
    { "LOGOUT",      cmd_logout,      CDU_F_ABBREV, 2,
      "Terminate an interactive session" },
    { "MAIL",      cmd_mail,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Send and receive electronic mail messages" },
    { "MONITOR",   cmd_monitor,   CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Display real-time system activity statistics" },
    { "MOUNT",       cmd_mount,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Mount a volume on a device" },
    { "OPEN",        cmd_open,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Open a file for reading or writing" },
    { "PHONE",       cmd_phone,       CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Phone utility for interactive conversation" },
    { "PIPE",        cmd_pipe,        CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Execute a DCL pipeline (cmd1 | cmd2 | ...)" },
    { "PRINT",       cmd_print,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Queue a file for printing" },
    { "PRODUCT",     cmd_product,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Software product management" },
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
    { "SPAWN",       cmd_spawn,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Create a subprocess" },
    { "STOP",        cmd_stop,        CDU_F_ABBREV, 2,
      "Stop the current process" },
    { "SUBMIT",      cmd_submit,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Submit a command procedure to a batch queue" },
    { "SYSGEN",      cmd_sysgen,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Invoke SYSGEN system parameter utility" },
    { "SYSMAN",      cmd_sysman,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Invoke SYSMAN system management utility" },
    { "TCPIP",       cmd_tcpip,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "TCP/IP Services network management commands" },
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
