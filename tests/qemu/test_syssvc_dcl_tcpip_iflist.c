/*
 * test_syssvc_dcl_tcpip_iflist.c - TCPIP SHOW INTERFACE reads the EXECUTIVE'S
 * interface list (rd vms-99f), driven through the shipped DCL.EXE in the guest
 * against a real vms.ko.
 *
 * THE BUG THIS GUARDS. SHOW INTERFACE used to open a host AF_INET datagram
 * socket in the DCL process and issue SIOCGIF* ioctls over a /sys/class/net
 * scan -- host introspection dressed as TCP/IP Services. On a DECC$SHR-linked
 * DCL (whose socket() is an executive BGn: socket) it printed
 * %TCPIP-E-SOCKERR. The list now comes from the executive: DCL $ASSIGNs a BGn:
 * channel, creates its socket (IO$_SETMODE) and issues the IO$_SENSEMODE
 * interface-list request (VMS_IOCTL_BG_IFLIST); vms.ko walks the network stack
 * that socket lives in.
 *
 * HOW THE VALUES ARE CROSS-CHECKED. The test itself (not DCL) configures the
 * guest NIC with a distinctive address, mask and MTU through the substrate's
 * own ioctls, then reads the expected values back from INDEPENDENT kernel
 * sources -- /sys/class/net/<if>/{mtu,address} and a SIOCGIFADDR on its own
 * socket -- and requires SHOW INTERFACE's rows to equal them. The NIC's
 * original state is restored afterwards. The no-executive half (honest
 * %TCPIP-E-NOEXEC, SS$_NOSUCHDEV, no host listing) is
 * tests/dcl/test_tcpip_show.sh, which runs DCL.EXE with no /dev/vms.
 *
 * With no executive it exits EXIT_SKIP (77), the honest-skip code the
 * kernel-executive negative-control rig requires of every public-sys$ suite.
 * NEGATIVE CONTROL: tests/qemu/defects.d/bg-iflist-mask-zeroed.defect.
 *
 * TCPIP SHOW ROUTE (rd vms-6d6c) is proven here too: its rows come from the
 * executive's IO$_SENSEMODE route-list request (VMS_IOCTL_BG_ROUTES), checked
 * against a route the test adds and the kernel's /proc/net/route count.
 * NEGATIVE CONTROL: tests/qemu/defects.d/bg-routes-gw-zeroed.defect.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/route.h>         /* struct rtentry, RTF_* (the test adds a route) */
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"

#define NIC_ADDR "10.0.2.15"
#define NIC_MASK "255.255.255.0"
#define NIC_MTU  1432          /* distinctive: not the 1500 default */

/* The route the test adds (rd vms-6d6c): TEST-NET-2 via the slirp gateway. */
#define RT_NET  "198.51.100.0"
#define RT_MASK "255.255.255.0"
#define RT_GW   "10.0.2.2"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char *const dcl_env[] = { (char *)"PATH=/bin", NULL };

static int run_dcl(const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];
    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) return -1;
    fflush(NULL);
    pid_t pid = ovmx_fork_login();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, dcl_env);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (write(in_pipe[1], script, strlen(script)) < 0) { /* best effort */ }
    close(in_pipe[1]);
    size_t used = 0;
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

/* Read the first line of a sysfs attribute (independent kernel source). */
static int read_sys(const char *ifname, const char *attr, char *buf, size_t bufsz)
{
    char path[128];
    snprintf(path, sizeof path, "/sys/class/net/%s/%s", ifname, attr);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)bufsz, f)) { fclose(f); return -1; }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

/* Find SHOW INTERFACE's row for `vmsname`; fill its columns. */
struct row { char name[32], ip[32], mask[32], state[16]; int mtu; char hw[64]; };

