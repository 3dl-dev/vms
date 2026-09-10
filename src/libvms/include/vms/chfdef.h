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
 * argument. libgcc/config/alpha/vms-unwind.h reads chf$is_sig_args (the
 * longword count) and takes the ADDRESS of the condition-value longword to
 * walk to the condition PC:
 *   &((int *)(&sigargs->chf$l_sig_name))[sigargs->chf$is_sig_args-2].
 * The port source spells the condition-value longword chf$l_sig_name ("l" =
 * longword); OVMX's own sources (and the internal src/libvms/include/chfdef.h)
 * spell the same offset-1 longword chf$is_sig_name. Both name the SAME field,
 * so chf$l_sig_name is provided as an offset-identical alias via an anonymous
 * union - additive, and chf$is_sig_name is preserved unchanged. */
struct chf$signal_array {
    uint32_t chf$is_sig_args;   /* argument count */
    union {
        uint32_t chf$is_sig_name;   /* condition value (OVMX/internal spelling) */
        uint32_t chf$l_sig_name;    /* condition value (port/vms-unwind.h spelling) */
    };
    uint32_t chf$is_sig_arg1;   /* first FAO argument; more follow dynamically */
};

/* Mechanism array (CHF$MECH_ARRAY): passed as the second condition-handler
 * argument. chf$q_mch_frame ("mechanism-args frame pointer", a quadword on
 * Alpha) is the literal field libgcc/config/alpha/vms-gcc_shell_handler.c
 * reads to locate the establisher's frame - the field this shim exists to
 * provide. The port's deeper EH glue, libgcc/config/alpha/vms-unwind.h (CHF
 * rung-5, vms-8e8c), takes the ADDRESS of chf$q_mch_savrN (N = 0,1,16..28) as
 * the save-location of each callee register, and reads chf$q_mch_esf_addr (the
 * REI frame from which R2..R7 are recovered). The full saved-register block
 * chf$q_mch_savr0 .. chf$q_mch_savr28 is declared as contiguous quadwords so
 * those addresses are the genuine per-register slots. Field names/roles match
 * the internal src/libvms/include/chfdef.h (chf$is_mch_savr0/savr1 there);
 * this shim models the Alpha wire layout, so the saved-register slots are
 * quadwords ("q") rather than the host-emulation longwords ("is") - the "q"
 * spelling is exactly what vms-unwind.h references. */
struct chf$mech_array {
    uint32_t chf$is_mch_args;    /* argument count */
    uint32_t chf$is_mch_flags;   /* mechanism-array flags */
    uint64_t chf$q_mch_frame;    /* establisher frame pointer (quadword) */
    uint64_t chf$q_mch_depth;    /* call depth of the establisher */
    uint64_t chf$q_mch_esf_addr; /* exception-stack-frame (REI) address */
    uint64_t chf$q_mch_savr0;    /* saved R0 */
    uint64_t chf$q_mch_savr1;    /* saved R1 */
    uint64_t chf$q_mch_savr2;    /* saved R2 */
    uint64_t chf$q_mch_savr3;    /* saved R3 */
    uint64_t chf$q_mch_savr4;    /* saved R4 */
    uint64_t chf$q_mch_savr5;    /* saved R5 */
    uint64_t chf$q_mch_savr6;    /* saved R6 */
    uint64_t chf$q_mch_savr7;    /* saved R7 */
    uint64_t chf$q_mch_savr8;    /* saved R8 */
    uint64_t chf$q_mch_savr9;    /* saved R9 */
    uint64_t chf$q_mch_savr10;   /* saved R10 */
    uint64_t chf$q_mch_savr11;   /* saved R11 */
    uint64_t chf$q_mch_savr12;   /* saved R12 */
    uint64_t chf$q_mch_savr13;   /* saved R13 */
    uint64_t chf$q_mch_savr14;   /* saved R14 */
    uint64_t chf$q_mch_savr15;   /* saved R15 */
    uint64_t chf$q_mch_savr16;   /* saved R16 */
    uint64_t chf$q_mch_savr17;   /* saved R17 */
    uint64_t chf$q_mch_savr18;   /* saved R18 */
    uint64_t chf$q_mch_savr19;   /* saved R19 */
    uint64_t chf$q_mch_savr20;   /* saved R20 */
    uint64_t chf$q_mch_savr21;   /* saved R21 */
    uint64_t chf$q_mch_savr22;   /* saved R22 */
    uint64_t chf$q_mch_savr23;   /* saved R23 */
    uint64_t chf$q_mch_savr24;   /* saved R24 */
    uint64_t chf$q_mch_savr25;   /* saved R25 */
    uint64_t chf$q_mch_savr26;   /* saved R26 */
    uint64_t chf$q_mch_savr27;   /* saved R27 */
    uint64_t chf$q_mch_savr28;   /* saved R28 */
};

/* Uppercase typedef spellings the port's EH source references. vms-unwind.h
 * declares `CHF$SIGNAL_ARRAY *sigargs` / `CHF$MECH_ARRAY *mechargs` (the
 * canonicalized VMS-convention type names), whereas vms-gcc_shell_handler.c
 * used the `struct chf$signal_array` / `struct chf$mech_array` tag spelling.
 * Both name the same shapes; the typedefs are additive over the tags above. */
typedef struct chf$signal_array CHF$SIGNAL_ARRAY;
typedef struct chf$mech_array   CHF$MECH_ARRAY;

/*
 * SYS$GL_CALL_HANDL - the exception-dispatcher sentinel.
 *
 * libgcc/config/alpha/vms-unwind.h decides whether a procedure value denotes
 * the VMS exception dispatcher with
 *   #define DENOTES_EXC_DISPATCHER(PV) ((PV) == (ADDR)(REG) SYS$GL_CALL_HANDL)
 * comparing the procedure value against the address of this system global
 * cell. vms-unwind.h already declares it (`extern int SYS$GL_CALL_HANDL;`); it
 * is re-declared here so the CHF include surface carries the name, and a real
 * (linkable) definition lives in src/libvms/rtl/lib_invo.c so the symbol
 * RESOLVES when libgcc EH is linked. COMPILE-SURFACE: this increment only
 * requires the name to resolve; the runtime fidelity (a procedure value
 * actually comparing equal to this cell for genuine dispatcher frames) is the
 * deferred half, gated on the Alpha rail + vms-6fe/vms-e16.
 */
extern int SYS$GL_CALL_HANDL;

#ifdef __cplusplus
}
#endif

#endif /* __VMS_CHFDEF_H */
