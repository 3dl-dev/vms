/*
 * status_exit.c - the OVMX/VAX $STATUS proof image (rd vms-b869, parent
 * vms-3b3f). The VAX analogue of the Alpha native-image gate's RETST.
 *
 * Built twice by tests/netbsd/guest/CMakeLists.txt as ordinary Decision-A
 * OVMX/VAX images (elf32-vax, ld.elf_so, statically bound to the OVMX RTL --
 * the same shape as every shipped VAX image):
 *   STSNORM.EXE   OVMX_STATUS_COND = SS$_NORMAL (%X00000001)
 *   STSCOND.EXE   OVMX_STATUS_COND = %X0FEDC0A9 (a distinctive condition value
 *                 with SUCCESS severity, so DCL prints no message for it and
 *                 the only place it can show up is $STATUS)
 *
 * tests/lab-vax/SYSTARTUP_VMS_STATUS_PROOF.COM RUNs both from SYS$SYSTEM and
 * WRITEs ''$STATUS' to the console; tests/lab-vax/run-boot.sh status-gate
 * asserts the exact values. The image announces itself first, so a RUN that
 * never activated the image cannot pass on a status alone.
 *
 * It calls SYS$EXIT rather than returning from main: a C main's return value
 * goes through the C exit path, while SYS$EXIT is the VMS service that sets
 * the image's completion status -- the thing $STATUS reports.
 */
#include <stdint.h>
#include <stdio.h>

#include "starlet.h"

#ifndef OVMX_STATUS_NAME
#error "OVMX_STATUS_NAME must be defined (the image name the proof prints)"
#endif
#ifndef OVMX_STATUS_COND
#error "OVMX_STATUS_COND must be defined (the condition value passed to SYS$EXIT)"
#endif

int main(void)
{
    const uint32_t cond = (uint32_t)(OVMX_STATUS_COND);

    printf("OVMX-STATUS %s: image ran, calling SYS$EXIT(%%X%08X)\n",
           OVMX_STATUS_NAME, (unsigned)cond);
    /* SYS$EXIT does not return and does not run the C exit path, so flush
     * the announcement before calling it. */
    fflush(stdout);
    sys$exit(cond);
    return 0; /* not reached */
}
