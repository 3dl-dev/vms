/*
 * test_kmod_devtab_terminal.c - RTAn: dynamic terminal unit (rd vms-f881,
 * design docs/design/faithful-sessions-and-network-subsystems.md §3.2/§6-P2,
 * §7.5's ratification tell).
 *
 * WHAT THIS PROVES, and why it is shaped this way. The design's own anti-LARP
 * instrument (§7.5) is explicit: "if a 'SET HOST works' green can be produced
 * WITHOUT THE EXECUTIVE DEVICE TABLE CHANGING, it is a LARP. Bind the
 * acceptance gate to $GETDVI RTA0: returning a real device FROM A DIFFERENT
 * PROCESS than the session." This suite is exactly that bind:
 *
 *   PROCESS B (this binary, the parent) creates RTA0: through the real
 *   executive, PTY-backed by a REAL /dev/pts device it opened itself.
 *
 *   PROCESS A (a fork+exec of this same binary, --owner mode) opens its OWN
 *   /dev/vms, registers with the executive independently, and takes the
 *   ONLY channel that exists to RTA0: via a REAL VMS_IOCTL_ASSIGN -- the
 *   SAME implicit-ownership rule every non-shareable device already
 *   enforces (docs/oracle/vax73-terminal-device.md §7), not a special case
 *   this facility invented for terminals.
 *
 *   PROCESS B THEN READS RTA0: BACK, cold, from the executive's table --
 *   never told anything A did except A's own VMS pid, over a pipe -- and
 *   finds A's ownership, A's OPA0:-shape characteristics, and the fact
 *   that killing A releases them, exactly as it would for OPA0:.
 *
 * A device table entry that lived in one process's memory would pass every
 * one of these checks perfectly and still be a facade (CLAUDE.md rule 11) --
 * this is the SAME A-writes/B-reads discipline test_kmod_devtab.c (vms-d0b)
 * already applies to OPA0:, run against the newly-dynamic device class.
 *
 * WHY A SYSFS KNOB CREATES THE DEVICE. vms_devtab_add_terminal()/
 * _remove_terminal() (src/kernel-core/vms_devtab.c) are executive-internal
 * entry points -- their real caller is design P1's $CREPRC (a separate,
 * parallel item not yet landed) and P3/P4's vmssshd / decnetd-CTERM. This
 * item's own hard-acceptance gate needs a REAL /dev/vms round trip proving
 * the device-table primitive TODAY, without waiting on that caller -- the
 * same bring-up posture OVMX_KTEST_CLUSTER_SEAM already established for
 * FC-P0.2/FC-P0.16 (test_kmod_cluster_seam.c). The knob (vms_module.c,
 * compiled ONLY under OVMX_KTEST_DEVTAB_TERMINAL -- never the bootable
 * executive) does nothing but forward to the real kernel-core function; it
 * stamps NO ownership of its own, which is why process A still has to win
 * ownership the ordinary way, through a real $ASSIGN.
 *
 * NEGATIVE CONTROL: if the module is not built with OVMX_KTEST_DEVTAB_TERMINAL
 * (or not loaded at all), the sysfs parameter files do not exist and the
 * fopen()s below fail -- the suite fails honestly (no per-process fallback,
 * INV-6) rather than fabricating a device that was never entered.
 */

#define _GNU_SOURCE       /* posix_openpt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>

#include "vms_kif.h"

#define SS_NORMAL       1
#define SS_IVCHAN       602
#define SS_IVDEVNAM     608
#define SS_NOSUCHDEV    2680

#define DC_TERM         66      /* DC$_TERM */

#define RTA_DEV         "RTA0:"
#define CONSOLE_DEV     "OPA0:"

#define ADD_PARAM       "/sys/module/vms/parameters/vms_ktest_devtab_add_terminal"
#define REMOVE_PARAM    "/sys/module/vms/parameters/vms_ktest_devtab_remove_terminal"

