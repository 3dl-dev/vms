/*
 * test_kmod_mbx.c - Executive mailboxes: A-writes / B-reads (vms-d44)
 *
 * A mailbox a process keeps in its own address space (the AF_UNIX
 * socketpair implementation this item replaces -- see
 * src/kernel/vms_mbx.h) passes every single-process test perfectly and is
 * still a facade: it reports success while sharing nothing with any other
 * process (CLAUDE.md rule 11). So this suite is built around the same
 * decisive check test_kmod_devtab.c uses for the device table:
 *
 *   PROCESS A CREATES A MAILBOX AND WRITES A MESSAGE INTO IT.
 *   PROCESS B, WHICH DID NOT CREATE IT, OPENS THE SAME MAILBOX BY NAME
 *   AND READS THE EXACT MESSAGE.
 *
 * plus the two lifecycle rules OpenVMS documents for $CREMBX/$DELMBX:
 *
 *   A TEMPORARY MAILBOX IS FREED THE INSTANT ITS LAST CHANNEL (ANY
 *   PROCESS'S) IS DEASSIGNED.
 *   $DELMBX MARKS A MAILBOX FOR DELETION BUT DOES NOT ITSELF DEASSIGN
 *   ANYTHING.
 *
 * The test drives the real userspace client (src/libvmssys/vms_kif.c)
 * against a real /dev/vms, not a hand-rolled ioctl copy -- the client
 * src/libvms/syssvc/sys_mailbox.c actually calls.
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is
 * loaded -- see test_syssvc_lnm_system.c for the same pattern): every
 * vms_kif_mbx_* entry point must fail SS$_NOSUCHDEV, never a private
 * per-process substitute (CLAUDE.md Rule 9 / INV-6).
 *
 * Modes:
 *   (no args)           process B (parent)
 *   --writer <wfd> <gfd> process A
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
#define SS_EXQUOTA      28
#define SS_NOPRIV       36
#define SS_IVCHAN       602
#define SS_IVDEVNAM     608
#define SS_NOSUCHDEV    2680

#define EXIT_SKIP 77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char MSG1[] = "hello from process A, byte-exact or bust";

/* What process A reports back over the pipe. */
struct mbx_report {
    uint32_t create_status;
    uint32_t perm_status;      /* creating a PERMANENT mailbox: expect SS$_NOPRIV */
    uint32_t write_status;
    uint32_t unit;
    char     devnam[32];
    uint32_t dassgn_status;    /* filled after the "go" signal, second write */
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

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

/*
 * process_a - creates a TEMPORARY mailbox, writes one message into it,
 * reports its device name to B, then waits for B's "go" before giving its
 * own channel back (so B can prove the mailbox survives A's exit as long
 * as B still holds a channel to it).
 */
static int process_a(int wfd, int gfd)
{
    struct mbx_report rep;
    uint32_t exec_chan = 0, perm_chan = 0;
    char go;

    memset(&rep, 0, sizeof(rep));

    if (vms_kif_open() < 0 || vms_kif_register(NULL) != SS_NORMAL) {
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }

    rep.create_status = vms_kif_mbx_create(0 /* temporary */, 0, 0,
                                            &exec_chan, &rep.unit,
                                            rep.devnam, sizeof(rep.devnam));

    /* Creating a PERMANENT mailbox needs PRMMBX, which this test process
     * (default privileges only -- see vms_module.c's VMS_DEFAULT_PRIVS)
     * does not hold. No side effect: on refusal the executive creates
     * nothing (vms_mbx.c checks privilege before allocating anything). */
    rep.perm_status = vms_kif_mbx_create(1 /* permanent */, 0, 0,
                                          &perm_chan, NULL, NULL, 0);

    if (rep.create_status == SS_NORMAL)
        rep.write_status = vms_kif_mbx_write(exec_chan, MSG1, sizeof(MSG1));

    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        return 1;

    /* Wait for B to say it has its own channel(s) to the mailbox before
     * this one goes away. */
    if (read(gfd, &go, 1) != 1)
        return 1;

    rep.dassgn_status = vms_kif_dassgn((uint16_t)exec_chan);
    if (write(wfd, &rep.dassgn_status, sizeof(rep.dassgn_status)) !=
        (ssize_t)sizeof(rep.dassgn_status))
        return 1;

    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc >= 3 && strcmp(argv[1], "--writer") == 0)
        return process_a(atoi(argv[2]), atoi(argv[3]));

    printf("=== test_kmod_mbx: executive mailboxes (vms-d44) ===\n");

