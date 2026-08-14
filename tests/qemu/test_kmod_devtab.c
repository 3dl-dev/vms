/*
 * test_kmod_devtab.c - Executive device table: A-writes / B-reads (vms-d0b)
 *
 * A terminal is only a VMS device because the whole system agrees it
 * exists. A device table a process keeps in its own address space
 * passes every single-process test perfectly and is still a facade --
 * it reports success while sharing nothing (CLAUDE.md rule 11). So
 * this test is built around the check that can tell the difference:
 *
 *   PROCESS A ASSIGNS THE DEVICE AND CHANGES IT.
 *   PROCESS B, WHICH DID NOTHING, SEES THE CHANGE.
 *
 * and around the property that makes the device the executive's and
 * not the process's:
 *
 *   THE DEVICE EXISTS BEFORE ANY PROCESS ASKS FOR IT, AND IT SURVIVES
 *   THE DEATH OF THE PROCESS THAT OWNED IT.
 *
 * The test drives the real userspace client (src/libvmssys/vms_kif.c)
 * against a real /dev/vms, not a hand-rolled ioctl copy, so the client
 * that libvms will call is the client under test.
 *
 * Process A runs in two phases so that the two ownership rules can be
 * observed SEPARATELY -- if A always assigned and allocated in one
 * breath, either rule alone would be enough to make every refusal come
 * out right, and deleting one of them would leave the suite green.
 *
 *   phase 1: A holds nothing but a CHANNEL. It is already the owner.
 *   phase 2: A allocates, on B's word.
 *
 * Modes:
 *   (no args)              process B (parent)
 *   --owner <wfd> <rfd>    process A
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "vms_kif.h"

#define SS_NORMAL       1
#define SS_BADPARAM     20
#define SS_IVCHAN       602
#define SS_IVDEVNAM     608
#define SS_NOMOREDEV    2648
#define SS_NOSUCHDEV    2680
#define SS_DEVALLOC     2112    /* oracle-measured; see ssdef.h provenance */
#define SS_DEVNOTALLOC  2136

#define DC_TERM         6
#define DC_SCOM         3       /* DC$_SCOM -- LAN/Ethernet device class */

#define CONSOLE         "OPA0:"
#define ABSENT_DEV      "ZZA0:"

/*
 * The VMS-visible name of the NIC device face (vms-9d2). ONE constant, matching
 * the single source of truth in src/kernel-core/vms_devtab.c (VMS_NIC_DEVNAM):
 * ETH0: per device-native-naming (operator 2026-08-14) -- OVMX device names
 * track the substrate's native kernel device name, so the Linux "eth" root
 * renders as ETH0:. Any later name change is a one-line edit there and here.
 * NIC_ABSENT is a plausible SECOND Ethernet unit that must NOT exist -- the
 * executive registers exactly ONE unit for the ONE primary net device, never a
 * fake for every nameable controller (INV-6).
 */
#define NIC_DEV         "ETH0:"
#define NIC_ABSENT      "ETH1:"

/* What process A changes the console to, so B can look for it. */
#define A_WIDTH         64
#define A_PAGE          40

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What process A reports back over a pipe, once per phase. */
struct owner_report {
    /*
     * The VMS process ID the EXECUTIVE assigned to this process
     * (vms-2b8). Device ownership is recorded as a VMS process ID, and
     * these assertions used to compare it against the LINUX pid -- which
     * only worked because registration copied the Linux pid into the VMS
     * one. The executive assigns it now, so the owner has to be told to
     * us rather than assumed.
     */
    uint32_t own_vms_pid;
    uint32_t assign_status;
    uint32_t setmode_status;
    uint32_t alloc_status;
    uint32_t realloc_status;    /* allocating again, already ours */
    uint32_t chan;
    uint32_t owner_after_assign;/* owner the executive shows after $ASSIGN alone */
    uint32_t refcnt_after_assign;
    uint32_t alloc_after_assign;/* "allocated" flag after $ASSIGN alone */
    uint32_t owner_pid;         /* owner after $ALLOC */
    uint32_t refcnt;            /* reference count after $ALLOC */
    uint32_t allocated;
};

/* The VMS process ID the executive assigned to THIS process. Device
 * ownership is recorded and reported as a VMS process ID (vms-2b8), so
 * the assertions below compare against this, not against getpid(). */
static uint32_t self_vms_pid;

