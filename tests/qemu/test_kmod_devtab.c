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
 * Modes:
 *   (no args)          process B (parent)
 *   --owner <wfd>      process A: assigns OPA0:, changes it, reports
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

#define DC_TERM         6

#define CONSOLE         "OPA0:"
#define ABSENT_DEV      "ZZA0:"

/* What process A changes the console to, so B can look for it. */
#define A_WIDTH         64
#define A_PAGE          40

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What process A reports back over a pipe. */
struct owner_report {
    uint32_t assign_status;
    uint32_t setmode_status;
    uint32_t chan;
    uint32_t owner_pid;     /* owner the executive shows A after assign */
    uint32_t refcnt;
};

static int open_and_register(void)
{
    if (vms_kif_open() < 0) {
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        return -1;
    }
    if (vms_kif_register((uint32_t)getpid(), 0) != SS_NORMAL) {
        printf("  FAIL: VMS_IOCTL_REGISTER rejected\n");
        return -1;
    }
    return 0;
}

/*
 * process_a - assigns a channel to the console, becomes its owner, and
 * changes characteristics that it never tells anyone about except
 * through the executive.
 */
static int process_a(int wfd)
{
    struct owner_report rep;
    struct vms_devinfo info;

    memset(&rep, 0, sizeof(rep));

    if (vms_kif_open() < 0) {
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }
    if (vms_kif_register((uint32_t)getpid(), 0) != SS_NORMAL) {
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }

    rep.assign_status = vms_kif_assign(CONSOLE, &rep.chan);
    if (rep.assign_status == SS_NORMAL) {
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
    int pipefd[2];
    pid_t child;
    struct owner_report rep;
    struct vms_devinfo info;
    uint32_t status, chan = 0, bogus_chan, index, scanned, saw_console;
    char wfd_arg[16];

    if (argc >= 3 && strcmp(argv[1], "--owner") == 0)
        return process_a(atoi(argv[2]));

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
    CHECK(status == SS_NOMOREDEV, "device scan terminates with SS$_NOMOREDEV");
    CHECK(saw_console, "device scan lists the console terminal");

    /* --------------------------------------------------------------
     * 3. A-writes / B-reads. A different process assigns the console,
     *    takes ownership and changes its characteristics. This process
     *    did none of that and must see all of it.
     * -------------------------------------------------------------- */
    if (pipe(pipefd) < 0) {
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
        /* Take our own channel rather than the inherited descriptor. */
        vms_kif_close();
        snprintf(wfd_arg, sizeof(wfd_arg), "%d", pipefd[1]);
        execl(argv[0], argv[0], "--owner", wfd_arg, (char *)NULL);
        _exit(73);
    }

    close(pipefd[1]);
    memset(&rep, 0, sizeof(rep));
    if (read(pipefd[0], &rep, sizeof(rep)) != (ssize_t)sizeof(rep)) {
        printf("  FAIL: owner process never reported\n");
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        printf("=== test_kmod_devtab: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    CHECK(rep.assign_status == SS_NORMAL, "another process assigns a channel to OPA0:");
    CHECK(rep.setmode_status == SS_NORMAL, "owner sets terminal characteristics");
    CHECK(rep.owner_pid == (uint32_t)child, "executive records the assigning process as owner");
    CHECK(rep.refcnt == 1, "executive counts one channel to the device");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(status == SS_NORMAL, "this process can still read the device");
    CHECK(info.owner_pid == (uint32_t)child,
          "B sees the owner A took (A writes, B reads)");
    CHECK(info.refcnt == 1, "B sees A's reference count");
    CHECK(info.width == A_WIDTH && info.page == A_PAGE,
          "B sees the width and page A set");
    CHECK((info.devchar & VMS_TTC_PASTHRU) != 0,
          "B sees the characteristic A set (Pasthru)");
    CHECK((info.devchar & VMS_TTC_ECHO) == 0,
          "B sees the characteristic A cleared (No Echo)");

    /* --------------------------------------------------------------
     * 4. B assigns the same device: the device is shared, and B does
     *    NOT displace A as its owner.
     * -------------------------------------------------------------- */
    status = vms_kif_assign(CONSOLE, &chan);
    CHECK(status == SS_NORMAL && chan != 0, "B assigns its own channel to the same device");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_chan(chan, &info);
    CHECK(status == SS_NORMAL && info.refcnt == 2,
          "device reference count counts both processes");
    CHECK(info.owner_pid == (uint32_t)child,
          "a second assigner does not steal ownership");

    /* A failed $ASSIGN must not disturb the channel we already hold. */
    bogus_chan = chan;
    status = vms_kif_assign(ABSENT_DEV, &bogus_chan);
    CHECK(status == SS_NOSUCHDEV, "assigning an absent device fails with SS$_NOSUCHDEV");
    CHECK(bogus_chan == chan, "failed $ASSIGN leaves the caller's channel untouched");

    /* --------------------------------------------------------------
     * 5. The device outlives its owner. When A dies the executive
     *    takes the ownership back -- the device is the executive's,
     *    not A's.
     * -------------------------------------------------------------- */
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(status == SS_NORMAL, "device still exists after its owner dies");
    CHECK(info.owner_pid == 0, "dead process no longer owns the device");
    CHECK(info.refcnt == 1, "dead process's channel was released");
    CHECK(info.width == A_WIDTH && info.page == A_PAGE,
          "characteristics set by the dead process persist in the executive");

    /* --------------------------------------------------------------
     * 6. Giving a channel back releases the reference and, with it,
     *    ownership of the device.
     * -------------------------------------------------------------- */
    status = vms_kif_dassgn(chan);
    CHECK(status == SS_NORMAL, "channel deassigned");
    status = vms_kif_dassgn(chan);
    CHECK(status == SS_IVCHAN, "deassigning a released channel reports SS$_IVCHAN");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(status == SS_NORMAL && info.refcnt == 0,
          "reference count returns to zero");
    CHECK(info.owner_pid == 0, "device is unowned once the last channel is gone");

    vms_kif_close();

    printf("=== test_kmod_devtab: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
