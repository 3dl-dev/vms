/*
 * test_syssvc_ssh_server.c - a WRAPPED OpenSSH sshd binds, listens, AND accepts an
 * inbound connection over BGn: (rd vms-0cd, RUNG-3a: the sshd SERVER-transport
 * listener path). The inverse of test_syssvc_ssh_kex: there the CLIENT was wrapped
 * and sshd stock; here the SERVER is wrapped and a real host client connects in.
 *
 * WHAT THIS PROVES (RUNG-3a). The wrapped sshd's socket()/bind()/listen()/accept()
 * dispatch to the executive BGn: veneer (--wrap), so its listener is an
 * executive-resident socket bound to a REAL host port (the IP stack is the host's).
 * A real host-socket client connects to that port; the executive accepts the
 * inbound connection (a veneer handle) and sshd forks a per-connection child to
 * service it. So the wrapped sshd's LISTENER PATH -- socket, bind, listen, and the
 * accept of an inbound connection -- rides BGn:, with NO AF_UNIX socketpair.
 *
 * WHAT THIS DOES NOT YET PROVE (RUNG-3b, deliberately NOT asserted here). The full
 * authenticated session is NOT completed. Portable sshd hands the accepted
 * connection to sshd-session by dup2()'ing it onto stdin/stdout before execv() --
 * and a veneer HANDLE (>= OVMX_BGSOCK_BASE) is not a real fd, so that dup2 fails
 * EBADF and the preauth child exits. Completing the session needs the executive to
 * materialize an accepted BG channel as a REAL vms.ko-backed fd (dup2-able,
 * exec-surviving, its fops routing read/write to the executive socket) -- a focused
 * kernel-core capability tracked as RUNG-3b. This test asserts EXACTLY the listener
 * + accept that works today, honestly, and no more (CLAUDE.md Rule 9 / INV-6).
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
#define SRV_SSHD   "/ovmxsshsrv/sshd"   /* WRAPPED server (socket/bind/listen/accept over BGn:) */
#define SRV_SSHDCFG "/ovmxsshsrv/etc/sshd_config"
#define SSHD_LOG   "/tmp/ovmx_sshd.log" /* sshd's own -e stderr, captured for assertions */

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

/* One real-socket connect to 127.0.0.1:SSH_PORT. Returns 1 if it connected. */
static int host_connect_once(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    int ok;
    if (s < 0) return 0;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(SSH_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok = (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0);
    if (ok) usleep(50 * 1000);          /* let sshd accept + fork before we close */
    close(s);
    return ok;
}

/* Poll the port until the executive-bound listener accepts a real host connect
 * (the wrapped sshd is up + bound over BGn: to its real host port). */
static int wait_for_sshd(int tries)
{
    int i;
    for (i = 0; i < tries; i++) {
        if (host_connect_once()) return 1;
        usleep(200 * 1000);
    }
    return 0;
}

/* Slurp sshd's captured -e stderr log into buf. */
static size_t read_sshd_log(char *buf, size_t cap)
{
    FILE *f = fopen(SSHD_LOG, "r");
    size_t n;
    if (!f) { if (cap) buf[0] = '\0'; return 0; }
    n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return n;
}

int main(void)
{
    char log[8192];

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, watchdog);
    alarm(60);

    printf("=== test_syssvc_ssh_server (WRAPPED sshd: bind+listen+accept an inbound conn over BGn:) ===\n");

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

    /* ---- fork the WRAPPED sshd: its socket/bind/listen/accept ride BGn: ----
     * argv[0] MUST be the absolute path: sshd re-execs itself through argv[0] and
     * refuses ("sshd requires execution with an absolute path") otherwise. -D keeps
     * it a persistent listener; -e sends its log to stderr, which we capture to a
     * file so the test can ASSERT on sshd's own account of binding + accepting. */
    pid_t sd = fork();
    if (sd == 0) {
        char *av[] = { (char *)SRV_SSHD, "-D", "-e", "-f", (char *)SRV_SSHDCFG, NULL };
        int lf = open(SSHD_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); close(lf); }
        execv(SRV_SSHD, av);
        _exit(127);
    }

    int up = wait_for_sshd(50);
    CHECK(up, "a real host client's TCP connect completes against the wrapped sshd's "
              "executive-resident listener -- its socket/bind/listen rode BGn:");
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

    /* Drive one more inbound connection to make sshd accept + fork a child for it,
     * then let it log. (host_connect_once already connected inside wait_for_sshd,
     * but this makes the accept deterministic right before we read the log.) */
    (void)host_connect_once();
    usleep(600 * 1000);

    read_sshd_log(log, sizeof(log));

    CHECK(strstr(log, "Server listening on 127.0.0.1 port 2223") != NULL,
          "the wrapped sshd reports it bound + listened on 127.0.0.1:2223 -- over BGn:, "
          "not a host socket (its socket/bind/listen are --wrap'd to the executive)");

    /* sshd logs the inbound peer once it has accept()ed the connection and forked a
     * child to service it; "connection from 127.0.0.1" is sshd's own record that the
     * executive delivered the accepted BG channel to it. */
    CHECK(strstr(log, "connection from 127.0.0.1") != NULL,
          "the executive accepted the inbound connection over BGn: and sshd forked a "
          "per-connection child for it -- accept rides BGn: (full session handoff = RUNG-3b)");

    CHECK(!proc_has_afunix_socket(sd),
          "no AF_UNIX socket fd in the wrapped sshd process -- its listener + accepted "
          "connection are executive-resident veneer handles, no fabricated socketpair (vms-0cd / INV-6)");

    if (fail) {
        printf("  --- captured sshd log ---\n%s\n  --- end sshd log ---\n", log);
    }

    kill(sd, SIGTERM);
    waitpid(sd, NULL, 0);

    printf("=== test_syssvc_ssh_server: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
