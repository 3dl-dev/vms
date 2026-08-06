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

/*
 * Status values are ORACLE-PINNED (vms-8019) on the reference lab
 * OpenVMS VAX V7.3 node VAX1: $SSDEF extracted from
 * SYS$LIBRARY:STARLET.MLB gives SS$_DUPLNAM 148 and SS$_NONEXPR 2280,
 * and F$MESSAGE round-trips them to %SYSTEM-F-DUPLNAM / %SYSTEM-W-NONEXPR.
 */
#define SS_NORMAL       1
#define SS_DUPLNAM      148
#define SS_IVLOGNAM     340
#define SS_NONEXPR      2280

#define CHILD_NAME      "VMS$WORKER"
#define PARENT_NAME     "VMS$PARENT"
#define ABSENT_NAME     "VMS$NOSUCH"
#define ZERO_NAME       "VMS$GRP0"
#define G300_NAME       "VMS$GRP300"

/*
 * Name-length boundary pair. LEN16_NAME is LEN15_NAME plus one
 * character, so a truncating implementation turns the illegal name into
 * the legal one and answers SS$_NORMAL for both.
 */
#define LEN15_NAME      "VMS$TRUNCBAIT12"     /* 15 chars: the VMS maximum */
#define LEN16_NAME      "VMS$TRUNCBAIT123"    /* 16 chars: one too many */

/*
 * UIC group used by the cross-group case. OVMX maps UIC [group,member]
 * onto the task's [gid,uid], so a helper that setgid()s to this value
 * lands in a different UIC group from the root parent (group 0) while
 * remaining uid 0 -- it keeps the privilege needed to open /dev/vms,
 * which a full setuid() would drop.
 */
#define ALT_UIC_GROUP   300

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What the re-execed image reports back to the parent over a pipe. */
struct child_report {
    uint32_t getjpi_status;             /* status of its GETJPI(self) */
    uint32_t linux_pid;                 /* pid the executive has for it */
    /* The VMS process ID the EXECUTIVE assigned (vms-2b8). The parent
     * used to look the child up by its Linux pid, which only worked
     * because the VMS pid was a copy of it -- i.e. because userspace
     * chose the key. The executive chooses it now, so the only way to
     * know it is to be told. */
    uint32_t vms_pid;
    char     prcnam[VMS_PRCNAM_SIZE];   /* name the executive has for it */
};

/* What the cross-UIC-group helper reports back over a pipe. */
struct group_report {
    uint32_t setprn_shared;     /* SETPRN of a name group 0 already holds */
    uint32_t lookup_status;     /* GETJPI(prcnam) of that shared name */
    uint32_t lookup_pid;        /* ... and the pid it resolved to */
    uint32_t setprn_private;    /* SETPRN of a name only this group holds */
    uint32_t own_uic;           /* the UIC the executive derived for us */
};

