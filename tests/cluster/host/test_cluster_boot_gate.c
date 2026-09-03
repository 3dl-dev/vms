/*
 * test_cluster_boot_gate.c - R1 host proof of the FC-P0.11 VAXCLUSTER
 * gating decision (docs/plan-faithful-cluster-executive.md FC-P0.11 done-
 * condition: "VAXCLUSTER=0 => no PEA0:, no HELLO").
 *
 * Exercises the EXACT function ovmx_init.c's start_cluster_port() calls
 * (src/libvms/include/cluster_boot_gate.h), so this test and the shipping
 * boot path can never drift apart -- there is no second copy of the
 * decision anywhere.
 *
 * This is the honest near-term proof for a decision the R4 booted-node leg
 * (tests/qemu/test_cluster_start_negctl.sh) proves end-to-end in the lab:
 * this test proves the ARITHMETIC (which VAXCLUSTER values start the port),
 * not that a real PEA0:/HELLO appears -- that needs the kernel module and a
 * tap, which is the negctl script's job.
 */
#include "cluster_boot_gate.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

static void expect(uint32_t vaxcluster, int want)
{
    int got = cluster_start_wanted(vaxcluster);

    if ((got != 0) != (want != 0)) {
        fprintf(stderr,
                "FAIL: cluster_start_wanted(%u) = %d, want %d\n",
                (unsigned)vaxcluster, got, want);
        failures++;
    }
}

int main(void)
{
    /* VAXCLUSTER=0: never a cluster member -- the plan row's own negctl:
     * no PEA0:, no HELLO, CLUSTER_START must not even be issued. */
    expect(0, 0);

    /* VAXCLUSTER=1: "a member only if a cluster is present" -- the port
     * still has to come up so it CAN observe whether one is (design
     * SS3.5's own "1 with a cluster present" join clause presupposes an
     * already-up port); the join-vs-standalone decision is FC-P3.9, out
     * of this item's scope. */
    expect(1, 1);

    /* VAXCLUSTER=2: always a cluster member -- the port starts. */
    expect(2, 1);

    /* Any other non-zero value: still "start the port" -- the gate is a
     * pure zero/non-zero test, exactly matching vms_pe_start()'s own
     * `if (cl->params.vaxcluster == 0) return SS__NOSUCHDEV;` check
     * (src/kernel-core/vms_pe.c, FC-P0.9), so the two never disagree. */
    expect(3, 1);
    expect(255, 1);

    if (failures) {
        fprintf(stderr, "test_cluster_boot_gate: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_cluster_boot_gate: OK\n");
    return 0;
}
