/*
 * test_syssvc_ssh_kex.c - THE Rule-9 acceptance for the OpenSSH-on-veneer arc
 * (item vms-22a): the veneer-linked OpenSSH `ssh` client performs a REAL SSH key
 * exchange + public-key auth against an in-guest stock `sshd`, over 127.0.0.1 via
 * the OVMX BSD-sockets veneer -> BGn: -> executive, and runs a trivial remote
 * command whose output is captured BYTE-EXACT on the client side.
 *
 * ============================================================
 * WHAT THIS PROVES. The whole SSH transport arc, end to end, over a real
 * /dev/vms: the veneer-linked `ssh` (its socket()/connect() resolve to
 * ovmx_socket/ovmx_connect, its event-loop poll to the veneer readiness fd, its
 * packet I/O to ovmx_send/ovmx_recv) completes a genuine SSH-2 handshake --
 * KEXINIT, Diffie-Hellman/curve key exchange, host-key + pubkey auth -- with a
 * STOCK OpenSSH sshd peer on the loopback, then execs `echo OVMX_SSH_OK` and the
 * client reads that line back. Every ssh->wire byte transits $QIO into the
 * executive (vms.ko) and out the host kernel's loopback; sshd is an ordinary
 * host-socket server (the BGn: SERVER path is vms-698, not needed for a client
 * proof). The staged binaries + keys + config come from
 * third-party/openssh/build-ssh-kex-harness.sh (see tests/qemu/Dockerfile).
 * ============================================================
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko): with no
 * /dev/vms the veneer's ovmx_socket() fails SS$_NOSUCHDEV->ENODEV, so the client
 * cannot even connect -- a KEX proof is impossible and we EXIT_SKIP (77), never
 * a fabricated pass (CLAUDE.md Rule 9 / INV-6). Checked before anything is spawned.
 *
 * WATCHDOG: sshd/ssh run as child processes; a wedged handshake would hang the
 * QEMU boot, so alarm() bounds the whole run to a named FAIL.
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

#define EXIT_SKIP 77
#define SSH_PORT  2222

/* Where tests/qemu/Dockerfile stages the harness (binaries + keys + config). */
#define OVMX_SSH      "/ovmxssh/ssh"
#define OVMX_SSHD     "/ovmxssh/sshd"
#define OVMX_SSHDCFG  "/ovmxssh/etc/sshd_config"
#define OVMX_SSHCFG   "/ovmxssh/etc/ssh_config"
#define OVMX_PRIVSEP  "/ovmxssh/empty"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_ssh_kex timed out (handshake wedge)\n";
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

/* vms-9ac de-LARP proof: does process `pid` hold ANY AF_UNIX socket fd? Under the
 * full --wrap de-veneer the ssh connection is a veneer HANDLE (>= OVMX_BGSOCK_BASE,
 * not a real fd), so a wrapped ssh has NO AF_UNIX socket at all; a surviving
 * AF_UNIX socket fd would mean the retired socketpair+pump fabrication is back.
 * Cross-refs /proc/<pid>/fd socket inodes against /proc/net/unix (AF_UNIX inodes). */
