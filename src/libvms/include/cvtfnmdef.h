/*
 * CVTFNMDEF.H - VMS Convert Filename Format Definitions
 *
 * OpenVMX compatibility layer - Defines the CVTFNM$C_ constants used
 * with sys$cvt_filename to convert filenames between ACP/QIO format
 * and RMS format.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

#ifndef __CVTFNMDEF_H
#define __CVTFNMDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * CVTFNM$C_ — Filename conversion direction codes
 *
 * Passed as the "function" argument to sys$cvt_filename.
 * ================================================================ */

#define CVTFNM$C_ACPQIO_TO_RMS  1   /* Convert ACP/QIO format to RMS format */
#define CVTFNM$C_RMS_TO_ACPQIO  2   /* Convert RMS format to ACP/QIO format */

/* ================================================================
 * CVTFNM$M_ — Output flags returned by sys$cvt_filename
 * ================================================================ */

#define CVTFNM$M_WILDCARD       0x00000001  /* Filename contains wildcards */
#define CVTFNM$M_TRUNCATED      0x00000002  /* Filename was truncated */

#ifdef __cplusplus
}
#endif

#endif /* __CVTFNMDEF_H */
