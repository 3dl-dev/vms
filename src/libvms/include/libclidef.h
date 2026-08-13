/*
 * LIBCLIDEF.H - LIB$ CLI (Command Language Interpreter) Definitions
 *
 * OpenVMX compatibility layer - Defines constants for CLI callback
 * routines and lib$get_symbol / lib$set_symbol.
 *
 * On VMS, these constants are used to specify which symbol table
 * to operate on and to define callback actions for CLI integration.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual
 */

#ifndef __LIBCLIDEF_H
#define __LIBCLIDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Symbol table types for lib$get_symbol / lib$set_symbol
 *
 * These specify which symbol table to use.
 * ================================================================ */

#define LIB$K_CLI_LOCAL_SYM     1   /* Local symbol table */
#define LIB$K_CLI_GLOBAL_SYM    2   /* Global symbol table */

/* ================================================================
 * CLI callback request types
 *
 * Used with CLI callback routines to request specific operations
 * from the command language interpreter.
 * ================================================================ */

#define CLI$K_GETCMD        1   /* Get command line from user */
#define CLI$K_GETQUAL       2   /* Get qualifier value */
#define CLI$K_GETPAR        3   /* Get parameter value */
#define CLI$K_GETOPT        4   /* Get option value */
#define CLI$K_PRESENT       5   /* Check if qualifier present */
#define CLI$K_CLISERV       6   /* General CLI service request */

/* ================================================================
 * CLI$ status returns (from cli$present / cli$get_value)
 *
 * SEVERITY IS LOAD-BEARING and matches documented VMS behavior (DCL
 * Dictionary, CLI$PRESENT / CLI$GET_VALUE): callers test the returned
 * status with $VMS_STATUS_SUCCESS (see e.g. MMK's cli_get_value wrapper,
 * tests/corpus/tier3-mmk/mmk.c) to decide whether a value can be fetched.
 * Therefore CLI$_PRESENT / CLI$_DEFAULTED / CLI$_COMMA / CLI$_CONCAT carry
 * SUCCESS severity (low bit set) while CLI$_ABSENT / CLI$_NEGATED carry
 * WARNING severity (low bit clear) so that $VMS_STATUS_SUCCESS is FALSE for
 * them -- an absent or negated qualifier has no value to get.
 *
 * PROVENANCE: the exact 32-bit values are OVMX-assigned (the VSI $CLIDEF
 * numeric assignments are not published in a fetchable public source this
 * pass -- flagged, to be pinned to the oracle later). Only the SUCCESS/
 * WARNING severity bit is grounded in documented behavior. NOTE: this fixes
 * a latent bug -- CLI$_ABSENT (0x0003A031) and CLI$_NEGATED (0x0003A039)
 * previously carried SUCCESS severity, which would make MMK fetch a value
 * from an absent qualifier.
 * ================================================================ */

#define CLI$_PRESENT        0x0003A019  /* Entity present (success)          */
#define CLI$_ABSENT         0x0003A030  /* Entity absent (warning: no value) */
#define CLI$_NEGATED        0x0003A038  /* Entity negated /NOxxx (warning)   */
#define CLI$_DEFAULTED      0x0003A041  /* Value came from CLD default (succ)*/
#define CLI$_COMMA          0x0003A049  /* Value followed by ',' (success)   */
#define CLI$_CONCAT         0x0003A051  /* Value followed by '+' (success)   */

/* ================================================================
 * Spawn flags for lib$spawn
 * ================================================================ */

#define CLI$M_NOWAIT        0x01    /* Don't wait for subprocess */
#define CLI$M_NOCLISYM      0x02    /* Don't pass CLI symbols */
#define CLI$M_NOLOGNAM      0x04    /* Don't pass logical names */
#define CLI$M_NOKEYPAD      0x08    /* Don't pass keypad state */
#define CLI$M_NOTIFY        0x10    /* Notify on completion */
#define CLI$M_NOCONTROL     0x20    /* Don't give subprocess control */
#define CLI$M_TRUSTED       0x40    /* Trusted subprocess */

/* ================================================================
 * DEFERRED (vms-f16): LIB$M_CLI_CTRLT (and LIB$M_CLI_CTRLY)
 *
 * The Ctrl/T mask for LIB$DISABLE_CTRL / LIB$ENABLE_CTRL, used by
 * tests/corpus/tier1-examples/lib_ctrl.c.  On OpenVMS this symbol is
 * defined only in the DEC C header <libclidef.h> (VSI source); it is
 * NOT present in the MACRO-32 definition libraries -- the 2026-08-13
 * oracle confirmed STARLET.MLB has no LIB$M_CLI_CTRLT and neither
 * $LIBCLIDEF nor $LIBDEF emits it.  The clean-room oracle path used for
 * every other vms-f16 constant (assemble the public macro, read the GSD
 * with ANALYZE/OBJECT) therefore cannot observe its value, and copying
 * the #define out of the VSI C header is not permitted under Rule 8.
 * No public byte-level value was found this pass.  Rather than invent a
 * value (empirical-not-gate), it is left DEFINED NOWHERE and deferred to
 * a focused pin once a permitted source is available.
 * ================================================================ */

#ifdef __cplusplus
}
#endif

#endif /* __LIBCLIDEF_H */
