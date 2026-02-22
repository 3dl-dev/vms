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

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <pwd.h>
#include "starlet.h"
#include "vms/pcb.h"

/* Operator log file path */
#include "ovmx_layout.h"
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
    int hundredths = (int)(ts.tv_nsec / 10000000);

    snprintf(buf, bufsz, "%02d-%s-%04d %02d:%02d:%02d.%02d",
             tm->tm_mday,
             month_names[tm->tm_mon],
             tm->tm_year + 1900,
             tm->tm_hour, tm->tm_min, tm->tm_sec,
             hundredths);
}

/*
 * Get the current VMS username from PCB or passwd database.
 */
static const char *get_current_username(void)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (pcb && pcb->username[0] != '\0')
        return pcb->username;

    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_name : "UNKNOWN";
}

/*
 * Open the operator log file, trying primary then fallback path.
 * Returns FILE* opened in append mode, or NULL on failure.
 */
static FILE *open_operator_log(void)
{
    FILE *f = fopen(OPERATOR_LOG_PATH, "a");
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

    /* Static request counter */
    static unsigned int req_count = 0;
    req_count++;

    /* Open log */
    FILE *log = open_operator_log();
    if (!log)
        return SS$_FILACCERR;

    /* Write OPCOM header */
    fprintf(log, "%%%%OPCOM, %s, request %u from user %s on node OVMX\n",
            timestamp, req_count, username);

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
