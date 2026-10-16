/*
 * test_syssvc_cancel_uring.c - $CANCEL cancels REAL in-flight asynchronous I/O
 * (vms-c8c), through the PUBLIC sys$ API.
 *
 * THE BUG THIS GUARDS. sys$cancel was a literal no-op that returned SS$_NORMAL
 * for any io_uring-queued request. QIO is genuinely asynchronous (a read on a
 * pipe/FIFO is parked in the kernel's io_uring), so a program that queued a read
 * and then called $CANCEL was told "cancelled" while the read stayed pending:
 * its IOSB never completed, its event flag never set, its AST never fired, and
 * the read would later consume bytes meant for someone else. A success that
 * shares no real state with what it claims -- a facade.
 *
 * WHAT RUNS. Channels to FIFOs (a host path is OVMX's own $ASSIGN spelling; the
 * descriptor is one that waits, so $QIO takes the io_uring path). Each scenario
 * uses its OWN FIFO so a request left pending by a broken $CANCEL cannot steal
 * another scenario's bytes.
 *   1. a read with nothing to read is PENDING (IOSB 0, event flag clear, AST not
 *      run); $CANCEL completes it: IOSB = SS$_CANCEL, event flag set, AST run
 *      once with its parameter.
 *   2. a read whose data had already arrived is NOT reported cancelled: $CANCEL
 *      settles it with the result it finished with (SS$_NORMAL, the byte count).
 *   3. $CANCEL is per channel: cancelling channel A completes A's read and leaves
 *      B's pending; cancelling B then completes B's.
 *   4. $CANCEL on a channel with nothing pending is SS$_NORMAL.
 *   5. $CANCEL on a channel the process does not hold is refused.
 * Needs the real executive like every test_syssvc_*: with none it exits
 * EXIT_SKIP (the kernel-executive negative-control contract).
 *
 * NEGATIVE CONTROL: tests/qemu/defects.d/cancel-uring-noop.defect puts back the
 * no-op (the io_uring half of sys$cancel is not called).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "iodef.h"
#include "iosbdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

typedef struct { uint16_t status; uint16_t bcnt; uint32_t dev; } iosb_t;

static volatile int ast_runs;
static volatile uint32_t ast_prm;
static void ast_cb(uint32_t prm) { ast_runs++; ast_prm = prm; }

struct pipechan {
    char     path[64];
    uint16_t chan;
    int      wfd;      /* the test's own descriptor on the FIFO (writes bytes) */
};

static int open_pipechan(struct pipechan *p, const char *tag)
{
    snprintf(p->path, sizeof p->path, "/tmp/ovmx_cancel_%s_%d", tag, (int)getpid());
    unlink(p->path);
    if (mkfifo(p->path, 0600) != 0)
        return -1;
    p->wfd = open(p->path, O_RDWR);          /* keeps the FIFO from ever EOF-ing */
    if (p->wfd < 0)
        return -1;
    struct dsc$descriptor_s d;
    d.dsc$w_length = (uint16_t)strlen(p->path);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = p->path;
    p->chan = 0;
    uint32_t st = sys$assign(&d, &p->chan, 0, NULL);
    return ((st & 1) && p->chan) ? 0 : -1;
}

static void close_pipechan(struct pipechan *p)
{
    if (p->chan) sys$dassgn(p->chan);
    if (p->wfd >= 0) close(p->wfd);
    unlink(p->path);
}

static uint32_t queue_read(uint16_t chan, iosb_t *iosb, uint32_t efn,
                           void (*ast)(uint32_t), uint32_t prm, char *buf, uint32_t len)
{
    memset(iosb, 0, sizeof *iosb);
    return sys$qio(efn, chan, IO$_READVBLK, iosb, ast, prm, buf, len, 0, 0, 0, 0);
}

