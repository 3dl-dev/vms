/*
 * test_syssvc_mbx_crossproc.c - named mailboxes connect two unrelated
 * processes through the PUBLIC sys$ API: $CREMBX by name / $ASSIGN by name /
 * $QIO message round trip (vms-mb1).
 *
 * ============================================================
 * THIS IS THE POINT OF vms-mb1. On OpenVMS a mailbox created with a logical
 * name is THE canonical IPC rendezvous: one process $CREMBXes it with a name,
 * an UNRELATED process $ASSIGNs that NAME -- not a unit number it was told, not
 * an fd it was handed -- and the two exchange messages with $QIO. It works
 * because the mailbox is a device the EXECUTIVE owns and the name lives in a
 * system-wide table every process can translate.
 *
 * vms-d44 made the mailbox itself executive-resident and proved cross-process
 * rendezvous BY DEVICE NAME (test_kmod_mbx.c, where B learns "MBAn:" over a
 * pipe). That still leaves the by-NAME half -- the half $CREMBX's logical-name
 * argument exists for -- unproven end to end: an unrelated process that knows
 * only the NAME has no pipe to learn the unit over. This suite closes it. The
 * creator publishes its mailbox's logical name in the executive-resident
 * LNM$SYSTEM table (vms-d37); the reader, a SEPARATE process holding no
 * relationship to the creator but the name itself, does sys$assign(NAME) --
 * which now translates the name through LNM$SYSTEM to the "MBAn:" unit
 * (src/libvms/syssvc/sys_assign.c, vms-mb1) -- and reads the exact message the
 * creator wrote. Nothing crosses between the two processes except the name and
 * the executive that resolves it.
 * ============================================================
 *
 * PRIVILEGE, MEASURED AND STATED. This suite creates a TEMPORARY mailbox
 * (TMPMBX, a default privilege) and publishes its name in LNM$SYSTEM (SYSNAM,
 * which the kernel-executive rig's processes hold -- the same privilege
 * test_syssvc_lnm_crossproc.c's DEFINE/SYSTEM relies on). It does NOT create a
 * PERMANENT mailbox: PRMMBX is not held here, and the "$CREMBX/PERMANENT is
 * refused SS$_NOPRIV without PRMMBX" property is already proved by
 * test_kmod_mbx.c. A temporary named mailbox is a complete proof of the by-NAME
 * rendezvous: both processes hold a channel to it at once, so it lives for the
 * whole exchange (temporary-mailbox last-channel deletion is, again, proved by
 * test_kmod_mbx.c). Permanent-mailbox-by-name, mailbox attention ASTs and
 * multi-reader fan-out remain on vms-mb1.
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is
 * loaded, exactly as test_syssvc_lnm_crossproc.c does): $CREMBX must fail
 * SS$_NOSUCHDEV, never fabricate a private per-process mailbox (CLAUDE.md Rule
 * 9 / INV-6). A test_syssvc_* suite that returned 0 with no executive would be
 * claiming a pass it never earned; this one returns EXIT_SKIP (77).
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * the by-name cross-process assertions are anchored by the mbx-not-shared
 * defect. That defect makes the executive's mailbox $ASSIGN refuse any caller
 * whose pid is not the mailbox's CREATOR -- so B's $ASSIGN of A's mailbox is
 * refused, whether B reached it by unit (test_kmod_mbx) or, here, by name.
 * Every assertion A makes about its OWN mailbox stays green; only the reader's
 * cross-process reach reddens, which is exactly the property CLAUDE.md rule 11
 * exists to protect.
 *
 * SYNCHRONISATION: pipes only, no sleeps; the parent bounds each wait so a
 * failure is a named FAIL line, not a harness-wide QEMU timeout -- the same
 * discipline as test_syssvc_lnm_crossproc.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
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
#define PEER_TIMEOUT_MS 20000

/* The logical name the two processes rendezvous on. Distinctive so a re-run in
 * the same booted guest cannot collide with another suite's LNM$SYSTEM name. */
#define MBX_LOGNAME "OVMX$MB1_RENDEZVOUS"
static const char MSG[] = "HELLO from an unrelated process";

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What process A (the creator/writer) reports to B before B tries to reach the
 * mailbox by name. B never learns the unit number -- only that A succeeded. */
