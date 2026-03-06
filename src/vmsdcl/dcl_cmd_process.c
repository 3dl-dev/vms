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
 * RUN - Execute a program.
 */
int cmd_run(struct dcl_command *cmd)
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
