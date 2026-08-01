/*
 * sys_operator.c - Operator Communication System Services
 *
 * Implements:
 *   sys$sndopr   — Send message to operator log
 *   sys$brkthruw — Broadcast message to terminal(s)
 *
 * sys$sndopr writes formatted messages to the operator log file
 * at /vms/sys$manager/OPERATOR.LOG in standard OpenVMS OPCOM format.
 *
 * sys$brkthruw writes a message to one or more terminal devices,
 * identified by a VMS device name (TT:, _FTAn:, _TTAn:).
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-USERSPACE: sys$sndopr (vms-5b4) -- appends to OPERATOR.LOG through
 *     vmsfs path translation, tagged with the username the EXECUTIVE holds
 *     for the caller (vms-cb5; it used to read the caller's own PCB and then
 *     the host passwd database). There is no OPCOM process to request, so no
 *     operator is notified and no reply can ever come back.
 * OVMX-USERSPACE: sys$brkthruw (vms-5b4) -- open()s the resolved terminal
 *     device and write()s to it directly, falling back to the caller's own
 *     stdout when that open fails. No executive mediates the broadcast, so it
 *     reaches a terminal only if this process can already open that device
 *     itself, and sndtyp (the VMS target class) is discarded.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
/*
 * <pwd.h> IS DELETED, DELIBERATELY (vms-cb5) -- see get_current_username
 * below. The operator log's user name comes from the executive; the host
 * passwd database is not an authority on who a VMS process is.
 */
#include "starlet.h"
#include "vms_kif.h"

/* Operator log file path */
#include "ovmx_layout.h"
#include "vmsfs/filespec.h"
#define OPERATOR_LOG_PATH VMS_OPERATOR_LOG

/* Fallback operator log (writable in container environments) */
#define OPERATOR_LOG_FALLBACK "/tmp/OPERATOR.LOG"

/* Month names for VMS-style timestamp */
static const char * const month_names[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

/*
 * Format the current time in VMS OPCOM style:
 *   DD-MON-YYYY HH:MM:SS.CC
 *
 * Writes into buf (must be at least 24 bytes).
 */
static void format_vms_timestamp(char *buf, size_t bufsz)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm *tm = localtime(&ts.tv_sec);
    if (!tm) {
        snprintf(buf, bufsz, "00-JAN-1970 00:00:00.00");
        return;
    }
    int hundredths = (int)(ts.tv_nsec / 10000000);

    snprintf(buf, bufsz, "%02d-%s-%04d %02d:%02d:%02d.%02d",
             tm->tm_mday,
             month_names[tm->tm_mon],
             tm->tm_year + 1900,
             tm->tm_hour, tm->tm_min, tm->tm_sec,
             hundredths);
}

/*
 * The user name the executive holds for THIS process, or nothing.
 *
 * =====================================================================
 * THE getpwuid() FALLBACK AND THE "UNKNOWN" LITERAL ARE DELETED
 * (vms-cb5, vms-f39; CLAUDE.md Rules 10 and 11)
 * =====================================================================
 * This used to read the userspace PCB and, when that held no name, fall
 * back to getpwuid(getuid()) and then to the literal "UNKNOWN". So the
 * OPERATOR.LOG record -- a VMS-facing artifact -- named the HOST's Linux
 * account. MEASURED on the host build before this change, by running
 * `printf 'LOGOUT\n' | DCL.EXE` and reading the log it wrote:
 *
 *   %%OPCOM, 01-AUG-2026 19:09:28.24, request 1 from user baron on node OVMX
 *
 * "baron" is the Linux login of the machine, not any VMS account. The
 * same record is written by REPLY/ENABLE.
 *
 * WHY THERE IS NO REPLACEMENT VALUE. OpenVMS has no state where a
 * process has no user name -- the name lives in the executive's process
 * table and every process is entered in it. So this is Rule 10's second
 * candidate: the condition is not handled, and nothing is invented to
 * stand in for it. What goes in the record is what the executive holds,
 * INCLUDING NOTHING, which is the same disposition already applied to
 * SHOW PROCESS, F$USER, PRINT, SUBMIT, ACCOUNTING, REPLY and LOGOUT.
 * An OPCOM record naming no user is visibly wrong and points at the
 * real gap (an unnamed row, vms-afd); a record naming "baron" or
 * "UNKNOWN" looks right and hides it.
 *
 * WHY IT READS THE EXECUTIVE AND NOT THE PCB. The userspace PCB is
 * per-process memory (Rule 11), a copy of what the executive decided.
 * Reading the executive directly means this cannot report a name the
 * executive does not have -- and on a process the executive has not
 * named there is no second source to fall through to.
 */
