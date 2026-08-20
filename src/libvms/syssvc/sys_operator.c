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

/* Operator log file path */
#include "ovmx_layout.h"
/*
 * OPERATOR.LOG is written the VMS way now (vms-aac, epic vms-208 atomic flip):
 * one RMS $PUT-at-EOF record per line over the Files-11 ODS-2 ACP when the
 * SYSTEM DISK holding SYS$MANAGER is MOUNTED (rms_textfile.c carries the
 * RMS-over-ACP writer). When there is no mounted volume for the log -- no
 * /dev/vms (host ctest / plain container / netbsd-vax cross), OR /dev/vms
 * present but the system disk not yet mounted (early boot / an isolated
 * executive, as in the test_syssvc_* harness) -- there is no on-volume log, so
 * the record defers to the legacy host/console writer (open_operator_log_host()
 * below), where real VMS OPCOM writes before OPERATOR.LOG opens and the SAME
 * defer RMS and IMGACT take (rms_operator_log_absent() / rms_executive_absent()).
 * Once the log's volume IS mounted the ACP is the only path and an ACP write
 * that fails fails honestly -- never a private POSIX log on the retired /vms
 * passthrough (Rule 9 / INV-6).
 */
#include "rms_textfile.h"
#include "vmsfs/filespec.h"
#define OPERATOR_LOG_PATH VMS_OPERATOR_LOG

/* Fallback operator log, writable when SYS$MANAGER: does not resolve to a
 * populated tree (dev seat / plain container). */
#define OPERATOR_LOG_FALLBACK "/tmp/OPERATOR.LOG"

/*
 * MOUNTED-VOLUME DEFER (vms-aac, epic vms-208 atomic flip; Rule 9 / INV-6).
 *
 * OPERATOR.LOG is written over the Files-11 ODS-2 ACP whenever the SYSTEM DISK
 * holding SYS$MANAGER is MOUNTED (the flip's genuine path --
 * rms_textfile_append_line -> RMS $PUT-at-EOF on the on-volume log). When there
 * is no such mounted volume -- no /dev/vms (host ctest, plain-container gate,
 * netbsd-vax cross), OR /dev/vms present but the system disk not yet mounted
 * (early boot, or an isolated executive with no mounted volume, as in the
 * test_syssvc_* harness) -- there is no on-volume OPERATOR.LOG to write, so the
 * record is written the LEGACY host way instead: the console/host log, exactly
 * where real VMS OPCOM writes before SYS$MANAGER:OPERATOR.LOG opens at startup,
 * and the SAME defer RMS's own $OPEN/$CREATE and IMGACT's imgsrc_open() take
 * when /dev/vms is absent (rms_operator_log_absent() / rms_executive_absent(),
 * src/vmsrms/rms_core.c). This is NOT a /vms masquerade with a mounted volume:
 * once the log's volume is mounted the ACP is the only path taken, and an ACP
 * write that then fails fails honestly (INV-6) -- the defer covers "no mounted
 * volume", never masks a real write error on a mounted one.
 *
 * rms_operator_log_absent()/rms_executive_absent() live in LIBVMSRMS, which
 * links LIBVMS (this file), so they are referenced WEAKLY -- the same
 * library-layering seam rms_textfile.c uses for the RMS services. An image that
 * links vmsrms (DCL, LOGINOUT, VMSSSHD, PROVISION) binds the real probe; an
 * image that does not has no ACP at all, so "absent" is the correct reading and
 * the legacy writer is used.
 */
#pragma weak rms_executive_absent
extern int rms_executive_absent(void);
#pragma weak rms_operator_log_absent
extern int rms_operator_log_absent(void);

/*
 * Should OPERATOR.LOG be written the LEGACY host way rather than over the ACP?
 *
 * OPERATOR.LOG lives on SYS$MANAGER (== SYS$SYSDEVICE), so it can only be $PUT
 * over the Files-11 ODS-2 ACP once that SYSTEM DISK is MOUNTED. The right
 * question is therefore MOUNT state, not mere /dev/vms presence:
 * rms_operator_log_absent() (vms-aac) reports 1 when there is no mounted boot
 * volume to hold the log -- no /dev/vms, OR /dev/vms present but the volume not
 * yet mounted (early boot; or an isolated executive with no mounted volume, as
 * in the test_syssvc_* harness). In that state OPCOM writes the host
 * log/console, exactly as real VMS OPCOM does before OPERATOR.LOG opens at
 * startup. It returns 0 ONLY for a mounted volume, so an ACP $PUT that then
 * fails on a mounted volume still fails honestly below (Rule 9 / INV-6) -- this
 * defer is for "no volume", never a mask over a real on-volume write error.
 *
 * Falls back to the /dev/vms-presence probe (rms_executive_absent), then to
 * "absent", for an image that links neither (no LIBVMSRMS -> no ACP at all).
 */
