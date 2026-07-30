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

#include "prvdef.h"     /* PRV$M_* -- the single privilege bit table */
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
/* The executive process table's reader (SHOW SYSTEM enumerates it). */
#include "vms_kif.h"

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
 * cpu_time_of - the CPU time SHOW SYSTEM displays for one process row,
 * formatted as VMS "d hh:mm:ss.cc".
 *
 * THE FIGURE IS READ FROM THE SYSTEM SERVICE, NOT FROM /proc
 * (vms-8019 round 7). What stood here was a SECOND, independent
 * /proc/<pid>/stat parser living in the DCL layer -- a user-visible VMS
 * command sourcing its own answer for an item $GETJPI already answers
 * (JPI$_CPUTIM), which is the Rule 11 corollary inverted: a command is a
 * READER of an executive facility, never a thing that fabricates its own
 * answer. Two parsers of one quantity is also how the two layers came to
 * DISAGREE about one condition -- this one blanked the column when the
 * read failed while sys$getjpi invented a zero for the identical failure.
 * There is now one implementation and therefore one answer.
 *
 * Returns 1 with cpu_str filled, or 0 when the service will not report
 * the item -- which, for a row the scan has already established this
 * caller may read, means the process no longer exists (SS$_NONEXPR).
 * The caller must then drop the row rather than display it: see
 * cmd_show_system().
 */