/*
 * The OPA0:-shape characteristic set (§3.2: "the VMS_TTC_* characteristics
 * OPA0: carries"). Byte-identical to VMS_CONSOLE_DEVCHAR in vms_devtab.c --
 * that file's own comment carries the oracle provenance (docs/oracle/
 * vax73-terminal-device.md); this constant is repeated here, not re-derived,
 * so a divergence between what RTA0: actually reports and what the console
 * was measured to report shows up as a failing CHECK, not a passing one that
 * happens to agree with whatever vms_devtab.c does today.
 */
#define OPA0_SHAPE_DEVCHAR (VMS_TTC_INTERACTIVE    | \
                            VMS_TTC_ECHO           | \
                            VMS_TTC_TYPEAHEAD      | \
                            VMS_TTC_TTSYNC         | \
                            VMS_TTC_LOWERCASE      | \
                            VMS_TTC_WRAP           | \
                            VMS_TTC_BROADCAST      | \
                            VMS_TTC_FULLDUP        | \
                            VMS_TTC_SET_SPEED      | \
                            VMS_TTC_INSERT_EDITING | \
                            VMS_TTC_NUMERIC_KEYPAD | \
                            VMS_TTC_VMS_STYLE_INPUT)
#define OPA0_SHAPE_WIDTH   132
#define OPA0_SHAPE_PAGE    24

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

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

/* What process A reports back over the pipe: only its own identity and
 * what $ASSIGN told IT. B never learns anything about A except this. */
struct owner_report {
    uint32_t own_vms_pid;
    uint32_t assign_status;
    uint32_t chan;
    uint32_t netlogin_status;               /* VMS_IOCTL_TERM_GETLOGIN status  */
    char     netlogin[VMS_USERNAME_SIZE];   /* the note B stamped, read by A    */
};

static int process_a(int wfd)
{
    struct owner_report rep;

    memset(&rep, 0, sizeof(rep));

    if (vms_kif_open() < 0) {
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }
    if (vms_kif_register(&rep.own_vms_pid) != SS_NORMAL) {
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }

    rep.assign_status = vms_kif_assign(RTA_DEV, &rep.chan);

    /* vms-65b: read back the network-login note B stamped on RTA0: BEFORE
     * forking us -- from A's OWN fresh /dev/vms fd. If the note lived in B's
     * process memory rather than the shared executive device table, A would
     * read nothing here (the §7.5 anti-LARP bind, applied to the note). */
    rep.netlogin_status = vms_kif_terminal_getlogin(RTA_DEV, rep.netlogin,
                                                    sizeof(rep.netlogin));

    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        return 1;

    /* Stay alive and holding the channel until the parent kills us --
     * exactly the shape test_kmod_devtab.c uses for the console. */
    for (;;)
        pause();

    return 0;
}

/*
 * rd vms-1875 -- the HOLDER: a session process still bound to an RTAn: when
 * the daemon that minted it withdraws it. Assigns `devnam` from its own fresh
 * /dev/vms, reports, then on each byte from the parent performs one step and
 * reports again:
 *   'c'  $GETDVI through its OWN channel (the channel's device must still be
 *        the real row, not freed memory)
 *   'd'  $DASSGN that channel (the release that must delete a withdrawn unit)
 *   'x'  exit(0) -- the process-exit release path (vms_dev_release ->
 *        vms_proc_release_channels), the exact path the booted CTERM session
 *        took when it wrote into the freed row.
 */
struct holder_report {
    uint32_t status;
    uint32_t chan;
    uint32_t refcnt;
    char     devnam[VMS_DEVNAM_SIZE];
};

