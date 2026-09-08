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

#include "vms_kif.h"

#define SS_NORMAL       1
#define SS_IVCHAN       602
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

    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        return 1;

    /* Stay alive and holding the channel until the parent kills us --
     * exactly the shape test_kmod_devtab.c uses for the console. */
    for (;;)
        pause();

    return 0;
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

    CHECK(write_param(REMOVE_PARAM, RTA_DEV) == 0,
          "vms_devtab_remove_terminal(\"RTA0:\") withdraws the unit it minted");
    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(RTA_DEV, &info);
    CHECK(status == SS_NOSUCHDEV, "RTA0: no longer exists after withdrawal");

    CHECK(write_param(REMOVE_PARAM, RTA_DEV) != 0,
          "withdrawing an already-gone RTA0: reports failure, not a silent no-op");

    close(master_fd);
    vms_kif_close();

    printf("=== test_kmod_devtab_terminal: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
