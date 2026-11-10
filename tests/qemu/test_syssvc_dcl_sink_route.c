/*
 * test_syssvc_dcl_sink_route.c - the shipped DCL.EXE writes its OWN output
 * where SYS$OUTPUT names, through the VMS service that destination takes
 * (rd vms-1b6a): a mailbox by $QIO IO$_WRITEVBLK, one record per message; a
 * disk file by RMS $PUT, one record per line.
 *
 * WHAT VMS DOES. DCL's output -- SHOW, WRITE, its messages -- is records
 * written to the process-permanent file SYS$OUTPUT. A subprocess whose
 * SYS$OUTPUT is a mailbox (LIB$SPAWN OUTPUT=) delivers each record as one
 * mailbox message; a process whose SYS$OUTPUT is a disk file (a batch log,
 * RUN/DETACHED /OUTPUT=) leaves each record in that file (VSI OpenVMS DCL
 * Dictionary, SYS$OUTPUT; I/O User's Reference, mailbox driver).
 *
 * THE TWO DRIVES. The parent $CREMBXes a SYS$INPUT command mailbox (published
 * as SYS$INPUT in LNM$SYSTEM, as test_syssvc_mbx_dcldrv does) and runs the
 * shipped DCL.EXE with its substrate stdout/stderr on the null device, so
 * nothing DCL writes can reach anyone through a descriptor.
 *   MAILBOX: SYS$OUTPUT is a second mailbox. `SHOW SYMBOL NUM` -- DCL's own
 *     formatted output, not WRITE -- and `WRITE SYS$OUTPUT` must each arrive
 *     as exactly one mailbox message (a record), read through a SEQPACKET
 *     relay that keeps message boundaries.
 *   FILE: SYS$OUTPUT names a file on the system volume. After LOGOUT the file,
 *     read back through RMS, must hold `SHOW SYMBOL`'s line and the WRITE's
 *     record, each a record of its own.
 * The computed value (6*7) exists nowhere unless DCL evaluated it.
 *
 * NEGATIVE CONTROL: tests/qemu/defects.d/dcl-sysout-file-route-dropped.defect
 * makes the file route drop every record; the FILE assertion reddens.
 *
 * NO EXECUTIVE: $CREMBX must fail SS$_NOSUCHDEV (Rule 9 / INV-6); EXIT_SKIP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
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
#define OUTFILE "VDA0:[OVMXDIR]SINK1B6A.LIS"
#define MARK "SINK1B6A:42"

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

static int send_command(uint16_t chan, const char *cmd)
{
    struct _iosb iosb = {0};
    uint32_t st = sys$qiow(0, chan, IO$_WRITEVBLK, &iosb, NULL, 0,
                           (void *)cmd, (uint32_t)strlen(cmd), 0, 0, 0, 0);
    return (st & 1) ? 0 : -1;
}

static int translate(const char *logical, char *out, size_t outsz)
{
    struct dsc$descriptor_s namdsc = mkdsc(logical);
    uint16_t rl = 0;
    struct item_list_3 itmlst[2];
    memset(itmlst, 0, sizeof(itmlst));
    itmlst[0].buflen    = (uint16_t)(outsz - 1);
    itmlst[0].item_code = LNM$_STRING;
    itmlst[0].bufaddr   = out;
    itmlst[0].retlen    = &rl;
    if (!(sys$trnlnm(NULL, NULL, &namdsc, NULL, itmlst) & 1) || rl == 0)
        return 0;
    out[rl] = '\0';
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

/* Each message of the SYS$OUTPUT mailbox, one SEQPACKET datagram apiece. */
struct out_reader_arg { char devname[64]; int sock; };

static void *out_reader_main(void *argp)
{
    struct out_reader_arg *a = argp;
    char buf[4096];
    uint32_t exec_chan = 0;

    if (!(vms_kif_mbx_assign(a->devname, &exec_chan) & 1))
        return NULL;
    for (;;) {
        uint32_t actlen = 0;
        if (!(vms_kif_mbx_read(exec_chan, buf, sizeof buf, &actlen, 0) & 1))
            break;
        if (send(a->sock, buf, actlen > sizeof buf ? sizeof buf : actlen, 0) < 0)
            break;
    }
    return NULL;
}

#define DCL_ERRLOG "/tmp/dcl_sink_route.err"