static int ef_is_set(uint32_t efn)
{
    uint32_t state = 0;
    return sys$readef(efn, &state) == SS$_WASSET;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_cancel_uring ($CANCEL of real in-flight QIO) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0 || !(ovmx_test_self(NULL) & 1)) {
        printf("=== test_syssvc_cancel_uring: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }

    struct pipechan a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.wfd = b.wfd = -1;
    if (open_pipechan(&a, "a") != 0 || open_pipechan(&b, "b") != 0) {
        printf("  FAIL: could not assign channels to two FIFOs (errno=%d)\n", errno);
        return 1;
    }

    char buf[64];
    iosb_t iosb, iosb_b;

    /* --- 1. a pending read is cancelled for real --- */
    uint32_t qst = queue_read(a.chan, &iosb, 3, ast_cb, 0x1234, buf, sizeof buf);
    CHECK(qst == SS$_NORMAL, "$QIO read on an empty FIFO is queued (SS$_NORMAL)");
    CHECK(iosb.status == 0 && !ef_is_set(3) && ast_runs == 0,
          "the queued read is PENDING: IOSB 0, event flag clear, AST not run");
    uint32_t cst = sys$cancel(a.chan);
    CHECK(cst == SS$_NORMAL, "$CANCEL on the channel returns SS$_NORMAL");
    /* negctl: cancel-uring-noop */
    CHECK(iosb.status == SS$_CANCEL && iosb.bcnt == 0,
          "$CANCEL completed the pending read: IOSB = SS$_CANCEL, 0 bytes");
    /* negctl: cancel-uring-noop */
    CHECK(ef_is_set(3), "$CANCEL set the cancelled read's event flag");
    /* negctl: cancel-uring-noop */
    CHECK(ast_runs == 1 && ast_prm == 0x1234,
          "$CANCEL ran the cancelled read's AST once with its parameter");

    /* --- 2. a read that had already completed is not reported cancelled --- */
    {
        struct pipechan c;
        memset(&c, 0, sizeof c); c.wfd = -1;
        if (open_pipechan(&c, "c") != 0) {
            printf("  FAIL: could not assign a channel to the third FIFO\n");
            return 1;
        }
        iosb_t i3;
        CHECK(write(c.wfd, "abc", 3) == 3, "three bytes are written ahead of the read");
        (void)queue_read(c.chan, &i3, 4, NULL, 0, buf, sizeof buf);
        (void)sys$cancel(c.chan);
        /* negctl: cancel-uring-noop */
        CHECK(i3.status == SS$_NORMAL && i3.bcnt == 3,
              "a read whose data had arrived keeps its result under $CANCEL (SS$_NORMAL, 3 bytes), not SS$_CANCEL");
        close_pipechan(&c);
    }

    /* --- 3. $CANCEL is per channel --- */
    ast_runs = 0;
    (void)queue_read(a.chan, &iosb, 5, ast_cb, 1, buf, sizeof buf);
    (void)queue_read(b.chan, &iosb_b, 6, ast_cb, 2, buf, sizeof buf);
    (void)sys$cancel(a.chan);
    /* negctl: cancel-uring-noop */
    CHECK(iosb.status == SS$_CANCEL, "cancelling channel A completed A's read with SS$_CANCEL");
    CHECK(iosb_b.status == 0 && !ef_is_set(6),
          "... and left channel B's read pending (IOSB 0, event flag clear)");
    (void)sys$cancel(b.chan);
    /* negctl: cancel-uring-noop */
    CHECK(iosb_b.status == SS$_CANCEL && ef_is_set(6),
          "cancelling channel B then completed B's read with SS$_CANCEL and set its event flag");

    /* --- 4. nothing pending --- */
    CHECK(sys$cancel(a.chan) == SS$_NORMAL, "$CANCEL with nothing pending is SS$_NORMAL");

    /* --- 5. a channel not held --- */
    CHECK(!(sys$cancel(0x7FF0) & 1), "$CANCEL on a channel the process does not hold is refused");

    close_pipechan(&a);
    close_pipechan(&b);

    printf("=== test_syssvc_cancel_uring: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
