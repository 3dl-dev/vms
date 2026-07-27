/*
 * PRDEF.H - VMS Processor Register (PR$) Definitions
 *
 * OpenVMX compatibility layer - Defines the PR$M_ bit masks used to
 * decode fields of the Alpha/IA64 processor-status register (PS), as
 * returned by the __PAL_RD_PS() PALcode builtin and consumed by
 * __PAL_PROBER/__PAL_PROBEW to determine the caller's previous
 * access mode before probing user-supplied addresses from a more
 * privileged mode (e.g. inside a sys$cmkrnl/sys$cmexec routine).
 *
 * PROVENANCE: the PS register's "previous mode" concept is documented
 * in the public Alpha Architecture Reference Manual / OpenVMS
 * Calling Standard, but the exact bit position within PS could NOT
 * be confirmed against a fetchable public source this session. This
 * is inherently Alpha/IA64 PALcode-specific and only reachable in
 * corpus code compiled for those architectures (guarded by
 * "#ifdef __VAX ... #error" — i.e. the code assumes Alpha/IA64/x86_64
 * only); OVMX's own placeholder bit position is used below so the
 * declaration is present and the surrounding code compiles.
 * Flagged in vms-531 findings for operator sign-off.
 *
 * Reference: Alpha Architecture Reference Manual, Processor Status
 *            (PS) register; OpenVMS Calling Standard, access modes
 */

#ifndef __PRDEF_H
#define __PRDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * PR$M_ — Processor Status (PS) register field masks
 * ================================================================ */

#define PR$M_PS_PRVMOD    0x00000003  /* Previous access mode field (bits 0-1) */

#ifdef __cplusplus
}
#endif

#endif /* __PRDEF_H */
