/*
 * test_kmod_procnam.c - Executive process table: A-writes / B-reads (vms-8019)
 *
 * A VMS process name is only a process name because OTHER processes can
 * see it. A name a process keeps in its own address space passes every
 * single-process test perfectly and is still a facade -- it reports
 * success while sharing nothing. So this test is built around the only
 * check that can tell the difference:
 *
 *   PROCESS A WRITES THE NAME, PROCESS B READS IT BACK.
 *
 * and around the second property that made the previous OVMX process
 * name meaningless -- it died at exec():
 *
 *   THE NAMING PROCESS CLOSES ITS /dev/vms CHANNEL AND execve()s A
 *   FRESH IMAGE, WHICH THEN ASKS THE EXECUTIVE FOR ITS OWN NAME.
 *
 * Nothing is carried across exec in userspace: no environment
 * variable, no inherited descriptor, no file. The post-exec image opens
 * a brand new channel and the executive still knows who it is, because
 * the name lives in the executive's process table keyed by a pid that
 * execve() does not change.
 *
 * This test drives the real userspace client (src/libvmssys/vms_kif.c)
 * against a real /dev/vms, not a hand-rolled ioctl copy, so the client
 * that libvms will call is the client under test.
 *
 * Modes:
 *   (no args)                   parent / process B
 *   --named-child <pipe-wfd>    the re-execed image / process A's image
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "vms_kif.h"

#define SS_NORMAL       1
#define SS_DUPLNAM      434
#define SS_NONEXPR      2540

#define CHILD_NAME      "VMS$WORKER"
#define PARENT_NAME     "VMS$PARENT"
#define ABSENT_NAME     "VMS$NOSUCH"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What the re-execed image reports back to the parent over a pipe. */
struct child_report {
    uint32_t getjpi_status;             /* status of its GETJPI(self) */
    uint32_t linux_pid;                 /* pid the executive has for it */
    char     prcnam[VMS_PRCNAM_SIZE];   /* name the executive has for it */
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
 * child_after_exec - the freshly activated image.
 *
 * It was named before exec by an image that no longer exists, over a
 * channel that was closed before exec. If it can still learn its own
 * name, the name is genuinely executive-resident.
 */
static int child_after_exec(int wfd)
{
    struct child_report rep;
    struct vms_procinfo info;

    memset(&rep, 0, sizeof(rep));

    if (vms_kif_open() < 0) {
        rep.getjpi_status = 0; /* 0 is not a VMS status: "never got there" */
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }

    /* Deliberately NO register() call: this image was never registered.
     * Its predecessor was, and the executive owns that registration. */
    memset(&info, 0, sizeof(info));
    rep.getjpi_status = vms_kif_getjpi_self(&info);
    rep.linux_pid = info.linux_pid;
    memcpy(rep.prcnam, info.prcnam, VMS_PRCNAM_SIZE);

    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        return 1;
    close(wfd);

    /* Stay alive and named so the parent can look us up. The parent
     * kills us when it is done. */
    for (;;)
        pause();

    return 0;
}

int main(int argc, char **argv)
{
    int pipefd[2];
    pid_t child;
    struct child_report rep;
    struct vms_procinfo info;
    uint32_t status, index, scanned, saw_child, saw_parent;
    char wfd_arg[16];

    if (argc >= 3 && strcmp(argv[1], "--named-child") == 0)
        return child_after_exec(atoi(argv[2]));

    printf("=== test_kmod_procnam: executive process table ===\n");

    if (open_and_register() < 0) {
        printf("=== test_kmod_procnam: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* --------------------------------------------------------------
     * 0. Negative control: the name must not resolve before anyone
     *    sets it. Without this, a lookup that always succeeds would
     *    look identical to a lookup that works.
     * -------------------------------------------------------------- */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_prcnam(CHILD_NAME, &info);
    CHECK(status == SS_NONEXPR, "unset name does not resolve (SS$_NONEXPR)");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_prcnam(ABSENT_NAME, &info);
    CHECK(status == SS_NONEXPR, "unknown name does not resolve (SS$_NONEXPR)");

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
        /* ---------- PROCESS A: writes the name, then vanishes ---------- */
        close(pipefd[0]);

        /* Drop the descriptor inherited from the parent and take our
         * own channel, so that closing it below actually releases the
         * underlying file and exercises vms_dev_release(). */
        vms_kif_close();
        if (vms_kif_open() < 0)
            _exit(70);
        if (vms_kif_register((uint32_t)getpid(), 0) != SS_NORMAL)
            _exit(71);
        if (vms_kif_setprn(CHILD_NAME) != SS_NORMAL)
            _exit(72);

        /* Close the channel BEFORE exec. If the PCB were owned by the
         * channel instead of by the process, the name would die here. */
        vms_kif_close();

        snprintf(wfd_arg, sizeof(wfd_arg), "%d", pipefd[1]);
        execl(argv[0], argv[0], "--named-child", wfd_arg, (char *)NULL);
        _exit(73);
    }

    /* ---------- PROCESS B: reads the name process A wrote ---------- */
    close(pipefd[1]);

    memset(&rep, 0, sizeof(rep));
    if (read(pipefd[0], &rep, sizeof(rep)) != (ssize_t)sizeof(rep)) {
        printf("  FAIL: re-execed child never reported (exec or open failed)\n");
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        printf("=== test_kmod_procnam: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* 1. The re-execed image, which never registered and never carried
     *    anything across exec, learned its own name from the executive. */
    CHECK(rep.getjpi_status == SS_NORMAL,
          "re-execed image resolves itself in the executive process table");
    CHECK(strcmp(rep.prcnam, CHILD_NAME) == 0,
          "process name survives execve() with no userspace carrier");
    CHECK(rep.linux_pid == (uint32_t)child,
          "executive row for the re-execed image has the right pid");

    /* 2. A-writes / B-reads: a DIFFERENT process resolves the name. */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_prcnam(CHILD_NAME, &info);
    CHECK(status == SS_NORMAL,
          "another process resolves the name (A writes, B reads)");
    CHECK(info.linux_pid == (uint32_t)child,
          "name resolves to the naming process's pid");

    /* 3. Resolution by VMS PID reaches the same row. */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_pid((uint32_t)child, &info);
    CHECK(status == SS_NORMAL && strcmp(info.prcnam, CHILD_NAME) == 0,
          "lookup by PID returns the same name");

    /* 4. Uniqueness is enforced across processes, not within one. */
    status = vms_kif_setprn(CHILD_NAME);
    CHECK(status == SS_DUPLNAM,
          "duplicate process name rejected with SS$_DUPLNAM");

    status = vms_kif_setprn(PARENT_NAME);
    CHECK(status == SS_NORMAL, "distinct process name accepted");

    /* 5. The table enumerates more than the calling process -- the
     *    reader SHOW SYSTEM needs. */
    scanned = 0;
    saw_child = 0;
    saw_parent = 0;
    index = 0;
    for (;;) {
        memset(&info, 0, sizeof(info));
        status = vms_kif_procscan(&index, &info);
        if (status != SS_NORMAL)
            break;
        scanned++;
        if (strcmp(info.prcnam, CHILD_NAME) == 0)
            saw_child = 1;
        if (strcmp(info.prcnam, PARENT_NAME) == 0)
            saw_parent = 1;
        if (scanned > 64)
            break;
    }
    CHECK(status == SS_NONEXPR, "process scan terminates with SS$_NONEXPR");
    CHECK(scanned >= 2, "process scan enumerates more than the caller");
    CHECK(saw_child && saw_parent,
          "process scan lists both the caller and another process by name");

    /* 6. A name belongs to a live process: when the process dies the
     *    executive gives the name up. */
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_prcnam(CHILD_NAME, &info);
    CHECK(status == SS_NONEXPR, "name released when the process exits");

    status = vms_kif_setprn(CHILD_NAME);
    CHECK(status == SS_NORMAL, "released name is available again");

    vms_kif_close();

    printf("=== test_kmod_procnam: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
