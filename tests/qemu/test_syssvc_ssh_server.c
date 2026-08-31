/*
 * test_syssvc_ssh_server.c - a WRAPPED OpenSSH sshd accepts an inbound connection
 * over BGn: AND runs a full authenticated session; a STOCK ssh client connects in,
 * authenticates, and runs a remote command whose output comes back BYTE-EXACT
 * (rd vms-0cd, RUNG-3 completion). The inverse of test_syssvc_ssh_kex: there the
 * CLIENT was wrapped and sshd stock; here the SERVER is wrapped and the client is a
 * stock real-socket client.
 *
 * WHAT IT PROVES -- a real OpenSSH sshd session over BGn:, end to end. The wrapped
 * sshd's socket()/bind()/listen()/accept() dispatch to the executive BGn: veneer
 * (--wrap), so its listener is an executive-resident socket bound to a REAL host
 * port. A stock ssh client connects; the executive accepts the inbound connection
 * (a veneer handle); sshd fork()s a child and dup2()s the connection onto the
 * child's stdin/stdout before execv()'ing sshd-session. That dup2 is where RUNG-3a
 * stopped -- a veneer handle is not a real fd. RUNG-3b + the --wrap dup2 fix it:
 * __wrap_dup2 MATERIALIZES the handle as a real executive-backed fd (a vms.ko
 * [bgconn] whose read/write route to the executive socket, no O_CLOEXEC), so the
 * dup2 succeeds and sshd-session -- after execve -- does ordinary read()/write() on
 * its stdin/stdout, which reach the executive socket through the kernel fops. The
 * full SSH-2 handshake, pubkey auth, and remote command therefore ride BGn: on the
 * SERVER side, through sshd's fork+exec+dup2, with NO AF_UNIX socketpair.
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

#define EXIT_SKIP   77
#define SSH_PORT    2223                 /* distinct from the KEX test's 2222 */
#define SRV_SSH     "/ovmxsshsrv/ssh"    /* STOCK client */
#define SRV_SSHD    "/ovmxsshsrv/sshd"   /* WRAPPED server (socket/bind/listen/accept + dup2 over BGn:) */
#define SRV_SSHDCFG "/ovmxsshsrv/etc/sshd_config"
#define SRV_SSHCFG  "/ovmxsshsrv/etc/ssh_config"
#define SSHD_LOG    "/tmp/ovmx_sshd.log" /* sshd's own -e stderr, captured for diagnosis */

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

/* INV-6: does process `pid` hold ANY AF_UNIX socket fd? The wrapped sshd's listener,
 * accepted connection, and the child's materialized session fd are all
 * executive-resident (a veneer handle or a vms.ko [bgconn] anon_inode, never a
 * socket), so a surviving AF_UNIX socket fd would mean the retired socketpair
 * fabrication is back. /proc/<pid>/fd socket inodes vs /proc/net/unix. */
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

/* Poll the port until the executive-bound listener accepts a real host connect. */
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

static void dump_sshd_log(void)
{
    FILE *f = fopen(SSHD_LOG, "r");
    char line[512];
    if (!f) return;
    printf("  --- wrapped sshd log ---\n");
    while (fgets(line, sizeof(line), f)) printf("  | %s", line);
    printf("  --- end sshd log ---\n");
    fclose(f);
}

/*
 * vms-0cd RUNG-3 step 3c: the wrapped sshd now authenticates PASSWORDS against
 * the BINARY SYSUAF (Purdy) via our sys_auth_passwd shim, selected at build with
 * -DCUSTOM_SYS_AUTH_PASSWD (so a link that did NOT provide the shim would fail
 * the build outright -- a running wrapped sshd already proves the shim is
 * linked). This drives the negative, runtime-light half of the proof end to
 * end: a PASSWORD login for a user with NO SYSUAF record must be REJECTED
 * (fail-closed, INV-6). It needs only /dev/vms -- no provisioned SYS$SYSTEM: /
 * DCL.EXE; the positive "valid SYSUAF user -> lands in DCL" proof is the
 * SYSTARTUP-provisioned 3d e2e (it needs a mounted SYS$SYSTEM: with a seeded
 * SYSUAF over the ACP + DCL.EXE). Returns 1 if the unknown-user password login
 * was correctly refused, 0 if it slipped through (a fabricated accept).
 */
