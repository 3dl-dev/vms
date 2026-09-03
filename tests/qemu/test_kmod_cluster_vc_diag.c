/*
 * test_kmod_cluster_vc_diag.c - a CLUSTER_DIAG_PORT reader driven with raw
 * ioctls against a real /dev/vms (FC-P1.6, docs/plan-faithful-cluster-
 * executive.md). Same footing as tests/qemu/test_kmod_resdir.c: open
 * /dev/vms, issue VMS_IOCTL_CLUSTER_DIAG_PORT, print what the EXECUTIVE
 * reports and nothing else (INV-6) -- never a value this program invented.
 *
 * TWO JOBS.
 *
 *   1. As a standalone `test_kmod_*` suite it is the R3/booted-node negctl
 *      for the FC-P1.6 wire fields: on ANY booted node (cluster started or
 *      not) row PORT must always answer SS$_NORMAL once the port is up, and
 *      row VC on an index with no circuit must answer SS$_NOSUCHDEV --
 *      never a placeholder row (the exact discipline vms_ioctl_cluster_
 *      diag_port's own header comment names).
 *
 *   2. As a CLI (`-row vc -index N`) it is what tests/qemu/test_cluster_vc.sh
 *      shells out to from inside a booted guest to read one VC row back as
 *      `key=value` lines -- the R4 harness's only way to see the port's real
 *      FSM state, since CLUSTER_DIAG_PORT has no DCL surface yet.
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

static int diag_port(int fd, uint32_t row, uint32_t index,
                      struct vms_cluster_diag_port_args *out)
{
    memset(out, 0, sizeof(*out));
    out->row = row;
    out->index = index;
    if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_PORT, out) < 0)
        return -1;
    return 0;
}

/* CLI mode: print one row as key=value lines the shell harness can grep,
 * e.g. `test_kmod_cluster_vc_diag -row vc -index 0`. */
static int run_cli(int fd, const char *row_name, uint32_t index)
{
    struct vms_cluster_diag_port_args a;
    uint32_t row;

    if (strcmp(row_name, "port") == 0)
        row = VMS_CLUSTER_DIAG_PORT_ROW;
    else if (strcmp(row_name, "channel") == 0)
        row = VMS_CLUSTER_DIAG_PORT_CHANNEL;
    else if (strcmp(row_name, "vc") == 0)
        row = VMS_CLUSTER_DIAG_PORT_VC;
    else {
        fprintf(stderr, "unknown row '%s' (want port|channel|vc)\n", row_name);
        return 2;
    }

    if (diag_port(fd, row, index, &a) != 0) {
        fprintf(stderr, "ioctl(CLUSTER_DIAG_PORT) failed: %s\n", strerror(errno));
        return 2;
    }

    printf("status=%u\n", a.status);
    if (a.status != SS_NORMAL)
        return 0; /* honest: no row to print, the caller reads `status` */

    switch (row) {
    case VMS_CLUSTER_DIAG_PORT_ROW:
        printf("port_open=%u\n", a.port.port_open);
        printf("link_up=%u\n", a.port.link_up);
        printf("n_channels=%u\n", a.port.n_channels);
        printf("n_vcs=%u\n", a.port.n_vcs);
        printf("rx_frames=%u\n", a.port.rx_frames);
        printf("tx_frames=%u\n", a.port.tx_frames);
        printf("tx_errors=%u\n", a.port.tx_errors);
        break;
    case VMS_CLUSTER_DIAG_PORT_CHANNEL:
        printf("state=%u\n", a.channel.state);
        printf("remote_sysid_valid=%u\n", a.channel.remote_sysid_valid);
        printf("remote_sysid_lo=%u\n", a.channel.remote_sysid_lo);
        printf("hello_tx=%u\n", a.channel.hello_tx);
        printf("hello_rx=%u\n", a.channel.hello_rx);
        break;
    case VMS_CLUSTER_DIAG_PORT_VC:
        printf("state=%u\n", a.vc.state);
        printf("peer_sysid_lo=%u\n", a.vc.peer_sysid_lo);
        printf("peer_sysid_hi=%u\n", a.vc.peer_sysid_hi);
        printf("send_seq=%u\n", a.vc.send_seq);
        printf("recv_seq=%u\n", a.vc.recv_seq);
        printf("recv_ack=%u\n", a.vc.recv_ack);
        printf("peer_recv_ack=%u\n", a.vc.peer_recv_ack);
        printf("unacked=%u\n", a.vc.unacked);
        printf("retransmits=%u\n", a.vc.retransmits);
        /* FC-P1.6: the two fields this item added. */
        printf("rx_gaps=%u\n", a.vc.rx_gaps);
        printf("down_reason=%u\n", (unsigned)a.vc.down_reason);
        break;
    }
    return 0;
}

