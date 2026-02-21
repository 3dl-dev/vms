/*
 * sys_opcom.c - OPCOM (Operator Communication Manager) System Service
 *
 * Implements sys$sndopr — send a message to the operator terminal.
 *
 * On real VMS, OPCOM is a separate process (OPCOM.EXE) that routes
 * operator messages to enabled operator terminals and the operator log.
 * In OVMX we implement a lightweight version that:
 *   - Writes messages to the operator log (/vms/sys$manager/OPERATOR.LOG)
 *   - Returns SS$_NORMAL on success
 *
 * The OPC$_RQ_RQST message type (user REQUEST) is distinguished from
 * operator reply and broadcast types so callers can tell them apart.
 *
 * Reference: OpenVMS System Services Reference Manual — SYS$SNDOPR
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "starlet.h"
#include "ssdef.h"
#include "opcdef.h"

/* Operator log path — matches /vms/sys$manager tree */
#define OPERATOR_LOG  "/vms/sys$manager/OPERATOR.LOG"

/* ------------------------------------------------------------------ */
/* Format a VMS-style timestamp: DD-MMM-YYYY HH:MM:SS                 */
/* ------------------------------------------------------------------ */
static void fmt_vms_time(char *buf, size_t bufsiz)
{
    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(buf, bufsiz, "%02d-%s-%04d %02d:%02d:%02d",
             tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ------------------------------------------------------------------ */
/* Write a line to the operator log.                                   */
/* Creates the log file if it does not exist.                         */
/* ------------------------------------------------------------------ */
static void opcom_log(const char *msg)
{
    /* Ensure parent directory exists */
    struct stat st;
    if (stat("/vms/sys$manager", &st) != 0) {
        mkdir("/vms", 0755);
        mkdir("/vms/sys$manager", 0755);
    }

    FILE *fp = fopen(OPERATOR_LOG, "a");
    if (!fp)
        return;

    char timebuf[32];
    fmt_vms_time(timebuf, sizeof(timebuf));

    fprintf(fp, "%%%s-%s\n", timebuf, msg);
    fclose(fp);
}

/* ------------------------------------------------------------------ */
/* sys$sndopr — Send message to operator                              */
/*                                                                      */
/* Parameters (per OpenVMS reference):                                 */
/*   msgbuf  — descriptor pointing to an OPCDEF buffer                 */
/*   chan    — optional mailbox channel for replies (0 = no reply)     */
/*                                                                      */
/* The OPCDEF buffer layout (relevant fields):                         */
/*   opc$b_ms_type    — message type (OPC$_RQ_RQST = user request)    */
/*   opc$b_ms_target  — target operator class bitmask                  */
/*   opc$l_ms_rqstid  — request ID (filled in by service)              */
/*   opc$l_ms_text    — message text starts here                       */
/* ------------------------------------------------------------------ */
uint32_t sys$sndopr(
    const struct dsc$descriptor_s *msgbuf,
    uint16_t chan)
{
    (void)chan;   /* Reply-mailbox support not implemented in OVMX */

    if (!msgbuf || !msgbuf->dsc$a_pointer || msgbuf->dsc$w_length < 8)
        return SS$_BADPARAM;

    const struct opcdef *opc = (const struct opcdef *)msgbuf->dsc$a_pointer;

    /* Extract message text — starts at opc$l_ms_text offset (byte 8) */
    int text_len = (int)msgbuf->dsc$w_length - 8;
    if (text_len < 0) text_len = 0;
    if (text_len > 255) text_len = 255;

    char text[256];
    memcpy(text, (const char *)&opc->opc$l_ms_text, (size_t)text_len);
    text[text_len] = '\0';

    /* Build a log line based on message type */
    char logline[512];
    switch (opc->opc$b_ms_type) {
    case OPC$_RQ_RQST:
        snprintf(logline, sizeof(logline),
                 "OPCOM-I-RQST, user request: %s", text);
        break;
    case OPC$_RQ_REPLY:
        snprintf(logline, sizeof(logline),
                 "OPCOM-I-REPLY, operator reply: %s", text);
        break;
    case OPC$_RQ_CANCEL:
        snprintf(logline, sizeof(logline),
                 "OPCOM-I-CANCEL, request cancelled");
        break;
    case OPC$_RQ_ENABLE:
        snprintf(logline, sizeof(logline),
                 "OPCOM-I-ENABLE, operator terminal enabled");
        break;
    case OPC$_RQ_DISABLE:
        snprintf(logline, sizeof(logline),
                 "OPCOM-I-DISABLE, operator terminal disabled");
        break;
    default:
        snprintf(logline, sizeof(logline),
                 "OPCOM-I-MSG, message (type=%d): %s",
                 (int)opc->opc$b_ms_type, text);
        break;
    }

    opcom_log(logline);
    return SS$_NORMAL;
}
