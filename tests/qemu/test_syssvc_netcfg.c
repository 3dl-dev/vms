/*
 * test_syssvc_netcfg.c - network configuration and reserved-port binds are the
 * executive's, gated by VMS privilege (rd vms-137e).
 *
 * No OVMX process runs as substrate root, so none holds CAP_NET_ADMIN or
 * CAP_NET_BIND_SERVICE. TCP/IP's SET INTERFACE / SET ROUTE go through
 * VMS_IOCTL_NETCFG (OPER on the caller's PCB, as VMS requires for SET ROUTE) and a BG bind to a
 * port below 1024 needs SYSPRV, BYPASS or OPER; the executive then acts as the
 * kernel. This suite is SYSTEM (the harness boot task made it):
 *   - it reads and rewrites the loopback interface's flags through the
 *     executive (a no-op write: the flags it read);
 *   - it binds a reserved port;
 *   - a malformed request is SS$_BADPARAM.
 * An UNPRIVILEGED process it creates ([100,100], TMPMBX+NETMBX only) is
 * refused both SS$_NOPRIV, while its ephemeral-port bind still succeeds --
 * the refusals are not blanket.
 *
 * negctl: netcfg-privilege-unchecked
 * negctl: bg-bind-reserved-port-unchecked
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77
#define RESERVED_PORT 1000

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Bind a fresh BG socket to `port` on 127.0.0.1; the bind status. */
static uint32_t bind_port(uint16_t port)
{
    uint32_t chan = 0, unit = 0, st;
    char devnam[32];

    st = vms_kif_bg_create(&chan, &unit, devnam, sizeof devnam);
    if (!(st & 1))
        return st;
    st = vms_kif_bg_setmode(chan);
    if (st & 1)
        st = vms_kif_bg_bind(chan, AF_INET, htons(port), htonl(INADDR_LOOPBACK), NULL, NULL);
    (void)vms_kif_bg_dassgn(chan);
    return st;
}

struct child_report {
    uint32_t netcfg_st, reserved_st, ephemeral_st;
};

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_netcfg: network configuration and reserved ports need VMS privilege ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_netcfg: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    /* --- SYSTEM: granted ------------------------------------------------ */
    {
        struct ifreq ifr;
        int err = 0;
        uint32_t st;

        memset(&ifr, 0, sizeof ifr);
        strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
        st = vms_kif_netcfg(VMS_NETCFG_GET_IFFLAGS, &ifr, &err);
        CHECK(st == SS$_NORMAL && (ifr.ifr_flags & IFF_LOOPBACK),
              "SYSTEM reads the loopback interface's flags through the executive");
        if (st & 1) {
            st = vms_kif_netcfg(VMS_NETCFG_SET_IFFLAGS, &ifr, &err);
            if (!(st & 1))
                printf("  (SET_IFFLAGS: status %#x errno %d)\n", (unsigned)st, err);
            CHECK(st == SS$_NORMAL,
                  "SYSTEM writes them back through the executive (a configuration change it may make)");
        }
        CHECK(vms_kif_netcfg(99, &ifr, NULL) == SS$_BADPARAM,
              "a request the executive does not define is SS$_BADPARAM");
        CHECK(vms_kif_netcfg(VMS_NETCFG_GET_IFFLAGS, NULL, NULL) == SS$_BADPARAM,
              "a request with no substrate structure is SS$_BADPARAM");

        st = bind_port(RESERVED_PORT);
        if (!(st & 1))
            printf("  (reserved bind: status %#x)\n", (unsigned)st);
        CHECK(st == SS$_NORMAL, "SYSTEM binds a reserved port (below 1024)");
    }

    /* --- an unprivileged process: refused ------------------------------- */
    {
        int p[2];
        struct child_report rep;
        ssize_t n = -1;

        memset(&rep, 0, sizeof rep);
        if (pipe(p) != 0) { printf("  FAIL: pipe\n"); return 1; }
        uint32_t cst = 0;
        struct vms_procinfo me;
        memset(&me, 0, sizeof me);
        (void)vms_kif_getjpi_self(&me);
        pid_t c = ovmx_fork_detached_st(me.username, (100u << 16) | 100u,
                                        VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX, &cst);
        if (c == 0) {
            struct child_report r;
            struct ifreq ifr;
            close(p[0]);
            memset(&r, 0, sizeof r);
            memset(&ifr, 0, sizeof ifr);
            strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
            r.netcfg_st = vms_kif_netcfg(VMS_NETCFG_GET_IFFLAGS, &ifr, NULL);
            r.reserved_st = bind_port(RESERVED_PORT + 1);
            r.ephemeral_st = bind_port(0);
            (void)!write(p[1], &r, sizeof r);
            _exit(0);
        }
        close(p[1]);
        if (c > 0)
            n = read(p[0], &rep, sizeof rep);
        close(p[0]);
        while (c > 0 && waitpid(c, NULL, 0) < 0 && errno == EINTR)
            ;
        CHECK(c > 0 && n == (ssize_t)sizeof rep,
              "an unprivileged process ([100,100], TMPMBX+NETMBX) the suite created reported");
        /* negctl: netcfg-privilege-unchecked */
        CHECK(rep.netcfg_st == SS$_NOPRIV,
              "without OPER, network configuration is refused SS$_NOPRIV");
        /* negctl: bg-bind-reserved-port-unchecked */
        CHECK(rep.reserved_st == SS$_NOPRIV,
              "without SYSPRV, BYPASS or OPER, a reserved-port bind is refused SS$_NOPRIV");
        CHECK(rep.ephemeral_st == SS$_NORMAL,
              "the same process binds an ephemeral port -- the refusal is for reserved ports only");
    }

    printf("=== test_syssvc_netcfg: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
