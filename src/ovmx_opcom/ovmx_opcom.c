/*
 * ovmx_opcom.c - OPCOM.EXE, the operator communication process (rd vms-798).
 *
 * On VMS, OPCOM is the detached system process that owns operator
 * communication: $SNDOPR hands it each request through its mailbox; it numbers
 * requests, files every message in SYS$MANAGER:OPERATOR.LOG and broadcasts it
 * to every operator terminal enabled for the message's classes. At startup it
 * enables the console, OPA0:, as an operator terminal and says so on it. A real
 * VAX's console shows exactly that (tests/lab/captures/
 * vms-c56-console-chatter-20261021/C.console.txt):
 *
 *   %%%%%%%%%%%  OPCOM  21-OCT-2026 15:07:10.69  %%%%%%%%%%%
 *   Operator _VAXC$OPA0: has been enabled, username SYSTEM
 *
 * WHAT THIS PROCESS DOES, in that order:
 *   1. names itself OPCOM (SHOW SYSTEM lists it) and creates a PERMANENT
 *      mailbox named OPCOM$MAILBOX in LNM$PERMANENT_MAILBOX (opcom_mbx.h) --
 *      the address sys$sndopr (src/libvms/syssvc/sys_operator.c) sends to;
 *   2. enables OPA0: as an operator terminal for every class and says so,
 *      on OPA0: and in the log ("has been enabled", "Operator status for");
 *   3. for ever: reads the mailbox; for each request takes the SENDER from the
 *      executive (the read's sender PID -> $GETJPI user name -- never from the
 *      message), builds the oracle record, files it and broadcasts it to OPA0:.
 *
 * WHAT IT DOES NOT DO YET (honest scope): REPLY/ENABLE of further terminals,
 * REPLY to a pending request, and a new OPERATOR.LOG version per boot ("Logfile
 * has been initialized ...") -- OPA0: is the one operator terminal, and the log
 * is appended to. Tracked under vms-798's follow-ups.
 *
 * It is created by PID 1 (src/ovmx_init/ovmx_init.c start_opcom), DETACHED,
 * with the system process's identity, silently (no %RUN-S-PROC_ID on OPA0:,
 * as VMS's STARTUP creates it), and it never runs as root.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "opcdef.h"
#include "brkdef.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_layout.h"
#include "ovmx_identity.h"
#include "rms_textfile.h"
#include "opcom_mbx.h"
#include "opcom_record.h"
#if defined(__linux__)
#endif

#define OPCOM_BRK_CLASS 7u          /* BRK$C_OPCOM: the message class OPCOM broadcasts under */

static pthread_mutex_t oplog_lock = PTHREAD_MUTEX_INITIALIZER;
static char node[OVMX_IDENTITY_MAXLEN];

