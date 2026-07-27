/*
 * FIBDEF.H - VMS RMS File Information Block (FIB) Structure
 *
 * OpenVMX compatibility layer - Defines FIBDEF, the File Information
 * Block used with the IO$_ACCESS/IO$_DEACCESS QIO functions (and
 * other ACP-QIO file operations) to identify a file by its File ID
 * (FID) rather than by name.
 *
 * PROVENANCE: the FIB is a long-standing, publicly documented VAX/VMS
 * ACP-QIO structure (OpenVMS I/O User's Reference Manual, RMS
 * internals appendix). Only the two fields actually exercised by the
 * current corpus (fib$w_fid, fib$l_acctl) are populated with field
 * names/positions this agent has reasonable confidence in from that
 * public documentation; the byte-exact offsets of the full FIB are
 * NOT independently re-verified against a fetchable public source
 * this session, so this struct's shape beyond those two fields is
 * OVMX's own minimal representation (clean-room, CLAUDE.md rule 8).
 * Flagged in vms-531 findings for operator sign-off before being
 * relied on for wire-level ACP-QIO interop.
 *
 * Reference: OpenVMS I/O User's Reference Manual, RMS Internals /
 *            File Information Block (FIB)
 */

#ifndef __FIBDEF_H
#define __FIBDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * FIBDEF — File Information Block
 * ================================================================ */

typedef struct _fibdef {
    unsigned short int fib$w_did[3];    /* Directory file ID (num, seq, rvn) */
    unsigned short int fib$w_fid[3];    /* File ID (num, seq, rvn) */
    unsigned int        fib$l_acctl;    /* Access control flags */
} FIBDEF;

#ifdef __cplusplus
}
#endif

#endif /* __FIBDEF_H */