static int open_and_register(void)
{
    if (vms_kif_open() < 0) {
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        return -1;
    }
    if (vms_kif_register(NULL) != SS_NORMAL) {
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
    rep.vms_pid   = info.vms_pid;
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

/*
 * alt_group_helper - the other half of the UIC-group scoping case.
 *
 * Runs in a DIFFERENT UIC group from the parent (setgid before
 * registering, so the executive derives the new group from our
 * credentials -- we never get to declare it). It then does the two
 * things that a group-blind implementation cannot do:
 *
 *   - takes a process name the parent's group already holds, which
 *     must be ACCEPTED because uniqueness is scoped to the group; and
 *   - resolves that shared name to ITSELF, not to the parent's row.
 *
 * Finally it renames itself to a name that exists only in this group,
 * so the parent can prove it CANNOT see it from group 0.
 *
 * Called in a forked child; never returns.
 */
static void alt_group_helper(int wfd)
{
    struct group_report rep;
    struct vms_procinfo info;

    memset(&rep, 0, sizeof(rep));

    /* setgid() only: staying uid 0 keeps the privilege needed to open
     * /dev/vms, while moving us into UIC group ALT_UIC_GROUP. */
    if (setgid(ALT_UIC_GROUP) != 0)
        _exit(80);

    /* Drop the inherited descriptor and take our own channel. */
    vms_kif_close();
    if (vms_kif_open() < 0)
        _exit(81);
    if (vms_kif_register(NULL) != SS_NORMAL)
        _exit(82);

    memset(&info, 0, sizeof(info));
    if (vms_kif_getjpi_self(&info) == SS_NORMAL)
        rep.own_uic = info.uic;

    rep.setprn_shared = vms_kif_setprn(ZERO_NAME);

    memset(&info, 0, sizeof(info));
    rep.lookup_status = vms_kif_getjpi_prcnam(ZERO_NAME, &info);
    rep.lookup_pid = info.linux_pid;

    rep.setprn_private = vms_kif_setprn(G300_NAME);

    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        _exit(83);
    close(wfd);

    for (;;)
        pause();
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
    /* negctl: proctab-getjpi-nonexpr-status-wrong */
    CHECK(status == SS_NONEXPR, "unset name does not resolve (SS$_NONEXPR)");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_prcnam(ABSENT_NAME, &info);
    /* negctl-knockon: proctab-getjpi-nonexpr-status-wrong */
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
        if (vms_kif_register(NULL) != SS_NORMAL)
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

    /* 3. Resolution by VMS PID reaches the same row. The key is the ID
     *    the EXECUTIVE assigned and reported, not the Linux pid this
     *    used to pass (vms-2b8). */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_pid(rep.vms_pid, &info);
    CHECK(status == SS_NORMAL && strcmp(info.prcnam, CHILD_NAME) == 0,
          "lookup by PID returns the same name");
    CHECK(rep.vms_pid != 0 && rep.vms_pid != (uint32_t)child,
          "the VMS process ID is the executive's, not a copy of the Linux pid");

    /* 4. Uniqueness is enforced across processes, not within one. */
    status = vms_kif_setprn(CHILD_NAME);
    /* negctl: proctab-duplicate-name */
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
    /* negctl: proctab-procscan-nonexpr-status-wrong */
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
    /* negctl-knockon: proctab-getjpi-nonexpr-status-wrong */
    CHECK(status == SS_NONEXPR, "name released when the process exits");

    status = vms_kif_setprn(CHILD_NAME);
    CHECK(status == SS_NORMAL, "released name is available again");

    /* --------------------------------------------------------------
     * 7. UIC-GROUP SCOPING.
     *
     * ORACLE-PINNED on the reference lab, OpenVMS VAX V7.3 node VAX1,
     * 2026-07-30 -- three detached processes, same process name,
     * different UICs:
     *
     *   RUN/DETACHED/UIC=[300,1]/PROCESS_NAME=OVMXDUP ...
     *     %RUN-S-PROC_ID, identification of created process is 20200220
     *   RUN/DETACHED/UIC=[301,1]/PROCESS_NAME=OVMXDUP ...
     *     %RUN-S-PROC_ID, identification of created process is 20200221
     *   RUN/DETACHED/UIC=[300,2]/PROCESS_NAME=OVMXDUP ...
     *     %RUN-F-CREPRC, process creation failed
     *     -SYSTEM-F-DUPLNAM, duplicate name
     *
     * i.e. the SAME name in a DIFFERENT group is accepted; the same
     * name in the SAME group is refused with SS$_DUPLNAM.
     *
     * Every process above this line runs as root, so uic_group is 0
     * everywhere and the group comparison is trivially true -- those
     * assertions cannot tell a group-scoped table from a global one.
     * The helper below is the only part of this test that can.
     * -------------------------------------------------------------- */
    {
        struct group_report grpt;
        int gpipe[2];
        pid_t helper;

        status = vms_kif_setprn(ZERO_NAME);
        CHECK(status == SS_NORMAL, "group 0 takes a name");

        if (pipe(gpipe) < 0) {
            printf("  FAIL: pipe() for group helper\n");
            fail++;
            goto done;
        }

        helper = fork();
        if (helper < 0) {
            printf("  FAIL: fork() for group helper\n");
            fail++;
            close(gpipe[0]);
            close(gpipe[1]);
            goto done;
        }
        if (helper == 0) {
            close(gpipe[0]);
            alt_group_helper(gpipe[1]);
            _exit(84);   /* not reached */
        }

        close(gpipe[1]);
        memset(&grpt, 0, sizeof(grpt));
        if (read(gpipe[0], &grpt, sizeof(grpt)) != (ssize_t)sizeof(grpt)) {
            printf("  FAIL: cross-UIC-group helper never reported\n");
            fail++;
            kill(helper, SIGKILL);
            waitpid(helper, NULL, 0);
            goto done;
        }

        /* The executive derived the helper's UIC from its credentials. */
        CHECK((grpt.own_uic >> 16) == (uint32_t)ALT_UIC_GROUP,
              "executive derives the helper's UIC group from its credentials");

        /* DISCRIMINATOR A: a name held in group 0 is free in group 300.
         * A group-blind table returns SS$_DUPLNAM here. */
        CHECK(grpt.setprn_shared == SS_NORMAL,
              "same process name in a different UIC group is accepted");

        /* DISCRIMINATOR B: the helper resolves the shared name to
         * itself, not to the group-0 holder. */
        CHECK(grpt.lookup_status == SS_NORMAL &&
              grpt.lookup_pid == (uint32_t)helper,
              "name lookup resolves within the caller's own UIC group");

        CHECK(grpt.setprn_private == SS_NORMAL,
              "helper renames itself to a group-private name");

        /* DISCRIMINATOR C: from group 0, a name that exists only in
         * group 300 must not resolve at all. A group-blind table
         * returns SS$_NORMAL here. */
        memset(&info, 0, sizeof(info));
        status = vms_kif_getjpi_prcnam(G300_NAME, &info);
        /* negctl-knockon: proctab-getjpi-nonexpr-status-wrong */
        CHECK(status == SS_NONEXPR,
              "a name held only in another UIC group does not resolve");

        /* Control: our own name in our own group still resolves, so a
         * failure above is scoping and not a broken lookup. */
        memset(&info, 0, sizeof(info));
        status = vms_kif_getjpi_prcnam(ZERO_NAME, &info);
        CHECK(status == SS_NORMAL && info.linux_pid == (uint32_t)getpid(),
              "our own name still resolves to us in our own group");

        kill(helper, SIGKILL);
        waitpid(helper, NULL, 0);
        close(gpipe[0]);
    }

    /* --------------------------------------------------------------
     * 8. THE NAME-LENGTH RULE IS THE EXECUTIVE'S, AND IT REJECTS --
     *    IT DOES NOT TRUNCATE.
     *
     * ORACLE-PINNED on the reference lab, OpenVMS VAX V7.3 node VAX1,
     * 2026-07-30:
     *
     *   $ SET PROCESS/NAME="IMPL8019NAM15X"      ! 14 chars
     *   $ WRITE SYS$OUTPUT F$GETJPI("","PRCNAM")
     *   IMPL8019NAM15X
     *   $ SET PROCESS/NAME="IMPL8019NAM15XY"     ! 15 chars
     *   $ WRITE SYS$OUTPUT F$GETJPI("","PRCNAM")
     *   IMPL8019NAM15XY
     *   $ SET PROCESS/NAME="IMPL8019NAM15XYZ"    ! 16 chars
     *   %SET-E-NOTSET, error modifying process name
     *   -SYSTEM-F-IVLOGNAM, invalid logical name
     *   $ WRITE SYS$OUTPUT F$GETJPI("","PRCNAM")
     *   IMPL8019NAM15XY                          ! UNCHANGED
     *
     * VMS refuses the oversized name outright and leaves the existing
     * name in place: it neither truncates it into a legal name nor
     * applies it partially. Truncating in the userspace client would
     * have produced SS$_NORMAL for the 16-character case and named the
     * process something the caller never asked for -- and, worse, made
     * an oversized LOOKUP key resolve a DIFFERENT process. These
     * assertions are what tell those two implementations apart.
     * -------------------------------------------------------------- */
    {
        char toolong[256];

        status = vms_kif_setprn(LEN15_NAME);
        CHECK(status == SS_NORMAL,
              "a 15-character name (the VMS maximum) is accepted");

        /* DISCRIMINATOR: with a truncating client this returns
         * SS$_NORMAL, because the clipped name is the one we already
         * hold and therefore clashes with nobody. */
        status = vms_kif_setprn(LEN16_NAME);
        CHECK(status == SS_IVLOGNAM,
              "a 16-character name is rejected with SS$_IVLOGNAM");

        memset(&info, 0, sizeof(info));
        status = vms_kif_getjpi_self(&info);
        CHECK(status == SS_NORMAL && strcmp(info.prcnam, LEN15_NAME) == 0,
              "the rejected name was not applied, not even truncated");

        /* Far past the userspace transfer buffer: clipping there must
         * still not manufacture a legal name. */
        memset(toolong, 'X', sizeof(toolong) - 1);
        toolong[sizeof(toolong) - 1] = '\0';
        status = vms_kif_setprn(toolong);
        CHECK(status == SS_IVLOGNAM,
              "a name longer than the transfer buffer is also rejected");

        memset(&info, 0, sizeof(info));
        status = vms_kif_getjpi_self(&info);
        CHECK(status == SS_NORMAL && strcmp(info.prcnam, LEN15_NAME) == 0,
              "our name survived both rejections intact");

        /* DISCRIMINATOR: an oversized LOOKUP key must be refused, not
         * clipped into a key that resolves the process holding its
         * first 15 characters -- which is us. */
        memset(&info, 0, sizeof(info));
        status = vms_kif_getjpi_prcnam(LEN16_NAME, &info);
        CHECK(status == SS_IVLOGNAM,
              "an oversized lookup key is rejected, not truncated into a match");
        CHECK(info.linux_pid == 0,
              "the rejected lookup returned no row");

        /* Control: the legal 15-character key does resolve, so the two
         * rejections above are the length rule and not a dead lookup. */
        memset(&info, 0, sizeof(info));
        status = vms_kif_getjpi_prcnam(LEN15_NAME, &info);
        CHECK(status == SS_NORMAL && info.linux_pid == (uint32_t)getpid(),
              "the legal 15-character key still resolves to us");
    }

done:
    vms_kif_close();

    printf("=== test_kmod_procnam: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
