/*
 * bench_lnm_cost.c - Measured per-translation cost inputs for the
 *                    logical-name placement ruling (vms-ln0).
 *
 * WHAT THIS MEASURES AND WHY
 * --------------------------
 * vms-ln0 must decide where LNM$SYSTEM / LNM$GROUP / LNM$JOB live once the
 * executive (vms.ko, /dev/vms) owns system-wide state:
 *
 *   A. kernel-owned, one ioctl per translation
 *   B. kernel-owned + per-process cache with invalidation
 *   C. mmap(MAP_SHARED) arena, executive-owned lifetime
 *
 * Logical-name translation sits on the hot path of every file open, so the
 * ruling turns on one number that nobody had: how expensive is a /dev/vms
 * ioctl round trip *relative to the in-process hash lookup it would replace*,
 * on the real runtime target (real kernel, vms.ko loaded).
 *
 * This program measures, in the same process on the same machine:
 *
 *   [clock]     clock_gettime pair overhead (subtracted from nothing; reported
 *               so the reader can see the noise floor)
 *   [getppid]   syscall(SYS_getppid) - minimal syscall entry/exit floor
 *   [enotty]    ioctl() on a non-vms fd returning ENOTTY - syscall entry plus
 *               VFS ioctl dispatch, with no driver work. The floor for ANY
 *               ioctl-based design.
 *   [vms_out]   ioctl(/dev/vms, VMS_IOCTL_GETMODE) - a REAL executive round
 *               trip, copy-OUT only: RCU hash lookup of the caller's PCB,
 *               spin_lock, copy_to_user(24B). The honest LOWER BOUND for
 *               option A's per-translation cost.
 *   [vms_inout] ioctl(/dev/vms, VMS_IOCTL_CHKPRIV) - a REAL executive round
 *               trip with traffic in BOTH directions: PCB lookup,
 *               copy_from_user(32B), spin_lock, compare, copy_to_user(32B).
 *               This is the closest shape vms.ko currently offers to what an
 *               LNM translate ioctl would do (copy a key in, consult
 *               executive state under a lock, copy a value out), so it is the
 *               figure the ruling uses. A real translate ioctl would copy a
 *               longer name/value and walk a hash chain, so even this is a
 *               lower bound - stated as such, never rounded up by guess.
 *   [lnm_hit_p] real lnm_translate() through LNM$FILE_DEV, name in
 *               LNM$PROCESS_TABLE (1 hash lookup - the cheapest hit)
 *   [lnm_hit_s] real lnm_translate() through LNM$FILE_DEV, name in
 *               LNM$SYSTEM (4 hash lookups - process, job, group, system)
 *   [lnm_miss]  real lnm_translate() through LNM$FILE_DEV, name in no table
 *               (4 hash lookups, all miss)
 *
 * The lnm_* cases link the REAL src/vmslnm/ sources with the REAL
 * lnm_setup_defaults() population - not a reimplementation.
 *
 * NO SILENT FALLBACK: if /dev/vms is absent this program FAILS (exit 1) and
 * says so. It never substitutes a fabricated ioctl number. The only way to
 * skip the /dev/vms case is the explicit --calibrate flag, which exists so the
 * identical binary can be run on a native host to quantify how much QEMU TCG
 * emulation inflates the numbers. --calibrate output is labelled CALIBRATION
 * and is not a measurement of the runtime target.
 *
 * Usage:
 *   bench_lnm_cost              # runtime target: requires /dev/vms
 *   bench_lnm_cost --calibrate  # host calibration: skips /dev/vms
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "vms/logical.h"
#include "ssdef.h"
#include "vms_ioctl.h"

/* Target wall time per measured case, in nanoseconds. Each case runs in
 * batches until it has accumulated at least this much time, so a slow TCG
 * guest and a fast native host both get a statistically usable sample
 * without hard-coding an iteration count for either. */
#define TARGET_NS      300000000ULL   /* 300 ms */
#define BATCH          200
#define MAX_ITERS      20000000ULL

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct result {
    const char *label;
    const char *what;
    double ns_per_op;
    uint64_t iters;
    int valid;
};

