/*
 * LIBICB.H - VMS LIB$ Invocation Context Block Definitions
 *
 * OpenVMX compatibility layer - Defines the INVO_CONTEXT_BLK structure
 * (typedef INVO_CONTEXT_BLK) and LIBICB$_ constants used by
 * lib$get_curr_invo_context, lib$get_prev_invo_context,
 * lib$i64_get_curr_invo_context, lib$x86_get_curr_invo_context,
 * and related traceback/stack-walk routines.
 *
 * The Invocation Context Block (ICB) captures the machine state of
 * a call frame for use during stack traversal.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual
 *            HP C User's Guide for OpenVMS Systems
 */

#ifndef __LIBICB_H
#define __LIBICB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * LIBICB$K_INVO_CONTEXT_VERSION — ICB version constant
 *
 * Must be passed to lib$i64_init_invo_context and
 * lib$x86_init_invo_context as the version argument.
 * ================================================================ */

#define LIBICB$K_INVO_CONTEXT_VERSION   1

/* ================================================================
 * INVO_CONTEXT_BLK — Invocation Context Block
 *
 * Architecture-aware structure holding register state and stack
 * frame pointers for one call frame.  This is a stub definition
 * with the fields accessed by corpus programs.
 *
 * The actual structure is architecture-specific; this definition
 * covers the union of Alpha, IA64, and x86_64 field names.
 * ================================================================ */

struct _invo_context_blk {
    /* Common fields */
    uint32_t libicb$v_bottom_of_stack;  /* Non-zero if this is the last frame */
    uint32_t libicb$v_handler_present;  /* Non-zero if a handler is registered */

    /* Alpha-specific: integer registers (32 x 64-bit) */
    uint64_t libicb$q_ireg[32];         /* Alpha integer registers (r0-r31) */

    /* Alpha/generic: program counter */
    uint64_t libicb$q_program_counter;  /* Program counter (Alpha) */
    uint64_t libicb$q_stack_pointer;    /* Stack pointer */

    /* IA64-specific: instruction pointer */
    void    *libicb$ih_pc;              /* IA64 instruction pointer */
    void    *libicb$ih_ip;              /* x86_64 instruction pointer */

    /* Padding to ensure structure is large enough for all architectures */
    uint8_t  libicb$reserved[512];
};

typedef struct _invo_context_blk INVO_CONTEXT_BLK;

#ifdef __cplusplus
}
#endif

#endif /* __LIBICB_H */
