/*
 * test_syssvc_mbx_cmdresp.c - a parent streams COMMANDS to a spawned child
 * over one mailbox and reads the child's RESULTS back over a second mailbox,
 * through the PUBLIC sys$ API against a real /dev/vms (vms-e0b).
 *
 * ============================================================
 * THIS IS THE POINT OF vms-e0b. MMK (the self-host spine's make, vms-ec70)
 * drives the compiler/linker by keeping ONE persistent DCL subprocess open and
 * FEEDING it resolved command lines over a VMS mailbox, reading each command's
 * results back over a SECOND mailbox (sp_mgr.c's sp_open/sp_send/sp_receive:
 * $CREMBX an inbox and an outbox, $GETDVI their names, hand them to LIB$SPAWN as
 * the subprocess's SYS$INPUT/SYS$OUTPUT, then send_cmd_and_wait streams commands
 * in and reads $STATUS-terminated results out). The mailbox primitives that
 * makes possible -- $CREMBX, $ASSIGN by name, $QIO WRITEVBLK/READVBLK,
 * cross-process delivery, temporary-mailbox lifecycle -- are executive-resident
 * (kernel-core/vms_mbx.c) and were proved ONE-DIRECTIONAL and single-message by
 * test_kmod_mbx.c (vms-d44, by unit) and test_syssvc_mbx_crossproc.c (vms-mb1,
 * by name). This suite proves the piece those left open and the one MMK's drive
 * actually stands on: a BIDIRECTIONAL, MULTI-MESSAGE command/response loop
 * between a parent and a genuinely separate spawned child -- exactly
 * send_cmd_and_wait's shape, minus the write-attention AST (that is vms-9003;
 * here the parent simply blocks in $QIOW READVBLK for each result).
 * ============================================================
 *
 * WHAT THIS EXERCISES THAT THE PRIOR SUITES DID NOT -- THE BLOCKING WAKE PATH.
 * test_kmod_mbx.c and test_syssvc_mbx_crossproc.c both write their one message
 * BEFORE the reader reads, so the reader always finds it already queued and
 * never actually blocks. MMK's send_cmd_and_wait, by contrast, blocks waiting
 * for a result the subprocess has not produced yet. This suite runs a PING-PONG:
 * the parent writes command N, then blocks in READVBLK on the (empty) result
 * mailbox until the child produces result N; the child, symmetrically, blocks in
 * READVBLK on the (empty) command mailbox until the parent sends command N+1. In
 * steady state EVERY read blocks on an empty mailbox and is released only by the
 * other process's write -- the wake/wake-not-lost path (vms_mbx.c's read_wq / cv
 * contract). A single LOST WAKEUP would deadlock one side forever, so the whole
 * exchange COMPLETING, with byte-exact results, is itself the proof the wake is
 * never lost. The harness `timeout` bounds that into a red, not a hang.
 *
 * INDEPENDENT ORACLE (CLAUDE.md rule 11 / the veracity crux). The child does not
 * echo bytes back; it TRANSFORMS each command by a rule the parent knows and the
 * parent alone did not perform -- result = "OK:" prepended to the command. The
 * parent asserts the exact transformed bytes, so a green assertion can only come
 * from the child having actually received the command, computed on it, and
 * delivered the result through the real mailbox device -- not from any local
 * fabrication (there is nowhere local for a "OK:CMD 7" to come from).
 *
 * RENDEZVOUS. Nothing crosses between parent and child but the two mailbox
 * LOGICAL NAMES (compile-time constants) and the executive that resolves them.
 * The parent $CREMBXes both with logical names (published in the
 * executive-resident LNM$SYSTEM, vms-d37/vms-d44); the child -- a re-exec'd,
 * genuinely separate image holding no fd, no unit, no channel from the parent --
 * does sys$assign(NAME) on each, which translates through LNM$SYSTEM to the
 * "MBAn:" unit (src/libvms/syssvc/sys_assign.c, vms-mb1). This is the same
 * mechanism MMK relies on: the subprocess reaches its I/O mailboxes by the names
 * LIB$SPAWN wired to SYS$INPUT/SYS$OUTPUT, not by any handle inherited in memory.
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is loaded,
 * exactly as test_syssvc_mbx_crossproc.c does): $CREMBX must fail SS$_NOSUCHDEV,
 * never fabricate a private per-process mailbox (CLAUDE.md Rule 9 / INV-6). A
 * test_syssvc_* suite that returned 0 with no executive would be claiming a pass
 * it never earned; this one returns EXIT_SKIP (77) -- the contract ci.yml's
 * kernel-executive-negative-control job holds every test_syssvc_* suite to.
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * the cross-process assertions are anchored by the mbx-not-shared defect, which
 * makes the executive's mailbox $ASSIGN refuse any caller whose pid is not the
 * mailbox's CREATOR. Under it the CHILD -- which never created either mailbox --
 * is refused both $ASSIGNs, so it can neither receive commands nor deliver
 * results, and every cross-process assertion here reddens while the parent's
 * assertions about its OWN mailboxes ($CREMBX, its own writes) stay green:
 * precisely the A-writes/B-reads shape CLAUDE.md rule 11 exists to catch.
 *
 * SYNCHRONISATION: two coordination pipes, no sleeps. The parent signals "both
 * mailboxes are created and published" before the child assigns; the child
 * reports its assign result back so a child-side failure becomes a NAMED FAIL
 * here rather than a parent that blocks forever on a result no child will send.
 * Once the child has reported both channels assigned, the parent's blocking
 * result-reads are safe -- a live child is guaranteed to answer each command.
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

/* The two logical names the parent and child rendezvous on. Distinctive so a
 * re-run in the same booted guest cannot collide with another suite's
 * LNM$SYSTEM names. CMD carries parent->child commands; RSP carries
 * child->parent results. */