/* Volatile sink so nothing measured can be optimised away. */
static volatile uint64_t g_sink;

typedef void (*op_fn)(void *ctx);

static struct result run_case(const char *label, const char *what,
                              op_fn fn, void *ctx)
{
    struct result r;
    r.label = label;
    r.what = what;
    r.ns_per_op = 0.0;
    r.iters = 0;
    r.valid = 0;

    /* Warm up: page in code, prime caches, fault in the ioctl path. */
    for (int i = 0; i < BATCH; i++)
        fn(ctx);

    uint64_t total = 0;
    uint64_t iters = 0;
    uint64_t t0 = now_ns();
    while (total < TARGET_NS && iters < MAX_ITERS) {
        for (int i = 0; i < BATCH; i++)
            fn(ctx);
        iters += BATCH;
        total = now_ns() - t0;
    }

    if (iters == 0)
        return r;

    r.ns_per_op = (double)total / (double)iters;
    r.iters = iters;
    r.valid = 1;
    return r;
}

/* ---------------------------------------------------------------- cases */

static void op_clock(void *ctx)
{
    (void)ctx;
    g_sink += now_ns();
}

static void op_getppid(void *ctx)
{
    (void)ctx;
    g_sink += (uint64_t)syscall(SYS_getppid);
}

struct fd_ctx { int fd; };

/* An ioctl that is guaranteed to reach the VFS ioctl path and return ENOTTY.
 * 0xDEAD is not a valid request on the fd we pass. */
static void op_enotty(void *ctx)
{
    struct fd_ctx *c = (struct fd_ctx *)ctx;
    long rc = ioctl(c->fd, 0xDEAD, 0);
    g_sink += (uint64_t)(rc + errno);
}

static void op_vms_getmode(void *ctx)
{
    struct fd_ctx *c = (struct fd_ctx *)ctx;
    struct vms_getmode_args a;
    memset(&a, 0, sizeof(a));
    long rc = ioctl(c->fd, VMS_IOCTL_GETMODE, &a);
    g_sink += (uint64_t)rc + a.mode;
}

static void op_vms_chkpriv(void *ctx)
{
    struct fd_ctx *c = (struct fd_ctx *)ctx;
    struct vms_priv_args a;
    memset(&a, 0, sizeof(a));
    a.mask = 1ULL;
    long rc = ioctl(c->fd, VMS_IOCTL_CHKPRIV, &a);
    g_sink += (uint64_t)rc + a.status;
}

struct lnm_ctx {
    lnm_manager_t *mgr;
    const char *name;
};

static void op_lnm(void *ctx)
{
    struct lnm_ctx *c = (struct lnm_ctx *)ctx;
    char buf[LNM_MAX_VALUE + 1];
    uint16_t len = 0;
    uint32_t attrs = 0;
    uint32_t st = lnm_translate(c->mgr, LNM_FILE_DEV, c->name,
                                buf, sizeof(buf), &len, &attrs);
    g_sink += st + len;
}

/* ----------------------------------------------------------------- main */

static void print_result(const struct result *r)
{
    if (!r->valid) {
        printf("  %-10s %-52s   (no sample)\n", r->label, r->what);
        return;
    }
    printf("  %-10s %-52s %10.1f ns/op   (n=%llu)\n",
           r->label, r->what, r->ns_per_op,
           (unsigned long long)r->iters);
}

