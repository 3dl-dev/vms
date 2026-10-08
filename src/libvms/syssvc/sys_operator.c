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
 * OVMX-PARTIAL: sys$sndopr (vms-042) -- exec: the user name in the OPCOM
 *     header is the one the EXECUTIVE holds for the caller, read back through
 *     vms_kif_getjpi_self(). It used to come from the caller's own PCB and
 *     then from the host passwd database, which is why this line is an upgrade
 *     rather than a correction -- see get_current_username below
 *     (vms-cb5 / vms-f39).
 * OVMX-LOCAL: sys$sndopr -- everything else. The record is appended to
 *     OPERATOR.LOG by this process through vmsfs path translation, the request
 *     number is a counter private to this image, and there is no OPCOM process
 *     to request, so no operator is notified and no reply can ever come back.
 * OVMX-PARTIAL: sys$brkthruw (vms-96e2) -- exec: the target terminal device
 *     spec is resolved through the executive-resident LNM$SYSTEM table (vmsfs
 *     path translation -> lnm_translate -> vms_kif_lnm_translate) for system
 *     logical names before the device is opened.
 * OVMX-LOCAL: sys$brkthruw -- open()s the resolved terminal device and write()s
 *     to it directly, falling back to the caller's own stdout when that open
 *     fails. No executive mediates the broadcast, so it reaches a terminal this
 *     process can already open itself and no other, and sndtyp (the VMS target
 *     class) is discarded.
 *
 * THE TWO SERVICES CITE DIFFERENT ITEMS ON PURPOSE (vms-fab). They shared vms-5b4,
 * which is the closed item that BUILT this register and owned neither of them.
 * $BRKTHRU's remainder is broadcast delivery, which is vms-905.
 *
 * $SNDOPR'S CITATION MOVED FROM vms-2d37 TO vms-042 WHEN vms-2d37 WAS FIXED
 * AND CLOSED, and the move is the point rather than the bookkeeping. vms-2d37
 * was the defect "the message body never reaches OPERATOR.LOG, because the
 * callers point the descriptor at an OPC message block and this file copies it
 * as a C string". That is now fixed -- the reader takes the text at
 * OPC$K_MS_HDRLEN and the record carries the message -- so the item closed, and
 * a closed id tracks nothing and cannot be what a live declaration is declared
 * against (rd_cite_check treats it as red by design).
 *
 * What REMAINS partial is what this line has always claimed: the user name is
 * the executive's, and nothing else about the record is. vms-042 -- Phase 3,
 * the real VMS system facilities -- is where a genuine OPCOM process lives, so
 * it is the owner of the remainder now that the body defect is gone. This is
 * the same disposition, for the same reason, that sys_lock.c took when vms-82a
 * closed under it (rd vms-344).
 *
 * THAT THIS KEEPS HAPPENING IS ITSELF TRACKED. An OVMX-PARTIAL/-EXECUTIVE line
 * asserts a state and must cite an item, yet every item eventually closes, so
 * every successful fix arms this same red. The structural question is rd
 * vms-344 and the CI-visibility half is rd vms-72d; neither is answered by
 * repointing, which is only the honest local move.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
/* <pwd.h> is no longer used by this file -- get_current_username() below
 * reads the executive, not the passwd database. It stays because
 * tests/qemu/facility_defects.sh's opcom-header-host-login-name control
 * restores the getpwuid() call verbatim to prove the assertions can see it,
 * and a mutation that will not compile is a broken fixture, not a gate. */
#include <pwd.h>
#include "starlet.h"
#include "vms/pcb.h"
#include "vms_kif.h"
extern int vms$$chan_to_fd(uint16_t chan);   /* sys_assign.c */
extern int vms$$chan_is_mailbox(uint16_t chan);

/* Operator log file path */
#include "ovmx_layout.h"
/*
 * OPERATOR.LOG is written the VMS way now (vms-aac, epic vms-208 atomic flip):
 * one RMS $PUT-at-EOF record per line over the Files-11 ODS-2 ACP when the
 * on-volume log is writable (rms_textfile.c carries the RMS-over-ACP writer).
 * When it cannot be written -- no /dev/vms (host ctest / plain container /
 * netbsd-vax cross), OR /dev/vms present but SYS$MANAGER's system disk not
 * mounted/provisioned (early boot; an isolated executive, as in the
 * test_syssvc_* harness) -- the record is written the legacy host/console way
 * instead (open_operator_log_host() below), where real VMS OPCOM writes before
 * OPERATOR.LOG opens: OPCOM never silently drops an operator audit record. On a
 * booted system with a provisioned system disk the ACP $PUT succeeds and the
 * host writer is never reached. See operator_log_put_record() for the full
 * on-volume-first / console-fallback contract (Rule 9 / INV-6).
 */
