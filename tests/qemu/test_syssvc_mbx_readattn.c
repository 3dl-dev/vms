/*
 * test_syssvc_mbx_readattn - mailbox READ-ATTENTION ASTs (rd vms-b24).
 *
 * VSI OpenVMS I/O User's Reference, Mailbox Driver: IO$_SETMODE|IO$M_READATTN
 * arms a one-shot AST that is delivered when a read request is queued to the
 * EMPTY mailbox, or at once if a read is already waiting when it is armed. It
 * is re-armed by another SETMODE. P1 is the AST routine, P2 its parameter.
 *
 * Process A creates the mailbox and arms; process B (re-exec'd, unrelated,
 * assigns by logical name) is the reader. Every wait is bounded.
 *
 *   1. arming alone fires nothing;
 *   2. a read that finds a message fires nothing; an IO$M_NOW read on the empty
 *      mailbox (it is not queued) fires nothing; a write fires nothing;
 *   3. B's blocking read on the empty mailbox fires A's AST once, with A's P2;
 *   4. one-shot: B's next read on the empty mailbox, not re-armed, fires nothing;
 *   5. armed while B's read is already waiting, it fires at once;
 *   6. in-process feeder: A arms, then A's own $QIOW read on the empty mailbox
 *      fires A's AST DURING that read; the AST routine writes a record and the
 *      read completes with it (the AST-driven data feeder of rd vms-07ae).
 *      A SIGALRM guard writes "TIMEOUT" instead if the read is never fed.
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
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77
#define PEER_TIMEOUT_MS 20000
#define FIRE_TIMEOUT_MS 10000
#define QUIET_MS 1500

#define MBX_LOGNAME "OVMX$READATTN_RENDEZVOUS"
#define MAGIC1 0x00B24001u
#define MAGIC2 0x00B24002u
#define MAGIC3 0x00B24003u

static const char FEED[] = "FEED-FROM-AST";
static const char TIMEOUT_MSG[] = "TIMEOUT";

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static volatile uint32_t g_fired = 0;
static volatile uint32_t g_param = 0;
static volatile uint32_t g_feed = 0;
static volatile int g_in_read = 0;
static volatile int g_feed_in_read = 0;
static uint16_t g_chan_a = 0;

static void record_ast(uint32_t prm) { g_fired++; g_param = prm; }

/* The feeder: runs while A's own read is outstanding and gives it a record. */
static void feed_ast(uint32_t prm)
{
    struct _iosb iosb = {0};
    g_feed++;
    g_param = prm;
    g_feed_in_read = g_in_read;
    (void)sys$qiow(0, g_chan_a, IO$_WRITEVBLK | IO$M_NORSWAIT, &iosb, NULL, 0,
                   (void *)FEED, (uint32_t)sizeof(FEED), 0, 0, 0, 0);
}

static void on_alarm(int sig)
{
    struct _iosb iosb = {0};
    (void)sig;
    (void)sys$qiow(0, g_chan_a, IO$_WRITEVBLK | IO$M_NORSWAIT, &iosb, NULL, 0,
                   (void *)TIMEOUT_MSG, (uint32_t)sizeof(TIMEOUT_MSG), 0, 0, 0, 0);
}

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

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* Drain A's AST queue until g_fired reaches `want` or the bound passes. */
static void await_fired(uint32_t want, int ms)
{
    for (int t = 0; t < ms; t += 20) {
        (void)sys$setast(1);
        if (g_fired >= want)
            return;
        usleep(20 * 1000);
    }
    (void)sys$setast(1);
}

/* B: the reader. 'R' = announce 'r', then one blocking $QIOW read; report 'd'. */
static int process_b(int p2c_read, int c2p_write)
{
    uint16_t chan_b = 0;
    uint32_t st;

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        st = SS$_BADPARAM;
        (void)!write(c2p_write, &st, sizeof(st));
        return 1;
    }
    struct dsc$descriptor_s namdsc = mkdsc(MBX_LOGNAME);
    st = sys$assign(&namdsc, &chan_b, 0, NULL);
    if (write(c2p_write, &st, sizeof(st)) != (ssize_t)sizeof(st) || !(st & 1))
        return 1;

    for (;;) {
        char cmd, buf[64];
        if (read(p2c_read, &cmd, 1) != 1 || cmd == 'Q')
            break;
        if (cmd == 'R') {
            struct _iosb iosb = {0};
            if (send_token(c2p_write, 'r') != 0) break;
            (void)sys$qiow(0, chan_b, IO$_READVBLK, &iosb, NULL, 0,
                           buf, sizeof(buf), 0, 0, 0, 0);
            if (send_token(c2p_write, 'd') != 0) break;
        }
    }
    if (chan_b) (void)sys$dassgn(chan_b);
    return 0;
}

