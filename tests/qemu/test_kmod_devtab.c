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
#define SS_DEVALLOC     2112    /* oracle-measured; see ssdef.h provenance */
#define SS_DEVNOTALLOC  2136

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
    uint32_t alloc_status;
    uint32_t realloc_status;    /* allocating again, already ours */
    uint32_t chan;
    uint32_t owner_after_assign;/* owner the executive shows after $ASSIGN alone */
    uint32_t refcnt_after_assign;
    uint32_t owner_pid;         /* owner after $ALLOC */
    uint32_t refcnt;            /* reference count after $ALLOC */
    uint32_t allocated;
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
 * process_a - assigns a channel to the console, allocates it (which is
 * what makes it the owner -- $ASSIGN does not, per the oracle), and
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
        /* After $ASSIGN alone: the oracle says we are NOT the owner. */
        memset(&info, 0, sizeof(info));
        if (vms_kif_getdvi_chan(rep.chan, &info) == SS_NORMAL) {
            rep.owner_after_assign = info.owner_pid;
            rep.refcnt_after_assign = info.refcnt;
        }

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

    /* ORACLE (VAX 7.3, NLA0: held open by one process): a channel does
     * not confer ownership. The owner field stayed empty and the
     * process ID stayed 00000000 for as long as the channel was held;
     * only the reference count moved. */
    CHECK(rep.owner_after_assign == 0,
          "$ASSIGN alone does not make the caller the device's owner");
    CHECK(rep.refcnt_after_assign == 1,
          "$ASSIGN alone adds one to the reference count");

    /* ORACLE: ALLOCATE is what sets Owner process / Owner process ID
     * and adds the word "allocated" to SHOW DEVICE/FULL, and doing it
     * twice changes nothing further (OPA0: 2 -> 3 -> 3). */
    CHECK(rep.alloc_status == SS_NORMAL, "$ALLOC of a free device succeeds");
    CHECK(rep.realloc_status == SS_NORMAL,
          "$ALLOC of a device we already have allocated succeeds");
    CHECK(rep.owner_pid == (uint32_t)child, "executive records the allocating process as owner");
    CHECK(rep.allocated == 1, "device reports itself allocated");
    CHECK(rep.refcnt == 2,
          "reference count is one channel plus the allocation, and re-allocating adds none");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(status == SS_NORMAL, "this process can still read the device");
    CHECK(info.owner_pid == (uint32_t)child,
          "B sees the owner A took (A writes, B reads)");
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
     *      a detached process, $ASSIGN OPA0:  -> %SYSTEM-S-NORMAL
     *      the same process,   ALLOCATE OPA0: -> %SYSTEM-W-DEVALLOC,
     *                             device already allocated to another user
     *
     *    So a terminal owned by somebody else is assignable but not
     *    allocatable. Neither line below is OVMX's opinion.
     * -------------------------------------------------------------- */
    status = vms_kif_assign(CONSOLE, &chan);
    CHECK(status == SS_NORMAL && chan != 0,
          "oracle: $ASSIGN to a device another process owns returns SS$_NORMAL");

    status = vms_kif_alloc(CONSOLE);
    CHECK(status == SS_DEVALLOC,
          "oracle: $ALLOC of a device another process owns returns SS$_DEVALLOC");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_chan(chan, &info);
    CHECK(status == SS_NORMAL && info.refcnt == 3,
          "device reference count counts both processes' channels and the allocation");
    CHECK(info.owner_pid == (uint32_t)child,
          "a refused $ALLOC leaves the existing owner in place");

    /* Deallocating something we never allocated is DEVNOTALLOC, as it
     * is on the lab for a second DEALLOCATE. */
    status = vms_kif_dalloc(CONSOLE);
    CHECK(status == SS_DEVNOTALLOC,
          "oracle: $DALLOC of a device we do not have allocated returns SS$_DEVNOTALLOC");
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(info.owner_pid == (uint32_t)child,
          "a refused $DALLOC does not release somebody else's allocation");

    /* A failed $ASSIGN must not disturb the channel we already hold. */
    bogus_chan = chan;
    status = vms_kif_assign(ABSENT_DEV, &bogus_chan);
    CHECK(status == SS_NOSUCHDEV, "assigning an absent device fails with SS$_NOSUCHDEV");
    CHECK(bogus_chan == chan, "failed $ASSIGN leaves the caller's channel untouched");
    status = vms_kif_alloc(ABSENT_DEV);
    CHECK(status == SS_NOSUCHDEV, "allocating an absent device fails with SS$_NOSUCHDEV");

    /* --------------------------------------------------------------
     * 5. The device outlives its owner. When A dies the executive
     *    takes the allocation back -- the device is the executive's,
     *    not A's. (A device left allocated to a process that no longer
     *    exists is not a state VMS has.)
     * -------------------------------------------------------------- */
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    memset(&info, 0, sizeof(info));
    status = vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(status == SS_NORMAL, "device still exists after its owner dies");
    CHECK(info.owner_pid == 0, "dead process no longer owns the device");
    CHECK(info.allocated == 0, "dead process's allocation was released");
    CHECK(info.refcnt == 1, "dead process's channel and allocation were both released");
    CHECK(info.width == A_WIDTH && info.page == A_PAGE,
          "characteristics set by the dead process persist in the executive");

    /* Now that A is gone the device is free, so B can take it. */
    status = vms_kif_alloc(CONSOLE);
    CHECK(status == SS_NORMAL, "device is allocatable again once its owner is gone");
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(info.owner_pid == (uint32_t)getpid() && info.allocated == 1,
          "executive records the new owner");
    status = vms_kif_dalloc(CONSOLE);
    CHECK(status == SS_NORMAL, "$DALLOC gives the device back");
    memset(&info, 0, sizeof(info));
    (void)vms_kif_getdvi_devnam(CONSOLE, &info);
    CHECK(info.owner_pid == 0 && info.allocated == 0 && info.refcnt == 1,
          "$DALLOC clears the owner and drops its reference");

    /* --------------------------------------------------------------
     * 6. Giving a channel back releases the reference.
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
