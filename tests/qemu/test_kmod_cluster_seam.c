/*
 * test_kmod_cluster_seam.c - the R3 substrate-contract test for FC-P0.2 (the
 * Linux binding of the cluster substrate seam, exec_kbackend.h SS14..SS18:
 * design docs/design-faithful-cluster-executive.md SS3.2.1/SS3.2.2, plan
 * docs/plan-faithful-cluster-executive.md FC-P0.2).
 *
 * WHAT THIS PROVES. exec_lan_*, exec_kthread_*, exec_timer_*, exec_time_now_vms
 * and exec_ticks_ms are called ONLY from src/kernel-core/vms_pe.c in the shipped
 * executive, and vms_pe.c has no ioctl surface yet (that is FC-P0.9/FC-P0.11).
 * So this suite drives the seam directly through a TEST-ONLY sysfs knob
 * (vms_ktest_cluster_seam_run/_teardown/_result, src/kernel/vms_module.c,
 * compiled ONLY under OVMX_KTEST_CLUSTER_SEAM -- never the bootable
 * executive, the same posture as vms_ktest_bdev_fault/test_kmod_errcnt.c) that
 * runs the REAL Linux binding end to end against a veth pair THIS program
 * creates via raw rtnetlink (no `ip` binary is staged in the initramfs):
 *
 *   - exec_lan_open registers rx_cb on interface A; a frame injected from
 *     interface B (over the already-real SS13 exec_l2_send) is delivered to
 *     rx_cb in Linux's receive softirq -- the veth-pair loopback the R3
 *     done-condition names.
 *   - exec_lan_xmit from A is captured on B via the already-real SS13
 *     exec_l2_recv. Interface A is left administratively DOWN by this
 *     harness (vms-fc-e51) -- exec_lan_open itself must bring it up, the
 *     same obligation SS13's exec_l2_open already carries, or this TX
 *     proof fails exactly as a booted node's HELLOs silently did (port
 *     reports up, dev_queue_xmit's soft NET_XMIT_* codes read as success,
 *     nothing crosses the still-down/noop-qdisc interface).
 *   - exec_lan_mc_add's join is left open so THIS process can independently
 *     confirm it in /proc/net/dev_mcast -- the exact table `ip maddr` reads
 *     -- before the teardown knob removes it.
 *   - exec_kthread_create/_should_stop/_stop: a real kthread runs, is
 *     observed to have iterated, and joins on stop.
 *   - exec_timer_init/_arm posts-and-wakes a completion (CONTRACT RULE 2).
 *   - exec_time_now_vms / exec_ticks_ms are observed strictly increasing
 *     across a real sleep.
 *
 * NEGATIVE CONTROL: under NEGATIVE_CONTROL=1 (tests/qemu/Dockerfile boots
 * without insmod'ing vms.ko) the sysfs parameter files do not exist, every
 * fopen below fails, and the suite fails honestly (no per-process fallback,
 * INV-6) rather than fabricating a result.
 */

#define _GNU_SOURCE      /* strcasestr */
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

#define RUN_PARAM       "/sys/module/vms/parameters/vms_ktest_cluster_seam_run"
#define TEARDOWN_PARAM  "/sys/module/vms/parameters/vms_ktest_cluster_seam_teardown"
#define RESULT_PARAM    "/sys/module/vms/parameters/vms_ktest_cluster_seam_result"
#define DEV_MCAST       "/proc/net/dev_mcast"

#define IF_A "vseam0"
#define IF_B "vseam1"
#define MC_MAC_TEXT "ab:00:04:01:00:2a"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* ---- minimal rtnetlink veth-pair creation (public UAPI, no `ip` binary
 * staged in the initramfs -- clean-room: linux/rtnetlink.h + linux/if_link.h
 * + linux/veth.h are the documented kernel interface, nothing copied from
 * iproute2). ---- */

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

/* One RTM_NEWLINK creating a veth pair "a"<->"b" in one message, mirroring
 * `ip link add <a> type veth peer name <b>`: IFLA_IFNAME=a, nested
 * IFLA_LINKINFO{IFLA_INFO_KIND="veth", IFLA_INFO_DATA{VETH_INFO_PEER=an
 * embedded ifinfomsg + IFLA_IFNAME=b}}. Returns 0 on success. */
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
        fprintf(stderr, "DEBUG create_veth_pair: sendto rc=%d errno=%d (%s)\n",
                rc, errno, strerror(errno));
        close(fd);
        return -1;
    }
    rc = (int)recv(fd, &ack, sizeof(ack), 0);
    close(fd);
    if (rc < (int)sizeof(struct nlmsghdr)) {
        fprintf(stderr, "DEBUG create_veth_pair: recv rc=%d errno=%d (%s)\n",
                rc, errno, strerror(errno));
        return -1;
    }
    if (ack.err.error != 0)
        fprintf(stderr, "DEBUG create_veth_pair: nlmsgerr.error=%d (%s)\n",
                ack.err.error, strerror(-ack.err.error));
    return ack.err.error == 0 ? 0 : -1;
}

/* RTM_SETLINK: bring an interface administratively UP by name (IFF_UP set
 * in both ifi_flags and ifi_change, the documented "modify only this bit"
 * shape rtnetlink link-flag updates use). */
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

/* ---- sysfs knob helpers ---- */

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

/* KEY=VAL token lookup over the space-separated result line; VAL may itself
 * contain no spaces (every field the kernel formats is numeric, a %02x MAC,
 * or a short identifier string). */
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

