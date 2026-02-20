/*
 * DVIDEF.H - VMS Device/Volume Information Item Code Definitions
 *
 * OpenVMX compatibility layer - Defines the DVI$_ item codes used
 * with sys$getdvi and sys$getdviw (Get Device/Volume Information)
 * and lib$getdvi.
 *
 * Item codes are passed in an item list (ILE3 or itm3 structs) to
 * request specific device or volume attributes.  The system fills
 * the caller's buffer with the requested information.
 *
 * Data type key (in comments below):
 *   L  = Longword (32-bit unsigned integer)
 *   W  = Word (16-bit unsigned integer)
 *   T  = String (counted or fixed-length ASCII)
 *   Q  = Quadword (64-bit value)
 *
 * Reference: OpenVMS System Services Reference Manual (SYS$GETDVI)
 *            OpenVMS I/O User's Reference Manual, Device Information
 */

#ifndef __DVIDEF_H
#define __DVIDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * DVI$_ item codes for SYS$GETDVI / SYS$GETDVIW / LIB$GETDVI
 * ================================================================ */

/* Device identification */
#define DVI$_DEVNAM             0x0001  /* Device name string (T, 64 bytes max) */
#define DVI$_UNIT               0x0002  /* Unit number (L) */
#define DVI$_DEVTYPE            0x0003  /* Device type code (L) */
#define DVI$_DEVCLASS           0x0004  /* Device class code (L) — see dcdef.h */
#define DVI$_DEVCHAR            0x0005  /* Device characteristics flags (L) */
#define DVI$_DEVCHAR2           0x0006  /* Extended device characteristics (L) */
#define DVI$_DEVDEPEND          0x0007  /* Device dependent info (L) */
#define DVI$_DEVDEPEND2         0x0008  /* Extended device dependent info (L) */
#define DVI$_FULLDEVNAM         0x0009  /* Full device name including node (T) */
#define DVI$_ALLDEVNAM          0x000A  /* All device names (T) */

/* Volume information (disk/tape) */
#define DVI$_VOLNAM             0x0010  /* Volume name (T, 12 bytes max) */
#define DVI$_MAXBLOCK           0x0011  /* Total blocks on volume (L) */
#define DVI$_FREEBLOCKS         0x0012  /* Free blocks remaining (L) */
#define DVI$_MAXFILES           0x0013  /* Maximum files on volume (L) */
#define DVI$_CLUSTER            0x0014  /* Cluster size in blocks (L) */
#define DVI$_VOLCOUNT           0x0015  /* Volume count (volume set) (L) */
#define DVI$_VOLNUMBER          0x0016  /* Relative volume number (L) */
#define DVI$_SERIALNUM          0x0017  /* Volume serial number (L) */
#define DVI$_LOGVOLNAM          0x0018  /* Logical volume name (T) */
#define DVI$_VOLSETMEM          0x0019  /* Volume is member of volume set (L, boolean) */
#define DVI$_SECTORS            0x001A  /* Sectors per track (L) */
#define DVI$_TRACKS             0x001B  /* Tracks per cylinder (L) */
#define DVI$_CYLINDERS          0x001C  /* Cylinders on volume (L) */
#define DVI$_BLOCKSIZE          0x001D  /* Block size in bytes (L) */

/* Mount / access status */
#define DVI$_MOUNTCNT           0x0020  /* Mount count (number of accessors) (L) */
#define DVI$_ACPTYPE            0x0021  /* ACP type (L) */
#define DVI$_REFCNT             0x0022  /* Reference count (L) */
#define DVI$_ERRCNT             0x0023  /* Error count (L) */
#define DVI$_OPCNT              0x0024  /* Operations count (L) */
#define DVI$_TT_PHYDEVNAM       0x0025  /* Terminal physical device name (T) */

/* Process ownership */
#define DVI$_PID                0x0030  /* PID of process that allocated device (L) */
#define DVI$_OWNUIC             0x0031  /* UIC of device owner (L) */

/* Locking */
#define DVI$_DEVLOCKNAM         0x0040  /* Device lock name (T) */
#define DVI$_LOCKID             0x0041  /* Lock ID for device (L) */

/* Multipath */
#define DVI$_AVAILABLE_PATH_COUNT 0x0050 /* Number of available paths (L) */
#define DVI$_PATH_COUNT         0x0051  /* Total path count (L) */

/* Media / label */
#define DVI$_LABEL_STATUS       0x0060  /* Label status (L) */
#define DVI$_MEDIA_NAME         0x0061  /* Media name string (T) */
#define DVI$_MEDIA_TYPE         0x0062  /* Media type (L) */
#define DVI$_MEDIA_ID           0x0063  /* Media ID (L) */

/* ================================================================
 * DVI$_ boolean item codes (return 1/0 in a longword)
 * ================================================================ */

#define DVI$_MNTVERNCPY         0x0070  /* Volume has been verified (L, boolean) */
#define DVI$_CONCEALED          0x0071  /* Device is concealed (L, boolean) */
#define DVI$_SERVED             0x0072  /* Device is served (L, boolean) */
#define DVI$_MSCP_SERVED        0x0073  /* Device is MSCP served (L, boolean) */
#define DVI$_AVAILABLE          0x0074  /* Device is available (L, boolean) */
#define DVI$_DUAL_PORT          0x0075  /* Device has dual porting (L, boolean) */
#define DVI$_FOR_ODS2           0x0076  /* Formatted for ODS-2 (L, boolean) */
#define DVI$_FOR_ODS5           0x0077  /* Formatted for ODS-5 (L, boolean) */

#ifdef __cplusplus
}
#endif

#endif /* __DVIDEF_H */
