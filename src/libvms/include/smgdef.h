/*
 * SMGDEF.H - VMS Screen Management (SMG$) Status Codes and Key Constants
 *
 * OpenVMX compatibility layer - Defines the SMG$_ status code and
 * SMG$K_ terminal key-code constant used with the smg$read_keystroke
 * RTL routine for device-independent, no-echo terminal input.
 *
 * PROVENANCE: the existence of SMG$_EOF and SMG$K_TRM_DELETE is
 * documented in the public OpenVMS Screen Management (SMG$) Run-Time
 * Library Reference Manual. Exact numeric values not confirmed
 * against a fetchable public source this session. SMG$_EOF is
 * encoded using the standard VMS condition-value layout (facility
 * <27:16>, message number <15:3>, severity <2:0> — see stsdef.h)
 * with an OVMX-placeholder facility number (22, chosen to not
 * collide with RMS$_ = 1 or LIB$_ = 21), mirroring the severity
 * ERROR(2) already used for the analogous RMS$_EOF condition in
 * rmsdef.h. SMG$K_TRM_DELETE is a small OVMX-placeholder key code.
 * Flagged in vms-531 findings for operator sign-off.
 *
 * Reference: OpenVMS Screen Management (SMG$) Run-Time Library
 *            Reference Manual (SMG$READ_KEYSTROKE)
 */

#ifndef __SMGDEF_H
#define __SMGDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * SMG$_ — status codes
 * ================================================================ */

#define SMG$_EOF    1441802   /* facility 22, msgnum 1, severity ERROR(2): Ctrl-Z / end of input */

/* ================================================================
 * SMG$K_ — terminal key codes returned by SMG$READ_KEYSTROKE
 * ================================================================ */

#define SMG$K_TRM_DELETE    1   /* DELETE/Backspace key */

#ifdef __cplusplus
}
#endif

#endif /* __SMGDEF_H */