    if (!executive_present()) {
        /*
         * NO EXECUTIVE: every mailbox entry point must fail honestly
         * (CLAUDE.md Rule 9 / INV-6), never fabricate a private mailbox.
         * Run on the host, before vms.ko is loaded.
         */
        uint32_t exec_chan = 0, unit = 0, actlen = 0;
        char devnam[32], buf[16];
        uint32_t st;

        printf("  (no /dev/vms -- running executive-absent assertions)\n");

        st = vms_kif_mbx_create(0, 0, 0, &exec_chan, &unit, devnam, sizeof(devnam));
        CHECK(st == SS_NOSUCHDEV,
              "no executive: $CREMBX fails SS$_NOSUCHDEV, never a local fallback");

        st = vms_kif_mbx_assign("MBA1:", &exec_chan);
        CHECK(st == SS_NOSUCHDEV,
              "no executive: $ASSIGN to MBA1: fails SS$_NOSUCHDEV, never a local fallback");

        st = vms_kif_mbx_write(1, "x", 1);
        CHECK(st == SS_NOSUCHDEV,
              "no executive: mailbox write fails SS$_NOSUCHDEV, never a local fallback");

        st = vms_kif_mbx_read(1, buf, sizeof(buf), &actlen);
        CHECK(st == SS_NOSUCHDEV,
              "no executive: mailbox read fails SS$_NOSUCHDEV, never a local fallback");

        st = vms_kif_mbx_delmbx(1);
        CHECK(st == SS_NOSUCHDEV,
              "no executive: $DELMBX fails SS$_NOSUCHDEV, never a local fallback");

        printf("=== test_kmod_mbx: %d passed, %d failed (SKIPPED: no /dev/vms -- "
               "executive-present scenarios not exercised) ===\n", pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    if (open_and_register() < 0) {
        printf("=== test_kmod_mbx: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* --------------------------------------------------------------
     * A-writes / B-reads: fork process A, which creates a TEMPORARY
     * mailbox and writes MSG1 into it before this process (B) has done
     * anything at all.
     * -------------------------------------------------------------- */
    int pipefd[2], gofd[2];
    if (pipe(pipefd) < 0 || pipe(gofd) < 0) {
        printf("  FAIL: pipe()\n");
        return 1;
    }

    pid_t child = fork();
    if (child < 0) {
        printf("  FAIL: fork()\n");
        return 1;
    }
    if (child == 0) {
        close(pipefd[0]);
        close(gofd[1]);
        char wfd_arg[16], gfd_arg[16];
        snprintf(wfd_arg, sizeof(wfd_arg), "%d", pipefd[1]);
        snprintf(gfd_arg, sizeof(gfd_arg), "%d", gofd[0]);
        execl(argv[0], argv[0], "--writer", wfd_arg, gfd_arg, (char *)NULL);
        _exit(1);
    }
    close(pipefd[1]);
    close(gofd[0]);

    struct mbx_report rep;
    memset(&rep, 0, sizeof(rep));
    if (read(pipefd[0], &rep, sizeof(rep)) != (ssize_t)sizeof(rep)) {
        printf("  FAIL: could not read A's report\n");
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return 1;
    }

    CHECK(rep.create_status == SS_NORMAL, "A: $CREMBX (temporary) succeeds");
    CHECK(rep.perm_status == SS_NOPRIV,
          "oracle: $CREMBX (permanent) without PRMMBX privilege fails SS$_NOPRIV "
          "(System Services Reference, $CREMBX)");
    CHECK(rep.write_status == SS_NORMAL, "A: mailbox write succeeds");
    CHECK(rep.devnam[0] == 'M' && rep.devnam[1] == 'B' && rep.devnam[2] == 'A',
          "A's mailbox has a device name of the form MBAn:");

    /* --------------------------------------------------------------
     * B reaches A's mailbox by DEVICE NAME ALONE -- it never called
     * $CREMBX and holds no relationship to A but the name itself. This
     * is the assertion the mbx-not-shared negative control (tests/qemu/
     * facility_defects.sh) exists to protect: a mutation that gates the
     * $ASSIGN lookup on the creator's identity would fail this exact
     * check while every single-process assertion above stayed green.
     * -------------------------------------------------------------- */
    uint32_t chan_b1 = 0, chan_b2 = 0, chan_b3 = 0, status;

    status = vms_kif_mbx_assign(rep.devnam, &chan_b1);
    /* negctl: mbx-not-shared */
    CHECK(status == SS_NORMAL,
          "B: $ASSIGN to A's mailbox by device name succeeds (cross-process rendezvous)");

    char buf[64];
    uint32_t actlen = 0;
    memset(buf, 0, sizeof(buf));
    status = vms_kif_mbx_read(chan_b1, buf, sizeof(buf), &actlen);
    /* negctl-knockon: mbx-not-shared */
    CHECK(status == SS_NORMAL, "B: mailbox read succeeds on A's mailbox");
    /* negctl-knockon: mbx-not-shared */
    CHECK(actlen == sizeof(MSG1) && memcmp(buf, MSG1, sizeof(MSG1)) == 0,
          "B reads the EXACT message A wrote (byte-exact, right length)");

    /* A second channel from B, taken before A gives its own back. */
    status = vms_kif_mbx_assign(rep.devnam, &chan_b2);
    /* negctl-knockon: mbx-not-shared */
    CHECK(status == SS_NORMAL, "B: a second $ASSIGN to the same mailbox succeeds");

    /* --------------------------------------------------------------
     * Tell A to deassign. The mailbox must survive -- B still holds two
     * channels to it.
     * -------------------------------------------------------------- */
    if (write(gofd[1], "x", 1) != 1) {
        printf("  FAIL: could not signal A\n");
    }
    uint32_t a_dassgn_status = 0;
    if (read(pipefd[0], &a_dassgn_status, sizeof(a_dassgn_status)) !=
        (ssize_t)sizeof(a_dassgn_status)) {
        printf("  FAIL: could not read A's $DASSGN status\n");
    }
    CHECK(a_dassgn_status == SS_NORMAL, "A: $DASSGN of its own channel succeeds");
    waitpid(child, NULL, 0);

    status = vms_kif_mbx_assign(rep.devnam, &chan_b3);
    /* negctl-knockon: mbx-not-shared */
    CHECK(status == SS_NORMAL,
          "temporary mailbox SURVIVES its creator's $DASSGN while another "
          "process (B) still holds a channel to it");

    /* $DELMBX: marks for deletion, does NOT deassign anything. The
     * mailbox must still be reachable right after this call. */
    status = vms_kif_mbx_delmbx(chan_b1);
    /* negctl-knockon: mbx-not-shared */
    CHECK(status == SS_NORMAL, "B: $DELMBX succeeds");

    uint32_t probe_chan = 0;
    status = vms_kif_mbx_assign(rep.devnam, &probe_chan);
    /* negctl-knockon: mbx-not-shared */
    CHECK(status == SS_NORMAL,
          "oracle: $DELMBX alone does not deassign or delete -- the mailbox "
          "is still assignable immediately afterward");
    (void)vms_kif_dassgn((uint16_t)probe_chan);

    /* --------------------------------------------------------------
     * Give back every channel B holds. The LAST one must free the
     * mailbox: it is temporary (and now also $DELMBX-marked), so once
     * refcnt reaches zero the executive deletes it.
     * -------------------------------------------------------------- */
    /* negctl-knockon: mbx-not-shared */
    CHECK(vms_kif_dassgn((uint16_t)chan_b1) == SS_NORMAL, "B: $DASSGN channel 1");
    /* negctl-knockon: mbx-not-shared */
    CHECK(vms_kif_dassgn((uint16_t)chan_b2) == SS_NORMAL, "B: $DASSGN channel 2");
    /* negctl-knockon: mbx-not-shared */
    CHECK(vms_kif_dassgn((uint16_t)chan_b3) == SS_NORMAL, "B: $DASSGN channel 3 (the last one)");

    uint32_t final_chan = 0;
    status = vms_kif_mbx_assign(rep.devnam, &final_chan);
    CHECK(status == SS_NOSUCHDEV,
          "the mailbox is GONE after its last channel was deassigned "
          "(temporary-mailbox deletion on last-channel-deassign)");

    /* --------------------------------------------------------------
     * Negative controls that were never exercised above: a malformed
     * device name, and $DASSGN of a channel that is not ours.
     * -------------------------------------------------------------- */
    uint32_t bogus = 0;
    status = vms_kif_mbx_assign("not a device", &bogus);
    CHECK(status == SS_IVDEVNAM, "malformed mailbox device name reports SS$_IVDEVNAM");
    status = vms_kif_mbx_assign("MBA999999:", &bogus);
    CHECK(status == SS_NOSUCHDEV, "assigning a never-created mailbox unit reports SS$_NOSUCHDEV");
    status = vms_kif_dassgn(60000);
    CHECK(status == SS_IVCHAN, "$DASSGN of a channel we never held reports SS$_IVCHAN");

    printf("=== test_kmod_mbx: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
