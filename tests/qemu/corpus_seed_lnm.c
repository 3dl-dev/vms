/*
 * corpus_seed_lnm.c - harness FIXTURE (not a test suite) that seeds
 * LNM$SYSTEM's standard device/directory logicals before the device-corpus
 * programs run (vms-08c).
 *
 * WHY THIS EXISTS. tests/corpus/tier1-examples/lib_getdvi.c is an
 * unmodified Eight-Cubed download (tests/corpus/PROVENANCE.md -- never
 * edited) that asks $GETDVI about "SYS$SYSDEVICE:", the standard VMS
 * logical name for "the system disk". src/libvms/syssvc/sys_device.c's
 * sys$getdvi translates a devnam that is not a literal physical unit
 * through LNM$SYSTEM (device_lookup_translated(), added by vms-08c) --
 * genuine VMS $GETDVI behavior, reading the executive's own arena, never a
 * per-process guess. But this QEMU harness (tests/qemu/init.sh) never runs
 * STARTUP.COM -- it insmods vms.ko/vmsfs.ko and execs test binaries
 * directly -- so nothing has DEFINEd SYS$SYSDEVICE yet when lib_getdvi
 * would otherwise run.
 *
 * lnm_setup_defaults() (src/vmslnm/lnm_defaults.c) is the exact call
 * STARTUP.COM triggers on a real boot, and the exact call several existing
 * suites here already make for themselves for the same reason
 * (test_syssvc_rightslist.c, test_syssvc_sysuaf_uic_base.c,
 * test_syssvc_setuai.c, test_syssvc_scratch_writable.c). Those suites are
 * OUR code, so each can call it inline; lib_getdvi.c is not -- it cannot be
 * edited to add the call, so this fixture makes it once, into the SAME
 * executive-resident LNM$SYSTEM arena (LNM_MODE_EXEC, node-wide) every
 * later process on this boot reads via vms_kif_lnm_translate, including
 * lib_getdvi's own process.
 *
 * NOT a suite: no test_ prefix (tests/qemu/init.sh's suite loop globs
 * test_kmod_, test_syssvc_, test_imgact_ and test_corpus_, none of which
 * this matches), no PASS/FAIL lines, no meaningful non-zero exit -- it is a setup
 * step tests/qemu/init.sh runs explicitly and unconditionally, once, ahead
 * of the suite loop, so it does not depend on the loop's own (unspecified)
 * glob iteration order.
 */

#include <stdio.h>

#include "ovmx_layout.h"
#include "vms/logical.h"
#include "vmsfs/device.h"

int main(void)
{
    /* Same two-line bootstrap test_syssvc_rightslist.c and
     * test_syssvc_setuai.c use: the device table entry SYS$SYSDEVICE's
     * value names (VDA0: -> SYSDISK_MOUNT), then the logical itself. */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    printf("corpus_seed_lnm: LNM$SYSTEM seeded (SYS$SYSDEVICE -> VDA0: and friends)\n");
    return 0;
}
