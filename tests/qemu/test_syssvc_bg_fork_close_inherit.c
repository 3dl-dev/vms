/*
 * test_syssvc_bg_fork_close_inherit.c - a forked child inherits a BGn: connection
 * even when the parent CLOSES its copy right after the fork (rd vms-0cd; the
 * accept->fork->close forking-server race that #815 did not cover).
 *
 * THE RACE. #815 inherits a parent's BG channels at the child's REGISTRATION. A
 * classic forking server (sshd, inetd) accepts a connection, forks a child to
 * handle it, then CLOSES its own copy of the accepted connection -- all before the
 * child ever calls the executive. So a snapshot taken at the child's registration
 * finds the channel already $DASSGN'd (SS$_IVCHAN). This test reproduces exactly
 * that: the parent accepts an inbound connection over BGn:, forks+execs a child,
 * and IMMEDIATELY closes both the accepted channel and the listener -- then the
 * child (post-exec) materializes the inherited connection and round-trips bytes.
 *
 * WHAT IT PROVES. Eager fork-time inheritance (vms_bg_forkinherit.c) captures the
 * parent's BG channels AT FORK -- taking a shared ref on the host socket before the
 * parent can drop it -- so the child, given only the inherited handle NUMBER, gets
 * its own channel and materializes a real fd whose read()/write() reach the still-
 * alive executive socket BYTE-EXACT. Without the fork capture (see the
 * fork-inherit-disabled negctl) the child falls back to #815, finds the channel
 * gone, and fails -- which is the teeth on this test.
 *
 * Honest Rule-9 skip with no /dev/vms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "vms_kif.h"
#include "vms_bgsock.h"

#define EXIT_SKIP 77
#define MSG "OVMX_FORKCLOSE_OK"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_bg_fork_close_inherit timed out\n";
    (void)!write(1, m, sizeof(m) - 1);
    _exit(1);
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

static void bring_lo_up(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    if (s < 0) return;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        (void)ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
}

/* Host CLIENT peer (a real host socket, the "ssh client" analogue): connect to the
 * executive-bound listener, then echo one message back. */
static uint16_t g_port;
static void *host_client(void *arg)
{
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    char buf[128];
    ssize_t n;
    (void)arg;
    if (c < 0) return NULL;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    /* retry until the listener is up */
    for (int i = 0; i < 50; i++) {
        if (connect(c, (struct sockaddr *)&a, sizeof(a)) == 0) break;
        usleep(100 * 1000);
    }
    n = read(c, buf, sizeof(buf));       /* the server child's MSG */
    if (n > 0) (void)!write(c, buf, (size_t)n);   /* echo it back */
    usleep(100 * 1000);
    close(c);
    return NULL;
}

int main(int argc, char **argv)
{
    /* Child (post-exec): materialize the INHERITED accepted handle and round-trip. */
    if (argc == 2 && strncmp(argv[1], "--child-io=", 11) == 0) {
        int h = atoi(argv[1] + 11);
        int fd = ovmx_materialize_fd(h);   /* inherited channel -> real fd */
        char rb[64] = {0};
        ssize_t w, r;
        if (fd < 0 || fd >= OVMX_BGSOCK_BASE)
            return 2;                      /* no channel (race lost) or not a real fd */
        w = write(fd, MSG, sizeof(MSG));
        r = (w == (ssize_t)sizeof(MSG)) ? read(fd, rb, sizeof(MSG)) : -1;
        close(fd);
        return (r == (ssize_t)sizeof(MSG) && memcmp(rb, MSG, sizeof(MSG)) == 0) ? 0 : 1;
    }

    signal(SIGALRM, watchdog);
    alarm(30);
    printf("test_syssvc_bg_fork_close_inherit: forked child inherits a BGn: conn the parent CLOSED\n");

    if (!executive_present()) {
        int h = ovmx_socket(AF_INET, SOCK_STREAM, 0);
        CHECK(h < 0 && errno == ENODEV,
              "no executive: ovmx_socket() fails ENODEV, never a local socket");
        printf("=== test_syssvc_bg_fork_close_inherit: SKIPPED (no /dev/vms) ===\n");
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    bring_lo_up();

    /* Server over BGn:: socket + bind ephemeral + listen; learn the bound port. */
    int ls = ovmx_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in la;
    socklen_t sl = sizeof(la);
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    la.sin_port = 0;                       /* ephemeral */
    CHECK(ls >= 0, "ovmx_socket() returns a listener veneer handle");
    if (ls < 0 || ovmx_bind(ls, (struct sockaddr *)&la, sizeof(la)) != 0 ||
        ovmx_listen(ls, 4) != 0 ||
        ovmx_getsockname(ls, (struct sockaddr *)&la, &sl) != 0) {
        printf("  FAIL: BGn: server setup\n");
        printf("=== test_syssvc_bg_fork_close_inherit: %d passed, %d failed ===\n", pass, ++fail);
        return 1;
    }
    g_port = ntohs(la.sin_port);

    pthread_t th;
    pthread_create(&th, NULL, host_client, NULL);

    /* Accept the inbound connection over BGn: -> a veneer handle for it. */
    int h = ovmx_accept(ls, NULL, NULL);
    CHECK(h >= 0, "ovmx_accept() returns the accepted-connection veneer handle over BGn:");

    /* Fork+exec the child to service the connection, then -- like a real forking
     * server -- CLOSE our copies right away. The child must still inherit the
     * connection (fork-time capture took a shared ref before this close). */
    char harg[32];
    snprintf(harg, sizeof(harg), "--child-io=%d", h);
    pid_t c = fork();
    if (c == 0) {
        execl(argv[0], argv[0], harg, (char *)NULL);
        _exit(127);
    }
    ovmx_socket_close(h);      /* the accept->fork->CLOSE race: parent drops it now */
    ovmx_socket_close(ls);

    int cst = 0;
    waitpid(c, &cst, 0);
    /* negctl: fork-inherit-disabled */
    CHECK(WIFEXITED(cst) && WEXITSTATUS(cst) == 0,
          "the forked+exec'd child materialized the inherited connection and round-tripped BYTE-EXACT, "
          "though the parent CLOSED it right after the fork -- eager fork-time inheritance (vms-0cd)");

    pthread_join(th, NULL);
    printf("=== test_syssvc_bg_fork_close_inherit: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
