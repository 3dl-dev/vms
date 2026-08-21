/*
 * test_syssvc_hiber_ast.c - $HIBER is INTERRUPTED by asynchronous AST delivery,
 * cross-process, through the PUBLIC sys$ API against a real /dev/vms (vms-feb).
 *
 * ============================================================
 * THE POINT OF vms-feb. Before this, a queued AST was only ever drained on an
 * explicit sys$setast(1), and sys$hiber was a bare pause(): a process that armed
 * a write-attention AST on a mailbox and then $HIBERed waiting for it would
 * NEVER be interrupted to run the AST when another process wrote the mailbox --
 * a deadlock. That is exactly the $HIBER/$WAKE + write-attention-AST pattern
 * MMK's send_cmd_and_wait uses to learn a spawned DCL command finished (spine
 * #4, vms-b23). vms-feb makes the wait, the wake state and the AST-arrival
 * notification the executive's, so process B's write actually interrupts
 * process A's $HIBER.
 *
 * WHAT THIS SUITE PROVES, AND WHY A PER-PROCESS FAKE CANNOT PASS IT. Process A
 * (a child forked by the coordinator) creates a named mailbox, arms a
 * write-attention AST whose routine issues sys$wake(SELF), and then calls
 * sys$hiber() -- with NO sys$setast(1) drain anywhere. An UNRELATED re-exec'd
 * process B assigns the mailbox BY NAME and writes it. For A's sys$hiber() to
 * return, the executive must, entirely on its own:
 *   1. land B's write-attention AST in A's executive AST queue (cross-process),
 *   2. WAKE A out of its $HIBER because that AST became deliverable,
 *   3. let A run the AST (which issues $WAKE), and
 *   4. end A's $HIBER on that $WAKE.
 * Nothing crosses between A and B but the mailbox name; a per-process AST queue
 * in A's own heap could never be fired by a write B performed, and a bare
 * pause() could never be woken by an AST at all. A reports the outcome to the
 * coordinator over a pipe; the coordinator BOUNDS its wait, so a genuine
 * deadlock (async delivery broken) is a named FAIL line here, not a harness-
 * wide QEMU timeout.
 *
 * A SECOND SCENARIO proves the sticky-wake ordering VMS specifies (VSI System
 * Services Reference, $WAKE/$HIBER): a $WAKE issued to A BEFORE A hibernates
 * makes A's next $HIBER fall straight through, so a $WAKE is never lost to a
 * race with the $HIBER it precedes.
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is loaded,
 * exactly as test_syssvc_mbx_wrtattn.c does): $CREMBX must fail SS$_NOSUCHDEV
 * and the whole cross-process scenario is skipped (EXIT_SKIP 77), never
 * fabricated (CLAUDE.md Rule 9 / INV-6).
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * hiber-ast-not-delivered removes vms_ast_notify_arrival()'s wake broadcast, so
 * the write-attention AST still lands in A's queue but A's $HIBER is never woken
 * to run it -- A deadlocks, the coordinator's bounded wait fails, and this
 * suite's "$HIBER returned" / "AST ran" assertions redden while
 * test_syssvc_mbx_wrtattn (which drains with an explicit $SETAST, never hibers)
 * stays green.
 *
 * SYNCHRONISATION: pipes only, no sleeps; every cross-process wait is bounded.
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
/* env-tunable so the negctl full-suite run can bound a broken peer's wait
 * (tests/qemu/inject_and_run.sh sets OVMX_KE_PEER_TIMEOUT_MS); default 20000
 * unchanged. Only a peer that FAILS to deliver ever reaches this bound -- a
 * pristine read returns on its poll the instant the peer writes -- so
 * shortening it bounds a mutation-broken run without touching legit timing. */
static int ke_peer_timeout_ms(void){const char*e=getenv("OVMX_KE_PEER_TIMEOUT_MS");int v=(e&&*e)?atoi(e):0;return v>0?v:20000;}
#define PEER_TIMEOUT_MS ke_peer_timeout_ms()

#define MBX_LOGNAME "OVMX$HIBER_AST_RENDEZVOUS"
static const char MSG[] = "MMK____status=1";
#define ASTPRM  0x00FEB001u

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Written from the AST routine the executive hands back, read by A before it
 * reports to the coordinator. volatile: it crosses the AST dispatch boundary. */
static volatile uint32_t g_fired = 0;
static volatile uint32_t g_param = 0;

/* The armed write-attention AST: record the delivery AND end A's own $HIBER by
 * a self-directed $WAKE -- the MMK send_cmd_and_wait shape. */