static pid_t start_dcl(void)
{
    pid_t pid = ovmx_fork_subprocess();
    if (pid == 0) {
        /* stdout on the null device: DCL's records can reach the parent only
         * through the destination SYS$OUTPUT names. stderr (SYS$ERROR is not
         * defined) to a scratch file the suite shows when a check fails. */
        int devnull = open("/dev/null", O_RDWR);
        int errfd = open(DCL_ERRLOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); }
        if (errfd >= 0) dup2(errfd, 2);
        execl(dcl_path(), dcl_path(), (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void reap(pid_t pid)
{
    int ws = 0;
    for (int w = 0; w < DRIVE_TIMEOUT_MS; w += 20) {
        pid_t r = waitpid(pid, &ws, WNOHANG);
        if (r == pid || r < 0) return;
        usleep(20000);
    }
    kill(pid, 9);
    (void)waitpid(pid, &ws, 0);
}

static void drive(uint16_t in_chan)
{
    (void)send_command(in_chan, "NUM = 6 * 7");
    (void)send_command(in_chan, "SHOW SYMBOL NUM");
    (void)send_command(in_chan, "WRITE SYS$OUTPUT \"" "SINK1B6A:''NUM'" "\"");
    (void)send_command(in_chan, "LOGOUT");
}

/* ---- MAILBOX ---- */
static void mailbox_case(uint16_t in_chan)
{
    uint16_t out_chan = 0;
    struct dsc$descriptor_s outdsc = mkdsc("SYS$OUTPUT");
    uint32_t os = sys$crembx(0, &out_chan, 1024, 1024, 0, 0, &outdsc);
    CHECK(os & 1, "mailbox: $CREMBX of the SYS$OUTPUT mailbox succeeds");
    if (!(os & 1))
        return;

    int sv[2];
    static struct out_reader_arg ora;
    memset(&ora, 0, sizeof ora);
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0 ||
        !translate("SYS$OUTPUT", ora.devname, sizeof ora.devname)) {
        CHECK(0, "mailbox: the SYS$OUTPUT mailbox's device name resolves");
        return;
    }
    ora.sock = sv[1];
    pthread_t rd;
    if (pthread_create(&rd, NULL, out_reader_main, &ora) != 0) {
        CHECK(0, "mailbox: output reader starts");
        return;
    }
    pthread_detach(rd);

    pid_t pid = start_dcl();
    drive(in_chan);

    int show_rec = 0, write_rec = 0;
    for (int waited = 0; waited < DRIVE_TIMEOUT_MS && !write_rec; ) {
        struct pollfd pfd = { .fd = sv[0], .events = POLLIN };
        if (poll(&pfd, 1, 200) <= 0) { waited += 200; continue; }
        char msg[4097];
        ssize_t n = recv(sv[0], msg, sizeof msg - 1, 0);
        if (n <= 0) continue;
        msg[n] = '\0';
        if (strstr(msg, "NUM = 42") && strchr(msg, '\n') == msg + n - 1)
            show_rec = 1;
        if (strcmp(msg, MARK "\n") == 0)
            write_rec = 1;
    }
    CHECK(show_rec,
          "mailbox: DCL's own SHOW SYMBOL output arrives over the SYS$OUTPUT mailbox as one record");
    CHECK(write_rec,
          "mailbox: WRITE SYS$OUTPUT arrives over the SYS$OUTPUT mailbox as exactly one record");
    reap(pid);
    (void)sys$dassgn(out_chan);
    undefine_system("SYS$OUTPUT");
}

/* ---- FILE ---- */
static void file_case(uint16_t in_chan)
{
    CHECK($VMS_STATUS_SUCCESS(vms_kif_acp_mount("VDA0:")),
          "file: MOUNT VDA0: (writable), the volume the SYS$OUTPUT file goes on");
    CHECK(define_system("SYS$OUTPUT", OUTFILE) & 1,
          "file: SYS$OUTPUT names a file on the system volume");
    pid_t pid = start_dcl();
    drive(in_chan);
    reap(pid);
    undefine_system("SYS$OUTPUT");

    int show_rec = 0, write_rec = 0, nrec = 0;
    rms_textfile_t *tf = rms_textfile_open(OUTFILE);
    printf("  info: %s %s through RMS\n", OUTFILE, tf ? "opens" : "does NOT open");
    if (tf) {
        char rec[1024];
        int too_long = 0;
        while (rms_textfile_getline(tf, rec, sizeof rec, &too_long)) {
            size_t l = strlen(rec);
            while (l && (rec[l - 1] == '\n' || rec[l - 1] == '\r'))
                rec[--l] = '\0';
            if (nrec++ < 8)
                printf("  info: record %d: %s\n", nrec, rec);
            if (strstr(rec, "NUM = 42"))
                show_rec = 1;
            if (strcmp(rec, MARK) == 0)
                write_rec = 1;
        }
        rms_textfile_close(tf);
    }
    if (!(show_rec && write_rec)) {
        FILE *e = fopen(DCL_ERRLOG, "r");
        char line[512];
        while (e && fgets(line, sizeof line, e))
            printf("  info: DCL SYS$ERROR: %s", line);
        if (e) fclose(e);
    }
    /* negctl: dcl-sysout-file-route-dropped */
    CHECK(show_rec && write_rec,
          "file: DCL's SHOW SYMBOL output and its WRITE are records of the SYS$OUTPUT file, read back through RMS");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    printf("=== test_syssvc_dcl_sink_route (DCL's output to a SYS$OUTPUT mailbox by $QIO, to a file by RMS, vms-1b6a) ===\n");

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
        printf("=== test_syssvc_dcl_sink_route: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }
    struct stat sb;
    if (stat(dcl_path(), &sb) != 0) {
        printf("  FAIL: shipped DCL image not found at %s\n", dcl_path());
        return 1;
    }

    /* One command mailbox per drive: a DCL that never read its LOGOUT must
     * not hand its commands to the next one. The FILE drive runs first: the
     * mailbox drive's SYS$OUTPUT mailbox stays alive (and named) while its
     * reader thread holds a channel to it, and would shadow the file. */
    struct dsc$descriptor_s indsc = mkdsc("SYS$INPUT");
    uint16_t in1 = 0, in2 = 0;
    CHECK(sys$crembx(0, &in1, 1024, 1024, 0, 0, &indsc) & 1,
          "parent: $CREMBX of the SYS$INPUT command mailbox succeeds");
    file_case(in1);
    (void)sys$dassgn(in1);
    undefine_system("SYS$INPUT");
    undefine_system("SYS$OUTPUT");

    CHECK(sys$crembx(0, &in2, 1024, 1024, 0, 0, &indsc) & 1,
          "parent: $CREMBX of the second SYS$INPUT command mailbox succeeds");
    mailbox_case(in2);
    (void)sys$dassgn(in2);
    undefine_system("SYS$INPUT");
    undefine_system("SYS$OUTPUT");

    printf("=== test_syssvc_dcl_sink_route: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