static const char *const month_names[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

static void timestamp(char *buf, size_t sz)
{
    struct timespec ts;
    struct tm tmv, *tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    tm = localtime_r(&ts.tv_sec, &tmv);
    if (!tm) {
        snprintf(buf, sz, "00-JAN-1970 00:00:00.00");
        return;
    }
    snprintf(buf, sz, "%2d-%s-%04d %02d:%02d:%02d.%02d", tm->tm_mday,
             month_names[tm->tm_mon], tm->tm_year + 1900, tm->tm_hour,
             tm->tm_min, tm->tm_sec, (int)(ts.tv_nsec / 10000000));
}

/* One line into SYS$MANAGER:OPERATOR.LOG (RMS $PUT at EOF over the ACP). */
static void oplog_line(const char *line)
{
    (void)rms_textfile_append_line(VMS_OPERATOR_LOG, line);
}

/* Broadcast `text` to OPA0:, the enabled operator terminal, on lines of its
 * own (carriage control 0x20), as an OPCOM-class message. */
static void broadcast_opa0(const char *text)
{
    static const char opa0[] = "OPA0:";
    struct dsc$descriptor_s msg = { (uint16_t)strlen(text), DSC$K_DTYPE_T,
                                    DSC$K_CLASS_S, (char *)text };
    struct dsc$descriptor_s to = { sizeof opa0 - 1, DSC$K_DTYPE_T,
                                   DSC$K_CLASS_S, (char *)opa0 };

    (void)sys$brkthruw(0, &msg, &to, BRK$C_DEVICE, NULL, 0x20, 0,
                       OPCOM_BRK_CLASS, 0, NULL, 0);
}

/*
 * One OPCOM record: the banner, then `nlines` body lines. Filed in the log
 * (followed by the blank separator line OPCOM writes) and, when `broadcast`,
 * shown on OPA0: with a blank line after it, as the real console shows.
 */
static void opcom_record(const char *const *lines, size_t nlines, int broadcast)
{
    char ts[32], banner[80];
    char text[2048];
    size_t used, i;

    timestamp(ts, sizeof ts);
    opcom_banner(banner, sizeof banner, ts);

    pthread_mutex_lock(&oplog_lock);
    oplog_line(banner);
    for (i = 0; i < nlines; i++)
        oplog_line(lines[i]);
    oplog_line("");
    pthread_mutex_unlock(&oplog_lock);

    if (!broadcast)
        return;
    used = (size_t)snprintf(text, sizeof text, "%s", banner);
    for (i = 0; i < nlines && used < sizeof text; i++)
        used += (size_t)snprintf(text + used, sizeof text - used, "\r\n%s", lines[i]);
    if (used < sizeof text)
        (void)snprintf(text + used, sizeof text - used, "\r\n");
    broadcast_opa0(text);
}

/* Enable OPA0: as the operator terminal for every class, and say so (step 2). */
static void enable_console_operator(void)
{
    char enabled[128], status[128];
    const char *one[1];
    const char *st[1 + OPCOM_N_CLASS_LINES];
    struct vms_procinfo me;
    size_t i;

    memset(&me, 0, sizeof me);
    (void)vms_kif_getjpi_self(&me);

    opcom_enabled_line(enabled, sizeof enabled, node, "OPA0",
                       me.username[0] ? me.username : "SYSTEM");
    one[0] = enabled;
    opcom_record(one, 1, 1);

    opcom_status_line(status, sizeof status, node, "OPA0");
    st[0] = status;
    for (i = 0; i < OPCOM_N_CLASS_LINES; i++)
        st[1 + i] = opcom_all_classes[i];
    opcom_record(st, 1 + OPCOM_N_CLASS_LINES, 1);
}

/* Create the permanent mailbox and give it its name (step 1). */
static int open_mailbox(uint32_t *exec_chan)
{
    char devnam[32];
    uint32_t unit = 0, st;
    struct dsc$descriptor_s tab, nam;
    struct item_list_3 it[3];
    uint32_t eqattr = LNM$M_TERMINAL;
    static const char tabname[] = "LNM$PERMANENT_MAILBOX";
    static const char lognam[] = OPCOM_MBX_LOGNAM;

    st = vms_kif_mbx_create_prot(1, OPCOM_MBX_MAXMSG, 64u * OPCOM_MBX_MAXMSG, 0,
                                 exec_chan, &unit, devnam, sizeof devnam);
    if (!(st & 1)) {
        fprintf(stderr, "%%OPCOM-F-MBXCRE, cannot create the OPCOM mailbox "
                "(status %%X%08X)\n", (unsigned)st);
        return 0;
    }
    tab.dsc$w_length = sizeof tabname - 1; tab.dsc$b_dtype = DSC$K_DTYPE_T;
    tab.dsc$b_class = DSC$K_CLASS_S; tab.dsc$a_pointer = (char *)tabname;
    nam.dsc$w_length = sizeof lognam - 1; nam.dsc$b_dtype = DSC$K_DTYPE_T;
    nam.dsc$b_class = DSC$K_CLASS_S; nam.dsc$a_pointer = (char *)lognam;
    memset(it, 0, sizeof it);
    it[0].buflen = sizeof eqattr; it[0].item_code = LNM$_ATTRIBUTES; it[0].bufaddr = &eqattr;
    it[1].buflen = (uint16_t)strlen(devnam); it[1].item_code = LNM$_STRING; it[1].bufaddr = devnam;
    st = sys$crelnm(NULL, &tab, &nam, NULL, it);
    if (!(st & 1)) {
        fprintf(stderr, "%%OPCOM-F-MBXLOG, cannot name the OPCOM mailbox "
                "(status %%X%08X)\n", (unsigned)st);
        return 0;
    }
    return 1;
}

/* One request off the mailbox (step 4). */
static void serve_request(const unsigned char *buf, uint32_t len, uint32_t sender)
{
    static unsigned reqno;
    struct opcom_mbx_msg hdr;
    struct vms_procinfo who;
    char text[OPCOM_MBX_MAXBLK + 1], from[160];
    const char *lines[2];

    if (len < sizeof hdr)
        return;
    memcpy(&hdr, buf, sizeof hdr);
    if (hdr.magic != OPCOM_MBX_MAGIC || hdr.blklen > len - sizeof hdr)
        return;
    if (hdr.blklen < OPC$K_MS_HDRLEN)
        return;                         /* no request in it: nothing to log */
    (void)opcom_block_text((const char *)buf + sizeof hdr, hdr.blklen,
                           OPC$K_MS_HDRLEN, text, sizeof text);

    /* THE SENDER IS THE EXECUTIVE'S WORD, not the message's. */
    memset(&who, 0, sizeof who);
    if (sender == 0 || !(vms_kif_getjpi_pid(sender, &who) & 1))
        who.username[0] = '\0';
    who.username[sizeof who.username - 1] = '\0';

    if (hdr.flags & OPCOM_MBX_F_REPLY)
        ++reqno;
    opcom_from_line(from, sizeof from, (hdr.flags & OPCOM_MBX_F_REPLY) != 0,
                    reqno, who.username, node);
    lines[0] = from;
    lines[1] = text;
    opcom_record(lines, 2, 1);
}

int main(void)
{
    uint32_t chan = 0;
    unsigned char buf[OPCOM_MBX_MAXMSG];

    signal(SIGHUP, SIG_IGN);
    (void)vms_kif_setprn("OPCOM");
    ovmx_node_name(node, sizeof node);

    if (!open_mailbox(&chan))
        return 1;
    enable_console_operator();

    for (;;) {
        uint32_t actlen = 0, sender = 0;
        uint32_t st = vms_kif_mbx_read_ex(chan, buf, sizeof buf, &actlen, 0, &sender);

        if (!(st & 1)) {
            if (st == SS$_ENDOFFILE)
                continue;
            sleep(1);                   /* a failed read is retried, never spun */
            continue;
        }
        serve_request(buf, actlen < sizeof buf ? actlen : (uint32_t)sizeof buf, sender);
    }
}