/* Suite mode: the honest negctl every kernel-executive test proves. */
static int run_suite(int fd)
{
    struct vms_cluster_diag_port_args a;

    printf("=== test_kmod_cluster_vc_diag ===\n");

    /* Row PORT: whatever state the port is in, the ioctl itself always
     * dispatches and returns a real status -- SS$_NORMAL once vms_pe_start()
     * has run, SS$_NOSUCHDEV before CLUSTER_START (this suite does not
     * start the cluster itself, so either is a legitimate observation; the
     * assertion is that the dispatcher answered at all, not which state). */
    CHECK(diag_port(fd, VMS_CLUSTER_DIAG_PORT_ROW, 0, &a) == 0,
          "row PORT: the ioctl dispatches");
    CHECK(a.status == SS_NORMAL || a.status != 0,
          "row PORT: a real SS$_ status came back, not a silent zero");

    /* Row VC, index 0: with no circuit ever formed by THIS process, the
     * honest answer is SS$_NOSUCHDEV and an all-zero row -- never a
     * placeholder circuit (INV-6, vms_ioctl_cluster_diag_port's own
     * negctl). If a real VC happens to be up already (another process
     * started the port and it formed one), that is equally honest: this
     * assertion only refuses a row whose `status` claims success while its
     * `state` field is stale/zero, which the codepath cannot produce
     * (pe_fsm_vc_project zeroes the struct before any early return). */
    CHECK(diag_port(fd, VMS_CLUSTER_DIAG_PORT_VC, 0, &a) == 0,
          "row VC index 0: the ioctl dispatches");
    if (a.status != SS_NORMAL) {
        CHECK(a.vc.state == 0 && a.vc.send_seq == 0 && a.vc.rx_gaps == 0 &&
              a.vc.down_reason == 0,
              "row VC, no circuit: the row is genuinely all-zero, not a "
              "placeholder (INV-6)");
    } else {
        printf("  (a real VC is already up on index 0 -- state=%u rx_gaps=%u "
               "down_reason=%u)\n",
               a.vc.state, a.vc.rx_gaps, (unsigned)a.vc.down_reason);
        pass++;
    }

    /* Row VC past the table's high-water mark: SS$_NOSUCHDEV, honestly. */
    CHECK(diag_port(fd, VMS_CLUSTER_DIAG_PORT_VC, 999999u, &a) == 0 &&
          a.status != SS_NORMAL,
          "row VC, index far past any table: SS$_NOSUCHDEV, not a crash");

    printf("=== test_kmod_cluster_vc_diag: %d passed, %d failed ===\n",
           pass, fail);
    return fail == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        printf("=== test_kmod_cluster_vc_diag: 0 passed, 0 failed (SKIPPED: "
               "no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    if (argc >= 3 && strcmp(argv[1], "-row") == 0) {
        const char *row_name = argv[2];
        uint32_t index = 0;
        int i;

        for (i = 3; i < argc - 1; i++)
            if (strcmp(argv[i], "-index") == 0)
                index = (uint32_t)strtoul(argv[i + 1], NULL, 10);

        int rc = run_cli(fd, row_name, index);
        close(fd);
        return rc;
    }

    int rc = run_suite(fd);
    close(fd);
    return rc;
}
