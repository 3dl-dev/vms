/*
 * ILEDEF.H - VMS Item List Entry Structure Definitions
 *
 * OpenVMX compatibility layer - Defines the ILE3 and ILE2 item list
 * entry structures used with system services that accept item lists:
 *   SYS$GETJPIW  — Get job/process information
 *   SYS$GETSYIW  — Get system information
 *   SYS$GETDVIW  — Get device information
 *   SYS$GETUAI   — Get user authorization information
 *   SYS$SETUAI   — Set user authorization information
 *   SYS$SNDJBCW  — Send to job controller
 *   SYS$ACMW     — Authenticate user
 *   and others
 *
 * An item list is an array of ILE3 (or ILE2) structures terminated
 * by an entry with both ile3$w_length and ile3$w_code set to zero.
 *
 * ILE3 (Item List Entry, 3-field): the standard VMS item list entry
 * that includes a return-length address.  Used when the caller
 * needs to know the actual number of bytes returned.
 *
 * ILE2 (Item List Entry, 2-field): a simpler form without a
 * return-length pointer.  Used for output-only or fixed-size items
 * (e.g., SYS$FILESCAN FSCN$_ items).
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual, Chapter 8
 */

#ifndef __ILEDEF_H
#define __ILEDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * ILE3 — Standard item list entry (3 address fields)
 *
 * Layout (12 bytes on 32-bit VMS, 16 bytes on 64-bit with padding):
 *
 *   +0  ile3$w_length       WORD  — Buffer length (bytes)
 *   +2  ile3$w_code         WORD  — Item code (e.g. JPI$_USERNAME)
 *   +4  ile3$ps_bufaddr     LONG  — Address of buffer to receive data
 *   +8  ile3$ps_retlen_addr LONG  — Address of word to receive length
 *                                   (NULL if not needed)
 *  +12  (next entry or terminator)
 *
 * The terminator entry has both ile3$w_length and ile3$w_code = 0.
 * ================================================================ */

typedef struct _ile3 {
    uint16_t    ile3$w_length;          /* Buffer length in bytes */
    uint16_t    ile3$w_code;            /* Item code */
    void       *ile3$ps_bufaddr;        /* Buffer address */
    uint16_t   *ile3$ps_retlen_addr;    /* Return length address (or NULL) */
} ILE3;

/* ================================================================
 * ILE2 — Simple item list entry (2 address fields, no retlen)
 *
 * Used by services such as SYS$FILESCAN that return a list of
 * output descriptors rather than filling caller-supplied buffers.
 *
 *   +0  ile2$w_length   WORD  — Length of data (set by service)
 *   +2  ile2$w_code     WORD  — Item code
 *   +4  ile2$ps_bufaddr LONG  — Address of data (set by service)
 *   +8  (next entry or terminator)
 * ================================================================ */

typedef struct _ile2 {
    uint16_t    ile2$w_length;          /* Length of returned data */
    uint16_t    ile2$w_code;            /* Item code */
    void       *ile2$ps_bufaddr;        /* Buffer / data address */
} ILE2;

/* ================================================================
 * Terminator macro
 *
 * Convenience initializer for the list-terminating entry.
 * Usage:  ILE3 list[] = { { ... }, ILE3_TERMINATOR };
 * ================================================================ */

#define ILE3_TERMINATOR     { 0, 0, NULL, NULL }
#define ILE2_TERMINATOR     { 0, 0, NULL }

#ifdef __cplusplus
}
#endif

#endif /* __ILEDEF_H */
