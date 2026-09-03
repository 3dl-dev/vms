/*
 * test_kmod_cluster_fork_hammer.c - the R3 same-CPU hammer for FC-P0.16
 * (receive-level lock conformance: design docs/design-faithful-cluster-
 * executive.md SS3.2.3 RULING / CONTRACT RULE 14.1, plan
 * docs/plan-faithful-cluster-executive.md FC-P0.16).
 *
 * WHAT THIS PROVES. vms_ktest_cluster_fork_hammer_run (src/kernel/vms_module.c,
 * compiled ONLY under OVMX_KTEST_CLUSTER_SEAM -- never the bootable executive)
 * stands up the REAL cluster fork context (vms_cluster_fork_start, the actual
 * exec_rxlock_t + exec_cv_wait_rx path FC-P0.16 lands) and hammers its ONE
 * shared object -- the fork queue -- from receive level (a real 0x6007 frame
 * flood over a veth pair, delivered to exec_lan_open's rx_cb in Linux's
 * receive softirq) and from process context (a poster kthread calling
 * cf_post() in a tight loop) at the same time, both kthreads bound to CPU 0.
 * The harness this runs under boots with -smp 1 (tests/qemu/run_tests.sh), so
 * CPU 0 is the ONLY vCPU: every receive-softirq delivery and every poster
 * iteration genuinely share one core, reproducing exactly the hazard design
 * SS3.2.3 records (a process-context holder of the pre-FC-P0.16 exec_lock_t
 * preempted by a same-CPU softirq taking the same lock deadlocks solid).
 *
 * DEFAULT DURATION. This program drives the knob with NO third field, so the
 * kernel-side default (3000ms) applies -- a real proof of the mechanism that
 * fits inside run_tests.sh's shared ~600s wall across ~77 suites. The SAME
 * knob accepts a duration up to 65000ms; tests/qemu/run_cluster_fork_hammer_60s.sh
 * drives the full 60s design-doc figure from a SEPARATE, non-default harness
 * invocation (never inside the default per-PR battery).
 *
 * NEGATIVE CONTROL: under NEGATIVE_CONTROL=1 (no vms.ko inserted) every sysfs
 * open below fails and the suite fails honestly (no per-process fallback,
 * INV-6) rather than fabricating a result -- the same posture
 * test_kmod_cluster_seam.c takes.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/veth.h>

#define RUN_PARAM    "/sys/module/vms/parameters/vms_ktest_cluster_fork_hammer_run"
#define RESULT_PARAM "/sys/module/vms/parameters/vms_ktest_cluster_fork_hammer_result"

#define IF_A "vhmr0"
#define IF_B "vhmr1"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* ---- minimal rtnetlink veth-pair creation, identical mechanism to
 * test_kmod_cluster_seam.c (clean-room: linux/rtnetlink.h + linux/if_link.h +
 * linux/veth.h, nothing copied from iproute2). ---- */

struct nl_req {
    struct nlmsghdr nh;
    struct ifinfomsg ifi;
    char attrbuf[512];
};