static const char *get_current_username(void)
{
    static __thread char name[VMS_USERNAME_SIZE];
    struct vms_procinfo self;

    name[0] = '\0';
    memset(&self, 0, sizeof(self));
    if (vms_kif_getjpi_self(&self) & 1) {
        strncpy(name, self.username, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
    return name;
}

/*
 * Open the operator log file, trying primary then fallback path.
 * Returns FILE* opened in append mode, or NULL on failure.
 */
static FILE *open_operator_log(void)
{
    char oplog_linux[1024];
    vmsfs_to_linux_path(OPERATOR_LOG_PATH, oplog_linux, sizeof(oplog_linux));
    FILE *f = fopen(oplog_linux, "a");
    if (!f)
        f = fopen(OPERATOR_LOG_FALLBACK, "a");
    return f;
}

/*
 * sys$sndopr - Send message to operator.
 *
 * Writes a formatted OPCOM-style entry to OPERATOR.LOG.
 * The log format matches OpenVMS OPCOM output:
 *
 *   %%OPCOM, DD-MON-YYYY HH:MM:SS.CC, request NNN from user USERNAME on node OVMX
 *   <message text>
 *
 * @param msgbuf  Descriptor of message text to log
 * @param chan     Channel number (ignored — OVMX logs all to OPERATOR.LOG)
 */
uint32_t sys$sndopr(const struct dsc$descriptor_s *msgbuf, uint16_t chan)
{
    (void)chan;

    if (!msgbuf || !msgbuf->dsc$a_pointer)
        return SS$_BADPARAM;

    /* Extract message text */
    char msgtext[512];
    dsc$strncpy(msgtext, msgbuf, sizeof(msgtext));

    /* Format timestamp */
    char timestamp[32];
    format_vms_timestamp(timestamp, sizeof(timestamp));

    /* Get username */
    const char *username = get_current_username();

    /* Thread-safe static request counter */
    static volatile unsigned int req_count = 0;
    unsigned int this_req = __atomic_add_fetch(&req_count, 1, __ATOMIC_SEQ_CST);

    /* Open log */
    FILE *log = open_operator_log();
    if (!log)
        return SS$_FILACCERR;

    /* Write OPCOM header */
    fprintf(log, "%%%%OPCOM, %s, request %u from user %s on node OVMX\n",
            timestamp, this_req, username);

    /* Write message body (strip trailing whitespace) */
    size_t msglen = strlen(msgtext);
    while (msglen > 0 &&
           (msgtext[msglen - 1] == '\n' || msgtext[msglen - 1] == '\r' ||
            msgtext[msglen - 1] == ' '))
        msglen--;
    msgtext[msglen] = '\0';

    fprintf(log, "%s\n\n", msgtext);
    fflush(log);
    fclose(log);

    return SS$_NORMAL;
}

/*
 * Resolve a VMS terminal device name to a Linux /dev path.
 *
 * Mappings:
 *   TT:, _TTA0:    -> /dev/tty  (current terminal)
 *   _FTA<n>:       -> /dev/pts/<n>
 *   _TTA<n>:       -> /dev/tty<n>
 *
 * Returns 1 if resolved, 0 if unmappable.
 */
static int resolve_terminal(const char *devnam,
                             char *linux_dev, size_t devsz)
{
    if (!devnam || !devnam[0]) {
        strncpy(linux_dev, "/dev/tty", devsz);
        linux_dev[devsz - 1] = '\0';
        return 1;
    }

    /* Uppercase copy, strip colon */
    char upper[64];
    strncpy(upper, devnam, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = '\0';
    for (size_t i = 0; upper[i]; i++)
        if (upper[i] >= 'a' && upper[i] <= 'z')
            upper[i] = (char)(upper[i] - 'a' + 'A');
    size_t ulen = strlen(upper);
    if (ulen > 0 && upper[ulen - 1] == ':')
        upper[ulen - 1] = '\0';

    /* TT or _TTA0 = current terminal */
    if (strcmp(upper, "TT") == 0 ||
        strcmp(upper, "_TTA0") == 0) {
        strncpy(linux_dev, "/dev/tty", devsz);
        linux_dev[devsz - 1] = '\0';
        return 1;
    }

    /* _FTA<n> -> /dev/pts/<n> */
    if (strncmp(upper, "_FTA", 4) == 0) {
        int n = atoi(upper + 4);
        snprintf(linux_dev, devsz, "/dev/pts/%d", n);
        return 1;
    }

    /* _TTA<n> -> /dev/tty<n> */
    if (strncmp(upper, "_TTA", 4) == 0) {
        int n = atoi(upper + 4);
        snprintf(linux_dev, devsz, "/dev/tty%d", n);
        return 1;
    }

    return 0;
}

/*
 * sys$brkthruw - Broadcast message to terminal(s).
 *
 * Writes msgbuf to the terminal identified by sendto.
 * If sendto is NULL or empty, broadcasts to the current terminal.
 *
 * The message is written with a leading bell character (^G) and a
 * VMS-style header line to match OpenVMS OPCOM broadcast format.
 *
 * @param efn      Event flag (ignored — synchronous)
 * @param msgbuf   Descriptor of message to broadcast
 * @param sendto   Descriptor of target terminal device name (NULL = TT:)
 * @param sndtyp   Send type flags (ignored)
 * @param iosb     Optional I/O status block
 * @param astadr   AST completion routine (ignored)
 * @param astprm   AST parameter (ignored)
 */
uint32_t sys$brkthruw(uint32_t efn,
                       struct dsc$descriptor_s *msgbuf,
                       struct dsc$descriptor_s *sendto,
                       uint32_t sndtyp,
                       struct _iosb *iosb,
                       void (*astadr)(uint32_t),
                       uint32_t astprm)
{
    (void)efn; (void)sndtyp; (void)astadr; (void)astprm;

    if (!msgbuf || !msgbuf->dsc$a_pointer)
        return SS$_BADPARAM;

    /* Resolve target terminal */
    char target_devnam[64] = "";
    if (sendto && sendto->dsc$a_pointer)
        dsc$strncpy(target_devnam, sendto, sizeof(target_devnam));

    char linux_dev[64];
    int resolved = resolve_terminal(target_devnam, linux_dev, sizeof(linux_dev));

    uint32_t status = SS$_NORMAL;

    if (resolved) {
        /* Extract message */
        char msgtext[512];
        dsc$strncpy(msgtext, msgbuf, sizeof(msgtext));

        /* Format timestamp for the broadcast header */
        char timestamp[32];
        format_vms_timestamp(timestamp, sizeof(timestamp));

        /* Build broadcast string */
        char broadcast[640];
        int blen = snprintf(broadcast, sizeof(broadcast),
                            "\r\n\007\007\007"
                            "%%OPCOM-%s, %s\r\n%s\r\n",
                            target_devnam[0] ? target_devnam : "TT",
                            timestamp,
                            msgtext);

        /* Write to terminal device */
        int fd = open(linux_dev, O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) {
            /* write() may return short — best effort */
            (void)write(fd, broadcast, (size_t)(blen > 0 ? blen : 0));
            close(fd);
        } else {
            /* Fall back: write to stdout if we can't open the terminal */
            (void)write(STDOUT_FILENO, broadcast,
                        (size_t)(blen > 0 ? blen : 0));
        }

        /* Also log to OPERATOR.LOG */
        FILE *log = open_operator_log();
        if (log) {
            fprintf(log, "%%%%OPCOM BROADCAST to %s at %s:\n%s\n\n",
                    target_devnam[0] ? target_devnam : "TT",
                    timestamp, msgtext);
            fclose(log);
        }
    } else {
        status = SS$_NOSUCHDEV;
    }

    if (iosb) {
        iosb->iosb$w_status = (uint16_t)status;
        iosb->iosb$w_bcnt   = 0;
    }
    return status;
}
