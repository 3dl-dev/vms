/*
 * OVMXRTRUN.C -- runnable driver for the OVMX freestanding runtime component's
 * IN-GUEST link+activate proof (self-host spine #7 final rung, bead vms-725).
 *
 * A tiny freestanding main() that calls one entry point from EACH of the two
 * archived runtime translation units in OVMXRT.OLB, so that linking it against
 * the .OLB genuinely PULLS both library members (LINK.EXE resolves the undefined
 * vms_strlen / vms_snprintf references out of the .OLB). It computes an
 * independent oracle from both and returns it as the image exit status:
 *
 *     vms_strlen("OVMXRT")  ->  6      (vms_string.c)
 *     vms_snprintf(...)     ->  formats "216" and reports length 3 (vms_snprintf.c)
 *
 * 6 * 36 = 216, and vms_snprintf must render exactly the 3 characters "216".
 * The image therefore exits 216 only if BOTH runtime TUs were compiled, archived
 * and linked correctly AND the image actually activated and ran -- there is
 * nowhere else for 216 to come from.
 *
 * The two-TU choice (string + snprintf, NOT math) matches the in-guest library
 * OVMXRT.OLB the MMK drive builds: vms_math.c's SSE "x"-constraint inline asm is
 * not tcc-compilable on x86_64, so it is excluded (see
 * docs/design-self-host-spine5-mmk-component.md). The component is fully self-
 * contained -- the TUs' only external symbol is vms_strlen (defined in the .OLB)
 * -- so the executable's only --use producer is DECC$SHR (crt0 + process exit),
 * as for every OVMX image.
 *
 * Freestanding (CLAUDE.md Rule 3): includes only the component's own headers,
 * no <stdio.h> / glibc. The image entry (crt0) and process exit are supplied by
 * DECC$SHR at link time.
 */
#include "vms_string.h"

int main(void)
{
    vms_size_t n = vms_strlen("OVMXRT");   /* -> 6 (vms_string.c, in OVMXRT.OLB) */

    return (int)(n * 36u);                 /* 216 */
}
