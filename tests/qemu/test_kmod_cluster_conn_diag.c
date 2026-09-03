/*
 * test_kmod_cluster_conn_diag.c - a CLUSTER_DIAG_CONN reader driven with raw
 * ioctls against a real /dev/vms (FC-P2.4, docs/plan-faithful-cluster-
 * executive.md). Exact sibling of test_kmod_cluster_vc_diag.c (FC-P1.6): open
 * /dev/vms, issue VMS_IOCTL_CLUSTER_DIAG_CONN, print what the EXECUTIVE
 * reports and nothing else (INV-6) -- never a value this program invented.
 *
 * TWO JOBS.
 *
 *   1. As a standalone `test_kmod_*` suite it is the R3/booted-node NEGCTL for
 *      the SCS diagnostics: the ioctl must DISPATCH on a booted node whether or
 *      not the cluster was started, row CDT on a slot holding no connection
 *      must answer SS$_NOSUCHDEV with an ALL-ZERO row, and a Con.ID column must
 *      never be reported as bound unless the executive really learned it
 *      (`remote_conid_valid`). That last one is the INV-6 tripwire this whole
 *      layer exists under: a placeholder connection identifier is what
 *      bugchecked a real VAX.
 *
 *   2. As a CLI (`-row cdt -index N`, `-row scs`) it is what
 *      tests/qemu/test_cluster_conn.sh shells out to from inside a booted guest
 *      to read the SCS view and one CDT row back as `key=value` lines -- the R4
 *      harness's only way to see SCS's real state, since CLUSTER_DIAG_CONN has
 *      no DCL surface yet.
 *
 * The CDT row's columns ARE the SDA `SHOW CONNECTIONS` decoder ring (wire spec
 * SS3): Local SYSAP, Remote, the Con.ID pair, Credit (Send/Recv), State, and
 * the spec SS4(m) MTYPE phase byte -- printed here in that order so a lab
 * comparison against a real VAX's SDA output is a field-by-field match.
 *
 * Exit 77 (honest skip) when /dev/vms is absent, the test_syssvc_* contract
 * every kernel-executive suite in this tree follows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "vms_ioctl.h"

#define EXIT_SKIP 77
#define SS_NORMAL 1u

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static int diag_conn(int fd, uint32_t row, uint32_t index,
                     struct vms_cluster_diag_conn_args *out)
{
    memset(out, 0, sizeof(*out));
    out->row = row;
    out->index = index;
    if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_CONN, out) < 0)
        return -1;
    return 0;
}

/* A 16-byte blank-padded SYSAP name, printed with its trailing blanks trimmed
 * -- the same shape SDA prints, and the bytes exactly as the wire carries
 * them (no NUL is added: NUL padding is a DIFFERENT name to a real VAX). */
static void print_name(const char *key, const uint8_t *n)
{
    char buf[17];
    int i, end = 0;

    for (i = 0; i < 16; i++) {
        buf[i] = (n[i] >= 0x20 && n[i] < 0x7f) ? (char)n[i] : '.';
        if (n[i] != ' ' && n[i] != 0)
            end = i + 1;
    }
    buf[end] = '\0';
    printf("%s=%s\n", key, buf);
}

static int row_from_name(const char *row_name, uint32_t *out)
{
    if (strcmp(row_name, "scs") == 0) {
        *out = VMS_CLUSTER_DIAG_CONN_ROW;
        return 0;
    }
    if (strcmp(row_name, "cdt") == 0) {
        *out = VMS_CLUSTER_DIAG_CONN_CDT;
        return 0;
    }
    return -1;
}

static void print_scs_row(const struct vms_cluster_diag_conn_args *a)
{
    printf("n_sbs=%u\n", a->scs.n_sbs);
    printf("n_cdts=%u\n", a->scs.n_cdts);
    printf("n_sysaps=%u\n", a->scs.n_sysaps);
    printf("conid_seq=%u\n", a->scs.conid_seq);
    printf("conid_epoch=%u\n", a->scs.conid_epoch);
    printf("dir_lookups_served=%u\n", a->scs.dir_lookups_served);
    printf("dir_lookups_sent=%u\n", a->scs.dir_lookups_sent);
    printf("credit_stalls=%u\n", a->scs.credit_stalls);
}

