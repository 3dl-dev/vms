/*
 * PRVDEF.H - VMS Privilege Definitions
 *
 * OpenVMX compatibility layer - Defines the PRV$M_ privilege mask
 * bits used with SYS$SETPRV and returned by SYS$GETJPI (JPI$_CURPRIV).
 *
 * Each privilege is a single bit in a 64-bit quadword mask.
 * Bit positions match real OpenVMS Alpha values.
 *
 * Reference: OpenVMS Guide to System Security
 *            OpenVMS System Services Reference Manual (SYS$SETPRV)
 */

#ifndef __PRVDEF_H
#define __PRVDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Privilege mask bits (quadword - 64 bits)
 *
 * Bit 0 is the least significant bit of the first longword.
 * ================================================================ */

#define PRV$M_CMKRNL        ((uint64_t)1 <<  0)  /* Change mode to kernel */
#define PRV$M_CMEXEC        ((uint64_t)1 <<  1)  /* Change mode to executive */
#define PRV$M_SYSNAM        ((uint64_t)1 <<  2)  /* Create system logical names */
#define PRV$M_GRPNAM        ((uint64_t)1 <<  3)  /* Create group logical names */
#define PRV$M_ALLSPOOL      ((uint64_t)1 <<  4)  /* Allocate spooled device */
#define PRV$M_DETACH        ((uint64_t)1 <<  5)  /* Create detached process */
#define PRV$M_DIAGNOSE      ((uint64_t)1 <<  6)  /* Diagnose devices */
#define PRV$M_LOG_IO        ((uint64_t)1 <<  7)  /* Logical I/O */
#define PRV$M_GROUP         ((uint64_t)1 <<  8)  /* Group privilege */
#define PRV$M_ACNT          ((uint64_t)1 <<  9)  /* Suppress accounting */
#define PRV$M_PRMCEB        ((uint64_t)1 << 10)  /* Permanent common event flag cluster */
#define PRV$M_PRMMBX        ((uint64_t)1 << 11)  /* Create permanent mailbox */
#define PRV$M_PSWAPM        ((uint64_t)1 << 12)  /* Change process swap mode */
#define PRV$M_SETPRI        ((uint64_t)1 << 13)  /* Set any priority */
#define PRV$M_SETPRV        ((uint64_t)1 << 14)  /* Set any privilege */
#define PRV$M_TMPMBX        ((uint64_t)1 << 15)  /* Create temporary mailbox */
#define PRV$M_WORLD         ((uint64_t)1 << 16)  /* World privilege */
#define PRV$M_MOUNT         ((uint64_t)1 << 17)  /* Mount volumes */
#define PRV$M_OPER          ((uint64_t)1 << 18)  /* Operator privilege */
#define PRV$M_EXQUOTA       ((uint64_t)1 << 19)  /* Exceed quotas */
#define PRV$M_NETMBX        ((uint64_t)1 << 20)  /* Create network device */
#define PRV$M_VOLPRO        ((uint64_t)1 << 21)  /* Override volume protection */
#define PRV$M_PHY_IO        ((uint64_t)1 << 22)  /* Physical I/O */
#define PRV$M_BUGCHK        ((uint64_t)1 << 23)  /* Make bugcheck error log entries */
#define PRV$M_PRMGBL        ((uint64_t)1 << 24)  /* Create permanent global sections */
#define PRV$M_SYSGBL        ((uint64_t)1 << 25)  /* Create system global sections */
#define PRV$M_PFNMAP        ((uint64_t)1 << 26)  /* Map to specific physical pages */
#define PRV$M_SHMEM         ((uint64_t)1 << 27)  /* Create shared memory structures */
#define PRV$M_SYSPRV        ((uint64_t)1 << 28)  /* System privilege */
#define PRV$M_BYPASS        ((uint64_t)1 << 29)  /* Bypass all object access controls */
#define PRV$M_SYSLCK        ((uint64_t)1 << 30)  /* Lock system resources */
#define PRV$M_SHARE         ((uint64_t)1 << 31)  /* Assign shared access to devices */
#define PRV$M_UPGRADE       ((uint64_t)1 << 32)  /* Upgrade object secrecy */
#define PRV$M_DOWNGRADE     ((uint64_t)1 << 33)  /* Downgrade object secrecy */
#define PRV$M_GRPPRV        ((uint64_t)1 << 34)  /* Group privilege (access) */
#define PRV$M_READALL       ((uint64_t)1 << 35)  /* Read access to all objects */
#define PRV$M_SECURITY      ((uint64_t)1 << 38)  /* Security administrator */
#define PRV$M_IMPERSONATE   ((uint64_t)1 << 37)  /* Impersonate other users */
#define PRV$M_ALTPRI        ((uint64_t)1 << 36)  /* Alter process priority */

/* Convenience: all privileges */
#define PRV$M_ALL           (~(uint64_t)0)

/* Named privilege sets commonly used in VMS */
#define PRV$M_NOACCESS      0                     /* No special privileges */

/* ================================================================
 * Privilege bit positions (PRV$V_ constants)
 * ================================================================ */

#define PRV$V_CMKRNL         0
#define PRV$V_CMEXEC         1
#define PRV$V_SYSNAM         2
#define PRV$V_GRPNAM         3
#define PRV$V_ALLSPOOL       4
#define PRV$V_DETACH         5
#define PRV$V_DIAGNOSE       6
#define PRV$V_LOG_IO         7
#define PRV$V_GROUP          8
#define PRV$V_ACNT           9
#define PRV$V_PRMCEB        10
#define PRV$V_PRMMBX        11
#define PRV$V_PSWAPM        12
#define PRV$V_SETPRI        13
#define PRV$V_SETPRV        14
#define PRV$V_TMPMBX        15
#define PRV$V_WORLD         16
#define PRV$V_MOUNT         17
#define PRV$V_OPER          18
#define PRV$V_EXQUOTA       19
#define PRV$V_NETMBX        20
#define PRV$V_VOLPRO        21
#define PRV$V_PHY_IO        22
#define PRV$V_BUGCHK        23
#define PRV$V_PRMGBL        24
#define PRV$V_SYSGBL        25
#define PRV$V_PFNMAP        26
#define PRV$V_SHMEM         27
#define PRV$V_SYSPRV        28
#define PRV$V_BYPASS        29
#define PRV$V_SYSLCK        30
#define PRV$V_SHARE         31
#define PRV$V_UPGRADE       32
#define PRV$V_DOWNGRADE     33
#define PRV$V_GRPPRV        34
#define PRV$V_READALL       35
#define PRV$V_ALTPRI        36
#define PRV$V_IMPERSONATE   37
#define PRV$V_SECURITY      38

#ifdef __cplusplus
}
#endif

#endif /* __PRVDEF_H */
