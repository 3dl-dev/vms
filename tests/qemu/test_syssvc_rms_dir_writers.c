/*
 * test_syssvc_rms_dir_writers.c - two processes writing DIFFERENT files in ONE
 * directory never stall each other, and every record of the log arrives
 * (rd vms-b6e).
 *
 * WHY. On the live VAX bench (vms-0a7 S3, tests/lab/captures/vms-0a7-s3-20261103
 * on work/vms-0a7-s3-netacp) NETACP appends its log to SYS$MANAGER:NETACP.LOG
 * through RMS while its $SETIMR fires every 5 ms. Once FAL (writing a received
 * file) or LOGINOUT (writing LASTLOGIN) wrote in the same directory, NETACP.LOG
 * got no more lines and every writer on the volume stalled: BIG COPY ended
 * LINKDISCON, the inbound login hung, once the console login too.
 *
 * THE CAUSE. Every ACP write takes the Files-11 volume-sync lock (F11B$s, EX)
 * in the executive. When NETACP's request had to wait for FAL's, NETACP's
 * timer signal interrupted the wait; the executive re-entered the $ENQ in a
 * loop -- each pass a NEW EX request, because a pending signal never lets the
 * wait sleep -- and the first of those orphans was granted when FAL finished,
 * to nobody. The volume lock was then held for ever.
 *
 * WHAT IS ASSERTED, on the real executive (/dev/vms) and the ODS-2 volume the
 * harness mounts on VDA0:, with no network at all:
 *   - LOGGER, a subprocess with a 1 ms $SETIMR re-armed from its own AST (the
 *     NETACP shape), holds LOG open and $PUTs LINES records;
 *   - WRITER, a second subprocess with the same timer (FAL's shape), creates,
 *     fills and erases a file in the same directory CYCLES times;
 *   - both finish within the deadline, every $PUT succeeds, and LOG then holds
 *     exactly LINES records, each the one written, in order.
 * Before the fix the two wedge on the volume lock and the deadline names it.
 *
 * NO /dev/vms -> honest SKIP (77): no ACP, nothing to write.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "starlet.h"
#include "ssdef.h"
#include "rms/rms.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP   77
#define LOG_SPEC    "VDA0:[OVMXDIR]B6ELOG.LOG"
#define BIG_SPEC    "VDA0:[OVMXDIR]B6EFAL.DAT"
#define LINES       600
#define CYCLES      30
#define BIG_RECS    60
#define DEADLINE_S  150
#define TIMER_ID    0xB6E

static int pass = 0;
static int fail = 0;

static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
    fflush(stdout);
}

/*
 * WHERE EACH CHILD IS, readable by the parent after a deadline (rd vms-2a84). A
 * child's stdout is buffered and lost when it is killed, so the progress lives
 * in a page the parent shares: which record/cycle it reached, how many timer
 * ASTs ran, and the first $SETIMR re-arm that failed.
 */
struct child_progress {
    volatile uint32_t step;          /* logger: $PUTs done; writer: cycles done */
    volatile uint32_t asts;          /* timer ASTs run */
    volatile uint32_t setimr_bad;    /* first failing re-arm status, 0 = none */
    volatile uint32_t setimr_bad_at; /* the AST count it failed at */
};
static struct child_progress *progress;   /* [0] logger, [1] writer */
static struct child_progress *me;         /* this child's slot */

/* ---- the timer: 1 ms, re-armed from its own AST, until told to stop ---- */
static volatile int timer_on;
static volatile uint32_t timer_fired;
static const uint64_t one_ms = (uint64_t)-10000LL;   /* 1 ms delta, 100 ns units */

static void timer_ast(uint32_t reqidt)
{
    (void)reqidt;
    timer_fired++;
    if (me)
        me->asts = timer_fired;
    if (timer_on) {
        uint32_t st = sys$setimr(0, &one_ms, timer_ast, TIMER_ID, 0);
        if (!(st & 1) && me && me->setimr_bad == 0) {
            me->setimr_bad = st;
            me->setimr_bad_at = timer_fired;
        }
    }
}

static int timer_start(void)
{
    timer_on = 1;
    return (sys$setimr(0, &one_ms, timer_ast, TIMER_ID, 0) & 1) ? 0 : -1;
}

static void timer_stop(void)
{
    timer_on = 0;
    (void)sys$cantim(TIMER_ID, 0);
}

static void erase_all(const char *spec)
{
    char s[96];
    struct FAB fab;
    int i;

    snprintf(s, sizeof(s), "%s;*", spec);
    for (i = 0; i < 4; i++) {
        fab = cc$rms_fab;
        fab.fab$l_fna = s;
        fab.fab$b_fns = (uint8_t)strlen(s);
        if (!(sys$erase(&fab, 0, 0) & 1))
            break;
    }
}

static void line_text(char *buf, size_t sz, int i)
{
    snprintf(buf, sz, "B6E-LOG line %04d: NETACP keeps writing while FAL writes", i);
}

