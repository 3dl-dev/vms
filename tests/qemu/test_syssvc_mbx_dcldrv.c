/*
 * test_syssvc_mbx_dcldrv.c - a parent drives the REAL, SHIPPED DCL.EXE as a
 * persistent subprocess over two mailboxes: it feeds command lines into DCL's
 * SYS$INPUT mailbox and reads DCL's results back from its SYS$OUTPUT mailbox,
 * through the PUBLIC sys$ API against a real /dev/vms (vms-786).
 *
 * ============================================================
 * THIS IS THE POINT OF vms-786. DCL's command loop is fd/stdio-based
 * (dcl_main.c reads stdin with fgets(), writes stdout with printf()) while OVMX
 * mailboxes are executive-resident and have no fd (sys_mailbox.c). So a
 * persistent DCL a parent drives over mailboxes -- MMK's send_cmd_and_wait,
 * which keeps one DCL open and feeds it resolved command lines over one mailbox
 * while reading each command's $STATUS-terminated results back over a second
 * (docs/design-mmk-exec-drive-ovmx.md, vms-b23) -- could not take its SYS$INPUT
 * from a mailbox nor send SYS$OUTPUT to one. src/vmsdcl/dcl_mbx.c closes that:
 * when SYS$INPUT/SYS$OUTPUT translate to a mailbox device, DCL reads commands
 * via $QIO IO$_READVBLK on the input mailbox and writes results via
 * IO$_WRITEVBLK on the output mailbox. This suite proves it end to end, against
 * the shipped DCL.EXE image, not a hand-rolled stand-in.
 * ============================================================
 *
 * INDEPENDENT ORACLE (CLAUDE.md Rule 11 / the veracity crux). The parent does
 * not merely look for bytes it wrote coming back. It sends TWO commands the
 * child must EXECUTE, not echo: first `NUM = 6 * 7` (a DCL symbol assignment),
 * then `WRITE SYS$OUTPUT "OVMX786:''NUM'"` (write the substituted symbol). The
 * result the parent asserts, "OVMX786:42", exists nowhere unless the shipped
 * DCL actually READ both commands out of the SYS$INPUT mailbox, EVALUATED the
 * arithmetic and the symbol substitution, and DELIVERED the computed line back
 * over the SYS$OUTPUT mailbox. The parent knows 6*7==42 but never computed it
 * inside DCL; there is nowhere local for "OVMX786:42" to come from.
 *
 * RENDEZVOUS. Nothing crosses from parent to the DCL child but the two mailbox
 * logical names: the parent $CREMBXes the input mailbox with logical name
 * SYS$INPUT and the output mailbox with logical name SYS$OUTPUT (published in
 * the executive-resident LNM$SYSTEM, vms-d37/vms-d44), then fork+execs the
 * shipped DCL.EXE. That DCL -- a genuinely separate image holding no fd, no
 * unit, no channel from the parent -- translates SYS$INPUT/SYS$OUTPUT through
 * LNM$SYSTEM to the two "MBAn:" units and $ASSIGNs them (src/vmsdcl/dcl_mbx.c
 * -> sys$assign, vms-mb1). This is exactly the mechanism LIB$SPAWN uses: the
 * subprocess reaches its I/O mailboxes by the names wired to SYS$INPUT/
 * SYS$OUTPUT, not by any handle inherited in memory.
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is
 * loaded, exactly as test_syssvc_mbx_cmdresp.c does): $CREMBX must fail
 * SS$_NOSUCHDEV, never fabricate a private per-process mailbox (Rule 9 /
 * INV-6). This suite returns EXIT_SKIP (77) there -- the contract ci.yml's
 * kernel-executive-negative-control job holds every test_syssvc_* suite to.
 *
 * NEGATIVE CONTROL (facility_defects.sh dcl-sysinput-mbx-not-read): the
 * cross-process assertion is anchored by a mutation that makes dcl_mbx.c's
 * reader thread forward ZERO bytes of each mailbox message into DCL's command
 * loop. Under it the DCL child assigns both mailboxes but never actually
 * RECEIVES a command, so it computes nothing and delivers nothing -- the driven
 * result-read reddens -- while the parent's own $CREMBX and its writes into the
 * command mailbox stay green: precisely the A-writes/B-reads shape Rule 11
 * exists to catch.
 *
 * BOUNDED, NO SLEEPS. The parent's blocking read of DCL's output runs in a
 * detached helper thread that funnels each mailbox message into a pipe; the
 * main thread poll()s that pipe with a generous failure bound, so a DCL that
 * never delivers becomes a NAMED FAIL here rather than an unattributable VM
 * timeout. Nothing here sleeps waiting for something to become true.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
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
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

/* Where the harness stages the shipped DCL image (tests/qemu/Dockerfile copies
 * build-static/bin/DCL.EXE to both /bin and /tests). The env var lets the same
 * program be pointed at a host build by hand. */