#include "rms_textfile.h"
#include "vmsfs/filespec.h"
#define OPERATOR_LOG_PATH VMS_OPERATOR_LOG

/* Fallback operator log, writable when SYS$MANAGER: does not resolve to a
 * populated tree (dev seat / plain container). */
#define OPERATOR_LOG_FALLBACK "/tmp/OPERATOR.LOG"

/*
 * ON-VOLUME FIRST, CONSOLE/HOST FALLBACK (vms-aac, epic vms-208 atomic flip;
 * Rule 9 / INV-6). See operator_log_put_record() below for the writer that
 * carries this contract: OPCOM $PUTs the record over the Files-11 ODS-2 ACP
 * (rms_textfile_append_line -> RMS $PUT-at-EOF on the on-volume log) when the
 * on-volume OPERATOR.LOG is writable, and writes the legacy console/host log
 * when it is not -- exactly where real VMS OPCOM writes before OPERATOR.LOG
 * opens. rms_executive_absent() (below) only gates whether the ACP is reachable
 * AT ALL; whether SYS$MANAGER's volume is mounted/provisioned is settled by the
 * $PUT itself, not by a mount probe (the executive can hold a mounted ODS-2
 * volume that carries no writable SYS$MANAGER -- the test_syssvc_* harness's
 * fixture is exactly that, which is why a DKA0:-mount probe cannot answer this).
 *
 * rms_executive_absent() lives in LIBVMSRMS, which links LIBVMS (this file), so
 * it is referenced WEAKLY -- the same library-layering seam rms_textfile.c uses
 * for the RMS services. An image that links vmsrms (DCL, LOGINOUT, VMSSSHD,
 * PROVISION) binds the real probe; an image that does not has no ACP at all, so
 * "absent" is the correct reading and the legacy writer is used.
 */
#pragma weak rms_executive_absent
extern int rms_executive_absent(void);

/*
 * Is the Files-11 ODS-2 ACP reachable at all (i.e. is /dev/vms present in an
 * image that links LIBVMSRMS)? Returns 1 when it is not -- a host ctest, a
 * plain-container gate, the netbsd-vax cross, or an image that links no RMS
 * engine. This gates whether OPCOM even ATTEMPTS the on-volume log below; it is
 * NOT the whole answer, because /dev/vms being present does not mean
 * SYS$MANAGER:OPERATOR.LOG can be written -- the log's system disk may not be
 * mounted or provisioned (early boot, or an isolated executive as in the
 * test_syssvc_* harness). The write itself is what settles that, and OPCOM
 * defers to the host/console writer when it fails (see operator_log_put_record).
 */
static int operator_log_executive_absent(void)
{
    if (rms_executive_absent)
        return rms_executive_absent();
    return 1;   /* no RMS engine in this image -> no ACP reachable */
}

/*
 * operator_log_put_record - append one OPCOM record (a banner line, a body-2
 * line, the message, and a blank separator) to SYS$MANAGER:OPERATOR.LOG.
 *
 * ON-VOLUME FIRST, CONSOLE/HOST FALLBACK (vms-aac / vms-censusident; Rule 9 /
 * INV-6). When /dev/vms is present OPCOM attempts the genuine flip path -- RMS
 * $PUT-at-EOF over the Files-11 ODS-2 ACP (rms_textfile_append_line). On a
 * booted system with a provisioned, mounted SYS$MANAGER that $PUT succeeds and
 * is the ONLY writer touched. When it cannot be written -- no /dev/vms, OR
 * /dev/vms present but SYS$MANAGER's volume is not mounted/provisioned (early
 * boot; the isolated test_syssvc_* executive, whose one mounted ODS-2 fixture
 * carries no writable SYS$MANAGER) -- the record is written the legacy
 * host/console way instead. That is exactly what real VMS OPCOM does before
 * OPERATOR.LOG is opened at startup, and it is the SAME shared host log the
 * executive-absent path already uses (open_operator_log_host, the writer
 * opcom_record_body_gate exercises): OPCOM never SILENTLY DROPS an operator
 * audit record. It is NOT the INV-6 per-process masquerade -- one shared file,
 * genuine stream-LF records, not a fabricated per-process success -- and once
 * the on-volume log is writable the host writer is never reached.
 *
 * Returns SS$_NORMAL if the record was written to either destination,
 * SS$_FILACCERR only when NEITHER could be opened.
 */
