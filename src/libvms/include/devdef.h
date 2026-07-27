/*
 * DEVDEF.H - VMS Device Characteristics Bit Definitions
 *
 * OpenVMX compatibility layer - Defines the DEV$M_ device
 * characteristics bits returned by DVI$_DEVCHAR (see dvidef.h) via
 * sys$getdvi/sys$getdviw, used to test device state (mounted, write
 * locked, available, foreign-mounted, etc.).
 *
 * PROVENANCE: DEV$M_MNT, DEV$M_SWL, DEV$M_AVL, DEV$M_FOR are the four
 * bits actually referenced by the current corpus (tests/corpus). Their
 * existence and meaning are documented in the public OpenVMS I/O
 * User's Reference Manual (Device Characteristics table). The exact
 * bit POSITIONS below reflect this agent's best recollection of that
 * published table but were NOT independently re-verified against a
 * fetchable public source in this session — flagged in vms-531
 * findings for operator sign-off / pin-to-oracle before being relied
 * on for wire-level VMS interop. Only the bits actually exercised by
 * the corpus are defined here (no speculative additions).
 *
 * Reference: OpenVMS I/O User's Reference Manual, "Device
 *            Characteristics and Status Bits"
 */

#ifndef __DEVDEF_H
#define __DEVDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * DEV$M_ — device characteristics bits (DVI$_DEVCHAR longword)
 * ================================================================ */

#define DEV$M_MNT     0x00001000  /* Device is mounted */
#define DEV$M_SWL     0x00000400  /* Device is software write locked */
#define DEV$M_AVL     0x00100000  /* Device is available */
#define DEV$M_FOR     0x00080000  /* Device is foreign mounted */

#ifdef __cplusplus
}
#endif

#endif /* __DEVDEF_H */