#define DCL_PATH_DEFAULT "/tests/DCL.EXE"

/* Generous failure bound on how long the driven DCL may take to read two
 * commands and deliver the computed result -- milliseconds under TCG; this is
 * a failure ceiling, not pacing. Well inside run_tests.sh's 120s QEMU timeout,
 * so a wedged DCL becomes a named FAIL, not a VM-level hang. */
#define DRIVE_TIMEOUT_MS 30000

/* The two logical names the parent publishes for the DCL child to translate.
 * They ARE SYS$INPUT / SYS$OUTPUT: dcl_mbx.c translates exactly those names,
 * and defining them in LNM$SYSTEM is contained to this per-suite QEMU boot. */
#define SYSINPUT_NAME  "SYS$INPUT"
#define SYSOUTPUT_NAME "SYS$OUTPUT"

/* The computed marker the child must PRODUCE (6*7 evaluated in DCL). */
#define EXPECT_MARKER  "OVMX786:42"

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

/* Write one command line (no trailing newline) into the command mailbox: DCL's
 * reader thread terminates each mailbox record with a newline itself so its
 * fgets() loop sees a complete line (src/vmsdcl/dcl_mbx.c). */
static int send_command(uint16_t chan, const char *cmd)
{
    struct _iosb iosb = {0};
    uint32_t st = sys$qiow(0, chan, IO$_WRITEVBLK, &iosb, NULL, 0,
                           (void *)cmd, (uint32_t)strlen(cmd), 0, 0, 0, 0);
    return (st & 1) ? 0 : -1;
}

/* Resolve a logical name (SYS$OUTPUT) to its "MBAn:" mailbox device name. The
 * parent published it with $CREMBX; the reader thread assigns its OWN channel to
 * that device rather than reusing the main thread's -- OVMX's userspace channel
 * table (the PCB) is thread-local (src/vmsprocess/vms_pcb.c), so a channel
 * $ASSIGNed on the main thread is not usable from a helper thread's sys$qiow.
 * The executive keys the process by tgid, so a fresh channel the thread assigns
 * by name reaches the same mailbox. */
static int resolve_mbx_devname(const char *logical, char *out, size_t outsz)
{
    struct dsc$descriptor_s namdsc = mkdsc(logical);
    char equiv[256];
    uint16_t rl = 0;
    struct item_list_3 itmlst[2];
    memset(itmlst, 0, sizeof(itmlst));
    itmlst[0].buflen    = (uint16_t)(sizeof(equiv) - 1);
    itmlst[0].item_code = LNM$_STRING;
    itmlst[0].bufaddr   = equiv;
    itmlst[0].retlen    = &rl;
    equiv[0] = '\0';
    uint32_t st = sys$trnlnm(NULL, NULL, &namdsc, NULL, itmlst);
    if (!(st & 1) || rl == 0) return 0;
    if (rl >= sizeof(equiv)) rl = (uint16_t)(sizeof(equiv) - 1);
    equiv[rl] = '\0';
    strncpy(out, equiv, outsz - 1);
    out[outsz - 1] = '\0';
    return 1;
}

