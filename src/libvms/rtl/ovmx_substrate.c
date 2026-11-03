/*
 * ovmx_substrate.c - the substrate scope (rd vms-a66e; ovmx_substrate.h).
 *
 * Its own translation unit, with no dependencies: every system service that
 * marks a scope links only this, never the RTL routines around it.
 */
#include "ovmx_substrate.h"

/*
 * THE SUBSTRATE SCOPE (rd vms-a66e; ovmx_substrate.h). Depth of the system
 * services / RTL routines now doing the substrate's own work. The C RTL file
 * layer (DECC$SHR, src/vmsrms/crtl_rms_fd.c) asks ovmx_substrate_active() and
 * leaves those calls to the kernel. One process-wide count, as the C RTL is
 * single-threaded (a non-TLS producer).
 */
static int ovmx_substrate_depth;

int ovmx_substrate_enter(void)
{
    return ++ovmx_substrate_depth;
}

void ovmx_substrate_leave(void)
{
    if (ovmx_substrate_depth > 0)
        ovmx_substrate_depth--;
}

int ovmx_substrate_active(void)
{
    return ovmx_substrate_depth != 0;
}

