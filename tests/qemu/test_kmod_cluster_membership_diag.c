/*
 * test_kmod_cluster_membership_diag.c - a CLUSTER_DIAG_CSB reader driven with
 * raw ioctls against a real /dev/vms (FC-P3.8, docs/plan-faithful-cluster-
 * executive.md). Exact sibling of test_kmod_cluster_conn_diag.c (FC-P2.4):
 * open /dev/vms, issue VMS_IOCTL_CLUSTER_DIAG_CSB, print what the EXECUTIVE
 * reports and nothing else (INV-6) -- never a value this program invented.
 *
 * TWO JOBS.
 *
 *   1. As a standalone `test_kmod_*` suite it is the R3/booted-node NEGCTL for
 *      the connection manager's own diagnostics: the ioctl must DISPATCH on a
 *      booted node whether or not the cluster has ever formed, a CSB index
 *      past the executive's high-water mark must answer SS$_NOSUCHDEV with an
 *      ALL-ZERO row, and `csid` must never be reported as learned unless
 *      `csid_valid` is set (the E30 tripwire: a placeholder CSID is what
 *      bugchecked a real VAX).
 *
 *   2. As a CLI (`-row club`, `-row csb -index N`, `-walk`) it is what
 *      tests/qemu/test_cluster_membership.sh shells out to from inside a
 *      booted guest to read the CLUB/CSB table back as `key=value` lines --
 *      exactly the shape test_kmod_cluster_conn_diag.c already established.
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

static int diag_csb(int fd, uint32_t row, uint32_t index,
                    struct vms_cluster_diag_csb_args *out)
{
    memset(out, 0, sizeof(*out));
    out->row = row;
    out->index = index;
    if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_CSB, out) < 0)
        return -1;
    return 0;
}

static void print_club_row(const struct vms_cluster_diag_csb_args *a)
{
    printf("local_csid_valid=%u\n", (unsigned)a->club.local_csid_valid);
    if (a->club.local_csid_valid)
        printf("local_csid=%08x\n", a->club.local_csid);
    else
        printf("local_csid=<absent>\n");   /* NOT csid 0 (INV-6, E30) */
    printf("state=%u\n", (unsigned)a->club.state);
    printf("cluster_nodes=%u\n", a->club.cluster_nodes);
    printf("cevotes=%u\n", (unsigned)a->club.cevotes);
    printf("quorum=%u\n", (unsigned)a->club.quorum);
    printf("transition_active=%u\n", (unsigned)a->club.transition_active);
    printf("barrier_step=%u\n", (unsigned)a->club.barrier_step);
    printf("reformations=%u\n", a->club.reformations);
}

static void print_csb_row(const struct vms_cluster_diag_csb_args *a)
{
    printf("csid_valid=%u\n", (unsigned)a->csb.csid_valid);
    if (a->csb.csid_valid)
        printf("csid=%08x\n", a->csb.csid);
    else
        printf("csid=<absent>\n");         /* NOT csid 0 (INV-6, E30) */
    printf("state=%u\n", (unsigned)a->csb.state);
    printf("is_member=%u\n", (unsigned)a->csb.is_member);
    printf("is_selected=%u\n", (unsigned)a->csb.is_selected);
    printf("peer_sysid_lo=%u\n", a->csb.peer_sysid_lo);
    printf("peer_sysid_hi=%u\n", a->csb.peer_sysid_hi);
    printf("votes_valid=%u\n", (unsigned)a->csb.votes_valid);
    if (a->csb.votes_valid)
        printf("votes=%u\n", (unsigned)a->csb.votes);
    printf("cdt_conid=%08x\n", a->csb.cdt_conid);
}