static FILE *open_operator_log_host(void);   /* defined below */

static uint32_t operator_log_put_record(const char *banner,
                                        const char *line2,
                                        const char *msgtext)
{
    if (!operator_log_executive_absent()) {
        /* /dev/vms present: try the genuine on-volume ACP log first. */
        if (rms_textfile_append_line(OPERATOR_LOG_PATH, banner) == 0) {
            rms_textfile_append_line(OPERATOR_LOG_PATH, line2);
            rms_textfile_append_line(OPERATOR_LOG_PATH, msgtext);
            rms_textfile_append_line(OPERATOR_LOG_PATH, "");
            return SS$_NORMAL;
        }
        /* fall through: the ACP could not write the on-volume OPERATOR.LOG. */
    }

    FILE *log = open_operator_log_host();
    if (!log)
        return SS$_FILACCERR;
    fprintf(log, "%s\n", banner);
    fprintf(log, "%s\n", line2);
    fprintf(log, "%s\n\n", msgtext);
    fclose(log);
    return SS$_NORMAL;
}

/*
 * Open OPERATOR.LOG the legacy host way (executive absent): translate
 * SYS$MANAGER:OPERATOR.LOG through vmsfs and append, falling back to
 * /tmp/OPERATOR.LOG when that path is not writable. Returns an append-mode
 * FILE* or NULL.
 */
static FILE *open_operator_log_host(void)
{
    char oplog_linux[1024];
    vmsfs_to_linux_path(OPERATOR_LOG_PATH, oplog_linux, sizeof(oplog_linux));
    FILE *f = fopen(oplog_linux, "a");
    if (!f)
        f = fopen(OPERATOR_LOG_FALLBACK, "a");
    return f;
}

/*
 * ovmx_node_name() -- the real SCSNODE-configured node identity (falling
 * back to OVMX's own default when unconfigured), the SAME accessor
 * SYI$_SCSNODE, F$GETSYI("SCSNODE") and SHOW SYSTEM's banner already use.
 * The OPCOM header used to hardcode the literal string "OVMX" here instead
 * (rd vms-32a) -- see format_opcom_header() below.
 */
#include "ovmx_identity.h"

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
 * THE OPCOM BANNER, ORACLE-EXACT (rd vms-32a; docs/design-opcom-executive-
 * logging.md sec2/sec6). Observed verbatim on lab-Alpha (OpenVMS Alpha
 * V8.4):
 *
 *     %%%%%%%%%%%  OPCOM  12-AUG-2026 13:21:35.77  %%%%%%%%%%%
 *
 * -- eleven '%', two spaces, "OPCOM", two spaces, the timestamp, two
 * spaces, eleven '%'. This REPLACES the one-line "%%OPCOM, <ts>, request N
 * from user X on node OVMX" shape this function used to write, which had
 * neither the boxed banner nor a real node name (the "OVMX" was a hardcoded
 * literal, not SCSNODE).
 *
 * OPCOM_BANNER_HASHES is a plain string constant -- not a printf format
 * string being interpreted here, only ever passed through a "%s" slot --
 * so its eleven '%' characters cannot silently become a different count
 * under a future edit that adds or removes a level of %%-escaping.
 */
#define OPCOM_BANNER_HASHES "%%%%%%%%%%%"

static void format_opcom_banner(char *buf, size_t bufsz, const char *timestamp)
{
    snprintf(buf, bufsz, "%s  OPCOM  %s  %s",
             OPCOM_BANNER_HASHES, timestamp, OPCOM_BANNER_HASHES);
}

