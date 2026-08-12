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
#include <errno.h>

#include "prvdef.h"     /* PRV$M_* -- the single privilege bit table */
#include "prv_names.h"  /* VMS_PRIV_NAME_LIST -- single source for vms_priv_names[]
                          * below AND for prv_agreement.c's coverage check (vms-2b8) */
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
#include "ovmx_accounting.h"
#include "vmsqueue.h"
#include "descrip.h"
#include "jpidef.h"
/* The kernel-interface client: SHOW SYSTEM enumerates the executive process
 * table through it, and SHOW DEVICE reads the executive's device table
 * through it (vms-fb9). This is the same client src/libvms's system
 * services call; DCL uses it directly because the public $DEVICE_SCAN /
 * $GETDVI in src/libvms/syssvc/sys_device.c are themselves still
 * fabricators (a static scan_devices[] table and statvfs() of "/"), pinned
 * in that shape by host ctests that assert their invented answers
 * (tests/libvms/test_lib_fb3.c). Converting those services is tracked
 * separately -- it is not resolvable here without deleting host coverage,
 * which this item is explicitly forbidden to do. */
#include "vms_kif.h"

/*
 * The VMS condition-value renderer ("%FACILITY-S-IDENT, text"), from
 * src/libvms/status.c. SHOW PROCESS prints the status $GETJPI returned
 * for a target it could not read rather than choosing its own words for
 * it -- see the comment at the failure branch in cmd_show_process().
 */
extern int vms_status_string(uint32_t status, char *buf, size_t bufsize);

/* Forward declarations for queue/intrusion subcommands (dcl_cmd_process.c) */
extern int cmd_show_queue(struct dcl_command *cmd);
extern int cmd_show_entry(struct dcl_command *cmd);
extern int cmd_show_intrusion(struct dcl_command *cmd);

/* Forward declarations for helper functions used by cmd_show_process */
static int cmd_show_process_privileges(struct dcl_context *ctx);
static int cmd_show_process_quotas(struct dcl_context *ctx);

/*
 * Privilege display table -- BITS FROM prvdef.h, TEXT FROM THE ORACLE.
 *
 * File scope because two commands decode the same executive-held mask:
 * SHOW PROCESS's "Privileges:" line and SHOW PROCESS/PRIVILEGES's list.
 * They used to disagree, because only one of them had a table.
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
 * STILL NOT FIXED, deliberately: OVMX enforces neither privilege, and
 * picking an encoding for privileges nothing enforces would be
 * choosing a constant without an oracle pin. The divergence is
 * recorded in the oracle doc so the item that DOES enforce them pins
 * both aliases deliberately.
 *
 * NOT static, and typed as struct dcl_priv_name (dcl/dcl_cmd.h) rather
 * than an anonymous local struct (vms-2b8 round 6): dcl_lexical.c's
 * F$GETJPI CURPRIV/AUTHPRIV filters this SAME table by
 * VMS_PRV_M_ENFORCED to derive the names it may show, instead of
 * carrying its own hand-maintained second list of just the enforced
 * ones. One table, two readers.
 *
 * GENERATED FROM src/libvms/include/prv_names.h's VMS_PRIV_NAME_LIST
 * (vms-2b8 round 10), not hand-typed here: prv_agreement.c derives its
 * VMS_PRV_M_ENFORCED coverage check from that SAME list, so a row deleted
 * from this table is a row deleted from the coverage mask in the same
 * edit -- see prv_names.h for why that closes the gap a hand-typed
 * coverage whitelist could not.
 */
const struct dcl_priv_name vms_priv_names[] = {
    VMS_PRIV_NAME_LIST(VMS_PRIV_ROW_ENTRY)
    { NULL, 0, NULL }
};

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

/*
 * print_lnm_search_list - render a logical name's equivalence string(s) in
 * the shape real SHOW LOGICAL uses for a search-list (multi-valued) logical
 * (vms-420, oracle-pinned against OpenVMS VAX V7.3, lab-2/vaxlab-0):
 *
 *    "FOO" = "BAR" (LNM$PROCESS_TABLE)
 *         = "BAZ"
 *
 * i.e. the first value shares the name/table line; every further value gets
 * its own continuation line, 8 spaces then "= \"value\"" -- fixed indentation,
 * not aligned to the name's length. `values`/`n` must have n >= 1.
 */
static void print_lnm_search_list(const char *name, const char *table_label,
                                  char values[][LNM_MAX_VALUE + 1], uint8_t n)
{
    if (n == 0)
        return;
    printf("   \"%s\" = \"%s\" (%s)\n", name, values[0], table_label);
    for (uint8_t i = 1; i < n; i++)
        printf("        = \"%s\"\n", values[i]);
}

static int show_lnm_callback(const char *name, const lnm_entry_t *entry, void *ctx)
{
    struct show_lnm_ctx *sctx = (struct show_lnm_ctx *)ctx;
    if (entry->num_translations > 0) {
        /* Capped at LNM_MAX_SEARCHLIST, the same display cap
         * lnm_translate_values() uses, to keep this stack buffer small. */
        uint8_t n = entry->num_translations;
        char values[LNM_MAX_SEARCHLIST][LNM_MAX_VALUE + 1];
        if (n > LNM_MAX_SEARCHLIST)
            n = LNM_MAX_SEARCHLIST;
        for (uint8_t i = 0; i < n; i++) {
            strncpy(values[i], entry->translations[i].value, LNM_MAX_VALUE);
            values[i][LNM_MAX_VALUE] = '\0';
        }
        print_lnm_search_list(name, sctx->table_name, values, n);
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

    /*
     * Scope qualifiers (OpenVMS DCL Dictionary, SHOW LOGICAL): /PROCESS, /JOB,
     * /GROUP and /SYSTEM each restrict the operation to the named table --
     * "/SYSTEM ... Displays the system logical name table (LNM$SYSTEM)", and
     * likewise for the others. With NONE given, all four are shown/searched,
     * which is the search-list default; the qualifiers may be combined to show
     * several. Before this a qualified SHOW LOGICAL enumerated (or a named
     * lookup searched) all four regardless -- SHOW LOGICAL/SYSTEM dumped the
     * process, job and group tables too.
     */
    int q_process = dcl_has_qualifier(cmd, "PROCESS");
    int q_job     = dcl_has_qualifier(cmd, "JOB");
    int q_group   = dcl_has_qualifier(cmd, "GROUP");
    int q_system  = dcl_has_qualifier(cmd, "SYSTEM");
    int q_any     = q_process || q_job || q_group || q_system;
    int want[4];   /* PROCESS, JOB, GROUP, SYSTEM, in search order */
    want[0] = q_any ? q_process : 1;
    want[1] = q_any ? q_job     : 1;
    want[2] = q_any ? q_group   : 1;
    want[3] = q_any ? q_system  : 1;

    const char *tnames[4] = {
        LNM_PROCESS_TABLE, LNM_JOB_TABLE, LNM_GROUP_TABLE, LNM_SYSTEM_TABLE
    };
    /* Display label per table -- LNM$PROCESS shows as LNM$PROCESS_TABLE. */
    const char *tlabels[4] = {
        "LNM$PROCESS_TABLE", "LNM$JOB", "LNM$GROUP", "LNM$SYSTEM"
    };

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
        char values[LNM_MAX_SEARCHLIST][LNM_MAX_VALUE + 1];
        uint8_t nvalues;

        /*
         * A scope qualifier restricts the lookup to the named table(s), so a
         * name that exists only in LNM$PROCESS is NOT reported by SHOW
         * LOGICAL/SYSTEM. Without a qualifier, the default search list applies.
         *
         * vms-420: both branches below used to call lnm_translate(), which
         * only ever returns equivalence index 0 -- so DEFINE FOO BAR,BAZ
         * followed by SHOW LOGICAL FOO showed "FOO" = "BAR" with BAZ
         * silently dropped. lnm_translate_values() returns every value in
         * the found table, and print_lnm_search_list() renders all of them
         * in the oracle-pinned search-list shape.
         */
        if (q_any) {
            if (mgr) {
                for (int t = 0; t < 4; t++) {
                    if (!want[t]) continue;
                    uint32_t st = lnm_translate_values(mgr, tnames[t], upper_name,
                                                       values, LNM_MAX_SEARCHLIST,
                                                       &nvalues, NULL);
                    if (st == SS$_NORMAL) {
                        print_lnm_search_list(upper_name, tnames[t], values, nvalues);
                        return SS$_NORMAL;
                    }
                }
            }
            dcl_error("DCL", 0, "NOLOG", "no logical name match");
            return SS$_NOLOGNAM;
        }

        if (dcl_translate_logical(upper_name, value, sizeof(value)) == 0) {
            /*
             * Determine which table the name was found in, and fetch ALL of
             * its equivalence strings from that same table.
             */
            const char *found_table = LNM_PROCESS_TABLE;
            nvalues = 0;
            if (mgr) {
                /* Search tables in order to find where the name lives */
                lnm_table_t *search[4];
                search[0] = mgr->process_table;
                search[1] = mgr->job_table;
                search[2] = mgr->group_table;
                search[3] = mgr->system_table;
                for (int t = 0; t < 4; t++) {
                    if (!search[t]) continue;
                    uint32_t st = lnm_translate_values(mgr, tnames[t], upper_name,
                                                       values, LNM_MAX_SEARCHLIST,
                                                       &nvalues, NULL);
                    if (st == SS$_NORMAL) {
                        found_table = tnames[t];
                        break;
                    }
                }
            }
            if (nvalues == 0) {
                /* dcl_translate_logical()'s own fallback (SYS$DISK,
                 * SYS$LOGIN) resolved this name outside any LNM table. */
                strncpy(values[0], value, LNM_MAX_VALUE);
                values[0][LNM_MAX_VALUE] = '\0';
                nvalues = 1;
            }
            print_lnm_search_list(upper_name, found_table, values, nvalues);
        } else {
            dcl_error("DCL", 0, "NOLOG", "no logical name match");
            return SS$_NOLOGNAM;
        }
    } else {
        /* Enumerate the requested table(s) — all four unless a scope
         * qualifier narrows it. */
        if (mgr) {
            struct show_lnm_ctx sctx;
            int shown = 0;

            for (int t = 0; t < 4; t++) {
                if (!want[t]) continue;
                if (shown) printf("\n");
                printf("(%s)\n\n", tlabels[t]);
                sctx.table_name = tnames[t];
                lnm_enumerate(mgr, tnames[t], show_lnm_callback, &sctx);
                shown = 1;
            }
        } else {
            /* LNM not available — show nothing (graceful degrade) */
            printf("(LNM$PROCESS_TABLE)\n\n");
            printf("   %%DCL-W-NOLOGNAM, logical name manager not available\n");
        }
    }

    return SS$_NORMAL;
}

/*
 * format_cputim_field - render a JPI$_CPUTIM figure (10ms units) into VMS's
 * own 16-column CPU field, "d hh:mm:ss.cc".
 *
 * THE FIGURE COMES FROM THE EXECUTIVE ROW (vms-a7e). vms-8019 round 7
 * deleted the DCL-layer /proc parser that used to live here and routed
 * SHOW SYSTEM's CPU through sys$getjpi(JPI$_CPUTIM) -- a command is a
 * READER of an executive facility, never a second source (Rule 11). This
 * goes one step further now that vms_ioctl_procscan carries the figure in
 * the row it already returns (struct vms_procinfo.cputim, sourced in the
 * executive by fill_proc_acct from the task the process table pins). The
 * scan row IS that facility, so the caller reads info.cputim directly --
 * one ioctl per table instead of an extra $GETJPI per row -- and, because
 * the executive now carries the accounting on redacted rows too, a CPU
 * figure is rendered for a cross-group process exactly as the oracle
 * showed VMS does (docs/oracle/vax73-show-system-process.md §1.2), closing
 * the divergence §1.2 recorded.
 *
 * VMS's field is byte-for-byte a 4-column right-justified day count, one
 * space, then hh:mm:ss.cc -- 16 columns ("   0 00:00:00.05"), counted
 * through `cat -A` on VAX1 (ibid. §1.1).
 */