static int run_cli(int fd, const char *row_name, uint32_t index)
{
    struct vms_cluster_diag_csb_args a;
    uint32_t row;

    if (strcmp(row_name, "club") == 0)
        row = VMS_CLUSTER_DIAG_CSB_CLUB;
    else if (strcmp(row_name, "csb") == 0)
        row = VMS_CLUSTER_DIAG_CSB_CSB;
    else {
        fprintf(stderr, "unknown row '%s' (want club|csb)\n", row_name);
        return 2;
    }
    if (diag_csb(fd, row, index, &a) != 0) {
        fprintf(stderr, "ioctl(CLUSTER_DIAG_CSB) failed: %s\n",
                strerror(errno));
        return 2;
    }

    printf("status=%u\n", a.status);
    if (a.status != SS_NORMAL)
        return 0;   /* honest: no row to print, the caller reads `status` */

    if (row == VMS_CLUSTER_DIAG_CSB_CLUB)
        print_club_row(&a);
    else
        print_csb_row(&a);
    return 0;
}

/* Every CSB row the executive holds -- so the harness can count real peers
 * without knowing which CLUB slot each landed in. */
static int run_walk(int fd, uint32_t limit)
{
    struct vms_cluster_diag_csb_args a;
    uint32_t i, found = 0u, members = 0u;

    for (i = 0; i < limit; i++) {
        if (diag_csb(fd, VMS_CLUSTER_DIAG_CSB_CSB, i, &a) != 0)
            return 2;
        if (a.status != SS_NORMAL)
            continue;
        printf("--- csb index=%u ---\n", i);
        print_csb_row(&a);
        found++;
        if (a.csb.is_member)
            members++;
    }
    printf("csb_rows=%u\n", found);
    printf("csb_members=%u\n", members);
    return 0;
}

static void suite_club_row(int fd)
{
    struct vms_cluster_diag_csb_args a;

    /* The ioctl always dispatches and always returns a real status:
     * SS$_NORMAL once vms_cnxman_start() has run, SS$_NOSUCHDEV before
     * CLUSTER_START. Either is a legitimate observation on a node this suite
     * did not start; what is asserted is that the dispatcher answered. */
    CHECK(diag_csb(fd, VMS_CLUSTER_DIAG_CSB_CLUB, 0, &a) == 0,
          "row CLUB: the ioctl dispatches");
    CHECK(a.status != 0,
          "row CLUB: a real SS$_ status came back, not a silent zero");
    if (a.status != SS_NORMAL) {
        CHECK(a.club.local_csid_valid == 0 && a.club.cluster_nodes == 0,
              "row CLUB, CNXMAN not started: the row is genuinely all-zero, "
              "never a placeholder");
    } else {
        printf("  (informational: local_csid_valid=%u -- E30 predicts 0 "
               "until the op-06 layout is lab-pinned)\n",
               (unsigned)a.club.local_csid_valid);
    }
}

static void suite_csb_negctl(int fd)
{
    struct vms_cluster_diag_csb_args a;
    uint32_t i, any_valid = 0;

    CHECK(diag_csb(fd, VMS_CLUSTER_DIAG_CSB_CSB, 99999u, &a) == 0 &&
          a.status != SS_NORMAL,
          "an index far past the high-water mark refuses (SS$_NOSUCHDEV), "
          "never a wrapped/aliased row");
    CHECK(a.csb.csid == 0 && a.csb.csid_valid == 0 && a.csb.peer_sysid_lo == 0,
          "... and the refused row is genuinely all-zero, never a "
          "placeholder member");

    for (i = 0; i < 32; i++) {
        if (diag_csb(fd, VMS_CLUSTER_DIAG_CSB_CSB, i, &a) == 0 &&
            a.status == SS_NORMAL && a.csb.csid_valid)
            any_valid++;
    }
    printf("  (informational: %u of the first 32 CSB rows carry a LEARNED "
         "csid -- E30 predicts 0 until the op-06 layout is lab-pinned)\n",
         any_valid);
}

static int run_suite(int fd)
{
    printf("=== test_kmod_cluster_membership_diag ===\n");
    suite_club_row(fd);
    suite_csb_negctl(fd);
    printf("=== test_kmod_cluster_membership_diag: %d passed, %d failed ===\n",
           pass, fail);
    return fail == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    int fd = open("/dev/vms", O_RDWR);
    int rc;

    if (fd < 0) {
        printf("=== test_kmod_cluster_membership_diag: 0 passed, 0 failed "
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
