/*
 * sys_float.c - Floating-Point System Services
 *
 * Implements:
 *
 *   SYS$CHECK_FEN - Check Floating-point ENable status
 *
 * On OpenVMS Alpha and IA-64, the FEN (Floating-point ENable) bit in
 * the hardware PCB controls whether floating-point instructions are
 * available to the process. SYS$CHECK_FEN returns the current state.
 *
 * On IA-64, the optional *flags longword receives individual FP bank
 * enable bits:
 *   Bit 0 - low floating-point register bank (f2..f31) enabled
 *   Bit 1 - high floating-point register bank (f32..f127) enabled
 *
 * On Linux/x86-64 and aarch64, floating-point is always enabled for
 * user processes. We therefore always return 1 (enabled) and set
 * *flags to 0 (flag bits not applicable on this platform).
 *
 * Reference: OpenVMS System Services Reference Manual (SYS$CHECK_FEN)
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * $CHECK_FEN cited vms-5b4 until vms-fab; it is closed. vms-f90 owns the eight
 * compute-only services, and this is the one of them with the sharpest doubt:
 * a service whose answer is a compiled-in constant is the declared-stub shape
 * vms-09c (INV-6) exists to find.
 *
 * OVMX-USERSPACE: sys$check_fen (vms-f90) -- returns compiled-in constants
 *     (*flags = 0, return 1) without reading any hardware PCB, process or
 *     executive state; the answer is the same on every system and every
 *     process, whatever the FEN bit would say.
 */

#include <stdint.h>
#include "ssdef.h"
#include "starlet.h"

/*
 * sys$check_fen - Check whether floating-point is enabled.
 *
 * Parameters:
 *   flags - Optional pointer to longword that receives floating-point
 *           bank enable flags (IA-64 only; set to 0 on other platforms)
 *
 * Returns: 1 if floating-point is enabled, 0 if disabled.
 *          On Linux this is always 1.
 *          (Note: return value is a boolean, not a VMS status code.)
 */
uint32_t sys$check_fen(uint32_t *flags)
{
    /*
     * On Linux, floating-point is always enabled for user-space
     * processes. There is no per-process FEN bit equivalent.
     */
    if (flags)
        *flags = 0;   /* No IA-64 FP bank bits to report */

    return 1;         /* Floating-point is enabled */
}