static void format_cputim_field(uint32_t centisec_total, char *cpu_str,
                                size_t cpu_len)
{
    /* JPI$_CPUTIM counts 10-millisecond units, i.e. centiseconds. */
    unsigned long total_sec = centisec_total / 100UL;
    unsigned long centisec  = centisec_total % 100UL;
    unsigned long hh = total_sec / 3600;
    unsigned long mm = (total_sec % 3600) / 60;
    unsigned long ss = total_sec % 60;

    snprintf(cpu_str, cpu_len, "%4lu %02lu:%02lu:%02lu.%02lu",
             hh / 24,          /* days (usually 0) */
             hh % 24, mm, ss, centisec);
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
     * THE COLUMN SET IS NOW ORACLE-PINNED (vms-6a7).
     * docs/oracle/vax73-show-system-process.md Sections 1 and 5.1.
     * ================================================================
     *
     * vms-8019 round 4 deleted "---" placeholders from five columns and
     * left the verbatim question for this item. VAX1 (OpenVMS VAX V7.3)
     * was booted and SHOW SYSTEM captured through `cat -A`, so the
     * geometry below is COUNTED, not eyeballed. VMS prints, at these
     * 0-based columns:
     *
     *   Pid           0-7    %08X, uppercase, NO LEADING SPACE
     *   (separator)   8
     *   Process Name  9-23   %-15s
     *   (separator)   24
     *   State        25-29   %-5s
     *   Pri          30-34   %5d
     *   I/O          35-43   %9d
     *   CPU          44-59   "%4d %02d:%02d:%02d.%02d"
     *   Page flts    60-69   %10d
     *   Pages        70-76   %7d
     *   type         77-79   "   " or "  N" -- and it has NO heading
     *
     *   "  Pid    Process Name    State  Pri      I/O       CPU       Page flts  Pages"
     *
     * OVMX's executive row (struct vms_procinfo) holds vms_pid and prcnam,
     * and -- since vms-a7e -- CPU time, PAGE FAULTS and RESIDENT PAGES,
     * sourced IN THE EXECUTIVE by fill_proc_acct (src/kernel/vms_proctab.c)
     * from the Linux task the process table pins. It still holds NO VMS
     * scheduler state, NO VMS priority and no VMS direct/buffered I/O split.
     *
     * THE RULE APPLIED HERE, which is the answer the "---" markers were
     * not: a column OVMX can source keeps VMS's OWN width, justification
     * and spelling; a column it cannot source is removed WHOLE -- heading
     * text and field together -- and the survivors close up.
     *
     * WHAT vms-a7e CHANGED. Page flts and Pages were absent because the
     * executive did not hold them and a /proc read in THIS command would be
     * a Rule-11 second source. That objection is gone: the executive now
     * holds both (read by the executive from the task it owns, so the
     * command is a reader of a facility, not a second source), so both
     * columns are RESTORED at VMS's own widths. State, Pri and the I/O
     * split remain removed -- OVMX has no VMS scheduler state, no VMS
     * priority and no direct/buffered I/O split, and mapping a Linux value
     * onto any of them would be the "unpinned invention" the oracle refused
     * (docs/oracle/vax73-show-system-process.md §5.1).
     *
     * THE SURVIVORS' CLOSED-UP GEOMETRY (OVMX's own, extending vms-6a7's
     * precedent of removing 25-43 and closing up). 0-based row columns:
     *   Pid        0-7    %08X            (VMS width, no leading space)
     *   (sep)      8
     *   Process Name 9-23 %-15s           (VMS width)
     *   (sep)      24
     *   CPU       25-40   16-col field    (VMS's own "%4d %02d:%02d:%02d.%02d")
     *   (sep)      41
     *   Page flts 42-51   %10u            (VMS width)
     *   (sep)      52
     *   Pages     53-59   %7u             (VMS width)
     * State/Pri/I/O (VMS 25-43) stay removed, so the table is VISIBLY
     * narrower than VMS's rather than VMS-shaped with invented content.
     *
     * DO NOT "RESTORE" State/Pri/I/O by deriving them from Linux. The way
     * to widen this header further is to make the executive hold the
     * quantity faithfully, and then read it.
     */
    printf("  Pid    Process Name          CPU         Page flts   Pages\n");

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
         * ACCOUNTING IS CARRIED ON EVERY ROW, redacted or not (vms-a7e) --
         * and this CLOSES a divergence vms-6a7 measured but could not fix.
         *
         * THE DIVERGENCE, AS IT STOOD. VAX1 (OpenVMS VAX V7.3) was booted
         * with the caller holding NO privileges at all
         * (docs/oracle/vax73-show-system-process.md §1.2): VMS printed the
         * COMPLETE accounting row -- State, Pri, I/O, CPU, Page flts,
         * Pages -- for a process in another UIC group, one command after
         * $GETJPI on that same process was refused every item. NOTHING in a
         * SHOW SYSTEM row is privileged on VMS. OVMX diverged because the
         * executive zeroed linux_pid on a redacted row, so the DCL layer's
         * per-row $GETJPI/proc read had nothing to source and blanked the
         * CPU figure -- §1.2 named the fix as "carry the accounting datum
         * on a redacted row" and placed it in the executive, not here.
         *
         * THAT FIX IS NOW DONE, in the executive where §1.2 put it.
         * vms_ioctl_procscan() sources CPU time, page faults and resident
         * pages from the task it pins by pid_ref (fill_proc_acct,
         * src/kernel/vms_proctab.c), independent of the zeroed linux_pid,
         * and sets a VMS_PI_V_* bit per field. Accounting is not identity,
         * so it rides through the redaction the identity fields still get.
         * A redacted row therefore arrives with real CPU/Page-flts/Pages
         * and this loop renders them, exactly as the oracle showed VMS
         * does -- no per-row $GETJPI, no /proc read, no fabricated zero.
         *
         * ABSENT IS STILL NOT ZERO. A field whose valid bit is CLEAR is
         * rendered as blanks of the column's width, never as a zero -- the
         * same discipline the `redacted` flag carries. For a live task
         * cputim/pageflts are always set; pages is set unless the task has
         * no address space (a kernel thread, never a VMS process). The scan
         * reaps dead entries first, so a returned row backs a live task.
         */
        char cpu_str[24];
        char pf_str[16];
        char pg_str[16];

        if (info.fields_valid & VMS_PI_V_CPUTIM)
            format_cputim_field(info.cputim, cpu_str, sizeof(cpu_str));
        else
            snprintf(cpu_str, sizeof(cpu_str), "%16s", "");

        if (info.fields_valid & VMS_PI_V_PAGEFLTS)
            snprintf(pf_str, sizeof(pf_str), "%10u", info.pageflts);
        else
            snprintf(pf_str, sizeof(pf_str), "%10s", "");

        if (info.fields_valid & VMS_PI_V_PAGES)
            snprintf(pg_str, sizeof(pg_str), "%7u", info.pages);
        else
            snprintf(pg_str, sizeof(pg_str), "%7s", "");

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
         *
         * WHAT vms-6a7 ADDED TO vms-d0e's EVIDENCE, having booted the
         * oracle: across every SHOW SYSTEM capture on VAX1 NO row had an
         * empty Process Name -- not even SWAPPER, which no user created
         * (docs/oracle/vax73-show-system-process.md Section 1.3). The
         * blank-name condition does not occur on VMS, so there is no VMS
         * rendering of it to copy, and the blank stays a placeholder
         * rather than a match. What the executive assigns to a process
         * created with no prcnam was NOT established -- the detached-
         * process attempt and why it failed are recorded in Section 1.3
         * so the next agent does not repeat it.
         */
        /*
         * VMS geometry (oracle-pinned, Section 1.1): the pid starts at
         * column ZERO -- there is no leading space -- the name is a
         * 15-column left-justified field at 9-23, and column 24 is the
         * single separator before the next column. What stood here
         * printed " %08X %-15s  %s", which shifted the whole row one
         * column right of VMS and put the CPU figure two columns further
         * out again.
         */
        printf("%08X %-15s %s %s %s\n",
               info.vms_pid,
               info.prcnam[0] ? info.prcnam : "",
               cpu_str, pf_str, pg_str);
    }

    return SS$_NORMAL;
}

/*
 * SHOW$_INVQUAVAL - "%SHOW-E-INVQUAVAL, value '<x>' invalid for
 * /IDENTIFICATION qualifier".
 *
 * ORACLE-PINNED (vms-6a7), docs/oracle/vax73-show-system-process.md
 * Section 3.2.2. On VAX1 (OpenVMS VAX V7.3):
 *
 *   $ SHOW PROCESS/ID=ZZZZ
 *   %SHOW-E-INVQUAVAL, value 'ZZZZ' invalid for /IDENTIFICATION qualifier
 *   $ WRITE SYS$OUTPUT "ST="+F$STRING(F$INTEGER($STATUS))
 *   ST=276304682
 *
 * 276304682 = 0x1078802A. The 0x10000000 is DCL's own STS$M_INHIB_MSG
 * control bit, set AFTER the message was printed; the condition value
 * itself is 0x0078802A -- facility 0x078, severity 2 (E). The value is
 * used here rather than a self-chosen status because CLAUDE.md Rule 10
 * forbids self-certifying a constant: this one was measured.
 *
 * Local to this file on purpose. Promoting it to a public header would
 * imply OVMX carries the whole SHOW message facility, which it does not.
 */
#define SHOW$_INVQUAVAL  0x0078802Au

/*
 * show_process_target_qual - the value of /IDENTIFICATION, given by any
 * abbreviation of two characters or more.
 *
 * dcl_qualifier_value() matches the qualifier name EXACTLY, so it cannot
 * see /ID -- which is how the qualifier is actually written, and how the
 * oracle transcript exercises it. VMS accepts any unambiguous
 * abbreviation; dcl_match_command() is the parser's existing
 * minimum-uniqueness matcher, so this is that rule applied to one
 * qualifier rather than a new mechanism.
 *
 * Returns NULL when the qualifier is absent (or negated), otherwise its
 * value -- which may be the empty string for a bare /IDENTIFICATION.
 */
static const char *show_process_target_qual(const struct dcl_command *cmd)
{
    if (!cmd) return NULL;

    for (int i = 0; i < cmd->qualifier_count; i++) {
        if (cmd->qualifiers[i].negated) continue;
        if (dcl_match_command(cmd->qualifiers[i].name, "IDENTIFICATION", 2))
            return cmd->qualifiers[i].value;
    }
    return NULL;
}

