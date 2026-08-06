/*
 * test_kmod_setterm.c - the job-to-terminal binding is executive-resident
 * (vms-d0b).
 *
 * WHAT THIS PROVES, AND WHY IT IS NOT test_kmod_devtab.c AGAIN.
 * tests/qemu/test_kmod_devtab.c proves the DEVICE table: OPA0: exists for
 * everyone, its ownership and characteristics are shared, one process
 * changes them and another sees the change. What it says nothing about is
 * WHICH TERMINAL A GIVEN JOB IS ON -- and that is the question SHOW TERMINAL
 * asks. Until this item OVMX answered it from a VMS_TERMINAL environment
 * variable, i.e. a process telling itself what it was on, with no other
 * process able to see or contradict the answer. vms-fb9 deleted that; this
 * puts the binding in the executive instead.
 *
 * THE DECISIVE TEST IS A-WRITES / B-READS (CLAUDE.md rule 11). A binding
 * kept in the process's own memory passes every single-process check
 * perfectly, so a single-process check proves nothing. Here process A
 * assigns the console and records it as its terminal; process B -- which
 * assigned nothing, recorded nothing, and knows A only by the VMS process
 * id A sent it down a pipe -- reads the binding back out of the executive.
 *
 * BOTH DIRECTIONS ARE ASSERTED, because a reader that always reported a
 * terminal would satisfy the positive half on its own:
 *   - before A binds, A's row carries NO terminal, read from B;
 *   - after A binds, A's row carries OPA0:, read from B;
 *   - B's OWN row still carries no terminal at that moment, so what B sees
 *     in A's row cannot be B's own state leaking through the copy-out.
 *
 * AND IT SURVIVES execve(), which is what the environment variable was
 * there to do: A binds, then execs a fresh image of this program, which
 * opens a brand-new /dev/vms channel and asks the executive for its own
 * terminal. Nothing is carried across the exec in userspace -- no
 * environment variable, no inherited descriptor, no file.
 *
 * NEGATIVE CONTROL RIG: under NEGATIVE_CONTROL=1 (tests/qemu/Dockerfile
 * boots without insmod'ing vms.ko) there is no /dev/vms to open and this
 * program fails at the first line of main() saying so. It has no
 * per-process fallback to fall back to, which is the point.
 *
 * Modes:
 *   (no args)                     process B / the parent
 *   --bound-child <pipe-wfd>      process A, before and after exec
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "vms_kif.h"

/*
 * Status values. SS$_NORMAL is 1 in $SSDEF on the reference lab
 * (OpenVMS VAX V7.3, VAX1). SS$_IVCHAN and SS$_IVDEVNAM are this tree's
 * src/libvms/include/ssdef.h values, which carry their own
 * operator-sign-off caveat there (vms-47f) -- this test inherits it
 * rather than re-deriving a second opinion, exactly as
 * src/kernel/vms_internal.h does.
 */
#define SS_NORMAL       1
#define SS_IVCHAN       602

/*
 * DISCLOSED, NOT HIDDEN, in the same terms vms_devtab_init() uses for
 * the shareable flag: VMS_IOCTL_SETTERM's OTHER refusal -- SS$_IVDEVNAM
 * for a channel to a device that is not a terminal -- HAS NO ASSERTION
 * HERE AND CANNOT HAVE ONE. OPA0: is the only device the executive
 * creates, and vms.ko has no operation that adds another, so there is
 * no non-terminal channel in existence to hand it. The check is written
 * (src/kernel/vms_devtab.c, vms_ioctl_setterm) because omitting it
 * would silently permit binding a disk as a terminal the day a disk
 * appears; the first non-terminal device added to the table owes this
 * suite that assertion. The identical gap exists one function up, on
 * IO$_SETMODE's class check, and for the same reason.
 */