static int find_row(const char *out, const char *vmsname, struct row *r)
{
    const char *p = out;
    memset(r, 0, sizeof *r);
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = '\0';
        if (sscanf(line, "%31s %31s %31s %d %15s", r->name, r->ip, r->mask,
                   &r->mtu, r->state) == 5 && strcmp(r->name, vmsname) == 0) {
            /* /FULL: the next line may carry the hardware address */
            if (eol) {
                const char *n = eol + 1;
                const char *k = strstr(n, "Hardware address: ");
                const char *neol = strchr(n, '\n');
                if (k && (!neol || k < neol))
                    sscanf(k + strlen("Hardware address: "), "%63s", r->hw);
            }
            return 0;
        }
        p = eol ? eol + 1 : NULL;
    }
    return -1;
}

static int count_rows(const char *out)
{
    int n = 0;
    const char *p = strstr(out, "Interface");
    if (!p) return -1;
    p = strchr(p, '\n');
    while (p && *++p) {
        const char *eol = strchr(p, '\n');
        char name[32], ip[32], mask[32], state[16]; int mtu;
        char line[256];
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = '\0';
        if (sscanf(line, "%31s %31s %31s %d %15s", name, ip, mask, &mtu, state) == 5)
            n++;
        p = eol;
    }
    return n;
}

/* The kernel's own count of usable main-table routes (independent source:
 * /proc/net/route, read by the TEST, never by DCL). */
static int count_proc_routes(void)
{
    FILE *f = fopen("/proc/net/route", "r");
    char line[256];
    int n = 0;
    if (!f) return -1;
    if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }
    while (fgets(line, sizeof line, f)) {
        char ifn[32]; unsigned long dst, gw; unsigned int flags;
        if (sscanf(line, "%31s %lx %lx %x", ifn, &dst, &gw, &flags) == 4 && (flags & RTF_UP))
            n++;
    }
    fclose(f);
    return n;
}

/* SHOW ROUTE rows: "<dest> <gw> <mask> <iface>" after the Destination header. */
static int route_rows(const char *out, const char *d, const char *g,
                      const char *m, const char *ifn)
{
    int n = 0, hit = 0;
    const char *p = strstr(out, "Destination");
    if (!p) return -1;
    p = strchr(p, '\n');
    while (p && *++p) {
        const char *eol = strchr(p, '\n');
        char line[256], c1[32], c2[32], c3[32], c4[32];
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = '\0';
        if (sscanf(line, "%31s %31s %31s %31s", c1, c2, c3, c4) == 4 &&
            (isdigit((unsigned char)c1[0]) || !strcmp(c1, "default"))) {
            n++;
            if (d && !strcmp(c1, d) && !strcmp(c2, g) && !strcmp(c3, m) && !strcmp(c4, ifn))
                hit = 1;
        }
        p = eol;
    }
    return d ? hit : n;
}
static int count_route_rows(const char *out) { return route_rows(out, NULL, NULL, NULL, NULL); }
static int has_route_row(const char *out, const char *d, const char *g,
                         const char *m, const char *ifn)
{
    return route_rows(out, d, g, m, ifn) == 1;
}

