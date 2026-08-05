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
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>

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
#include "msgdef.h"
#include "ovmx_status.h"

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

    /* No fabricated owner -- same defect, same deletion, as SUBMIT above
     * (vms-f42d). */
    const char *user = ctx->username;

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
int cmd_show_queue(struct dcl_command *cmd)
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
int cmd_set_entry(struct dcl_command *cmd)
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

    char *endptr;
    long entry_val = strtol(cmd->params[1], &endptr, 10);
    if (endptr == cmd->params[1] || *endptr != '\0' || entry_val <= 0) {
        dcl_error("SET", 2, "BADENTRY", "invalid entry number - %s", cmd->params[1]);
        return SS$_BADPARAM;
    }
    uint32_t entry_id = (uint32_t)entry_val;

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
int cmd_show_entry(struct dcl_command *cmd)
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
        char *endptr;
        long entry_val = strtol(cmd->params[1], &endptr, 10);
        if (endptr == cmd->params[1] || *endptr != '\0' || entry_val <= 0) {
            dcl_error("SHOW", 2, "BADENTRY", "invalid entry number - %s",
                      cmd->params[1]);
            return SS$_BADPARAM;
        }
        uint32_t entry_id = (uint32_t)entry_val;

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
int cmd_set_queue(struct dcl_command *cmd)
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
    /* /UIC is checked FIRST, and before the image parameter is even
     * looked at. The DCL lexer splits on ',', so "/UIC=[300,1]" arrives
     * as the qualifier value "[300" plus a stray parameter "1]"; if the
     * image were resolved first, the user would be told "image not
     * found - 1]" and never hear about the UIC at all. Refusing on the
     * qualifier's PRESENCE also means OVMX never has to pretend it
     * understood a UIC it cannot honour. */
    if (run_has_qualifier(cmd, "UIC")) {
        run_creprc_failed(OVMX$_NOPRCUIC);
        return OVMX$_NOPRCUIC;
    }

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
     * running the image with the priority thrown away. */
    if (!run_has_qualifier(cmd, "DETACHED") &&
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
 * Qualifiers honoured here: /PROCESS_NAME=, /INPUT=, /OUTPUT=, /ERROR=.
 * /UIC is NOT one of them -- it is refused before this is reached (see
 * run_refuse_unhonourable), because the created process's UIC is the
 * executive's to derive and nothing DCL passes can change it.
 *
 * KNOWN GAP, TRACKED AS vms-69e -- READ THIS BEFORE ADDING A QUALIFIER.
 * Every OTHER RUN (Process) qualifier reaching this function is READ BY
 * NOBODY: baspri, prvadr and the whole quota set are passed to $CREPRC
 * as bare literals below, so RUN/DETACHED/PRIORITY=4 creates the process
 * and announces %RUN-S-PROC_ID while the priority is discarded in
 * silence. That is the same Rule 10 illegal third answer this file
 * refuses one layer up, and it is reachable from real VMS software in
 * this repo (tests/corpus/tier4-mx/kit/mx_start.com). It is asserted --
 * as it BEHAVES, not as it should behave -- in P10 of
 * tests/qemu/test_syssvc_startup_service.c, so that the day vms-69e
 * settles the question (refuse with a condition value, or propagate
 * quota and privilege to the executive) the change cannot land without
 * that assertion being rewritten.
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
                                 NULL, NULL,
                                 prc_d.dsc$a_pointer ? &prc_d : NULL,
                                 0, 0 /* uic: see run_refuse_unhonourable */,
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

    /* /DETACHED creates a detached process -- a service -- instead of
     * running the image as a subprocess of this DCL. */
    if (run_has_qualifier(cmd, "DETACHED"))
        return run_detached(ctx, cmd, linux_path);

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
 * STOP - Forceful exit.
 */
int cmd_stop(struct dcl_command *cmd)
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
