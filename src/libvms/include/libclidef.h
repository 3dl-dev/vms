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
 * CLI callback status returns
 * ================================================================ */

#define CLI$_PRESENT        0x0003A019  /* Qualifier is present */
#define CLI$_ABSENT         0x0003A031  /* Qualifier is absent */
#define CLI$_NEGATED        0x0003A039  /* Qualifier is negated (/NOQUAL) */
#define CLI$_DEFAULTED      0x0003A041  /* Qualifier has default value */
#define CLI$_COMMA          0x0003A049  /* More values follow (comma-separated) */
#define CLI$_CONCAT         0x0003A051  /* More values follow (concatenated) */

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

#ifdef __cplusplus
}
#endif

#endif /* __LIBCLIDEF_H */