int main(int argc, char **argv)
{
    int calibrate = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--calibrate") == 0)
            calibrate = 1;
    }

    printf("=== vms-ln0 logical-name placement cost measurement ===\n");
    if (calibrate)
        printf("MODE: CALIBRATION (native host, /dev/vms case SKIPPED)\n");
    else
        printf("MODE: RUNTIME TARGET (/dev/vms required)\n");
    printf("\n");

    /* ---- open /dev/vms, or fail honestly ---- */
    int vmsfd = -1;
    if (!calibrate) {
        vmsfd = open("/dev/vms", O_RDWR);
        if (vmsfd < 0) {
            printf("  FAIL: /dev/vms absent or unopenable (%s)\n",
                   strerror(errno));
            printf("  This is SS$_NOSUCHDEV. The executive is the only OVMX\n");
            printf("  runtime; there is no per-process fallback to measure.\n");
            printf("FAIL: bench_lnm_cost\n");
            return 1;
        }
        /* The executive requires the caller to register a PCB before any
         * other ioctl; every non-REGISTER ioctl returns -ESRCH otherwise.
         * That registration is a per-process one-off, NOT a per-translation
         * cost, so it is done here and excluded from every timed loop. */
        struct vms_register_args reg;
        memset(&reg, 0, sizeof(reg));
        reg.vms_pid = (uint32_t)getpid();
        reg.init_privs = 0;
        if (ioctl(vmsfd, VMS_IOCTL_REGISTER, &reg) != 0) {
            printf("  FAIL: VMS_IOCTL_REGISTER rejected (%s)\n", strerror(errno));
            printf("FAIL: bench_lnm_cost\n");
            close(vmsfd);
            return 1;
        }

        /* Prove the module answers before we time it. */
        struct vms_getmode_args probe;
        memset(&probe, 0, sizeof(probe));
        if (ioctl(vmsfd, VMS_IOCTL_GETMODE, &probe) != 0) {
            printf("  FAIL: VMS_IOCTL_GETMODE rejected (%s)\n", strerror(errno));
            printf("FAIL: bench_lnm_cost\n");
            close(vmsfd);
            return 1;
        }
        struct vms_priv_args cprobe;
        memset(&cprobe, 0, sizeof(cprobe));
        cprobe.mask = 1ULL;
        if (ioctl(vmsfd, VMS_IOCTL_CHKPRIV, &cprobe) != 0) {
            printf("  FAIL: VMS_IOCTL_CHKPRIV rejected (%s)\n", strerror(errno));
            printf("FAIL: bench_lnm_cost\n");
            close(vmsfd);
            return 1;
        }
        printf("  /dev/vms open, registered, GETMODE answers: mode=%u\n",
               probe.mode);
    }

    /* An fd that is NOT /dev/vms, for the ENOTTY dispatch floor. */
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd < 0) {
        printf("  FAIL: cannot open /dev/null (%s)\n", strerror(errno));
        printf("FAIL: bench_lnm_cost\n");
        if (vmsfd >= 0)
            close(vmsfd);
        return 1;
    }

    /* ---- real logical-name manager, real defaults ---- */
    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) {
        printf("  FAIL: lnm_get_manager() returned NULL\n");
        printf("FAIL: bench_lnm_cost\n");
        return 1;
    }
    lnm_setup_defaults(mgr, NULL);
    /* One process-table name, so the "cheapest hit" case is real. */
    lnm_create(mgr, LNM_PROCESS_TABLE, "BENCH$PROC", "DKA0:[BENCH]",
               LNM_ATTR_TERMINAL, LNM_MODE_USER);

    /* Sanity: the names we time must resolve the way we claim. */
    {
        char buf[LNM_MAX_VALUE + 1];
        uint16_t len = 0;
        uint32_t st;
        st = lnm_translate(mgr, LNM_FILE_DEV, "BENCH$PROC", buf, sizeof(buf),
                           &len, NULL);
        if (st != SS$_NORMAL) {
            printf("  FAIL: BENCH$PROC did not resolve (status=%u)\n", st);
            printf("FAIL: bench_lnm_cost\n");
            return 1;
        }
        st = lnm_translate(mgr, LNM_FILE_DEV, "SYS$SYSTEM", buf, sizeof(buf),
                           &len, NULL);
        if (st != SS$_NORMAL) {
            printf("  FAIL: SYS$SYSTEM did not resolve (status=%u)\n", st);
            printf("FAIL: bench_lnm_cost\n");
            return 1;
        }
        st = lnm_translate(mgr, LNM_FILE_DEV, "NO$SUCH$NAME$AT$ALL",
                           buf, sizeof(buf), &len, NULL);
        if (st == SS$_NORMAL) {
            printf("  FAIL: NO$SUCH$NAME$AT$ALL unexpectedly resolved\n");
            printf("FAIL: bench_lnm_cost\n");
            return 1;
        }
    }

    printf("\n");
    printf("RESULTS (lower is better):\n");

    struct fd_ctx nullctx = { nullfd };
    struct fd_ctx vmsctx  = { vmsfd };
    struct lnm_ctx lp = { mgr, "BENCH$PROC" };
    struct lnm_ctx ls = { mgr, "SYS$SYSTEM" };
    struct lnm_ctx lm = { mgr, "NO$SUCH$NAME$AT$ALL" };

    struct result r_clock = run_case("[clock]", "clock_gettime pair (noise floor)",
                                     op_clock, NULL);
    struct result r_ppid  = run_case("[getppid]", "syscall(SYS_getppid) - syscall floor",
                                     op_getppid, NULL);
    struct result r_enot  = run_case("[enotty]", "ioctl(non-vms fd) -> ENOTTY - dispatch floor",
                                     op_enotty, &nullctx);
    struct result r_vms, r_vmsio;
    memset(&r_vms, 0, sizeof(r_vms));
    memset(&r_vmsio, 0, sizeof(r_vmsio));
    r_vms.label   = "[vms_out]";
    r_vms.what    = "ioctl(/dev/vms, GETMODE) - exec round trip, out only";
    r_vmsio.label = "[vms_inout]";
    r_vmsio.what  = "ioctl(/dev/vms, CHKPRIV) - exec round trip, in+out";
    if (!calibrate) {
        r_vms = run_case(r_vms.label, r_vms.what, op_vms_getmode, &vmsctx);
        r_vmsio = run_case(r_vmsio.label, r_vmsio.what, op_vms_chkpriv, &vmsctx);
    }

    struct result r_lp = run_case("[lnm_hit_p]", "lnm_translate FILE_DEV, hit LNM$PROCESS (1 tbl)",
                                  op_lnm, &lp);
    struct result r_ls = run_case("[lnm_hit_s]", "lnm_translate FILE_DEV, hit LNM$SYSTEM (4 tbl)",
                                  op_lnm, &ls);
    struct result r_lm = run_case("[lnm_miss]", "lnm_translate FILE_DEV, miss all 4 tables",
                                  op_lnm, &lm);

    print_result(&r_clock);
    print_result(&r_ppid);
    print_result(&r_enot);
    print_result(&r_vms);
    print_result(&r_vmsio);
    print_result(&r_lp);
    print_result(&r_ls);
    print_result(&r_lm);

    printf("\nDERIVED:\n");
    if (r_ls.valid && r_ls.ns_per_op > 0.0) {
        if (r_vms.valid)
            printf("  exec ioctl (out only) / in-process 4-table translate = %.1fx\n",
                   r_vms.ns_per_op / r_ls.ns_per_op);
        if (r_vmsio.valid)
            printf("  exec ioctl (in+out)   / in-process 4-table translate = %.1fx\n",
                   r_vmsio.ns_per_op / r_ls.ns_per_op);
        printf("  enotty dispatch       / in-process 4-table translate = %.1fx\n",
               r_enot.ns_per_op / r_ls.ns_per_op);
    }
    if (r_vmsio.valid) {
        /* Option A per-open projection. K = logical-name translations per
         * file open that MISS LNM$PROCESS and therefore must reach the
         * executive. Reported for a range so the reader can apply the K
         * measured separately on the vmsfs path. */
        printf("  Option A added cost per file open, by translations/open K\n");
        printf("  (using the in+out ioctl figure):\n");
        for (int k = 1; k <= 4; k++)
            printf("      K=%d : %.2f us/open (ioctl) vs %.2f us/open (in-process)\n",
                   k, (double)k * r_vmsio.ns_per_op / 1000.0,
                   (double)k * r_ls.ns_per_op / 1000.0);
    }

    if (vmsfd >= 0)
        close(vmsfd);
    close(nullfd);

    printf("\nPASS: bench_lnm_cost\n");
    return 0;
}