/* LOGGER: hold LOG open (SHRGET, as NETACP does) and $PUT LINES records. */
static int run_logger(void)
{
    struct FAB fab = cc$rms_fab;
    struct RAB rab = cc$rms_rab;
    char line[96];
    int i, bad = 0;

    me = &progress[0];
    if (timer_start() != 0)
        return 10;
    fab.fab$l_fna = LOG_SPEC;
    fab.fab$b_fns = (uint8_t)strlen(LOG_SPEC);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_VAR;
    fab.fab$b_rat = FAB$M_CR;
    fab.fab$b_fac = FAB$M_PUT;
    fab.fab$b_shr = FAB$M_SHRGET;
    if (sys$create(&fab, 0, 0) != RMS$_NORMAL)
        return 11;
    rab.rab$l_fab = &fab;
    if (sys$connect(&rab, 0, 0) != RMS$_NORMAL) {
        sys$close(&fab, 0, 0);
        return 12;
    }
    for (i = 0; i < LINES; i++) {
        line_text(line, sizeof(line), i);
        rab.rab$l_rbf = line;
        rab.rab$w_rsz = (uint16_t)strlen(line);
        if (sys$put(&rab, 0, 0) != RMS$_NORMAL) {
            if (bad++ == 0)
                printf("  logger: $PUT #%d failed sts=%08X stv=%08X\n", i,
                       (unsigned)rab.rab$l_sts, (unsigned)rab.rab$l_stv);
            fflush(stdout);
        }
        me->step = (uint32_t)(i + 1);
    }
    timer_stop();
    if (!(sys$close(&fab, 0, 0) & 1))
        return 13;
    printf("  logger: %d $PUTs failed, %u timer ASTs ran\n", bad, (unsigned)timer_fired);
    fflush(stdout);
    return bad ? 14 : 0;
}

/* WRITER: create, fill and erase a file in the same directory, CYCLES times. */
static int run_writer(void)
{
    char rec[256];
    int c, r;

    memset(rec, 'F', sizeof(rec));
    me = &progress[1];
    if (timer_start() != 0)
        return 20;
    for (c = 0; c < CYCLES; c++) {
        struct FAB fab = cc$rms_fab;
        struct RAB rab = cc$rms_rab;

        fab.fab$l_fna = BIG_SPEC;
        fab.fab$b_fns = (uint8_t)strlen(BIG_SPEC);
        fab.fab$b_org = FAB$C_SEQ;
        fab.fab$b_rfm = FAB$C_VAR;
        fab.fab$b_fac = FAB$M_PUT;
        if (sys$create(&fab, 0, 0) != RMS$_NORMAL)
            return 21;
        rab.rab$l_fab = &fab;
        if (sys$connect(&rab, 0, 0) != RMS$_NORMAL)
            return 22;
        for (r = 0; r < BIG_RECS; r++) {
            rab.rab$l_rbf = rec;
            rab.rab$w_rsz = (uint16_t)sizeof(rec);
            if (sys$put(&rab, 0, 0) != RMS$_NORMAL) {
                printf("  writer: cycle %d $PUT #%d failed sts=%08X stv=%08X\n", c, r,
                       (unsigned)rab.rab$l_sts, (unsigned)rab.rab$l_stv);
                fflush(stdout);
                return 23;
            }
        }
        if (!(sys$close(&fab, 0, 0) & 1))
            return 24;
        fab = cc$rms_fab;
        fab.fab$l_fna = BIG_SPEC;
        fab.fab$b_fns = (uint8_t)strlen(BIG_SPEC);
        if (!(sys$erase(&fab, 0, 0) & 1))
            return 25;
        me->step = (uint32_t)(c + 1);
    }
    timer_stop();
    return 0;
}

/* A wedged child, described from outside: how far it got, and where the
 * kernel says it is (state, wait channel, the system call it is in). */
static void describe(const char *who, pid_t pid, const struct child_progress *p,
                     int total)
{
    static const char *const files[] = { "stat", "wchan", "syscall", "stack" };
    char path[64], buf[512];
    unsigned f;

    printf("  %s: reached %u of %d, %u timer ASTs, first failed re-arm %08X at AST %u\n",
           who, (unsigned)p->step, total, (unsigned)p->asts,
           (unsigned)p->setimr_bad, (unsigned)p->setimr_bad_at);
    for (f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        FILE *fp;
        size_t n;

        snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, files[f]);
        fp = fopen(path, "r");
        if (!fp)
            continue;
        n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        buf[n] = '\0';
        printf("  %s %s: %s%s", who, files[f], buf,
               (n && buf[n - 1] == '\n') ? "" : "\n");
    }
    fflush(stdout);
}

/* Wait for `pid` until the shared deadline; -1 = still running at it. */
static int wait_until(pid_t pid, time_t deadline)
{
    int st;

    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
        if (r < 0 && errno != EINTR)
            return 129;
        if (time(NULL) >= deadline)
            return -1;
        usleep(100000);
    }
}