static void nl_put(struct nlmsghdr *nh, int type, const void *data, size_t len)
{
    struct rtattr *rta = (struct rtattr *)((char *)nh + NLMSG_ALIGN(nh->nlmsg_len));

    rta->rta_type = type;
    rta->rta_len = RTA_LENGTH(len);
    if (len)
        memcpy(RTA_DATA(rta), data, len);
    nh->nlmsg_len = NLMSG_ALIGN(nh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
}

static struct rtattr *nl_nest(struct nlmsghdr *nh, int type)
{
    struct rtattr *rta = (struct rtattr *)((char *)nh + NLMSG_ALIGN(nh->nlmsg_len));

    rta->rta_type = type;
    rta->rta_len = RTA_LENGTH(0);
    nh->nlmsg_len = NLMSG_ALIGN(nh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    return rta;
}

static void nl_nest_end(struct nlmsghdr *nh, struct rtattr *rta)
{
    rta->rta_len = (char *)nh + NLMSG_ALIGN(nh->nlmsg_len) - (char *)rta;
}

static int create_veth_pair(const char *a, const char *b)
{
    int fd, rc;
    struct nl_req req;
    struct rtattr *linkinfo, *infodata, *peer;
    struct ifinfomsg *peer_ifi;
    struct sockaddr_nl sa;
    struct { struct nlmsghdr nh; struct nlmsgerr err; } ack;

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        return -1;

    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nh.nlmsg_type = RTM_NEWLINK;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req.nh.nlmsg_seq = 1;
    req.ifi.ifi_family = AF_UNSPEC;

    nl_put(&req.nh, IFLA_IFNAME, a, strlen(a) + 1);
    linkinfo = nl_nest(&req.nh, IFLA_LINKINFO);
    nl_put(&req.nh, IFLA_INFO_KIND, "veth", 5);
    infodata = nl_nest(&req.nh, IFLA_INFO_DATA);
    peer = nl_nest(&req.nh, VETH_INFO_PEER);
    peer_ifi = (struct ifinfomsg *)((char *)&req.nh + NLMSG_ALIGN(req.nh.nlmsg_len));
    memset(peer_ifi, 0, sizeof(*peer_ifi));
    req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + sizeof(*peer_ifi);
    nl_put(&req.nh, IFLA_IFNAME, b, strlen(b) + 1);
    nl_nest_end(&req.nh, peer);
    nl_nest_end(&req.nh, infodata);
    nl_nest_end(&req.nh, linkinfo);

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    rc = (int)sendto(fd, &req, req.nh.nlmsg_len, 0,
                      (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0) {
        close(fd);
        return -1;
    }
    rc = (int)recv(fd, &ack, sizeof(ack), 0);
    close(fd);
    if (rc < (int)sizeof(struct nlmsghdr))
        return -1;
    return ack.err.error == 0 ? 0 : -1;
}

static int link_set_up(const char *name)
{
    int fd, rc, ifindex;
    struct nl_req req;
    struct sockaddr_nl sa;
    struct { struct nlmsghdr nh; struct nlmsgerr err; } ack;

    ifindex = (int)if_nametoindex(name);
    if (ifindex == 0)
        return -1;

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        return -1;

    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nh.nlmsg_type = RTM_SETLINK;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nh.nlmsg_seq = 1;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index = ifindex;
    req.ifi.ifi_flags = IFF_UP;
    req.ifi.ifi_change = IFF_UP;

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    rc = (int)sendto(fd, &req, req.nh.nlmsg_len, 0,
                      (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0) {
        close(fd);
        return -1;
    }
    rc = (int)recv(fd, &ack, sizeof(ack), 0);
    close(fd);
    if (rc < (int)sizeof(struct nlmsghdr))
        return -1;
    return ack.err.error == 0 ? 0 : -1;
}

/* ---- sysfs knob helpers (identical shape to test_kmod_cluster_seam.c) ---- */

static int write_param(const char *path, const char *val)
{
    FILE *f = fopen(path, "w");
    int rc;

    if (!f)
        return -1;
    rc = fprintf(f, "%s", val);
    if (fclose(f) != 0)
        return -1;
    return rc > 0 ? 0 : -1;
}

static int read_param(const char *path, char *out, size_t outsz)
{
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f)
        return -1;
    n = fread(out, 1, outsz - 1, f);
    fclose(f);
    out[n] = '\0';
    return 0;
}

static int result_field(const char *buf, const char *key, char *out, size_t outsz)
{
    const char *p = strstr(buf, key);
    size_t klen = strlen(key);
    size_t i = 0;

    if (!p || p[klen] != '=')
        return -1;
    p += klen + 1;
    while (p[i] && p[i] != ' ' && p[i] != '\n' && i < outsz - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return 0;
}

static long long result_ll(const char *buf, const char *key)
{
    char v[32];

    if (result_field(buf, key, v, sizeof(v)) != 0)
        return -1;
    return atoll(v);
}

/*
 * ovmx_hammer_ms= on /proc/cmdline (default per-PR boots carry no such token,
 * so this returns 0 and the kernel-side default of 3000ms applies). The
 * dedicated, non-default tests/qemu/run_cluster_fork_hammer_60s.sh boots the
 * SAME already-built image with "-append ... ovmx_hammer_ms=60000" so the
 * genuine 60s design-doc figure runs from a separate invocation, never inside
 * the default per-PR battery (see the file header NOTE and vms_module.c's
 * matching comment).
 */
static unsigned int cmdline_hammer_ms(void)
{
    FILE *f = fopen("/proc/cmdline", "r");
    char line[512];
    char *p;
    unsigned int v = 0;

    if (!f)
        return 0;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    p = strstr(line, "ovmx_hammer_ms=");
    if (!p)
        return 0;
    p += strlen("ovmx_hammer_ms=");
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (unsigned int)(*p - '0');
        p++;
    }
    return v;
}

int main(void)
{
    char result[1024];
    char run_val[64];
    unsigned int hammer_ms;
    long long frames_sent, rx_enqueued, rx_dispatched, work_posted, work_dispatched;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_kmod_cluster_fork_hammer (FC-P0.16 R3 same-CPU hammer) ===\n");

    CHECK(create_veth_pair(IF_A, IF_B) == 0, "create veth pair " IF_A "/" IF_B);
    CHECK(link_set_up(IF_A) == 0, IF_A " administratively up");
    CHECK(link_set_up(IF_B) == 0, IF_B " administratively up");

    hammer_ms = cmdline_hammer_ms();
    if (hammer_ms > 0) {
        snprintf(run_val, sizeof(run_val), IF_A ":" IF_B ":%u", hammer_ms);
        printf("  info ovmx_hammer_ms=%u on /proc/cmdline -- running the non-default duration\n",
               hammer_ms);
    } else {
        snprintf(run_val, sizeof(run_val), IF_A ":" IF_B);
        printf("  info no ovmx_hammer_ms= on /proc/cmdline -- the kernel-side default "
               "(3000ms) applies, as it does in the default per-PR battery\n");
    }

    CHECK(write_param(RUN_PARAM, run_val) == 0,
          "write vms_ktest_cluster_fork_hammer_run (executive present, sysfs knob exists, "
          "the write returning proves vms_cluster_fork_stop's join did NOT hang -- "
          "the genuine lockup detector)");
    CHECK(read_param(RESULT_PARAM, result, sizeof(result)) == 0,
          "read vms_ktest_cluster_fork_hammer_result");

    CHECK(result_ll(result, "DONE") == 1, "self-test ran to completion (no lockup, no hang)");
    CHECK(result_ll(result, "FORK_START") == 1,
          "vms_cluster_fork_start ran the REAL exec_rxlock_init/exec_cv_init/kthread path");
    CHECK(result_ll(result, "OPEN") == 1, "exec_lan_open succeeded on " IF_A);
    CHECK(result_ll(result, "L2_OPEN") == 1, "exec_l2_open succeeded on the peer " IF_B);

    frames_sent = result_ll(result, "FRAMES_SENT");
    CHECK(frames_sent > 0, "the receive-level flood kthread sent real 0x6007 frames");

    rx_enqueued = result_ll(result, "RX_ENQUEUED");
    CHECK(rx_enqueued > 0,
          "at least one flooded frame reached exec_lan_open's rx_cb -> cf_rx_deliver "
          "in Linux's receive softirq (CONTRACT RULE 14.1(a))");

    CHECK(result_ll(result, "POSTS_ACCEPTED") > 0,
          "the process-context poster kthread's cf_post() calls were accepted "
          "(CONTRACT RULE 14.1's process-context-poster row) on the SAME CPU as the flood");

    CHECK(result_ll(result, "DRAIN_OK") == 1,
          "cf_stats converged (dispatched >= enqueued/posted) within the 2s bound -- "
          "no lost wakeup");

    rx_dispatched = result_ll(result, "RX_DISPATCHED");
    CHECK(rx_dispatched == rx_enqueued,
          "every enqueued frame was dispatched exactly once (no frame stuck in the queue)");

    work_posted = result_ll(result, "WORK_POSTED");
    work_dispatched = result_ll(result, "WORK_DISPATCHED");
    CHECK(work_dispatched == work_posted,
          "every posted work item was dispatched exactly once (no lost wakeup, no stuck poster)");

    CHECK(result_ll(result, "WAITS") > 0,
          "the fork thread actually slept and was actually woken during the hammer "
          "(exec_cv_wait_rx really ran, not just exec_rxlock_acquire/release)");

    printf("  info elapsed=%lldms frames_sent=%lld rx_enqueued=%lld rx_dispatched=%lld "
           "work_posted=%lld work_dispatched=%lld drain_ms=%lld\n",
           result_ll(result, "ELAPSED_MS"), frames_sent, rx_enqueued, rx_dispatched,
           work_posted, work_dispatched, result_ll(result, "DRAIN_MS"));

    printf("=== test_kmod_cluster_fork_hammer: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
