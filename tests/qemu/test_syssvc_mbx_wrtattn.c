/*
 * test_syssvc_mbx_wrtattn.c - mailbox WRITE-ATTENTION ASTs delivered
 * cross-process through the PUBLIC sys$ API, against a real /dev/vms
 * (vms-9003, the long-pole facility MMK's send_cmd_and_wait waits on).
 *
 * ============================================================
 * THE POINT OF vms-9003. On OpenVMS a process reading a mailbox arms a
 * WRITE-ATTENTION AST on its channel (IO$_SETMODE|IO$M_WRTATTN, P1 = AST
 * routine, P2 = parameter); when ANOTHER process writes a message to the
 * mailbox, the executive delivers that AST to the reader. It is the
 * notification MMK's subprocess manager uses to learn each DCL command
 * finished (tests/corpus/tier4-mx/common/sp_mgr.b32's SP_WRTATTN_AST, and
 * MMK's own build_target.c send_cmd_and_wait) -- the last executive facility
 * MMK needs before it can drive a real build (spine #4, vms-b23).
 *
 * WHY THIS SUITE CAN TELL THE EXECUTIVE FROM A PER-PROCESS FAKE. This is a
 * genuine A-arms / B-writes / A-receives cross-process proof: process A (this
 * parent) creates a named mailbox and arms the write-attention AST; an
 * UNRELATED re-exec'd process B assigns the mailbox BY NAME and writes to it;
 * and A's AST routine runs in A with the parameter A declared. Nothing crosses
 * between the two processes but the mailbox name and the executive that carries
 * the AST -- a per-process fake (the AST living in A's own heap) could NEVER be
 * fired by a write performed in a different process. The AST lands in A's
 * executive AST queue (src/kernel-core/vms_ast.c, the same 4-level queue
 * $DCLAST uses) and A drains it with sys$setast(1), exactly as a $DCLAST AST is
 * drained.
 *
 * ONE-SHOT, RE-ARM ON SETMODE (VSI OpenVMS I/O User's Reference, mailbox
 * driver). The suite proves all three: round 1 arms then fires; round 2
 * RE-ARMS then fires again (with a different parameter, so a stale delivery is
 * visible); round 3 does NOT re-arm and a write must NOT fire the AST -- the
 * defining one-shot property.
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is
 * loaded, exactly as test_syssvc_mbx_crossproc.c does): $CREMBX must fail
 * SS$_NOSUCHDEV and the whole cross-process scenario is skipped (EXIT_SKIP 77),
 * never fabricated (CLAUDE.md Rule 9 / INV-6).
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * the "AST fired" assertions are anchored by the mbx-wrtattn-not-fired defect,
 * which makes the executive's write path drop the write-attention firing. Under
 * it, A's AST never runs (g_fired stays 0), so the fire assertions redden while
 * A's own $CREMBX/$SETMODE/write assertions and the one-shot NON-fire assertion
 * stay green.
 *
 * SYNCHRONISATION: pipes only, no sleeps; the parent bounds each wait so a
 * failure is a named FAIL line, not a harness-wide QEMU timeout -- the same
 * discipline as test_syssvc_mbx_crossproc.c.
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
#define MBX_LOGNAME "OVMX$WRTATTN_RENDEZVOUS"
static const char MSG[] = "MMK____status=1";

/* AST parameter magics -- distinct so a stale/confused astprm is visible, and
 * so round 2's re-arm delivery is provably NOT round 1's leftover. */
#define MAGIC1  0x00A51001u
#define MAGIC2  0x00A51002u

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* The delivered-AST recorder. volatile: written from the AST routine the
 * executive hands back and read by the assertions. */
static volatile uint32_t g_fired = 0;
static volatile uint32_t g_param = 0;

static void record_ast(uint32_t prm) {
    g_fired++;
    g_param = prm;
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

/*
 * process_b - the WRITER. Assigns the named mailbox (never creates it, never
 * arms any AST) and writes one message each time the parent asks. It shares
 * nothing with A but the name. Protocol on its two pipes:
 *   p2c ('W' = write now, 'Q' = quit) from parent
 *   c2p (uint32 assign status once, then a 'd' byte after each write)
 */
static int process_b(int p2c_read, int c2p_write)
{
    uint16_t chan_b = 0;
    uint32_t assign_status;

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        assign_status = SS$_BADPARAM;
        (void)!write(c2p_write, &assign_status, sizeof(assign_status));
        return 1;
    }

    struct dsc$descriptor_s namdsc = mkdsc(MBX_LOGNAME);
    assign_status = sys$assign(&namdsc, &chan_b, 0, NULL);
    if (write(c2p_write, &assign_status, sizeof(assign_status)) != (ssize_t)sizeof(assign_status))
        return 1;
    if (!(assign_status & 1))
        return 1;

    for (;;) {
        char cmd;
        if (read(p2c_read, &cmd, 1) != 1) break;
        if (cmd == 'Q') break;
        if (cmd == 'W') {
            struct _iosb iosb = {0};
            (void)sys$qiow(0, chan_b, IO$_WRITEVBLK, &iosb, NULL, 0,
                           (void *)MSG, (uint32_t)sizeof(MSG), 0, 0, 0, 0);
            /* Ack regardless: the parent bounds its own wait and asserts on the
             * AST it did or did not receive, not on B's write status here. */
            if (send_token(c2p_write, 'd') != 0) break;
        }
    }

    if (chan_b) (void)sys$dassgn(chan_b);
    return 0;
}

