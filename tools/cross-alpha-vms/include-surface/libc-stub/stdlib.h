/* stdlib.h - ambient-CRTL stub for the vms-unwind.h include-surface proof
 * (vms-8e8c, CHF rung-5).
 *
 * NOT the OVMX vms/ include surface under test, and NOT a port source. The
 * alpha-dec-vms cross toolchain image is a bare compiler (only GCC's own
 * include dir is on the default path - no target libc headers), whereas a real
 * alpha-vms libgcc build compiles against the VMS DEC C RTL, which supplies
 * <stdlib.h>/<stdio.h>. libgcc/config/alpha/vms-unwind.h unconditionally
 * #includes both (for the EH_DEBUG getenv/atoi/printf path). This stub declares
 * ONLY what that verbatim port header references, so the proof isolates the
 * vms/ CRTL surface (pdscdef/libicb/chfctxdef/chfdef) rather than tripping over
 * the ambient libc. Present identically in the baseline and success compiles,
 * so it is not part of what the proof asserts.
 */
#ifndef __VMS_UNWIND_PROOF_STDLIB_H
#define __VMS_UNWIND_PROOF_STDLIB_H

#ifndef NULL
#define NULL ((void *)0)
#endif

extern char *getenv(const char *name);
extern int   atoi(const char *nptr);

#endif /* __VMS_UNWIND_PROOF_STDLIB_H */