/* The SDA SHOW CONNECTIONS column order. */
static void print_cdt_row(const struct vms_cluster_diag_conn_args *a)
{
    print_name("local_name", a->cdt.local_name);
    print_name("remote_name", a->cdt.remote_name);
    printf("peer_sysid_lo=%u\n", a->cdt.peer_sysid_lo);
    printf("peer_sysid_hi=%u\n", a->cdt.peer_sysid_hi);
    printf("local_conid=%08x\n", a->cdt.local_conid);
    printf("remote_conid_valid=%u\n", (unsigned)a->cdt.remote_conid_valid);
    if (a->cdt.remote_conid_valid)
        printf("remote_conid=%08x\n", a->cdt.remote_conid);
    else
        printf("remote_conid=<absent>\n");  /* NOT a zero handle (INV-6) */
    printf("credit_send=%u\n", a->cdt.credit_send);
    printf("credit_receive=%u\n", a->cdt.credit_receive);
    printf("credit_pending=%u\n", a->cdt.credit_pending);
    printf("state=%u\n", (unsigned)a->cdt.state);
    printf("msgtype=%02x\n", (unsigned)a->cdt.msgtype);
    printf("msgs_sent=%u\n", a->cdt.msgs_sent);
    printf("msgs_received=%u\n", a->cdt.msgs_received);
}

static int run_cli(int fd, const char *row_name, uint32_t index)
{
    struct vms_cluster_diag_conn_args a;
    uint32_t row;

    if (row_from_name(row_name, &row) != 0) {
        fprintf(stderr, "unknown row '%s' (want scs|cdt)\n", row_name);
        return 2;
    }
    if (diag_conn(fd, row, index, &a) != 0) {
        fprintf(stderr, "ioctl(CLUSTER_DIAG_CONN) failed: %s\n",
                strerror(errno));
        return 2;
    }

    printf("status=%u\n", a.status);
    if (a.status != SS_NORMAL)
        return 0;   /* honest: no row to print, the caller reads `status` */

    if (row == VMS_CLUSTER_DIAG_CONN_ROW)
        print_scs_row(&a);
    else
        print_cdt_row(&a);
    return 0;
}

/* Every CDT row the executive holds, so the harness can look for one by name
 * without knowing which CDL slot it landed in. */
static int run_walk(int fd, uint32_t limit)
{
    struct vms_cluster_diag_conn_args a;
    uint32_t i, found = 0u;

    for (i = 0; i < limit; i++) {
        if (diag_conn(fd, VMS_CLUSTER_DIAG_CONN_CDT, i, &a) != 0)
            return 2;
        if (a.status != SS_NORMAL)
            continue;
        printf("--- cdt index=%u ---\n", i);
        print_cdt_row(&a);
        found++;
    }
    printf("cdt_rows=%u\n", found);
    return 0;
}

static void suite_scs_row(int fd)
{
    struct vms_cluster_diag_conn_args a;

    /* The ioctl always dispatches and always returns a real status:
     * SS$_NORMAL once vms_scs_start() has run, SS$_NOSUCHDEV before
     * CLUSTER_START. Either is a legitimate observation on a node this suite
     * did not start; what is asserted is that the dispatcher answered. */
    CHECK(diag_conn(fd, VMS_CLUSTER_DIAG_CONN_ROW, 0, &a) == 0,
          "row SCS: the ioctl dispatches");
    CHECK(a.status != 0,
          "row SCS: a real SS$_ status came back, not a silent zero");
    if (a.status != SS_NORMAL) {
        CHECK(a.scs.n_sbs == 0 && a.scs.n_cdts == 0 && a.scs.n_sysaps == 0 &&
              a.scs.conid_epoch == 0,
              "row SCS, SCS not started: the row is genuinely all-zero, "
              "not a placeholder (INV-6)");
    } else {
        printf("  (SCS is up: n_sbs=%u n_cdts=%u n_sysaps=%u conid_epoch=%u)\n",
               a.scs.n_sbs, a.scs.n_cdts, a.scs.n_sysaps, a.scs.conid_epoch);
        /*
         * SCS$DIRECTORY is registered by vms_scs_start() itself, and that
         * registration ALLOCATES A CDT AND MINTS ITS Con.ID -- which the
         * allocator refuses to do until the glue has seeded it from a real
         * per-boot value (SCS_ERR_NOCONID, vms_scs_fsm.h SS4). So a non-zero
         * SYSAP count on a started SCS is also the proof that the seeding
         * happened; `conid_epoch` is deliberately NOT asserted non-zero,
         * because a seed of 0 is a legal value and asserting against it would
         * be a coin-flip, not a check.
         */
        CHECK(a.scs.n_sysaps >= 1u,
              "row SCS, SCS up: at least SCS$DIRECTORY is registered -- which "
              "means its Con.ID was minted, which means the allocator was "
              "really seeded (spec SS4(t))");
    }
}