/*
 * SHOW PROCESS - report a process, WHICH NEED NOT BE THE CALLER.
 *
 * ================================================================
 * A READER OF THE EXECUTIVE PROCESS TABLE (vms-6a7, Rule 11).
 * ================================================================
 *
 * What stood here could only ever describe the process running it, and
 * described it out of values that process had written about ITSELF: the
 * DCL context's process_name, getpid(), getgid()/getuid(), and a
 * privilege list read from the VMS_PRIVILEGES environment variable. It
 * accepted no target at all -- "SHOW PROCESS AUDIT_SERVER" printed the
 * caller. That is the facade shape CLAUDE.md Rule 11 names: a command
 * that fabricates its own answer instead of reading an executive
 * facility, and one that passes every single-process test perfectly.
 *
 * The target is now resolved through $GETJPI, which resolves BY NAME and
 * BY PID in the executive (src/kernel/vms_proctab.c), and every field
 * printed comes from the row it returns.
 *
 * THE LAYOUT IS ORACLE-PINNED, NOT CARRIED OVER FROM THE OLD CODE.
 * VAX1 (OpenVMS VAX V7.3) was booted and every form below captured
 * through `cat -A` so columns were counted:
 * docs/oracle/vax73-show-system-process.md Sections 2 and 3. In
 * particular the header is a PAIR of lines carrying Node: and
 * Process name: at column 26/49, and body labels sit in a 20-column
 * field (the old code used 19 and put Process name: on its own line).
 *
 * WHAT IS DELIBERATELY NOT PRINTED, AND WHY (Rule 10 -- match VMS, or do
 * not expose it; never invent a plausible handler):
 *
 *   Terminal:          VMS prints it. The executive holds no terminal
 *                      for a process; OVMX's terminal is still a
 *                      per-process VMS_TERMINAL environment variable,
 *                      which is a facade with its own item (vms-d0b),
 *                      not a facility to read. Printing the caller's own
 *                      environment under a VMS label -- for a row that
 *                      may belong to another process entirely -- is the
 *                      defect, not the fix.
 *   Base priority:     VMS prints it. The executive holds no VMS
 *                      priority; the old code printed the literal 4 for
 *                      every process. Same answer as SHOW SYSTEM's Pri
 *                      column above.
 *   Devices allocated: absent, which is exactly what VMS prints for a
 *                      process with none (Section 3.1 -- AUDIT_SERVER
 *                      has no such section). OVMX allocates no devices
 *                      to a process in the executive.
 *   Privileges:        VMS PRINTS NO SUCH LINE. Plain SHOW PROCESS says
 *                      nothing whatever about privileges; they live
 *                      behind /PRIVILEGES (Section 2.2, and
 *                      vax73-privileges.md Section 6 (1)). The line was
 *                      an OVMX invention fed by getenv("VMS_PRIVILEGES")
 *                      -- a process reporting privileges it declared
 *                      about itself -- so it is DELETED rather than
 *                      rewired. /PRIVILEGES is untouched.
 *   Process quotas:    VMS prints no quota block here either; that is
 *                      /QUOTAS (Section 2.2). The inline block was seven
 *                      hardcoded lines of invented numbers, identical
 *                      for every process on every system. DELETED.
 *                      cmd_show_process_quotas() is untouched.
 *
 * User Identifier: IS printed, from JPI$_UIC, as the octal
 * [group,member]. VMS shows the RIGHTS IDENTIFIER ([SYSTEM]) when the
 * UIC has one; OVMX has no RIGHTSLIST. That divergence pre-dates this
 * item, is recorded in vax73-privileges.md Section 6 (4), and is not
 * vms-6a7's -- what IS vms-6a7's is that the numbers now come from the
 * TARGET's executive row instead of the caller's getgid()/getuid().
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

    /*
     * /ALL: "ALL INFORMATION ABOUT *THIS* PROCESS", not "all processes"
     * (vms-70eb -- the item vms-6a7/vms-2b8 flagged and left).
     *
     * WHAT STOOD HERE, AND WHY IT WAS DELETED WHOLE. The /ALL branch
     * printed a "      Processes at <date>" line and a
     * "Process Name / PID / UIC / State" table, then ONE row built from
     * ctx->process_name (the DCL context's self-declared name, NOT the
     * executive's prcnam) and a HARDCODED "LEF" state -- with the pid/UIC
     * read from vms_kif_getjpi_self but the identity column and the state
     * fabricated. The oracle capture in
     * docs/oracle/vax73-show-system-process.md Section 4 is decisive on
     * BOTH counts: VMS `SHOW PROCESS/ALL` prints "all information about
     * *this* process" -- the plain display, then Process Quotas,
     * Accounting, both privilege blocks, rights, dynamic-memory and the
     * job's process list -- and NOT a process table. Section 4 names the
     * table "the wrong shape AND a fabrication". A re-sourced table would
     * still be the wrong shape, so the table is removed WHOLE (the
     * vms-8019 SHOW SYSTEM ruling -- "remove the unsourceable thing whole,
     * do not invent content for it" -- applied to a whole display rather
     * than a column), and `LEF` goes with it: OVMX holds no VMS scheduler
     * state, exactly as the SHOW SYSTEM State column is absent for the
     * same reason (Section 5.1).
     *
     * WHAT /ALL IS NOW. It falls through to the same target-selection and
     * plain display every other SHOW PROCESS form uses -- every field read
     * from the target's executive row through $GETJPI, no getpid(), no
     * ctx->process_name, no literal. Then, for the CALLER'S OWN process,
     * it appends the one VMS /ALL section OVMX can source faithfully: the
     * two privilege blocks, from the executive-held perm_privs/cur_privs
     * masks (cmd_show_process_privileges, vms-2b8). Every OTHER VMS /ALL
     * section is OMITTED, not invented -- see the end of this function for
     * the per-section list and why each is unsourceable. That leaves a
     * /ALL that is a strict, honest subset of VMS's, in VMS's own order,
     * rather than a VMS-shaped fabrication (CLAUDE.md Rules 10/11).
     */

    /*
     * ---- SELECT THE TARGET ----
     *
     * ORACLE-PINNED SELECTION RULES (Section 3):
     *   /IDENTIFICATION=<hex pid>  wins over a name parameter --
     *       "SHOW PROCESS/ID=2020020E SYSTEM" reported 2020020E.
     *   /IDENTIFICATION=0          selects the CALLER, the same rule
     *       $GETJPI documents for a pidadr of 0.
     *   a name parameter is UPCASED -- "SHOW PROCESS audit_server"
     *       resolved AUDIT_SERVER.
     *   neither                    selects the caller.
     *
     * The two selectors are mutually exclusive here because sys$getjpi
     * tests prcnam FIRST: passing both would silently invert the pinned
     * precedence.
     */
    const char *idval = show_process_target_qual(cmd);
    char     sel_name[VMS_PRCNAM_XFER];
    uint32_t sel_pid = 0;
    int      by_pid = 0, by_name = 0;

    sel_name[0] = '\0';

    if (idval) {
        char *end = NULL;
        unsigned long v;

        errno = 0;
        v = strtoul(idval, &end, 16);
        if (idval[0] == '\0' || !end || *end != '\0' ||
            errno == ERANGE || v > 0xFFFFFFFFUL) {
            /*
             * Message text and quoting are verbatim from the oracle
             * (Section 3.2.2). A malformed value is rejected at the DCL
             * layer and never reaches $GETJPI -- which is also why it
             * cannot be reported as NONEXPR: "ZZZZ" is not a process
             * that does not exist, it is not a process ID.
             */
            printf("%%SHOW-E-INVQUAVAL, value '%s' invalid for "
                   "/IDENTIFICATION qualifier\n", idval);
            return SHOW$_INVQUAVAL;
        }
        sel_pid = (uint32_t)v;
        by_pid  = (sel_pid != 0);       /* 0 means "the caller" */
    } else if (cmd->param_count > 1 && cmd->params[1][0]) {
        /*
         * params[1], NOT params[0]: cmd_show() dispatches on params[0],
         * which is the SHOW keyword itself ("PROCESS"). Reading params[0]
         * here made every "SHOW PROCESS" -- with or without a target --
         * look up a process literally named PROCESS, so both the self
         * case and the targeted case answered NONEXPR. Caught by
         * tests/qemu/test_syssvc_showproc.c against the real DCL.EXE;
         * every host-side ctest passed over it, because the host has no
         * executive and cannot tell "no such process" apart from
         * "no executive to ask".
         */
        size_t i;
        for (i = 0; i + 1 < sizeof(sel_name) && cmd->params[1][i]; i++)
            sel_name[i] = (char)toupper((unsigned char)cmd->params[1][i]);
        sel_name[i] = '\0';
        by_name = 1;
    }

    /*
     * ---- READ THE TARGET'S ROW ----
     *
     * JPI$_CPUTIM is deliberately NOT requested: SHOW PROCESS displays no
     * CPU figure, and sys$getjpi fails the WHOLE call with SS$_NONEXPR
     * when the target has exited between resolution and the accounting
     * read. Asking for an item this display does not print would let a
     * process that vanished mid-command turn a complete answer into an
     * error.
     */
    struct dsc$descriptor_s namdsc;
    struct item_list_3 items[5];
    uint32_t tgt_pid = 0, tgt_uic = 0;
    char     tgt_prcnam[VMS_PRCNAM_SIZE + 1];
    char     tgt_user[VMS_USERNAME_SIZE + 1];
    uint16_t rl_pid = 0, rl_nam = 0, rl_uic = 0, rl_user = 0;

    memset(tgt_prcnam, 0, sizeof(tgt_prcnam));
    memset(tgt_user, 0, sizeof(tgt_user));
    memset(items, 0, sizeof(items));

    items[0].buflen = sizeof(uint32_t);
    items[0].item_code = JPI$_PID;
    items[0].bufaddr = &tgt_pid;
    items[0].retlen = &rl_pid;

    items[1].buflen = (uint16_t)(sizeof(tgt_prcnam) - 1);
    items[1].item_code = JPI$_PRCNAM;
    items[1].bufaddr = tgt_prcnam;
    items[1].retlen = &rl_nam;

    items[2].buflen = sizeof(uint32_t);
    items[2].item_code = JPI$_UIC;
    items[2].bufaddr = &tgt_uic;
    items[2].retlen = &rl_uic;

    items[3].buflen = (uint16_t)(sizeof(tgt_user) - 1);
    items[3].item_code = JPI$_USERNAME;
    items[3].bufaddr = tgt_user;
    items[3].retlen = &rl_user;

    memset(&namdsc, 0, sizeof(namdsc));
    namdsc.dsc$w_length  = (uint16_t)strlen(sel_name);
    namdsc.dsc$b_dtype   = DSC$K_DTYPE_T;
    namdsc.dsc$b_class   = DSC$K_CLASS_S;
    namdsc.dsc$a_pointer = sel_name;

    uint32_t st = sys$getjpi(0,
                             by_pid ? &sel_pid : NULL,
                             by_name ? &namdsc : NULL,
                             items, NULL, NULL, 0);

    if (!(st & 1)) {
        /*
         * THE STATUS IS RENDERED, NOT INTERPRETED. The two failures a
         * target read produces are both oracle-pinned (Section 3.2/3.3),
         * and they are DIFFERENT answers for the same process:
         *
         *   by NAME, out of the caller's UIC group
         *       -> %SYSTEM-W-NONEXPR, nonexistent process
         *          (the name search is group-scoped -- the process is not
         *           found at all, exactly as for a name never created)
         *   by PID, out of the caller's UIC group
         *       -> %SYSTEM-F-NOPRIV, insufficient privilege or object
         *          protection violation
         *
         * The executive already returns exactly those two statuses, so
         * this prints the one it got and adds no rule of its own. Do NOT
         * "normalise" the two into one message: the difference is the
         * measured behaviour.
         */
        char msg[160];
        vms_status_string(st, msg, sizeof(msg));
        printf("%s\n", msg);
        return (int)st;
    }

    /*
     * Is this the caller's own row? VMS gives its own process the full
     * display (real Default file spec) and any other process the
     * "Not available" rendering -- and it does so however the process was
     * named, including /ID=0 and /ID=<own pid> (Section 3.2.1). So the
     * question is decided on the RESOLVED pid, never on which selector
     * was used.
     */
    uint32_t self_pid = 0;
    uint16_t rl_self = 0;
    struct item_list_3 selfitems[2];
    memset(selfitems, 0, sizeof(selfitems));
    selfitems[0].buflen = sizeof(uint32_t);
    selfitems[0].item_code = JPI$_PID;
    selfitems[0].bufaddr = &self_pid;
    selfitems[0].retlen = &rl_self;
    int is_self = (sys$getjpi(0, NULL, NULL, selfitems, NULL, NULL, 0) & 1) &&
                  rl_self == sizeof(uint32_t) && self_pid == tgt_pid;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char node[OVMX_IDENTITY_MAXLEN];
    ovmx_node_name(node, sizeof(node));

    /*
     * Geometry counted through `cat -A` on the oracle (Section 2.1):
     * date/time in columns 0-22, three spaces, "User:" at 26 with its
     * value in a 17-column field starting at 32, "Process ID:" at 49,
     * three spaces, the pid at 63. Line two is 26 spaces then the same
     * two-column arrangement carrying Node: and Process name:. A blank
     * line precedes the pair and another follows it.
     */
    printf("\n");
    printf("%2d-%s-%04d %02d:%02d:%02d.%02d   User: %-17sProcess ID:   %08X\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000),
           tgt_user, tgt_pid);
    printf("                          Node: %-17sProcess name: \"%s\"\n",
           node, tgt_prcnam);
    printf("\n");

    /* Body labels sit in a 20-column field -- values start at column 20. */
    char uicbuf[32];
    snprintf(uicbuf, sizeof(uicbuf), "[%03o,%03o]",
             (unsigned)((tgt_uic >> 16) & 0xFFFFu),
             (unsigned)(tgt_uic & 0xFFFFu));
    printf("%-20s%s\n", "User Identifier:", uicbuf);

    /*
     * "Not available" is VMS's OWN literal for a default directory it
     * cannot read (Section 3.1, AUDIT_SERVER). It is not an OVMX
     * placeholder standing in for a value we failed to fetch: OVMX is in
     * exactly the condition VMS renders that way, because a process's
     * default directory is RMS state private to that process and the
     * executive row does not carry it.
     */
    printf("%-20s%s\n", "Default file spec:",
           is_self ? ctx->default_dir : "Not available");

    /*
     * ---- /ALL: THE SOURCEABLE EXTRA SECTIONS, AND ONLY THOSE ----
     *
     * VMS `SHOW PROCESS/ALL` follows the plain display with, in order
     * (docs/oracle/vax73-show-system-process.md Section 4): Devices
     * allocated, Process Quotas, Accounting information, Authorized
     * privileges, Process privileges, Process rights, System rights,
     * Auto-unshelve, Image Dump, Scheduling class name, the Process
     * Dynamic Memory Area, and the job's process list.
     *
     * OVMX can faithfully source exactly ONE of those from the executive:
     * the two privilege blocks, from the perm_privs/cur_privs masks the
     * executive holds (vms-2b8) -- so those are printed, in VMS's own
     * order and format, by the SAME reader `SHOW PROCESS/PRIVILEGES` uses.
     * Every other section is OMITTED, never invented, because none of them
     * is sourceable and inventing a plausible one is the exact facade this
     * item exists to remove (Rule 10 -- match VMS, or do not expose it):
     *
     *   Devices allocated:  the executive allocates no devices to a
     *                       process (already absent from the plain body).
     *   Process Quotas:     OVMX's /QUOTAS block is a hardcoded, invented
     *                       set of numbers identical on every system
     *                       (its own pre-existing defect, tracked with
     *                       /QUOTAS, NOT reintroduced here under /ALL).
     *   Accounting info:    the executive holds CPU time / page faults /
     *                       resident pages (SHOW SYSTEM reads them) but
     *                       not the wider accounting set VMS prints here;
     *                       a partial block dressed as the whole would be
     *                       an invention.
     *   Process/System rights: OVMX has no RIGHTSLIST (the same reason
     *                       User Identifier prints octal, not [SYSTEM]).
     *   Auto-unshelve / Image Dump / Scheduling class / Dynamic Memory /
     *   job process list:   no executive facility backs any of these.
     *
     * Gated on is_self: privileges are IDENTITY, and OVMX does not
     * disclose another process's identity across UIC groups (the redaction
     * rule, vax73-privileges.md Section 5). For a non-self /ALL target the
     * plain display above is the whole honest answer.
     */
    if (dcl_has_qualifier(cmd, "ALL") && is_self) {
        printf("\n");
        cmd_show_process_privileges(ctx);
    }

    return SS$_NORMAL;
}