#define CONSOLE_DEVNAM  "OPA0:"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What process A reports back to process B over a pipe. */
struct bound_report {
    uint32_t vms_pid;                   /* the id the executive gave A */
    uint32_t assign_status;
    uint32_t chan;
    uint32_t setterm_status;
    uint32_t phase;                     /* 0 = pre-exec, 1 = post-exec */
    char     terminal[VMS_DEVNAM_SIZE]; /* what A's own row says */
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

static void report(int wfd, const struct bound_report *rep)
{
    ssize_t n = write(wfd, rep, sizeof(*rep));
    (void)n;
}

/*
 * Process A. Runs twice in the same Linux process: once as the forked
 * child (phase 0), and once as the image execve() replaced it with
 * (phase 1). Phase 1 does not bind anything -- it only asks who it is.
 */
static void bound_child(int wfd, int gfd, const char *self, int phase)
{
    struct bound_report rep;
    struct vms_procinfo info;

    memset(&rep, 0, sizeof(rep));
    rep.phase = (uint32_t)phase;

    if (vms_kif_open() < 0 || vms_kif_register(&rep.vms_pid) != SS_NORMAL)
        _exit(3);

    if (phase == 0) {
        char go;

        /* Report the UNBOUND row first, so the parent can see the
         * before state through the executive rather than assume it. */
        memset(&info, 0, sizeof(info));
        if (vms_kif_getjpi_self(&info) == SS_NORMAL)
            memcpy(rep.terminal, info.terminal, VMS_DEVNAM_SIZE);
        report(wfd, &rep);

        /*
         * WAIT FOR THE PARENT TO HAVE LOOKED, and this is not
         * politeness -- it is the difference between an assertion and a
         * race. The parent's "A's row has no terminal yet" read has to
         * happen while that is still true, and without this gate A
         * bound the terminal first about half the time, failing an
         * assertion that was perfectly correct.
         */
        while (read(gfd, &go, 1) < 0)
            ;

        rep.assign_status = vms_kif_assign(CONSOLE_DEVNAM, &rep.chan);
        rep.setterm_status = vms_kif_setterm(rep.chan);

        memset(&info, 0, sizeof(info));
        if (vms_kif_getjpi_self(&info) == SS_NORMAL)
            memcpy(rep.terminal, info.terminal, VMS_DEVNAM_SIZE);
        report(wfd, &rep);

        /*
         * Hand the SAME Linux process to a fresh image. The channel and
         * the binding were made by an image that is about to stop
         * existing, over a descriptor that closes here.
         */
        vms_kif_close();
        {
            char wfd_str[16];
            snprintf(wfd_str, sizeof(wfd_str), "%d", wfd);
            execl(self, self, "--bound-child-execed", wfd_str, (char *)NULL);
        }
        _exit(4);   /* exec failed: the parent sees a missing phase-1 report */
    }

    /* phase 1: a brand new image on a brand new channel. */
    memset(&info, 0, sizeof(info));
    if (vms_kif_getjpi_self(&info) == SS_NORMAL)
        memcpy(rep.terminal, info.terminal, VMS_DEVNAM_SIZE);
    report(wfd, &rep);

    /*
     * Stay alive: the parent still has to read this process's row out
     * of the executive, and a row is only there while its process is.
     * The parent signals when it is done. (Sleeping on the pipe is not
     * available here -- wfd is the WRITE end, and it is the only
     * descriptor that survives the exec.)
     */
    for (;;)
        sleep(3600);
}

int main(int argc, char **argv)
{
    struct bound_report pre, post, execed;
    struct vms_procinfo info;
    uint32_t status, chan = 0;
    int pipefd[2], gatefd[2];
    pid_t child;
    ssize_t n;

    if (argc >= 3 && strcmp(argv[1], "--bound-child-execed") == 0) {
        bound_child(atoi(argv[2]), -1, argv[0], 1);
        return 0;
    }

    /*
     * A broken pipe must be a NAMED failure, not a dead test. If process A
     * exits early -- which it does the moment the executive refuses it
     * anything -- the parent's write to the gate pipe would otherwise kill
     * this program with SIGPIPE and the suite's verdict would be a signal
     * number, attributing nothing. Its sibling test_syssvc_showterm was
     * MEASURED doing exactly that under the bind-client-no-register control
     * (rc=141) before this line was added to both.
     */
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_kmod_setterm: job-to-terminal binding (vms-d0b) ===\n");

    if (open_and_register() < 0) {
        printf("=== test_kmod_setterm: %d passed, %d failed ===\n",
               pass, fail + 1);
        return 1;
    }

    /*
     * B's own starting state, read from the executive rather than
     * assumed: B has bound no terminal, so its row must name none. This
     * is also what makes the cross-process read below meaningful -- if
     * B's row already said OPA0:, seeing OPA0: in A's row would prove
     * nothing about A.
     */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    CHECK(status == SS_NORMAL && info.terminal[0] == '\0',
          "a process that has bound no terminal has none in its row");

    /* ---- The two refusals, before anything is bound ---------------- */

    status = vms_kif_setterm(0);
    CHECK(status == SS_IVCHAN,
          "binding a channel this process does not hold is refused SS$_IVCHAN");

    memset(&info, 0, sizeof(info));
    CHECK(vms_kif_getjpi_self(&info) == SS_NORMAL && info.terminal[0] == '\0',
          "the refused bind left no terminal behind");

    /* ---- A-writes / B-reads ---------------------------------------- */

    /*
     * Two pipes. `pipefd` carries A's reports up; `gatefd` is how the
     * parent tells A to go ahead, so every assertion about A's state is
     * made while that state still holds. Synchronisation is on observed
     * events, never on sleeps.
     */
    if (pipe(pipefd) != 0 || pipe(gatefd) != 0) {
        printf("  FAIL: pipe()\n");
        fail++;
        goto done;
    }

    child = fork();
    if (child < 0) {
        printf("  FAIL: fork()\n");
        fail++;
        goto done;
    }
    if (child == 0) {
        close(pipefd[0]);
        close(gatefd[1]);
        bound_child(pipefd[1], gatefd[0], argv[0], 0);
        _exit(0);
    }
    close(pipefd[1]);
    close(gatefd[0]);

    memset(&pre, 0, sizeof(pre));
    memset(&post, 0, sizeof(post));
    memset(&execed, 0, sizeof(execed));

    n = read(pipefd[0], &pre, sizeof(pre));
    CHECK(n == (ssize_t)sizeof(pre) && pre.vms_pid != 0,
          "process A registered with the executive");
    CHECK(pre.terminal[0] == '\0',
          "A's row carries no terminal before A binds one");

    /*
     * B READS A'S ROW OUT OF THE EXECUTIVE, BEFORE A BINDS. B knows A
     * only by the VMS process id A sent it; it has no access to A's
     * memory and A has told it nothing about terminals.
     */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_pid(pre.vms_pid, &info);
    CHECK(status == SS_NORMAL && info.terminal[0] == '\0',
          "B reads A's row: no terminal, before A binds one");

    /* That read is done: A may bind now. */
    if (write(gatefd[1], "g", 1) != 1) {
        CHECK(0, "could not release A to bind its terminal");
        goto done;
    }

    n = read(pipefd[0], &post, sizeof(post));
    CHECK(n == (ssize_t)sizeof(post) && post.assign_status == SS_NORMAL,
          "A took a channel to the console");
    CHECK(post.setterm_status == SS_NORMAL,
          "A recorded that channel's device as its terminal");
    /* negctl-knockon: setterm-binding-not-recorded */
    CHECK(strcmp(post.terminal, CONSOLE_DEVNAM) == 0,
          "A's own row now names the console");

    /* THE ASSERTION THE WHOLE FILE EXISTS FOR. */
    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_pid(pre.vms_pid, &info);
    /* negctl: setterm-binding-not-recorded */
    CHECK(status == SS_NORMAL && strcmp(info.terminal, CONSOLE_DEVNAM) == 0,
          "A-WRITES/B-READS: B reads OPA0: out of A's row -- a binding a "
          "different process made, which a per-process binding could not show");

    /*
     * ... and B's own row is STILL empty at the same moment. Without
     * this, the assertion above would also be satisfied by a copy-out
     * that returned the CALLER's terminal for every row.
     */
    memset(&info, 0, sizeof(info));
    CHECK(vms_kif_getjpi_self(&info) == SS_NORMAL && info.terminal[0] == '\0',
          "B's own row is still empty, so A's row is not B's state echoed back");

    /* ---- survives image activation --------------------------------- */

    n = read(pipefd[0], &execed, sizeof(execed));
    CHECK(n == (ssize_t)sizeof(execed) && execed.phase == 1,
          "the re-execed image reported in");
    /* negctl-knockon: setterm-binding-not-recorded */
    CHECK(strcmp(execed.terminal, CONSOLE_DEVNAM) == 0,
          "the freshly activated image still finds its terminal -- nothing "
          "was carried across execve() in userspace");
    /* negctl: register-adopt-pid-not-reported */
    CHECK(execed.vms_pid == pre.vms_pid,
          "and it is the same VMS process, so the binding was not re-made");

    /* ---- the binding dies with the job ----------------------------- */

    close(pipefd[0]);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_pid(pre.vms_pid, &info);
    CHECK(status != SS_NORMAL,
          "A's row is gone once A exits, and its binding with it");

    /* ---- this process can bind too, so the refusals above were about
     *      the channel and not about a dead facility ------------------ */

    status = vms_kif_assign(CONSOLE_DEVNAM, &chan);
    CHECK(status == SS_NORMAL && chan != 0,
          "B can take its own channel to the console");
    CHECK(vms_kif_setterm(chan) == SS_NORMAL,
          "B can bind it");
    memset(&info, 0, sizeof(info));
    /* negctl-knockon: setterm-binding-not-recorded */
    CHECK(vms_kif_getjpi_self(&info) == SS_NORMAL &&
          strcmp(info.terminal, CONSOLE_DEVNAM) == 0,
          "and B's row now names the console");

    /*
     * The name in the row came from the DEVICE TABLE, not from the
     * caller: nothing in this program ever handed the executive the
     * string "OPA0:" for the binding -- vms_kif_setterm takes a channel
     * number and nothing else. The row's name matches what $GETDVI
     * reports for that same channel's device.
     */
    {
        struct vms_devinfo dinfo;
        memset(&dinfo, 0, sizeof(dinfo));
        status = vms_kif_getdvi_chan(chan, &dinfo);
        /* negctl-knockon: setterm-binding-not-recorded */
        CHECK(status == SS_NORMAL && strcmp(dinfo.devnam, info.terminal) == 0,
              "the bound name is the device table's name for that channel");
    }

    /* A channel handed back is no longer bindable. */
    CHECK(vms_kif_dassgn(chan) == SS_NORMAL, "B returns its channel");
    CHECK(vms_kif_setterm(chan) == SS_IVCHAN,
          "the returned channel can no longer be bound");

done:
    vms_kif_close();
    printf("=== test_kmod_setterm: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