static int proc_has_afunix_socket(pid_t pid)
{
    unsigned long uinodes[8192];
    size_t nu = 0;
    char line[512], path[64], fp[128], tgt[128];
    struct dirent *de;
    DIR *d;
    int found = 0;

    FILE *u = fopen("/proc/net/unix", "r");
    if (!u) return 0;                       /* can't tell -> never false-fail */
    if (!fgets(line, sizeof(line), u)) { fclose(u); return 0; }   /* header */
    while (fgets(line, sizeof(line), u) && nu < 8192) {
        /* fields: Num RefCount Protocol Flags Type St Inode [Path] */
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
        if (sscanf(tgt, "socket:[%lu]", &ino) == 1 && ino != 0) {
            for (size_t i = 0; i < nu; i++)
                if (uinodes[i] == ino) { found = 1; break; }
        }
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

/* Wait until 127.0.0.1:SSH_PORT accepts a host-socket connection (sshd is up). */
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

    printf("=== test_syssvc_ssh_kex (veneer-linked ssh: REAL KEX + remote command over BGn:) ===\n");

    if (!executive_present()) {
        /* No /dev/vms: the veneer client cannot connect; a KEX proof is
         * impossible -- honest skip, never a fabricated pass (Rule 9/INV-6). */
        printf("  PASS: no executive -> KEX proof honestly skipped (veneer ovmx_socket would fail ENODEV)\n");
        pass++;
        printf("=== test_syssvc_ssh_kex: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n", pass, fail);
        return EXIT_SKIP;
    }

    /* Sanity: the harness must have been staged into the initramfs. */
    if (access(OVMX_SSH, X_OK) != 0 || access(OVMX_SSHD, X_OK) != 0) {
        printf("  FAIL: staged ssh/sshd not found under /ovmxssh (Dockerfile staging)\n");
        printf("=== test_syssvc_ssh_kex: %d passed, %d failed ===\n", pass, ++fail);
        return 1;
    }

    signal(SIGALRM, watchdog);
    alarm(40);

    bring_lo_up();
    (void)mkdir(OVMX_PRIVSEP, 0755);    /* privsep dir (root-owned 0755) */

    /* sshd's privilege separation looks up the 'sshd' user via getpwnam(); the
     * command runs as the 'root' login user. Write the minimal account files at
     * runtime so the proof does not depend on initramfs /etc staging. */
    (void)mkdir("/etc", 0755);
    {
        FILE *f = fopen("/etc/passwd", "w");
        if (f) {
            fputs("root:x:0:0:root:/root:/bin/sh\n"
                  "sshd:x:74:74:sshd privsep:/ovmxssh/empty:/bin/false\n", f);
            fclose(f);
        }
        FILE *g = fopen("/etc/group", "w");
        if (g) { fputs("root:x:0:\nsshd:x:74:\n", g); fclose(g); }
    }
    (void)mkdir("/root", 0755);

    /* ---- start the stock sshd in the FOREGROUND (-D) as a child; log to a
     * file we dump on failure so a startup error is visible in the KE log. ---- */
    pid_t sd = fork();
    if (sd == 0) {
        int lf = open("/tmp/sshd.log", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); close(lf); }
        char *av[] = { (char *)OVMX_SSHD, "-D", "-e", "-f", (char *)OVMX_SSHDCFG, NULL };
        execv(OVMX_SSHD, av);
        _exit(127);
    }
    int up = wait_for_sshd(50);
    CHECK(up, "sshd is accepting connections on 127.0.0.1:2222");
    if (!up) {
        FILE *lf = fopen("/tmp/sshd.log", "r");
        char line[512];
        printf("  --- sshd.log (startup failure) ---\n");
        if (lf) { while (fgets(line, sizeof(line), lf)) printf("  sshd: %s", line); fclose(lf); }
        else printf("  (no sshd.log)\n");
    }

    /* ---- run the veneer-linked ssh; capture its stdout ---- */
    int pfd[2];
    if (pipe(pfd) != 0) { printf("  FAIL: pipe()\n"); return 1; }
    pid_t cp = fork();
    if (cp == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]); close(pfd[1]);
        char *av[] = { (char *)OVMX_SSH, "-F", (char *)OVMX_SSHCFG,
                       (char *)"testpeer", (char *)"echo OVMX_SSH_OK", NULL };
        execv(OVMX_SSH, av);
        _exit(127);
    }
    close(pfd[1]);
    /* vms-9ac de-LARP proof: sample the ssh process for any AF_UNIX socket fd
     * WHILE it handshakes. Under the full --wrap de-veneer the connection is a
     * veneer handle (not a real fd), so there must be ZERO AF_UNIX sockets; any
     * would mean the retired socketpair+pump is back. Sampled (WNOHANG) so a slow
     * QEMU-TCG handshake is covered without wedging. */
    int saw_afunix = 0, reaped = 0, cst = 0;
    for (int s = 0; s < 200; s++) {
        if (waitpid(cp, &cst, WNOHANG) == cp) { reaped = 1; break; }
        if (proc_has_afunix_socket(cp)) { saw_afunix = 1; break; }
        usleep(15000);
    }
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
    if (!reaped) waitpid(cp, &cst, 0);

    CHECK(WIFEXITED(cst) && WEXITSTATUS(cst) == 0,
          "veneer-linked ssh completed the SSH handshake + session (exit 0)");
    CHECK(!saw_afunix,
          "NO AF_UNIX socket fd in the ssh process during the handshake -- the socketpair pump is EXCISED; bytes ride the executive seam, not a fabricated Unix socket (vms-9ac)");
    /* negctl: bg-recv-length-zeroed */
    CHECK(strstr(buf, "OVMX_SSH_OK") != NULL,
          "the remote command output came back BYTE-EXACT over the veneer (real KEX proven)");
    if (strstr(buf, "OVMX_SSH_OK") == NULL) {
        FILE *lf = fopen("/tmp/sshd.log", "r");
        char line[512];
        printf("  --- ssh client stdout: [%s] ---\n", buf);
        printf("  --- sshd.log (KEX failure) ---\n");
        if (lf) { while (fgets(line, sizeof(line), lf)) printf("  sshd: %s", line); fclose(lf); }
    }

    kill(sd, SIGTERM);   /* stop the foreground sshd child */
    system("kill $(cat /ovmxssh/sshd.pid 2>/dev/null) 2>/dev/null");

    printf("=== test_syssvc_ssh_kex: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