static int set_ifaddr(int s, const char *ifn, unsigned long req, const char *dotted)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, ifn, IFNAMSIZ - 1);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, dotted, &sin->sin_addr);
    return ioctl(s, req, &ifr);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_tcpip_iflist (TCPIP SHOW INTERFACE reads the executive's interface list) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_tcpip_iflist: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    /* ---- the guest's interfaces, independently: /sys/class/net ---- */
    char nic[IFNAMSIZ] = "";
    int sys_count = 0;
    DIR *d = opendir("/sys/class/net");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            sys_count++;
            char type[16];
            if (!nic[0] && strcmp(de->d_name, "lo") != 0 &&
                read_sys(de->d_name, "type", type, sizeof type) == 0 &&
                strcmp(type, "1") == 0 &&                       /* ARPHRD_ETHER */
                (!strncmp(de->d_name, "eth", 3) || !strncmp(de->d_name, "ens", 3) ||
                 !strncmp(de->d_name, "enp", 3)))
                strncpy(nic, de->d_name, IFNAMSIZ - 1);
        }
        closedir(d);
    }
    CHECK(sys_count > 0 && sys_count <= 16, "the guest has 1..16 interfaces in /sys/class/net");
    CHECK(nic[0] != '\0', "the guest has a virtio Ethernet NIC (run_tests.sh's -device virtio-net-pci)");
    if (!nic[0]) {
        printf("=== test_syssvc_dcl_tcpip_iflist: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }
    printf("  guest NIC: %s (maps to SE0)\n", nic);

    /* ---- configure lo + the NIC through the substrate (the TEST's doing) ---- */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(s >= 0, "test-side substrate socket for configuring/reading the NIC");
    if (s < 0) return 1;
    struct ifreq ifr;
    short nic_flags0 = 0;
    int nic_mtu0 = 1500;
    int had_addr = 0;
    char addr0[INET_ADDRSTRLEN] = "", mask0[INET_ADDRSTRLEN] = "";

    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        (void)ioctl(s, SIOCSIFFLAGS, &ifr);
    }

    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, nic, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) nic_flags0 = ifr.ifr_flags;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, nic, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFMTU, &ifr) == 0) nic_mtu0 = ifr.ifr_mtu;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, nic, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        had_addr = 1;
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, addr0, sizeof addr0);
        memset(&ifr, 0, sizeof ifr);
        strncpy(ifr.ifr_name, nic, IFNAMSIZ - 1);
        if (ioctl(s, SIOCGIFNETMASK, &ifr) == 0)
            inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr, mask0, sizeof mask0);
    }

    int cfg_ok = set_ifaddr(s, nic, SIOCSIFADDR, NIC_ADDR) == 0 &&
                 set_ifaddr(s, nic, SIOCSIFNETMASK, NIC_MASK) == 0;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, nic, IFNAMSIZ - 1);
    ifr.ifr_mtu = NIC_MTU;
    cfg_ok = cfg_ok && ioctl(s, SIOCSIFMTU, &ifr) == 0;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, nic, IFNAMSIZ - 1);
    ifr.ifr_flags = nic_flags0 | IFF_UP | IFF_RUNNING;
    cfg_ok = cfg_ok && ioctl(s, SIOCSIFFLAGS, &ifr) == 0;
    CHECK(cfg_ok, "test configured the NIC " NIC_ADDR "/" NIC_MASK " MTU 1432 up (substrate ioctls)");

    /* Independent expected values */
    char lo_mtu[32] = "", nic_mtu[32] = "", nic_hw[64] = "", nic_ip[INET_ADDRSTRLEN] = "";
    read_sys("lo", "mtu", lo_mtu, sizeof lo_mtu);
    read_sys(nic, "mtu", nic_mtu, sizeof nic_mtu);
    read_sys(nic, "address", nic_hw, sizeof nic_hw);
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, nic, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, nic_ip, sizeof nic_ip);
    for (char *c = nic_hw; *c; c++) { *c = (char)toupper((unsigned char)*c); if (*c == ':') *c = '-'; }
    printf("  independent: lo mtu=%s  %s mtu=%s ip=%s hw=%s\n", lo_mtu, nic, nic_mtu, nic_ip, nic_hw);

    /* ---- the executive request directly: no socket yet -> SS$_IVCHAN ---- */
    {
        uint32_t chan = 0, cnt = 99;
        struct vms_bg_ifent ent[VMS_BG_IFLIST_MAX];
        uint32_t st = vms_kif_bg_create(&chan, NULL, NULL, 0);
        CHECK(st & 1, "$ASSIGN TCPIP$DEVICE: (vms_kif_bg_create) succeeds");
        st = vms_kif_bg_iflist(chan, ent, VMS_BG_IFLIST_MAX, &cnt);
        CHECK(st == SS$_IVCHAN && cnt == 0,
              "the interface list on a channel with no socket (no IO$_SETMODE) is SS$_IVCHAN, nothing returned");
        (void)vms_kif_bg_dassgn(chan);
    }

    /* ---- SHOW INTERFACE through DCL ---- */
    static char out[32768];
    CHECK(run_dcl("TCPIP SHOW INTERFACE/FULL\nSHOW SYMBOL $STATUS\n", out, sizeof out) == 0,
          "DCL ran TCPIP SHOW INTERFACE/FULL");
    printf("  ---- SHOW INTERFACE ----\n%s  ---- end ----\n", out);

    CHECK(strstr(out, "SOCKERR") == NULL && strstr(out, "NOEXEC") == NULL,
          "no %TCPIP-E-SOCKERR / NOEXEC: the list came from the executive");
    CHECK(strstr(out, "$STATUS == \"%X00000001\"") != NULL, "SHOW INTERFACE exits SS$_NORMAL");
    CHECK(count_rows(out) == sys_count,
          "one row per interface the guest kernel has (/sys/class/net count)");

    struct row lo, se;
    int have_lo = find_row(out, "LO0", &lo) == 0;
    CHECK(have_lo, "LO0 (the loopback) is listed");
    CHECK(have_lo && strcmp(lo.ip, "127.0.0.1") == 0, "LO0 address is 127.0.0.1");
    /* negctl: bg-iflist-mask-zeroed */
    CHECK(have_lo && strcmp(lo.mask, "255.0.0.0") == 0, "LO0 netmask is 255.0.0.0");
    CHECK(have_lo && lo_mtu[0] && lo.mtu == atoi(lo_mtu), "LO0 MTU equals /sys/class/net/lo/mtu");
    CHECK(have_lo && strcmp(lo.state, "Up") == 0, "LO0 state is Up");

    int have_se = find_row(out, "SE0", &se) == 0;
    CHECK(have_se, "SE0 (the guest NIC) is listed under its VMS name");
    CHECK(strstr(out, nic) == NULL, "the substrate NIC name is not shown");
    CHECK(have_se && nic_ip[0] && strcmp(se.ip, nic_ip) == 0 && strcmp(se.ip, NIC_ADDR) == 0,
          "SE0 address equals the NIC's live address (" NIC_ADDR ", read back by SIOCGIFADDR)");
    /* negctl: bg-iflist-mask-zeroed */
    CHECK(have_se && strcmp(se.mask, NIC_MASK) == 0, "SE0 netmask is 255.255.255.0");
    CHECK(have_se && nic_mtu[0] && se.mtu == atoi(nic_mtu) && se.mtu == NIC_MTU,
          "SE0 MTU equals /sys/class/net/<nic>/mtu (1432, not a default)");
    CHECK(have_se && strcmp(se.state, "Up") == 0, "SE0 state is Up");
    CHECK(have_se && nic_hw[0] && strcmp(se.hw, nic_hw) == 0,
          "SE0 /FULL hardware address equals /sys/class/net/<nic>/address");

    /* ==== TCPIP SHOW ROUTE reads the EXECUTIVE'S routing table (rd vms-6d6c) ====
     * The test adds a distinctive gateway route through the substrate
     * (SIOCADDRT, the TEST's doing), then requires SHOW ROUTE -- whose rows
     * come from the IO$_SENSEMODE route-list request, never /proc/net/route in
     * DCL -- to show it and the NIC's on-link route on SE0, and to carry exactly
     * as many routes as the kernel's own /proc/net/route (an independent
     * source, read here by the test). */
    {
        uint32_t chan = 0, cnt = 99;
        struct vms_bg_rtent rent[VMS_BG_ROUTES_MAX];
        uint32_t st = vms_kif_bg_create(&chan, NULL, NULL, 0);
        CHECK(st & 1, "route list: $ASSIGN TCPIP$DEVICE: succeeds");
        st = vms_kif_bg_routes(chan, rent, VMS_BG_ROUTES_MAX, &cnt);
        CHECK(st == SS$_IVCHAN && cnt == 0,
              "the route list on a channel with no socket (no IO$_SETMODE) is SS$_IVCHAN, nothing returned");
        (void)vms_kif_bg_dassgn(chan);
    }

    struct rtentry rte;
    memset(&rte, 0, sizeof rte);
    ((struct sockaddr_in *)&rte.rt_dst)->sin_family = AF_INET;
    inet_pton(AF_INET, RT_NET, &((struct sockaddr_in *)&rte.rt_dst)->sin_addr);
    ((struct sockaddr_in *)&rte.rt_genmask)->sin_family = AF_INET;
    inet_pton(AF_INET, RT_MASK, &((struct sockaddr_in *)&rte.rt_genmask)->sin_addr);
    ((struct sockaddr_in *)&rte.rt_gateway)->sin_family = AF_INET;
    inet_pton(AF_INET, RT_GW, &((struct sockaddr_in *)&rte.rt_gateway)->sin_addr);
    rte.rt_flags = RTF_UP | RTF_GATEWAY;
    rte.rt_dev = nic;
    int rt_added = ioctl(s, SIOCADDRT, &rte) == 0;
    CHECK(rt_added, "test added the route " RT_NET "/" RT_MASK " via " RT_GW " (substrate SIOCADDRT)");

    int proc_routes = count_proc_routes();
    static char rout[16384];
    CHECK(run_dcl("TCPIP SHOW ROUTE\nSHOW SYMBOL $STATUS\n", rout, sizeof rout) == 0,
          "DCL ran TCPIP SHOW ROUTE");
    printf("  ---- SHOW ROUTE ----\n%s  ---- end ----\n", rout);
    CHECK(strstr(rout, "NOEXEC") == NULL && strstr(rout, "NOROUTE") == NULL,
          "no %TCPIP-E-NOEXEC / NOROUTE: the routing table came from the executive");
    CHECK(strstr(rout, "$STATUS == \"%X00000001\"") != NULL, "SHOW ROUTE exits SS$_NORMAL");
    /* negctl: bg-routes-gw-zeroed */
    CHECK(has_route_row(rout, RT_NET, RT_GW, RT_MASK, "SE0"),
          "SHOW ROUTE: " RT_NET " via " RT_GW " mask " RT_MASK " on SE0 (the gateway the test added)");
    CHECK(has_route_row(rout, "10.0.2.0", "*", NIC_MASK, "SE0"),
          "SHOW ROUTE: the NIC's on-link route 10.0.2.0 * 255.255.255.0 on SE0");
    CHECK(proc_routes > 0 && count_route_rows(rout) == proc_routes,
          "one row per usable route in the kernel's main table (/proc/net/route count)");
    CHECK(strstr(rout, nic) == NULL, "the substrate NIC name is not shown by SHOW ROUTE");

    if (rt_added)
        (void)ioctl(s, SIOCDELRT, &rte);

    /* ---- restore the NIC ---- */
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, nic, IFNAMSIZ - 1);
    ifr.ifr_mtu = nic_mtu0;
    (void)ioctl(s, SIOCSIFMTU, &ifr);
    if (had_addr) {
        (void)set_ifaddr(s, nic, SIOCSIFADDR, addr0);
        if (mask0[0]) (void)set_ifaddr(s, nic, SIOCSIFNETMASK, mask0);
    } else {
        (void)set_ifaddr(s, nic, SIOCSIFADDR, "0.0.0.0");   /* Linux: removes it */
    }
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, nic, IFNAMSIZ - 1);
    ifr.ifr_flags = nic_flags0;
    (void)ioctl(s, SIOCSIFFLAGS, &ifr);
    close(s);

    printf("=== test_syssvc_dcl_tcpip_iflist: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
