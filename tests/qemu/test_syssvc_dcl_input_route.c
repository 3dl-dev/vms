/*
 * test_syssvc_dcl_input_route.c - the shipped DCL.EXE reads its command lines,
 * and the data of READ SYS$INPUT, from the SYS$INPUT process-permanent file
 * through the VMS service that source takes (rd vms-4d27, vms-67a, vms-91fe,
 * vms-ee7): a mailbox by $QIOW IO$_READVBLK, one message a record; a disk
 * file by RMS $GET, one record a line.
 *
 * WHAT VMS DOES. DCL reads SYS$INPUT record by record. A subprocess whose
 * SYS$INPUT is a mailbox (LIB$SPAWN INPUT=, a parent driving it) takes each
 * message as the next command line; a process whose SYS$INPUT is a command
 * file reads the file's records in order. READ SYS$INPUT, in either, takes
 * the NEXT record of that same stream -- the data line that follows the READ
 * command -- and a command ending in '-' continues on the next record (VSI
 * OpenVMS DCL Dictionary, READ and SYS$INPUT; I/O User's Reference, mailbox
 * driver).
 *
 * THE TWO DRIVES. DCL runs with its substrate stdin on the null device, so a
 * command can reach it only through what SYS$INPUT names; its SYS$OUTPUT
 * names a file on the system volume, read back through RMS after LOGOUT.
 *   MAILBOX: SYS$INPUT is a $CREMBX'd mailbox; the parent $QIOs each line.
 *   FILE:    SYS$INPUT names a command file the parent laid down through RMS.
 * Both carry the same script: compute NUM = 6*7, READ SYS$INPUT DATA with the
 * data record right after it, and WRITE "IN4D27:<NUM>:<DATA>" split over a
 * continuation. The record IN4D27:42:<data> exists only if DCL read every one
 * of those records, in order, from SYS$INPUT.
 *
 * NEGATIVE CONTROLS: tests/qemu/defects.d/dcl-sysinput-mbx-not-read.defect
 * empties every mailbox record DCL reads (the MAILBOX assertion reddens);
 * dcl-sysinput-file-not-read.defect ends the file stream at its first $GET
 * (the FILE assertion reddens).
 *
 * NO EXECUTIVE: $CREMBX must fail SS$_NOSUCHDEV (Rule 9 / INV-6); EXIT_SKIP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"
#include "stsdef.h"
#include "vms_kif.h"
#include "rms_textfile.h"
#include "ovmx_test_fork.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77
#define DCL_PATH_DEFAULT "/tests/DCL.EXE"
#define DRIVE_TIMEOUT_MS 30000
#define COMFILE  "VDA0:[OVMXDIR]IN4D27.COM"
#define OUT_MBX  "VDA0:[OVMXDIR]IN4D27M.LIS"
#define OUT_FILE "VDA0:[OVMXDIR]IN4D27F.LIS"
#define DCL_ERRLOG "/tmp/dcl_input_route.err"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char *dcl_path(void)
{
    const char *p = getenv("OVMX_DCL");
    return (p && p[0]) ? p : DCL_PATH_DEFAULT;
}

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

static int define_system(const char *name, const char *equiv)
{
    struct dsc$descriptor_s tab = mkdsc("LNM$SYSTEM"), nm = mkdsc(name);
    struct item_list_3 it[2];
    memset(it, 0, sizeof it);
    it[0].buflen = (uint16_t)strlen(equiv);
    it[0].item_code = LNM$_STRING;
    it[0].bufaddr = (void *)equiv;
    return (int)sys$crelnm(NULL, &tab, &nm, NULL, it);
}

static void undefine_system(const char *name)
{
    struct dsc$descriptor_s tab = mkdsc("LNM$SYSTEM"), nm = mkdsc(name);
    (void)sys$dellnm(&tab, &nm, NULL);
}

/* The script, one record a line: the data record follows the READ, and the
 * WRITE continues on the record after its '-'. */
static const char *script(int dollar, const char *data, int i)
{
    static char line[256];
    const char *body[] = {
        "NUM = 6 * 7",
        "READ SYS$INPUT DATA",
        NULL,                                  /* the data record */
        "WRITE SYS$OUTPUT -",
        "\"IN4D27:''NUM':''DATA'\"",
        "LOGOUT",
    };
    if (i >= (int)(sizeof body / sizeof body[0]))
        return NULL;
    if (!body[i])
        return data;
    /* a continuation record carries no '$' */
    snprintf(line, sizeof line, "%s%s", (dollar && i != 4) ? "$ " : "", body[i]);
    return line;
}

