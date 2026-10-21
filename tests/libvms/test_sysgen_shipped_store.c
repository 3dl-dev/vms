/*
 * test_sysgen_shipped_store.c - a parameter the executive READS must exist in
 * the store that SHIPS (rd vms-c2b, for rd vms-6ad1's RA7).
 *
 * WHAT WENT WRONG, AND IS WHAT THIS PINS. rd vms-ed5 shipped the whole path
 * for OVMX_RM_RESIDUE_FILL -- PID 1 reads it with sysgen_read_param(), hands it
 * to the executive in the cluster parameter set, and the op-0x14 builder writes
 * it across the residue span. Every piece worked and the knob was still
 * UNREACHABLE, because the parameter existed in no SYSGEN store: SYSBOOT> SET
 * and SYSGEN SET operate on the working set a store was loaded into, and
 * USE CURRENT does NOT merge the factory table over it
 * (tools/vms_sysgen.c's use_cmd), so SET answered %SYSGEN-E-NOSUCHP and
 * sysgen_read_param() answered "not found" forever.
 *
 * So this suite reads the REAL SHIPPED STORE --
 * distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/OVMXVMSSYS.PAR, the file
 * distro/Dockerfile.bootable masters onto the system volume -- through the
 * RUNTIME READER, and asserts the parameter is there, is numeric, and defaults
 * to the rd vms-42de ruling's 0. The NEGATIVE CONTROL is in the same run: a
 * name that is NOT in the store must FAIL the same reader, so the positive
 * assertion cannot pass vacuously.
 *
 * This is deliberately a test about the SHIPPED FILE and not about a temp store
 * this test writes itself: a store this test wrote would have proven only that
 * the format round-trips, which test_sysgen_identity.c already proves, and
 * would not have caught the gap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sysgen_params.h"
#include "sysgen_store_path.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

int main(void)
{
    uint32_t value = 0xdeadbeefu;
    char node[16];

    printf("test_sysgen_shipped_store: the shipped OVMXVMSSYS.PAR carries the "
           "parameters the executive reads (rd vms-c2b)\n");

    setenv("OVMX_SYSGEN_PATH", SHIPPED_SYSGEN_STORE, 1);
    printf("  store: %s\n", SHIPPED_SYSGEN_STORE);

    /* The knob RA7 selects its two fires with. */
    check(sysgen_read_param("OVMX_RM_RESIDUE_FILL", &value) == 0,
          "OVMX_RM_RESIDUE_FILL is IN the shipped store (so SYSBOOT> SET can "
          "reach it; absent, the knob answers %SYSGEN-E-NOSUCHP)");
    check(value == 0,
          "its shipped value is 0 -- the rd vms-42de ruling's zeros, never a "
          "pattern a lab arm left behind");

    /* NEGATIVE CONTROL, in the same run: the reader really can say no. */
    value = 0xdeadbeefu;
    check(sysgen_read_param("OVMX_NO_SUCH_KNOB", &value) != 0,
          "a parameter that is NOT in the store FAILS the same reader "
          "(the assertion above is not vacuous)");

    /* The store is otherwise untouched: its identity still reads. */
    memset(node, 0, sizeof(node));
    check(sysgen_read_string("SCSNODE", node, sizeof(node)) == 0 &&
          strcmp(node, "OVMX") == 0,
          "SCSNODE still reads OVMX -- adding a parameter did not disturb the "
          "store's identity");

    printf("test_sysgen_shipped_store: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
