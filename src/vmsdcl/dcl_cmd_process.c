/*
 * dcl_cmd_process.c - DCL process/job command implementations
 *
 * SUBMIT, PRINT, SPAWN, RUN, WAIT, STOP, EXIT, LOGOUT, PIPE, CONTINUE
 * Also queue sub-commands: SHOW QUEUE, SHOW ENTRY, SET ENTRY, SET QUEUE,
 * SHOW INTRUSION.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/dcl_cmd.h"
#include "ssdef.h"
#include "vmsfs/filespec.h"
#include "ovmx_layout.h"
#include "vmsqueue.h"
#include "opcdef.h"
#include "starlet.h"
#include "descrip.h"
#include "prcdef.h"
#include "vms/privs.h"    /* parse_privilege_string, PRV$M_* (RUN/PRIVILEGES, vms-d31d) */
#include "msgdef.h"
#include "ovmx_status.h"
#include "vms_kif.h"
#include "imgact_activate.h"
#include "dcl/dcl_rms.h"    /* rms_file_attr / dcl_rms_attr: ACP image probe (vms-5f0) */

int cmd_wait(struct dcl_command *cmd)
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
 * Queue message idents (vms-916, authenticity Tier-0). The queue/submit
 * commands below emit REAL VSI OpenVMS message idents from the JBC (Job
 * Controller / queue) facility, NOT the invented QMANERR/SUBMITERR/PRINTERR/
 * ENTNOTFND idents that used to sit here. Grounded to public VSI OpenVMS
 * documentation (clean-room Rule 8); the ident-by-ident table with citations
 * is docs/audit-message-idents-vms-916.md. Summary:
 *   queue manager unavailable  -> %JBC-E-JOBQUEDIS, system job queue manager
 *                                 is not running
 *   no such queue              -> %JBC-E-NOSUCHQUE, no such queue
 *   no such queue entry        -> %JBC-E-NOSUCHENT, no such entry
 *                                 (DELETE/ENTRY chains %DELETE-W-SEARCHFAIL)
 *   missing required parameter -> %DCL-W-INSFPRM, missing command parameters
 * Two idents have no VMS-authentic equivalent and are LABELLED OVMX-design so
 * no reader mistakes them for VMS: %OVMX-E-IVENTNUM (a non-numeric entry value,
 * which real VMS rejects in the CLD parser OVMX does not reach here) and
 * %OVMX-E-QUESETERR (an internal queue-state write fault).
 *
 * Queue initialization helper — ensures QMAN$MASTER.DAT exists and
 * default queues (SYS$BATCH, SYS$PRINT) are created.
 * Called lazily on first queue command.
 */
int queue_initialized = 0;

int ensure_queue_init(void)
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
int cmd_submit(struct dcl_command *cmd)
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
        dcl_error("JBC", 2, "JOBQUEDIS", "system job queue manager is not running");
        return sts;
    }

    /* Get queue name (/QUEUE=name; default is the queue SYS$BATCH translates
     * to). SYS$BATCH is a logical name for the default batch queue (VSI OpenVMS
     * DCL Dictionary, SUBMIT), so a DEFINE SYS$BATCH <queue> redirects a bare
     * SUBMIT live (vms-f89). Read at point of use, not hardcoded; the seeded
     * default is "SYS$BATCH" (lnm_setup_defaults). */
    char qbuf[256];
    const char *queue_name = dcl_qualifier_value(cmd, "QUEUE");
    if (!queue_name || !queue_name[0]) {
        if (dcl_translate_logical("SYS$BATCH", qbuf, sizeof(qbuf)) == 0
                && qbuf[0])
            queue_name = qbuf;
        else
            queue_name = "SYS$BATCH";
    }

    /* Format job name from filename (uppercase, strip path and extension) */
    const char *bn = strrchr(cmd->params[0], ']');
    if (!bn) bn = strrchr(cmd->params[0], ':');
    if (bn) bn++; else bn = cmd->params[0];

    char upper_name[256];
    size_t i;
    for (i = 0; bn[i] && bn[i] != '.' && bn[i] != ';' && i < sizeof(upper_name)-1; i++)
        upper_name[i] = (char)toupper((unsigned char)bn[i]);
    upper_name[i] = '\0';

    /* /NAME=job-name overrides the derived job name (DCL Dictionary
     * SUBMIT /NAME). Uppercased into the real queue entry. */
    const char *name_q = dcl_qualifier_value(cmd, "NAME");
    if (name_q && name_q[0]) {
        for (i = 0; name_q[i] && i < sizeof(upper_name) - 1; i++)
            upper_name[i] = (char)toupper((unsigned char)name_q[i]);
        upper_name[i] = '\0';
    }

    /*
     * NO FABRICATED OWNER (vms-f42d, CLAUDE.md Rule 10). This read
     * ": \"SYSTEM\"" -- so a process the executive holds no name for
     * submitted its job under the most privileged account on the system.
     * That state is reachable without privilege: vms_proc_register()
     * gives each new task a row with a zeroed username and inherits
     * nothing from its parent (src/kernel/vms_module.c), so any SPAWNed
     * subprocess is in it. The fallback is DELETED, not replaced -- see
     * the long form at lex_user() in src/vmsdcl/dcl_lexical.c. What goes
     * in the queue entry is what the executive holds, including when
     * that is nothing.
     */
    const char *user = ctx->username;

    uint32_t entry_id = 0;
    sts = vmsq_submit(queue_name, upper_name, user, &entry_id);
    if (!(sts & 1)) {
        dcl_error("JBC", 2, "NOSUCHQUE", "no such queue - %s", queue_name);
        return sts;
    }

    /* /HOLD places the entry in HOLDING state immediately after submission
     * (DCL Dictionary SUBMIT /HOLD); /NOHOLD is the default. Real state change
     * in the queue manager, observable via SHOW QUEUE. */
    int held = 0;
    if (dcl_has_qualifier(cmd, "HOLD")) {
        int hsts = vmsq_hold_entry(entry_id);
        if (hsts & 1) held = 1;
    }

    printf("%%SUBMIT-S-SUBMITTED, job %s (queue %s, entry %u) %s\n",
           upper_name, queue_name, entry_id, held ? "holding" : "queued");

    return SS$_NORMAL;
}

/*
 * PRINT - Queue a file for printing.
 * Format: PRINT filespec [/QUEUE=queue-name] [/NAME=job-name] [/HOLD]
 * Sends files to the print queue via vmsqueue. Qualifiers with no backing
 * queue-entry field (e.g. /COPIES) are rejected with %DCL-W-IVQUAL by the
 * CLD table (q_print, dcl_builtin.c) rather than silently accepted.
 */
int cmd_print(struct dcl_command *cmd)
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
        dcl_error("JBC", 2, "JOBQUEDIS", "system job queue manager is not running");
        return sts;
    }

    /* Get queue name (/QUEUE=name; default is the queue SYS$PRINT translates
     * to). SYS$PRINT is a logical name for the default print queue (VSI OpenVMS
     * DCL Dictionary, PRINT), so a DEFINE SYS$PRINT <queue> redirects a bare
     * PRINT live (vms-f89). Read at point of use, not hardcoded; the seeded
     * default is "SYS$PRINT" (lnm_setup_defaults). */
    char qbuf[256];
    const char *queue_name = dcl_qualifier_value(cmd, "QUEUE");
    if (!queue_name || !queue_name[0]) {
        if (dcl_translate_logical("SYS$PRINT", qbuf, sizeof(qbuf)) == 0
                && qbuf[0])
            queue_name = qbuf;
        else
            queue_name = "SYS$PRINT";
    }

    /* Format filename for display (uppercase, keep extension) */
    const char *bn = strrchr(cmd->params[0], ']');
    if (!bn) bn = strrchr(cmd->params[0], ':');
    if (bn) bn++; else bn = cmd->params[0];

    char upper_name[256];
    size_t i;
    for (i = 0; bn[i] && bn[i] != ';' && i < sizeof(upper_name)-1; i++)
        upper_name[i] = (char)toupper((unsigned char)bn[i]);
    upper_name[i] = '\0';

    /* /NAME=job-name overrides the derived job name (DCL Dictionary
     * PRINT /NAME). */
    const char *name_q = dcl_qualifier_value(cmd, "NAME");
    if (name_q && name_q[0]) {
        for (i = 0; name_q[i] && i < sizeof(upper_name) - 1; i++)
            upper_name[i] = (char)toupper((unsigned char)name_q[i]);
        upper_name[i] = '\0';
    }

    /* No fabricated owner -- same defect, same deletion, as SUBMIT above
     * (vms-f42d). */
    const char *user = ctx->username;

    uint32_t entry_id = 0;
    sts = vmsq_submit(queue_name, upper_name, user, &entry_id);
    if (!(sts & 1)) {
        dcl_error("JBC", 2, "NOSUCHQUE", "no such queue - %s", queue_name);
        return sts;
    }

    /* /HOLD holds the entry immediately (DCL Dictionary PRINT /HOLD);
     * /NOHOLD is the default. */
    int held = 0;
    if (dcl_has_qualifier(cmd, "HOLD")) {
        int hsts = vmsq_hold_entry(entry_id);
        if (hsts & 1) held = 1;
    }

    printf("%%PRINT-S-QUEUED, job %s (queue %s, entry %u) %s\n",
           upper_name, queue_name, entry_id, held ? "holding" : "queued");

    return SS$_NORMAL;
}

/*
 * SHOW QUEUE - Display queue status and entries.
 * Format: SHOW QUEUE [name] [/ALL] [/FULL]
 * Displays queue information matching VMS output format.
 */