static int open_and_register(void)
{
    if (vms_kif_open() < 0) {
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        return -1;
    }
    if (vms_kif_register(&self_vms_pid) != SS_NORMAL) {
        printf("  FAIL: VMS_IOCTL_REGISTER rejected\n");
        return -1;
    }
    return 0;
}

/*
 * process_a - phase 1: take a channel to the console and stop there, so
 * that B can see what a channel alone does. Phase 2, once B says so:
 * allocate, and change characteristics nobody is told about except
 * through the executive.
 */
static int process_a(int wfd, int rfd)
{
    struct owner_report rep;
    struct vms_devinfo info;
    char go;

    memset(&rep, 0, sizeof(rep));

    if (vms_kif_open() < 0) {
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }
    if (vms_kif_register(&rep.own_vms_pid) != SS_NORMAL) {
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }

    /* ---- phase 1: a channel and nothing else ---- */
    rep.assign_status = vms_kif_assign(CONSOLE, &rep.chan);
    if (rep.assign_status == SS_NORMAL) {
        memset(&info, 0, sizeof(info));
        if (vms_kif_getdvi_chan(rep.chan, &info) == SS_NORMAL) {
            rep.owner_after_assign = info.owner_pid;
            rep.refcnt_after_assign = info.refcnt;
            rep.alloc_after_assign = info.allocated;
        }
    }
    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        return 1;

    /* Wait for B, on B's word -- not on a timer. */
    if (read(rfd, &go, 1) != 1)
        return 1;

    /* ---- phase 2: allocate, and change the terminal ---- */
    if (rep.assign_status == SS_NORMAL) {
        rep.alloc_status = vms_kif_alloc(CONSOLE);
        /* Allocating a device we already have is a no-op on the lab,
         * including its reference count. */
        rep.realloc_status = vms_kif_alloc(CONSOLE);

        rep.setmode_status = vms_kif_ttsetmode(
            rep.chan,
            VMS_TTSET_CHAR | VMS_TTSET_WIDTH | VMS_TTSET_PAGE,
            VMS_TTC_PASTHRU,        /* set */
            VMS_TTC_ECHO,           /* clear */
            A_WIDTH, A_PAGE);

        memset(&info, 0, sizeof(info));
        if (vms_kif_getdvi_chan(rep.chan, &info) == SS_NORMAL) {
            rep.owner_pid = info.owner_pid;
            rep.refcnt = info.refcnt;
            rep.allocated = info.allocated;
        }
    }

    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        return 1;
    close(wfd);

    /* Stay alive and owning the device until the parent kills us. */
    for (;;)
        pause();

    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so a still-buffered write cannot splice into a child process output */
    int pipefd[2], gofd[2];
    pid_t child;
    struct owner_report rep;
    struct vms_devinfo info;
    uint32_t status, chan = 0, bogus_chan, index, scanned, saw_console;
    char wfd_arg[16], rfd_arg[16];

    if (argc >= 4 && strcmp(argv[1], "--owner") == 0)
        return process_a(atoi(argv[2]), atoi(argv[3]));

    printf("=== test_kmod_devtab: executive device table ===\n");

    if (open_and_register() < 0) {
        printf("=== test_kmod_devtab: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* --------------------------------------------------------------
     * 1. The device exists before anyone asks for it. Nothing in this
     *    process created OPA0:; the executive did, at module init,
     *    the way a VMS driver creates the console unit at boot.
     * -------------------------------------------------------------- */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(status == SS_NORMAL, "console OPA0: exists without any process creating it");
    CHECK(strcmp(info.devnam, CONSOLE) == 0, "device reports its VMS physical name");
    CHECK(info.devclass == DC_TERM, "console is a terminal-class device (DC$_TERM)");
    CHECK(info.owner_pid == 0, "console starts unowned");

    /* Negative control: a lookup that always succeeded would look
     * identical to a lookup that works. */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(ABSENT_DEV, &info);
    CHECK(status == SS_NOSUCHDEV, "absent device reports SS$_NOSUCHDEV");

    status = vms_kif_getdvi_devnam("not a device", &info);
    CHECK(status == SS_IVDEVNAM, "malformed device name reports SS$_IVDEVNAM");

    /* A channel we never assigned is not ours. */
    status = vms_kif_getdvi_chan(4242, &info);
    CHECK(status == SS_IVCHAN, "unassigned channel reports SS$_IVCHAN");
    status = vms_kif_ttsetmode(4242, VMS_TTSET_WIDTH, 0, 0, 100, 0);
    CHECK(status == SS_IVCHAN,
          "cannot set characteristics without a channel (SS$_IVCHAN)");

    /* --------------------------------------------------------------
     * 2. The scan enumerates the table, and terminates.
     * -------------------------------------------------------------- */
    scanned = 0;
    saw_console = 0;
    index = 0;
    for (;;) {
        memset(&info, 0, sizeof(info));
        status = vms_kif_devscan(&index, &info);
        if (status != SS_NORMAL)
            break;
        scanned++;
        if (strcmp(info.devnam, CONSOLE) == 0)
            saw_console = 1;
        if (scanned > 64)
            break;
    }
    /* negctl: devtab-devscan-found-status-wrong */
    CHECK(status == SS_NOMOREDEV, "device scan terminates with SS$_NOMOREDEV");
    /* negctl-knockon: devtab-devscan-found-status-wrong */
    CHECK(saw_console, "device scan lists the console terminal");

    /* --------------------------------------------------------------
     * 2b. The NIC as a VMS device ETH0: (vms-9d2, epic vms-67f L0).
     *
     *     Like the console and the disks, ETH0: is entered by the
     *     executive at module init from the node's primary Ethernet net
     *     device -- sourced through the generic netdev abstraction, so it
     *     is the SAME executive-owned table this process reads, not a
     *     per-process fiction. The QEMU harness gives the guest one
     *     virtio-net NIC (run_tests.sh), which the driver-agnostic probe
     *     enumerates; on a node with no NIC the executive enters no ETH0:
     *     and these lookups would honestly report SS$_NOSUCHDEV.
     * -------------------------------------------------------------- */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(NIC_DEV, &info);
    CHECK(status == SS_NORMAL,
          "NIC ETH0: exists without any process creating it (entered from the host's primary Ethernet net device)");
    CHECK(strcmp(info.devnam, NIC_DEV) == 0, "ETH0: reports its VMS physical name");
    CHECK(info.devclass == DC_SCOM, "ETH0: is a LAN-class device (DC$_SCOM)");
    CHECK(info.owner_pid == 0, "ETH0: starts unowned");

    /* The executive registers exactly ONE unit for the ONE primary net
     * device -- a second, plausible Ethernet name is NOT invented. This
     * is the INV-6 line: no fake device for a NIC that is not there. */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(NIC_ABSENT, &info);
    CHECK(status == SS_NOSUCHDEV,
          "a second Ethernet unit ETH1: does NOT exist -- no fabricated NIC (INV-6)");

    /* ETH0: is enumerated by the same scan as the console. */
    {
        uint32_t nidx = 0, saw_nic = 0;
        for (;;) {
            memset(&info, 0, sizeof(info));
            status = vms_kif_devscan(&nidx, &info);
            if (status != SS_NORMAL)
                break;
            if (strcmp(info.devnam, NIC_DEV) == 0) {
                saw_nic = 1;
                CHECK(info.devclass == DC_SCOM,
                      "device scan reports ETH0: with the LAN class (DC$_SCOM)");
            }
            if (nidx > 64)
                break;
        }
        CHECK(saw_nic, "device scan lists the NIC ETH0:");
    }

    /* $ASSIGN ETH0: succeeds and -- because a LAN controller is a
     * SHAREABLE device -- a bare channel confers NO ownership. This is
     * the shareable side of the ownership rule the console's shareable=0
     * comment said the first shareable device owes the suite (measured on
     * NLA0:, docs/oracle/vax73-terminal-device.md §7): OPA0: above took an
     * owner on $ASSIGN; ETH0: must not. Read back BY NAME, i.e. from the
     * executive's table, not from this process's channel. */
    {
        uint32_t nic_chan = 0;
        status = vms_kif_assign(NIC_DEV, &nic_chan);
        CHECK(status == SS_NORMAL && nic_chan != 0,
              "$ASSIGN ETH0: succeeds (a program can take a channel to the NIC)");

        memset(&info, 0, sizeof(info));
        status = vms_kif_getdvi_devnam(NIC_DEV, &info);
        CHECK(status == SS_NORMAL && info.owner_pid == 0,
              "a channel to the shareable ETH0: confers NO ownership (owner stays unowned)");

        /* $GETDVI through the assigned channel resolves to the same row. */
        memset(&info, 0, sizeof(info));
        status = vms_kif_getdvi_chan(nic_chan, &info);
        CHECK(status == SS_NORMAL && info.devclass == DC_SCOM &&
              strcmp(info.devnam, NIC_DEV) == 0,
              "$GETDVI on the ETH0: channel returns the LAN device row");

        vms_kif_dassgn(nic_chan);
    }

    /* --------------------------------------------------------------
     * 3. A-writes / B-reads. A different process assigns the console,
     *    takes ownership and changes its characteristics. This process
     *    did none of that and must see all of it.
     * -------------------------------------------------------------- */
    if (pipe(pipefd) < 0 || pipe(gofd) < 0) {
        printf("  FAIL: pipe()\n");
        return 1;
    }

    child = fork();
    if (child < 0) {
        printf("  FAIL: fork()\n");
        return 1;
    }

    if (child == 0) {
        close(pipefd[0]);
        close(gofd[1]);
        /* Take our own channel rather than the inherited descriptor. */
        vms_kif_close();
        snprintf(wfd_arg, sizeof(wfd_arg), "%d", pipefd[1]);
        snprintf(rfd_arg, sizeof(rfd_arg), "%d", gofd[0]);
        execl(argv[0], argv[0], "--owner", wfd_arg, rfd_arg, (char *)NULL);
        _exit(73);
    }

    close(pipefd[1]);
    close(gofd[0]);
    memset(&rep, 0, sizeof(rep));
    if (read(pipefd[0], &rep, sizeof(rep)) != (ssize_t)sizeof(rep)) {
        printf("  FAIL: owner process never reported\n");
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        printf("=== test_kmod_devtab: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    CHECK(rep.assign_status == SS_NORMAL, "another process assigns a channel to OPA0:");

    /* --------------------------------------------------------------
     * 3a. A HOLDS ONLY A CHANNEL. Nothing in the system is allocated.
     *
     *  ORACLE (VAX 7.3, node VAX2, /tmp/clean-vax1-test/vax2.log):
     *    TTA0: -- a TERMINAL, whose SHOW DEVICE/FULL status clause
     *    carries no "shareable" -- went from
     *      "Owner process """ / "Reference count 0"          (l.1121,1123)
     *    to
     *      "Owner process "SYSTEM"" / "Owner process ID 20400216"
     *      / "Reference count 1"                             (l.1136-1138)
     *    on a bare OPEN/WRITE (l.1127), with its status clause still
     *    reading "is online, record-oriented device, carriage control"
     *    -- no "allocated" (l.1132). A DEALLOCATE at that instant was
     *    refused "%SYSTEM-W-DEVNOTALLOC, device not allocated" (l.1143).
     *
     *    The counter-case pins the criterion: the identical sequence on
     *    NLA0:, "shareable, mailbox device" (l.1176), left
     *    "Owner process """ (l.1179). Shareability is what decides,
     *    which is why the assertions below are about OPA0:, a terminal,
     *    and not about the null device.
     * -------------------------------------------------------------- */
    /* negctl: devtab-owner-not-recorded */
    CHECK(rep.owner_after_assign == rep.own_vms_pid,
          "oracle: $ASSIGN to a non-shareable device nobody owns makes the caller its owner");
    CHECK(rep.alloc_after_assign == 0,
          "oracle: ownership by channel is not an allocation");
    CHECK(rep.refcnt_after_assign == 1,
          "oracle: ownership by channel costs one reference, not two");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(CONSOLE, &info);
    /* negctl: devtab-owner-not-recorded */
    CHECK(status == SS_NORMAL && info.owner_pid == rep.own_vms_pid,
          "B sees the ownership A took with a channel alone (A writes, B reads)");
    CHECK(info.allocated == 0, "B sees that nothing is allocated");

    /*
     * THE ISOLATING CASE for the $ALLOC refusal, and the one the old
     * foreign-channel rule was invented for. Measured directly: with
     * the detached process CHANHOLD holding one channel to TTA0: and no
     * allocation --
     *    "Owner process "CHANHOLD""                          (l.1005)
     *    "Reference count 1"                                 (l.1007)
     *    $ ALLOCATE TTA0:                                    (l.1009)
     *    %SYSTEM-W-DEVALLOC, device already allocated to another user
     *                                                        (l.1010)
     */
    status = vms_kif_alloc(CONSOLE);
    CHECK(status == SS_DEVALLOC,
          "oracle: $ALLOC is refused while another process owns the device by channel alone");
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getdvi_devnam(CONSOLE, &info);
    /* negctl-knockon: devtab-owner-not-recorded */
    CHECK(info.owner_pid == rep.own_vms_pid && info.allocated == 0,
          "the refused $ALLOC neither took ownership nor allocated anything");

    /* And $DALLOC of a device owned by channel is DEVNOTALLOC (l.1143). */
    status = vms_kif_dalloc(CONSOLE);
    CHECK(status == SS_DEVNOTALLOC,
          "oracle: $DALLOC of a device nobody has allocated returns SS$_DEVNOTALLOC");

    /* --------------------------------------------------------------
     * 3b. Now let A allocate. Synchronised on B's word, not on a timer.
     * -------------------------------------------------------------- */
    if (write(gofd[1], "g", 1) != 1) {
        printf("  FAIL: could not release the owner process\n");
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return 1;
    }
    memset(&rep, 0, sizeof(rep));
    if (read(pipefd[0], &rep, sizeof(rep)) != (ssize_t)sizeof(rep)) {
        printf("  FAIL: owner process never reported phase 2\n");
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        printf("=== test_kmod_devtab: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    CHECK(rep.setmode_status == SS_NORMAL, "owner sets terminal characteristics");

    /* ORACLE: ALLOCATE by the process that already owned OPA0: by
     * channel added the word "allocated" and took the reference count
     * 2 -> 3 (l.670-682); doing it twice changed nothing further
     * (3 -> 3). */
    CHECK(rep.alloc_status == SS_NORMAL, "$ALLOC of a device we own by channel succeeds");
    CHECK(rep.realloc_status == SS_NORMAL,
          "$ALLOC of a device we already have allocated succeeds");
    CHECK(rep.owner_pid == rep.own_vms_pid, "executive records the allocating process as owner");
    /* negctl: devtab-alloc-not-recorded */
    CHECK(rep.allocated == 1, "device reports itself allocated");
    CHECK(rep.refcnt == 2,
          "reference count is one channel plus the allocation, and re-allocating adds none");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(status == SS_NORMAL, "this process can still read the device");
    CHECK(info.owner_pid == rep.own_vms_pid,
          "B sees the owner A took (A writes, B reads)");
    /* negctl: devtab-alloc-not-recorded */
    CHECK(info.allocated == 1, "B sees that A allocated the device");
    CHECK(info.refcnt == 2, "B sees A's reference count");
    CHECK(info.width == A_WIDTH && info.page == A_PAGE,
          "B sees the width and page A set");
    CHECK((info.devchar & VMS_TTC_PASTHRU) != 0,
          "B sees the characteristic A set (Pasthru)");
    CHECK((info.devchar & VMS_TTC_ECHO) == 0,
          "B sees the characteristic A cleared (No Echo)");

    /* --------------------------------------------------------------
     * 4. What a SECOND process may and may not do to a device another
     *    process owns. Both answers are measured on the ~/vax OpenVMS
     *    VAX V7.3 lab, not chosen (docs/oracle/vax73-terminal-device.md
     *    section 7):
     *
     *      a detached process, $ASSIGN OPA0:  -> %SYSTEM-S-NORMAL (l.536)
     *      the same process,   ALLOCATE OPA0: -> %SYSTEM-W-DEVALLOC,
     *                    device already allocated to another user (l.548)
     *
     *    So a terminal owned by somebody else is assignable but not
     *    allocatable. The console stayed Owner "SYSTEM" / 20400216
     *    across that foreign $ASSIGN (l.531-533 vs l.543-545), so the
     *    channel did not move ownership; note honestly that the
     *    reference count was back to 2 by the time SHOW DEVICE ran, so
     *    what that pins is "ownership did not transfer", not "a foreign
     *    channel was still open".
     * -------------------------------------------------------------- */
    status = vms_kif_assign(CONSOLE, &chan);
    CHECK(status == SS_NORMAL && chan != 0,
          "oracle: $ASSIGN to a device another process owns returns SS$_NORMAL");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_chan(chan, &info);
    CHECK(status == SS_NORMAL && info.owner_pid == rep.own_vms_pid,
          "oracle: $ASSIGN to an owned device does not transfer ownership");

    status = vms_kif_alloc(CONSOLE);
    CHECK(status == SS_DEVALLOC,
          "oracle: $ALLOC of a device another process has allocated returns SS$_DEVALLOC");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_chan(chan, &info);
    CHECK(status == SS_NORMAL && info.refcnt == 3,
          "device reference count counts both processes' channels and the allocation");
    CHECK(info.owner_pid == rep.own_vms_pid,
          "a refused $ALLOC leaves the existing owner in place");

    /* Deallocating something we never allocated is DEVNOTALLOC, as it
     * is on the lab for a second DEALLOCATE. */
    status = vms_kif_dalloc(CONSOLE);
    CHECK(status == SS_DEVNOTALLOC,
          "oracle: $DALLOC of a device we do not have allocated returns SS$_DEVNOTALLOC");
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(info.owner_pid == rep.own_vms_pid,
          "a refused $DALLOC does not release somebody else's allocation");

    /* A failed $ASSIGN must not disturb the channel we already hold. */
    bogus_chan = chan;
    status = vms_kif_assign(ABSENT_DEV, &bogus_chan);
    CHECK(status == SS_NOSUCHDEV, "assigning an absent device fails with SS$_NOSUCHDEV");
    CHECK(bogus_chan == chan, "failed $ASSIGN leaves the caller's channel untouched");
    status = vms_kif_alloc(ABSENT_DEV);
    CHECK(status == SS_NOSUCHDEV, "allocating an absent device fails with SS$_NOSUCHDEV");

    /* --------------------------------------------------------------
     * 5. The device outlives its owner. When A dies the executive takes
     *    back everything A held -- the device is the executive's, not
     *    A's. ORACLE: STOP CHANHOLD put TTA0: back to Owner "" with a
     *    reference count of 0 (l.1036-1038).
     *
     *    B gives its own channel back FIRST, so that when A dies nobody
     *    else is holding the console. Whether a surviving channel-holder
     *    would inherit ownership was never measured, and this test does
     *    not assert an answer to it.
     * -------------------------------------------------------------- */
    status = vms_kif_dassgn(chan);
    /* negctl: devtab-dassgn-status-wrong */
    CHECK(status == SS_NORMAL, "channel deassigned");
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(info.refcnt == 2 && info.owner_pid == rep.own_vms_pid,
          "giving our channel back drops one reference and leaves the owner alone");

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(CONSOLE, &info);
    /* negctl: devtab-getdvi-devnam-status-wrong */
    CHECK(status == SS_NORMAL, "device still exists after its owner dies");
    CHECK(info.owner_pid == 0, "dead process no longer owns the device");
    CHECK(info.allocated == 0, "dead process's allocation was released");
    CHECK(info.refcnt == 0, "dead process's channel and allocation were both released");
    CHECK(info.width == A_WIDTH && info.page == A_PAGE,
          "characteristics set by the dead process persist in the executive");

    /* --------------------------------------------------------------
     * 6. The whole ownership life cycle in ONE process, in the order
     *    the oracle showed it on TTA0: and OPA0:.
     * -------------------------------------------------------------- */
    status = vms_kif_assign(CONSOLE, &chan);
    CHECK(status == SS_NORMAL, "console is assignable again once its owner is gone");
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getdvi_devnam(CONSOLE, &info);
    /* negctl-knockon: devtab-owner-not-recorded */
    CHECK(info.owner_pid == self_vms_pid && info.allocated == 0 && info.refcnt == 1,
          "oracle: a channel to the free console makes us its owner, unallocated (TTA0: l.1136-1138)");

    status = vms_kif_alloc(CONSOLE);
    CHECK(status == SS_NORMAL, "device is allocatable once its owner is gone");
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getdvi_devnam(CONSOLE, &info);
    /* negctl: devtab-alloc-not-recorded */
    CHECK(info.owner_pid == self_vms_pid && info.allocated == 1 && info.refcnt == 2,
          "oracle: allocating what we already own adds the allocation and one reference (OPA0: 2 -> 3, l.682)");

    status = vms_kif_dalloc(CONSOLE);
    CHECK(status == SS_NORMAL, "$DALLOC gives the allocation back");
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(info.allocated == 0 && info.refcnt == 1,
          "oracle: $DALLOC drops the allocation and its reference (OPA0: 3 -> 2, l.695)");
    CHECK(info.owner_pid == self_vms_pid,
          "oracle: $DALLOC does NOT unown a device we still hold a channel to (OPA0: still Owner \"SYSTEM\", l.693)");

    status = vms_kif_dassgn(chan);
    CHECK(status == SS_NORMAL, "last channel deassigned");
    status = vms_kif_dassgn(chan);
    CHECK(status == SS_IVCHAN, "deassigning a released channel reports SS$_IVCHAN");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(status == SS_NORMAL && info.refcnt == 0,
          "reference count returns to zero");
    CHECK(info.owner_pid == 0,
          "oracle: returning the last channel unowns the device (TTA0: CLOSE -> Owner \"\", l.1165)");

    vms_kif_close();

    printf("=== test_kmod_devtab: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
