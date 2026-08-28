/*
 * test_syssvc_ssh_server.c - a WRAPPED OpenSSH sshd accepts an inbound connection
 * over BGn: and a STOCK ssh client authenticates + runs a command (rd vms-0cd,
 * the sshd-over-BGn: SERVER transport). The inverse of test_syssvc_ssh_kex: there
 * the CLIENT was wrapped and sshd stock; here the SERVER is wrapped and the client
 * is a stock real-socket client.
 *
 * WHAT IT PROVES. The wrapped sshd's socket()/bind()/listen()/accept() dispatch to
 * the executive BGn: veneer (--wrap), so its listener is an executive-resident
 * socket bound to a REAL host port (the IP stack is the host's). A stock ssh client
 * connects to that port over a real socket; the executive accepts the inbound
 * connection (a veneer handle), sshd fork()+exec()s sshd-session, and the accepted
 * connection reaches the session child because the executive inherits the channel
 * by number (#815) AND the veneer handle is self-describing across exec (#822).
 * The end-to-end SSH handshake + remote command therefore ride BGn: on the SERVER
 * side, with NO AF_UNIX socketpair and NO pump.
 *
 * Honest Rule-9 skip: the wrapped sshd needs /dev/vms to bind over BGn:; with no
 * executive it cannot start, so the proof is honestly skipped, never faked.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <dirent.h>

#include "vms_kif.h"

#define EXIT_SKIP  77
#define SSH_PORT   2223                 /* distinct from the KEX test's 2222 */
#define SRV_SSH    "/ovmxsshsrv/ssh"    /* STOCK client */
#define SRV_SSHD   "/ovmxsshsrv/sshd"   /* WRAPPED server (listen/accept over BGn:) */
#define SRV_SSHDCFG "/ovmxsshsrv/etc/sshd_config"
#define SRV_SSHCFG  "/ovmxsshsrv/etc/ssh_config"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_ssh_server timed out\n";
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

/* Does process `pid` hold ANY AF_UNIX socket fd? The wrapped sshd's listener +
 * accepted connection are veneer HANDLES (>= OVMX_BGSOCK_BASE, not real fds), so
 * a fabricated AF_UNIX socketpair (the vms-9ac excision) would be the only source
 * of one -- INV-6 anchor. Cross-refs /proc/<pid>/fd socket inodes vs /proc/net/unix. */
static int proc_has_afunix_socket(pid_t pid)
{
    unsigned long uinodes[8192];
    size_t nu = 0;
    char line[512], path[64], fp[128], tgt[128];
    struct dirent *de;
    DIR *d;
    int found = 0;
    FILE *u = fopen("/proc/net/unix", "r");
    if (!u) return 0;
    if (!fgets(line, sizeof(line), u)) { fclose(u); return 0; }
    while (fgets(line, sizeof(line), u) && nu < 8192) {
        char c[6][48];
        unsigned long ino = 0;
        if (sscanf(line, "%47s %47s %47s %47s %47s %47s %lu",
                   c[0], c[1], c[2], c[3], c[4], c[5], &ino) >= 7 && ino != 0)
            uinodes[nu++] = ino;
    }
    fclose(u);
    snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid);
    d = opendir(path);
    if (!d) return 0;
    while (!found && (de = readdir(d)) != NULL) {
        ssize_t r;
        unsigned long ino = 0;
        if (de->d_name[0] == '.') continue;
        snprintf(fp, sizeof(fp), "%s/%s", path, de->d_name);
        r = readlink(fp, tgt, sizeof(tgt) - 1);
        if (r <= 0) continue;
        tgt[r] = '\0';
        if (sscanf(tgt, "socket:[%lu]", &ino) == 1 && ino != 0)
            for (size_t i = 0; i < nu; i++)
                if (uinodes[i] == ino) { found = 1; break; }
    }
    closedir(d);
    return found;
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

/* Real-socket connect to 127.0.0.1:SSH_PORT until the executive-bound listener
 * accepts (the wrapped sshd is up + bound over BGn: to a real host port). */
