/*
 * vms/chfdef.h - port include-surface shim (vms-714c, R5).
 *
 * The alpha-dec-vms GCC port's own host sources (e.g.
 * libgcc/config/alpha/vms-gcc_shell_handler.c) `#include <vms/chfdef.h>`
 * unchanged - a VMS-convention header name (canonicalized: lowercase
 * basename + ".h", under a directory literally named "vms") that a native
 * VMS DEC C compiler resolves via the STARLET/SYS$STARLET_C text libraries.
 * See docs/design-gcc-port-surface-gaps-register.md S1.2 row R5.
 *
 * CLEAN-ROOM (Rule 8): the two structures below are the literal shapes the
 * port's own host sources dereference (chf$mech_array.chf$q_mch_frame in
 * vms-gcc_shell_handler.c and vms-unwind.h), reconstructed from the public
 * "HP OpenVMS Calling Standard" condition-handling chapter and the OpenVMS
 * Programming Concepts Manual chapter 9 (signal/mechanism arrays) - never
 * from VSI/HPE source.
 *
 * DELIBERATELY SEPARATE from OVMX's own internal CHF representation
 * (src/libvms/include/chfdef.h, owned by the vms-1fa/vms-2e72 condition-
 * handling lane): that header models OVMX's own emulated handler-stack
 * dispatch and is free to evolve independently. This one exists ONLY to
 * satisfy the port's literal field references so its host sources compile
 * UNCHANGED against our cross cc1 - it is not wired into OVMX's own SYS$SETEXV
 * dispatch path.
 */
#ifndef __VMS_CHFDEF_H
#define __VMS_CHFDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Signal array (CHF$SIGNAL_ARRAY): passed as the first condition-handler
 * argument. The port's LIB2ADD sources only ever pass this through by
 * pointer (never dereference a field of it), so a tag declaration with the
 * documented leading fields is sufficient for those TUs to compile. */
struct chf$signal_array {
    uint32_t chf$is_sig_args;   /* argument count */
    uint32_t chf$is_sig_name;   /* condition value */
    uint32_t chf$is_sig_arg1;   /* first FAO argument; more follow dynamically */
};

/* Mechanism array (CHF$MECH_ARRAY): passed as the second condition-handler
 * argument. chf$q_mch_frame ("mechanism-args frame pointer", a quadword on
 * Alpha) is the literal field libgcc/config/alpha/vms-gcc_shell_handler.c
 * reads to locate the establisher's frame - the field this shim exists to
 * provide. A few sibling quadword fields are declared alongside it for
 * layout headroom (the port's deeper EH glue, libgcc/config/alpha/
 * vms-unwind.h, reads more of this structure plus a CHFCTX type this shim
 * does NOT yet provide - out of scope for vms-714c, tracked under vms-2e72). */
struct chf$mech_array {
    uint32_t chf$is_mch_args;    /* argument count */
    uint32_t chf$is_mch_flags;   /* mechanism-array flags */
    uint64_t chf$q_mch_frame;    /* establisher frame pointer (quadword) */
    uint64_t chf$q_mch_depth;    /* call depth of the establisher */
    uint64_t chf$q_mch_esf_addr; /* exception-stack-frame address */
    uint64_t chf$q_mch_savr0;    /* saved R0 */
    uint64_t chf$q_mch_savr1;    /* saved R1 */
};

#ifdef __cplusplus
}
#endif

#endif /* __VMS_CHFDEF_H */