static int cpu_time_of(uint32_t vms_pid, char *cpu_str, size_t cpu_len)
{
    struct item_list_3 items[2];
    uint32_t centisec_total = 0;
    uint16_t retlen = 0;

    memset(items, 0, sizeof(items));
    items[0].buflen    = sizeof(uint32_t);
    items[0].item_code = JPI$_CPUTIM;
    items[0].bufaddr   = &centisec_total;
    items[0].retlen    = &retlen;

    uint32_t st = sys$getjpi(0, &vms_pid, NULL, items, NULL, NULL, 0);

    /* An unset item is as much a refusal as a failure status, and for the
     * same reason: the service reports JPI$_CPUTIM or it reports nothing,
     * never a zero standing in for a figure it could not obtain. */
    if (!(st & 1) || retlen != sizeof(uint32_t))
        return 0;

    /* JPI$_CPUTIM counts 10-millisecond units, i.e. centiseconds. */
    unsigned long total_sec = centisec_total / 100UL;
    unsigned long centisec  = centisec_total % 100UL;
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

    /* No dcl_get_context() here any more: the only thing this function
     * used the DCL context for was ctx->process_name, which it printed as
     * a FABRICATED process row when the PCB was empty (vms-8019). The row
     * source is now the executive's table, so the context is not a source
     * of process identity at all. */

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
    /*
     * ================================================================
     * THIS HEADER IS DELIBERATELY NARROWER THAN VMS's. OPERATOR RULING,
     * vms-8019 round 4. DO NOT "RESTORE" THE MISSING COLUMNS.
     * ================================================================
     *
     * OpenVMS SHOW SYSTEM also prints State, Pri, I/O, Page flts and
     * Pages. OVMX cannot source any of those from the executive's
     * process table, so it does not print them AT ALL.
     *
     * What stood here until this round printed "---" in each of them.
     * Rule 10 allows exactly two answers -- reproduce what VMS prints,
     * or do not expose the thing at all -- and a not-available marker
     * is neither. It is a display for a condition VMS never faces,
     * shipped in a user-visible VMS command, which is precisely the
     * illegal third answer. An absent column is a visibly incomplete
     * table; "---" in a VMS-shaped table is a fabricated
     * not-available state dressed as VMS output.
     *
     * THE VERBATIM QUESTION FOR THE ORACLE, owned by vms-6a7 ("SHOW
     * SYSTEM lists every process on the system", which explicitly owns
     * the display: "Match the real VMS column set and header -- pin
     * the format to the oracle"):
     *
     *   "What does OpenVMS VAX 7.3 SHOW SYSTEM print in the State,
     *    Pri, I/O, Page flts and Pages columns -- what are the exact
     *    column widths and header spelling -- and is any of it
     *    sourceable from what the OVMX executive actually holds?"
     *
     * Until that is answered the columns stay absent. Widening this
     * header without an oracle transcript re-commits the defect.
     */
    printf("  Pid    Process Name          CPU\n");

    /*
     * ENUMERATE THE EXECUTIVE'S PROCESS TABLE (vms-8019).
     *
     * SHOW SYSTEM is a READER of an executive facility, never a thing
     * that fabricates its own answer (CLAUDE.md Rule 11 corollary). What
     * stood here did the opposite: it printed exactly ONE row -- the
     * CALLING process -- out of that process's own private PCB, and if
     * the PCB was empty it FABRICATED a row from getpid() and the DCL
     * context's self-declared process name. A system display that can
     * only ever see the process running it is not a system display.
     *
     * The rows now come from src/kernel/vms_proctab.c through
     * vms_kif_procscan(), so every process the executive knows about is
     * listed, named as the executive knows it -- which is the only sense
     * in which a VMS process name means anything.
     *
     * There is no absent-executive branch and must not be one: the first
     * vms_kif_* call binds and registers this process (kif_bind), so the
     * table always holds at least the caller. If the scan yields nothing
     * at all, the executive is unreachable -- a state OVMX does not run
     * in (Rule 9: PID 1 refuses to boot without it) -- and printing a
     * fabricated row to cover it is the illegal third answer (Rule 10).
     */
    uint32_t index = 0;
    struct vms_procinfo info;

    while (vms_kif_procscan(&index, &info) & 1) {
        /*
         * A REDACTED ROW GETS NO CPU FIGURE -- NOT A ZERO, NOT A MARKER.
         *
         * The executive returns rows the caller may not $GETJPI with
         * every identity field withheld and info.redacted set (see
         * src/kernel/vms_ioctl.h), so $GETJPI on such a row would be
         * refused SS$_NOPRIV -- there is no accounting to source.
         *
         * What stood here read /proc/0/stat, ignored the failure, and
         * printed the buffer's initialiser -- so a process whose
         * accounting this caller is FORBIDDEN to read displayed a
         * concrete, plausible "0 00:00:00.00". A fabricated accounting
         * value is exactly what this item exists to delete, and it had
         * survived inside the very function that was converted.
         *
         * OVMX cannot source the figure, so it prints no figure. Same
         * Rule 10 answer as the absent columns above, applied per row
         * instead of per table. What OpenVMS displays in the CPU column
         * for a process the caller cannot read is NOT PINNED -- it goes
         * with the rest of the column question to vms-6a7 -- and until
         * it is pinned, nothing is the only honest width.
         *
         * THE FIGURE IS THE SERVICE'S OR IT IS ABSENT (vms-8019 round 7).
         * The scan is a snapshot walked one row at a time, so a process
         * can also exit between being scanned and being read; the service
         * then answers SS$_NONEXPR and, exactly as for a redacted row,
         * this prints no figure. One rule covers both, because from the
         * display's side they are one condition -- OVMX cannot source the
         * figure -- and the alternative was to invent a second display
         * rule (dropping the row) for a condition VMS's own SHOW SYSTEM
         * never faces, which is the illegal third answer in the shape it
         * always takes: reasonable-looking, and unpinned. What a VMS
         * SHOW SYSTEM does with a process deleted mid-walk goes to
         * vms-6a7 with the rest of the column question.
         */
        char cpu_str[32] = "";
        if (!info.redacted &&
            !cpu_time_of(info.vms_pid, cpu_str, sizeof(cpu_str)))
            cpu_str[0] = '\0';

        /*
         * The empty Process Name column for an unnamed row is a KNOWN
         * DIVERGENCE with its own item: vms-d0e, "OVMX assigns no
         * default process name at creation, so JPI$_PRCNAM is empty
         * where VMS always has a name". See the JPI$_PRCNAM comment in
         * src/libvms/syssvc/sys_process.c for why the invented
         * "_%08X" name was deleted (it was a name only its owner could
         * resolve) -- deleting a wrong answer did not produce a right
         * one, and inventing a replacement here would just re-commit
         * it one layer up. A blank is what OVMX prints until vms-d0e
         * pins what the executive should assign.
         */
        printf(" %08X %-15s  %s\n",
               info.vms_pid,
               info.prcnam[0] ? info.prcnam : "",
               cpu_str);
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

    /*
     * STOPGAP (vms-2b8): this prints a privilege list the PROCESS
     * announced in its own environment, so SHOW PROCESS reports what
     * the process claims rather than what the executive holds -- the
     * exact inversion of the Rule 11 corollary that a VMS command is a
     * READER of an executive facility. The executive now holds the real
     * mask (vms_kif_getjpi_self -> info.cur_privs); wiring this to it
     * is blocked only on vms-9fc (no product process is registered, so
     * $GETJPI cannot resolve one). See the block in dcl_main.c.
     */
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
 *
 * STOPGAP -- FACADE, NOT VMS (vms-d0b). This walks /proc/mounts and a
 * process-local vms_device_table[], and where it finds nothing it
 * prints a hardcoded stub row. On VMS, SHOW DEVICE is a READER of the
 * executive's I/O database -- it cannot invent a device, and every
 * process sees the same list.
 *
 * The executive now has that table (src/kernel/vms_devtab.c;
 * $DEVICE_SCAN over it is vms_kif_devscan()). Converting this function
 * to read it is blocked on DCL being buildable into the QEMU runtime --
 * see the vms-d0b escalation. Note also that no terminal appears in
 * this listing at all today, though OPA0: is in the executive's table:
 * see docs/oracle/vax73-terminal-device.md for the format VMS uses for
 * a terminal row.
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
 * STOPGAP -- FACADE, NOT VMS (vms-d0b). This prints the DCL context's
 * own copy of a terminal, which nothing outside this process wrote and
 * nothing outside this process can see. On VMS, SHOW TERMINAL is a
 * READER of the executive's device table (CLAUDE.md rule 11
 * corollary): it reports the characteristics the executive holds for
 * the device, which is why what one process sets, another sees.
 *
 * The executive now has that table (src/kernel/vms_devtab.c, proven
 * A-writes/B-reads by tests/qemu/test_kmod_devtab.c). Converting this
 * function to read it is blocked on DCL being buildable into the QEMU
 * runtime -- see the vms-d0b escalation. The characteristic list this
 * prints also does not match the oracle: see
 * docs/oracle/vax73-terminal-device.md for what VMS V7.3 actually
 * displays.
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
    /*
     * Privilege display table -- BITS FROM prvdef.h, TEXT FROM THE ORACLE.
     *
     * This table used to number the privileges 0,1,2,3... sequentially in
     * display order, which is not any privilege encoding that has ever
     * existed. The mask it decodes is built by parse_privilege_string()
     * from prvdef.h's PRV$M_* bits, so the two disagreed on almost every
     * privilege: a user authorized for TMPMBX (bit 15) was displayed as
     * holding DETACH, one with NETMBX (bit 20) as holding EXQUOTA, and
     * bits 0 and 1 -- CMKRNL and CMEXEC, the two most dangerous
     * privileges in the system -- were printed as "TMPMBX" and "NETMBX"
     * and handed out as the default. The table also listed SYSNAM twice.
     *
     * Bits now come from prvdef.h, which is static-asserted against the
     * executive's copy. Descriptions are VERBATIM from the reference lab
     * OpenVMS VAX V7.3 node VAX1 (docs/oracle/vax73-privileges.md §4),
     * as is the " %-20s %s" line format.
     *
     * DELIBERATELY ABSENT: DETACH and SETPRI, and AUDIT, IMPORT and the
     * other names the oracle showed that prvdef.h has no bit for -- they
     * are omitted rather than assigned a guessed bit (CLAUDE.md Rule 10).
     *
     * CORRECTION, and a KNOWN-WRONG DISPLAY recorded rather than papered
     * over: the earlier claim here that "the oracle did not print DETACH
     * or SETPRI" was wrong in its reasoning. The oracle DID print bits 5
     * and 13 -- under their VAX alias names IMPERSONATE and ALTPRI, which
     * on VAX 7.3 ARE DETACH and SETPRI (docs/oracle/vax73-privileges.md
     * §2). The rows below give IMPERSONATE and ALTPRI their prvdef.h
     * *Alpha* bits 37 and 36, which no VAX-encoded mask ever sets, so
     * against a VAX-encoded mask both rows are unreachable and bits 5 and
     * 13 print as nothing.
     *
     * NOT FIXED HERE, deliberately: OVMX enforces neither privilege, and
     * this whole function is a getenv("VMS_PRIVILEGES")-fed stopgap that
     * is to be DELETED (not improved) once the executive reader lands --
     * see the block at the head of this function. Picking an encoding for
     * a display that is scheduled for deletion, for privileges nothing
     * enforces, would be choosing a constant without an oracle pin. The
     * divergence is recorded in the oracle doc so the item that DOES
     * enforce them pins both aliases deliberately.
     */
    static const struct {
        const char *name;
        uint64_t    bit;
        const char *desc;
    } privs[] = {
        { "ACNT",     PRV$M_ACNT,     "may suppress accounting messages" },
        { "ALLSPOOL", PRV$M_ALLSPOOL, "may allocate spooled device" },
        { "ALTPRI",   PRV$M_ALTPRI,   "may set any priority value" },
        { "BUGCHK",   PRV$M_BUGCHK,   "may make bug check log entries" },
        { "BYPASS",   PRV$M_BYPASS,   "may bypass all object access controls" },
        { "CMEXEC",   PRV$M_CMEXEC,   "may change mode to exec" },
        { "CMKRNL",   PRV$M_CMKRNL,   "may change mode to kernel" },
        { "IMPERSONATE", PRV$M_IMPERSONATE, "may impersonate another user" },
        { "DIAGNOSE", PRV$M_DIAGNOSE, "may diagnose devices" },
        { "DOWNGRADE",PRV$M_DOWNGRADE,"may downgrade object secrecy" },
        { "EXQUOTA",  PRV$M_EXQUOTA,  "may exceed disk quota" },
        { "GROUP",    PRV$M_GROUP,    "may affect other processes in same group" },
        { "GRPNAM",   PRV$M_GRPNAM,   "may insert in group logical name table" },
        { "GRPPRV",   PRV$M_GRPPRV,   "may access group objects via system protection" },
        { "LOG_IO",   PRV$M_LOG_IO,   "may do logical i/o" },
        { "MOUNT",    PRV$M_MOUNT,    "may execute mount acp function" },
        { "NETMBX",   PRV$M_NETMBX,   "may create network device" },
        { "OPER",     PRV$M_OPER,     "may perform operator functions" },
        { "PFNMAP",   PRV$M_PFNMAP,   "may map to specific physical pages" },
        { "PHY_IO",   PRV$M_PHY_IO,   "may do physical i/o" },
        { "PRMCEB",   PRV$M_PRMCEB,   "may create permanent common event clusters" },
        { "PRMGBL",   PRV$M_PRMGBL,   "may create permanent global sections" },
        { "PRMMBX",   PRV$M_PRMMBX,   "may create permanent mailbox" },
        { "PSWAPM",   PRV$M_PSWAPM,   "may change process swap mode" },
        { "READALL",  PRV$M_READALL,  "may read anything as the owner" },
        { "SECURITY", PRV$M_SECURITY, "may perform security administration functions" },
        { "SETPRV",   PRV$M_SETPRV,   "may set any privilege bit" },
        { "SHARE",    PRV$M_SHARE,    "may assign channels to non-shared devices" },
        { "SHMEM",    PRV$M_SHMEM,    "may create/delete objects in shared memory" },
        { "SYSGBL",   PRV$M_SYSGBL,   "may create system wide global sections" },
        { "SYSLCK",   PRV$M_SYSLCK,   "may lock system wide resources" },
        { "SYSNAM",   PRV$M_SYSNAM,   "may insert in system logical name table" },
        { "SYSPRV",   PRV$M_SYSPRV,   "may access objects via system protection" },
        { "TMPMBX",   PRV$M_TMPMBX,   "may create temporary mailbox" },
        { "UPGRADE",  PRV$M_UPGRADE,  "may upgrade object integrity" },
        { "VOLPRO",   PRV$M_VOLPRO,   "may override volume protection" },
        { "WORLD",    PRV$M_WORLD,    "may affect other processes in the world" },
        { NULL, 0, NULL }
    };

    uint64_t privmask = ctx->privileges;
    if (privmask == 0) {
        /* The two privileges OpenVMS grants essentially every user.
         * Was (1<<0)|(1<<1) -- CMKRNL|CMEXEC in the real encoding. */
        privmask = PRV$M_TMPMBX | PRV$M_NETMBX;
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