struct a_report {
    uint32_t create_status;   /* sys$crembx(temporary, LOG=MBX_LOGNAME) */
    uint32_t write_status;    /* sys$qiow WRITEVBLK "HELLO..." into it   */
    uint32_t write_bcnt;      /* IOSB byte count A wrote                 */
};

static int read_bounded(int fd, void *buf, size_t len, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t got = 0;
    while (got < len) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0) return 0;
        if (pr < 0) return -1;
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 1;
}

static int send_token(int fd, char tok) { return write(fd, &tok, 1) == 1 ? 0 : -1; }

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* Same skip-vs-run decision, and same reasoning, as test_syssvc_lnm_crossproc.c:
 * open only to decide, never register by hand (the sys$ services bind lazily). */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/*
 * process_a - the CREATOR/WRITER. Creates a temporary mailbox WITH a logical
 * name (published in LNM$SYSTEM by sys$crembx), writes one message into it,
 * reports success to B, then holds its channel open until B says it is done so
 * the temporary mailbox survives the whole exchange.
 */
static int process_a(int a2b_write, int b2a_read)
{
    struct a_report rep;
    uint16_t chan_a = 0;
    char go;

    memset(&rep, 0, sizeof(rep));

    /* This re-exec'd child is a plain gcc program, not an activated image, so
     * it has no per-process PCB until it makes one -- without it every sys$
     * channel call fails at its first line with SS$_BADPARAM (see
     * test_syssvc_qio_terminal.c). Full mask: the executive, not this
     * userspace PCB, is what actually gates $CREMBX privilege. */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        (void)!write(a2b_write, &rep, sizeof(rep));
        return 1;
    }

    struct dsc$descriptor_s lognam = mkdsc(MBX_LOGNAME);
    rep.create_status = sys$crembx(0 /* temporary */, &chan_a,
                                   0 /* maxmsg default */, 0 /* bufquo default */,
                                   0 /* promsk */, 0 /* acmode */, &lognam);

    if (rep.create_status & 1) {
        struct _iosb iosb = {0};
        rep.write_status = sys$qiow(0, chan_a, IO$_WRITEVBLK, &iosb, NULL, 0,
                                    (void *)MSG, (uint32_t)sizeof(MSG),
                                    0, 0, 0, 0);
        rep.write_bcnt = iosb.iosb$w_bcnt;
    }

    if (write(a2b_write, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        return 1;

    /* Keep the mailbox alive: do not give back A's channel until B is done. */
    if (read(b2a_read, &go, 1) != 1)
        return 1;

    if (chan_a) (void)sys$dassgn(chan_a);
    return 0;
}

static int run_reader(int a2b_read, int b2a_write)
{
    struct a_report rep;
    uint16_t chan_b = 0;
    char buf[128];

    /* Wait for A to have created the named mailbox and written into it. */
    if (read_bounded(a2b_read, &rep, sizeof(rep), PEER_TIMEOUT_MS) != 1) {
        printf("  FAIL: reader: never got the creator's report\n");
        fail++;
        goto done;
    }

    CHECK(rep.create_status & 1,
          "creator: $CREMBX (temporary) with a logical name succeeds");
    CHECK((rep.write_status & 1) && rep.write_bcnt == (uint16_t)sizeof(MSG),
          "creator: $QIO WRITEVBLK into its own mailbox succeeds");

    /*
     * THE POINT OF THE ITEM. This process never created the mailbox and holds
     * no channel, no fd, no pipe to it -- only the NAME. sys$assign(NAME) must
     * translate MBX_LOGNAME through LNM$SYSTEM to the "MBAn:" unit and bind a
     * channel to the SAME executive-resident mailbox A created. Under the
     * mbx-not-shared defect (a creator-pid check in the executive's mailbox
     * $ASSIGN) this is exactly the call that is refused.
     */
    struct dsc$descriptor_s namdsc = mkdsc(MBX_LOGNAME);
    uint32_t ast = sys$assign(&namdsc, &chan_b, 0, NULL);
    /* negctl-knockon: mbx-not-shared */
    CHECK(ast & 1,
          "reader: $ASSIGN by LOGICAL NAME reaches the creator's mailbox cross-process (sys$assign translates the name through LNM$SYSTEM to MBAn:)");

    /*
     * Always attempt the read (not gated on the assign) so that under the
     * mbx-not-shared defect -- where the assign above is refused and chan_b is
     * never a valid mailbox channel -- this assertion still goes red BY NAME:
     * $QIO on the (invalid) channel is refused SS$_IVCHAN, the exact message is
     * not delivered, and the FAIL line carries this defect's knock-on text.
     * In the non-defect case chan_b is A's mailbox and the read returns the
     * queued message immediately (A wrote it before signalling readiness).
     */
    struct _iosb iosb = {0};
    memset(buf, 0, sizeof(buf));
    uint32_t rst = sys$qiow(0, chan_b, IO$_READVBLK, &iosb, NULL, 0,
                            buf, (uint32_t)sizeof(buf), 0, 0, 0, 0);
    /* negctl-knockon: mbx-not-shared */
    CHECK((rst & 1) && iosb.iosb$w_bcnt == (uint16_t)sizeof(MSG) &&
          memcmp(buf, MSG, sizeof(MSG)) == 0,
          "reader: $QIO READVBLK returns the EXACT message the creator wrote (byte-exact, right length) -- named-mailbox message delivery through the executive");
    if (ast & 1) (void)sys$dassgn(chan_b);

    /*
     * Negative control WITH an executive: a NAME that was never defined is not
     * a mailbox and not a device, so $ASSIGN must refuse it -- proving the
     * success above came from a real translation, not from $ASSIGN accepting
     * any string. (It is refused, not fabricated.)
     */
    struct dsc$descriptor_s bogus = mkdsc("OVMX$MB1_NEVER_DEFINED");
    uint16_t junk = 0;
    uint32_t bst = sys$assign(&bogus, &junk, 0, NULL);
    CHECK(!(bst & 1),
          "reader: $ASSIGN of a never-defined name is refused, not fabricated into a channel");

done:
    /* Release A regardless, so it can give back its channel and exit. */
    (void)send_token(b2a_write, 'x');
    return fail > 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* A failure-path peer exit must not take this process down with a SIGPIPE
     * on the coordination pipe -- a named FAIL is the intended outcome. */
    signal(SIGPIPE, SIG_IGN);

    /* Re-exec'd child mode: process A (the creator/writer). */
    if (argc >= 4 && strcmp(argv[1], "--creator") == 0)
        return process_a(atoi(argv[2]), atoi(argv[3]));

    printf("=== test_syssvc_mbx_crossproc (named mailboxes connect two unrelated processes: $CREMBX/$ASSIGN by name, vms-mb1) ===\n");

    /* A per-process PCB is a prerequisite for every sys$ channel call in this
     * file (the channel table lives in it); a gcc test binary must make its
     * own -- see test_syssvc_qio_terminal.c. Needed on BOTH branches below:
     * the no-executive branch still calls sys$crembx. */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        /*
         * NO EXECUTIVE: $CREMBX must fail honestly (CLAUDE.md Rule 9 / INV-6),
         * never a private per-process mailbox. Run on the host before vms.ko.
         */
        uint16_t chan = 0;
        struct dsc$descriptor_s lognam = mkdsc(MBX_LOGNAME);
        uint32_t st = sys$crembx(0, &chan, 0, 0, 0, 0, &lognam);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: $CREMBX fails SS$_NOSUCHDEV, never a local per-process fallback");
        printf("=== test_syssvc_mbx_crossproc: %d passed, %d failed (SKIPPED: no /dev/vms -- cross-process scenario not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    int a2b[2], b2a[2];
    if (pipe(a2b) < 0 || pipe(b2a) < 0) { printf("  FAIL: pipe() failed\n"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { printf("  FAIL: fork() failed\n"); return 1; }
    if (pid == 0) {
        /* Child = process A. Re-exec so it is a genuinely separate image with
         * its own executive PCB, sharing nothing with B but the name. */
        close(a2b[0]); close(b2a[1]);
        char wfd[16], rfd[16];
        snprintf(wfd, sizeof(wfd), "%d", a2b[1]);
        snprintf(rfd, sizeof(rfd), "%d", b2a[0]);
        execl(argv[0], argv[0], "--creator", wfd, rfd, (char *)NULL);
        _exit(1);
    }
    close(a2b[1]); close(b2a[0]);

    /* Parent = process B, the reader-by-name. */
    int rc = run_reader(a2b[0], b2a[1]);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    printf("=== test_syssvc_mbx_crossproc: %d passed, %d failed ===\n", pass, fail);
    (void)rc;
    return fail > 0 ? 1 : 0;
}