/*
 * THE USER NAME IN THE OPCOM HEADER, READ FROM THE EXECUTIVE'S ROW
 * (vms-cb5 / vms-f39; CLAUDE.md Rules 9, 10 and 11).
 *
 * TWO SOURCES WERE DELETED HERE, not demoted to fallbacks -- the same pair
 * $GETJPI deleted at src/libvms/syssvc/sys_process.c (JPI$_USERNAME):
 *
 *  - vms_pcb_get()->username, a value THIS process wrote about itself. The
 *    PCB is process-private memory and its setter is a plain strncpy of the
 *    caller's own argument (src/vmsprocess/vms_pcb.c), with no executive
 *    anywhere in the path -- so the operator log recorded whatever the
 *    requesting process had told itself it was called (Rule 11).
 *  - getpwuid(getuid())->pw_name, the HOST Linux account name, with the
 *    literal "UNKNOWN" behind it. MEASURED on this repo's build host, before
 *    this change:
 *
 *        $ printf 'LOGOUT\n' | ./build/bin/DCL.EXE
 *        (in the operator log)
 *        %%OPCOM, 01-AUG-2026 18:50:05.30, request 1 from user baron on node OVMX
 *
 *    -- the developer's Linux login name, written into a VMS operator record
 *    for a process the executive had never named. That is vms-f39's defect in
 *    a second file, and it outlived the round that deleted the DCL half
 *    because that round fixed the call sites it was handed and called the
 *    class settled.
 *
 * What is left is the row the executive holds, read back through
 * vms_kif_getjpi_self(). A row with no name yields the empty string, and the
 * header is then written with that field empty: no name is invented to fill
 * it, and no name is taken from anywhere this process can set. That is the
 * same answer $GETJPI, F$USER() and SHOW PROCESS already give for the same
 * row, so it is not a third answer (Rule 10) -- VMS has no process without a
 * user name, so there is no VMS rendering of this state to match and nothing
 * legal to invent for it.
 *
 * WITH NO /dev/vms THERE IS NO ROW TO READ, and then this reports no name
 * rather than substituting one (Rule 9). It does not fail the request:
 * the record itself is not the executive's to write on OVMX (see the
 * OVMX-PARTIAL / OVMX-LOCAL declaration at the top of this file), the message
 * is still the caller's to log, and an empty user field is the honest
 * rendering of "nothing holds a name for the requester".
 */