static void wake_ast(uint32_t prm) {
    g_fired++;
    g_param = prm;
    (void)sys$wake(NULL, NULL);   /* self-wake -> A's sys$hiber() returns */
}

static int read_bounded(int fd, void *buf, size_t len, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t got = 0;
    while (got < len) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0) return 0;      /* timed out */
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
 * arms any AST, never drains anything) and writes one message when asked. It
 * shares nothing with A but the name.
 *   p2c ('W' = write now, 'Q' = quit) from coordinator
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
            if (send_token(c2p_write, 'd') != 0) break;
        }
    }

    if (chan_b) (void)sys$dassgn(chan_b);
    return 0;
}

/*
 * process_a - the HIBERNATOR. Creates the mailbox, arms the write-attention AST,
 * tells the coordinator it is armed, then sys$hiber()s -- and NEVER calls
 * sys$setast(1). When it returns from $HIBER it reports g_fired to the
 * coordinator. If async delivery is broken it deadlocks in sys$hiber() and the
 * coordinator's bounded wait catches it.
 *   a2c: 'A' (armed) then a final uint32 g_fired after $HIBER returns
 *   c2a: 'G' (the coordinator has released B to write) -- unused for ordering
 *        beyond keeping A alive; A hibers regardless.
 */
static int process_a(int c2a_read, int a2c_write)
{
    (void)c2a_read;
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL))
        return 1;

    uint16_t chan_a = 0;
    struct dsc$descriptor_s lognam = mkdsc(MBX_LOGNAME);
    uint32_t cst = sys$crembx(0 /* temporary */, &chan_a, 0, 0, 0, 0, &lognam);
    if (!(cst & 1)) return 1;

    struct _iosb iosb = {0};
    uint32_t ast_st = sys$qiow(0, chan_a, IO$_SETMODE | IO$M_WRTATTN, &iosb,
                               NULL, 0, (void *)wake_ast, ASTPRM, 0, 0, 0, 0);
    if (!(ast_st & 1)) return 1;

    /* Tell the coordinator the AST is armed; it will only then let B write, so
     * B's write (and the AST it queues) lands while A is in -- or entering --
     * $HIBER. */
    if (send_token(a2c_write, 'A') != 0) return 1;

    /* THE PROOF: block here. Only real async AST delivery ends this. */
    (void)sys$hiber();

    /* Reached ONLY if $HIBER was interrupted by the delivered AST (which
     * self-woke). Report the delivery back to the coordinator. */
    uint32_t fired = g_fired;
    (void)!write(a2c_write, &fired, sizeof(fired));
    uint32_t param = g_param;
    (void)!write(a2c_write, &param, sizeof(param));

    if (chan_a) (void)sys$dassgn(chan_a);
    return 0;
}

/* Scenario 2 subject: a $WAKE that PRECEDES $HIBER must make $HIBER fall
 * straight through (sticky wake). Runs in a child so a hang is bounded. */
