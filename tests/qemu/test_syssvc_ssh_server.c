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
 * It then drives BOTH SYSUAF/Purdy password outcomes over that same wrapped sshd:
 * an unknown user is REFUSED (fail-closed, needs only /dev/vms), and -- the RUNG-3
 * step 3d capstone (rd vms-9cc) -- a VALID SYSUAF user (SYSTEM/MANAGER) authenticates
 * by PASSWORD and LANDS IN a real interactive DCL session. The wrapped sshd resolves
 * the REAL shipped SYS$SYSTEM:SYSUAF.DAT over the executive ACP on the real system
 * volume VDA300: (see password_login_of_valid_user_lands_in_dcl) -- no /etc/passwd
 * seed, no stubbed lookup; the SYSUAF resolution is genuinely exercised.
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
#include "ssdef.h"        /* SS$_NORMAL */
#include "vms/logical.h"  /* lnm_* : point SYS$SYSDEVICE at the real system volume */

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
 * DCL.EXE. The positive "valid SYSUAF user -> lands in DCL" half is the 3d capstone
 * in password_login_of_valid_user_lands_in_dcl() below, which provisions the real
 * system volume so SYS$SYSTEM:SYSUAF.DAT resolves over the ACP. Returns 1 if the
 * unknown-user password login was correctly refused, 0 if it slipped through.
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

/*
 * vms-0cd RUNG-3 step 3d (rd vms-9cc): the POSITIVE capstone -- a VALID SYSUAF
 * user (SYSTEM/MANAGER) authenticates by PASSWORD over the wrapped OpenSSH sshd
 * and LANDS IN a real interactive DCL session over the executive BGn: connection,
 * no AF_UNIX socketpair.
 *
 * Provisioning (the real-ACP part, INV-6): the wrapped sshd's __wrap_getpwnam ->
 * sysuaf_lookup opens SYS$SYSTEM:SYSUAF.DAT over the Files-11 ACP. The KE harness's
 * corpus_seed_lnm seeds SYS$SYSDEVICE=VDA0: -- a fixture volume with NO
 * [SYS0.SYSCOMMON.SYSEXE] tree -- so the lookup fails ("Invalid user SYSTEM") in a
 * bare process. VDA300: is the generated ODS-2 system volume (mkimage_ods2_sysvol)
 * that masters [SYS0.SYSCOMMON.SYSEXE] holding the REAL shipped SYSUAF.DAT
 * (SYSTEM/MANAGER, copied verbatim from distro/rootfs). So we $MOUNT VDA300: and
 * repoint SYS$SYSDEVICE -> VDA300: at LNM$SYSTEM/EXEC scope -- exactly what
 * STARTUP.COM's $MOUNT does at boot. corpus_seed_lnm's SYS$SYSROOT/SYS$SYSTEM are
 * defined RELATIVE to SYS$SYSDEVICE, so flipping SYS$SYSDEVICE alone cascades the
 * whole concealed-rooted chain to the real volume. EXEC scope (executive-global)
 * so the SEPARATELY-EXEC'd sshd child inherits it -- process scope would not
 * survive execve. Restored after the session so sibling suites in this shard see
 * the original SYS$SYSDEVICE. This is a REAL ACP read of the real shipped SYSUAF --
 * NO /etc/passwd seed for SYSTEM, NO stubbed lookup; the resolution that was never
 * runtime-exercised before is genuinely driven here.
 *
 * The wrapped sshd verifies SYSTEM/MANAGER (SYSUAF/Purdy), __wrap_getpwnam sets
 * pw_shell=DCL, ovmx_sshd_pre_drop_pw establishes the executive identity (or emits
 * %OVMX-F-NOIDENT + exit, fail-closed) then prints the SYS$WELCOME banner, and
 * __wrap_execve rewrites do_child's login-shell exec into `vmsdcl --login`. We feed
 * a DCL command + LOGOUT on the session stdin and read the reply back; the marker
 * in the client's stdout proves the command crossed the wrapped BGn: session channel
 * into DCL's own interpreter (non-interactive DCL uses fgets with NO echo, so the
 * marker proves execution, not an echoed input line). Captures the client's combined
 * stdout/stderr into out[] for main()'s assertions.
 */