static uint32_t a_write(const char *s)
{
    struct _iosb iosb = {0};
    uint32_t st = sys$qiow(0, g_chan_a, IO$_WRITEVBLK, &iosb, NULL, 0,
                           (void *)s, (uint32_t)strlen(s) + 1, 0, 0, 0, 0);
    return (st & 1) ? iosb.iosb$w_status : st;
}

static uint32_t arm(void (*rtn)(uint32_t), uint32_t prm)
{
    struct _iosb iosb = {0};
    return sys$qiow(0, g_chan_a, IO$_SETMODE | IO$M_READATTN, &iosb, NULL, 0,
                    (void *)rtn, prm, 0, 0, 0, 0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 4 && strcmp(argv[1], "--reader") == 0)
        return process_b(atoi(argv[2]), atoi(argv[3]));

    printf("=== test_syssvc_mbx_readattn (mailbox read-attention ASTs, rd vms-b24) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (!executive_present()) {
        uint16_t chan = 0;
        struct dsc$descriptor_s lognam = mkdsc(MBX_LOGNAME);
        uint32_t st = sys$crembx(0, &chan, 0, 0, 0, 0, &lognam);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: $CREMBX fails SS$_NOSUCHDEV, never a local per-process fallback");
        printf("=== test_syssvc_mbx_readattn: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    struct dsc$descriptor_s lognam = mkdsc(MBX_LOGNAME);
    uint32_t cst = sys$crembx(0, &g_chan_a, 0, 0, 0, 0, &lognam);
    CHECK(cst & 1, "A: $CREMBX (temporary) with a logical name succeeds");
    if (!(cst & 1))
        goto done;

    /* 1. arming alone fires nothing */
    CHECK(arm(record_ast, MAGIC1) & 1,
          "A: $QIO IO$_SETMODE|IO$M_READATTN is accepted on a mailbox channel");
    await_fired(1, QUIET_MS);
    CHECK(g_fired == 0, "arming read-attention with no reader fires nothing");

    /* 2. a read that finds a message, an IO$M_NOW read, a write: nothing */
    {
        char buf[64];
        struct _iosb iosb = {0};
        CHECK(a_write("ONE") & 1, "A: write a message");
        uint32_t st = sys$qiow(0, g_chan_a, IO$_READVBLK, &iosb, NULL, 0,
                               buf, sizeof(buf), 0, 0, 0, 0);
        CHECK((st & 1) && iosb.iosb$w_status == SS$_NORMAL,
              "A: the read that finds the message completes SS$_NORMAL");
        iosb = (struct _iosb){0};
        st = sys$qiow(0, g_chan_a, IO$_READVBLK | IO$M_NOW, &iosb, NULL, 0,
                      buf, sizeof(buf), 0, 0, 0, 0);
        CHECK(iosb.iosb$w_status == SS$_ENDOFFILE,
              "A: an IO$M_NOW read of the empty mailbox completes SS$_ENDOFFILE");
        await_fired(1, QUIET_MS);
        CHECK(g_fired == 0,
              "a read that finds a message, an IO$M_NOW read and a write fire no read-attention AST");
    }

    int p2c[2], c2p[2];
    if (pipe(p2c) < 0 || pipe(c2p) < 0) { printf("  FAIL: pipe() failed\n"); fail++; goto done; }
    pid_t pid = ovmx_fork_subprocess();
    if (pid < 0) { printf("  FAIL: fork() failed\n"); fail++; goto done; }
    if (pid == 0) {
        close(p2c[1]); close(c2p[0]);
        char rfd[16], wfd[16];
        snprintf(rfd, sizeof(rfd), "%d", p2c[0]);
        snprintf(wfd, sizeof(wfd), "%d", c2p[1]);
        execl(argv[0], argv[0], "--reader", rfd, wfd, (char *)NULL);
        _exit(1);
    }
    close(p2c[0]); close(c2p[1]);

    uint32_t b_assign = 0;
    if (read_bounded(c2p[0], &b_assign, sizeof(b_assign), PEER_TIMEOUT_MS) != 1 ||
        !(b_assign & 1)) {
        printf("  FAIL: reader did not assign the mailbox by name (%08x)\n", b_assign);
        fail++;
        goto teardown;
    }

    char tok;
    /* 3. B's read on the empty mailbox fires A's armed AST once */
    if (send_token(p2c[1], 'R') != 0 || read_bounded(c2p[0], &tok, 1, PEER_TIMEOUT_MS) != 1) {
        printf("  FAIL: reader did not start its read\n"); fail++; goto teardown;
    }
    await_fired(1, FIRE_TIMEOUT_MS);
    /* negctl: mbx-readattn-not-fired */
    CHECK(g_fired == 1,
          "an unrelated process's read queued to the EMPTY mailbox fires A's read-attention AST");
    /* negctl: mbx-readattn-not-fired */
    CHECK(g_param == MAGIC1, "the read-attention AST ran with the parameter A passed in P2");
    CHECK(a_write("REL1") & 1, "A: write releases B's read");
    if (read_bounded(c2p[0], &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'd') {
        printf("  FAIL: reader's read did not complete\n"); fail++; goto teardown;
    }

    /* Counts below are relative to step 3's, so each property is judged alone. */
    uint32_t f3 = g_fired;

    /* 4. one-shot: not re-armed, B's next empty read fires nothing */
    if (send_token(p2c[1], 'R') != 0 || read_bounded(c2p[0], &tok, 1, PEER_TIMEOUT_MS) != 1) {
        printf("  FAIL: reader did not start its second read\n"); fail++; goto teardown;
    }
    await_fired(f3 + 1, QUIET_MS);
    CHECK(g_fired == f3, "one-shot: a second empty read with no re-arm fires nothing");

    /* 5. B's read is still waiting: arming now fires at once */
    usleep(300 * 1000);
    CHECK(arm(record_ast, MAGIC2) & 1, "A: re-arm read-attention while B's read waits");
    await_fired(f3 + 1, FIRE_TIMEOUT_MS);
    CHECK(g_fired == f3 + 1,
          "read-attention armed while a read is ALREADY waiting is delivered at once");
    CHECK(g_param == MAGIC2, "the re-armed AST ran with the NEW parameter");
    CHECK(a_write("REL2") & 1, "A: write releases B's second read");
    if (read_bounded(c2p[0], &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'd') {
        printf("  FAIL: reader's second read did not complete\n"); fail++; goto teardown;
    }
    await_fired(f3 + 2, QUIET_MS);
    CHECK(g_fired == f3 + 1, "no further read-attention AST after the waiting read completes");

    /* 6. in-process feeder: A's own read on the empty mailbox is fed by the AST */
    {
        char buf[64];
        struct _iosb iosb = {0};
        memset(buf, 0, sizeof(buf));
        CHECK(arm(feed_ast, MAGIC3) & 1, "A: arm the feeder read-attention AST");
        signal(SIGALRM, on_alarm);
        alarm(20);
        g_in_read = 1;
        uint32_t st = sys$qiow(0, g_chan_a, IO$_READVBLK, &iosb, NULL, 0,
                               buf, sizeof(buf), 0, 0, 0, 0);
        g_in_read = 0;
        alarm(0);
        /* negctl: mbx-readattn-not-fired */
        CHECK(g_feed == 1 && g_feed_in_read,
              "A's own read on the empty mailbox fires A's read-attention AST, which runs while the read is outstanding");
        /* negctl: mbx-readattn-not-fired */
        CHECK((st & 1) && iosb.iosb$w_status == SS$_NORMAL &&
              iosb.iosb$w_bcnt == sizeof(FEED) && memcmp(buf, FEED, sizeof(FEED)) == 0,
              "the read completes normally with the record the AST wrote (the AST-driven feeder)");
        printf("  info: read status %%X%08X iosb %u count %u data '%.20s'\n",
               st, iosb.iosb$w_status, iosb.iosb$w_bcnt, buf);
    }

teardown:
    (void)send_token(p2c[1], 'Q');
    {
        int wst = 0;
        for (int t = 0; t < 200 && waitpid(pid, &wst, WNOHANG) == 0; t++)
            usleep(50 * 1000);
        if (waitpid(pid, &wst, WNOHANG) == 0) { kill(pid, SIGKILL); waitpid(pid, &wst, 0); }
    }
done:
    if (g_chan_a) (void)sys$dassgn(g_chan_a);
    printf("=== test_syssvc_mbx_readattn: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
