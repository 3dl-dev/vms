/*
 * SECSRVMSGDEF.H - VMS Security Server (SECSRV$) Status Codes
 *
 * OpenVMX compatibility layer - Defines the SECSRV$_ condition
 * values returned by the security-server-backed system services
 * (sys$add_proxy, sys$delete_proxy, sys$show_intrusion, ...).
 *
 * PROVENANCE: the existence of these specific condition names is
 * documented in the public OpenVMS System Services Reference Manual
 * (proxy and intrusion-detection service descriptions), but the
 * SECSRV facility number and per-condition message numbers could NOT
 * be confirmed against a fetchable public source this session. Values
 * below are encoded using the standard VMS condition-value layout
 * (facility <27:16>, message number <15:3>, severity <2:0> — see
 * stsdef.h) with an OVMX-placeholder facility number (23, chosen to
 * not collide with the facility numbers already assigned elsewhere in
 * this tree: RMS$_ = 1, LIB$_ = 21). Flagged in vms-531 findings for
 * operator sign-off.
 *
 * Reference: OpenVMS System Services Reference Manual ($ADD_PROXY,
 *            $DELETE_PROXY, $SHOW_INTRUSION)
 */

#ifndef __SECSRVMSGDEF_H
#define __SECSRVMSGDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * SECSRV$_ — Security Server condition values
 * facility 23, severity ERROR(2), sequential message numbers
 * ================================================================ */

#define SECSRV$_DUPLICATEUSER      1507338   /* facility 23, msgnum 1, severity ERROR(2) */
#define SECSRV$_INVALIDDELETE      1507346   /* facility 23, msgnum 2, severity ERROR(2) */
#define SECSRV$_NOSUCHINTRUDER     1507354   /* facility 23, msgnum 3, severity ERROR(2) */
#define SECSRV$_CIADBEMPTY         1507362   /* facility 23, msgnum 4, severity ERROR(2) */

#ifdef __cplusplus
}
#endif

#endif /* __SECSRVMSGDEF_H */