#define CMD_LOGNAME "OVMX$E0B_CMD"
#define RSP_LOGNAME "OVMX$E0B_RSP"

/* How many command/result round trips to stream. >1 is the point: it proves the
 * mailbox is a PERSISTENT stream (MMK sends many commands down one subprocess),
 * not a one-shot, and it re-arms the blocking wake path on both mailboxes every
 * iteration. */
#define NROUNDS 8

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* The child's transform rule, known to both sides so the parent has an
 * independent oracle for each result. result = "OK:" + command. */
static int expected_result(const char *cmd, char *out, size_t outsz)
{
    return snprintf(out, outsz, "OK:%s", cmd);
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

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* What the child reports to the parent after trying to assign both mailboxes by
 * name, BEFORE the command loop begins -- so a child-side assign failure is a
 * named FAIL, not a parent blocked forever on a result. */
struct child_report {
    uint32_t cmd_assign_status;
    uint32_t rsp_assign_status;
};

/*
 * run_child - the spawned "subprocess" (MMK's DCL role). Re-exec'd, so it is a
 * genuinely separate image with its own executive PCB, sharing nothing with the
 * parent but the two logical names. Waits for the parent's "mailboxes ready"
 * token, assigns both by name, reports the two assign statuses, then loops:
 * block for a command, transform it, write the result -- until the parent sends
 * the QUIT sentinel.
 */
static int run_child(int p2c_read, int c2p_write)
{
    struct child_report rep;
    uint16_t cmd_chan = 0, rsp_chan = 0;
    char go;

    memset(&rep, 0, sizeof(rep));

    /* A re-exec'd gcc child has no per-process PCB until it makes one -- without
     * it every sys$ channel call fails at its first line with SS$_BADPARAM (see
     * test_syssvc_qio_terminal.c). The executive, not this userspace PCB, is
     * what actually gates access. */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        (void)!write(c2p_write, &rep, sizeof(rep));
        return 1;
    }

    /* Wait until the parent has $CREMBXed and published both mailbox names. */
    if (read(p2c_read, &go, 1) != 1)
        return 1;

    struct dsc$descriptor_s cmddsc = mkdsc(CMD_LOGNAME);
    struct dsc$descriptor_s rspdsc = mkdsc(RSP_LOGNAME);
    rep.cmd_assign_status = sys$assign(&cmddsc, &cmd_chan, 0, NULL);
    rep.rsp_assign_status = sys$assign(&rspdsc, &rsp_chan, 0, NULL);

    if (write(c2p_write, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        return 1;

    /* If either assign failed (e.g. under the mbx-not-shared negative control),
     * do not enter the loop: the parent has our report and will emit the named
     * FAILs itself. */
    if (!(rep.cmd_assign_status & 1) || !(rep.rsp_assign_status & 1)) {
        if (cmd_chan) (void)sys$dassgn(cmd_chan);
        if (rsp_chan) (void)sys$dassgn(rsp_chan);
        return 1;
    }

    for (;;) {
        struct _iosb iosb = {0};
        char cmd[128], result[160];
        memset(cmd, 0, sizeof(cmd));

        /* Block until the parent sends the next command. In steady state this
         * mailbox is EMPTY when we arrive -- the parent is blocked waiting for
         * our previous result -- so this is the blocking wake path. */
        uint32_t rst = sys$qiow(0, cmd_chan, IO$_READVBLK, &iosb, NULL, 0,
                                cmd, (uint32_t)sizeof(cmd), 0, 0, 0, 0);
        if (!(rst & 1))
            break;
        uint32_t clen = iosb.iosb$w_bcnt;
        if (clen >= sizeof(cmd)) clen = sizeof(cmd) - 1;
        cmd[clen] = '\0';

        if (strcmp(cmd, "QUIT") == 0)
            break;

        int rlen = expected_result(cmd, result, sizeof(result));
        struct _iosb wiosb = {0};
        uint32_t wst = sys$qiow(0, rsp_chan, IO$_WRITEVBLK, &wiosb, NULL, 0,
                                result, (uint32_t)(rlen + 1), 0, 0, 0, 0);
        if (!(wst & 1))
            break;
    }

    (void)sys$dassgn(cmd_chan);
    (void)sys$dassgn(rsp_chan);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* A peer that exits on a failure path must not take this process down with
     * a SIGPIPE on a coordination pipe -- a named FAIL is the intended outcome. */
    signal(SIGPIPE, SIG_IGN);

    /* Re-exec'd child mode. */
    if (argc >= 4 && strcmp(argv[1], "--child") == 0)
        return run_child(atoi(argv[2]), atoi(argv[3]));

    printf("=== test_syssvc_mbx_cmdresp (parent streams commands to a spawned child and reads results back over two mailboxes, vms-e0b) ===\n");

    /* A per-process PCB is a prerequisite for every sys$ channel call in this
     * file (the channel table lives in it); a gcc test binary must make its own.
     * Needed on BOTH branches below: the no-executive branch still calls
     * sys$crembx. */
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
        struct dsc$descriptor_s lognam = mkdsc(CMD_LOGNAME);
        uint32_t st = sys$crembx(0, &chan, 0, 0, 0, 0, &lognam);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: $CREMBX fails SS$_NOSUCHDEV, never a local per-process fallback");
        printf("=== test_syssvc_mbx_cmdresp: %d passed, %d failed (SKIPPED: no /dev/vms -- command/response scenario not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* Create the two mailboxes the parent owns, each with a logical name the
     * child will translate. Temporary (TMPMBX, a default privilege); the parent
     * holds a channel to each for the whole exchange, so both survive it. */
    uint16_t cmd_chan = 0, rsp_chan = 0;
    struct dsc$descriptor_s cmddsc = mkdsc(CMD_LOGNAME);
    struct dsc$descriptor_s rspdsc = mkdsc(RSP_LOGNAME);
    uint32_t cs = sys$crembx(0, &cmd_chan, 1024, 1024, 0, 0, &cmddsc);
    uint32_t rs = sys$crembx(0, &rsp_chan, 1024, 1024, 0, 0, &rspdsc);
    CHECK(cs & 1, "parent: $CREMBX of the command mailbox (with a logical name) succeeds");
    CHECK(rs & 1, "parent: $CREMBX of the result mailbox (with a logical name) succeeds");
    if (!(cs & 1) || !(rs & 1)) {
        if (cmd_chan) (void)sys$dassgn(cmd_chan);
        if (rsp_chan) (void)sys$dassgn(rsp_chan);
        printf("=== test_syssvc_mbx_cmdresp: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    int p2c[2], c2p[2];
    if (pipe(p2c) < 0 || pipe(c2p) < 0) { printf("  FAIL: pipe() failed\n"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { printf("  FAIL: fork() failed\n"); return 1; }
    if (pid == 0) {
        /* Child = the spawned subprocess. Re-exec so it is a genuinely separate
         * image with its own executive PCB, sharing nothing with the parent but
         * the two logical names. */
        close(p2c[1]); close(c2p[0]);
        char rfd[16], wfd[16];
        snprintf(rfd, sizeof(rfd), "%d", p2c[0]);
        snprintf(wfd, sizeof(wfd), "%d", c2p[1]);
        execl(argv[0], argv[0], "--child", rfd, wfd, (char *)NULL);
        _exit(1);
    }
    close(p2c[0]); close(c2p[1]);

    /* Tell the child both mailboxes exist and are published: it may now assign
     * them by name. */
    (void)!write(p2c[1], "g", 1);

    /* Learn whether the child reached BOTH mailboxes by name before we commit to
     * blocking reads on results (a child that could not assign will never
     * answer, so we must not block on it). */
    struct child_report rep;
    memset(&rep, 0, sizeof(rep));
    int got = read_bounded(c2p[0], &rep, sizeof(rep), PEER_TIMEOUT_MS);
    if (got != 1) {
        printf("  FAIL: never got the child's assign report\n");
        fail++;
    } else {
        /* negctl-knockon: mbx-not-shared */
        CHECK(rep.cmd_assign_status & 1,
              "child: $ASSIGN of the command mailbox BY NAME reaches the parent's mailbox cross-process");
        /* negctl-knockon: mbx-not-shared */
        CHECK(rep.rsp_assign_status & 1,
              "child: $ASSIGN of the result mailbox BY NAME reaches the parent's mailbox cross-process");
    }

    int child_reachable = (got == 1) &&
                          (rep.cmd_assign_status & 1) && (rep.rsp_assign_status & 1);

    if (child_reachable) {
        /*
         * THE POINT OF THE ITEM. Stream NROUNDS commands down the command
         * mailbox and read each result back from the result mailbox. Each result
         * read blocks on an initially-empty mailbox until the child answers (the
         * wake path); a lost wakeup would deadlock and time out. Every result is
         * asserted byte-exact against the transform the child, not this process,
         * applied.
         */
        int all_ok = 1;
        for (int i = 1; i <= NROUNDS && all_ok; i++) {
            char cmd[128], want[160], buf[160];
            int clen = snprintf(cmd, sizeof(cmd), "MMK$CMD %d", i);

            struct _iosb wiosb = {0};
            uint32_t wst = sys$qiow(0, cmd_chan, IO$_WRITEVBLK, &wiosb, NULL, 0,
                                    cmd, (uint32_t)(clen + 1), 0, 0, 0, 0);
            if (!(wst & 1)) { all_ok = 0; break; }

            struct _iosb riosb = {0};
            memset(buf, 0, sizeof(buf));
            uint32_t rst = sys$qiow(0, rsp_chan, IO$_READVBLK, &riosb, NULL, 0,
                                    buf, (uint32_t)sizeof(buf), 0, 0, 0, 0);
            int wantlen = expected_result(cmd, want, sizeof(want));
            if (!((rst & 1) &&
                  riosb.iosb$w_bcnt == (uint16_t)(wantlen + 1) &&
                  memcmp(buf, want, (size_t)wantlen + 1) == 0))
                all_ok = 0;
        }
        /*
         * No negctl comment here on purpose: under the mbx-not-shared mutation
         * the child cannot assign either mailbox, so child_reachable is false and
         * THIS branch never runs -- the NOT-REACHED assertion in the else branch
         * below is the one that reddens and carries the anchor. This branch is
         * the healthy-executive proof.
         */
        CHECK(all_ok,
              "parent: all 8 streamed commands come back TRANSFORMED and byte-exact from the spawned child (bidirectional multi-message command/response through the executive; the blocking result-read is woken by the child's write every round)");

        /* Send the QUIT sentinel so the child leaves its loop cleanly. */
        struct _iosb qiosb = {0};
        (void)sys$qiow(0, cmd_chan, IO$_WRITEVBLK, &qiosb, NULL, 0,
                       (void *)"QUIT", (uint32_t)sizeof("QUIT"), 0, 0, 0, 0);
    } else {
        /* Child unreachable (e.g. the mbx-not-shared negative control): the
         * command/response loop cannot run at all, so the round-trip property
         * goes red BY NAME, and we tell the child (already exiting) nothing more. */
        /* negctl-knockon: mbx-not-shared */
        CHECK(0,
              "parent: all 8 streamed commands come back TRANSFORMED and byte-exact from the spawned child -- NOT REACHED because the child could not assign the mailboxes cross-process");
    }

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    (void)sys$dassgn(cmd_chan);
    (void)sys$dassgn(rsp_chan);

    printf("=== test_syssvc_mbx_cmdresp: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
