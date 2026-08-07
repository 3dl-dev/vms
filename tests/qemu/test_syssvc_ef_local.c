/*
 * test_syssvc_ef_local.c - LOCAL event flag semantics through the PUBLIC
 * sys$ API, against a real executive (vms-2a8).
 *
 * ============================================================
 * MOVED, NOT DELETED. This suite IS
 * tests/conformance/vms_programs/test_event_flags.c, relocated to the one
 * harness where the services it calls can actually run.
 *
 * That program asserted the documented WASSET/WASCLR semantics of
 * $SETEF/$CLREF/$READEF/$WAITFR on a local flag, and it passed on a plain
 * Linux host for eight rounds -- because src/libvms/syssvc/sys_event.c kept
 * all 128 flags in per-process memory and never called the executive. When
 * vms-2a8 made sys_event.c a reader of the executive (CLAUDE.md Rule 11),
 * every one of its eight checks went red with status 0x2a4 (SS$_BUGCHECK,
 * what vms_kif returns when /dev/vms is absent).
 *
 * NOTHING IT ASSERTED WAS BROKEN BY THAT CHANGE. What broke was the venue:
 * the conformance job runs in a plain ubuntu tooling container, which has
 * no /dev/vms and never will -- the only OVMX runtime is the kernel/QEMU
 * path (Rule 9). So after the conversion, what that program actually
 * asserted was THAT A VMS SYSTEM SERVICE SUCCEEDS WITH NO EXECUTIVE
 * PRESENT: a state OpenVMS is never in and OVMX refuses to boot into
 * (vms-0ff). The only ways to keep it green where it stood were to give
 * $SETEF a per-process fallback -- the facade this item exists to delete,
 * reintroduced to satisfy a test -- or to skip it, and under Rule 10 a
 * permanently skipped test is a failing test.
 *
 * This is the same move, for the same reason, that vms-8019 made with the
 * host lib$getjpi block in tests/libvms/test_lib_rtl.c. See that file's
 * block comment; the record left behind at the old site is in
 * tests/conformance/run_conformance.sh.
 *
 * NOT ONE ASSERTION WAS WEAKENED IN THE MOVE. All thirteen of the old
 * program's checks arrive verbatim in meaning and in order (blocks C1..C8
 * below; the run that failed printed 8 FAIL lines only because its checks
 * were nested, so an outer failure skipped the inner one). THREE ARE ADDED,
 * all of them things the old program computed and then never checked: it
 * passed &ef_state to sys$readef on every call and printed the cluster word
 * without ever asserting on it, so that word could have been garbage for
 * the program's whole life. Net assertions: 13 -> 16.
 * ============================================================
 *
 * WHY A LOCAL FLAG IS STILL AN EXECUTIVE QUESTION, which is the thing that
 * makes this suite worth having next to test_syssvc_ef_mproc.c: local
 * clusters are per-process on VMS too, but that is the EXECUTIVE's
 * per-process state (proc->ef.local[] in src/kernel/vms_eflag.c, keyed by
 * thread-group id), not the image's. The sharing property -- common
 * clusters are shared by name, local ones are not -- is measured
 * A-writes/B-reads in test_syssvc_ef_mproc.c. What is measured HERE is the
 * status DISCRIMINATION the VMS documentation specifies for a single
 * caller: whether the service reports what the flag was before the call.
 *
 * THE STATUS VALUES ARE ASSERTED BY VALUE HERE, deliberately, and that is
 * the one respect in which this suite differs from test_syssvc_ef_mproc.c
 * (which asserts the odd/even convention only, so that it cannot go red for
 * a numbering reason belonging to another item). The old conformance
 * program compared against SS$_WASCLR and SS$_WASSET by value, and those
 * values are now oracle-pinned -- $EQU SS$_WASCLR 1, SS$_WASSET 9 on VAX1
 * OpenVMS VAX V7.3, docs/oracle/vax73-event-flags.md -- so keeping the
 * comparison keeps real coverage of the numbering rather than dropping it.
 *
 * NOTE ON SS$_WASCLR == SS$_NORMAL: both are 1 in $SSDEF (oracle, same
 * transcript). That is VMS, not an OVMX collision, so "status ==
 * SS$_WASCLR" cannot distinguish "was clear" from a generic success --
 * callers discriminate by testing for SS$_WASSET. The C3 block below is
 * written the way the original was (accept either, name which) precisely
 * because of that, and C4/C5/C7 carry the discriminating assertions.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

/* Event flag 1 -- local cluster 0 -- exactly as the conformance program used. */
#define TEST_EFN 1

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/*
 * Probe only -- vms_kif_open()/close() decide skip-vs-run and nothing else.
 * Every assertion below goes through the public sys$ API.
 */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: keep stdout line-buffered even when init.sh redirects it to a file, so an unflushed fork() cannot splice output */
    uint32_t status;
    uint32_t ef_state;

    printf("=== test_syssvc_ef_local (local event flag semantics via the PUBLIC sys$ API) ===\n");

    if (!executive_present()) {
        /*
         * Reached ONLY in the CI negative control (a rig deliberately booted
         * without insmod'ing vms.ko); vms-0ff removed OVMX's
         * executive-absent state, so the product cannot get here.
         *
         * The property that still holds and is still asserted: a public sys$
         * entry point must never report success for an executive operation
         * it could not have performed. This is what the old conformance
         * program could not assert, because on its host the answer was
         * always a fabricated success.
         */
        printf("  FAIL: cannot open /dev/vms\n");
        status = sys$setef(TEST_EFN);
        printf("  INFO: sys$setef with no executive returned status 0x%x\n", status);
        CHECK(!(status & 1),
              "sys$setef does NOT report success when the executive was never reached");
        status = sys$readef(TEST_EFN, &ef_state);
        printf("  INFO: sys$readef with no executive returned status 0x%x\n", status);
        CHECK(!(status & 1),
              "sys$readef does NOT report success when the executive was never reached");
        printf("=== test_syssvc_ef_local: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* C1: sys$clref - clear the flag. */
    status = sys$clref(TEST_EFN);
    /* negctl-knockon: bind-client-no-register */
    CHECK($VMS_STATUS_SUCCESS(status), "sys$clref(1) cleared the flag");

    /* C2: sys$readef on the cleared flag reports WASCLR. */
    status = sys$readef(TEST_EFN, &ef_state);
    /* negctl-knockon: bind-client-no-register */
    CHECK($VMS_STATUS_SUCCESS(status), "sys$readef(1) succeeded on the cleared flag");
    /* negctl-knockon: bind-client-no-register */
    /* negctl: eflag-readef-status-inverted */
    CHECK(status == SS$_WASCLR, "sys$readef(1) reported WASCLR for the cleared flag");
    /* ADDED (vms-2a8): the old program read ef_state and never checked it. */
    /* negctl-knockon: bind-client-no-register */
    /* negctl-knockon: eflag-readef-status-inverted */
    CHECK((status == SS$_WASCLR) && !(ef_state & (1u << TEST_EFN)),
          "the cluster state word agrees with the status: flag 1's bit is CLEAR");

    /* C3: sys$setef - set the flag. */
    status = sys$setef(TEST_EFN);
    /* negctl: bind-client-no-register */
    CHECK($VMS_STATUS_SUCCESS(status), "sys$setef(1) set the flag");
    /*
     * Written exactly as the conformance program wrote it: SS$_WASCLR is 1
     * and so is SS$_NORMAL, so the only honest single-call check is that the
     * service reported one of the two documented previous states. C4/C5
     * below are what actually discriminate.
     */
    /* negctl-knockon: bind-client-no-register */
    CHECK(status == SS$_WASCLR || status == SS$_WASSET,
          "sys$setef(1) reported a documented previous state (WASCLR or WASSET)");

    /* C4: sys$readef on the set flag reports WASSET -- the discriminator. */
    status = sys$readef(TEST_EFN, &ef_state);
    /* negctl-knockon: bind-client-no-register */
    CHECK($VMS_STATUS_SUCCESS(status), "sys$readef(1) succeeded after the flag was set");
    /* negctl-knockon: bind-client-no-register */
    /* negctl-knockon: eflag-readef-status-inverted */
    CHECK(status == SS$_WASSET, "sys$readef(1) reported WASSET after the flag was set");
    /* ADDED (vms-2a8): the state word must move with the flag. */
    /* negctl-knockon: bind-client-no-register */
    CHECK(ef_state & (1u << TEST_EFN),
          "the cluster state word agrees with the status: flag 1's bit is SET");

    /* C5: sys$setef on an already-set flag reports WASSET. */
    status = sys$setef(TEST_EFN);
    /* negctl-knockon: bind-client-no-register */
    CHECK($VMS_STATUS_SUCCESS(status), "sys$setef(1) succeeded on the already-set flag");
    /* negctl-knockon: bind-client-no-register */
    /* negctl: eflag-setef-status-inverted */
    CHECK(status == SS$_WASSET, "sys$setef(1) on an already-set flag reported WASSET");

    /* C6: sys$waitfr on a set flag returns immediately. */
    status = sys$waitfr(TEST_EFN);
    /* negctl-knockon: bind-client-no-register */
    CHECK($VMS_STATUS_SUCCESS(status), "sys$waitfr(1) returned for the already-set flag");
    /*
     * ADDED (vms-2a8): $WAITFR must not consume the flag. VMS event flags are
     * not counting semaphores -- $WAITFR waits until the flag is set and
     * leaves it set; only $CLREF clears one. If it returned because the flag
     * was set, the flag is still set.
     */
    status = sys$readef(TEST_EFN, &ef_state);
    /* negctl-knockon: bind-client-no-register */
    /* negctl-knockon: eflag-readef-status-inverted */
    CHECK(status == SS$_WASSET, "sys$waitfr(1) left the flag SET -- it is not a counting semaphore");

    /* C7: sys$clref on the set flag reports WASSET. */
    status = sys$clref(TEST_EFN);
    /* negctl-knockon: bind-client-no-register */
    CHECK($VMS_STATUS_SUCCESS(status), "sys$clref(1) succeeded on the set flag");
    /* negctl-knockon: bind-client-no-register */
    CHECK(status == SS$_WASSET, "sys$clref(1) reported WASSET for the previously-set flag");

    /* C8: and the flag really is clear afterwards. */
    status = sys$readef(TEST_EFN, &ef_state);
    /* negctl: eflag-clref-noop */
    /* negctl-knockon: bind-client-no-register */
    /* negctl-knockon: eflag-readef-status-inverted */
    CHECK(status == SS$_WASCLR, "sys$readef(1) reported WASCLR after the clear");

    printf("=== test_syssvc_ef_local: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