/*
 * SHOW USERS - Show logged-in users from the executive process table.
 *
 * ================================================================
 * A READER OF THE EXECUTIVE PROCESS TABLE (vms-72c, Rule 11 corollary).
 * ================================================================
 *
 * WHAT STOOD HERE was vms_term_list(), a reader of a file-based terminal
 * allocation table (src/vmsdcl/dcl_terminal.c) whose only WRITER,
 * vms_term_allocate(), vms-fb9 deleted for being the same self-declared-
 * name shape as the rejected VMS_PRCNAM environment cheat (CLAUDE.md
 * Rule 10, worked example 2). With no writer left, that table can never
 * hold an entry, so vms_term_list()'s `count` was always 0 and the
 * "no entries" branch always ran -- for every call, not as a fallback.
 * That branch fabricated a SINGLE row out of the CALLING process's own
 * DCL context: ctx->username, ctx->process_name, and getpid(). Measured
 * on a real QEMU boot before this fix: an authenticated SYSTEM console
 * session's SHOW USERS reported PID `00000049` -- a LINUX pid -- while
 * the SAME session's SHOW PROCESS, one command earlier in the same
 * transcript, reported the executive-assigned VMS pid `10000003` for
 * the identical process. A second login could never appear, because
 * nothing here ever looked past the caller.
 *
 * THE ROWS NOW COME FROM src/kernel/vms_proctab.c THROUGH
 * vms_kif_procscan(), the same source cmd_show_system() and
 * cmd_show_process() already read (this file), filtered to rows the
 * executive has bound to a terminal (VMS_IOCTL_SETTERM, vms-d0b) --
 * that is the same "is this job on a terminal" fact SHOW TERMINAL reads
 * for the caller, applied here to every row instead of just the
 * caller's own.
 *
 * A CROSS-GROUP SESSION MAY BE INVISIBLE HERE WITHOUT WORLD, and that is
 * a KNOWN, DISCLOSED divergence, not this item's to close: proc_fill_info()
 * (src/kernel/vms_proctab.c) redacts terminal along with the rest of a
 * row's identity when vms_proc_may_read() says no (oracle-pinned,
 * vax73-privileges.md Section 5 -- same-group needs no privilege,
 * cross-group needs WORLD), so a redacted row's terminal reads "" and is
 * skipped below exactly like an unbound one. cmd_show_system()'s own
 * comment above records the identical divergence for its CPU column and
 * why fixing it belongs to the redaction POLICY vms-8019 landed, not to
 * a display reader -- the same reasoning applies here without repeating
 * the whole argument.
 *
 * ================================================================
 * FORMAT, GROUNDED (vms-086). No oracle capture exists for SHOW USERS
 * (docs/oracle/ has none), so this is grounded in the public VSI/HPE
 * OpenVMS DCL Dictionary SHOW USERS entry (CLAUDE.md Rule 8):
 *   https://www0.mi.infn.it/~calcolo/OpenVMS/ssb71/9996/9996p060.htm
 * ================================================================
 *
 * THE SUMMARY LINE'S REAL WORDING, independently confirmed by three
 * captures (the 1996 DCL Dictionary example; a 2010 VAX session
 * transcript; a 2018 SHOW USERS/FULL transcript -- antapex.org/vms.txt,
 * raymii.org "Small OpenVMS titbits"), is:
 *
 *   "Total number of users = N,  number of processes = M"
 *
 * -- TWO DISTINCT counts, not one. "users" is the number of DISTINCT
 * usernames; "processes" is the number of rows (a username logged in
 * twice is one user, two processes). What stood here computed both from
 * the SAME loop variable, so they could never differ -- not a wording
 * bug (the words already matched VMS) but a COUNTING bug, invisible
 * until a username has more than one session. Fixed below by counting
 * distinct usernames separately from rows.
 *
 * THE DCL DICTIONARY ALSO PINS the column set: the default table is
 * "Username Node Interactive Subprocess Batch" (per-user counts); /FULL
 * is "Username Node Process Name PID Terminal" (per-process rows, plus
 * "port information"). OVMX's existing table is /FULL-shaped (rows, not
 * per-user counts) with Node missing -- restored below.
 *
 * INTERACTIVE/SUBPROCESS/BATCH IS SOURCED, NOT GUESSED, and it comes
 * out constant today for a STRUCTURAL reason worth stating plainly
 * (INV-6: hide what cannot be sourced, never invent a count).
 * VMS_IOCTL_SETTERM -- the only device-binding fact this row set is
 * built from (see the "READER OF THE EXECUTIVE PROCESS TABLE" section
 * above) -- is called from exactly ONE place in the whole tree:
 * src/ovmx_job_control/ovmx_job_control.c, once, on the login session's
 * process, before it execl()s LOGINOUT.EXE. It survives that execl()
 * because the executive keys the process table on the Linux thread-
 * group id, which execve() does not change (ovmx_job_control.c's own
 * comment). A SPAWN child (cmd_spawn, dcl_cmd_process.c) is a fork()ed
 * NEW thread-group id -- a fresh executive PCB -- that never calls
 * VMS_IOCTL_SETTERM itself, so it is never terminal-bound and this scan
 * (which filters on `terminal[0] != '\0'`) structurally never sees it.
 * SUBMIT (src/vmsqueue/vmsqueue.c) queues an entry but has no fork/exec
 * of its own -- OVMX has no batch EXECUTION engine yet, so no batch job
 * is ever a row in the executive process table at all. Given that, EVERY
 * row this scan can ever return is the terminal-bound root of an
 * interactive job: Interactive is not a guess about an ambiguous row,
 * it is the only value the sourcing can ever produce, and Subprocess/
 * Batch are honest, measured zeros -- not withheld, not invented -- for
 * as long as that structural fact holds. The day a subprocess or a real
 * batch executor registers a distinguishable row, this comment is the
 * marker that the classification needs a real per-row signal (the
 * executive's job_id, vms_internal.h, is not on the wire today -- see
 * struct vms_procinfo, vms_ioctl.h -- and adding it is a kernel-side
 * follow-up, not this item's).
 *
 * /FULL LOGIN TIME is an OVMX EXTENSION beyond the literal DCL
 * Dictionary SHOW USERS entry above, which documents no login-time
 * field for this command (CLAUDE.md Rule 8: where public docs do not
 * pin a byte/field, OVMX states its own choice rather than claiming
 * VMS-authentic verbatim output). It is offered because the source now
 * exists: JPI$_LOGINTIM (VMS_PI_V_LOGINTIM, vms-a7e) is carried on the
 * SAME row this command already reads, sourced in the executive from
 * the real Linux task's start_boottime (fill_proc_acct,
 * src/kernel/vms_proctab.c) -- not a second source (Rule 11). Formatted
 * through sys$asctim (starlet.h), which renders VMS's own canonical
 * "DD-MMM-YYYY HH:MM:SS.CC", so the value at least LOOKS like VMS even
 * where its placement in this command does not claim to reproduce a
 * measured VMS transcript. Absent the valid bit (no /dev/vms, INV-6),
 * the field renders as blanks -- never a fabricated timestamp.
 *
 * "Total number of users" is COUNTED BY WALKING THE SCAN, not carried in
 * a separate hand-maintained variable (Method Requirement 4): the header
 * line needs the count before the rows print, so the table is walked
 * once to count and once to print rather than accumulated into a
 * fixed-size buffer sized to a guessed maximum.
 */
