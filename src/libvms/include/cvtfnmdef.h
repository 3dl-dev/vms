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

#define CVTFNM$C_ACPQIO_TO_RMS  2   /* Convert ACP/QIO format to RMS format */
#define CVTFNM$C_RMS_TO_ACPQIO  1   /* Convert RMS format to ACP/QIO format */

#ifdef __cplusplus
}
#endif

#endif /* __CVTFNMDEF_H */