static int process_holder(int wfd, int rfd, const char *devnam)
{
    struct holder_report rep;
    struct vms_devinfo info;
    uint32_t chan = 0, own_pid = 0;
    char op;

    memset(&rep, 0, sizeof(rep));
    if (vms_kif_open() < 0 || vms_kif_register(&own_pid) != SS_NORMAL) {
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }
    rep.status = vms_kif_assign(devnam, &chan);
    rep.chan = chan;
    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        return 1;

    while (read(rfd, &op, 1) == 1) {
        memset(&rep, 0, sizeof(rep));
        rep.chan = chan;
        if (op == 'c') {
            memset(&info, 0, sizeof(info));
            rep.status = vms_kif_getdvi_chan(chan, &info);
            rep.refcnt = info.refcnt;
            memcpy(rep.devnam, info.devnam, sizeof(rep.devnam));
            rep.devnam[sizeof(rep.devnam) - 1] = '\0';
        } else if (op == 'd') {
            rep.status = vms_kif_dassgn(chan);
        } else if (op == 'x') {
            _exit(0);
        }
        if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
            return 1;
    }
    return 0;
}

/* Parent side of the holder protocol: fork+exec a holder bound to `devnam`
 * and return its pid (or -1), with its report/command pipes in *rfd / *wfd
 * and its first report (the $ASSIGN) in *rep. */
static pid_t spawn_holder(const char *self, const char *devnam,
                          int *rfd, int *wfd, struct holder_report *rep)
{
    int up[2], down[2];
    pid_t pid;

    if (pipe(up) < 0 || pipe(down) < 0)
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char w[16], r[16];

        close(up[0]);
        close(down[1]);
        vms_kif_close();                 /* take our OWN /dev/vms fd */
        snprintf(w, sizeof(w), "%d", up[1]);
        snprintf(r, sizeof(r), "%d", down[0]);
        execl(self, self, "--hold", w, r, devnam, (char *)NULL);
        _exit(73);
    }
    close(up[1]);
    close(down[0]);
    *rfd = up[0];
    *wfd = down[1];
    memset(rep, 0, sizeof(*rep));
    if (read(*rfd, rep, sizeof(*rep)) != (ssize_t)sizeof(*rep)) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    return pid;
}