/* Helper thread: assign the DCL child's SYS$OUTPUT mailbox BY DEVICE NAME (its
 * own executive channel, reachable from this thread because the executive keys
 * the process by tgid), block on IO$_READVBLK, and funnel each delivered
 * message into a pipe the bounded main thread reads. Detached: if the child
 * never delivers (e.g. the negative control), this stays blocked and is
 * reclaimed at process exit while the main thread's poll() bound turns the miss
 * into a named FAIL. */
struct out_reader_arg {
    char devname[64];
    int  pipe_w;
};

static void *out_reader_main(void *argp)
{
    struct out_reader_arg *a = (struct out_reader_arg *)argp;
    char buf[4096];
    uint32_t exec_chan = 0;

    if (!(vms_kif_mbx_assign(a->devname, &exec_chan) & 1))
        return NULL;

    for (;;) {
        uint32_t actlen = 0;
        uint32_t st = vms_kif_mbx_read(exec_chan, buf, (uint32_t)sizeof(buf),
                                       &actlen, 0 /* block for the child's output */);
        if (!(st & 1))
            break;
        uint32_t n = actlen;
        if (n > sizeof(buf)) n = sizeof(buf);
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(a->pipe_w, buf + off, n - off);
            if (w <= 0) return NULL;
            off += (size_t)w;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_mbx_dcldrv (parent drives the shipped DCL.EXE over SYS$INPUT/SYS$OUTPUT mailboxes, vms-786) ===\n");

    /* A per-process PCB is a prerequisite for every sys$ channel call (the
     * channel table lives in it); a gcc test binary must make its own. Needed on
     * BOTH branches below -- the no-executive branch still calls sys$crembx. */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        /* NO EXECUTIVE: $CREMBX must fail honestly (Rule 9 / INV-6), never a
         * private per-process mailbox. Run on the host before vms.ko. */
        uint16_t chan = 0;
        struct dsc$descriptor_s lognam = mkdsc(SYSINPUT_NAME);
        uint32_t st = sys$crembx(0, &chan, 0, 0, 0, 0, &lognam);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: $CREMBX fails SS$_NOSUCHDEV, never a local per-process fallback");
        printf("=== test_syssvc_mbx_dcldrv: %d passed, %d failed (SKIPPED: no /dev/vms -- DCL mailbox drive not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* The shipped DCL image must be present to drive. */
    struct stat stbuf;
    if (stat(dcl_path(), &stbuf) != 0) {
        printf("  FAIL: shipped DCL image not found at %s\n", dcl_path());
        return 1;
    }

    /* Create the two mailboxes the parent owns, publishing their names as
     * SYS$INPUT and SYS$OUTPUT so the DCL child translates them. The parent
     * holds a channel to each for the whole exchange (writes commands into the
     * input mailbox, reads results from the output mailbox). */
    uint16_t in_chan = 0, out_chan = 0;
    struct dsc$descriptor_s indsc  = mkdsc(SYSINPUT_NAME);
    struct dsc$descriptor_s outdsc = mkdsc(SYSOUTPUT_NAME);
    uint32_t is = sys$crembx(0, &in_chan, 1024, 1024, 0, 0, &indsc);
    uint32_t os = sys$crembx(0, &out_chan, 1024, 1024, 0, 0, &outdsc);
    CHECK(is & 1, "parent: $CREMBX of the SYS$INPUT command mailbox succeeds");
    CHECK(os & 1, "parent: $CREMBX of the SYS$OUTPUT result mailbox succeeds");
    if (!(is & 1) || !(os & 1)) {
        if (in_chan)  (void)sys$dassgn(in_chan);
        if (out_chan) (void)sys$dassgn(out_chan);
        printf("=== test_syssvc_mbx_dcldrv: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* Start the bounded output reader BEFORE launching DCL, so the first byte
     * DCL delivers is captured no matter how fast it starts. It assigns the
     * SYS$OUTPUT mailbox by its published device name (resolved here, on the
     * main thread). */
    int respipe[2];
    if (pipe(respipe) < 0) { printf("  FAIL: pipe() failed\n"); return 1; }
    static struct out_reader_arg ora;
    memset(&ora, 0, sizeof(ora));
    ora.pipe_w = respipe[1];
    if (!resolve_mbx_devname(SYSOUTPUT_NAME, ora.devname, sizeof(ora.devname))) {
        printf("  FAIL: could not resolve the SYS$OUTPUT mailbox device name\n");
        return 1;
    }
    pthread_t reader;
    if (pthread_create(&reader, NULL, out_reader_main, &ora) != 0) {
        printf("  FAIL: could not start output-reader thread\n");
        return 1;
    }
    pthread_detach(reader);

    /* Launch the shipped DCL.EXE as the driven subprocess. Plain invocation
     * (no -c, no script) so it takes the persistent-REPL path where dcl_mbx.c
     * binds SYS$INPUT/SYS$OUTPUT to the mailboxes we published. */
    pid_t pid = fork();
    if (pid < 0) { printf("  FAIL: fork() failed\n"); return 1; }
    if (pid == 0) {
        /* Child = the shipped DCL. It shares nothing with us but the two
         * published logical names; it makes its own PCB and assigns the
         * mailboxes by name. Its stdin/stdout are irrelevant -- dcl_mbx.c
         * rebinds them to the mailboxes -- but send our own stdio to /dev/null
         * so nothing of the child leaks into this suite's output. */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); }
        execl(dcl_path(), dcl_path(), (char *)NULL);
        _exit(127);
    }

    /* Feed the two commands. They queue in the input mailbox; the DCL child's
     * reader thread drains them once it has bound and started -- mailboxes
     * buffer, so there is no ordering race and no need to synchronise. */
    int sent_ok = (send_command(in_chan, "NUM = 6 * 7") == 0) &&
                  (send_command(in_chan, "WRITE SYS$OUTPUT \"OVMX786:''NUM'\"") == 0);
    CHECK(sent_ok, "parent: both command lines written into the SYS$INPUT mailbox");

    /* Bounded wait for the computed result to arrive over the SYS$OUTPUT
     * mailbox. Accumulate whatever the child delivers and scan for the marker
     * only DCL's own evaluation could produce. */
    char acc[8192];
    size_t acclen = 0;
    int got_marker = 0;
    int waited = 0;
    acc[0] = '\0';
    while (waited < DRIVE_TIMEOUT_MS && !got_marker) {
        struct pollfd pfd = { .fd = respipe[0], .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        waited += 200;
        if (pr <= 0) continue;
        ssize_t n = read(respipe[0], acc + acclen,
                         (acclen < sizeof(acc) - 1) ? (sizeof(acc) - 1 - acclen) : 0);
        if (n <= 0) continue;
        acclen += (size_t)n;
        acc[acclen] = '\0';
        if (strstr(acc, EXPECT_MARKER) != NULL)
            got_marker = 1;
    }

    /* THE POINT OF THE ITEM. The shipped DCL read its commands from the
     * SYS$INPUT mailbox, EXECUTED them (arithmetic + symbol substitution), and
     * delivered the computed result over the SYS$OUTPUT mailbox. */
    /* negctl: dcl-sysinput-mbx-not-read */
    CHECK(got_marker,
          "the spawned DCL read its commands from the SYS$INPUT mailbox, executed them, and delivered the computed result over the SYS$OUTPUT mailbox");

    /* Tell the child to leave its loop the VMS way -- a LOGOUT command down
     * SYS$INPUT (the executive mailbox has no writers-gone EOF; the parent ends
     * the subprocess by commanding it). */
    (void)send_command(in_chan, "LOGOUT");

    /* Bounded reap so a wedged child cannot hang the run. */
    int wstatus = 0;
    for (int w = 0; w < DRIVE_TIMEOUT_MS; w += 20) {
        pid_t r = waitpid(pid, &wstatus, WNOHANG);
        if (r == pid) break;
        if (r < 0) break;
        usleep(20000);
    }
    (void)waitpid(pid, &wstatus, WNOHANG);
    kill(pid, 9);
    (void)waitpid(pid, &wstatus, 0);

    (void)sys$dassgn(in_chan);
    (void)sys$dassgn(out_chan);

    printf("=== test_syssvc_mbx_dcldrv: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
