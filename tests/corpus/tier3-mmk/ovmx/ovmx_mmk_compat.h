/*
 * ovmx_mmk_compat.h - OVMX build-compatibility shim for the vendored MadGoat
 *                     MMK (self-host spine #4, bead vms-ec70).
 *
 * *** OVMX DESIGN CHOICE (clean-room, Rule 8) ***
 *
 * MadGoat MMK is third-party BSD freeware (tests/corpus/tier3-mmk/, NOT
 * VSI/HPE/DEC source).  It was written for DEC C on OpenVMS and uses a handful
 * of DEC-C storage-class keywords and older RTL call *arities* that OVMX's C
 * headers do not spell the same way.  This header is force-included (cc
 * -include) into every MMK translation unit so the *stock vendored source*
 * compiles against the OVMX RTL unmodified, EXCEPT for a small number of edits
 * that are tagged "OVMX (vms-ec70)" directly in the vendored files (grep for
 * that tag to see every one).
 *
 * The three things this header does:
 *   1. Neutralise the DEC-C storage-class keywords (globaldef/globalref/
 *      readonly/noshare/variant_*) that ISO C does not have.
 *   2. Reconcile struct/constant spellings MMK expects with OVMX's headers
 *      (struct tpadef -> struct _tpadef; TPA$C_LENGTH0; the NAM$M_/IO$M_/DVI$_/
 *      JPI$_ constants MMK's non-load-bearing terminal/CTRL-T code names; the
 *      XABFHC/XABRDT record-attribute XABs — MMK-local, OVMX RMS ignores the
 *      XAB chain, so these carry MMK's expected fields only).
 *   3. Adapt the RTL call *arity* differences.  VMS system services take
 *      optional trailing arguments (AST addresses, item lists) that MMK omits;
 *      OVMX declares them with fixed prototypes.  We include the real OVMX
 *      prototypes FIRST (locking their include guards), then map each affected
 *      name to a variadic forwarding wrapper (ovmx_mmk_*, defined in
 *      ovmx_mmk_compat.c) that supplies the omitted arguments and calls the
 *      real OVMX routine.  No OVMX header or RTL implementation is weakened.
 */
#ifndef OVMX_MMK_COMPAT_H
#define OVMX_MMK_COMPAT_H

/* --- 1. Pull in the real OVMX RTL headers FIRST, so their include guards are
 *        set and their real prototypes are parsed BEFORE any wrapper macro
 *        below can rename an identifier.  When MMK's own sources later
 *        #include these, the guards make them no-ops. -------------------- */
#include <stdint.h>
#include <descrip.h>
#include <ssdef.h>
#include <stsdef.h>
#include <rms.h>
#include <rmsdef.h>
#include <starlet.h>
#include <lib$routines.h>
#include <str$routines.h>
#include <ots$routines.h>
#include <libclidef.h>
#include <clitable.h>
#include <libdef.h>
#include <tpadef.h>
#include <fscndef.h>
#include <iodef.h>
#include <dvidef.h>
#include <jpidef.h>

/* --- 2a. DEC-C storage classes (ISO C has no equivalent; these are the
 *         placement/linkage hints DEC C accepts).  globaldef/globalref map to
 *         ordinary external linkage; readonly to const; the rest vanish. --- */
#define globaldef
#define globalref   extern
#define readonly    const
#define noshare
#define variant_struct struct
#define variant_union  union
/* MMK's vendored clidefs.h re-declares the CLI$_ status codes with DEC-C
 * `globalvalue`, which OVMX already provides as macros in <libclidef.h>.
 * Suppress the vendored header by pre-defining its include guard; the CLI$
 * callable prototypes come from <clitable.h> (included above). */
#define clidefs_h__

/* --- 2b. struct-name reconciliation: MMK writes `struct tpadef`; OVMX's
 *         <tpadef.h> spells the tag `struct _tpadef` (typedef TPADEF).  Only
 *         MMK's own use sites are affected (all OVMX headers are already
 *         included above). ------------------------------------------------ */
#define tpadef _tpadef

/* --- 2c. Constants MMK names that OVMX's headers do not (yet) define.
 *         Values transcribed from the public $TPADEF/$IODEF/$DVIDEF/$JPIDEF
 *         definitions (clean-room, Rule 8).  The IO$M_/DVI$_/JPI$_ ones are
 *         used only by MMK's terminal CTRL-T attention code (misc.c), which is
 *         not on the parse/build path this spine exercises. --------------- */
#ifndef TPA$C_LENGTH0
#define TPA$C_LENGTH0   ((int)sizeof(struct _tpadef))  /* base TPARSE block bytes */
#endif
#ifndef TPA$C_LENGTHN
#define TPA$C_LENGTHN   ((int)sizeof(struct _tpadef))
#endif
#ifndef IO$M_WRTATTN
#define IO$M_WRTATTN    0x0100   /* $QIO func modifier: write-attention AST */
#endif
#ifndef IO$M_READATTN
#define IO$M_READATTN   0x0200   /* $QIO func modifier: read-attention AST */
#endif
#ifndef IO$M_OUTBAND
#define IO$M_OUTBAND    0x0080   /* $QIO func modifier: out-of-band AST */
#endif
#ifndef DVI$_TRM
#define DVI$_TRM        16       /* $GETDVI: is-a-terminal (boolean) */
#endif
#ifndef DVI$_DEVBUFSIZ
#define DVI$_DEVBUFSIZ  19       /* $GETDVI: device buffer size */
#endif
#ifndef JPI$_DIOCNT
#define JPI$_DIOCNT     0x040c   /* $GETJPI: direct-I/O count */
#endif
#ifndef JPI$_BIOCNT
#define JPI$_BIOCNT     0x040b   /* $GETJPI: buffered-I/O count */
#endif

