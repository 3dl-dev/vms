/*
 * FIDDEF.H - VMS File Identifier Definitions
 *
 * OpenVMX compatibility layer - Defines the FIDDEF structure that
 * represents a VMS file identifier (FID).  The FID is a 6-byte
 * structure consisting of a file number, sequence number, and
 * relative volume number.
 *
 * Used with lib$fid_to_name and other file identification services.
 *
 * Reference: OpenVMS Record Management Services Reference Manual
 *            OpenVMS System Services Reference Manual
 */

#ifndef __FIDDEF_H
#define __FIDDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * FIDDEF — File Identifier structure
 *
 * A VMS file identifier uniquely identifies a file on a volume.
 * The well-known FID (1,1,0) refers to INDEXF.SYS on any ODS-2
 * or ODS-5 volume.
 *
 * Layout (6 bytes):
 *   fid$w_num   [0-1]  File number
 *   fid$w_seq   [2-3]  Sequence number
 *   fid$w_rvn   [4-5]  Relative volume number (0 = current volume)
 * ================================================================ */

struct _fiddef {
    uint16_t fid$w_num;     /* File number */
    uint16_t fid$w_seq;     /* Sequence number */
    uint16_t fid$w_rvn;     /* Relative volume number */
};

typedef struct _fiddef FIDDEF;

/* ================================================================
 * FID$K_ — Special file number constants
 * ================================================================ */

#define FID$K_LENGTH        6   /* Size of a file identifier (bytes) */

#ifdef __cplusplus
}
#endif

#endif /* __FIDDEF_H */