static int holder_step(int rfd, int wfd, char op, struct holder_report *rep)
{
    memset(rep, 0, sizeof(*rep));
    if (write(wfd, &op, 1) != 1)
        return -1;
    return read(rfd, rep, sizeof(*rep)) == (ssize_t)sizeof(*rep) ? 0 : -1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    uint32_t self_vms_pid = 0, status;
    struct vms_devinfo info;
    int master_fd;
    char *slave_path;
    char pty_short[VMS_BACKING_SIZE];
    char add_val[64];
    pid_t child;
    int pipefd[2];
    struct owner_report rep;
    char probe[8];
    ssize_t n;

    if (argc >= 3 && strcmp(argv[1], "--owner") == 0)
        return process_a(atoi(argv[2]));
    if (argc >= 5 && strcmp(argv[1], "--hold") == 0)
        return process_holder(atoi(argv[2]), atoi(argv[3]), argv[4]);

    printf("=== test_kmod_devtab_terminal: RTAn: dynamic terminal unit ===\n");

    if (vms_kif_open() < 0) {
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        printf("=== test_kmod_devtab_terminal: 0 passed, 1 failed ===\n");
        return 1;
    }
    if (vms_kif_register(&self_vms_pid) != SS_NORMAL) {
        printf("  FAIL: VMS_IOCTL_REGISTER rejected\n");
        printf("=== test_kmod_devtab_terminal: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* --------------------------------------------------------------
     * 0. RTA0: does not exist before this suite creates it -- no unit is
     *    invented ahead of a real session asking for one (INV-6).
     * -------------------------------------------------------------- */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(RTA_DEV, &info);
    CHECK(status == SS_NOSUCHDEV, "RTA0: does not exist before creation");

    /* --------------------------------------------------------------
     * 1. Open a REAL PTY -- RTA0:'s byte transport (§3.2: "backed by a
     *    PTY... for byte transport"). This is not a placeholder string:
     *    §3 proves it carries real bytes, independent of the executive.
     * -------------------------------------------------------------- */
    master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0 || grantpt(master_fd) != 0 || unlockpt(master_fd) != 0 ||
        (slave_path = ptsname(master_fd)) == NULL) {
        printf("  FAIL: could not open a real PTY (posix_openpt/grantpt/unlockpt/ptsname)\n");
        printf("=== test_kmod_devtab_terminal: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    /* Put the PTY into RAW mode before the byte-transport check below. The
     * slave defaults to CANONICAL (ICANON) line discipline, where read()
     * blocks until a line terminator; the step-4 probe write sends no newline,
     * so a canonical slave read() hangs forever -- the exact stall that fired
     * the whole-VM wall (it happens AFTER the mint + the cross-process $ASSIGN/
     * $GETDVI checks, which all pass, so it is a test-harness pty bug, not a
     * vms_devtab.c device bug: the byte transport is a plain POSIX pty and does
     * not traverse /dev/vms). cfmakeraw clears ICANON+ECHO and sets VMIN=1/
     * VTIME=0; master and slave share one line-discipline state, so setting it
     * on the master here (before open(slave) below) applies to the slave read. */
    {
        struct termios tio;
        if (tcgetattr(master_fd, &tio) == 0) {
            cfmakeraw(&tio);
            tcsetattr(master_fd, TCSANOW, &tio);
        }
    }
    /* `backing` (vms_devtab.c) holds the SHORT native name, the same
     * convention as a disk's "vda"/"sdb" -- not a full path. */
    if (strncmp(slave_path, "/dev/pts/", 9) == 0)
        snprintf(pty_short, sizeof(pty_short), "pts%s", slave_path + 9);
    else
        snprintf(pty_short, sizeof(pty_short), "%s", slave_path);

    /* --------------------------------------------------------------
     * 2. Mint RTA0:, OPA0:-shape, PTY-backed. NEGATIVE CONTROL: if the
     *    knob is absent (module not built/loaded with the ktest gate),
     *    fail honestly rather than pretend the device exists (INV-6).
     * -------------------------------------------------------------- */
    snprintf(add_val, sizeof(add_val), "%s%s", RTA_DEV, pty_short);
    if (write_param(ADD_PARAM, add_val) != 0) {
        printf("  FAIL: cannot write %s (vms.ko not built with OVMX_KTEST_DEVTAB_TERMINAL, "
               "or not loaded)\n", ADD_PARAM);
        printf("=== test_kmod_devtab_terminal: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    CHECK(1, "vms_devtab_add_terminal(\"RTA0:\", ...) accepted the mint");

    /* Minting the same name twice is refused (-EEXIST): the row already
     * in the table is not silently duplicated or replaced. */
    CHECK(write_param(ADD_PARAM, add_val) != 0,
          "minting RTA0: a second time is refused (-EEXIST, not a silent duplicate)");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(RTA_DEV, &info);
    CHECK(status == SS_NORMAL, "RTA0: exists in the executive's table after minting");
    CHECK(strcmp(info.devnam, RTA_DEV) == 0, "RTA0: reports its own VMS physical name");
    CHECK(info.devclass == DC_TERM, "RTA0: is a terminal-class device (DC$_TERM)");
    CHECK(info.devchar == OPA0_SHAPE_DEVCHAR,
          "RTA0: carries the exact OPA0:-shape VMS_TTC_* characteristic set");
    CHECK(info.width == OPA0_SHAPE_WIDTH && info.page == OPA0_SHAPE_PAGE,
          "RTA0: carries OPA0:'s width/page (132x24)");
    CHECK(info.owner_pid == 0, "RTA0: starts unowned -- ownership is not stamped at creation");

    /* --------------------------------------------------------------
     * 2b. THE NETWORK-LOGIN PRE-AUTH NOTE (rd vms-65b). A daemon vouches a
     *     pre-authenticated user onto the RTAn: it minted (VMS_IOCTL_TERM_
     *     SETLOGIN); the $CREPRC(LOGINOUT) child bound to that terminal reads
     *     it back (VMS_IOCTL_TERM_GETLOGIN) and skips the prompt. Prove the
     *     round trip through the REAL executive, the honest-omission floor,
     *     and the guards -- then the cross-process read is proven in §3 below.
     * -------------------------------------------------------------- */
    {
        char note[VMS_USERNAME_SIZE];

        /* Fresh RTA0: carries no note -> empty, honest omission (NOT an error:
         * an ordinary terminal authenticates its own user). LOGINOUT reads this
         * as "no network pre-auth" and prompts (fail-closed, INV-6). */
        memset(note, 0xAA, sizeof(note));
        status = vms_kif_terminal_getlogin(RTA_DEV, note, sizeof(note));
        CHECK(status == SS_NORMAL && note[0] == '\0',
              "a fresh RTA0: has no network-login note (honest omission, not error)");

        /* Stamp the pre-authenticated user (we are root here -> privileged). */
        status = vms_kif_terminal_setlogin(RTA_DEV, "SYSTEM");
        CHECK(status == SS_NORMAL,
              "VMS_IOCTL_TERM_SETLOGIN stamps the pre-authenticated user onto RTA0:");

        /* Read it back, same process, THROUGH /dev/vms (not process memory). */
        memset(note, 0, sizeof(note));
        status = vms_kif_terminal_getlogin(RTA_DEV, note, sizeof(note));
        CHECK(status == SS_NORMAL && strcmp(note, "SYSTEM") == 0,
              "VMS_IOCTL_TERM_GETLOGIN reads the stamped note back from the executive");

        /* The console (OPA0:, a static non-dynamic terminal) is not a legal
         * target for a network-login note -- the same IVDEVNAM category guard
         * RESOLVE gives, so a daemon cannot vouch a user onto the operator's
         * console. */
        status = vms_kif_terminal_setlogin(CONSOLE_DEV, "SYSTEM");
        CHECK(status == SS_IVDEVNAM,
              "SETLOGIN refuses OPA0: (a note belongs only on a dynamic RTAn:)");
        status = vms_kif_terminal_getlogin(CONSOLE_DEV, note, sizeof(note));
        CHECK(status == SS_IVDEVNAM,
              "GETLOGIN refuses OPA0: (the console carries no network-login note)");
    }

    /* --------------------------------------------------------------
     * 3. A DIFFERENT PROCESS takes RTA0:'s only channel. This is the
     *    §7.5 tell: the device must be real BEFORE this process ever asks
     *    for it, and cross-process visible without either side telling
     *    the other anything but a bare VMS pid.
     * -------------------------------------------------------------- */
    if (pipe(pipefd) < 0) {
        printf("  FAIL: pipe()\n");
        printf("=== test_kmod_devtab_terminal: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    child = fork();
    if (child < 0) {
        printf("  FAIL: fork()\n");
        printf("=== test_kmod_devtab_terminal: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    if (child == 0) {
        char pid_arg[16];

        close(pipefd[0]);
        vms_kif_close();                 /* take our OWN /dev/vms fd, not the inherited one */
        snprintf(pid_arg, sizeof(pid_arg), "%d", pipefd[1]);
        execl(argv[0], argv[0], "--owner", pid_arg, (char *)NULL);
        _exit(73);
    }
    close(pipefd[1]);

    memset(&rep, 0, sizeof(rep));
    if (read(pipefd[0], &rep, sizeof(rep)) != (ssize_t)sizeof(rep)) {
        printf("  FAIL: process A never reported\n");
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        printf("=== test_kmod_devtab_terminal: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    CHECK(rep.assign_status == SS_NORMAL && rep.chan != 0,
          "a DIFFERENT process ($ASSIGN from a fresh /dev/vms fd) takes RTA0:'s channel");

    /* vms-65b: process A, from its OWN /dev/vms fd, read back the note B
     * stamped on RTA0: before the fork. This is the §7.5 anti-LARP bind for
     * the conveyance: a note held in B's process memory would come back empty
     * here. It comes back "SYSTEM" -- the SSH-daemon-stamps / LOGINOUT-reads
     * path, proven cross-process through the shared executive device table. */
    CHECK(rep.netlogin_status == SS_NORMAL && strcmp(rep.netlogin, "SYSTEM") == 0,
          "process A reads B's network-login note on RTA0: cross-process (B writes, A reads)");

    /* B reads RTA0: back cold -- nothing above told B what A's pid is
     * except the bare integer over the pipe (the SAME provenance
     * test_kmod_devtab.c uses for OPA0:). */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(RTA_DEV, &info);
    CHECK(status == SS_NORMAL && info.owner_pid == rep.own_vms_pid,
          "process B sees process A's ownership of RTA0: through $GETDVI (A writes, B reads)");
    CHECK(info.refcnt == 1, "one channel assigned means one reference");
    CHECK(info.allocated == 0, "ownership by channel alone is not an allocation");
    CHECK(info.devchar == OPA0_SHAPE_DEVCHAR,
          "the OPA0:-shape characteristics are unchanged by who owns the device");

    /* --------------------------------------------------------------
     * 4. The PTY genuinely carries bytes -- proof that `backing` names a
     *    LIVE transport, not a placeholder string, WITHOUT the executive
     *    touching a single byte of it (§3.2: "does not require bytes to
     *    traverse the executive").
     * -------------------------------------------------------------- */
    {
        int slave_fd = open(slave_path, O_RDWR | O_NOCTTY);

        CHECK(slave_fd >= 0, "the PTY slave RTA0: is recorded against opens directly");
        if (slave_fd >= 0) {
            const char *msg = "OVMX-RTA0-PROBE";

            CHECK(write(master_fd, msg, strlen(msg)) == (ssize_t)strlen(msg),
                  "writing to the PTY master succeeds");
            n = read(slave_fd, probe, sizeof(probe) - 1);
            CHECK(n > 0 && memcmp(probe, msg, n) == 0,
                  "the bytes written to the PTY master arrive on RTA0:'s real slave device");
            close(slave_fd);
        }
    }

    /* --------------------------------------------------------------
     * 5. Killing the owning process releases the channel, exactly as it
     *    does for OPA0: -- the SAME generic rundown path, never re-
     *    implemented for terminals.
     * -------------------------------------------------------------- */
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(RTA_DEV, &info);
    CHECK(status == SS_NORMAL, "RTA0: still exists after its owner dies");
    CHECK(info.owner_pid == 0, "dead process no longer owns RTA0:");
    CHECK(info.refcnt == 0, "dead process's channel was released");

    /* --------------------------------------------------------------
     * 6. The removal path is defended: it can withdraw ONLY a row it
     *    minted, never the console -- the same mscp_served-flag shape
     *    vms_devtab_remove_served_disk() uses against a locally-probed
     *    disk row.
     * -------------------------------------------------------------- */
    CHECK(write_param(REMOVE_PARAM, CONSOLE_DEV) != 0,
          "vms_devtab_remove_terminal() refuses to withdraw OPA0: (-ENODEV, not its row)");
    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(CONSOLE_DEV, &info);
    CHECK(status == SS_NORMAL, "OPA0: is untouched by the refused removal");

    /* The withdrawal rides vms_devtab_remove_terminal()'s dynamic_term unlink
     * gate; neuter that gate and the minted RTAn: row can never be torn down,
     * so this call and the follow-up SS_NOSUCHDEV check both redden. */
    /* negctl: devtab-terminal-withdrawal-not-honored */
    CHECK(write_param(REMOVE_PARAM, RTA_DEV) == 0,
          "vms_devtab_remove_terminal(\"RTA0:\") withdraws the unit it minted");
    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(RTA_DEV, &info);
    CHECK(status == SS_NOSUCHDEV, "RTA0: no longer exists after withdrawal");

    CHECK(write_param(REMOVE_PARAM, RTA_DEV) != 0,
          "withdrawing an already-gone RTA0: reports failure, not a silent no-op");

    /* --------------------------------------------------------------
     * 7. rd vms-1875 -- WITHDRAWAL WHILE A SESSION STILL HOLDS THE UNIT.
     *    The booted DECnet CTERM path withdraws its RTAn: (VMS_IOCTL_TERM_
     *    DELETE, ovmx_vterm_delete) when the link closes, which can be
     *    BEFORE the LOGINOUT process bound to that terminal has exited. The
     *    executive used to free the row on the spot; the session's later
     *    channel release then wrote into freed memory (KASAN: slab-use-
     *    after-free in device_release_channel), and without KASAN those
     *    stray writes corrupted whatever reused the object -- the kernel
     *    faults that intermittently killed the next console login in the
     *    x86_64 DCL/SHOW acceptance gate. The unit must instead live until
     *    its LAST reference is released, then go.
     *
     *    Driven through the PRODUCT door (VMS_IOCTL_TERM_CREATE/_DELETE, the
     *    calls ovmx_vterm_create/_delete make), not the ktest knob, so the
     *    caller under test is the one the booted runtime uses. Two release
     *    paths, each its own holder process: an explicit $DASSGN (7a) and
     *    process exit (7b).
     * -------------------------------------------------------------- */
    {
        const char *paths[2] = { "$DASSGN", "process exit" };
        int k;

        for (k = 0; k < 2; k++) {
            char unit[VMS_DEVNAM_SIZE], msg[256];
            struct holder_report hrep;
            int hr = -1, hw = -1;
            pid_t holder;

            memset(unit, 0, sizeof(unit));
            status = vms_kif_terminal_create(pty_short, unit, sizeof(unit));
            snprintf(msg, sizeof(msg),
                     "[%s] VMS_IOCTL_TERM_CREATE mints an RTAn: unit", paths[k]);
            CHECK(status == SS_NORMAL && strncmp(unit, "RTA", 3) == 0, msg);
            if (status != SS_NORMAL)
                continue;

            holder = spawn_holder(argv[0], unit, &hr, &hw, &hrep);
            snprintf(msg, sizeof(msg),
                     "[%s] a session process takes a channel to the unit", paths[k]);
            CHECK(holder > 0 && hrep.status == SS_NORMAL && hrep.chan != 0, msg);
            if (holder <= 0)
                continue;

            /* The daemon withdraws the unit while the session still holds it. */
            status = vms_kif_terminal_delete(unit);
            snprintf(msg, sizeof(msg),
                     "[%s] withdrawing the unit while a channel is assigned is accepted",
                     paths[k]);
            CHECK(status == SS_NORMAL, msg);

            /* NOT freed out from under the channel: the row is still the
             * executive's, still referenced once, visible by name. */
            memset(&info, 0, sizeof(info));
            status = vms_kif_getdvi_devnam(unit, &info);
            snprintf(msg, sizeof(msg),
                     "[%s] the withdrawn unit stays in the device table while a channel still holds it (refcnt 1)",
                     paths[k]);
            CHECK(status == SS_NORMAL && strcmp(info.devnam, unit) == 0 &&
                  info.refcnt == 1, msg);

            /* ...and the holder's own channel still resolves the real row. */
            snprintf(msg, sizeof(msg),
                     "[%s] the session's channel still resolves the real unit after withdrawal",
                     paths[k]);
            CHECK(holder_step(hr, hw, 'c', &hrep) == 0 &&
                  hrep.status == SS_NORMAL && strcmp(hrep.devnam, unit) == 0 &&
                  hrep.refcnt == 1, msg);

            /* The last release deletes it. */
            if (k == 0) {
                CHECK(holder_step(hr, hw, 'd', &hrep) == 0 && hrep.status == SS_NORMAL,
                      "[$DASSGN] the session deassigns its channel");
                kill(holder, SIGKILL);
            } else {
                char op = 'x';

                (void)!write(hw, &op, 1);
            }
            waitpid(holder, NULL, 0);
            close(hr);
            close(hw);

            memset(&info, 0, sizeof(info));
            status = vms_kif_getdvi_devnam(unit, &info);
            snprintf(msg, sizeof(msg),
                     "[%s] releasing the last channel deletes the withdrawn unit (SS$_NOSUCHDEV)",
                     paths[k]);
            CHECK(status == SS_NOSUCHDEV, msg);
        }
    }

    close(master_fd);
    vms_kif_close();

    printf("=== test_kmod_devtab_terminal: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
