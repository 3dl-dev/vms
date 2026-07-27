/*
 * FSCNDEF.H - VMS File Specification Scan (FSCN$) Item Codes
 *
 * OpenVMX compatibility layer - Defines the FSCN$_ item codes and
 * FSCN$M_ flag bits used with sys$filescan to break a file
 * specification string into its component fields (node, device,
 * directory, name, type, version, ...).
 *
 * PROVENANCE: item-code mechanism and semantics documented in the
 * public OpenVMS System Services Reference Manual ($FILESCAN). Exact
 * numeric values not confirmed against a fetchable public source this
 * session — sequential OVMX assignment matching the numbering style
 * already used elsewhere in this tree (e.g. dvidef.h), flagged in
 * vms-531 findings for operator sign-off.
 *
 * Reference: OpenVMS System Services Reference Manual ($FILESCAN)
 */

#ifndef __FSCNDEF_H
#define __FSCNDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * FSCN$_ item codes for SYS$FILESCAN item lists (ILE2 entries)
 * ================================================================ */

#define FSCN$_NODE         0x0001  /* Node name field */
#define FSCN$_NODE_ACS     0x0002  /* Node access control string field */
#define FSCN$_DEVICE       0x0003  /* Device name field */
#define FSCN$_ROOT         0x0004  /* Root directory field */
#define FSCN$_DIRECTORY    0x0005  /* Directory field */
#define FSCN$_NAME         0x0006  /* File name field */
#define FSCN$_TYPE         0x0007  /* File type field */
#define FSCN$_VERSION      0x0008  /* File version field */
#define FSCN$_FILESPEC     0x0009  /* Whole normalized file spec */

/* ================================================================
 * FSCN$M_ — bits set in the "flags" longword returned by SYS$FILESCAN,
 * one per field actually present in the input string
 * ================================================================ */

#define FSCN$M_NODE         0x00000001
#define FSCN$M_NODE_ACS     0x00000002
#define FSCN$M_DEVICE       0x00000004
#define FSCN$M_DIRECTORY    0x00000008
#define FSCN$M_NAME         0x00000010
#define FSCN$M_TYPE         0x00000020
#define FSCN$M_VERSION      0x00000040

#ifdef __cplusplus
}
#endif

#endif /* __FSCNDEF_H */