/* Read LOG back: exactly LINES records, each the one written, in order. */
static void check_log(void)
{
    struct FAB fab = cc$rms_fab;
    struct RAB rab = cc$rms_rab;
    char buf[128], want[96];
    int n = 0, wrong = 0;
    uint32_t st;

    fab.fab$l_fna = LOG_SPEC;
    fab.fab$b_fns = (uint8_t)strlen(LOG_SPEC);
    fab.fab$b_fac = FAB$M_GET;
    fab.fab$b_shr = FAB$M_SHRGET;
    st = sys$open(&fab, 0, 0);
    check(st == RMS$_NORMAL, "the log opens for reading");
    if (st != RMS$_NORMAL)
        return;
    rab.rab$l_fab = &fab;
    if (sys$connect(&rab, 0, 0) == RMS$_NORMAL) {
        for (;;) {
            rab.rab$l_ubf = buf;
            rab.rab$w_usz = (uint16_t)(sizeof(buf) - 1);
            st = sys$get(&rab, 0, 0);
            if (st != RMS$_NORMAL)
                break;
            buf[rab.rab$w_rsz] = '\0';
            line_text(want, sizeof(want), n);
            if (strcmp(buf, want) != 0)
                wrong++;
            n++;
        }
        check(st == RMS$_EOF, "the log reads to RMS$_EOF (no read error)");
    }
    sys$close(&fab, 0, 0);
    printf("  log holds %d records (%d not the line written)\n", n, wrong);
    check(n == LINES, "*** every one of the logger's records arrived ***");
    check(wrong == 0, "  each is the line written, in order");
}

int main(void)
{
    pid_t logger, writer;
    time_t deadline;
    int lrc, wrc;

    printf("=== test_syssvc_rms_dir_writers (rd vms-b6e) ===\n");
    if (vms_kif_open() < 0) {
        printf("SKIP: no /dev/vms -- no executive, no ACP\n");
        return EXIT_SKIP;
    }
    vms_kif_close();

    /* The harness seeds VDA0: but each suite mounts it (idempotent) and
     * dismounts it at its end, as the other RMS/ACP suites do. */
    {
        uint32_t mst = vms_kif_acp_mount("VDA0:");
        check($VMS_STATUS_SUCCESS(mst), "VDA0: mounted executive-global for RMS");
        if (!$VMS_STATUS_SUCCESS(mst))
            return 1;
    }

    erase_all(LOG_SPEC);
    erase_all(BIG_SPEC);

    progress = mmap(NULL, 2 * sizeof(*progress), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (progress == MAP_FAILED)
        progress = NULL;
    check(progress != NULL, "the children's progress page is shared with the parent");
    if (progress == NULL)
        return 1;
    memset(progress, 0, 2 * sizeof(*progress));

    logger = ovmx_fork_subprocess();
    if (logger == 0)
        _exit(run_logger());
    writer = ovmx_fork_subprocess();
    if (writer == 0)
        _exit(run_writer());
    check(logger > 0 && writer > 0,
          "a logger and a writer run as two subprocesses of the executive");
    if (logger <= 0 || writer <= 0)
        return 1;

    deadline = time(NULL) + DEADLINE_S;
    lrc = wait_until(logger, deadline);
    wrc = wait_until(writer, deadline);
    printf("  logger rc=%d writer rc=%d (-1 = still running at the deadline)\n",
           lrc, wrc);
    printf("  INFO: logger %u timer ASTs (re-arm failed %08X at %u); writer %u (%08X at %u)\n",
           (unsigned)progress[0].asts, (unsigned)progress[0].setimr_bad,
           (unsigned)progress[0].setimr_bad_at, (unsigned)progress[1].asts,
           (unsigned)progress[1].setimr_bad, (unsigned)progress[1].setimr_bad_at);
    check(lrc != -1,
          "*** the logger finished: its $PUTs never stalled behind the writer ***");
    check(wrc != -1,
          "*** the writer finished: its creates/writes never stalled behind the logger ***");
    /* negctl: lock-interrupted-enq-orphans-request */
    check(lrc == 0 && wrc == 0,
          "*** both writers finished, every write succeeding: no orphaned volume lock wedged them ***");
    check(lrc == 0, "  and every logger $PUT succeeded");
    check(wrc == 0, "  and every writer create/$PUT/close/erase succeeded");
    if (lrc == -1 || wrc == -1) {
        /* Wedged: say so and end; the stuck processes are the evidence. */
        if (lrc == -1)
            describe("logger", logger, &progress[0], LINES);
        if (wrc == -1)
            describe("writer", writer, &progress[1], CYCLES);
        kill(logger, SIGKILL);
        kill(writer, SIGKILL);
    } else {
        check_log();
        erase_all(LOG_SPEC);
        erase_all(BIG_SPEC);
    }

    vms_kif_acp_dmount("VDA0:");
    printf("RESULTS: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
