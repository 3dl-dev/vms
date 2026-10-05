/*
 * LNMDEF.H - VMS Logical Name Definitions
 *
 * OpenVMX compatibility layer - Defines constants used with the
 * logical name system services: SYS$CRELNM, SYS$DELLNM, SYS$TRNLNM.
 *
 * Logical names are a central VMS abstraction that maps symbolic
 * names to equivalence strings, providing device independence and
 * a flexible configuration mechanism (similar to, but more powerful
 * than, Unix environment variables).
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual, Chapter 32
 */

#ifndef __LNMDEF_H
#define __LNMDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Item codes for SYS$CRELNM and SYS$TRNLNM item lists
 *
 * These codes are used in item list entries to specify what
 * information to provide or retrieve for a logical name.
 * ================================================================ */

#define LNM$_INDEX          1   /* Translation index (for multi-valued names) */
#define LNM$_STRING         2   /* Equivalence string */
#define LNM$_LENGTH         5   /* Length of equivalence string */
#define LNM$_ACMODE         6   /* Access mode */
#define LNM$_ATTRIBUTES     3   /* Logical name attributes */
#define LNM$_TABLE          4   /* Logical name table */
#define LNM$_MAX_INDEX      7   /* Maximum translation index */
#define LNM$_CHAIN          -1   /* Chain to next table (table attribute) */
#define LNM$_LNMB_ADDR     9   /* Address of LNM block (internal) */

/* ================================================================
 * Logical name attribute mask bits
 *
 * These bits describe properties of a logical name or
 * logical name table.  They are used with LNM$_ATTRIBUTES.
 * ================================================================ */

#define LNM$M_CONCEALED     0x100    /* Concealed device (RMS hides translation) */
#define LNM$M_TERMINAL      0x200    /* Terminal (do not translate further) */
#define LNM$M_CONFINE       0x2    /* Do not copy to subprocess */
#define LNM$M_NO_ALIAS      0x1    /* Do not allow outer-mode alias */
#define LNM$M_CRELOG        0x4    /* Created with CRELOG (compatibility) */
#define LNM$M_TABLE         0x8    /* Entry is a table name */
#define LNM$M_CREATE_IF     0x1000000    /* Create only if not existing */
#define LNM$M_CASE_BLIND    0x2000000    /* Case-blind name comparison */

/* Additional attribute bits */
#define LNM$M_INTERLOCKED   0x4000000   /* Interlocked for cluster use */
#define LNM$M_LOCAL_ACTION  0x8000000   /* Local action only */
#define LNM$M_CLUSTERWIDE   0x20000   /* Cluster-wide logical name */
#define LNM$M_EXISTS        0x400   /* Name exists (returned by TRNLNM) */
#define LNM$M_SHAREABLE     0x10000  /* Table is shareable */

/* Remaining V7.3 $LNMDEF names (rd vms-f811) */
#define LNM$V_NO_ALIAS         0
#define LNM$V_CONFINE          1
#define LNM$V_CRELOG           2
#define LNM$V_TABLE            3
#define LNM$V_CONCEALED        8
#define LNM$V_TERMINAL         9
#define LNM$V_EXISTS           10
#define LNM$V_SHAREABLE        16
#define LNM$V_CLUSTERWIDE      17
#define LNM$V_CREATE_IF        24
#define LNM$V_CASE_BLIND       25
#define LNM$V_INTERLOCKED      26
#define LNM$V_LOCAL_ACTION     27
#define LNM$_PARENT            8
#define LNM$_AGENT_ACMODE      10

/* ================================================================
 * Size and depth limits
 * ================================================================ */

#define LNM$C_NAMLENGTH     255     /* Maximum logical name length */
#define LNM$C_TABNAMLEN     31      /* Maximum table name length */
#define LNM$C_MAXDEPTH      10      /* Maximum iterative translation depth */
#define LNM$C_MAXVALLEN     255     /* Maximum equivalence value length */

/* ================================================================
 * Access modes used with LNM$_ACMODE
 *
 * These match the standard VMS processor access modes.
 * Logical names are segregated by access mode - a name
 * created in user mode is only visible at user mode.
 * ================================================================ */

#define LNM$C_KERNEL        0   /* Kernel mode */
#define LNM$C_EXEC          1   /* Executive mode */
#define LNM$C_SUPERVISOR    2   /* Supervisor mode */
#define LNM$C_USER          3   /* User mode */

/* PSL access mode aliases (commonly used in VMS code) */
#define PSL$C_KERNEL        0
#define PSL$C_EXEC          1
#define PSL$C_SUPER         2
#define PSL$C_USER          3

/* ================================================================
 * Predefined logical name table names
 *
 * These string constants define the standard VMS logical name
 * tables.  In actual VMS, these are referenced by descriptor;
 * here we provide the string constants for use with $DESCRIPTOR.
 * ================================================================ */

#define LNM$_PROCESS_TABLE      "LNM$PROCESS_TABLE"
#define LNM$_JOB_TABLE          "LNM$JOB"
#define LNM$_GROUP_TABLE        "LNM$GROUP"
#define LNM$_SYSTEM_TABLE       "LNM$SYSTEM_TABLE"
#define LNM$_PROCESS_DIRECTORY  "LNM$PROCESS_DIRECTORY"
#define LNM$_SYSTEM_DIRECTORY   "LNM$SYSTEM_DIRECTORY"
#define LNM$_FILE_DEV           "LNM$FILE_DEV"
#define LNM$_DCL_LOGICAL        "LNM$DCL_LOGICAL"

/* Shorthand aliases */
#define LNM$PROCESS_TABLE       LNM$_PROCESS_TABLE
#define LNM$JOB_TABLE           LNM$_JOB_TABLE
#define LNM$GROUP_TABLE         LNM$_GROUP_TABLE
#define LNM$SYSTEM_TABLE        LNM$_SYSTEM_TABLE
#define LNM$PROCESS_DIRECTORY   LNM$_PROCESS_DIRECTORY
#define LNM$SYSTEM_DIRECTORY    LNM$_SYSTEM_DIRECTORY
#define LNM$FILE_DEV            LNM$_FILE_DEV

/* ================================================================
 * Item list structure for logical name services
 *
 * Item lists are arrays of these structures, terminated by
 * a longword of zero.  Each entry specifies one piece of
 * information to set or retrieve.
 * ================================================================ */

struct item_list_3 {
    uint16_t  buflen;      /* Buffer length */
    uint16_t  item_code;   /* Item code (LNM$_xxx) */
    void     *bufaddr;     /* Buffer address */
    uint16_t *retlen;      /* Return length address (may be NULL) */
};

typedef struct item_list_3 ITEM_LIST_3;

/* Alternate name matching VMS documentation */
struct lnm_item_list {
    uint16_t  buflen;
    uint16_t  item_code;
    void     *bufaddr;
    uint16_t *retlen;
};

typedef struct lnm_item_list LNM_ITEM_LIST;

/* Terminator for item list (zero longword) */
#define ITEM_LIST_END       { 0, 0, NULL, NULL }
#define LNM$_ITEM_LIST_END  { 0, 0, NULL, NULL }

#ifdef __cplusplus
}
#endif

#endif /* __LNMDEF_H */
