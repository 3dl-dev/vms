/*
 * test_syssvc_mbx_astwait - ASTs are delivered while a process waits in a
 * mailbox read, and the read then completes normally (rd vms-b24).
 *
 * On VMS a process waiting in $QIOW runs its ASTs; the I/O stays outstanding.
 * Process A arms a write-attention AST on mailbox M2 and blocks in a $QIOW
 * read of the empty mailbox M1. Process B (unrelated, assigns both by logical
 * name) writes M2 -- landing the AST in A's executive queue while A waits --
 * then, after a pause, writes M1.
 *
 *   1. the AST runs DURING A's read (before the read returns);
 *   2. the read then completes normally with B's M1 message;
 *   3. with A's user-mode ASTs disabled ($SETAST(0)) the AST does NOT run
 *      during the read, and runs once delivery is enabled again.
 * Every wait is bounded: B always writes M1, so the read always completes.
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
#define PEER_TIMEOUT_MS 30000

#define M1_LOGNAME "OVMX$ASTWAIT_DATA"
#define M2_LOGNAME "OVMX$ASTWAIT_POKE"
#define MAGIC1 0x00B24A01u
#define MAGIC2 0x00B24A02u

static const char DATA[] = "DATA-FROM-B";
static const char POKE[] = "POKE";

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static volatile uint32_t g_ast = 0;
static volatile uint32_t g_param = 0;
static volatile int g_in_read = 0;
static volatile int g_ast_in_read = 0;

static void poke_ast(uint32_t prm)
{
    g_ast++;
    g_param = prm;
    g_ast_in_read = g_in_read;
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

/* B: 'G' = write M2 (the poke), pause, write M1 (the data); report 'd'. */
static int process_b(int p2c_read, int c2p_write)
{
    uint16_t m1 = 0, m2 = 0;
    uint32_t st;

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        st = SS$_BADPARAM;
        (void)!write(c2p_write, &st, sizeof(st));
        return 1;
    }
    struct dsc$descriptor_s d1 = mkdsc(M1_LOGNAME), d2 = mkdsc(M2_LOGNAME);
    st = sys$assign(&d1, &m1, 0, NULL);
    if (st & 1)
        st = sys$assign(&d2, &m2, 0, NULL);
    if (write(c2p_write, &st, sizeof(st)) != (ssize_t)sizeof(st) || !(st & 1))
        return 1;

    for (;;) {
        char cmd;
        if (read(p2c_read, &cmd, 1) != 1 || cmd == 'Q')
            break;
        if (cmd == 'G') {
            struct _iosb iosb = {0};
            (void)sys$qiow(0, m2, IO$_WRITEVBLK, &iosb, NULL, 0,
                           (void *)POKE, (uint32_t)sizeof(POKE), 0, 0, 0, 0);
            usleep(1500 * 1000);
            iosb = (struct _iosb){0};
            (void)sys$qiow(0, m1, IO$_WRITEVBLK, &iosb, NULL, 0,
                           (void *)DATA, (uint32_t)sizeof(DATA), 0, 0, 0, 0);
            if (send_token(c2p_write, 'd') != 0) break;
        }
    }
    if (m1) (void)sys$dassgn(m1);
    if (m2) (void)sys$dassgn(m2);
    return 0;
}

