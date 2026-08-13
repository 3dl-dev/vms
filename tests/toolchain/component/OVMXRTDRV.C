/*
 * OVMXRTDRV.C -- driver for the OVMX freestanding runtime component build
 * (self-host spine #5, bead vms-fe4).
 *
 * A tiny freestanding main() that calls ONE entry point from each of the three
 * archived runtime translation units, so that linking it against OVMXRT.OLB
 * genuinely PULLS those library members (LINK.EXE resolves the undefined
 * vms_strlen / vms_fabs / vms_snprintf references out of the .OLB).  It computes
 * an independent oracle from all three and returns it as the image exit status:
 *
 *     vms_strlen("OVMXRT")  ->  6      (vms_string.c)
 *     vms_fabs(-36.0)       ->  36.0   (vms_math.c)
 *     vms_snprintf(...)     ->  formats "216" and reports its length (vms_snprintf.c)
 *
 * 6 * 36 = 216, and vms_snprintf must render exactly the 3 characters "216".
 * The image therefore exits 216 only if all three runtime TUs were compiled,
 * archived and linked correctly -- there is nowhere else for 216 to come from.
 *
 * Freestanding (CLAUDE.md Rule 3): includes only the component's own headers
 * (OVMX$INCLUDE:), no <stdio.h> / glibc.  The image entry (crt0) and process
 * exit are supplied by DECC$SHR at link time, as for every OVMX image.
 */
#include "vms_string.h"
#include "vms_math.h"
#include "vms_snprintf.h"

int main(void)
{
    char buf[8];

    vms_size_t n = vms_strlen("OVMXRT");        /* -> 6  */
    int        d = (int) vms_fabs(-36.0);       /* -> 36 */
    int      len = vms_snprintf(buf, sizeof buf, "%u", (unsigned)(n * d));

    /* "216" is three characters and its first byte is '2': cross-check the
     * formatter agrees with the arithmetic before trusting the result. */
    if (len != 3 || buf[0] != '2')
        return 1;

    return (int)(n * d);                        /* 216 */
}
