/*
 * test_dlm_dir_h7.c - the DLM directory + consistent-mastering PROOF DRIVER
 *                     (rd vms-1bba, DLM harness rung H7 "DB"), one node.
 *
 * Runs on ONE OVMX node against a real /dev/vms. The executive was loaded with a
 * STATIC membership vector (insmod vms.ko vms_local_csid=<mine>
 * dlm_member_csids=1030,1031 -- see tests/qemu/init_dlm_h7.sh). For each of a
 * FIXED set of resource names it asks the executive, purely as a function of the
 * name and the shared membership vector, which node is the DIRECTORY and which is
 * the MASTER (vms_kif_get_resmaster), then issues a LOCAL $ENQ for the same name
 * (vms_kif_enq) and reports the status the executive returned.
 *
 * WHAT THIS DRIVER DOES NOT DO (deliberately): it does NOT assert cross-node
 * agreement. It runs on a SINGLE node and cannot see the other node's output;
 * agreement between the two nodes' independently-resolved directory/master is the
 * RUNNER's verdict (tests/qemu/run_dlm_harness_h7.sh), which reads both nodes'
 * machine-greppable H7DIR/H7ENQ lines. This driver only emits honest, verbatim
 * executive state so the runner has something real to compare.
 *
 * The line contract the runner parses (exactly):
 *   H7DIR name=<NAME> local=<local_csid> dir=<dir_csid> master=<master_csid> is_local=<is_local_master>
 *   H7ENQ name=<NAME> local=<local_csid> dir=<dir_csid> enq_status=<decimal status>
 *   H7-DRIVER-DONE
 *
 * The name set is chosen so the jhash-based directory (exec_jhash(name) %
 * dlm_member_count, src/kernel-core/vms_lock.c dlm_directory_csid) distributes
 * across BOTH members 1030 and 1031 -- so every node has at least one
 * locally-mastered name AND at least one remote-mastered name, which is what
 * makes the runner's split (b) and no-regression (c) assertions exercisable.
 *
 * INV-6 / honest SKIP (77) when /dev/vms is absent: the directory + mastering
 * state is executive-resident, so with no /dev/vms there is nothing to resolve
 * and nothing this driver can fabricate. init_dlm_h7.sh insmods vms.ko before
 * running this, so in the harness /dev/vms is always present; the 77 path exists
 * only so the driver never fakes a pass if it is ever run without the executive.
 *
 * A LOCAL $ENQ (vms_kif_enq) for a name whose directory hashes to THIS node
 * succeeds (SS$_NORMAL, odd) -- self is directory and master, served by the
 * single-node lock manager. A LOCAL $ENQ for a name whose directory is the OTHER
 * node fails honestly with SS$_UNSUPPORTED (2296): dlm_resolve_master() declines
 * a remote directory/master rather than forwarding (DC, 0.4) or fabricating a
 * remote grant. That honest no-regression is a REQUIRED part of the proof, so the
 * driver reports the raw status the executive returned, never a massaged one.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include "vms_ioctl.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

/*
 * FIXED name set. Deterministic given the jhash directory function and the
 * ordered vector 1030,1031: with this set the directory distributes across both
 * members (verified against the public Linux jhash the executive uses), so each
 * node sees both a local-mastered and a remote-mastered name. Twelve names give
 * a robust margin; the runner reads the REAL split from the executive output and
 * fails loudly if it is ever all-local (never a vacuous pass).
 */
static const char *const NAMES[] = {
    "DIRRESA", "DIRRESB", "DIRRESC", "DIRRESD", "DIRRESE", "DIRRESF",
    "DIRRESG", "DIRRESH", "DIRRESI", "DIRRESJ", "DIRRESK", "DIRRESL",
};
#define NNAMES ((int)(sizeof(NAMES) / sizeof(NAMES[0])))

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_dlm_dir_h7 (DLM directory + consistent mastering, one node) ===\n");

    /* Presence probe: honest SKIP (77) if the executive is absent. In the H7
     * harness init_dlm_h7.sh has already insmod'd vms.ko, so this never fires
     * there; it only guarantees the driver cannot fake a pass without /dev/vms. */
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        printf("H7-DRIVER-SKIP: no /dev/vms -- the DLM directory + mastering "
               "state is executive-resident, nothing to resolve or fabricate\n");
        printf("=== test_dlm_dir_h7: SKIPPED (no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    close(fd);

    /* Register this task with the executive. vms_kif_get_resmaster / vms_kif_enq
     * self-bind, but registering explicitly surfaces a clear status and matches
     * the other syssvc drivers. A failure here is reported, not hidden. */
    uint32_t rst = vms_kif_register(NULL);
    printf("register status=%u\n", rst);

    for (int i = 0; i < NNAMES; i++) {
        const char *name = NAMES[i];

        uint32_t found = 0, local = 0, dir = 0, master = 0,
                 is_local = 0, n_granted = 0, remote_holder = 0;
        uint32_t st = vms_kif_get_resmaster(name, &found, &local, &dir,
                                            &master, &is_local, &n_granted,
                                            &remote_holder);
        (void)st; (void)found; (void)n_granted; (void)remote_holder;

        /* Machine-greppable directory/master line (verbatim executive state). */
        printf("H7DIR name=%s local=%u dir=%u master=%u is_local=%u\n",
               name, local, dir, master, is_local);

        /* A LOCAL $ENQ for the same name; report the raw returned status. EX
         * mode, no flags, no ASTs. For a name mastered here this grants
         * (SS$_NORMAL); for a name whose directory is the other node it declines
         * SS$_UNSUPPORTED (2296) -- the honest no-regression the runner checks. */
        uint32_t lkid = 0;
        uint32_t est = vms_kif_enq(0 /*efn*/, LCK_K_EXMODE, 0 /*flags*/,
                                   name, 0 /*parid*/,
                                   0 /*astadr*/, 0 /*astprm*/, 0 /*blkastadr*/,
                                   &lkid, NULL /*valblk*/);
        printf("H7ENQ name=%s local=%u dir=%u enq_status=%u\n",
               name, local, dir, est);
    }

    printf("H7-DRIVER-DONE\n");
    printf("=== test_dlm_dir_h7: emitted %d names ===\n", NNAMES);
    return 0;
}