/* --- 2d. XABFHC / XABRDT record-attribute XABs.  MMK chains these off its FAB
 *         to read a file's longest-record-length (xab$w_lrl) and revision
 *         date/time (xab$q_rdt).  OVMX RMS does not consume the XAB chain, so
 *         these are MMK-local structures carrying exactly the fields MMK
 *         touches; the fields stay zero (MMK's own `== 0 ? default` fallbacks
 *         then apply — a big read buffer, and an RDT of 0 == "always stale",
 *         which is correct for a from-scratch build).  See the deferred-gap
 *         note in the PR: precise LRL/RDT would need OVMX RMS XAB support. -- */
struct XABFHC { uint16_t xab$w_lrl; uint32_t xab$l_ebk; uint16_t xab$w_ffb; };
struct XABRDT { uint8_t  xab$q_rdt[8]; uint16_t xab$w_rvn; };
#define cc$rms_xabfhc  ((struct XABFHC){0})
#define cc$rms_xabrdt  ((struct XABRDT){{0}})
#ifndef cc$rms_xabpro
/* (cc$rms_xabpro exists in OVMX rms.h if MMK ever needs it; guard only.) */
#endif

/* --- 3. RTL call-arity adapters.  Each variadic wrapper is defined in
 *        ovmx_mmk_compat.c (which does NOT force-include this header, so the
 *        names below resolve to the real OVMX routines there).  Function-like
 *        macros expand only at MMK's call sites; the real prototypes were
 *        already parsed above. ---------------------------------------------- */
uint32_t ovmx_mmk_sys_parse   (void *fab, ...);
uint32_t ovmx_mmk_sys_search  (void *fab, ...);
uint32_t ovmx_mmk_sys_filescan(const void *srcstr, void *valuelst, void *fldflags, ...);
uint32_t ovmx_mmk_lib_getdvi  (const void *item_code, void *chan, void *devnam, void *result, ...);
uint32_t ovmx_mmk_ots_cvt_tu_l(const void *src, void *dest, ...);
uint32_t ovmx_mmk_str_position(const void *src, const void *sub, ...);
uint32_t ovmx_mmk_lib_get_symbol(const void *sym, void *val, ...);
uint32_t ovmx_mmk_lib_set_logical(const void *lognam, const void *eqvnam, ...);
uint32_t ovmx_mmk_create_vm_zone(uint32_t *zone_id, ...);

#define sys$parse(...)      ovmx_mmk_sys_parse(__VA_ARGS__)
#define sys$search(...)     ovmx_mmk_sys_search(__VA_ARGS__)
#define sys$filescan(...)   ovmx_mmk_sys_filescan(__VA_ARGS__)
#define lib$getdvi(...)     ovmx_mmk_lib_getdvi(__VA_ARGS__)
#define ots$cvt_tu_l(...)   ovmx_mmk_ots_cvt_tu_l(__VA_ARGS__)
#define str$position(...)   ovmx_mmk_str_position(__VA_ARGS__)
#define lib$get_symbol(...) ovmx_mmk_lib_get_symbol(__VA_ARGS__)
#define lib$set_logical(...) ovmx_mmk_lib_set_logical(__VA_ARGS__)
#define lib$create_vm_zone(...) ovmx_mmk_create_vm_zone(__VA_ARGS__)

/* --- 4. va_count() replacement.  DEC C's va_count(n) sets n to the number of
 *        arguments the current variadic function was called with, using the
 *        VAX/Alpha/I64 calling-standard argument-count register.  The GCC/SysV
 *        x86-64 ABI has NO such register, so va_count cannot work at runtime.
 *        Instead we count the arguments at each CALL SITE with the standard
 *        preprocessor arg-counting trick (OVMX_NARG) and pass the count as a
 *        leading parameter.  mmk.h's OVMX seam turns cat()/Define_Symbol() —
 *        the only two va_count users — into their ovmx_* counted forms; the
 *        function bodies (misc.c / symbols.c) read the passed count instead of
 *        calling va_count.  (OVMX design choice, Rule 8.) ------------------- */
#define OVMX_NARG(...)  OVMX_NARG_(__VA_ARGS__, OVMX_RSEQ_N())
#define OVMX_NARG_(...) OVMX_ARG_N(__VA_ARGS__)
#define OVMX_ARG_N( \
     _1, _2, _3, _4, _5, _6, _7, _8, _9,_10,_11,_12,_13,_14,_15,_16, \
    _17,_18,_19,_20,_21,_22,_23,_24,_25,_26,_27,_28,_29,_30,_31,_32, N,...) N
#define OVMX_RSEQ_N() \
    32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17, \
    16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0

#endif /* OVMX_MMK_COMPAT_H */