static void get_current_username(char *buf, size_t bufsz)
{
    struct vms_procinfo info;

    if (!buf || bufsz == 0)
        return;
    buf[0] = '\0';

    memset(&info, 0, sizeof(info));
    if (!(vms_kif_getjpi_self(&info) & 1))
        return;

    strncpy(buf, info.username, bufsz - 1);
    buf[bufsz - 1] = '\0';
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

    if (!msgbuf || !msgbuf->dsc$a_pointer)
        return SS$_BADPARAM;

    /*
     * THE DESCRIPTOR POINTS AT AN OPC MESSAGE BLOCK, NOT AT A STRING
     * (rd vms-2d37). This is what $SNDOPR takes on VMS, and it is what every
     * caller in this tree builds: an opcdef header followed by the text.
     *
     * WHAT WAS WRONG, and it emptied the audit trail rather than corrupting a
     * corner of it. This function used to do dsc$strncpy() straight onto the
     * descriptor -- treating the first byte of the BLOCK as the first byte of a
     * C string. So it copied opc$b_ms_type and opc$b_ms_target and then stopped
     * at the first NUL inside opc$w_ms_rqstlen, and every OPCOM record OVMX
     * wrote had a body of two control bytes:
     *
     *     %%OPCOM, 02-AUG-2026 00:13:58.89, request 1 from user  on node OVMX
     *     ^A^A
     *
     * The record said that a request happened, by whom and when, and never what
     * it was.
     *
     * THE TEXT IS TAKEN BY LENGTH, NOT BY NUL. The block carries its extent in
     * the descriptor, and nothing guarantees a terminator inside it -- reading
     * to a NUL would be the same category of mistake as the one being fixed.
     *
     * OPC$K_MS_HDRLEN rather than 8: the offset is derived from the shared
     * declaration, so the reader here and the callers that size the block
     * cannot drift apart. Skipping a hardcoded 8 would have made this line
     * print correctly while leaving the two halves free to disagree again,
     * which is why rd vms-2d37 rules it out explicitly.
     */
    /* a channel to reply on must be one this process holds (OpenVMS: an
     * unassigned channel number is refused -- observed OPR.BADCHAN) */
    if (chan != 0 && vms$$chan_to_fd(chan) < 0 && !vms$$chan_is_mailbox(chan))
        return SS$_IVCHAN;
    /* a buffer shorter than the request header carries no request; OpenVMS
     * accepts it all the same (observed OPR.SHORT) and there is nothing to log */
    if (msgbuf->dsc$w_length < OPC$K_MS_HDRLEN)
        return SS$_NORMAL;

    const char *blk = (const char *)msgbuf->dsc$a_pointer;
    size_t textlen = (size_t)msgbuf->dsc$w_length - OPC$K_MS_HDRLEN;

    char msgtext[512];
    if (textlen > sizeof(msgtext) - 1)
        textlen = sizeof(msgtext) - 1;
    memcpy(msgtext, blk + OPC$K_MS_HDRLEN, textlen);
    msgtext[textlen] = '\0';

    /* Format timestamp */
    char timestamp[32];
    format_vms_timestamp(timestamp, sizeof(timestamp));

    /* The user name the executive holds for this process -- empty when it
     * holds none, and empty is then what the header carries. */
    char username[VMS_USERNAME_SIZE];
    get_current_username(username, sizeof(username));

    /* The real configured node identity (SYSGEN SCSNODE, falling back to
     * OVMX's own default) -- not a hardcoded "OVMX" literal. */
    char node[OVMX_IDENTITY_MAXLEN];
    ovmx_node_name(node, sizeof(node));

    /* Thread-safe static request counter */
    static volatile unsigned int req_count = 0;
    unsigned int this_req = __atomic_add_fetch(&req_count, 1, __ATOMIC_SEQ_CST);

    /*
     * Build the OPCOM header, oracle-exact (see format_opcom_banner()
     * above): the boxed banner line, then "Request N, from user U on N" --
     * the numbered-request body-line-2 variant, preserved rather than
     * replaced with the bare "Message from user U on N" form, because
     * every sys$sndopr call in this tree already assigns a request number
     * (this same req_count counter, unchanged by this fix).
     */
    char banner[64];
    format_opcom_banner(banner, sizeof(banner), timestamp);

    char reqline[128];
    snprintf(reqline, sizeof(reqline), "Request %u, from user %s on %s",
             this_req, username, node);

    /* Trim trailing whitespace from the message body */
    size_t msglen = strlen(msgtext);
    while (msglen > 0 &&
           (msgtext[msglen - 1] == '\n' || msgtext[msglen - 1] == '\r' ||
            msgtext[msglen - 1] == ' '))
        msglen--;
    msgtext[msglen] = '\0';

    /*
     * Write the record to SYS$MANAGER:OPERATOR.LOG: the genuine RMS $PUT-at-EOF
     * over the Files-11 ODS-2 ACP when the on-volume log is writable (one
     * stream-LF record per line, the first append $CREATEing it, a trailing
     * blank reproducing OPCOM's inter-record separator), else the legacy
     * host/console writer -- see operator_log_put_record() for the full
     * on-volume-first / console-fallback contract (vms-aac; Rule 9 / INV-6).
     */
    return operator_log_put_record(banner, reqline, msgtext);
}

/*
 * $BRKTHRU / $BRKTHRUW (rd vms-eb46), as observed on OpenVMS
 * (docs/oracle/semantics/brk/):
 *   - the IOSB is cleared first; its second word counts the terminals the
 *     message reached, the third those that timed out, the fourth those that
 *     refused it;
 *   - a send type outside BRK$C_DEVICE..BRK$C_ALLTERMS, or a message class
 *     outside BRK$C_GENERAL..BRK$C_OPCOM and BRK$C_USER1..USER16, is
 *     SS$_BADPARAM; a DEVICE or USERNAME broadcast with no sendto is
 *     SS$_ACCVIO; a device that does not exist is SS$_IVDEVNAM;
 *   - the message goes to the terminals themselves -- this process's own
 *     (TT:), a device the executive's device table names, every terminal a
 *     user is logged in on, or every terminal -- with the carriage control
 *     carcon asks for (0x20: on a line of its own).
 * Which terminals exist and who is logged in on each is the executive's device
 * table (vms_kif_devscan / vms_kif_terminal_getlogin), never a host lookup.
 */
#include "ovmx_console.h"
#include "brkdef.h"
#include "dcdef.h"