static int result_int(const char *buf, const char *key)
{
    char v[32];

    if (result_field(buf, key, v, sizeof(v)) != 0)
        return -1;
    return atoi(v);
}

/* /proc/net/dev_mcast is exactly the kernel table `ip maddr show` renders
 * (dev_mc_list via dev_mc_seq_show): "ifindex ifname refcnt gusers addr",
 * addr as unseparated lowercase hex. Confirms the R3 done-condition's
 * "multicast add visible in `ip maddr`" independent of the kernel's own
 * self-report. */
static int mcast_visible(const char *ifname, const char *mac_colon)
{
    FILE *f = fopen(DEV_MCAST, "r");
    char line[256];
    char mac_nodash[13];
    int i, j;

    if (!f)
        return 0;
    for (i = 0, j = 0; mac_colon[i] && j < 12; i++)
        if (mac_colon[i] != ':')
            mac_nodash[j++] = mac_colon[i];
    mac_nodash[j] = '\0';

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, ifname) && strcasestr(line, mac_nodash)) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

int main(void)
{
    char result[1024];

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_kmod_cluster_seam ===\n");

    CHECK(create_veth_pair(IF_A, IF_B) == 0, "create veth pair " IF_A "/" IF_B);
    /*
     * IF_A (the port side, exec_lan_open) is DELIBERATELY left
     * administratively DOWN here (vms-fc-e51): a harness that pre-brings it
     * up, as this test used to, hides the exact defect a booted node hit --
     * exec_lan_open reporting a port "up" on a down interface whose HELLOs
     * then silently drop at the (still noop) qdisc, dev_queue_xmit's soft
     * NET_XMIT_* codes reading as success. exec_lan_open itself must bring
     * IF_A up (mirroring SS13's exec_l2_open, exec_netdev_ensure_up); the
     * TX check below is where a re-regression would show up (a dropped
     * frame never reaches IF_B). Only IF_B -- the test's OWN peer socket,
     * never code under test -- is pre-brought-up here. */
    CHECK(link_set_up(IF_B) == 0, IF_B " administratively up");

    CHECK(write_param(RUN_PARAM, IF_A ":" IF_B) == 0,
          "write vms_ktest_cluster_seam_run (executive present, sysfs knob exists)");
    CHECK(read_param(RESULT_PARAM, result, sizeof(result)) == 0,
          "read vms_ktest_cluster_seam_result");

    CHECK(result_int(result, "DONE") == 1, "self-test ran to completion");
    CHECK(result_int(result, "OPEN") == 1, "exec_lan_open succeeded on " IF_A);
    CHECK(result_int(result, "HWADDR") == 1, "exec_lan_hwaddr reported a real MAC");
    CHECK(result_int(result, "MTU") == 1, "exec_lan_mtu reported a nonzero MTU");
    CHECK(result_int(result, "LINK") == 1, "exec_lan_link_up answered");
    CHECK(result_int(result, "LINK_UP") == 1,
          "veth carrier is up (IF_A brought up by exec_lan_open itself, IF_B by this harness)");
    CHECK(result_int(result, "MC_ADD") == 1, "exec_lan_mc_add joined the HELLO multicast group");

    CHECK(mcast_visible(IF_A, MC_MAC_TEXT),
          "multicast join is visible in /proc/net/dev_mcast (== `ip maddr`) while the port is open");

    CHECK(result_int(result, "RX") == 1,
          "a 0x6007 frame injected on " IF_B " reached exec_lan_open's rx_cb on " IF_A " (softirq delivery)");
    {
        char payload[64];
        result_field(result, "RX_PAYLOAD", payload, sizeof(payload));
        CHECK(strcmp(payload, "OVMXSEAMRX") == 0, "the received frame's payload is byte-exact");
    }

    CHECK(result_int(result, "TX") == 1,
          "exec_lan_xmit from " IF_A " was captured on the peer " IF_B);
    {
        char payload[64];
        result_field(result, "TX_PAYLOAD", payload, sizeof(payload));
        CHECK(strcmp(payload, "OVMXSEAMTX") == 0, "the transmitted frame's payload is byte-exact on the peer");
    }

    CHECK(result_int(result, "KTHREAD") == 1, "exec_kthread_create started and exec_kthread_stop joined a real kthread");
    CHECK(result_int(result, "KTHREAD_ITERS") > 0, "the kthread actually iterated before being stopped");

    CHECK(result_int(result, "TIMER") == 1, "exec_timer_arm posted-and-woke within 1s (CONTRACT RULE 2)");
    CHECK(result_int(result, "TIME_MONO") == 1, "exec_time_now_vms is strictly increasing across a real sleep");
    CHECK(result_int(result, "TICKS_MONO") == 1, "exec_ticks_ms is strictly increasing across a real sleep");

    CHECK(write_param(TEARDOWN_PARAM, "1") == 0, "write vms_ktest_cluster_seam_teardown");
    CHECK(read_param(RESULT_PARAM, result, sizeof(result)) == 0, "read post-teardown result");
    CHECK(result_int(result, "MC_DEL") == 1, "exec_lan_mc_del left the multicast group");
    CHECK(result_int(result, "CLOSE") == 1, "exec_lan_close ran with no crash");
    CHECK(!mcast_visible(IF_A, MC_MAC_TEXT),
          "the multicast join is gone from /proc/net/dev_mcast after teardown");

    printf("=== test_kmod_cluster_seam: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