int cmd_show_queue(struct dcl_command *cmd)
{
    int sts = ensure_queue_init();
    if (!(sts & 1)) {
        dcl_error("JBC", 2, "JOBQUEDIS", "system job queue manager is not running");
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
            dcl_error("JBC", 2, "NOSUCHQUE", "no such queue - %s", queue_name);
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
int cmd_set_entry(struct dcl_command *cmd)
{
    int sts = ensure_queue_init();
    if (!(sts & 1)) {
        dcl_error("JBC", 2, "JOBQUEDIS", "system job queue manager is not running");
        return sts;
    }

    /* Entry number is params[1] (params[0] is "ENTRY") */
    if (cmd->param_count < 2 || cmd->params[1][0] == '\0') {
        dcl_error("DCL", 0, "INSFPRM",
                  "missing command parameters - supply all required parameters");
        return SS$_BADPARAM;
    }

    char *endptr;
    long entry_val = strtol(cmd->params[1], &endptr, 10);
    if (endptr == cmd->params[1] || *endptr != '\0' || entry_val <= 0) {
        dcl_error("OVMX", 2, "IVENTNUM", "invalid entry number - %s", cmd->params[1]);
        return SS$_BADPARAM;
    }
    uint32_t entry_id = (uint32_t)entry_val;

    if (dcl_has_qualifier(cmd, "HOLD")) {
        sts = vmsq_hold_entry(entry_id);
        if (!(sts & 1)) {
            dcl_error("JBC", 2, "NOSUCHENT", "no such entry");
            return sts;
        }
        printf("%%SET-S-MODIFIED, entry %u set to HOLD\n", entry_id);
    } else if (dcl_has_qualifier(cmd, "RELEASE")) {
        sts = vmsq_release_entry(entry_id);
        if (!(sts & 1)) {
            dcl_error("JBC", 2, "NOSUCHENT", "no such entry");
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
int cmd_show_entry(struct dcl_command *cmd)
{
    int sts = ensure_queue_init();
    if (!(sts & 1)) {
        dcl_error("JBC", 2, "JOBQUEDIS", "system job queue manager is not running");
        return sts;
    }

    /* VMS binary time conversion constants */
    #define SHOW_ENTRY_VMS_UNIX_DIFF 3506716800ULL
    #define SHOW_ENTRY_VMS_TICKS     10000000ULL

    /* Entry number is params[1] if present (params[0] is "ENTRY") */
    if (cmd->param_count >= 2 && cmd->params[1][0] != '\0') {
        char *endptr;
        long entry_val = strtol(cmd->params[1], &endptr, 10);
        if (endptr == cmd->params[1] || *endptr != '\0' || entry_val <= 0) {
            dcl_error("OVMX", 2, "IVENTNUM", "invalid entry number - %s",
                      cmd->params[1]);
            return SS$_BADPARAM;
        }
        uint32_t entry_id = (uint32_t)entry_val;

        struct vms_queue_entry entry;
        sts = vmsq_show_entry(entry_id, &entry);
        if (!(sts & 1)) {
            dcl_error("JBC", 2, "NOSUCHENT", "no such entry");
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
int cmd_set_queue(struct dcl_command *cmd)
{
    int sts = ensure_queue_init();
    if (!(sts & 1)) {
        dcl_error("JBC", 2, "JOBQUEDIS", "system job queue manager is not running");
        return sts;
    }

    /* Queue name is params[1] (params[0] is "QUEUE") */
    if (cmd->param_count < 2 || cmd->params[1][0] == '\0') {
        dcl_error("DCL", 0, "INSFPRM",
                  "missing command parameters - supply all required parameters");
        return SS$_BADPARAM;
    }

    const char *queue_name = cmd->params[1];

    /* Check queue exists */
    struct vms_queue qinfo;
    sts = vmsq_show_queue(queue_name, &qinfo);
    if (!(sts & 1)) {
        dcl_error("JBC", 2, "NOSUCHQUE", "no such queue - %s", queue_name);
        return sts;
    }

    if (dcl_has_qualifier(cmd, "STOP")) {
        sts = vmsq_set_queue_status(queue_name, VMSQ_STATUS_STOPPED);
        if (!(sts & 1)) {
            dcl_error("OVMX", 2, "QUESETERR", "failed to stop queue %s", queue_name);
            return sts;
        }
        printf("%%SET-S-QUEMOD, queue %s stopped\n", queue_name);
    } else if (dcl_has_qualifier(cmd, "START")) {
        sts = vmsq_set_queue_status(queue_name, VMSQ_STATUS_STARTED);
        if (!(sts & 1)) {
            dcl_error("OVMX", 2, "QUESETERR", "failed to start queue %s", queue_name);
            return sts;
        }
        printf("%%SET-S-QUEMOD, queue %s started\n", queue_name);
    } else if (dcl_has_qualifier(cmd, "PAUSE")) {
        sts = vmsq_set_queue_status(queue_name, VMSQ_STATUS_PAUSED);
        if (!(sts & 1)) {
            dcl_error("OVMX", 2, "QUESETERR", "failed to pause queue %s", queue_name);
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
 * Reads VMS_MANAGER_DIR/INTRUSION.DAT
 * Format per line: timestamp|username|source|type|count
 */
int cmd_show_intrusion(struct dcl_command *cmd)
{
    (void)cmd;

    const char *intrusion_path = VMS_MANAGER_DIR "/INTRUSION.DAT";
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
/* ================================================================== */
/*                     Process/Session Commands                        */
/* ================================================================== */

/*
 * Build a CLASS_S string descriptor over a runtime C string.
 * The pointer is NULL for a NULL/empty string, which is how $CREPRC
 * distinguishes "argument omitted" from "argument supplied".
 */
static struct dsc$descriptor_s dsc_from_str(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = s ? (uint16_t)strlen(s) : 0;
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (s && *s) ? (char *)s : NULL;
    return d;
}

/*
 * run_creprc_failed - report a process creation that did not happen.
 *
 * ORACLE-PINNED (reference lab VAX1, OpenVMS VAX V7.3, 2026-07-30 --
 * the transcript recorded in tests/qemu/test_kmod_procnam.c): a
 * RUN/DETACHED that cannot create the process prints TWO lines,
 *
 *   %RUN-F-CREPRC, process creation failed
 *   -SYSTEM-F-DUPLNAM, duplicate name
 *
 * RUN reports its own failure and CHAINS the condition value it was
 * given. The secondary line is rendered from that condition value by
 * $GETMSG rather than chosen here, so a condition RUN has never seen is
 * reported as itself instead of being mapped onto the one message this
 * code happened to know. That is also what lets the OVMX-facility
 * refusals below reach the user as themselves: they are OVMX condition
 * values (src/libvms/include/ovmx_status.h), print under the facility
 * name OVMX, and cannot be mistaken for VMS conditions.
 */
static void run_print_condition(uint32_t status, int chained)
{
    char sec[256];
    struct dsc$descriptor_s sec_d;
    uint16_t sec_len = 0;
    sec_d.dsc$w_length  = (uint16_t)(sizeof(sec) - 1);
    sec_d.dsc$b_dtype   = DSC$K_DTYPE_T;
    sec_d.dsc$b_class   = DSC$K_CLASS_D;   /* not S: no blank padding */
    sec_d.dsc$a_pointer = sec;
    if (sys$getmsg(status, &sec_len, &sec_d, MSG$M_ALL, NULL) & 1) {
        sec[sec_len] = '\0';
        /* A chained message is introduced by '-', not '%'. */
        if (chained && sec[0] == '%') sec[0] = '-';
        fprintf(stderr, "%s\n", sec);
    }
}

static void run_creprc_failed(uint32_t status)
{
    dcl_error("RUN", 4, "CREPRC", "process creation failed");
    run_print_condition(status, 1);
}

/*
 * The RUN (Process) qualifier set, VERBATIM from the oracle.
 *
 * Source: reference lab VAX1, OpenVMS VAX V7.3, 2026-07-31,
 * `HELP/NOPROMPT RUN Process`, the "Qualifiers" index. Transcribed in
 * the order HELP prints it, with nothing added and nothing dropped.
 *
 * WHY THE LIST IS WRITTEN OUT RATHER THAN INFERRED. The oracle sentence
 * this table serves -- "A subprocess is created if any of the
 * qualifiers except the /UIC or the /DETACHED qualifier is specified"
 * -- appears under HELP RUN *Process*. The same HELP tree has a
 * separate HELP RUN *Image* topic whose entire qualifier list is
 * /DEBUG (/NODEBUG). Testing "does the command carry ANY qualifier?"
 * silently promotes the Process topic's sentence into a statement
 * about the Image topic's qualifiers, which the oracle does not
 * support -- and OVMX shipped exactly that: RUN/NODEBUG <image> was
 * refused as a subprocess request and the image did not run. Only the
 * names below may raise OVMX$_NOSUBPRC.
 *
 * /UIC and /DETACHED are IN the table because they are in the oracle's
 * index; the sentence excepts them, and run_refuse_unhonourable()
 * excepts them, so the table stays a faithful copy of what HELP prints
 * rather than a copy pre-filtered to this one use.
 *
 * A qualifier in NEITHER topic (RUN/NOSUCHQUAL) is not this code's
 * business: on the oracle DCL itself rejects it before RUN is entered
 * -- "%DCL-W-IVQUAL, unrecognized qualifier - check validity,
 * spelling, and placement" (VAX1, same session). The same is true of an
 * abbreviation that resolves to more than one of these names --
 * "%DCL-W-ABKEYW, ambiguous qualifier or keyword - supply more
 * characters" for RUN/P (VAX1, 2026-07-31, captures/
 * run-qualifier-abbrev-vax1-2026-07-31.txt). OVMX's DCL parser
 * validates no qualifier against any command's table, for any command;
 * that is a parser-wide gap, and inventing a RUN-only answer for it
 * here would be a third answer to a question VMS answers elsewhere.
 */
static const char *const run_process_qualifiers[] = {
    "ACCOUNTING", "AST_LIMIT", "AUTHORIZE", "BUFFER_LIMIT",
    "DELAY", "DETACHED", "DUMP", "ENQUEUE_LIMIT",
    "ERROR", "EXTENT", "FILE_LIMIT", "INPUT", "INTERVAL",
    "IO_BUFFERED", "IO_DIRECT", "JOB_TABLE_QUOTA",
    "MAILBOX", "MAXIMUM_WORKING_SET", "ON", "OUTPUT",
    "PAGE_FILE", "PRIORITY", "PRIVILEGES", "PROCESS_NAME",
    "QUEUE_LIMIT", "RESOURCE_WAIT", "SCHEDULE", "SERVICE_FAILURE",
    "SUBPROCESS_LIMIT", "SWAPPING", "TIME_LIMIT", "TRUSTED",
    "UIC", "WORKING_SET",
};

/*
 * The RUN (Image) qualifier set, VERBATIM from the oracle.
 *
 * Source: reference lab VAX1, OpenVMS VAX V7.3, 2026-07-31,
 * `HELP/NOPROMPT RUN Image Qualifier` -- which lists /DEBUG and /NODEBUG
 * and nothing else. One entry covers both spellings: the parser records
 * /NODEBUG as name "DEBUG" with negated set.
 *
 * It is here so that abbreviation resolution below runs over the
 * COMMAND's whole qualifier table, which is what DCL resolves against.
 * RUN is one command with one table; HELP splits it into two topics for
 * documentation, and only the SCOPE of the subprocess sentence follows
 * that split.
 */
static const char *const run_image_qualifiers[] = {
    "DEBUG",
};

/*
 * run_resolve_qualifier - resolve one qualifier name AS DCL RESOLVES IT.
 *
 * ORACLE-PINNED (reference lab VAX1, OpenVMS VAX V7.3, 2026-07-31;
 * transcript in the lab as captures/run-qualifier-abbrev-vax1-2026-07-31.txt).
 * Each probe named an image that does not exist, so DCL's verdict on the
 * qualifier is visible without creating anything: a resolved qualifier
 * reaches RUN and fails on the image, an unresolved one never gets there.
 *
 *   RUN/PRIO=4    -> %RUN-F-PARSEFAIL / -RMS-E-FNF   (resolved: /PRIORITY)
 *   RUN/PROC=FOO  -> %RUN-F-PARSEFAIL / -RMS-E-FNF   (resolved: /PROCESS_NAME)
 *   RUN/DETACH    -> %RUN-F-PARSEFAIL / -RMS-E-FNF   (resolved: /DETACHED)
 *   RUN/AST=100   -> %RUN-F-PARSEFAIL / -RMS-E-FNF   (resolved: /AST_LIMIT)
 *   RUN/PRIV=ALL  -> %RUN-F-PARSEFAIL / -RMS-E-FNF   (resolved: /PRIVILEGES)
 *   RUN/P=4       -> %DCL-W-ABKEYW, ambiguous qualifier or keyword
 *
 * So the rule is SHORTEST UNIQUE PREFIX, with no minimum length -- the
 * same rule dcl_match_command() already implements for verbs -- and
 * uniqueness, not length, is what /P fails.
 *
 * WHY THIS FUNCTION HAD TO EXIST. Matching qualifier names exactly is
 * not a stricter version of matching them the way DCL does; it is a
 * DIFFERENT command language. RUN/PRIO=4 is what an operator types and
 * what real VMS software ships -- tests/corpus/tier4-mx/kit/mx_start.com
 * builds "RUN/AST_LIMIT=100/BUFFER=.../DETACH/PRIV=ALL/PRIO=4/UIC=[1,4]"
 * -- and under exact matching every one of those spellings walked past
 * the refusal below and past run_detached()'s reads, so the image ran
 * with the whole instruction discarded and nothing said. That is Rule
 * 10's illegal third answer, reached by a route the full spellings never
 * take.
 *
 * Returns the full RUN qualifier name, or NULL if the given name matches
 * none of them or matches more than one. UNRESOLVED IS NOT DECIDED HERE:
 * on the oracle DCL refuses the command outright, with %DCL-W-IVQUAL for
 * an unknown qualifier and %DCL-W-ABKEYW for an ambiguous abbreviation,
 * BEFORE RUN is entered. OVMX's parser validates no qualifier against
 * any command's table, for any command, so neither refusal exists
 * anywhere in DCL; producing one here for RUN alone would answer for one
 * command a question VMS answers for the whole language. The gap is
 * reported, not patched over.
 */
static const char *run_resolve_qualifier(const char *given)
{
    const char *hit = NULL;
    size_t glen = strlen(given);

    if (glen == 0) return NULL;

    for (size_t j = 0;
         j < sizeof(run_process_qualifiers) / sizeof(run_process_qualifiers[0]);
         j++) {
        const char *full = run_process_qualifiers[j];
        if (strncasecmp(given, full, glen) == 0) {
            if (hit) return NULL;          /* ambiguous */
            hit = full;
        }
    }
    for (size_t j = 0;
         j < sizeof(run_image_qualifiers) / sizeof(run_image_qualifiers[0]);
         j++) {
        const char *full = run_image_qualifiers[j];
        if (strncasecmp(given, full, glen) == 0) {
            if (hit) return NULL;          /* ambiguous */
            hit = full;
        }
    }
    return hit;
}

/*
 * run_has_qualifier / run_qualifier_value - dcl_has_qualifier() and
 * dcl_qualifier_value() with DCL's abbreviation rule applied.
 *
 * RUN uses these EVERYWHERE it looks at a qualifier, and that is the
 * point: resolving abbreviations only where the command REFUSES, while
 * reading only exact names where it OBEYS, would refuse /DETACH and drop
 * /PROC=NAME -- a new silent discard created by the fix for the old one.
 *
 * These are RUN-local by intent. dcl_has_qualifier() is used by commands
 * that have no qualifier table at all; giving it a prefix rule with
 * nothing to be unique against would make every command's qualifier
 * matching depend on which literals its handler happened to test for.
 */
static int run_has_qualifier(const struct dcl_command *cmd, const char *full)
{
    for (int i = 0; i < cmd->qualifier_count; i++) {
        const char *r = run_resolve_qualifier(cmd->qualifiers[i].name);
        if (r && strcasecmp(r, full) == 0)
            return cmd->qualifiers[i].negated ? 0 : 1;
    }
    return 0;
}

static const char *run_qualifier_value(const struct dcl_command *cmd,
                                       const char *full)
{
    for (int i = 0; i < cmd->qualifier_count; i++) {
        const char *r = run_resolve_qualifier(cmd->qualifiers[i].name);
        if (r && strcasecmp(r, full) == 0)
            return cmd->qualifiers[i].value[0] ? cmd->qualifiers[i].value : NULL;
    }
    return NULL;
}

/*
 * How many of the command's qualifiers are RUN (Process) qualifiers
 * OTHER than the two the oracle's sentence excepts?
 *
 * The count is over the PARSED qualifier names, not over
 * run_has_qualifier(), because a negated form (/NOACCOUNTING) is still
 * a qualifier that was "specified" in the oracle's sense -- the parser
 * records it as name "ACCOUNTING" with negated set.
 */
static int run_process_qualifier_count(const struct dcl_command *cmd)
{
    int n = 0;
    for (int i = 0; i < cmd->qualifier_count; i++) {
        const char *name = run_resolve_qualifier(cmd->qualifiers[i].name);
        if (!name) continue;
        if (strcasecmp(name, "UIC") == 0 || strcasecmp(name, "DETACHED") == 0)
            continue;
        for (size_t j = 0;
             j < sizeof(run_process_qualifiers) / sizeof(run_process_qualifiers[0]);
             j++) {
            if (strcasecmp(name, run_process_qualifiers[j]) == 0) {
                n++;
                break;
            }
        }
    }
    return n;
}

/*
 * run_refuse_unhonourable - refuse a RUN that OVMX cannot carry out, at
 * the command layer, BEFORE anything is created.
 *
 * WHAT IT MAY AND MAY NOT REFUSE. Refusing a qualifier VMS accepts is
 * not the cautious version of accepting one VMS cannot honour -- it is
 * the mirror image of it, and both are Rule 10's illegal third answer.
 * Discarding tells the user their instruction was taken when it was
 * not; refusing tells them it was invalid when it was not. Every
 * refusal below is therefore scoped by a captured HELP topic, not by a
 * paraphrase of one.
 *
 * WHY THIS FUNCTION EXISTS AT ALL (CLAUDE.md Rule 10). RUN's
 * qualifiers are documented OpenVMS syntax, and the HELP text that
 * gives each of them its meaning -- and that says WHICH of RUN's two
 * topics it belongs to -- is quoted verbatim in ovmx_status.h next to
 * the three condition values raised here.
 * OVMX honours exactly one of the forms they select
 * -- RUN/DETACHED, which $CREPRC implements. For the others, Rule 10
 * allows two answers and only two: reproduce what VMS does, or make the
 * condition unreachable. It cannot be made unreachable, because a user
 * may type the qualifier; so it must be REFUSED, loudly, naming what
 * could not be done. Accepting it and quietly doing something else is
 * the illegal third answer, and it is what this code used to do:
 *
 *   - /PROCESS_NAME, /INPUT, /OUTPUT, /ERROR without /DETACHED were
 *     read and discarded, and the image ran in a plain fork()ed child
 *     that no one named. The oracle says those qualifiers ask for a
 *     SUBPROCESS; OVMX has no subprocess form of RUN.
 *   - /UIC was parsed into a packed UIC and passed to $CREPRC, which
 *     stores it in the created process's private PCB. The executive
 *     derives a process's UIC from Linux credentials and scopes process
 *     NAMES by its group, so the requested UIC changed nothing any
 *     other process could see (vms-afd). A qualifier whose whole
 *     purpose is the name's scope, having no effect on the name's
 *     scope, is a facade in VMS syntax.
 *
 * Returns 0 if the command may proceed, or the condition value that was
 * reported -- which is even, so DCL's $STATUS is a failure and SET ON
 * aborts the procedure, exactly as a refused creation does.
 */
static uint32_t run_refuse_unhonourable(struct dcl_command *cmd)
{
    /* /UIC IS NOW HONOURED on the detached path (vms-d31d): $CREPRC
     * stamps the created process's executive row with the requested UIC
     * (and /PRIVILEGES) via vms_kif_setident(), so a UIC handed to
     * $CREPRC now changes what every other process sees -- the exact
     * observability that once made refusing it the honest answer. /UIC
     * (like /DETACHED) creates a DETACHED process, so cmd_run routes it to
     * run_detached(), which reads and forwards it; it is NOT refused here
     * and it is excepted from the subprocess test below (as the oracle's
     * sentence excepts it). The DCL lexer still splits "/UIC=[g,m]" on the
     * comma, so run_detached()/cmd_run reassemble the "[g" value with the
     * stray "m]" parameter (run_parse_uic / run_uic_stray_param). */

    /* A RUN (PROCESS) qualifier -- any of the thirty-two the oracle's
     * index lists besides /UIC and /DETACHED -- asks OpenVMS for a
     * SUBPROCESS when /DETACHED is absent. HELP RUN Process (VAX1,
     * OpenVMS VAX V7.3, 2026-07-31): "A subprocess is created if any of
     * the qualifiers except the /UIC or the /DETACHED qualifier is
     * specified." Enumerating four of them (round 1) went on silently
     * discarding /PRIORITY; testing cmd->qualifier_count (round 2) went
     * the other way and refused /NODEBUG, which is not a process
     * qualifier at all. The set the sentence is scoped to is
     * run_process_qualifiers[], and that is the set tested here.
     *
     * Both halves go through run_resolve_qualifier(), so the set is the
     * set of qualifiers the user ASKED FOR, not the set they spelled out
     * in full: RUN/PRIO=4 is /PRIORITY (oracle-pinned, see that
     * function), and keying the membership test on exact names left it
     * running the image with the priority thrown away.
     *
     * /UIC (like /DETACHED) makes the command a DETACHED create, not a
     * subprocess one (the oracle sentence excepts BOTH), so /UIC present
     * means the OTHER process qualifiers ride the detached path too --
     * RUN/UIC=[1,4]/PRIVILEGES=(...) is a detached create, not a refused
     * subprocess (vms-d31d). */
    if (!run_has_qualifier(cmd, "DETACHED") &&
        !run_has_qualifier(cmd, "UIC") &&
        run_process_qualifier_count(cmd) > 0) {
        run_creprc_failed(OVMX$_NOSUBPRC);
        return OVMX$_NOSUBPRC;
    }

    /* /DEBUG belongs to the OTHER RUN topic. `HELP/NOPROMPT RUN Image
     * Qualifier` (VAX1, same session) lists /DEBUG and /NODEBUG and
     * nothing else, and the topic itself says the image is executed
     * "within the context of your process" -- no process is created,
     * so nothing here may be reported as a process creation failing.
     *
     * /NODEBUG is not mentioned in this function at all, deliberately.
     * It asks for the image to run without the debugger, which is what
     * OVMX does; VMS is matched by doing nothing. (The parser records
     * /NODEBUG as name "DEBUG" with negated set, so run_has_qualifier
     * returns 0 for it and this branch is not taken.)
     *
     * /DEBUG asks for a debugger OVMX has not got, and no OpenVMS
     * condition value means that (see OVMX$_NODEBUGGER in
     * ovmx_status.h). It is reported as itself, as a PRIMARY message,
     * because there is no VMS-side operation here that failed for it
     * to be chained to. */
    if (run_has_qualifier(cmd, "DEBUG")) {
        run_print_condition(OVMX$_NODEBUGGER, 0);
        return OVMX$_NODEBUGGER;
    }

    return 0;
}

/*
 * run_uic_stray_param - the index of the bare parameter that is really the
 * second half of a /UIC=[g,m] value (vms-d31d).
 *
 * The DCL lexer splits "/UIC=[g,m]" on the comma, so it reaches RUN as the
 * qualifier value "[g" plus a bare parameter "m]". That "m]" is NOT the
 * image. Return its parameter index so the image parameter can skip it, or
 * -1 when there is no such split (no /UIC, or the whole "[g,m]" survived as
 * a single qualifier value).
 */
static int run_uic_stray_param(const struct dcl_command *cmd)
{
    const char *v;
    if (!run_has_qualifier(cmd, "UIC"))
        return -1;
    v = run_qualifier_value(cmd, "UIC");
    if (!v || v[0] != '[' || strchr(v, ']'))
        return -1;                  /* value already complete: no stray half */
    for (int i = 0; i < cmd->param_count; i++) {
        size_t l = strlen(cmd->params[i]);
        if (l && cmd->params[i][l - 1] == ']')
            return i;
    }
    return -1;
}

/*
 * run_parse_uic - parse /UIC=[g,m] into a packed UIC, (group << 16) | member
 * (vms-d31d). VMS UIC numbers are OCTAL (OpenVMS User's Manual). Reassembles
 * the lexer's comma split (see run_uic_stray_param). Returns 0 on success,
 * -1 if the qualifier is absent or malformed.
 */
static int run_parse_uic(const struct dcl_command *cmd, uint32_t *out_uic)
{
    const char *v = run_qualifier_value(cmd, "UIC");
    char text[80];

    if (!v || !*v)
        return -1;

    if (strchr(v, ',') && strchr(v, ']')) {
        /* The whole "[g,m]" survived as one qualifier value. */
        strncpy(text, v, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
    } else {
        int si = run_uic_stray_param(cmd);
        int n;
        if (si < 0)
            return -1;
        n = snprintf(text, sizeof(text), "%s,%s", v, cmd->params[si]);
        if (n < 0 || (size_t)n >= sizeof(text))
            return -1;          /* absurdly long: not a UIC we can parse */
    }

    const char *lb = strchr(text, '[');
    const char *comma = strchr(text, ',');
    const char *rb = strchr(text, ']');
    if (!lb || !comma || !rb || comma <= lb || rb <= comma)
        return -1;

    char gs[32], ms[32];
    size_t gl = (size_t)(comma - (lb + 1));
    size_t ml = (size_t)(rb - (comma + 1));
    if (gl == 0 || gl >= sizeof(gs) || ml == 0 || ml >= sizeof(ms))
        return -1;
    memcpy(gs, lb + 1, gl); gs[gl] = '\0';
    memcpy(ms, comma + 1, ml); ms[ml] = '\0';

    char *end;
    unsigned long g = strtoul(gs, &end, 8);
    if (*end) return -1;
    unsigned long m = strtoul(ms, &end, 8);
    if (*end) return -1;

    *out_uic = ((uint32_t)(g & 0xFFFFu) << 16) | (uint32_t)(m & 0xFFFFu);
    return 0;
}

/*
 * run_parse_privileges - parse /PRIVILEGES=(name,...) into a mask (vms-d31d).
 *
 * The DCL parser preserves the surrounding parentheses of a list value, so
 * "/PRIVILEGES=(SYSPRV,BYPASS)" arrives as the literal "(SYSPRV,BYPASS)" and
 * "/PRIVILEGES=ALL" as "ALL" (dcl_parser.c). Strip one layer of parens and
 * hand the rest to the shared privilege-name parser. Returns 1 when the
 * qualifier was present (mask filled, possibly 0), 0 when absent.
 */
static int run_parse_privileges(const struct dcl_command *cmd, uint64_t *out_mask)
{
    const char *v = run_qualifier_value(cmd, "PRIVILEGES");
    char buf[512];
    char *p;
    size_t bl;

    if (!v || !*v)
        return 0;

    strncpy(buf, v, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    p = buf;
    bl = strlen(p);
    if (bl >= 2 && p[0] == '(' && p[bl - 1] == ')') {
        p[bl - 1] = '\0';
        p++;
    }
    *out_mask = parse_privilege_string(p);
    return 1;
}

/*
 * RUN/DETACHED - create a detached process.
 *
 * This is how a system startup procedure starts a service: the service
 * is a detached process with a VMS process name, so SHOW SYSTEM lists
 * it and $GETJPI resolves it BY NAME from any other process. DCL does
 * not wait on it -- it outlives the DCL that created it.
 *
 * THE NAME IS NOT DCL'S TO ASSIGN. /PROCESS_NAME is handed straight to
 * $CREPRC, which has the created process enter itself in the
 * EXECUTIVE's process table under that name before it activates the
 * image. DCL neither stores the name nor reports it back to anyone: it
 * prints the process ID the EXECUTIVE assigned. A name a process tells
 * only itself -- the rejected VMS_PRCNAM environment variable, CLAUDE.md
 * Rule 10 worked example 2 -- would satisfy every single-process test
 * and share nothing.
 *
 * Qualifiers honoured here: /PROCESS_NAME=, /INPUT=, /OUTPUT=, /ERROR=,
 * /UIC=[g,m] and /PRIVILEGES=(name,...) (vms-d31d). /UIC and /PRIVILEGES
 * are forwarded to $CREPRC's uic and prvadr arguments, which stamp them
 * onto the created process's EXECUTIVE row (vms_kif_setident): the
 * created process now runs under the requested UIC and privileges where
 * every other process and the Files-11 reference monitor can see them.
 * This is what lets SYSTEM's startup create a detached LOGINOUT that
 * holds SYSTEM's UIC [1,4] + SYSPRV/BYPASS and reads its own SYSUAF.
 *
 * PARTIAL GAP STILL TRACKED AS vms-69e: baspri (/PRIORITY) and the quota
 * set are still passed to $CREPRC as bare literals, so /PRIORITY on a
 * RUN/DETACHED is still parsed and discarded under %RUN-S-PROC_ID. That
 * remainder is asserted -- as it BEHAVES -- in P10 of
 * tests/qemu/test_syssvc_startup_service.c. /UIC and /PRIVILEGES were the
 * other half of vms-69e and are now honoured.
 */
static int run_detached(struct dcl_context *ctx, struct dcl_command *cmd,
                        const char *image_path)
{
    char in_path[1024]  = {0};
    char out_path[1024] = {0};
    char err_path[1024] = {0};

    const char *q;
    if ((q = run_qualifier_value(cmd, "INPUT")) && *q)
        dcl_resolve_path(ctx, q, in_path, sizeof(in_path));
    if ((q = run_qualifier_value(cmd, "OUTPUT")) && *q)
        dcl_resolve_path(ctx, q, out_path, sizeof(out_path));
    if ((q = run_qualifier_value(cmd, "ERROR")) && *q)
        dcl_resolve_path(ctx, q, err_path, sizeof(err_path));

    const char *prcnam = run_qualifier_value(cmd, "PROCESS_NAME");

    /* /UIC=[g,m] -> $CREPRC uic argument (0 = inherit the creator's). */
    uint32_t child_uic = 0;
    (void)run_parse_uic(cmd, &child_uic);

    /* /PRIVILEGES=(name,...) -> $CREPRC prvadr (NULL = inherit creator's). */
    uint64_t child_privs = 0;
    int have_privs = run_parse_privileges(cmd, &child_privs);

    struct dsc$descriptor_s img_d  = dsc_from_str(image_path);
    struct dsc$descriptor_s in_d   = dsc_from_str(in_path);
    struct dsc$descriptor_s out_d  = dsc_from_str(out_path);
    struct dsc$descriptor_s err_d  = dsc_from_str(err_path);
    struct dsc$descriptor_s prc_d  = dsc_from_str(prcnam);

    uint32_t pid = 0;
    uint32_t status = sys$creprc(&pid, &img_d,
                                 in_d.dsc$a_pointer  ? &in_d  : NULL,
                                 out_d.dsc$a_pointer ? &out_d : NULL,
                                 err_d.dsc$a_pointer ? &err_d : NULL,
                                 have_privs ? &child_privs : NULL, NULL,
                                 prc_d.dsc$a_pointer ? &prc_d : NULL,
                                 0, child_uic,
                                 0, PRC$M_DETACH);

    if (!(status & 1)) {
        run_creprc_failed(status);
        return status;
    }

    /* VMS reports the created process's ID, which is the EXECUTIVE's,
     * not a Linux pid $GETJPI could not resolve. */
    printf("%%RUN-S-PROC_ID, identification of created process is %08X\n",
           pid);
    return SS$_NORMAL;
}

/* ================================================================
 * P1 control region (vms-68f.v, docs/design-in-process-activation.md
 * Part II §A.1.1/§A.2.1).
 *
 * DCL's process-permanent state lives in P1, the process CONTROL region:
 * it is established once at process startup and survives every image
 * activation and rundown -- the opposite of P0, which imgact_activate()
 * maps per-image and tears down at rundown. Increments (i)-(iv) recorded
 * P0 extents, transitioned access mode and protected a CALLER-SUPPLIED
 * critical range, but nothing in the product had yet laid a real P1
 * control block or registered its extent -- vms_kif_p1_map (vms-6f1) sat
 * UNWIRED. This is that wiring: dcl_p1_init() reserves a page-aligned P1
 * block, stores a process-permanent marker in its critical page, and
 * registers [base,limit) with the executive so $GETJPI reports this
 * process's P1 region. dcl_activate_image() then hands the critical page
 * to imgact_activate(), so the design's critical-P1 mprotect (§A.2.3(b))
 * protects a REAL live DCL datum for the duration of an in-process image,
 * not the test's stand-in.
 * ================================================================ */
static uint64_t g_p1_base;          /* 0 until dcl_p1_init established it */
static uint64_t g_p1_limit;
static uint64_t g_p1_crit_base;
static uint64_t g_p1_crit_limit;

void dcl_p1_init(void)
{
    if (g_p1_base)                  /* idempotent: established once */
        return;

    /* 4096 is the page size assumed tree-wide (imgact.c's IMGACT_PGSZ and the
     * P0-window math in sys_imgact.c both hardcode it); use the same constant
     * rather than sysconf so this block's geometry matches the loader's. */
    const unsigned long pg = 4096;
    size_t sz = pg * 2;             /* page 0: critical; page 1: mutable */

    void *blk = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (blk == MAP_FAILED)
        return;                     /* no P1 window: RUN passes NULL critical */

    /* Page 0 carries a process-permanent marker so its read-only
     * protection during an activation guards a real datum, not an empty
     * page -- the enforced half of the access-mode model has something to
     * enforce. */
    *(volatile uint64_t *)blk = 0x3150584D564FULL;   /* "OVMXP1" LE */

    g_p1_base       = (uint64_t)(uintptr_t)blk;
    g_p1_limit      = g_p1_base + sz;
    g_p1_crit_base  = g_p1_base;
    g_p1_crit_limit = g_p1_base + (uint64_t)pg;

    /* Register the extent with the executive. Best-effort / INV-6: with no
     * /dev/vms this returns SS$_NOSUCHDEV and we hold the block WITHOUT an
     * executive record rather than faking one -- $GETJPI simply reports no
     * P1 extent, which is honest (the executive is absent). */
    (void)vms_kif_p1_map(g_p1_base, g_p1_limit);
}

int dcl_p1_critical_range(uint64_t *base, uint64_t *limit)
{
    if (!g_p1_base)
        return 0;
    if (base)
        *base = g_p1_crit_base;
    if (limit)
        *limit = g_p1_crit_limit;
    return 1;
}

/*
 * dcl_resolve_activatable - resolve an image spec to an activatable Linux path.
 *
 * VMS activates an image with READ access + EXECUTE file protection (not a Unix
 * execute bit), and `RUN FOO` / a foreign command run the HIGHEST version of the
 * file. But an OVMX image produced by LINK.EXE through RMS lands on the Linux
 * backing store as "FOO.EXE;1" (sys$create mints a ;version) and frequently
 * without a Unix +x bit. The old up-front access(X_OK) gate therefore refused to
 * activate a freshly linked image -- which is exactly what a self-host build
 * (MMK driving RUN of the produced image) must do. This resolver accepts a readable file (imgact_activate reads it),
 * fills in a missing .EXE type, and resolves the highest ;version.
 *
 * `resolved` is filled from `linux_path` (already run through dcl_resolve_path).
 * Returns 1 on success, 0 if nothing activatable was found.
 */
static int dcl_try_readable(const char *p, char *out, size_t sz)
{
    if (access(p, R_OK) == 0 && access(p, X_OK) != 0) {
        /* readable but not +x: only accept a regular file (an OVMX image),
         * never a directory that happens to be readable. */
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
            return 0;
    } else if (access(p, R_OK) != 0) {
        return 0;
    }
    strncpy(out, p, sz - 1);
    out[sz - 1] = '\0';
    return 1;
}

static int dcl_highest_version(const char *path, char *out, size_t sz)
{
    const char *slash = strrchr(path, '/');
    if (!slash) return 0;
    char dir[1024], leaf[512];
    size_t dl = (size_t)(slash - path);
    if (dl >= sizeof(dir)) return 0;
    memcpy(dir, path, dl);
    dir[dl] = '\0';
    strncpy(leaf, slash + 1, sizeof(leaf) - 1);
    leaf[sizeof(leaf) - 1] = '\0';
    size_t ll = strlen(leaf);
    if (ll == 0) return 0;

    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    long best = -1;
    char bestname[512] = "";
    while ((e = readdir(d)) != NULL) {
        const char *semi = strrchr(e->d_name, ';');
        if (!semi) continue;
        if ((size_t)(semi - e->d_name) != ll) continue;
        if (strncasecmp(e->d_name, leaf, ll) != 0) continue;
        /* The part after ';' must be a PURE version number. Without this, a
         * "<leaf>;N.rms_meta" / ".rms_idx" RMS sidecar (which shares the leaf
         * and whose atol() also yields N) can be picked instead of the real
         * image, and imgact then opens a 20-byte non-ELF sidecar (vms-615). */
        const char *vs = semi + 1;
        if (!*vs) continue;
        int alldig = 1;
        for (const char *q = vs; *q; q++)
            if (*q < '0' || *q > '9') { alldig = 0; break; }
        if (!alldig) continue;
        long v = atol(vs);
        if (v > best) {
            best = v;
            strncpy(bestname, e->d_name, sizeof(bestname) - 1);
            bestname[sizeof(bestname) - 1] = '\0';
        }
    }
    closedir(d);
    if (best < 0) return 0;
    snprintf(out, sz, "%s/%s", dir, bestname);
    return 1;
}

/* KEYED ON OVMX_HAVE_ACP, NOT __linux__ (vms-329): the coupled VAX cutover
 * retired the netbsd-vax /vms VFS mount, so the legacy opendir()/access()
 * resolver below can no longer find ANY image on that substrate -- DCL must
 * resolve activatable images through the executive ACP there exactly as it does
 * on Linux. dcl_rms_read_open()/rms_file_attr are already substrate-neutral. */
#if defined(OVMX_HAVE_ACP)
/* True if the final name component of a VMS filespec already carries a ".type"
 * (so ".EXE" must NOT be appended). Scans past the device/directory. */
static int dcl_spec_has_type(const char *spec)
{
    const char *base = spec;
    for (const char *p = spec; *p; p++)
        if (*p == ':' || *p == ']' || *p == '>' || *p == '/')
            base = p + 1;
    return strchr(base, '.') != NULL;
}

/*
 * dcl_resolve_activatable_acp - resolve an image spec to an activatable path
 * THROUGH the executive Files-11 (ODS-2) ACP (vms-5f0, epic vms-208 atomic flip).
 *
 * With the /vms passthrough retired, an image lives only on the mounted ODS-2
 * SYS$DISK. This probes its presence the VMS way: dcl_rms_attr()/rms_file_attr()
 * compose the ODS-2 search-list candidates (SYS$SYSTEM: -> [SYS0.SYSEXE] +
 * [SYS0.SYSCOMMON.SYSEXE]) and IO$_ACCESS each over /dev/vms -- the SAME
 * presence path RMS $OPEN and DIRECTORY/FULL already use -- never opendir()/
 * access() on /vms.
 *
 * On a hit, `resolved` is filled with the path DCL / $CREPRC / IMGACT activate
 * from: the boot-staged copy of a first-hop SYS$SYSTEM image (the POSIX home
 * the Linux kernel execve's; IMGACT still reads the GENUINE bytes off the
 * volume via the ACP, imgsrc_map_staged), else the on-volume path IMGACT reads
 * in-process via the ACP. $CREPRC's own ovmx_boot_stage_exec_path rewrite maps
 * an on-volume SYS$SYSTEM path to the staged copy too, so RUN/DETACHED works
 * either way.
 *
 * Returns 1 when resolved via the ACP. Returns 0 with *acp_usable = 1 when the
 * ACP is present but the image is genuinely absent (the caller reports an
 * honest %DCL-E-IVIMAGE; NO /vms fallback -- INV-6). Returns 0 with
 * *acp_usable = 0 when no ACP-mounted SYS$DISK is reachable (no /dev/vms -- the
 * plain host ctest), so the caller runs the legacy /vms resolver unchanged.
 */
static int dcl_resolve_activatable_acp(struct dcl_context *ctx,
                                       const char *vms_spec,
                                       const char *linux_path,
                                       char *resolved, size_t sz,
                                       int *acp_usable)
{
    *acp_usable = 0;

    /* NO EXECUTIVE => this ACP path does not apply: defer to the legacy resolver
     * (vms-104). With /dev/vms absent (the plain host ctest / the MMK
     * self-host toolchain ctests), rms_file_attr answers from a POSIX stat, NOT the ACP --
     * so dcl_rms_attr would report RMS$_NORMAL for SYS$SYSTEM:TCC.EXE and this
     * function would then try to stage it OVER an ACP that isn't there and fail,
     * turning a resolvable host-path image into a false %DCL-E-IVIMAGE. The flip
     * only retires /vms on the RUNTIME path (real /dev/vms); the legacy
     * SYS$SYSTEM:/SYS$SHARE: -> /vms POSIX resolution below is CORRECT and
     * expected when no executive is present (INV-6 governs the ACP-live path,
     * enforced by the ACP branch, not this host-defer). */
    if (rms_executive_absent()) {
        *acp_usable = 0;
        return 0;
    }

    const char *exts[2] = { "", ".EXE" };
    int nexts = dcl_spec_has_type(vms_spec) ? 1 : 2;

    for (int e = 0; e < nexts; e++) {
        char trial[1056];
        snprintf(trial, sizeof(trial), "%s%s", vms_spec, exts[e]);

        struct rms_fileattr at;
        uint32_t st = dcl_rms_attr(ctx, trial, &at);

        if (st == RMS$_NORMAL) {
            *acp_usable = 1;
            /* on-volume Linux path carrying the type we matched with */
            char lp[1024];
            snprintf(lp, sizeof(lp), "%s%s", linux_path, exts[e]);
            char staged[1024];
            /* (1) Booted runtime: the first-hop SYS$SYSTEM image was already
             * read off the volume over the ACP and staged to a POSIX file by
             * ovmx_init's boot bridge -- use it directly. */
            if (ovmx_boot_stage_exec_path(lp, staged, sizeof(staged)) &&
                access(staged, X_OK) == 0) {
                strncpy(resolved, staged, sz - 1);
                resolved[sz - 1] = '\0';
                return 1;
            }
            /* (2) Not boot-staged (a test harness, or a tool outside the boot
             * set such as the self-host TCC/LIBRARIAN/LINK.EXE, or an app like
             * SYS$SYSTEM:PARTS.EXE the first time `$ PARTS` runs): read the
             * GENUINE bytes off the ODS-2 volume THROUGH THE ACP now and stage
             * them to a POSIX home (the same read the boot bridge does, done
             * lazily). The bytes come from IO$_READVBLK over /dev/vms, NEVER a
             * /vms passthrough read (vms-104, Rule 9 / INV-6). A native musl
             * bootstrap tool (no OVMX symbol vector) is then execve()d off this
             * staged copy by dcl_activate_image's fork fallback; a real OVMX
             * symbol-vector image staged the same way is IMGACT-activated (its
             * PT_INTERP is opened by the kernel, imgsrc_map_staged re-reads it
             * over the ACP) -- imgact_activate makes that native-vs-image call
             * from the ELF, so ONE genuine ACP-sourced copy serves both.
             *
             * PER-USER PRIVATE STAGING (vms-a86f). LOGINOUT setuid()s a session
             * onto its SYSUAF UIC, so this runs as a NON-ROOT process (SYSTEM is
             * uid 4). The shared OVMX_BOOT_STAGE_DIR is root-owned 0755, so a
             * non-root session cannot create a file in it -- lazily staging into
             * the shared directory failed EACCES, which is exactly why the PARTS
             * demo went red. Stage instead into OVMX_BOOT_STAGE_DIR "/<uid>/", a
             * directory the activating process OWNS (created 0700): secure (no
             * world-writable plant hole) and writable by the session. A copy a
             * prior invocation already staged there is reused. (The deeper end
             * state is executive-mediated staging, vms-040; per-user-private is
             * the faithful, secure, in-scope fix.) */
            char user_dir[1024];
            if (ovmx_boot_stage_user_path(lp, staged, sizeof(staged),
                                          (unsigned long)getuid())) {
                /* Reuse an already-staged per-user copy (repeat activation). */
                if (access(staged, X_OK) == 0) {
                    strncpy(resolved, staged, sz - 1);
                    resolved[sz - 1] = '\0';
                    return 1;
                }
                /* Ensure the shared root (best-effort; PID 1 makes it) and the
                 * per-user private subdirectory (0700, owned by this uid). */
                (void)mkdir(OVMX_BOOT_STAGE_DIR, 0755);   /* EEXIST/EACCES fine */
                if (ovmx_boot_stage_user_dir(user_dir, sizeof(user_dir),
                                             (unsigned long)getuid()))
                    (void)mkdir(user_dir, 0700);          /* EEXIST is fine */
                if (dcl_rms_stage(ctx, trial, staged) == RMS$_NORMAL &&
                    access(staged, X_OK) == 0) {
                    strncpy(resolved, staged, sz - 1);
                    resolved[sz - 1] = '\0';
                    return 1;
                }
            }
            /* ACP confirmed the image is present but it could not be staged off
             * the volume. Fail HONESTLY -- do NOT read it off /vms (INV-6). The
             * caller reports %DCL-E-IVIMAGE. */
            return 0;
        }
        if (st == RMS$_ACC) {
            /* The ACP could not answer at all (no /dev/vms, no ACP-mounted
             * SYS$DISK) -- NOT a "file absent" answer. Defer to the legacy
             * resolver so the plain host ctest keeps working. */
            *acp_usable = 0;
            return 0;
        }
        /* RMS$_FNF (and other per-file errors): the ACP answered and this
         * spelling is absent. Keep probing (the .EXE default), and remember the
         * ACP is present so all-miss fails honestly with no /vms fallback. */
        *acp_usable = 1;
    }
    return 0;   /* ACP present, every spelling absent: honest miss (INV-6) */
}
#endif /* OVMX_HAVE_ACP */

int dcl_resolve_activatable(struct dcl_context *ctx, const char *vms_spec,
                            const char *linux_path, char *resolved, size_t sz)
{
#if defined(OVMX_HAVE_ACP)
    /* ATOMIC FLIP (vms-5f0): the image lives on the genuine ODS-2 SYS$DISK, not
     * the retired /vms passthrough. When the executive Files-11 ACP is present,
     * resolve THROUGH it and NEVER fall back to a /vms opendir()/access() probe
     * (INV-6). Only when no ACP-mounted SYS$DISK is reachable (no /dev/vms, the
     * plain host ctest) does the legacy resolver below run. */
    {
        int acp_usable = 0;
        if (dcl_resolve_activatable_acp(ctx, vms_spec, linux_path,
                                        resolved, sz, &acp_usable))
            return 1;
        if (acp_usable)
            return 0;   /* ACP present, image genuinely absent: honest miss */
    }
#else
    (void)ctx; (void)vms_spec;
#endif

    if (dcl_try_readable(linux_path, resolved, sz)) return 1;

    char cand[1024];
    snprintf(cand, sizeof(cand), "%s.exe", linux_path);
    if (dcl_try_readable(cand, resolved, sz)) return 1;
    snprintf(cand, sizeof(cand), "%s.EXE", linux_path);
    if (dcl_try_readable(cand, resolved, sz)) return 1;

    /* RMS ;version resolution: highest "<leaf>;N" in the directory. */
    if (dcl_highest_version(linux_path, cand, sizeof(cand)) &&
        dcl_try_readable(cand, resolved, sz))
        return 1;
    snprintf(cand, sizeof(cand), "%s.EXE", linux_path);
    if (dcl_highest_version(cand, cand, sizeof(cand)) &&
        dcl_try_readable(cand, resolved, sz))
        return 1;
    return 0;
}

/*
 * SYS$INPUT-from-procedure (vms-1a9). See dcl_cmd.h for the VMS-behaviour
 * citation. When an image is activated inside a command procedure, its
 * SYS$INPUT is the procedure's remaining data lines (those NOT beginning with
 * '$'), up to the next '$'-command or EOF. We gather that block into an
 * anonymous temp file, reposition the procedure stream to the next '$'-line so
 * the parent DCL loop resumes there, and point fd 0 at the block. SYS$INPUT
 * resolves to fd 0 (sys_assign.c), so both the in-process image and a
 * fork()+execve() child read the procedure lines. Interactive DCL leaves the
 * terminal on fd 0.
 */
void dcl_sysinput_setup(struct dcl_context *ctx, struct dcl_sysinput *si)
{
    si->saved_fd0 = -1;
    if (!ctx || ctx->proc_depth < 0)
        return;                     /* interactive: SYS$INPUT = terminal */
    FILE *fp = ctx->proc_stack[ctx->proc_depth].fp;
    if (!fp)
        return;

    /* tmpfile() is an unlinked, auto-deleted regular file: no pipe-buffer
     * deadlock even if the image never reads, and an honest EOF when the data
     * block is empty (the RUN line is immediately followed by a '$'-line). If
     * it cannot be created we fail open -- leave SYS$INPUT untouched rather
     * than fake a stream (INV-DCL). */
    FILE *tf = tmpfile();
    if (!tf)
        return;

    char line[DCL_MAX_LINE];
    long pos = ftell(fp);
    /* DECK/EOD explicit in-stream data (vms-3983). Normally the input block
     * ends at the first '$'-command line; DECK suspends that so a block may
     * itself contain '$'-lines, ending only at the EOD sentinel. Reference:
     * DCL Dictionary, "DECK" and "EOD". Default sentinel is a line beginning
     * "$EOD"; DECK/DOLLAR=string names a different one. */
    int  deck_mode = 0;
    char deck_term[48] = "EOD";
    while (fgets(line, sizeof(line), fp)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;

        if (deck_mode) {
            /* In a deck, only the EOD sentinel line ends the block; every other
             * line -- including '$'-lines -- is data. */
            if (*p == '$') {
                const char *q = p + 1;
                while (*q == ' ' || *q == '\t') q++;
                size_t tl = strlen(deck_term);
                if (strncasecmp(q, deck_term, tl) == 0 &&
                    (q[tl] == '\0' || q[tl] == '\n' ||
                     q[tl] == ' '  || q[tl] == '\t')) {
                    deck_mode = 0;   /* consume the EOD line, feed nothing */
                    ctx->proc_stack[ctx->proc_depth].line_number++;
                    pos = ftell(fp);
                    continue;
                }
            }
            /* fall through to feed the data line below */
        } else if (*p == '$') {
            /* A '$ DECK' opens an explicit data block; any other '$'-line is
             * the next DCL command and terminates the image's input. */
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (strncasecmp(q, "DECK", 4) == 0 &&
                (q[4] == '\0' || q[4] == '\n' || q[4] == ' ' ||
                 q[4] == '\t' || q[4] == '/')) {
                /* Optional /DOLLAR=string sets a custom sentinel. */
                const char *slash = strchr(q, '/');
                if (slash) {
                    const char *eq = strchr(slash, '=');
                    if (eq) {
                        eq++;
                        while (*eq == ' ' || *eq == '\t' || *eq == '"') eq++;
                        size_t di = 0;
                        while (*eq && *eq != ' ' && *eq != '\t' &&
                               *eq != '\n' && *eq != '"' &&
                               di < sizeof(deck_term) - 1)
                            deck_term[di++] = *eq++;
                        deck_term[di] = '\0';
                        if (di == 0) strcpy(deck_term, "EOD");
                    }
                }
                deck_mode = 1;   /* consume the DECK line, feed nothing */
                ctx->proc_stack[ctx->proc_depth].line_number++;
                pos = ftell(fp);
                continue;
            }
            /* Next DCL command terminates the image's input. Put the stream
             * back at the start of this line so the parent loop reads it. */
            fseek(fp, pos, SEEK_SET);
            break;
        }
        /* Data line fed to the image's SYS$INPUT. VMS DCL performs apostrophe
         * ('symbol') substitution on command-procedure data lines exactly as it
         * does on command lines (VMS DCL Concepts / User's Manual, "Symbol
         * Substitution"): this is what lets the classic
         *     $ RUN SYS$SYSTEM:AUTHORIZE
         *     MODIFY SYSTEM/PASSWORD='NEWPW'
         * idiom pass a run-time symbol into a utility's SYS$INPUT (vms-963).
         * Reuse the SAME dcl_sym_substitute() DCL applies to command lines so
         * the two agree on every edge -- a lone apostrophe is kept, ''sym' is
         * iterative, &sym and "..." string rules match. A data line that
         * contains no apostrophe/ampersand passes through byte-for-byte
         * (dcl_sym_substitute copies non-marker text verbatim, including the
         * trailing newline), so the pre-existing vms-1a9 behavior for
         * substitution-free blocks is unchanged. */
        char subst[DCL_MAX_LINE];
        dcl_sym_substitute(line, subst, sizeof(subst));
        fputs(subst, tf);
        ctx->proc_stack[ctx->proc_depth].line_number++;
        pos = ftell(fp);
    }
    fflush(tf);
    rewind(tf);

    int tfd = fileno(tf);
    si->saved_fd0 = dup(0);
    if (si->saved_fd0 < 0 || dup2(tfd, 0) < 0) {
        /* Could not install the redirection: undo any partial state and leave
         * fd 0 as it was. */
        if (si->saved_fd0 >= 0) {
            close(si->saved_fd0);
            si->saved_fd0 = -1;
        }
        fclose(tf);
        return;
    }
    /* fd 0 now shares the temp file's open description; closing tf's own fd
     * does not disturb fd 0, and the unlinked file goes away when fd 0 is
     * later restored. */
    fclose(tf);
}

void dcl_sysinput_restore(struct dcl_sysinput *si)
{
    if (si->saved_fd0 < 0)
        return;
    dup2(si->saved_fd0, 0);
    close(si->saved_fd0);
    si->saved_fd0 = -1;
    clearerr(stdin);
}

/*
 * dcl_activate_image - fork/exec a resolved image path and wait for it,
 * surfacing a nonzero exit or fatal signal as SS$_ABORT.
 *
 * Shared by RUN (argv = {linux_path, NULL} -- RUN has never passed
 * parameters to the image) and foreign-command dispatch in dcl_exec.c
 * (argv = {linux_path, P1, ..., P8, NULL}). Extracted from cmd_run's
 * body verbatim so this behavior change is refactor-only for RUN.
 *
 * SYS$INPUT wiring (vms-1a9): when activated from within a command procedure,
 * the image's SYS$INPUT is the procedure's following data lines. Set up before
 * activation and restored after, so it covers BOTH the in-process image and the
 * fork()+execve() fallback (both inherit fd 0).
 */
static int dcl_activate_image_inner(struct dcl_context *ctx,
                                    const char *display_name,
                                    const char *linux_path, char *argv[]);

int dcl_activate_image(struct dcl_context *ctx, const char *display_name,
                       const char *linux_path, char *argv[])
{
    struct dcl_sysinput si;
    dcl_sysinput_setup(ctx, &si);
    int rc = dcl_activate_image_inner(ctx, display_name, linux_path, argv);
    dcl_sysinput_restore(&si);
    return rc;
}

static int dcl_activate_image_inner(struct dcl_context *ctx,
                                    const char *display_name,
                                    const char *linux_path, char *argv[])
{
    /*
     * IN-PROCESS IMAGE ACTIVATION (vms-68f increment iv,
     * docs/design-in-process-activation.md Part II). On OpenVMS, RUN / a
     * foreign command / a DCL utility activates the image IN this process --
     * no new PID -- and image rundown returns here. This is the dispatch
     * point: imgact_activate() runs the image in-process when it is
     * in-process-eligible and a real /dev/vms is present. The in-process path
     * now takes (vms-db2): increment iv's marker image entered through the
     * (a0,a1) function ABI; a REAL image entered through the SysV auxv `_start`
     * ABI whose .vms$imp imports all bind to the ALREADY-RESIDENT producer; AND
     * (the EXTERNAL-image flip) such an image carrying a PT_INTERP that names
     * the OVMX loader (IMGACT.EXE) -- a genuinely external LINK.EXE executable,
     * whose interpreter in-process activation itself is. It ends by calling the
     * resident SYS$EXIT (imgact_image_exit), which returns control HERE in the
     * same process. It still returns SS$_UNSUPPORTED for an image with a FOREIGN
     * PT_INTERP (a real ld.so, a shebang), an own PT_TLS, a symbolic (PLT)
     * reloc, or an import naming a NON-resident producer -- the full 55 KB
     * loader re-homing (PT_TLS/DTV, non-resident producer mapping) is the
     * deferred remainder on vms-db2 -- and SS$_NOSUCHDEV with no executive, in which cases
     * activation falls through to the fork()+execve() model below (design
     * §A.6.6: the fork stays the fallback for those classes; SPAWN / RUN/DETACHED
     * / PIPE never take this path -- they create genuinely new processes).
     * The eligibility decision is made from the ELF alone, before any
     * executive call, so the fork fallback for real images does not depend on
     * /dev/vms. DCL's REAL critical-P1 range (dcl_p1_init, above) is handed to
     * imgact_activate() so the design's §A.2.3(b) mprotect protects DCL's own
     * process-permanent P1 page for the duration of the in-process image -- a
     * wild write from the User-mode image faults instead of corrupting DCL. If
     * dcl_p1_init() could not establish a P1 block, the range is NULL and this
     * behaves exactly as increment iv did.
     */
    {
        int image_rc = 0;
        struct imgact_critp1 cp1;
        const struct imgact_critp1 *cpp = NULL;
        if (dcl_p1_critical_range(&cp1.base, &cp1.limit))
            cpp = &cp1;
        uint32_t ia = imgact_activate(linux_path, 0, 0, cpp, &image_rc);
        /*
         * ONLY a genuine in-process run bypasses the fork: SS$_NORMAL (the
         * image ran and returned) or SS$_ACCVIO (it ran, faulted, and was run
         * down; DCL survives). EVERY other status -- SS$_UNSUPPORTED (not an
         * in-process image: a real utility, a #!/bin/sh script, a dynamic
         * image), SS$_NOSUCHDEV (no executive), SS$_NOSUCHFILE/SS$_BADPARAM
         * (imgact could not load it) -- means the image was NOT activated
         * in-process, so activation falls through to the fork()+execve() model
         * below, which handles all of those (a shebang script included).
         */
        if (ia == SS$_NORMAL || ia == SS$_ACCVIO) {
            if (ia == SS$_NORMAL) {
                /* The image's completion $STATUS is owned by the executive
                 * (vms-f60d, design §3.4): the in-process image recorded its
                 * full VMS condition value at SYS$EXIT via vms_kif_setexit, so
                 * read it back and make it DCL's $STATUS -- the real condition
                 * value, not a success/fail collapsed from the POSIX exit code.
                 * If no executive recorded one (no /dev/vms -> has_exited == 0),
                 * fall back to the image_rc verdict. */
                uint32_t cond = 0;
                int exited = 0;
                uint32_t gx = vms_kif_getexit(&cond, &exited);
                if (gx == SS$_NORMAL && exited) {
                    if (!(cond & 1))
                        dcl_error("DCL", (int)(cond & 7), "ABORT",
                                  "image %s exited with error status %%X%08X",
                                  display_name, (unsigned)cond);
                    return cond;
                }
                if (image_rc != 0) {
                    dcl_error("DCL", 2, "ABORT",
                              "image %s exited with error status %%X%08X",
                              display_name, (unsigned)image_rc);
                    return SS$_ABORT;
                }
                return SS$_NORMAL;
            }
            return SS$_ABORT;   /* SS$_ACCVIO: the image faulted and was run down */
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child */
        /*
         * IMAGE ACTIVATION CONTINUES THIS PROCESS'S IDENTITY (vms-4d7,
         * Option B). On OpenVMS, RUN / a foreign command / a DCL utility
         * activates the image IN the current process -- same PID, same
         * UIC, same privileges. OVMX fork()s+execve()s instead, so without
         * this the image would auto-register a fresh PCB and derive its own
         * privilege mask (that is why SYSTEM could not RUN AUTHORIZE: the
         * child never held SYSPRV). We are still DCL here -- the fork has
         * not yet been replaced -- so our real_parent is DCL and the
         * executive can share DCL's VMS PID, UIC, user name and privileges
         * onto this task. The PCB is keyed on the thread group and survives
         * the execve below, so the activated image inherits it.
         *
         * This is ONLY for image activation. SPAWN / RUN/DETACHED / $CREPRC
         * create genuinely new VMS processes and do not call this.
         */
        (void)vms_kif_register_continue();
        execv(linux_path, argv);
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
            /* vms-17f9: a nonzero image exit must be SURFACED, not swallowed.
             * Previously this returned SS$_ABORT silently, so a RUN that
             * failed looked identical to one that succeeded (the de-risk that
             * hunted a "no output" RUN, docs/derisk-vms-530-imgact-qemu.md). */
            if (exit_code != 0)
                dcl_error("DCL", 2, "ABORT",
                          "image %s exited with error status %%X%08X",
                          display_name, (unsigned)exit_code);
            return (exit_code == 0) ? SS$_NORMAL : SS$_ABORT;
        }
        /* vms-17f9: a child killed by a signal (a crash) was silently dropped
         * here and cmd_run fell through to SS$_NORMAL, reporting success for an
         * image that never ran. Report it and fail. */
        if (WIFSIGNALED(wstatus)) {
            dcl_error("DCL", 4, "ABORT",
                      "image %s terminated abnormally (signal %d)",
                      display_name, WTERMSIG(wstatus));
            return SS$_ABORT;
        }
    } else {
        dcl_error("DCL", 4, "CREPRC", "cannot create process");
        return SS$_ABORT;
    }

    return SS$_NORMAL;
}

/*
 * dcl_exec_foreign_command - VMS "foreign command" dispatch.
 *
 * OpenVMS: "SYMBOL :== $image-spec" defines a foreign command. Typing
 * the bare symbol afterwards activates image-spec, with the rest of
 * the command line becoming the image's P1-P8 parameters -- the same
 * effect as "RUN image-spec" would have if RUN forwarded parameters.
 * A value of "$" alone (nothing after the dollar) defaults the image
 * name to the symbol name itself (HELP SET SYMBOL, foreign commands).
 *
 * cmd->params[0..7] IS P1-P8 (DCL_MAX_PARAMS == 8, one array), already
 * split out by the parser, so they are forwarded as argv[1..] to the
 * activated image.
 *
 * OVMX difference (documented, not silent): real DCL never parses a
 * foreign command's argument text at all -- it hands the whole raw
 * command tail to the image as one string via LIB$GET_FOREIGN, and
 * the image's own CLI callable routines (CLI$GET_VALUE etc.) pick
 * P1-P8 back out of that string. OVMX's parser has already tokenized
 * the line into cmd->params[]/cmd->qualifiers[] by the time dispatch
 * decides this is a foreign command, and OVMX has no LIB$GET_FOREIGN.
 * So this reconstructs a conventional argv from the parsed positional
 * parameters instead of forwarding one untouched string -- same P1-P8
 * values, delivered the Unix way. A "/qualifier" on a foreign-command
 * line was already diverted into cmd->qualifiers[] by the parser
 * before dispatch is decided, so it is not forwarded to the image;
 * this is a lane limitation to flag, not an intended semantic.
 */
int dcl_exec_foreign_command(struct dcl_context *ctx, struct dcl_command *cmd,
                             const char *symbol_value)
{
    const char *image_spec = symbol_value;
    while (*image_spec == ' ' || *image_spec == '\t') image_spec++;
    if (*image_spec == '\0') image_spec = cmd->verb;

    char linux_path[1024];
    char resolved_path[1024];
    dcl_resolve_path(ctx, image_spec, linux_path, sizeof(linux_path));

    /* Resolve to an activatable path: fills a missing .EXE type and the RMS
     * ;version, and accepts a readable OVMX image (not just a +x file) so a
     * freshly linked image activates -- the RUN path uses the same resolver. */
    if (dcl_resolve_activatable(ctx, image_spec, linux_path,
                                resolved_path, sizeof(resolved_path))) {
        strncpy(linux_path, resolved_path, sizeof(linux_path) - 1);
        linux_path[sizeof(linux_path) - 1] = '\0';
    } else {
        dcl_error("DCL", 2, "IVIMAGE",
                  "image not found - %s", image_spec);
        return SS$_NOSUCHFILE;
    }

    /* Build argv from the RAW command tail (cmd->raw_tail) when the parser
     * captured it -- this is the OpenVMS-faithful whole-line delivery: real DCL
     * hands the entire foreign-command tail to the image and the image's CRTL
     * splits it into argc/argv, so the DCL_MAX_PARAMS (P1-P8) cap does NOT apply
     * to a foreign command. OVMX's parser had previously tokenized only P1-P8,
     * which capped argv at 8 and made a native `LINK --executable --use A --use
     * B ... -o X Y` invocation (well over 8 tokens) impossible. We whitespace-
     * split the raw tail here instead; a value with embedded spaces is a known
     * lane limitation (real DCL/CRTL honour "quoted strings" -- OVMX does not
     * yet). Falls back to the tokenized params[] if raw_tail is empty. vms-615. */
#define DCL_FC_MAX_ARGV 128
    char *argv[DCL_FC_MAX_ARGV];
    int argc = 0;
    argv[argc++] = linux_path;

    char tailbuf[DCL_MAX_LINE];
    if (cmd->raw_tail[0] != '\0') {
        strncpy(tailbuf, cmd->raw_tail, sizeof(tailbuf) - 1);
        tailbuf[sizeof(tailbuf) - 1] = '\0';
        /* Whitespace-split, but honour "double quotes" the way DECC$CRTL does
         * when it parses a foreign-command line into argv: a quoted span is one
         * argument and the quotes are stripped (so `PARTS LOAD 5 "dev:file.dat"`
         * delivers a clean filename, and `FOO "hello"` delivers hello). Tokens
         * are compacted in place -- the write cursor never overtakes the read
         * cursor. */
        char *s = tailbuf;
        while (*s && argc < DCL_FC_MAX_ARGV - 1) {
            while (*s == ' ' || *s == '\t') s++;
            if (!*s) break;
            char *arg = s;
            char *w = s;
            int inq = 0;
            while (*s && (inq || (*s != ' ' && *s != '\t'))) {
                if (*s == '"') { inq = !inq; s++; continue; }
                *w++ = *s++;
            }
            if (*s) s++;          /* consume the delimiter */
            *w = '\0';
            argv[argc++] = arg;
        }
    } else {
        for (int i = 0; i < cmd->param_count && argc < DCL_FC_MAX_ARGV - 1; i++)
            argv[argc++] = cmd->params[i];
    }
    argv[argc] = NULL;

    /* Record the RAW foreign command tail so the activated image's
     * LIB$GET_FOREIGN can return it (vms-54e). On OpenVMS a foreign command
     * hands the whole untokenized tail to the image via the CLI, and the CLI
     * relationship is owned by the executive -- so OVMX records it in the
     * executive process context (vms_kif_setcli, vms-f60d), which the activated
     * image inherits from this PCB at REGISTER_CONTINUE time and reads back with
     * LIB$GET_FOREIGN -> vms_kif_getcli. This is the authoritative channel and
     * replaces the former Linux-env-var shim on the real runtime (design §3.2 /
     * §4a.3, conductor ruling: read the executive, not an env var; INV-6).
     *
     * When /dev/vms is UNREACHABLE (host unit tests, a dev build with no
     * executive) vms_kif_setcli fails (a non-success VMS status) -- there is
     * genuinely no executive CLI relationship to record. VMS_FOREIGN_CMD is
     * retained ONLY as
     * that no-executive fallback (inherited environment; NOT a claim the
     * executive succeeded), so the same env channel DCL already uses for VMS
     * process context (VMS_DEFAULT_DIR, ...) still delivers the tail off the
     * runtime. Set ONLY for a foreign command; RUN / DCL-driven utilities leave
     * both unset so their LIB$GET_FOREIGN correctly falls through to SYS$INPUT. */
    uint32_t cli_st = vms_kif_setcli(1, cmd->raw_tail);
    int have_exec = (cli_st & 1);   /* odd == the executive recorded it */
    if (!have_exec)
        setenv("VMS_FOREIGN_CMD", cmd->raw_tail, 1);
    int fc_status = dcl_activate_image(ctx, image_spec, linux_path, argv);
    if (!have_exec)
        unsetenv("VMS_FOREIGN_CMD");
    return fc_status;
#undef DCL_FC_MAX_ARGV
}

/*
 * RUN - Execute a program.
 */
int cmd_run(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* BEFORE the image parameter is examined: a qualifier OVMX cannot
     * honour is refused, not silently dropped. This is first because
     * "/UIC=[300,1]" is lexed into a qualifier plus a stray parameter,
     * so resolving the image first would report a fragment of the UIC
     * as a missing image and never mention the UIC. */
    {
        uint32_t refused = run_refuse_unhonourable(cmd);
        if (refused)
            return refused;
    }

    /* Which parameter is the image? With /UIC=[g,m] the DCL lexer splits
     * on the comma, so the "m]" half lands as a bare parameter that is NOT
     * the image (vms-d31d) -- skip it. Without /UIC this picks params[0]. */
    int stray = run_uic_stray_param(cmd);
    int img_idx = -1;
    for (int i = 0; i < cmd->param_count; i++) {
        if (i == stray) continue;
        if (cmd->params[i][0] == '\0') continue;
        img_idx = i;
        break;
    }
    if (img_idx < 0) {
        dcl_error("DCL", 2, "NOFILE", "missing image specification");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    char resolved_path[1024];
    dcl_resolve_path(ctx, cmd->params[img_idx], linux_path, sizeof(linux_path));

    /* Resolve to an activatable path (fills .EXE, resolves the RMS ;version,
     * accepts a readable OVMX image) -- shared with foreign-command dispatch. */
    if (dcl_resolve_activatable(ctx, cmd->params[img_idx], linux_path,
                                resolved_path, sizeof(resolved_path))) {
        strncpy(linux_path, resolved_path, sizeof(linux_path) - 1);
        linux_path[sizeof(linux_path) - 1] = '\0';
    } else {
        dcl_error("DCL", 2, "IVIMAGE",
                  "image not found - %s", cmd->params[img_idx]);
        return SS$_NOSUCHFILE;
    }

    /* /DETACHED creates a detached process -- a service -- instead of
     * running the image as a subprocess of this DCL. /UIC also selects a
     * detached process (HELP RUN Process: /UIC "Specifies that the created
     * process be a detached process"), so it takes the same path (vms-d31d). */
    if (run_has_qualifier(cmd, "DETACHED") || run_has_qualifier(cmd, "UIC"))
        return run_detached(ctx, cmd, linux_path);

    /* RUN has never forwarded parameters to the image (unlike a foreign
     * command -- see dcl_exec_foreign_command above); preserve that. */
    char *argv[2];
    argv[0] = linux_path;
    argv[1] = NULL;
    return dcl_activate_image(ctx, cmd->params[img_idx], linux_path, argv);
}

/*
 * SPAWN - Create a DCL subprocess.
 *
 * SPAWN                    — interactive DCL subprocess
 * SPAWN cmd                — execute single DCL command in subprocess
 * SPAWN /NOWAIT cmd        — run DCL subprocess in background
 * SPAWN /OUTPUT=file cmd   — redirect subprocess stdout to file
 */
int cmd_spawn(struct dcl_command *cmd)
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

    /*
     * SUBPROCESS NAME (vms-c17). VMS names a SPAWNed subprocess after the
     * CREATOR's process name plus a unique number -- SYSTEM_1, SYSTEM_2, ...
     * -- unless /PROCESS=name overrides it (OpenVMS DCL Dictionary, SPAWN:
     * "the subprocess name is composed of the same base name as the parent
     * process and a unique number"; oracle VAX1, OpenVMS VAX V7.3:
     * "%DCL-S-SPAWNED, process SYSTEM_1 spawned", cited in dcl_lexical.c and
     * tests/uat/vms_session_qemu.sh). The base is read from the creator's OWN
     * executive-held process name ($GETJPI self) so it is the name the system
     * knows this process by, not a self-declared string; the DCL context is a
     * fallback only. Capped so the "_N" suffix fits VMS_PRCNAM_SIZE.
     */
    struct dcl_context *spawn_ctx = dcl_get_context();
    const char *proc_name_q = dcl_qualifier_value(cmd, "PROCESS");
    char base_name[VMS_PRCNAM_SIZE] = {0};
    {
        struct vms_procinfo self_info;
        const char *src = NULL;
        if ((vms_kif_getjpi_self(&self_info) & 1) && self_info.prcnam[0])
            src = self_info.prcnam;
        else if (spawn_ctx && spawn_ctx->process_name[0])
            src = spawn_ctx->process_name;
        else if (spawn_ctx && spawn_ctx->username[0])
            src = spawn_ctx->username;
        else
            src = "SYSTEM";
        /* Leave room for "_65535" (6) + NUL in the 16-byte field. */
        strncpy(base_name, src, sizeof(base_name) - 7);
        base_name[sizeof(base_name) - 7] = '\0';
    }

    /*
     * CREATION HANDSHAKE (vms-c17). Only the child can enter itself in the
     * executive's process table -- the entry is keyed by ITS tgid, which
     * execve() does not change -- so the child registers, names itself, and
     * reports the outcome (status + final name) back over a pipe. SPAWN does
     * not return until that report arrives, so the subprocess genuinely EXISTS
     * (named, in the executive, visible to SHOW SYSTEM/SHOW USERS) the moment
     * SPAWN returns, exactly as $CREPRC does (sys_process.c). Registering here
     * is what makes the subprocess a real PCB whose job_id -- inherited from
     * this DCL's job -- marks it a SUBPROCESS; the previous bare fork()+execl()
     * left the child out of the table entirely (the INV-6 fabrication class).
     * The pipe is O_CLOEXEC, so it costs the activated image nothing.
     */
    struct spawn_report { uint32_t status; char name[VMS_PRCNAM_SIZE]; };
    int namefd[2] = { -1, -1 };
    if (pipe(namefd) < 0) {
        dcl_error("DCL", 4, "CREPRC", "cannot create process");
        return SS$_INSFMEM;
    }
    /* Not pipe2(O_CLOEXEC): keep the glibc/musl-portable form sys_process.c uses. */
    fcntl(namefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(namefd[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child process */
        struct spawn_report rep;
        memset(&rep, 0, sizeof(rep));
        close(namefd[0]);

        /*
         * Register + name in the executive BEFORE the exec and before any
         * I/O redirection (the executive keys on the pid, which execve() does
         * not change, so the name survives image activation with no userspace
         * carrier). The FIRST vms_kif_* call binds and registers this task
         * (kif_bind), deriving job_id from the real parent -- this DCL. Do NOT
         * bind a terminal: a subprocess is terminal-unbound (only the login
         * root calls VMS_IOCTL_SETTERM), which is what keeps it classified
         * SUBPROCESS rather than INTERACTIVE. The executive enforces name
         * uniqueness within the UIC group (SS$_DUPLNAM); loop until a free
         * "base_N" is found, or stop on any other error.
         */
        if (proc_name_q && proc_name_q[0]) {
            strncpy(rep.name, proc_name_q, sizeof(rep.name) - 1);
            rep.name[sizeof(rep.name) - 1] = '\0';
            rep.status = vms_kif_setprn(rep.name);
        } else {
            rep.status = SS$_DUPLNAM;
            for (int n = 1; n <= 65535; n++) {
                /* Format into a wide temp then copy bounded into the fixed
                 * wire field: base_name is capped above, but the compiler
                 * cannot prove it, so a direct snprintf trips
                 * -Werror=format-truncation. */
                char nm[64];
                snprintf(nm, sizeof(nm), "%s_%d", base_name, n);
                strncpy(rep.name, nm, sizeof(rep.name) - 1);
                rep.name[sizeof(rep.name) - 1] = '\0';
                rep.status = vms_kif_setprn(rep.name);
                if (rep.status & 1)
                    break;                 /* named */
                if (rep.status != SS$_DUPLNAM)
                    break;                 /* a real error, not a clash */
            }
        }

        /*
         * Confirm the registration TOOK, exactly as sys$creprc does: the FIRST
         * vms_kif_* call (the setprn above) binds and registers this task via
         * kif_bind, so a successful setprn means the PCB exists -- but read it
         * back with $GETJPI to be certain the row is there before we hand the
         * creator SS$_NORMAL. (Do NOT probe with access("/dev/vms",...): access
         * checks the REAL uid against the device mode and can fail on a node
         * the process can nonetheless open(O_RDWR) -- the false negative that
         * made an earlier revision take the no-executive path in-guest and run
         * the subprocess unregistered.)
         */
        if (rep.status & 1) {
            struct vms_procinfo self_pi;
            uint32_t g = vms_kif_getjpi_self(&self_pi);
            if (!(g & 1))
                rep.status = g;             /* registration did not stick */
        }

        /*
         * If registration could not be done, decide HONESTLY why. Only when
         * the executive is genuinely unreachable -- /dev/vms cannot be opened,
         * i.e. build/test tooling, never the product runtime (Rule 9: PID 1
         * refuses to boot without it) -- does the subprocess still run,
         * UNREGISTERED, the same "it needs no executive" path lib$spawn takes
         * (src/libvms/rtl/lib_misc.c). That is not the fabrication INV-6
         * forbids: nothing claims a PCB, and SHOW USERS honestly shows nothing
         * because nothing registered. With the executive PRESENT, a failed
         * registration is a real error and is reported as such.
         */
        if (!(rep.status & 1)) {
            int probe = open("/dev/vms", O_RDWR);
            if (probe < 0) {
                rep.status = SS$_NORMAL;    /* no executive: run unregistered */
                if (rep.name[0] == '\0') {
                    char nm[64];
                    snprintf(nm, sizeof(nm), "%s_1", base_name);
                    strncpy(rep.name, nm, sizeof(rep.name) - 1);
                    rep.name[sizeof(rep.name) - 1] = '\0';
                }
            } else {
                close(probe);               /* executive present: honest fail */
            }
        }

        /* Report back BEFORE exec so the CLOEXEC pipe carries the result even
         * though the exec then closes it. A tiny (< PIPE_BUF) write cannot
         * block the child. */
        {
            const char *p = (const char *)&rep;
            size_t left = sizeof(rep);
            while (left > 0) {
                ssize_t w = write(namefd[1], p, left);
                if (w < 0) { if (errno == EINTR) continue; break; }
                p += w; left -= (size_t)w;
            }
        }
        close(namefd[1]);

        /* A subprocess the executive never entered describes nothing the
         * creator can see -- fail honestly rather than activate an image that
         * would make SPAWN's success a lie (INV-6). */
        if (!(rep.status & 1))
            _exit(126);

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
        /* Parent: wait for the child's registration report before returning,
         * so the subprocess exists in the executive the moment SPAWN returns. */
        struct spawn_report rep;
        memset(&rep, 0, sizeof(rep));
        close(namefd[1]);
        {
            char *p = (char *)&rep;
            size_t left = sizeof(rep);
            while (left > 0) {
                ssize_t r = read(namefd[0], p, left);
                if (r < 0) { if (errno == EINTR) continue; break; }
                if (r == 0) break;         /* child died before reporting */
                p += r; left -= (size_t)r;
            }
            close(namefd[0]);
            if (left != 0 || !(rep.status & 1)) {
                /* The child could not register -- reap it and report honestly. */
                int wst;
                while (waitpid(pid, &wst, 0) < 0 && errno == EINTR)
                    ;
                dcl_error("DCL", 4, "CREPRC", "cannot create process");
                return SS$_ABORT;
            }
        }

        if (nowait) {
            /* /NOWAIT: VMS answers "%DCL-S-SPAWNED, process <name> spawned"
             * (severity S, the process NAME) -- not the Linux pid. Oracle
             * VAX1, OpenVMS VAX V7.3 (cited above). */
            printf("%%DCL-S-SPAWNED, process %s spawned\n", rep.name);
        } else {
            /* Attached SPAWN: run the subprocess and wait. The authentic
             * ATTACHED/RETURNED handoff messages belong to the terminal
             * ATTACH work, which is out of scope for vms-c17 -- so this path
             * stays silent, as it was, while now registering a visible,
             * named subprocess for the duration it runs. */
            extern volatile sig_atomic_t dcl_running_child;
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
        close(namefd[0]);
        close(namefd[1]);
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
int cmd_pipe(struct dcl_command *cmd)
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
int cmd_exit(struct dcl_command *cmd)
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
 * stop_target_qual - the value of /IDENTIFICATION, given by any
 * abbreviation of two characters or more ("/ID" is how the Dictionary's
 * own examples write it). Same pattern as show_process_target_qual()
 * (dcl_cmd_show.c): dcl_qualifier_value() only matches an exact name, and
 * STOP has no CDU qualifier table of its own to canonicalise abbreviations
 * through dcl_validate_qualifiers() (see q_stop[] / dcl_builtin.c -- STOP's
 * table declares ONLY IDENTIFICATION, so a table lookup can't shortcut this
 * either without duplicating the same prefix rule).
 */
static const char *stop_target_qual(const struct dcl_command *cmd)
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
 * STOP - INV-DCL FACADE KILL (vms-1a8, docs/design-dcl-fidelity.md sec 5
 * Phase 2). Was `(void)cmd; ctx->exit_requested = 1; return SS$_NORMAL;` --
 * it ignored its target parameter and /IDENTIFICATION qualifier entirely
 * and unconditionally self-exited the CURRENT DCL context, claiming
 * SS$_NORMAL for a named target it never looked at.
 *
 * SCOPE (clean-room, OpenVMS DCL Dictionary -- "STOP" entry,
 * https://www.mrynet.com/FTP/operatingsystems/VMS/docs/ssb71/9996/
 * 9996p062.htm): this covers the two process-TARGET forms the Dictionary
 * documents -- a process-name parameter ("must share the same group
 * number in its UIC as the current process") and /IDENTIFICATION=pid ("an
 * alternative to the process-name parameter", reaching outside the
 * caller's group) -- plus the bare no-target form, which the Dictionary
 * describes as abnormal termination of the CURRENT image/command levels,
 * unstacking to the DCL prompt: precisely the ctx->exit_requested path
 * already here, now reached only when there is genuinely no target.
 * STOP/QUEUE, STOP/CPU and STOP/NETWORK are DIFFERENT Dictionary entries
 * (queue and cluster-node control, not process control) and are OUT OF
 * SCOPE: neither was ever implemented, so this file has never claimed
 * them, and STOP has no qualifier table entry for /QUEUE or /CPU or
 * /NETWORK below -- typing one now draws the authentic %DCL-W-IVQUAL
 * rather than silent acceptance, which is the honest answer INV-DCL
 * requires when a form is not implemented.
 *
 * The two target forms both go through sys$delprc (src/libvms/syssvc/
 * sys_process.c), which is what now actually resolves the name/pid
 * against the EXECUTIVE process table and enforces the Dictionary's
 * GROUP/WORLD privilege rule -- see that function's own comment for the
 * full citation and the reasoning. STOP is a thin DCL-syntax wrapper over
 * it, the same relationship RUN has to sys$creprc above.
 */
int cmd_stop(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    const char *idval = stop_target_qual(cmd);
    int have_name = (cmd->param_count >= 1 && cmd->params[0][0] != '\0');

    if (!idval && !have_name) {
        /* No target: abnormal termination of the current image/command
         * levels, unstacking to DCL -- unchanged from before this item. */
        ctx->exit_requested = 1;
        ctx->exit_status = 0;
        return SS$_NORMAL;
    }

    struct dsc$descriptor_s namdsc;
    const struct dsc$descriptor_s *namdsc_p = NULL;
    char upper_name[VMS_PRCNAM_XFER];
    uint32_t pid_val = 0;
    const uint32_t *pid_p = NULL;

    if (idval) {
        /*
         * /IDENTIFICATION overrides the process-name parameter (Dictionary,
         * verbatim: "an alternative to the process-name parameter") -- so
         * this branch is taken whenever /ID was specified, whether or not
         * a name parameter was ALSO given, matching SHOW PROCESS's already-
         * established precedence (dcl_cmd_show.c cmd_show_process()).
         *
         * The PID is hex, like every other VMS process-id text OVMX reads
         * (SHOW PROCESS/IDENTIFICATION, ATTACH/ID) -- $GETJPI's JPI$_PID is
         * printed and read as an 8-hex-digit value tree-wide.
         */
        char *end = NULL;
        unsigned long v;
        errno = 0;
        v = strtoul(idval, &end, 16);
        if (idval[0] == '\0' || !end || *end != '\0' || errno == ERANGE ||
            v > 0xFFFFFFFFUL) {
            dcl_error("DCL", 2, "IVIDENT",
                      "invalid value - %s - for /IDENTIFICATION qualifier",
                      idval);
            return SS$_BADPARAM;
        }
        pid_val = (uint32_t)v;
        pid_p = &pid_val;
    } else {
        /* Process-name parameter: upcased, like every other unquoted DCL
         * token (DCL upcases before the executive ever sees a name -- see
         * find_by_name()'s comment in src/kernel/vms_proctab.c). */
        size_t i;
        for (i = 0; i + 1 < sizeof(upper_name) && cmd->params[0][i]; i++)
            upper_name[i] = (char)toupper((unsigned char)cmd->params[0][i]);
        upper_name[i] = '\0';
        namdsc = dsc_from_str(upper_name);
        namdsc_p = &namdsc;
    }

    uint32_t status = sys$delprc(pid_p, namdsc_p);
    if (!(status & 1)) {
        /* The authentic VMS condition, exactly as sys$getmsg renders it
         * (run_print_condition, above) -- SS$_NONEXPR for no such process,
         * SS$_NOPRIV for missing GROUP/WORLD, never a fabricated STOP-
         * facility message standing in for the executive's own answer. */
        run_print_condition(status, 0);
        return status;
    }
    return SS$_NORMAL;
}

/*
 * LOGOUT - End session.
 */
int cmd_logout(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();
    /*
     * The name the console line and the OPC record text carry is whatever
     * ctx->username holds, including nothing: the
     * `ctx->username[0] ? ctx->username : "SYSTEM"` fallback that stood here
     * is deleted, not replaced (vms-f42d). Long form at lex_user() in
     * src/vmsdcl/dcl_lexical.c.
     *
     * SCOPE, because the wording here got it wrong once: this variable is the
     * console line and the OPC message TEXT. The user field of the OPCOM
     * HEADER is a different value from a different file -- sys$sndopr builds
     * it in src/libvms/syssvc/sys_operator.c -- and an earlier draft of this
     * comment claimed the deletion covered "the logout record or the OPCOM
     * entry" while that header was still being filled from getpwuid(getuid()).
     * It was falsified by running LOGOUT and reading the log.
     */
    const char *upper_user = ctx->username;

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
        desc.dsc$w_length  = (uint16_t)(OPC$K_MS_HDRLEN + n);
        sys$sndopr(&desc, 0);
    }

    ctx->exit_requested = 1;
    ctx->logout_requested = 1;
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
int cmd_continue(struct dcl_command *cmd)
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