/* Ask B to write, wait for its ack, then drain A's executive AST queue. */
static int poke_write_and_drain(int p2c_write, int c2p_read)
{
    char ack;
    if (send_token(p2c_write, 'W') != 0) return -1;
    if (read_bounded(c2p_read, &ack, 1, PEER_TIMEOUT_MS) != 1) return -1;
    /* sys$setast(1) enables this (USER-mode) process's AST delivery and drains
     * the executive's queue, dispatching any write-attention AST B's write just
     * landed there. */
    (void)sys$setast(1);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    /* Re-exec'd child mode: process B (the writer). */
    if (argc >= 4 && strcmp(argv[1], "--writer") == 0)
        return process_b(atoi(argv[2]), atoi(argv[3]));

    printf("=== test_syssvc_mbx_wrtattn (mailbox write-attention ASTs, cross-process, vms-9003) ===\n");

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
        printf("=== test_syssvc_mbx_wrtattn: %d passed, %d failed (SKIPPED: no /dev/vms -- cross-process scenario not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* Process A: create the named mailbox and arm the first write-attention AST
     * BEFORE B is allowed to assign or write. */
    uint16_t chan_a = 0;
    struct dsc$descriptor_s lognam = mkdsc(MBX_LOGNAME);
    uint32_t cst = sys$crembx(0 /* temporary */, &chan_a, 0, 0, 0, 0, &lognam);
    CHECK(cst & 1, "A: $CREMBX (temporary) with a logical name succeeds");
    if (!(cst & 1)) {
        printf("=== test_syssvc_mbx_wrtattn: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    struct _iosb iosb = {0};
    uint32_t ast_st = sys$qiow(0, chan_a, IO$_SETMODE | IO$M_WRTATTN, &iosb,
                               NULL, 0, (void *)record_ast, MAGIC1, 0, 0, 0, 0);
    CHECK(ast_st & 1,
          "A: $QIO IO$_SETMODE|IO$M_WRTATTN arms a write-attention AST on the mailbox");

    int p2c[2], c2p[2];
    if (pipe(p2c) < 0 || pipe(c2p) < 0) { printf("  FAIL: pipe() failed\n"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { printf("  FAIL: fork() failed\n"); return 1; }
    if (pid == 0) {
        close(p2c[1]); close(c2p[0]);
        char rfd[16], wfd[16];
        snprintf(rfd, sizeof(rfd), "%d", p2c[0]);
        snprintf(wfd, sizeof(wfd), "%d", c2p[1]);
        execl(argv[0], argv[0], "--writer", rfd, wfd, (char *)NULL);
        _exit(1);
    }
    close(p2c[0]); close(c2p[1]);

    /* B assigns the mailbox by NAME (an unrelated process reaching it through
     * LNM$SYSTEM) and reports its assign status. */
    uint32_t b_assign = 0;
    if (read_bounded(c2p[0], &b_assign, sizeof(b_assign), PEER_TIMEOUT_MS) != 1) {
        printf("  FAIL: writer never reported its $ASSIGN status\n");
        fail++;
        goto teardown;
    }
    CHECK(b_assign & 1,
          "B: $ASSIGN A's mailbox by LOGICAL NAME cross-process (rendezvous through LNM$SYSTEM)");
    if (!(b_assign & 1))
        goto teardown;

    /* ROUND 1 -- armed, must fire. */
    g_fired = 0; g_param = 0;
    if (poke_write_and_drain(p2c[1], c2p[0]) != 0) {
        printf("  FAIL: round 1: writer did not ack its write\n"); fail++; goto teardown;
    }
    /* negctl: mbx-wrtattn-not-fired */
    CHECK(g_fired == 1,
          "A's write-attention AST FIRES when the UNRELATED process B writes the mailbox (cross-process delivery through the executive)");
    /* negctl: mbx-wrtattn-not-fired */
    CHECK(g_param == MAGIC1,
          "A's AST ran with the exact parameter A passed in P2 (astprm round-tripped through the executive)");

    /* ROUND 2 -- RE-ARM with a different parameter, must fire again. */
    iosb = (struct _iosb){0};
    uint32_t rearm = sys$qiow(0, chan_a, IO$_SETMODE | IO$M_WRTATTN, &iosb,
                              NULL, 0, (void *)record_ast, MAGIC2, 0, 0, 0, 0);
    CHECK(rearm & 1, "A: re-arm the write-attention AST (fresh IO$_SETMODE|IO$M_WRTATTN)");
    if (poke_write_and_drain(p2c[1], c2p[0]) != 0) {
        printf("  FAIL: round 2: writer did not ack its write\n"); fail++; goto teardown;
    }
    /* negctl: mbx-wrtattn-not-fired */
    CHECK(g_fired == 2,
          "the RE-ARMED write-attention AST fires on the next cross-process write");
    /* negctl: mbx-wrtattn-not-fired */
    CHECK(g_param == MAGIC2,
          "the re-armed AST ran with the NEW parameter, not round 1's stale value");

    /* ROUND 3 -- one-shot: NOT re-armed, a write must NOT fire it. */
    if (poke_write_and_drain(p2c[1], c2p[0]) != 0) {
        printf("  FAIL: round 3: writer did not ack its write\n"); fail++; goto teardown;
    }
    CHECK(g_fired == 2,
          "one-shot: a write with NO re-arm does NOT fire the AST again (VSI mailbox driver semantics)");

teardown:
    (void)send_token(p2c[1], 'Q');
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    if (chan_a) (void)sys$dassgn(chan_a);

    printf("=== test_syssvc_mbx_wrtattn: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