static int operator_log_executive_absent(void)
{
    if (rms_operator_log_absent)
        return rms_operator_log_absent();
    if (rms_executive_absent)
        return rms_executive_absent();
    return 1;   /* no RMS engine in this image -> no ACP reachable */
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
    (void)chan;

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
    if (msgbuf->dsc$w_length < OPC$K_MS_HDRLEN)
        return SS$_BADPARAM;

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
     * Append the record to SYS$MANAGER:OPERATOR.LOG over the Files-11 ODS-2
     * ACP via RMS $PUT-at-EOF, one stream-LF record per line (vms-aac). The
     * first append $CREATEs the log if it does not yet exist; each is
     * re-resolved and positioned at EOF, so it accumulates the way OPCOM's
     * append-mode log does. A trailing empty record reproduces the blank
     * line the old fprintf("%s\n\n") separator wrote between records.
     *
     * NO MOUNTED VOLUME (host ctest / plain container / netbsd-vax cross; or an
     * executive with no system disk mounted yet -- early boot / the isolated
     * test_syssvc_* harness): there is no on-volume OPERATOR.LOG, so the record
     * is written the legacy host/console way instead -- the same defer RMS and
     * IMGACT take, and where real VMS OPCOM writes before the log opens
     * (operator_log_executive_absent() above). This is NOT a /vms fall-back once
     * the log's volume is mounted (Rule 9 / INV-6).
     */
    if (operator_log_executive_absent()) {
        FILE *log = open_operator_log_host();
        if (!log)
            return SS$_FILACCERR;
        fprintf(log, "%s\n", banner);
        fprintf(log, "%s\n", reqline);
        fprintf(log, "%s\n\n", msgtext);
        fclose(log);
        return SS$_NORMAL;
    }

    /*
     * FAIL-HONEST (Rule 9 / INV-6): the log's volume IS mounted, so the ACP is
     * the only path. A first append that returns -1 (write error on the mounted
     * volume) reports SS$_FILACCERR -- it never falls back to a private POSIX
     * file on the retired /vms passthrough.
     */
    if (rms_textfile_append_line(OPERATOR_LOG_PATH, banner) != 0)
        return SS$_FILACCERR;
    rms_textfile_append_line(OPERATOR_LOG_PATH, reqline);
    rms_textfile_append_line(OPERATOR_LOG_PATH, msgtext);
    rms_textfile_append_line(OPERATOR_LOG_PATH, "");

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

        /*
         * Also log to OPERATOR.LOG, in the SAME oracle-exact shape
         * sys$sndopr writes (rd vms-32a) -- one file, one OPCOM record
         * format, not two competing ones. A broadcast carries no request
         * number (it is not a repliable request), so this uses the plain
         * "Message from user U on N" body-line-2 variant rather than
         * sndopr's "Request N, ..." form -- the caller's identity comes
         * from the same executive-row read sndopr uses, not from the
         * target terminal (which the OLD line here wrote into the user
         * field by mistake).
         */
        char btimestamp[32];
        char buser[VMS_USERNAME_SIZE];
        char bnode[OVMX_IDENTITY_MAXLEN];
        char bbanner[64];
        char bmsgline[128];

        format_vms_timestamp(btimestamp, sizeof(btimestamp));
        get_current_username(buser, sizeof(buser));
        ovmx_node_name(bnode, sizeof(bnode));
        format_opcom_banner(bbanner, sizeof(bbanner), btimestamp);
        snprintf(bmsgline, sizeof(bmsgline), "Message from user %s on %s",
                 buser, bnode);

        /* Same one-file, one-OPCOM-record path sys$sndopr uses (vms-aac): the
         * ACP $PUT-at-EOF writer when the log's system disk is MOUNTED, the
         * legacy host/console writer when there is no mounted volume (host
         * ctest / container / vax cross, or an executive with no system disk
         * mounted yet). Best-effort -- a broadcast that reached its terminal is
         * not failed because the log is unreachable; and once the log's volume
         * is mounted there is no /vms POSIX fall-back (Rule 9 / INV-6). */
        if (operator_log_executive_absent()) {
            FILE *log = open_operator_log_host();
            if (log) {
                fprintf(log, "%s\n", bbanner);
                fprintf(log, "%s\n", bmsgline);
                fprintf(log, "%s\n\n", msgtext);
                fclose(log);
            }
        } else {
            rms_textfile_append_line(OPERATOR_LOG_PATH, bbanner);
            rms_textfile_append_line(OPERATOR_LOG_PATH, bmsgline);
            rms_textfile_append_line(OPERATOR_LOG_PATH, msgtext);
            rms_textfile_append_line(OPERATOR_LOG_PATH, "");
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
