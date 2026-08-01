/*
 * LKSDEF.H - OVMX Lock Status Block (LKSB) Layout
 *
 * OVMX DESIGN CHOICE, NOT VMS-AUTHENTIC (CLAUDE.md Rule 8). This is NOT a
 * transcription of a VSI/HPE-published byte layout. The OpenVMS Programming
 * Concepts Manual's $ENQ/$ENQW description documents the LKSB only at the
 * field level (a status word, a reserved word, a longword lock ID, and --
 * when LCK$M_VALBLK is set -- a 16-byte value block); it does NOT publish a
 * byte-offset table. Checked directly against the live OpenVMS VAX 7.3
 * oracle (~/vax/cluster) during vms-1d9 round-2 adversarial review:
 * SYS$LIBRARY:STARLET.MLB contains NO $LKSB macro at all (LIBRARIAN
 * reports %LIBRAR-W-NOMTCHFOU, "no such module") -- there is no VMS-
 * authentic macro to pin this layout against, because OpenVMS callers are
 * expected to declare the LKSB storage themselves (traditionally 2
 * longwords, or a language-specific record) rather than including a
 * library-supplied structure definition.
 *
 * The layout below (status, reserved, lkid, valblk[16]) reproduces
 * src/libvms/syssvc/sys_lock.c's existing, already-implemented private
 * struct field order, promoted here to a public header (zero behavior
 * change) so external callers (tests, future DCL/RTL code) have one
 * shared definition instead of each call site guessing sys_lock.c's
 * internal layout. It satisfies the field-level LKSB CONTRACT the manual
 * describes (same fields, same order, same sizes) -- it is an OVMX
 * IMPLEMENTATION of that contract, not a lift of a published VMS struct.
 */

#ifndef __LKSDEF_H
#define __LKSDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lksb {
    uint16_t lksb$w_status;     /* Completion status (SS$_xxx) */
    uint16_t lksb$w_reserved;
    uint32_t lksb$l_lkid;       /* Lock ID, assigned by the lock manager */
    char     lksb$b_valblk[16]; /* Lock value block (valid iff LCK$M_VALBLK) */
};

#ifdef __cplusplus
}
#endif

#endif /* __LKSDEF_H */