static int brk_write(const char *devnam, const char *text, size_t len, uint32_t carcon)
{
    char path[256] = "";
    int fd = -1;
    if (ovmx_console_terminal_path(devnam, path, sizeof path)) {
        fd = open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (fd < 0 && isatty(STDOUT_FILENO))
            fd = dup(STDOUT_FILENO);          /* the console line this process runs on */
    } else {
        char backing[128] = "";
        if (vms_kif_terminal_resolve(devnam, backing, sizeof backing) & 1) {
            snprintf(path, sizeof path, "%s", backing);
            fd = open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK);
        }
    }
    if (fd < 0)
        return 0;
    /* carcon 0x20: the message on a line of its own -- it ends with a new
     * line too, so whatever the process writes next starts a line */
    if (carcon == 0x20) (void)!write(fd, "\r\n", 2);
    if (len) (void)!write(fd, text, len);
    if (carcon == 0x20) (void)!write(fd, "\r\n", 2);
    close(fd);
    return 1;
}

static int brk_is_own_terminal(const char *upper)
{
    return !strcmp(upper, "TT") || !strcmp(upper, "TT0") ||
           !strcmp(upper, "OPA0") || !strcmp(upper, "_OPA0");
}

uint32_t (sys$brkthruw)(uint32_t efn,
                         struct dsc$descriptor_s *msgbuf,
                         struct dsc$descriptor_s *sendto,
                         uint32_t sndtyp,
                         struct _iosb *iosb,
                         uint32_t carcon,
                         uint32_t flags,
                         uint32_t reqid,
                         uint32_t timout,
                         void (*astadr)(uint32_t),
                         uint32_t astprm)
{
    (void)flags; (void)timout;

    if (iosb) memset(iosb, 0, 8);
    if (sndtyp < BRK$C_DEVICE || sndtyp > BRK$C_ALLTERMS)
        return SS$_BADPARAM;
    if (!(reqid <= 7 || (reqid >= 32 && reqid <= 47)))
        return SS$_BADPARAM;
    if (!msgbuf)
        return SS$_ACCVIO;
    if ((sndtyp == BRK$C_DEVICE || sndtyp == BRK$C_USERNAME) && !sendto)
        return SS$_ACCVIO;

    const char *text = msgbuf->dsc$a_pointer ? msgbuf->dsc$a_pointer : "";
    size_t len = msgbuf->dsc$a_pointer ? msgbuf->dsc$w_length : 0;
    char target[64] = "";
    if (sendto && sendto->dsc$a_pointer)
        dsc$strncpy(target, sendto, sizeof target);
    for (char *c = target; *c; c++) *c = (char)toupper((unsigned char)*c);
    size_t tl = strlen(target);
    while (tl && target[tl - 1] == ' ') target[--tl] = '\0';
    if (tl && target[tl - 1] == ':') target[--tl] = '\0';

    uint16_t sent = 0;
    if (sndtyp == BRK$C_DEVICE) {
        if (brk_is_own_terminal(target)) {
            sent += (uint16_t)brk_write("OPA0:", text, len, carcon);
        } else {
            char dn[72];
            struct vms_devinfo di;
            snprintf(dn, sizeof dn, "%s:", target);
            if (!(vms_kif_getdvi_devnam(dn, &di) & 1) || di.devclass != DC$_TERM)
                return SS$_IVDEVNAM;
            sent += (uint16_t)brk_write(di.devnam, text, len, carcon);
        }
    } else {
        /* every terminal (ALLTERMS / ALLUSERS), or every one the user is
         * logged in on (USERNAME) */
        uint32_t idx = 0;
        struct vms_devinfo di;
        while (vms_kif_devscan(&idx, &di) & 1) {
            if (di.devclass != DC$_TERM) continue;
            if (sndtyp == BRK$C_USERNAME) {
                char who[VMS_USERNAME_SIZE] = "";
                if (!(vms_kif_terminal_getlogin(di.devnam, who, sizeof who) & 1))
                    continue;
                size_t wl = strlen(who);
                while (wl && who[wl - 1] == ' ') who[--wl] = '\0';
                if (strcasecmp(who, target) != 0) continue;
            } else if (sndtyp == BRK$C_ALLUSERS) {
                char who[VMS_USERNAME_SIZE] = "";
                if (!(vms_kif_terminal_getlogin(di.devnam, who, sizeof who) & 1) || !who[0])
                    continue;
            }
            sent += (uint16_t)brk_write(di.devnam, text, len, carcon);
        }
    }

    if (iosb) {
        uint16_t *w = (uint16_t *)iosb;
        w[0] = (uint16_t)SS$_NORMAL;
        w[1] = sent;
        w[2] = 0;
        w[3] = 0;
    }
    if ((efn & 0xFFu) < 128) sys$setef(efn);
    if (astadr) astadr(astprm);
    return SS$_NORMAL;
}