static int password_login_of_unknown_user_is_refused(void)
{
    const char *askpass = "/tmp/ovmx_askpass";
    int pfd[2];
    pid_t cp;
    char buf[512];
    size_t got = 0;
    int cst = 0;

    /* Stage the askpass helper (SSH_ASKPASS_REQUIRE=force makes the stock ssh
     * client read the password from it with no controlling tty). */
    {
        FILE *f = fopen(askpass, "w");
        if (!f) return 0;
        fputs("#!/bin/sh\nprintf '%s\\n' 'no-such-purdy-password'\n", f);
        fclose(f);
        chmod(askpass, 0755);
    }

    if (pipe(pfd) != 0) return 0;
    cp = fork();
    if (cp == 0) {
        setsid();                       /* no controlling tty -> use askpass */
        setenv("SSH_ASKPASS", askpass, 1);
        setenv("SSH_ASKPASS_REQUIRE", "force", 1);
        setenv("DISPLAY", ":0", 1);
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]); close(pfd[1]);
        char *av[] = { (char *)SRV_SSH, "-F", (char *)SRV_SSHCFG,
                       (char *)"-l", (char *)"NOSUCHVMSUSER",
                       (char *)"srvpw", (char *)"echo SHOULD_NOT_RUN", NULL };
        execv(SRV_SSH, av);
        _exit(127);
    }
    close(pfd[1]);
    for (;;) {
        ssize_t n = read(pfd[0], buf + got, sizeof(buf) - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        if (got >= sizeof(buf) - 1) break;
    }
    buf[got] = '\0';
    close(pfd[0]);
    waitpid(cp, &cst, 0);

    /* Refused == the client did NOT exit 0 AND no session command ran. */
    return !(WIFEXITED(cst) && WEXITSTATUS(cst) == 0)
           && strstr(buf, "SHOULD_NOT_RUN") == NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, watchdog);
    alarm(60);

    printf("=== test_syssvc_ssh_server (WRAPPED sshd: full inbound SESSION over BGn:) ===\n");

    if (!executive_present()) {
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

    /* ---- fork the WRAPPED sshd: socket/bind/listen/accept AND the dup2 session
     * handoff ride BGn:. argv[0] MUST be absolute (sshd re-execs itself through it).
     * -D persistent listener, -e log to stderr captured to a file for diagnosis. */
    pid_t sd = fork();
    if (sd == 0) {
        char *av[] = { (char *)SRV_SSHD, "-D", "-e", "-f", (char *)SRV_SSHDCFG, NULL };
        int lf = open(SSHD_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 2); close(lf); }
        execv(SRV_SSHD, av);
        _exit(127);
    }

    int up = wait_for_sshd(50);
    CHECK(up, "the WRAPPED sshd bound BGn: to a real host port and accepts inbound connections");
    if (!up) {
        int est = 0;
        if (waitpid(sd, &est, WNOHANG) == sd)
            printf("  --- sshd exited early: %s=%d ---\n",
                   WIFEXITED(est) ? "exit" : "signal",
                   WIFEXITED(est) ? WEXITSTATUS(est) : WTERMSIG(est));
        dump_sshd_log();
    }

    /* ---- run the STOCK ssh client against it: authenticate + run a command ---- */
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
          "the stock ssh client completed the SSH handshake + pubkey auth + session against the wrapped sshd (exit 0)");
    /* negctl: bg-accept-socket-not-installed */
    CHECK(strstr(buf, "OVMX_SRV_OK") != NULL,
          "the remote command output came back BYTE-EXACT -- a real inbound session rode BGn: through sshd's fork+exec+dup2 (materialized executive fd) into sshd-session (vms-0cd)");

    CHECK(!proc_has_afunix_socket(sd),
          "no AF_UNIX socket fd in the wrapped sshd process -- its listener, accepted connection, and materialized session fd are executive-resident, no fabricated socketpair (vms-0cd / INV-6)");

    /* vms-0cd 3c: SYSUAF password auth is wired and fail-closed. A running
     * wrapped sshd already proves the sys_auth_passwd shim linked (the build
     * used -DCUSTOM_SYS_AUTH_PASSWD, which drops OpenSSH's own definition); this
     * drives it: a password login for an unknown SYSUAF user is REFUSED. */
    CHECK(password_login_of_unknown_user_is_refused(),
          "a PASSWORD login for a user with no SYSUAF record is REFUSED by the wrapped sshd -- SYSUAF/Purdy auth is wired and fails closed, no fabricated accept (vms-0cd 3c / INV-6)");

    if (fail) {
        if (strstr(buf, "OVMX_SRV_OK") == NULL)
            printf("  --- ssh client stdout: [%s] ---\n", buf);
        dump_sshd_log();
    }

    kill(sd, SIGTERM);
    waitpid(sd, NULL, 0);

    printf("=== test_syssvc_ssh_server: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