static void suite_cdt_negctl(int fd)
{
    struct vms_cluster_diag_conn_args a;
    uint32_t i;
    int any_bound_without_flag = 0;
    int any_row = 0, any_zero_conid = 0;

    /* A slot far past any CDL: SS$_NOSUCHDEV and an all-zero row. */
    CHECK(diag_conn(fd, VMS_CLUSTER_DIAG_CONN_CDT, 999999u, &a) == 0 &&
          a.status != SS_NORMAL,
          "row CDT, index far past any CDL: SS$_NOSUCHDEV, not a crash");
    CHECK(a.cdt.local_conid == 0 && a.cdt.remote_conid == 0 &&
          a.cdt.state == 0 && a.cdt.msgtype == 0 &&
          a.cdt.remote_conid_valid == 0,
          "... and the row is genuinely all-zero, never a placeholder "
          "connection (INV-6)");

    /* THE INV-6 TRIPWIRE. Across every row this node really holds, a
     * remote Con.ID may be non-zero ONLY when the executive flagged it
     * learned. A placeholder handle would show up here as a value with the
     * flag clear. */
    for (i = 0; i < 256u; i++) {
        if (diag_conn(fd, VMS_CLUSTER_DIAG_CONN_CDT, i, &a) != 0)
            break;
        if (a.status != SS_NORMAL)
            continue;
        any_row = 1;
        if (!a.cdt.remote_conid_valid && a.cdt.remote_conid != 0)
            any_bound_without_flag = 1;
        /* The allocator never mints a Con.ID whose low half is 0 -- the
         * wire uses 0 for "not bound yet" (spec SS4(m)), so a projected row
         * carrying one would be a handle nothing allocated. */
        if (a.cdt.local_conid == 0)
            any_zero_conid = 1;
    }
    CHECK(any_bound_without_flag == 0,
          "no CDT row reports a remote Con.ID the executive never learned");
    if (any_row) {
        CHECK(any_zero_conid == 0,
              "every projected CDT row carries a real minted Local Con.ID, "
              "never 0 (the wire's own 'not bound yet' value)");
    } else {
        printf("  (this node holds no CDT at all -- SCS is not started; the "
               "Local Con.ID check has nothing to read)\n");
    }
}

static int run_suite(int fd)
{
    printf("=== test_kmod_cluster_conn_diag ===\n");
    suite_scs_row(fd);
    suite_cdt_negctl(fd);
    printf("=== test_kmod_cluster_conn_diag: %d passed, %d failed ===\n",
           pass, fail);
    return fail == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    int fd = open("/dev/vms", O_RDWR);
    int rc;

    if (fd < 0) {
        printf("=== test_kmod_cluster_conn_diag: 0 passed, 0 failed "
               "(SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    if (argc >= 2 && strcmp(argv[1], "-walk") == 0) {
        uint32_t limit = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10)
                                     : 32u;
        rc = run_walk(fd, limit);
        close(fd);
        return rc;
    }
    if (argc >= 3 && strcmp(argv[1], "-row") == 0) {
        const char *row_name = argv[2];
        uint32_t index = 0;
        int i;

        for (i = 3; i < argc - 1; i++) {
            if (strcmp(argv[i], "-index") == 0)
                index = (uint32_t)strtoul(argv[i + 1], NULL, 10);
        }
        rc = run_cli(fd, row_name, index);
        close(fd);
        return rc;
    }

    rc = run_suite(fd);
    close(fd);
    return rc;
}