static int cmd_show_users(struct dcl_command *cmd)
{
    int full = dcl_has_qualifier(cmd, "FULL");

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    printf("      OpenVMS User Processes at %2d-%s-%04d %02d:%02d:%02d.%02d\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 10000000));

    uint32_t index = 0;
    struct vms_procinfo info;
    int process_count = 0;
    int interactive_count = 0;   /* every row this scan can return, see above */
    int subprocess_count = 0;    /* structurally unreachable today, see above */
    int batch_count = 0;         /* structurally unreachable today, see above */
    /* 256 distinct usernames is comfortably above any realistic concurrent
     * interactive-login count for a single OVMX node; a table this size
     * still exists solely to de-duplicate usernames within one scan, not
     * to bound how many rows print (process_count/interactive_count are
     * exact regardless of this cap). */
    char seen_users[256][VMS_USERNAME_SIZE];
    int seen_count = 0;

    while (vms_kif_procscan(&index, &info) & 1) {
        if (info.redacted || info.terminal[0] == '\0')
            continue;
        process_count++;
        interactive_count++;

        int already_seen = 0;
        for (int i = 0; i < seen_count; i++) {
            if (strcasecmp(seen_users[i], info.username) == 0) {
                already_seen = 1;
                break;
            }
        }
        if (!already_seen && seen_count < 256) {
            strncpy(seen_users[seen_count], info.username, VMS_USERNAME_SIZE - 1);
            seen_users[seen_count][VMS_USERNAME_SIZE - 1] = '\0';
            seen_count++;
        }
    }

    printf("    Total number of users = %d,  number of processes = %d\n"
           "    (interactive = %d, subprocess = %d, batch = %d)\n\n",
           seen_count, process_count,
           interactive_count, subprocess_count, batch_count);

    char node[OVMX_IDENTITY_MAXLEN];
    ovmx_node_name(node, sizeof(node));

    if (full)
        printf("      Username     Node       Process Name      PID        "
               "Terminal        Type         Login Time\n");
    else
        printf("      Username     Node       Process Name      PID        "
               "Terminal        Type\n");

    index = 0;
    while (vms_kif_procscan(&index, &info) & 1) {
        if (info.redacted || info.terminal[0] == '\0')
            continue;

        char upper_name[VMS_USERNAME_SIZE];
        size_t i;
        for (i = 0; i < sizeof(upper_name) - 1 && info.username[i]; i++)
            upper_name[i] = (char)toupper((unsigned char)info.username[i]);
        upper_name[i] = '\0';

        printf("      %-12s %-10s %-16s  %08X   %-15s %-12s",
               upper_name, node, info.prcnam, (unsigned)info.vms_pid,
               info.terminal, "Interactive");

        if (full) {
            char login_str[24] = "";
            if (info.fields_valid & VMS_PI_V_LOGINTIM) {
                struct dsc$descriptor_s login_d = {
                    sizeof(login_str) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                    login_str
                };
                uint16_t login_len = 0;
                uint64_t logintim = info.logintim;
                sys$asctim(&login_len, &login_d, &logintim, 0);
                login_str[login_len] = '\0';
            }
            printf(" %s", login_str);
        }
        printf("\n");
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
                /*
                 * A DCL INTEGER IS A LONGWORD ON BOTH ARCHITECTURES, AND THE
                 * HEX/OCTAL COLUMNS MUST SAY SO (rd vms-c71, rd vms-580).
                 *
                 * THIS CLAIM IS NOW MEASURED ON TWO ARCHITECTURES, WHICH IS
                 * WHY IT IS PHRASED FLATLY. When the fix landed it was not:
                 * the only oracle was OpenVMS VAX V7.3, VAX is 32-bit, and so
                 * "what VMS does" and "what 32-bit VMS does" were the same
                 * sentence -- no measurement taken then could tell them apart,
                 * and this comment said so rather than guessing.
                 *
                 * Asked of real OpenVMS **Alpha** V8.4 (lab-Alpha, AlphaServer
                 * ES40; see tests/lab-alpha/README.md), the same two commands
                 * answer BYTE-IDENTICALLY to the VAX:
                 *
                 *     IDENT_L = -2147483644   Hex = 80000004  Octal = 20000000004
                 *     IDENT_D = 8388736       Hex = 00800080  Octal = 00040000200
                 *
                 * and DCL arithmetic is a longword there too -- 2147483647 + 1
                 * yields -2147483648, and 4294967300 yields 4, exactly as on
                 * the VAX and exactly as this file's own measurement of OVMX
                 * found. DCL integer width is ARCHITECTURE-INVARIANT at 32
                 * bits; it is not a VAX artefact that a 64-bit VMS widens.
                 *
                 * "No divergence, measured on both" is a result, not a
                 * formality -- it is the difference between this rendering
                 * being correct and being accidentally correct on the only
                 * machine anyone asked.
                 *
                 * Measured side by side against OpenVMS VAX V7.3 on lab node
                 * VAX1:
                 *
                 *   real VMS:  IDENT_L = -2147483644  Hex = 80000004
                 *                                     Octal = 20000000004
                 *   OVMX was:  IDENT_L = -2147483644  Hex = FFFFFFFF80000004
                 *                                     Octal = 1777777777760000000004
                 *
                 * Two defects, one line. `v` is a long -- 64 bits on this host
                 * -- so %lX sign-extended every NEGATIVE value to 16 hex digits
                 * where VMS prints 8. Independently, %012lo padded to 12 octal
                 * digits where a longword is 11, which was wrong for POSITIVE
                 * values too and so was wrong for every integer symbol printed.
                 * Hex happened to look right for positives, which is why only
                 * the octal width survived unnoticed.
                 *
                 * The mask is the fix for both: render the low longword, in the
                 * two widths a longword actually occupies. The DECIMAL column is
                 * deliberately NOT masked -- it was already correct against the
                 * oracle, and the value itself is already a longword (assignment
                 * truncates: BIG = 4294967300 yields 4, and 2147483647 + 1
                 * yields -2147483648), so all three columns now agree.
                 */
                unsigned long lw = (unsigned long)v & 0xFFFFFFFFUL;
                printf("  %s = %ld   Hex = %08lX  Octal = %011lo\n",
                       upper_name, v, lw, lw);
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
 * Print one SHOW DEVICE row, emitting the column header before the
 * first one. Lazy so that a scan matching nothing prints the oracle's
 * bare "%SYSTEM-W-NOSUCHDEV" with no header above it, exactly as
 * section 6 of docs/oracle/vax73-terminal-device.md recorded it.
 *
 * Column geometry is taken character-for-character from sections 4 and
 * 4.1 of that file: the name occupies columns 0-23, the status starts
 * at column 24, and the error count's last digit lands on column 45
 * (line length 46). "%-24s%-12s%10u" reproduces BOTH measured rows
 * byte-for-byte -- the six-character "Online" and the twelve-character
 * "Online alloc" -- which a "%-6s%16u" cannot.
 *
 * DEVICE STATUS, and why exactly two strings exist here. Section 4.1
 * was captured for this function: VMS appends "alloc" to the status
 * when the device is ALLOCATEd, and a second process that did not
 * allocate it sees the word too (A-writes/B-reads, measured with a
 * RUN/DETACHED probe) and stops seeing it when the allocation ends.
 * Ownership does NOT show here -- in every capture the console was
 * owned by the interactive job and the unallocated rows still read a
 * plain "Online" -- so this must key on info->allocated and on nothing
 * else. "Online" itself is not read from a field: the executive's
 * table has no online/offline flag and a device absent from the table
 * is not reported at all, so it is the only state a row can be in.
 * Everything the oracle prints for devices OVMX does not have (a disk's
 * "Mounted", an offline device) is deliberately not guessed at.
 */
static void show_device_row(const struct vms_devinfo *info, int *rows)
{
    if (*rows == 0) {
        printf("Device                  Device           Error\n");
        printf(" Name                   Status           Count\n");
    }
    printf("%-24s%-12s%10u\n", info->devnam,
           info->allocated ? "Online alloc" : "Online", info->errcnt);
    (*rows)++;
}

/*
 * DELETED (vms-fb9 r5): show_device_exec_failed() used to print an
 * invented "%OVMX-F-EXECDEV, the executive did not answer" for this
 * function's default: branches. That was the illegal third answer under
 * CLAUDE.md rule 10 -- not because the reasoning in the deleted comment
 * was sloppy (it correctly identified that VMS has no oracle-pinned
 * message for "the executive refused me" and correctly avoided
 * borrowing NOSUCHDEV's voice for it) but because the ruling that
 * question needed had already been made, product-wide, by vms-a35/
 * vms-0ff: "the executive did not answer" is the SAME per-call
 * executive-absent condition those items deleted from
 * src/libvms/syssvc/sys_lock.c and src/libvmssys/vms_kif.c
 * (vms_kif_register(), kif_bind()) rather than handled, on the ground
 * that PID 1 refuses to bring the system up without /dev/vms and holds
 * it open for the life of the system (src/ovmx_init/ovmx_init.c,
 * executive_attach). This function re-invented a handler for exactly
 * that condition. It is now unreachable the same way, not handled: see
 * the default: cases below.
 *
 * What is left in that default: case is a genuinely different thing --
 * a raw ioctl failure the closed errno set in vms_kif_kerr_to_ss() can
 * still produce (SS$_ACCVIO / SS$_INSFMEM / SS$_ILLIOFUNC / SS$_BUGCHECK)
 * even with the executive present and the caller registered. Those ARE
 * real VMS status values with real VMS meanings (oracle-pinned in
 * src/libvmssys/vms_kif.c's own header comment), so inventing OVMX text
 * for them would be the same defect a second time. They are returned to
 * the caller as $STATUS, unprinted -- the same treatment kif_bind() gives
 * a bind it cannot complete ("Nothing is reported if the bind cannot be
 * completed... the only way to fail here is for the executive to be
 * unreachable, and OVMX does not run without the executive"). Rendering
 * them to the user is not this function's job to invent; $STATUS is
 * there for the caller (or an ON ERROR handler) to see WHICH failure this
 * was.
 */

/*
 * SHOW DEVICE - a READER of the executive's device table (vms-fb9).
 *
 * WHAT THIS USED TO BE, and why it is gone. Until vms-fb9 this function
 * walked /proc/mounts, translated Linux mount points into invented
 * "$1$DGAn:" names, appended a process-local vms_device_table[] that
 * MOUNT/DISMOUNT keep in this process's own memory, and -- if all of
 * that produced nothing -- printed a hardcoded "$1$DGA0: Mounted"
 * stub row. Not one of those rows came from anywhere another process
 * could see, and the stub row was printed on a system with no devices
 * at all. On VMS, SHOW DEVICE is a READER of the executive's I/O
 * database: it cannot invent a device, and every process on the node
 * sees the same list (CLAUDE.md rule 11 corollary).
 *
 * WHAT IT IS NOW: $DEVICE_SCAN / $GETDVI over the executive-resident
 * device table in vms.ko (src/kernel/vms_devtab.c), read through
 * vms_kif_devscan() / vms_kif_getdvi_devnam(). Every field printed
 * comes out of that table. If the executive reports no matching
 * device, nothing is printed but the oracle's message -- there is no
 * stub row and no second source to fall back to.
 *
 * FORMAT, oracle-pinned (docs/oracle/vax73-terminal-device.md):
 *   section 4   -- the two header lines and the terminal row, verbatim
 *                  from OpenVMS VAX V7.3 on the ~/vax lab.
 *   section 4.1 -- the Device Status column: "Online", and
 *                  "Online alloc" when the device is allocated, with
 *                  the byte columns of both. See show_device_row().
 *   section 6   -- a device that does not exist prints ONLY
 *                  "%SYSTEM-W-NOSUCHDEV, no such device available",
 *                  with no column header above it. That is why the
 *                  header is emitted lazily, before the first row,
 *                  rather than unconditionally at the top.
 *
 * The oracle's /FULL form (section 5) additionally reports owner,
 * owner UIC, owner PID, reference count, device protection and default
 * buffer size; SHOW DEVICE/FULL is not implemented here, and no part
 * of it is half-printed into the brief form.
 *
 * THE THREE OUTCOMES ARE KEPT DISTINCT, and that is the point of the
 * switch below. Until 2026-07-30 they were not: any failure at all
 * collapsed to rows == 0 and printed the oracle's NOSUCHDEV, so an
 * executive that rejected the caller was reported to the user as "that
 * device does not exist" in VMS's own voice.
 *
 *   SS$_NOSUCHDEV     the executive answered, and there is no such
 *                     device. Oracle section 6, verbatim.
 *   SS$_IVDEVNAM      the executive answered, and the name is not a
 *                     legal device name. Oracle section 9.
 *   SS$_NOMOREDEV     $DEVICE_SCAN's end-of-scan. Not an error and
 *                     never shown to the user -- it is how the cursor
 *                     says it is done.
 *   anything else     an ioctl-level failure (SS$_ACCVIO / SS$_INSFMEM /
 *                     SS$_ILLIOFUNC / SS$_BUGCHECK from
 *                     vms_kif_kerr_to_ss()), NOT "the executive did not
 *                     answer" -- that state is unreachable, the same
 *                     ruling and the same reason vms-a35/vms-0ff deleted
 *                     it from src/libvms/syssvc/sys_lock.c and
 *                     src/libvmssys/vms_kif.c. Returned as $STATUS,
 *                     unprinted; see the deleted show_device_exec_failed()
 *                     comment above for why no OVMX message is invented
 *                     for it.
 *
 * DEVICE-NAME ARGUMENT. The name is handed to the executive as-is and
 * the executive resolves it -- including the physical-name folding
 * rule (upper case, one trailing colon, no leading underscore) and the
 * SS$_IVDEVNAM / SS$_NOSUCHDEV verdicts, which live in
 * src/kernel/vms_devtab.c normalize_devnam() and nowhere else. DCL
 * does not re-implement or second-guess that rule; a reader that
 * decided for itself which names exist would be the same defect one
 * level up.
 *
 * NOT implemented, and deliberately not guessed at: the oracle records
 * that "SHOW DEVICE TT" -- a device CLASS prefix, which OPA0: does not
 * begin with -- returns the terminal row on the lab (section 4). The
 * rule VMS uses to get from "TT" to OPA0: is not published in anything
 * this work has, so it is left unimplemented rather than approximated
 * (rule 10). A class prefix reports NOSUCHDEV here.
 */
static int cmd_show_device(struct dcl_command *cmd)
{
    const char *want = (cmd && cmd->param_count >= 2) ? cmd->params[1] : NULL;

    /*
     * Opening the executive channel here is now belt-and-braces: since
     * vms-9fc, src/libvmssys/vms_kif.c's kif_bind() completes the
     * documented open -> register sequence before EVERY ioctl, keyed on
     * the process, so no caller has to remember it. The result is
     * deliberately NOT tested, for the same reason
     * src/libvms/syssvc/sys_lock.c's bind_to_executive() does not test
     * it: the condition it would test for -- an unreachable executive --
     * is one OVMX is never in, because PID 1 refuses to bring the system
     * up without /dev/vms and pins it open for the life of the system
     * (src/ovmx_init/ovmx_init.c, executive_attach). Adding an "if the
     * executive is missing" branch here would be a handler for a state
     * VMS cannot be in (rule 10), and the only thing such a branch could
     * usefully do is fabricate rows -- the defect this rewrite removes.
     * If the ioctl fails anyway for a real reason (SS$_ACCVIO /
     * SS$_INSFMEM / SS$_ILLIOFUNC / SS$_BUGCHECK), $STATUS carries it and
     * nothing is printed for it; nothing is invented to cover it up
     * (vms-fb9 r5, see the deleted show_device_exec_failed() comment
     * above).
     */
    (void)vms_kif_open();

    struct vms_devinfo info;
    int rows = 0;
    uint32_t status;

    if (want) {
        status = vms_kif_getdvi_devnam(want, &info);

        switch (status) {
        case SS$_NORMAL:
            info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';
            show_device_row(&info, &rows);
            return SS$_NORMAL;
        case SS$_IVDEVNAM:
            /* Oracle section 9, from VMS's own message facility:
             * "%SYSTEM-W-IVDEVNAM, invalid device name". */
            dcl_error("SYSTEM", 0, "IVDEVNAM", "invalid device name");
            return SS$_IVDEVNAM;
        case SS$_NOSUCHDEV:
            /* Oracle section 6, verbatim: SHOW DEVICE ZZA0: ->
             * "%SYSTEM-W-NOSUCHDEV, no such device available". This is
             * the ONE outcome that message was measured for. */
            dcl_error("SYSTEM", 0, "NOSUCHDEV", "no such device available");
            return SS$_NOSUCHDEV;
        default:
            /* An ioctl-level failure, not "the executive did not
             * answer" -- see the deleted show_device_exec_failed()
             * comment above. Silent: $STATUS carries it, nothing is
             * rendered. */
            return status;
        }
    }

    uint32_t index = 0;

    while ((status = vms_kif_devscan(&index, &info)) == SS$_NORMAL) {
        info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';
        show_device_row(&info, &rows);
    }

    /*
     * status != SS$_NOMOREDEV here is the same ioctl-level-failure case
     * as the switch's default: above -- silent, $STATUS carries it.
     *
     * rows == 0 with status == SS$_NOMOREDEV -- the scan ran to
     * completion and the executive's device table was EMPTY -- is NOT
     * handled as a distinct case, and that absence is deliberate, not an
     * oversight (vms-fb9 r5). The previous round gave it its own invented
     * "%OVMX-F-NODEVTAB" message; that was the identical rule-10 mistake
     * as EXECDEV, and the round it was in did not check the one thing
     * that would have settled it -- whether a booted OVMX can actually
     * reach this state.
     *
     * MEASURED, not reasoned about: vms.ko creates the console OPA0: at
     * module init (src/kernel/vms_devtab.c, vms_devtab_init()), and the
     * seven device-table ioctls it implements ($ASSIGN, $DASSGN,
     * $GETDVI, $DEVICE_SCAN, IO$_SETMODE, $ALLOC, $DALLOC --
     * vms_ioctl_assign/dassgn/getdvi/devscan/ttsetmode/alloc/dalloc) do
     * not include a remove. There is no code path, product or test, that
     * takes a device back out of the table once the module is loaded.
     * tests/qemu/test_syssvc_showdev.c and tests/uat/vms_session_qemu.sh
     * both run a bare SHOW DEVICE against a real /dev/vms and both see
     * OPA0: every time -- rerun as part of this fix, unchanged result.
     * A table with zero devices is therefore not a state a booted OVMX
     * can present to SHOW DEVICE; it is the same "condition VMS is never
     * in" as the executive-absent case, made unreachable at the source
     * (vms_devtab_init()) rather than handled here. If that ever stops
     * being true -- a device-table entry becomes removable -- this
     * comment is the place to add the case back, pinned to whatever the
     * oracle says for an empty listing (which section 6 does not cover;
     * it is a NAMED device that does not exist, not an empty scan).
     */
    if (status != SS$_NOMOREDEV)
        return status;

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
 * SHOW TERMINAL - the characteristics of the terminal THIS JOB is on,
 * read out of the executive (vms-d0b).
 *
 * WHAT IT USED TO BE. It printed struct dcl_context's own copy of a
 * terminal: a name that arrived in the VMS_TERMINAL environment
 * variable, characteristics this process had set on itself, and an
 * owner it copied out of its own username field. Nothing outside the
 * process wrote any of it and nothing outside the process could see it
 * -- a facade that passed every single-process test perfectly
 * (CLAUDE.md rule 11). vms-fb9 deleted the environment handoff, which
 * left the name EMPTY, which was honest and unfinished: to read a
 * device you must first be able to name one, and naming the terminal a
 * job is on is executive knowledge.
 *
 * WHAT IT IS NOW. Two reads, both of the executive, in the order VMS
 * asks the same two questions:
 *
 *   1. $GETJPI on this process -> which terminal is this job on. The
 *      answer comes from the executive's process table
 *      (src/kernel/vms_proctab.c, struct vms_proc::terminal), where
 *      PID 1's login child recorded it from the channel it holds to
 *      the console -- so another process can read it too, which is
 *      what makes it a binding rather than a self-description.
 *   2. $GETDVI on that name -> the device's characteristics, out of
 *      the executive's device table (src/kernel/vms_devtab.c). Width,
 *      page length, the characteristic bits and the owner are
 *      properties of the DEVICE. What another process changes through
 *      IO$_SETMODE, this command reports.
 *
 * NOT ONE FIELD IS DECIDED HERE. There is no getenv, no ttyname(), no
 * isatty(), and in particular no "OPA0: is the only terminal in the
 * table so it must be mine" -- that inference is the same defect one
 * level up, and it is the reason the name is not simply looked up by
 * the constant OVMX_CONSOLE_DEVICE.
 *
 * WHEN THE EXECUTIVE REPORTS NO TERMINAL, NOTHING IS PRINTED. That is
 * the ruling vms-fb9 recorded at this site and it is unchanged: an
 * unanswerable question gets no answer rather than a plausible one
 * (rule 10). It is not a state a console login can be in -- PID 1
 * binds the terminal before it activates the image -- so there is no
 * message for it, invented or otherwise.
 *
 * FORMAT, ORACLE-PINNED, docs/oracle/vax73-terminal-device.md:
 *   section 1  -- the header prints the PHYSICAL name, with its leading
 *                 underscore: "Terminal: _OPA0:".
 *   section 2  -- the header's three fields and their column widths,
 *                 the "Terminal Characteristics:" heading, and the
 *                 four-column characteristic grid: three-space indent,
 *                 19-character cells, last cell of a row unpadded.
 *   section 3  -- "Unknown" (capital U) is what VMS displays for a
 *                 terminal whose device type is not identified.
 */

/*
 * The characteristic grid.
 *
 * ORDER IS THE ORACLE'S ORDER. The rows in section 2 read
 * Interactive, Echo, Type_ahead, No Escape, No Hostsync, TTsync, ...
 * left to right and top to bottom, and that is exactly the order of
 * VMS_TTC_INTERACTIVE, VMS_TTC_ECHO, VMS_TTC_TYPEAHEAD,
 * VMS_TTC_ESCAPE, VMS_TTC_HOSTSYNC ... in src/kernel/vms_ioctl.h. The
 * table below therefore walks the bits in numeric order and does not
 * carry an order of its own to drift from.
 *
 * SPELLINGS ARE THE ORACLE'S SPELLINGS. Every `set` name below appears
 * verbatim in the section 2 capture. The `clear` names are the same
 * capture's own two-state rule, recorded in the notes under it: the
 * inactive form is "No <name>", with the exceptions the capture itself
 * names -- Lowercase/Uppercase and Fulldup/Halfdup.
 *
 * TWO CELLS HAVE NO CLEAR SPELLING AND CARRY NULL RATHER THAN A GUESS.
 * The captures show Interactive and Insert editing in one state each --
 * both set -- and the section 2 note says plainly that Insert editing's
 * pair "is a different word" without recording which word. Inventing
 * "No Insert editing" would be a statement about VMS this work cannot
 * support (rule 10), so a NULL cell prints as blank -- visibly absent
 * instead of plausibly wrong.
 *
 * (Line Editing is NOT one of them, and an earlier draft of this
 * comment wrongly said it was: section 3 shows "No Line Editing"
 * directly, on the device type OVMX's console has.)
 *
 * A blank cell is not something a booted OVMX displays. Its console
 * comes up with both bits SET (src/kernel/vms_devtab.c,
 * VMS_CONSOLE_DEVCHAR), and the only operation that can clear a
 * characteristic is IO$_SETMODE, which no product code calls:
 * src/libvmssys/vms_kif.h declares vms_kif_ttsetmode unwired and
 * tests/integration/test_kif_caller_census.sh fails the build if that
 * declaration and reality disagree. The NULLs exist so the renderer
 * cannot fabricate a name if that ever changes.
 */
static const struct {
    uint64_t    bit;
    const char *set;
    const char *clear;      /* NULL when the oracle never showed it */
} terminal_chars[] = {
    { VMS_TTC_INTERACTIVE,     "Interactive",        NULL                   },
    { VMS_TTC_ECHO,            "Echo",               "No Echo"              },
    { VMS_TTC_TYPEAHEAD,       "Type_ahead",         "No Type_ahead"        },
    { VMS_TTC_ESCAPE,          "Escape",             "No Escape"            },
    { VMS_TTC_HOSTSYNC,        "Hostsync",           "No Hostsync"          },
    { VMS_TTC_TTSYNC,          "TTsync",             "No TTsync"            },
    { VMS_TTC_LOWERCASE,       "Lowercase",          "Uppercase"            },
    { VMS_TTC_TAB,             "Tab",                "No Tab"               },
    { VMS_TTC_WRAP,            "Wrap",               "No Wrap"              },
    { VMS_TTC_HARDCOPY,        "Hardcopy",           "No Hardcopy"          },
    { VMS_TTC_REMOTE,          "Remote",             "No Remote"            },
    { VMS_TTC_EIGHTBIT,        "Eightbit",           "No Eightbit"          },
    { VMS_TTC_BROADCAST,       "Broadcast",          "No Broadcast"         },
    { VMS_TTC_READSYNC,        "Readsync",           "No Readsync"          },
    { VMS_TTC_FORM,            "Form",               "No Form"              },
    { VMS_TTC_FULLDUP,         "Fulldup",            "Halfdup"              },
    { VMS_TTC_MODEM,           "Modem",              "No Modem"             },
    { VMS_TTC_LOCAL_ECHO,      "Local_echo",         "No Local_echo"        },
    { VMS_TTC_AUTOBAUD,        "Autobaud",           "No Autobaud"          },
    { VMS_TTC_HANGUP,          "Hangup",             "No Hangup"            },
    { VMS_TTC_BRDCSTMBX,       "Brdcstmbx",          "No Brdcstmbx"         },
    { VMS_TTC_DMA,             "DMA",                "No DMA"               },
    { VMS_TTC_ALTYPEAHD,       "Altypeahd",          "No Altypeahd"         },
    { VMS_TTC_SET_SPEED,       "Set_speed",          "No Set_speed"         },
    { VMS_TTC_COMMSYNC,        "Commsync",           "No Commsync"          },
    { VMS_TTC_LINE_EDITING,    "Line Editing",       "No Line Editing"      },
    { VMS_TTC_INSERT_EDITING,  "Insert editing",     NULL                   },
    { VMS_TTC_FALLBACK,        "Fallback",           "No Fallback"          },
    { VMS_TTC_DIALUP,          "Dialup",             "No Dialup"            },
    { VMS_TTC_SECURE_SERVER,   "Secure server",      "No Secure server"     },
    { VMS_TTC_DISCONNECT,      "Disconnect",         "No Disconnect"        },
    { VMS_TTC_PASTHRU,         "Pasthru",            "No Pasthru"           },
    { VMS_TTC_SYSPASSWORD,     "Syspassword",        "No Syspassword"       },
    { VMS_TTC_SIXEL,           "SIXEL Graphics",     "No SIXEL Graphics"    },
    { VMS_TTC_SOFT_CHARACTERS, "Soft Characters",    "No Soft Characters"   },
    { VMS_TTC_PRINTER_PORT,    "Printer Port",       "No Printer Port"      },
    { VMS_TTC_NUMERIC_KEYPAD,  "Numeric Keypad",     "No Numeric Keypad"    },
    { VMS_TTC_ANSI_CRT,        "ANSI_CRT",           "No ANSI_CRT"          },
    { VMS_TTC_REGIS,           "Regis",              "No Regis"             },
    { VMS_TTC_BLOCK_MODE,      "Block_mode",         "No Block_mode"        },
    { VMS_TTC_ADVANCED_VIDEO,  "Advanced_video",     "No Advanced_video"    },
    { VMS_TTC_EDIT_MODE,       "Edit_mode",          "No Edit_mode"         },
    { VMS_TTC_DEC_CRT,         "DEC_CRT",            "No DEC_CRT"           },
    { VMS_TTC_DEC_CRT2,        "DEC_CRT2",           "No DEC_CRT2"          },
    { VMS_TTC_DEC_CRT3,        "DEC_CRT3",           "No DEC_CRT3"          },
    { VMS_TTC_DEC_CRT4,        "DEC_CRT4",           "No DEC_CRT4"          },
    { VMS_TTC_DEC_CRT5,        "DEC_CRT5",           "No DEC_CRT5"          },
    { VMS_TTC_ANSI_COLOR,      "Ansi_Color",         "No Ansi_Color"        },
    { VMS_TTC_VMS_STYLE_INPUT, "VMS Style Input",    "No VMS Style Input"   },
};

/*
 * The owner's user name, out of the executive's process table.
 *
 * The oracle's header reads "Owner: SYSTEM" -- a USER NAME, not a
 * process id. The device table records the owner as a VMS process id,
 * so the name is a second executive read ($GETJPI on that process),
 * not something this process substitutes from its own idea of who it
 * is. The old code put ctx->username there, which made the field a
 * restatement of the asking process rather than a fact about the
 * device.
 *
 * An empty result is printed as empty. The executive has a user name
 * for a process only once an identity has been stamped on it
 * (VMS_IOCTL_SETIDENT), and OVMX's LOGINOUT does not do that yet
 * (vms-2b8), so today this field is normally blank -- which is what is
 * true. It is not filled in from anywhere else.
 */
static void terminal_owner_name(uint32_t owner_pid, char *out, size_t outsz)
{
    struct vms_procinfo pinfo;

    out[0] = '\0';
    if (owner_pid == 0)
        return;
    if (vms_kif_getjpi_pid(owner_pid, &pinfo) != SS$_NORMAL)
        return;
    pinfo.username[VMS_USERNAME_SIZE - 1] = '\0';
    snprintf(out, outsz, "%s", pinfo.username);
}

static void show_terminal_render(const struct vms_devinfo *info)
{
    const unsigned ncells =
        sizeof(terminal_chars) / sizeof(terminal_chars[0]);
    char owner[VMS_USERNAME_SIZE];
    char phys[VMS_DEVNAM_SIZE + 2];
    unsigned i;
    int col = 0;

    terminal_owner_name(info->owner_pid, owner, sizeof(owner));
    snprintf(phys, sizeof(phys), "_%s", info->devnam);

    /*
     * Header. The leading underscore is the physical-name form the
     * oracle prints (section 1); the executive keys its table on the
     * form without it. "Unknown" is the oracle's spelling for a
     * terminal whose type is not identified (section 3), and it is the
     * only device type the executive's table can report -- vms.ko
     * creates the console with type 0 and has no operation that sets
     * another, so no other spelling is reachable and none is written
     * down here.
     */
    /*
     * FIELD WIDTHS MEASURED OFF THE CAPTURE, NOT COPIED FROM THE OLD
     * CODE, and the old code was wrong by one column: it read
     * "Terminal: %-12s Device_Type: %-14s Owner: %s" -- a %-12s AND a
     * literal space, which puts Device_Type at column 23. The oracle
     * has it at 22 ("Terminal: " is 10, "_OPA0:" is 6, and section 2
     * shows exactly six spaces between them), and Owner at 49. So the
     * padding IS the separator here; there is no literal space after
     * either field.
     */
    printf("Terminal: %-12sDevice_Type: %-14sOwner: %s\n\n",
           phys,
           /* "Unknown" is pinned (section 3) and 0 is the only device
            * type the executive's table can hold: vms.ko creates the
            * console with type 0 and implements no operation that sets
            * another. A type the oracle has not shown us gets NO
            * spelling rather than a plausible one -- the first device
            * with a real type owes this line its pin. */
           info->devtype == 0 ? "Unknown" : "",
           owner);

    /*
     * WIDTH AND PAGE ARE NOT PRINTED (vms-d0b), CORRECTING THE SAME
     * MISTAKE ONE FIELD LATER. The line this replaced put them on one
     * line, "   Width:%4u      Page:%5u\n\n" -- a layout the oracle has
     * never shown. Read docs/oracle/vax73-terminal-device.md section 2
     * (the verbatim capture, lines 33-34) rather than trusting a count
     * here: VMS prints Width and Page on TWO SEPARATE lines, each
     * sharing the line with other fields OVMX cannot source --
     * Input:/Output: (line speed), LFfill:/CRfill: (fill counts) and
     * Parity:. Crushing Width and Page onto a single line of their own
     * invents a layout VMS does not use -- the same charge that got the
     * renderer THIS FUNCTION replaced deleted, just moved one field
     * over.
     *
     * The two candidate honest answers were "pin it" (reproduce the
     * two-line form, leaving Input/Output/LFfill/CRfill/Parity blank)
     * or "print neither". Pinning was rejected: nobody has ever seen
     * VMS print that block with those fields blank, so inventing their
     * spacing would be exactly the same fabrication one field further
     * in. Width and Page are A-writes/B-reads proven against the
     * executive at the kernel layer instead
     * (tests/qemu/test_kmod_devtab.c), which is where that property
     * belongs when the display cannot show it honestly.
     */

    printf("Terminal Characteristics:\n");
    for (i = 0; i < ncells; i++) {
        const char *name = (info->devchar & terminal_chars[i].bit)
                           ? terminal_chars[i].set
                           : terminal_chars[i].clear;

        if (col == 0)
            printf("   ");
        if (col == 3 || i + 1 == ncells) {
            /* The last cell of a row is unpadded: the oracle's rows
             * carry no trailing whitespace (section 2, read byte for
             * byte with cat -A). */
            printf("%s\n", name ? name : "");
            col = 0;
        } else {
            printf("%-19s", name ? name : "");
            col++;
        }
    }
}

static int cmd_show_terminal(struct dcl_command *cmd)
{
    (void)cmd;
    struct vms_procinfo pinfo;
    struct vms_devinfo info;
    uint32_t status;

    status = vms_kif_getjpi_self(&pinfo);
    if (status != SS$_NORMAL)
        return status;

    pinfo.terminal[VMS_DEVNAM_SIZE - 1] = '\0';
    if (pinfo.terminal[0] == '\0')
        return status;      /* no terminal: nothing to report, nothing invented */

    status = vms_kif_getdvi_devnam(pinfo.terminal, &info);
    if (status != SS$_NORMAL)
        return status;

    info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';
    show_terminal_render(&info);
    return SS$_NORMAL;
}

/*
 * SHOW PROCESS /PRIVILEGES - Display process privilege mask.
 */
static int cmd_show_process_privileges(struct dcl_context *ctx)
{
    (void)ctx;

    /*
     * READ THE MASK THE EXECUTIVE HOLDS (vms-2b8).
     *
     * This used to decode ctx->privileges, which dcl_main.c filled from
     * getenv("VMS_PRIVILEGES") and which SET PROCESS/PRIVILEGES could
     * overwrite -- so SHOW PROCESS/PRIVILEGES reported what the process
     * had told itself, and any process could tell itself "ALL". It also
     * substituted TMPMBX|NETMBX whenever the mask was empty, which
     * turned "this process has no privileges" into a display of two it
     * had not been given.
     *
     * cur_privs comes from src/kernel/vms_proctab.c: derived from the
     * task's real credentials at registration, and changeable only
     * through $SETPRV and VMS_IOCTL_SETIDENT, both of which refuse to
     * widen it past the authorized mask without SETPRV. On failure the
     * command returns the executive's status and prints no list -- see
     * the block in cmd_show_process for why there is no fallback.
     *
     * MASKED TO VMS_PRV_M_ENFORCED, operator ruling 2026-07-31 (Rule 10,
     * applied a second time to the reporting side): VMS_IOCTL_SETIDENT
     * stores and reports whatever SYSUAF authorizes, including bits
     * nothing in OVMX checks (e.g. SYSPRV -- an access-control override
     * that would have to live in vmsfs.ko, tracked separately as
     * vms-f15/vms-36d, NOT this item's job). A privilege that is
     * displayed but unenforced reads as a security control while being
     * none -- worse than an absent one. So the two blocks below show
     * only the intersection with VMS_PRV_M_ENFORCED
     * (src/kernel/vms_ioctl.h): exactly the privileges some vms.ko code
     * path will actually refuse an operation over today. This narrows
     * the display, not the stored mask -- info.perm_privs/cur_privs
     * still carry the full SYSUAF-authorized bits for anything that
     * later needs them (e.g. $GETJPI callers).
     */
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    uint32_t jst = vms_kif_getjpi_self(&info);
    if (!(jst & 1))
        return (int)jst;
    uint64_t shown_authorized = info.perm_privs & VMS_PRV_M_ENFORCED;
    uint64_t shown_current    = info.cur_privs  & VMS_PRV_M_ENFORCED;

    /*
     * TWO BLOCKS, BECAUSE VMS PRINTS TWO (vms-2b8 round 6,
     * docs/oracle/vax73-privileges.md §4, re-captured byte-exact this
     * round). OVMX printed only "Process privileges:" and omitted
     * "Authorized privileges:" entirely -- a block the oracle capture in
     * this repo had recorded since round 2 and that nothing printed. It
     * is printable now for the first time because the executive holds
     * BOTH masks (perm_privs and cur_privs); before the executive owned
     * identity there was only one number to show.
     *
     * The grid is VMS's, reproduced including its defect: 8 columns of
     * exactly 10 characters, so IMPERSONATE (11) is CLIPPED to
     * "IMPERSONAT" and runs into the next cell with no separating space.
     * The oracle does that; do not "fix" it. The trailing cells of a
     * short line are not padded (the oracle's last row ends at WORLD
     * with no trailing blanks).
     *
     * The separator between blocks is a line containing a single space,
     * which is what the capture shows -- not an empty line.
     */
    printf("Authorized privileges:\n");
    {
        int col = 0;
        char line[128];
        size_t len = 1;
        /* ONE leading space per LINE, then cells of exactly 10 -- not a
         * space per cell. The oracle's row is
         * " ACNT      ALLSPOOL  ALTPRI    ..." : 10 columns apart. */
        line[0] = ' ';
        line[1] = '\0';
        for (int i = 0; vms_priv_names[i].name; i++) {
            if (!(shown_authorized & vms_priv_names[i].bit))
                continue;
            /*
             * CodeQL cpp/unclear-buffer-write (round 13): snprintf
             * returns the length it WOULD have written, not what
             * actually fit, so accumulating that return value into
             * `len` unguarded lets `len` walk past `sizeof(line)` on
             * truncation -- the next call then computes
             * `sizeof(line) - len` as a size_t underflow and writes far
             * past the buffer. The 8-cell-per-row reset keeps every row
             * at <=81 of `line`'s 128 bytes today (measured:
             * tests/libvms/test_priv_render_bounds.c), so this branch
             * is not reachable by any name vms_priv_names[] carries --
             * but the loop must bound-check what it actually wrote
             * rather than trust a length it never measured. On a
             * would-be truncation, flush the row built so far and stop
             * (Rule 10: make the unreachable case an honest halt, not a
             * silent trust).
             */
            int n = snprintf(line + len, sizeof(line) - len,
                             "%-10.10s", vms_priv_names[i].name);
            if (n < 0 || (size_t)n >= sizeof(line) - len) {
                while (len > 1 && line[len - 1] == ' ')
                    line[--len] = '\0';
                printf("%s\n", line);
                col = 0; len = 1; line[1] = '\0';
                break;
            }
            len += (size_t)n;
            if (++col == 8) {
                /* Trim the padding of the final cell before printing. */
                while (len > 1 && line[len - 1] == ' ')
                    line[--len] = '\0';
                printf("%s\n", line);
                col = 0; len = 1; line[1] = '\0';
            }
        }
        if (col > 0) {
            while (len > 1 && line[len - 1] == ' ')
                line[--len] = '\0';
            printf("%s\n", line);
        }
    }

    printf(" \n");

    /*
     * The one-privilege-per-line block. " %-20s %s" is the oracle's
     * format, measured -- it was " %-16s %s" here, which contradicted
     * the capture sitting in this repo's own docs/oracle file.
     *
     * An empty mask prints an EMPTY block, not a sentence. OVMX used to
     * print " (no privileges enabled)"; the oracle in the same state
     * (docs/oracle/vax73-privileges.md §5.2, SET PROCESS/PRIVILEGE=NOALL)
     * prints the heading and nothing under it. A message VMS does not
     * emit is an invention however helpful it reads (Rule 10).
     */
    printf("Process privileges:\n");
    for (int i = 0; vms_priv_names[i].name; i++) {
        if (shown_current & vms_priv_names[i].bit)
            printf(" %-20s %s\n", vms_priv_names[i].name,
                   vms_priv_names[i].desc);
    }

    return SS$_NORMAL;
}


/*
 * SHOW PROCESS /QUOTAS - Display process quotas.
 */
static int cmd_show_process_quotas(struct dcl_context *ctx)
{
    (void)ctx;

    /* The account name is identity, so it comes from the executive and
     * has no fallback -- it used to print the literal "SYSTEM" for any
     * process whose (environment-supplied) user name was empty, which
     * named every anonymous process after the most privileged account
     * on the system (vms-2b8). */
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    uint32_t jst = vms_kif_getjpi_self(&info);
    if (!(jst & 1))
        return (int)jst;

    printf("Process Quotas:\n");
    printf(" Account name: %s\n", info.username);
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

    /*
     * vms-240: walk the chain attribute-aware so LNM$M_TERMINAL is honored --
     * iterative translation stops at a terminal translation instead of
     * re-translating its equivalence (VSI OpenVMS User's Manual, "Logical Name
     * Translation"). This is what makes the create-side default observable: a
     * plain DEFINE is non-terminal (its chain composes here), whereas
     * DEFINE/TRANSLATION_ATTRIBUTES=TERMINAL stops the chain at the terminal
     * name. lnm_translate() carries the attributes; dcl_translate_logical()
     * remains the first-level fallback for the process-context logicals
     * (SYS$DISK/SYS$LOGIN) it seeds, which carry no translation attributes.
     */
    lnm_manager_t *mgr = lnm_get_manager();
    char value[256];
    uint32_t attr = 0;
    int terminal = 0;
    {
        uint16_t vlen = 0;
        uint32_t st = mgr
            ? lnm_translate(mgr, LNM_FILE_DEV, upper_name, value,
                            sizeof(value), &vlen, &attr)
            : SS$_NOLOGNAM;
        if (st == SS$_NORMAL || st == SS$_SUPERSEDE) {
            value[vlen < sizeof(value) ? vlen : sizeof(value) - 1] = '\0';
            terminal = (attr & LNM_ATTR_TERMINAL) != 0;
        } else if (dcl_translate_logical(logname, value, sizeof(value)) == 0) {
            terminal = 0;
        } else {
            dcl_error("DCL", 0, "NOLOG", "no logical name match");
            return SS$_NOLOGNAM;
        }
    }

    /* Print translation chain (resolve iteratively up to 8 levels) */
    printf("   \"%s\" = \"%s\"\n", upper_name, value);

    char current[256];
    strncpy(current, value, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    int depth = 0;
    while (!terminal && mgr && depth < 8) {
        char next[256];
        uint16_t nlen = 0;
        uint32_t nattr = 0;
        uint32_t st = lnm_translate(mgr, LNM_FILE_DEV, current, next,
                                    sizeof(next), &nlen, &nattr);
        if (st != SS$_NORMAL && st != SS$_SUPERSEDE)
            break;
        next[nlen < sizeof(next) ? nlen : sizeof(next) - 1] = '\0';
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
        if (nattr & LNM_ATTR_TERMINAL)
            break;   /* terminal: stop the chain */
    }

    return SS$_NORMAL;
}

/*
 * SHOW LICENSE - Display active product licenses (LMF grant-all surface).
 *
 * FORMAT (operation) is VMS-exact; the CONTENT (identity + policy) is OVMX's.
 *
 * The header and per-license row geometry below are pinned to a real
 * OpenVMS VAX V7.3 `SHOW LICENSE` transcript captured off the reference lab
 * (lab-2 node VAX1, 11-AUG-2026), Rule 8 clean-room: format RE'd only from
 * observed oracle output + the public LMF / DCL-Dictionary layout, never from
 * any VSI/HPE binary. The exact columns (Product 0, Producer 19, Units
 * right-justified to col 35, Avail 38, Activ 44, Version 51, Release 56,
 * Termination 68; the "Active licenses on node <n>:" banner and its blank
 * line) reproduce that transcript byte-for-byte, so software that scrapes
 * SHOW LICENSE sees VMS-shaped output.
 *
 * THE ALWAYS-GRANTED POLICY IS AN OVMX DESIGN CHOICE, not VMS-authentic
 * (operator ruling 2026-08-11, licensing-stance-grant-all): OVMX has no
 * reason to gate, meter, or expire anything -- the license facility exists
 * ONLY so software that queries a license and refuses to run without one
 * passes. So there is no PAK database, no unit accounting, no expiry; the
 * programmatic query path grants any product by query. This display lists
 * OVMX's own always-loaded core products under producer OVMX (identity stays
 * honestly OVMX per INV-0, never "OpenVMS"/"DEC"): a coherent, permanently
 * active, non-expiring grant-all state. Units 0 / Avail 0 = an unlimited,
 * unmetered grant (matches how a real unrestricted PAK renders); Activ 100 =
 * the standard availability rating; Termination "(none)" = no expiry.
 */
static int cmd_show_license(struct dcl_command *cmd)
{
    (void)cmd;

    char node[OVMX_IDENTITY_MAXLEN];
    ovmx_node_name(node, sizeof(node));

    /* Banner + column headings -- byte-exact to the VAX V7.3 oracle. */
    printf("Active licenses on node %s:\n\n", node);
    printf("------- Product ID --------    ---- Rating ----- -- Version --\n");
    printf("Product            Producer    Units Avail Activ Version Release"
           "    Termination\n");

    /*
     * OVMX's always-loaded core products (identity ours; grant-all policy
     * ours). Field order: product, producer, units, avail, activ, version,
     * release, termination. Row format string is the oracle geometry.
     */
    static const char *const rows[][8] = {
        /* product            prod   units avail activ ver    rel      term   */
        { "OVMX",             "OVMX", "0",  "0",  "100", "0.0", "(none)", "(none)" },
        { "OVMX-USER",        "OVMX", "0",  "0",  "100", "0.0", "(none)", "(none)" },
        { "OVMX-VMSCLUSTER",  "OVMX", "0",  "0",  "100", "0.0", "(none)", "(none)" },
        { "OVMX-TCPIP",       "OVMX", "0",  "0",  "100", "0.0", "(none)", "(none)" },
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        printf("%-18.18s %-11s%6s%3s     %-7s%-5s%-12s%-11s \n",
               rows[i][0], rows[i][1], rows[i][2], rows[i][3],
               rows[i][4], rows[i][5], rows[i][6], rows[i][7]);
    }

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
 *
 * vms-17d (INV-DCL): reads the SAME real, system-wide flag SET ACCOUNTING
 * now writes (ovmx_accounting_is_enabled(), src/libvms/rtl/ovmx_accounting.c)
 * -- not the per-DCL-context bool this used to read, which only ever
 * reflected the CURRENT session's own SET ACCOUNTING, never a change made
 * anywhere else.
 */
static int cmd_show_accounting(struct dcl_command *cmd)
{
    (void)cmd;
    if (ovmx_accounting_is_enabled()) {
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
