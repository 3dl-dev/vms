/*
 * CLIDEF.H - VMS Command Language Interpreter (CLI$) Constants
 *
 * OpenVMX compatibility layer - Defines the CLI$M_ flag bits used with
 * lib$spawn's "flags" argument to control subprocess creation (e.g.
 * spawn asynchronously and/or request completion notification).
 *
 * Only the bits exercised by the current corpus (tests/corpus) are
 * defined here — see docs/conformance-gap-report.md §3.1. Other CLI$
 * users may need additional symbols; not added speculatively per
 * project no-gold-plating rule.
 *
 * PROVENANCE: the CLI$M_NOWAIT/CLI$M_NOTIFY flags are documented in
 * the public OpenVMS RTL Library (LIB$) Routines Reference Manual
 * (LIB$SPAWN). Exact bit values not confirmed against a fetchable
 * public source this session — sequential OVMX assignment, flagged
 * in vms-531 findings.
 *
 * Reference: OpenVMS RTL Library (LIB$) Routines Reference Manual
 *            (LIB$SPAWN)
 */

#ifndef __CLIDEF_H
#define __CLIDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * CLI$M_ — LIB$SPAWN "flags" argument bit masks
 * ================================================================ */

#define CLI$M_NOWAIT     0x00000001  /* Spawn asynchronously (don't wait) */
#define CLI$M_NOTIFY     0x00000002  /* Notify via AST/event flag on completion */

#ifdef __cplusplus
}
#endif

#endif /* __CLIDEF_H */
