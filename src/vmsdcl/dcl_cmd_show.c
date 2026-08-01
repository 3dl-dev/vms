/*
 * dcl_cmd_show.c - DCL SHOW command implementations
 *
 * All cmd_show_* functions and the SHOW dispatcher.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>
#include <mntent.h>

#include "dcl/context.h"
#include "dcl/terminal.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/cdu.h"
#include "dcl/dcl_cmd.h"
#include "ssdef.h"
#include "vms/logical.h"
#include "vms/privs.h"
#include "starlet.h"
#include "vmsfs/filespec.h"
#include "vms/pcb.h"
#include "ovmx_identity.h"
#include "vmsqueue.h"

/* Forward declarations for queue/intrusion subcommands (dcl_cmd_process.c) */
extern int cmd_show_queue(struct dcl_command *cmd);
extern int cmd_show_entry(struct dcl_command *cmd);
extern int cmd_show_intrusion(struct dcl_command *cmd);

/* Forward declarations for helper functions used by cmd_show_process */
static int cmd_show_process_privileges(struct dcl_context *ctx);
static int cmd_show_process_quotas(struct dcl_context *ctx);

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

    /* Human surface: badged brand identity + SCSNODE (INV-0/INV-1). Never a
     * bare VSI product-and-version claim, and never the Linux hostname
     * (INV-4 leak). */
    char sysname[OVMX_IDENTITY_MAXLEN];
    ovmx_node_name(sysname, sizeof(sysname));

    printf("%s  on node %s  %2d-%s-%04d %02d:%02d:%02d.%02d"
           "  Uptime  %s\n",
           ovmx_product_banner(), sysname, tm.tm_mday, vms_months[tm.tm_mon],
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

    /* /ALL qualifier: show all processes (list from /proc) */
    if (dcl_has_qualifier(cmd, "ALL")) {
        struct timespec ats;
        clock_gettime(CLOCK_REALTIME, &ats);
        struct tm atm;
        localtime_r(&ats.tv_sec, &atm);
        printf("      Processes at %2d-%s-%04d %02d:%02d:%02d.%02d\n",
               atm.tm_mday, vms_months[atm.tm_mon], 1900 + atm.tm_year,
               atm.tm_hour, atm.tm_min, atm.tm_sec,
               (int)(ats.tv_nsec / 10000000));
        printf("    %-20s %-10s %-8s %s\n", "Process Name", "PID", "UIC", "State");
        /* Show at least the current process */
        const char *pname = ctx->process_name[0] ? ctx->process_name : "_FTA0:";
        const char *uname = ctx->username[0] ? ctx->username : "SYSTEM";
        printf("    %-20s %08X   [%03o,%03o] LEF\n",
               pname, (unsigned)getpid(),
               ctx->uic_group ? (unsigned)ctx->uic_group : (unsigned)(getgid() & 0377),
               ctx->uic_member ? (unsigned)ctx->uic_member : (unsigned)(getuid() & 0377));
        (void)uname;
        return SS$_NORMAL;
    }

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
    printf("OVMX                  OVMX      0      0      100   %-8s (none)\n",
           ovmx_product_version());
    printf("OVMX-TCP/IP           OVMX      0      0      100   %-8s (none)\n",
           ovmx_product_version());
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
    char netnode[OVMX_IDENTITY_MAXLEN];
    ovmx_node_name(netnode, sizeof(netnode));
    printf("Product: OVMX TCP/IP Services %s\n", ovmx_product_version());
    printf("Node: %s\n", netnode);
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

int cmd_show(struct dcl_command *cmd)
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