static int process_stickywake(int done_write)
{
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL))
        return 1;
    /* Wake ourselves BEFORE hibernating. */
    (void)sys$wake(NULL, NULL);
    /* Must not block: the sticky wake bit satisfies this $HIBER immediately. */
    (void)sys$hiber();
    char ok = 'K';
    (void)!write(done_write, &ok, 1);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    /* Re-exec'd child mode: process B (the writer). */
    if (argc >= 4 && strcmp(argv[1], "--writer") == 0)
        return process_b(atoi(argv[2]), atoi(argv[3]));

    printf("=== test_syssvc_hiber_ast ($HIBER interrupted by async AST delivery, cross-process, vms-feb) ===\n");

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
        printf("=== test_syssvc_hiber_ast: %d passed, %d failed (SKIPPED: no /dev/vms -- cross-process scenario not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* ---- SCENARIO 1: write-attention AST interrupts A's $HIBER ---- */
    int c2a[2], a2c[2], p2c[2], c2p[2];
    if (pipe(c2a) < 0 || pipe(a2c) < 0 || pipe(p2c) < 0 || pipe(c2p) < 0) {
        printf("  FAIL: pipe() failed\n"); return 1;
    }

    pid_t pa = fork();
    if (pa < 0) { printf("  FAIL: fork A failed\n"); return 1; }
    if (pa == 0) {
        close(c2a[1]); close(a2c[0]);
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        _exit(process_a(c2a[0], a2c[1]));
    }

    pid_t pb = fork();
    if (pb < 0) { printf("  FAIL: fork B failed\n"); return 1; }
    if (pb == 0) {
        close(p2c[1]); close(c2p[0]);
        close(c2a[0]); close(c2a[1]); close(a2c[0]); close(a2c[1]);
        char rfd[16], wfd[16];
        snprintf(rfd, sizeof(rfd), "%d", p2c[0]);
        snprintf(wfd, sizeof(wfd), "%d", c2p[1]);
        execl(argv[0], argv[0], "--writer", rfd, wfd, (char *)NULL);
        _exit(1);
    }
    close(c2a[0]); close(a2c[1]); close(p2c[0]); close(c2p[1]);

    /* Declared before any `goto teardown1` so teardown can tell whether A ever
     * reported (got == 1) and kill it if it deadlocked in $HIBER. */
    int got = 0;

    /* B reports its $ASSIGN status. */
    uint32_t b_assign = 0;
    if (read_bounded(c2p[0], &b_assign, sizeof(b_assign), PEER_TIMEOUT_MS) != 1) {
        printf("  FAIL: writer never reported its $ASSIGN status\n"); fail++; goto teardown1;
    }
    CHECK(b_assign & 1, "B: $ASSIGN A's mailbox by LOGICAL NAME cross-process");
    if (!(b_assign & 1)) goto teardown1;

    /* Wait for A to arm its write-attention AST and enter $HIBER. */
    char armed = 0;
    if (read_bounded(a2c[0], &armed, 1, PEER_TIMEOUT_MS) != 1 || armed != 'A') {
        printf("  FAIL: A never reported arming its write-attention AST\n"); fail++; goto teardown1;
    }
    CHECK(1, "A: armed write-attention AST and entered $HIBER (no explicit $SETAST drain)");

    /* Release B to write. Its write queues the AST into A's executive queue --
     * A must be woken out of $HIBER to run it. */
    char ack = 0;
    if (send_token(p2c[1], 'W') != 0 ||
        read_bounded(c2p[0], &ack, 1, PEER_TIMEOUT_MS) != 1) {
        printf("  FAIL: writer did not ack its write\n"); fail++; goto teardown1;
    }

    /* THE ASSERTION: A returns from $HIBER and reports g_fired. A bounded wait,
     * so a deadlock (async delivery broken) is a FAIL here, not a QEMU-wide
     * hang. negctl: hiber-ast-not-delivered */
    uint32_t a_fired = 0xFFFFFFFF;
    got = read_bounded(a2c[0], &a_fired, sizeof(a_fired), PEER_TIMEOUT_MS);
    CHECK(got == 1 && a_fired == 1,
          "A's $HIBER was INTERRUPTED by the cross-process write-attention AST: A ran the AST and returned from $HIBER");
    if (got == 1) {
        uint32_t a_param = 0;
        if (read_bounded(a2c[0], &a_param, sizeof(a_param), PEER_TIMEOUT_MS) == 1) {
            /* negctl: hiber-ast-not-delivered */
            CHECK(a_param == ASTPRM,
                  "the AST that woke A ran with the exact parameter A armed (astprm round-tripped through the executive)");
        } else {
            printf("  FAIL: A did not report the AST parameter\n"); fail++;
        }
    }

teardown1:
    (void)send_token(p2c[1], 'Q');
    /* If A never reported (got != 1) it is either dead or deadlocked in
     * $HIBER -- kill it so teardown's waitpid cannot hang. */
    if (got != 1) kill(pa, SIGKILL);
    { int ws = 0; waitpid(pa, &ws, 0); waitpid(pb, &ws, 0); }
    close(c2a[1]); close(a2c[0]); close(p2c[1]); close(c2p[0]);

    /* ---- SCENARIO 2: a $WAKE preceding $HIBER falls straight through ---- */
    int done[2];
    if (pipe(done) < 0) { printf("  FAIL: pipe() failed\n"); goto out; }
    pid_t ps = fork();
    if (ps < 0) { printf("  FAIL: fork stickywake failed\n"); goto out; }
    if (ps == 0) { close(done[0]); _exit(process_stickywake(done[1])); }
    close(done[1]);
    char sk = 0;
    int skgot = read_bounded(done[0], &sk, 1, PEER_TIMEOUT_MS);
    CHECK(skgot == 1 && sk == 'K',
          "sticky wake: a $WAKE issued BEFORE $HIBER makes $HIBER return immediately (not lost to the race)");
    if (skgot != 1) kill(ps, SIGKILL);
    { int ws = 0; waitpid(ps, &ws, 0); }
    close(done[0]);

out:
    printf("=== test_syssvc_hiber_ast: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