/* One round: arm the poke AST on M2, let B go, read M1. */
static uint32_t round_read(uint16_t m1, uint16_t m2, int p2c_w, int c2p_r,
                           uint32_t magic, char *buf, size_t bufsz,
                           struct _iosb *iosb, int *ok)
{
    struct _iosb siosb = {0};
    char tok;
    *ok = 0;
    if (!(sys$qiow(0, m2, IO$_SETMODE | IO$M_WRTATTN, &siosb, NULL, 0,
                   (void *)poke_ast, magic, 0, 0, 0, 0) & 1))
        return 0;
    if (send_token(p2c_w, 'G') != 0)
        return 0;
    memset(buf, 0, bufsz);
    *iosb = (struct _iosb){0};
    g_in_read = 1;
    uint32_t st = sys$qiow(0, m1, IO$_READVBLK, iosb, NULL, 0,
                           buf, (uint32_t)bufsz, 0, 0, 0, 0);
    g_in_read = 0;
    if (read_bounded(c2p_r, &tok, 1, PEER_TIMEOUT_MS) == 1 && tok == 'd')
        *ok = 1;
    /* B's poke message is not this test's data: drain it */
    {
        char junk[16];
        struct _iosb j = {0};
        (void)sys$qiow(0, m2, IO$_READVBLK | IO$M_NOW, &j, NULL, 0,
                       junk, sizeof(junk), 0, 0, 0, 0);
    }
    return st;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 4 && strcmp(argv[1], "--writer") == 0)
        return process_b(atoi(argv[2]), atoi(argv[3]));

    printf("=== test_syssvc_mbx_astwait (ASTs delivered during a mailbox read wait, rd vms-b24) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (!executive_present()) {
        uint16_t chan = 0;
        struct dsc$descriptor_s lognam = mkdsc(M1_LOGNAME);
        uint32_t st = sys$crembx(0, &chan, 0, 0, 0, 0, &lognam);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: $CREMBX fails SS$_NOSUCHDEV, never a local per-process fallback");
        printf("=== test_syssvc_mbx_astwait: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    uint16_t m1 = 0, m2 = 0;
    struct dsc$descriptor_s l1 = mkdsc(M1_LOGNAME), l2 = mkdsc(M2_LOGNAME);
    uint32_t c1 = sys$crembx(0, &m1, 0, 0, 0, 0, &l1);
    uint32_t c2 = sys$crembx(0, &m2, 0, 0, 0, 0, &l2);
    CHECK((c1 & 1) && (c2 & 1), "A: $CREMBX the data mailbox and the poke mailbox");
    if (!((c1 & 1) && (c2 & 1)))
        goto done;

    int p2c[2], c2p[2];
    if (pipe(p2c) < 0 || pipe(c2p) < 0) { printf("  FAIL: pipe() failed\n"); fail++; goto done; }
    pid_t pid = ovmx_fork_subprocess();
    if (pid < 0) { printf("  FAIL: fork() failed\n"); fail++; goto done; }
    if (pid == 0) {
        close(p2c[1]); close(c2p[0]);
        char rfd[16], wfd[16];
        snprintf(rfd, sizeof(rfd), "%d", p2c[0]);
        snprintf(wfd, sizeof(wfd), "%d", c2p[1]);
        execl(argv[0], argv[0], "--writer", rfd, wfd, (char *)NULL);
        _exit(1);
    }
    close(p2c[0]); close(c2p[1]);

    uint32_t b_assign = 0;
    if (read_bounded(c2p[0], &b_assign, sizeof(b_assign), PEER_TIMEOUT_MS) != 1 ||
        !(b_assign & 1)) {
        printf("  FAIL: writer did not assign both mailboxes by name (%08x)\n", b_assign);
        fail++;
        goto teardown;
    }

    char buf[64];
    struct _iosb iosb;
    int ok;

    /* 1+2. ASTs enabled: the AST runs during the wait; the read completes */
    g_ast = 0;
    uint32_t st = round_read(m1, m2, p2c[1], c2p[0], MAGIC1, buf, sizeof(buf), &iosb, &ok);
    CHECK(ok, "B poked M2 and wrote M1");
    /* negctl: mbx-read-ast-not-delivered */
    CHECK(g_ast == 1 && g_ast_in_read,
          "an AST queued to a process blocked in a mailbox read is delivered DURING the wait");
    CHECK(g_param == MAGIC1, "the AST ran with its own parameter");
    CHECK((st & 1) && iosb.iosb$w_status == SS$_NORMAL &&
          iosb.iosb$w_bcnt == sizeof(DATA) && memcmp(buf, DATA, sizeof(DATA)) == 0,
          "after the AST the read stays outstanding and completes normally with B's message");

    /* nothing of round 1 may be left queued for round 3 to count */
    (void)sys$setast(1);

    /* 3. ASTs disabled: held through the read, delivered on $SETAST(1) */
    (void)sys$setast(0);
    g_ast = 0;
    g_ast_in_read = 0;
    st = round_read(m1, m2, p2c[1], c2p[0], MAGIC2, buf, sizeof(buf), &iosb, &ok);
    uint32_t during = g_ast;
    CHECK((st & 1) && iosb.iosb$w_status == SS$_NORMAL &&
          memcmp(buf, DATA, sizeof(DATA)) == 0,
          "with ASTs disabled the read completes normally with B's message");
    CHECK(during == 0, "with ASTs disabled ($SETAST(0)) no AST runs during the read");
    (void)sys$setast(1);
    CHECK(g_ast == 1 && g_param == MAGIC2 && !g_ast_in_read,
          "the held AST runs once delivery is enabled again, after the read");

teardown:
    (void)send_token(p2c[1], 'Q');
    {
        int wst = 0;
        for (int t = 0; t < 200 && waitpid(pid, &wst, WNOHANG) == 0; t++)
            usleep(50 * 1000);
        if (waitpid(pid, &wst, WNOHANG) == 0) { kill(pid, SIGKILL); waitpid(pid, &wst, 0); }
    }
done:
    if (m1) (void)sys$dassgn(m1);
    if (m2) (void)sys$dassgn(m2);
    printf("=== test_syssvc_mbx_astwait: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