static pid_t start_dcl(void)
{
    pid_t pid = ovmx_fork_subprocess();
    if (pid == 0) {
        /* stdin and stdout on the null device: a command reaches DCL only
         * through what SYS$INPUT names, its output only through SYS$OUTPUT */
        int devnull = open("/dev/null", O_RDWR);
        int errfd = open(DCL_ERRLOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); }
        if (errfd >= 0) dup2(errfd, 2);
        execl(dcl_path(), dcl_path(), (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int reap(pid_t pid)
{
    int ws = 0;
    for (int w = 0; w < DRIVE_TIMEOUT_MS; w += 20) {
        pid_t r = waitpid(pid, &ws, WNOHANG);
        if (r == pid) return 1;
        if (r < 0) return 0;
        usleep(20000);
    }
    kill(pid, 9);
    (void)waitpid(pid, &ws, 0);
    return 0;
}

/* Does the SYS$OUTPUT file hold the record "IN4D27:42:<data>"? */
static int output_has(const char *spec, const char *data)
{
    char want[128], rec[1024];
    int found = 0, nrec = 0, too_long = 0;
    snprintf(want, sizeof want, "IN4D27:42:%s", data);
    rms_textfile_t *tf = rms_textfile_open(spec);
    printf("  info: %s %s through RMS\n", spec, tf ? "opens" : "does NOT open");
    while (tf && rms_textfile_getline(tf, rec, sizeof rec, &too_long)) {
        size_t l = strlen(rec);
        while (l && (rec[l - 1] == '\n' || rec[l - 1] == '\r'))
            rec[--l] = '\0';
        if (nrec++ < 8)
            printf("  info: record %d: %s\n", nrec, rec);
        if (strcmp(rec, want) == 0)
            found = 1;
    }
    rms_textfile_close(tf);
    if (!found) {
        FILE *e = fopen(DCL_ERRLOG, "r");
        char line[512];
        while (e && fgets(line, sizeof line, e))
            printf("  info: DCL SYS$ERROR: %s", line);
        if (e) fclose(e);
    }
    return found;
}

/* ---- MAILBOX ---- */
static void mailbox_case(void)
{
    struct dsc$descriptor_s indsc = mkdsc("SYS$INPUT");
    uint16_t in_chan = 0;
    CHECK(sys$crembx(0, &in_chan, 1024, 1024, 0, 0, &indsc) & 1,
          "mailbox: $CREMBX of the SYS$INPUT command mailbox succeeds");
    CHECK(define_system("SYS$OUTPUT", OUT_MBX) & 1,
          "mailbox: SYS$OUTPUT names a file on the system volume");
    pid_t pid = start_dcl();
    int sent = 1;
    for (int i = 0; script(0, "", i); i++) {
        const char *l = script(0, "MBXDATA", i);
        struct _iosb iosb = {0};
        uint32_t st = sys$qiow(0, in_chan, IO$_WRITEVBLK, &iosb, NULL, 0,
                               (void *)l, (uint32_t)strlen(l), 0, 0, 0, 0);
        sent &= (st & 1) != 0;
    }
    CHECK(sent, "mailbox: every script record is $QIO-written into the SYS$INPUT mailbox");
    /* negctl-knockon: dcl-sysinput-mbx-not-read */
    CHECK(reap(pid), "mailbox: DCL reads LOGOUT from the mailbox and exits");
    undefine_system("SYS$OUTPUT");
    /* negctl: dcl-sysinput-mbx-not-read */
    CHECK(output_has(OUT_MBX, "MBXDATA"),
          "mailbox: DCL read its commands, the READ SYS$INPUT data record and a continuation, in order, as SYS$INPUT mailbox messages");
    (void)sys$dassgn(in_chan);
    undefine_system("SYS$INPUT");
}

/* ---- FILE ---- */
static void file_case(void)
{
    int laid = 1;
    for (int i = 0; script(1, "", i); i++) {
        const char *l = script(1, "FILEDATA", i);
        laid &= (i == 0 ? rms_textfile_write_line(COMFILE, l)
                        : rms_textfile_append_line(COMFILE, l)) == 0;
    }
    CHECK(laid, "file: the command file is laid down on the system volume through RMS");
    CHECK(define_system("SYS$INPUT", COMFILE) & 1,
          "file: SYS$INPUT names the command file");
    CHECK(define_system("SYS$OUTPUT", OUT_FILE) & 1,
          "file: SYS$OUTPUT names a file on the system volume");
    pid_t pid = start_dcl();
    CHECK(reap(pid), "file: DCL reads LOGOUT from the command file and exits");
    undefine_system("SYS$INPUT");
    undefine_system("SYS$OUTPUT");
    /* negctl: dcl-sysinput-file-not-read */
    CHECK(output_has(OUT_FILE, "FILEDATA"),
          "file: DCL read its commands, the READ SYS$INPUT data record and a continuation, in order, as RMS records of the SYS$INPUT file");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    printf("=== test_syssvc_dcl_input_route (DCL reads SYS$INPUT from a mailbox by $QIO, from a file by RMS, vms-4d27) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (!executive_present()) {
        uint16_t chan = 0;
        struct dsc$descriptor_s lognam = mkdsc("SYS$INPUT");
        uint32_t st = sys$crembx(0, &chan, 0, 0, 0, 0, &lognam);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: $CREMBX fails SS$_NOSUCHDEV, never a local per-process fallback");
        printf("=== test_syssvc_dcl_input_route: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }
    struct stat sb;
    if (stat(dcl_path(), &sb) != 0) {
        printf("  FAIL: shipped DCL image not found at %s\n", dcl_path());
        return 1;
    }
    CHECK($VMS_STATUS_SUCCESS(vms_kif_acp_mount("VDA0:")),
          "MOUNT VDA0: (writable), the volume the command and output files go on");

    file_case();
    mailbox_case();

    printf("=== test_syssvc_dcl_input_route: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
