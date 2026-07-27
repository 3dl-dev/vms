/*
 * ACLDEF.H - VMS Access Control List (ACL$) Object Class Codes
 *
 * OpenVMX compatibility layer - Defines the ACL$C_ object-class
 * constants used with sys$check_access to identify the type of
 * object an access check applies to.
 *
 * PROVENANCE: object-class mechanism documented in the public
 * OpenVMS Guide to System Security ($CHECK_ACCESS, ACL editor object
 * classes: FILE, DEVICE, QUEUE, VOLUME, ...). Exact numeric values
 * not confirmed against a fetchable public source this session —
 * sequential OVMX assignment, flagged in vms-531 findings.
 *
 * Reference: OpenVMS Guide to System Security
 *            OpenVMS System Services Reference Manual ($CHECK_ACCESS)
 */

#ifndef __ACLDEF_H
#define __ACLDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * ACL$C_ — object class codes for SYS$CHECK_ACCESS
 * ================================================================ */

#define ACL$C_FILE      1   /* File object */
#define ACL$C_DEVICE     2   /* Device object */
#define ACL$C_VOLUME     3   /* Volume object */
#define ACL$C_QUEUE      4   /* Queue object */

#ifdef __cplusplus
}
#endif

#endif /* __ACLDEF_H */