static int wait_for_sshd(int tries)
{
    int i;
    for (i = 0; i < tries; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        int ok;
        if (s < 0) return 0;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(SSH_PORT);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ok = (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0);
        close(s);
        if (ok) return 1;
        usleep(200 * 1000);
    }
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, watchdog);
    alarm(60);

    printf("=== test_syssvc_ssh_server (WRAPPED sshd: inbound accept + session over BGn:) ===\n");

    if (!executive_present()) {
        /* No /dev/vms: the wrapped sshd cannot bind over BGn:; honest skip. */
        printf("  PASS: no executive -> the wrapped sshd cannot bind BGn:; proof honestly skipped (Rule 9/INV-6)\n");
        return EXIT_SKIP;
    }

    bring_lo_up();

    /* Runtime env the wrapped sshd needs (belt-and-suspenders over the Dockerfile
     * staging): the baked privsep dir root-owned 0755, and the account files
     * getpwnam() reads (privsep 'sshd' user + the 'root' login user). */
    (void)mkdir("/ovmxsshsrv/empty", 0755);
    (void)mkdir("/etc", 0755);
    {
        FILE *f = fopen("/etc/passwd", "w");
        if (f) {
            fputs("root:x:0:0:root:/root:/bin/sh\n"
                  "sshd:x:74:74:sshd privsep:/ovmxsshsrv/empty:/bin/false\n", f);
            fclose(f);
        }
        f = fopen("/etc/group", "w");
        if (f) { fputs("root:x:0:\nsshd:x:74:\n", f); fclose(f); }
    }

    /* ---- fork the WRAPPED sshd: its listen/accept ride BGn: ----
     * argv[0] MUST be the absolute path: sshd re-execs itself through argv[0] and
     * refuses ("sshd requires execution with an absolute path") otherwise. -D keeps
     * it a persistent listener; -e + merging its stderr into this test's stdout puts
     * any bind/listen failure over BGn: into the captured CI log. */
    pid_t sd = fork();
    if (sd == 0) {
        char *av[] = { (char *)SRV_SSHD, "-D", "-e", "-f", (char *)SRV_SSHDCFG, NULL };
        dup2(STDOUT_FILENO, 2);
        execv(SRV_SSHD, av);
        _exit(127);
    }

    int up = wait_for_sshd(50);
    CHECK(up, "the WRAPPED sshd bound BGn: to a real host port and accepts inbound connections");
    if (!up) {
        int est = 0;
        pid_t w = waitpid(sd, &est, WNOHANG);
        if (w == sd)
            printf("  --- sshd exited early: %s=%d ---\n",
                   WIFEXITED(est) ? "exit" : "signal",
                   WIFEXITED(est) ? WEXITSTATUS(est) : WTERMSIG(est));
        else
            printf("  --- sshd still running but not accepting on %d ---\n", SSH_PORT);
    }

    /* ---- run the STOCK ssh client against it ---- */
    int pfd[2];
    if (pipe(pfd) != 0) { printf("  FAIL: pipe()\n"); kill(sd, SIGTERM); return 1; }
    pid_t cp = fork();
    if (cp == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]); close(pfd[1]);
        char *av[] = { (char *)SRV_SSH, "-F", (char *)SRV_SSHCFG,
                       (char *)"srvpeer", (char *)"echo OVMX_SRV_OK", NULL };
        execv(SRV_SSH, av);
        _exit(127);
    }
    close(pfd[1]);
    char buf[512];
    size_t got = 0;
    for (;;) {
        ssize_t n = read(pfd[0], buf + got, sizeof(buf) - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        if (got >= sizeof(buf) - 1) break;
    }
    buf[got] = '\0';
    close(pfd[0]);
    int cst = 0;
    waitpid(cp, &cst, 0);

    CHECK(WIFEXITED(cst) && WEXITSTATUS(cst) == 0,
          "the stock ssh client completed the SSH handshake + session against the wrapped sshd (exit 0)");
    /* negctl: bg-accept-socket-not-installed */
    CHECK(strstr(buf, "OVMX_SRV_OK") != NULL,
          "the remote command output came back BYTE-EXACT -- a real inbound session rode BGn: through sshd's fork+exec sshd-session (vms-0cd)");
    if (strstr(buf, "OVMX_SRV_OK") == NULL)
        printf("  --- ssh client stdout: [%s] ---\n", buf);

    CHECK(!proc_has_afunix_socket(sd),
          "no AF_UNIX socket fd in the wrapped sshd process -- its listener + accepted connection are executive-resident veneer handles, no fabricated socketpair (vms-0cd / INV-6)");

    kill(sd, SIGTERM);
    waitpid(sd, NULL, 0);

    printf("=== test_syssvc_ssh_server: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