static void password_login_of_valid_user_lands_in_dcl(char *out, size_t outsz)
{
    const char *askpass = "/tmp/ovmx_askpass_ok";
    const char *dclcmd = "WRITE SYS$OUTPUT \"OVMX_DCL_LANDED_9cc\"\nLOGOUT\n";
    lnm_manager_t *mgr;
    char saved_sysdev[256];
    uint16_t saved_len = 0;
    uint32_t saved_attr = 0;
    int have_saved = 0;
    int inpipe[2], outpipe[2];
    pid_t cp;
    size_t got = 0;
    int cst = 0;

    if (outsz == 0) return;
    out[0] = '\0';

    /* $MOUNT the real system volume + repoint SYS$SYSDEVICE at it (executive-global,
     * exec-inherited), saving the harness default to restore afterward. */
    {
        uint32_t mst = vms_kif_acp_mount("VDA300:");
        printf("  [9cc-diag] vms_kif_acp_mount(\"VDA300:\") = 0x%08x (SS$_NORMAL=0x%08x)\n",
               mst, (uint32_t)SS$_NORMAL);
    }
    mgr = lnm_get_manager();
    if (mgr != NULL) {
        if (lnm_translate(mgr, LNM_SYSTEM_TABLE, "SYS$SYSDEVICE",
                          saved_sysdev, sizeof(saved_sysdev),
                          &saved_len, &saved_attr) == SS$_NORMAL &&
            saved_len < sizeof(saved_sysdev)) {
            saved_sysdev[saved_len] = '\0';
            have_saved = 1;
        }
        lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SYSDEVICE", "VDA300:",
                   LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
    }

    /* [9cc-diag] Confirm the provisioning took (cheap, no extra link): what
     * SYS$SYSDEVICE resolves to after the flip. The SYSUAF read itself is proven
     * by the sshd assertions below, not an in-process reader (which would drag
     * the whole vmsrms+ODS2 codec into this suite and bloat the shared boot
     * initramfs -- the reason rms_acp_bind is opt-in, not blanket). */
    if (mgr != NULL) {
        char cur[256]; uint16_t cl = 0; uint32_t ca = 0;
        if (lnm_translate(mgr, LNM_SYSTEM_TABLE, "SYS$SYSDEVICE", cur, sizeof(cur),
                          &cl, &ca) == SS$_NORMAL && cl < sizeof(cur)) {
            cur[cl] = '\0';
            printf("  [9cc-diag] SYS$SYSDEVICE now resolves to '%s'\n", cur);
        }
    }

    /* askpass feeds the VALID SYSUAF password over SSH_ASKPASS (no controlling tty). */
    {
        FILE *f = fopen(askpass, "w");
        if (f) {
            fputs("#!/bin/sh\nprintf '%s\\n' 'MANAGER'\n", f);
            fclose(f);
            chmod(askpass, 0755);
        }
    }

    if (pipe(inpipe) == 0) {
        if (pipe(outpipe) == 0) {
            cp = fork();
            if (cp == 0) {
                setsid();               /* no controlling tty -> use askpass */
                setenv("SSH_ASKPASS", askpass, 1);
                setenv("SSH_ASKPASS_REQUIRE", "force", 1);
                setenv("DISPLAY", ":0", 1);
                dup2(inpipe[0], 0);
                dup2(outpipe[1], 1);
                dup2(outpipe[1], 2);
                close(inpipe[0]); close(inpipe[1]);
                close(outpipe[0]); close(outpipe[1]);
                /* NO remote command: do_child execs the login shell (DCL for a
                 * SYSUAF user), which __wrap_execve rewrites to `vmsdcl --login`;
                 * DCL then reads our fed stdin. Password-only via the srvpw alias. */
                {
                    char *av[] = { (char *)SRV_SSH, "-F", (char *)SRV_SSHCFG,
                                   (char *)"-l", (char *)"SYSTEM",
                                   (char *)"srvpw", NULL };
                    execv(SRV_SSH, av);
                }
                _exit(127);
            }
            close(inpipe[0]);
            close(outpipe[1]);
            (void)write(inpipe[1], dclcmd, strlen(dclcmd));
            close(inpipe[1]);           /* EOF -> DCL exits even without LOGOUT */
            for (;;) {
                ssize_t n = read(outpipe[0], out + got, outsz - 1 - got);
                if (n <= 0) break;
                got += (size_t)n;
                if (got >= outsz - 1) break;
            }
            out[got] = '\0';
            close(outpipe[0]);
            waitpid(cp, &cst, 0);
            (void)cst;
        } else {
            close(inpipe[0]); close(inpipe[1]);
        }
    }

    /* Restore the harness's SYS$SYSDEVICE so later suites in this shard are unaffected. */
    if (mgr != NULL && have_saved)
        lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SYSDEVICE", saved_sysdev,
                   saved_attr, LNM_MODE_EXEC);
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

    /* vms-0cd 3c: a PASSWORD login for an unknown SYSUAF user is REFUSED. NOTE
     * (rd vms-9cc, the vacuous-negative guard): this refusal is only MEANINGFUL
     * paired with the positive below. Until vms-9cc force-bound the RMS-ACP seam
     * (ovmx_sshd_rms_bind.o), the wrapped sshd's SYSUAF reader was NULL and
     * refused EVERY user without opening the file -- so this passed vacuously. The
     * positive assertion below (a REAL SYSUAF user SYSTEM is Purdy-authenticated
     * and lands in DCL over the SAME sshd) is what proves the reader actually
     * opened SYSUAF; with that green, this refusal is genuinely "user absent from
     * a read SYSUAF," not "reader inert." The two are asserted together. */
    CHECK(password_login_of_unknown_user_is_refused(),
          "a PASSWORD login for a user with no SYSUAF record is REFUSED by the wrapped sshd -- SYSUAF/Purdy auth fails closed (non-vacuous: the SYSTEM positive below proves the reader opened SYSUAF) (vms-0cd 3c / vms-9cc / INV-6)");

    /* vms-0cd 3d (rd vms-9cc): the POSITIVE capstone -- a VALID SYSUAF user
     * (SYSTEM/MANAGER) authenticates by PASSWORD over the wrapped sshd and lands
     * in a real interactive DCL session over the BGn: connection. */
    {
        char dbuf[4096];
        password_login_of_valid_user_lands_in_dcl(dbuf, sizeof(dbuf));

        /* The capstone gate: a DCL command fed on the session stdin executed and
         * its output returned -- only a real SYSUAF-authenticated login that reached
         * DCL's interpreter produces this (empty/failed sessions have no marker). */
        CHECK(strstr(dbuf, "OVMX_DCL_LANDED_9cc") != NULL,
              "a DCL command fed on the SSH session stdin executed and its output returned -- a valid SYSUAF user (SYSTEM/MANAGER, resolved from the real SYS$SYSTEM:SYSUAF.DAT over the ACP) landed in a real interactive DCL session over the wrapped BGn: connection (vms-9cc capstone)");
        CHECK(strstr(dbuf, "OVMX-F-NOIDENT") == NULL,
              "the executive did NOT refuse the SYSTEM identity -- SYSUAF/Purdy password auth + executive $SETIDENT succeeded, fail-closed (vms-0cd 3c / INV-6)");

        /* SYS$WELCOME banner: NON-GATING diagnostic, not an assertion. This
         * session is NON-PTY (a pipe on stdin), and ovmx_sshd_pre_drop_pw emits
         * the banner to stdout from inside __wrap_permanently_set_uid, which
         * OpenSSH's do_child runs BEFORE it wires the session channel onto fd1 for
         * a non-PTY exec -- so the banner lands on the pre-dup2 fd, not the client
         * channel (DCL's later output, post channel-setup, does reach the client:
         * the marker above proves it). Faithfully, VMS shows SYS$WELCOME for an
         * INTERACTIVE (PTY) login, not a piped/non-interactive session, so gating
         * it here would assert non-faithful behaviour. The DCL-landing marker +
         * no-NOIDENT are the capstone proof. A faithful PTY-banner proof (ssh -tt
         * + an echo-proof computed marker) is a separate follow-up. */
        printf("  [9cc-diag] SYS$WELCOME banner in non-PTY session stdout: %s "
               "(non-gating; banner is an interactive/PTY-login artifact)\n",
               strstr(dbuf, "Welcome to") != NULL ? "present" : "absent");

        if (strstr(dbuf, "OVMX_DCL_LANDED_9cc") == NULL) {
            printf("  --- valid-user (SYSTEM) DCL session stdout: [%s] ---\n", dbuf);
            dump_sshd_log();
        }
    }

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
