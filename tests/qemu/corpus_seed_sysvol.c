/*
 * corpus_seed_sysvol.c - boot the corpus guest's SYSTEM DISK (vms-44a, R2.3).
 *
 * A VMS image that reads SYS$SYSTEM:SYSUAF.DAT ($GETUAI, $CREPRC's account
 * lookup) needs what a booted system has: SYS$SYSDEVICE naming a MOUNTED
 * Files-11 volume that carries [SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT. The guest rig
 * has no STARTUP.COM, so this fixture does STARTUP's two steps once, ahead of the
 * corpus loop (init.sh, ovmx.corpus mode):
 *
 *   1. publish the system unit (OVMX_SYSDEVICE -- the variable the boot chain
 *      uses, see lnm_setup_defaults()) = VDA300:, the generated system-disk ODS-2
 *      fixture tests/qemu stages (mkimage_ods2_sysvol: the REAL shipped SYSUAF.DAT
 *      and RIGHTSLIST.DAT), and define the system logicals over it in the
 *      executive-resident LNM$SYSTEM table;
 *   2. $MOUNT that volume (executive-global, so every later process sees it).
 *
 * It replaces corpus_seed_lnm (which defines the same logicals over the boot
 * default VDA0:) in corpus mode only. Not a suite: no PASS/FAIL lines.
 */
#include <stdio.h>
#include <stdlib.h>

#include "ovmx_layout.h"
#include "vms/logical.h"
#include "vmsfs/device.h"
#include "vms_kif.h"

#define SYSVOL_UNIT "VDA300:"

int main(void)
{
    setenv("OVMX_SYSDEVICE", SYSVOL_UNIT, 1);
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);
    uint32_t st = vms_kif_acp_mount(SYSVOL_UNIT);
    printf("corpus_seed_sysvol: SYS$SYSDEVICE -> %s, $MOUNT status %u\n", SYSVOL_UNIT, st);
    return (st & 1) ? 0 : 1;
}
