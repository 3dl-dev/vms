/*
 * PRCDEF.H - VMS Process Definition Constants
 *
 * OpenVMX compatibility layer - Defines the PRC$M_ flag bits
 * and constants used with process-related system services,
 * particularly SYS$CREPRC (create process).
 *
 * Also defines JPI$_ item codes for SYS$GETJPI and SYI$_ item
 * codes for SYS$GETSYI, since these are frequently used together
 * with process definitions.
 *
 * Reference: OpenVMS System Services Reference Manual (SYS$CREPRC)
 *            OpenVMS Programming Concepts Manual, Chapter 3
 */

#ifndef __PRCDEF_H
#define __PRCDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Process creation flag bits (stsflg parameter to SYS$CREPRC)
 *
 * These bits control the characteristics of a newly created
 * process.  They are specified in the stsflg argument.
 * ================================================================ */

#define PRC$M_DETACH        0x0001  /* Bit 0:  Create a detached process */
#define PRC$M_NOACNT        0x0002  /* Bit 1:  No accounting */
#define PRC$M_BATCH         0x0004  /* Bit 2:  Batch process */
#define PRC$M_HIBER         0x0008  /* Bit 3:  Start in hibernation */
#define PRC$M_LOGINOUT      0x0010  /* Bit 4:  Image is LOGINOUT */
#define PRC$M_NETWRK        0x0020  /* Bit 5:  Network process */
#define PRC$M_PSWAPM        0x0040  /* Bit 6:  Process swap mode (enable/disable) */
#define PRC$M_INTER         0x0080  /* Bit 7:  Interactive process */
#define PRC$M_NOPASSWORD    0x0100  /* Bit 8:  No password required */
#define PRC$M_IMGDMP        0x0200  /* Bit 9:  Image dump on error */
#define PRC$M_NOUAF         0x0400  /* Bit 10: Do not consult UAF */
#define PRC$M_SUBSYSTEM     0x0800  /* Bit 11: Subsystem process */
#define PRC$M_NOCLISYM      0x1000  /* Bit 12: No CLI symbol table */
#define PRC$M_SSRWAIT       0x2000  /* Bit 13: Wait for resource */
#define PRC$M_SSFEXCU       0x4000  /* Bit 14: Force exit on unhandled condition */
#define PRC$M_HOME_RAD      0x8000  /* Bit 15: Use home RAD */

/* Legacy aliases */
#define PRC$M_LOGIN         PRC$M_LOGINOUT
#define PRC$M_NETWORK       PRC$M_NETWRK

/* ================================================================
 * Process status flag bit positions
 * ================================================================ */

#define PRC$V_DETACH        0
#define PRC$V_NOACNT        1
#define PRC$V_BATCH         2
#define PRC$V_HIBER         3
#define PRC$V_LOGINOUT      4
#define PRC$V_NETWRK        5
#define PRC$V_PSWAPM        6
#define PRC$V_INTER         7
#define PRC$V_NOPASSWORD    8
#define PRC$V_IMGDMP        9
#define PRC$V_NOUAF         10
#define PRC$V_SUBSYSTEM     11
#define PRC$V_NOCLISYM      12
#define PRC$V_SSRWAIT       13
#define PRC$V_SSFEXCU       14
#define PRC$V_HOME_RAD      15

/* ================================================================
 * Process scheduling classes
 * ================================================================ */

#define PRC$K_NORMAL        0   /* Normal scheduling */
#define PRC$K_REALTIME      1   /* Real-time scheduling */

/* ================================================================
 * Process state constants
 *
 * These describe the current execution state of a process,
 * as reported by $GETJPI.
 * ================================================================ */

#define PRC$K_STATE_CEF     1   /* Common event flag wait */
#define PRC$K_STATE_COM     2   /* Computable */
#define PRC$K_STATE_COMO    3   /* Computable, outswapped */
#define PRC$K_STATE_CUR     4   /* Current (executing) */
#define PRC$K_STATE_FPG     5   /* Free page wait */
#define PRC$K_STATE_HIB     6   /* Hibernating */
#define PRC$K_STATE_HIBO    7   /* Hibernating, outswapped */
#define PRC$K_STATE_LEF     8   /* Local event flag wait */
#define PRC$K_STATE_LEFO    9   /* Local event flag wait, outswapped */
#define PRC$K_STATE_MWAIT   10  /* Mutex/resource wait */
#define PRC$K_STATE_PFW     11  /* Page fault wait */
#define PRC$K_STATE_SUSP    12  /* Suspended */
#define PRC$K_STATE_SUSPO   13  /* Suspended, outswapped */
#define PRC$K_STATE_COLPG   14  /* Collided page wait */

/* ================================================================
 * Process base priority limits
 * ================================================================ */

#define PRC$K_MIN_PRIO      0   /* Minimum priority */
#define PRC$K_MAX_PRIO      31  /* Maximum priority */
#define PRC$K_RT_MIN_PRIO   16  /* Minimum real-time priority */

/* ================================================================
 * UIC (User Identification Code) structure
 *
 * A UIC is a 32-bit value consisting of a group number (upper word)
 * and a member number (lower word).
 * ================================================================ */

struct _uic {
    uint16_t  uic$w_mem;     /* Member number */
    uint16_t  uic$w_grp;     /* Group number */
};

typedef struct _uic UIC;

/* Macros for UIC manipulation */
#define PRC$UIC(grp, mem)    ((uint32_t)(((grp) << 16) | ((mem) & 0xFFFF)))
#define PRC$UIC_GRP(uic)     (((uic) >> 16) & 0xFFFF)
#define PRC$UIC_MEM(uic)     ((uic) & 0xFFFF)

/* ================================================================
 * JPI$_ item codes for SYS$GETJPI
 *
 * These item codes are used in item lists passed to SYS$GETJPI
 * to request specific pieces of process information.
 * ================================================================ */

#define JPI$_PRCNAM         0x0100  /* Process name (string) */
#define JPI$_PID            0x0101  /* Process ID (longword) */
#define JPI$_MASTER_PID     0x0102  /* Master PID (longword) */
#define JPI$_OWNER          0x0103  /* Owner PID (longword) */
#define JPI$_UIC            0x0104  /* UIC (longword) */
#define JPI$_USERNAME       0x0105  /* Username (string, 12 chars) */
#define JPI$_ACCOUNT        0x0106  /* Account name (string, 8 chars) */
#define JPI$_GRP            0x0107  /* UIC group (word) */
#define JPI$_MEM            0x0108  /* UIC member (word) */
#define JPI$_STATE          0x0109  /* Process state (longword) */
#define JPI$_PRI            0x010A  /* Current priority (longword) */
#define JPI$_TERMINAL       0x010B  /* Terminal name (string) */
#define JPI$_IMAGNAME       0x010C  /* Image name (string) */
#define JPI$_CPUTIM         0x010D  /* CPU time in 10ms units (longword) */
#define JPI$_BUFIO          0x010E  /* Buffered I/O count (longword) */
#define JPI$_DIRIO          0x010F  /* Direct I/O count (longword) */
#define JPI$_PAGEFLTS       0x0110  /* Page fault count (longword) */
#define JPI$_PPGCNT         0x0111  /* Process page count (longword) */
#define JPI$_VIRTPEAK       0x0112  /* Peak virtual size (longword) */
#define JPI$_WSPEAK         0x0113  /* Peak working set (longword) */
#define JPI$_WSSIZE         0x0114  /* Working set size (longword) */
#define JPI$_LOGINTIM       0x0115  /* Login time (quadword) */
#define JPI$_MODE           0x0116  /* Process mode (longword) */
#define JPI$_CURPRIV        0x0117  /* Current privileges (quadword) */
#define JPI$_PROCPRIV       0x0118  /* Process privileges (quadword) */
#define JPI$_RIGHTS_SIZE    0x0119  /* Rights list size (longword) */
#define JPI$_RIGHTSLIST     0x011A  /* Rights list (array) */
#define JPI$_DFPROT         0x011B  /* Default protection (word) */
#define JPI$_DFDEV          0x011C  /* Default device (string) */
#define JPI$_DFDIR          0x011D  /* Default directory (string) */
#define JPI$_PRIB           0x011E  /* Base priority (longword) */
#define JPI$_APTCNT         0x011F  /* Active page table count (longword) */
#define JPI$_ASTLM          0x0120  /* AST limit (longword) */
#define JPI$_BIOLM          0x0121  /* Buffered I/O limit (longword) */
#define JPI$_DIOLM          0x0122  /* Direct I/O limit (longword) */
#define JPI$_ENQLM          0x0123  /* Enqueue limit (longword) */
#define JPI$_FILLM          0x0124  /* Open file limit (longword) */
#define JPI$_PGFLQUOTA      0x0125  /* Page file quota (longword) */
#define JPI$_PRCLM          0x0126  /* Subprocess limit (longword) */
#define JPI$_TQLM           0x0127  /* Timer queue limit (longword) */
#define JPI$_WSQUOTA        0x0128  /* Working set quota (longword) */
#define JPI$_WSEXTENT       0x0129  /* Working set extent (longword) */
#define JPI$_CLINAME        0x012A  /* CLI name (string) */
#define JPI$_TABLENAME      0x012B  /* CLI table name (string) */
#define JPI$_JOBTYPE        0x012C  /* Job type (longword) */

/* ================================================================
 * SYI$_ item codes for SYS$GETSYI
 *
 * These item codes are used in item lists passed to SYS$GETSYI
 * to request system-level information.
 * ================================================================ */

#define SYI$_NODENAME       0x0200  /* Node name (string) */
#define SYI$_BOOTTIME       0x0201  /* Boot time (quadword) */
#define SYI$_VERSION        0x0202  /* VMS version string */
#define SYI$_SID            0x0203  /* System ID (longword) */
#define SYI$_HW_NAME        0x0204  /* Hardware name (string) */
#define SYI$_AVAILCPU_CNT   0x0205  /* Available CPU count (longword) */
#define SYI$_ACTIVECPU_CNT  0x0206  /* Active CPU count (longword) */
#define SYI$_MEMSIZE        0x0207  /* Physical memory size in pages (longword) */
#define SYI$_PAGEFILE_FREE  0x0208  /* Free pagefile pages (longword) */
#define SYI$_SWAPFILE_FREE  0x0209  /* Free swapfile pages (longword) */
#define SYI$_ARCH_TYPE      0x020A  /* Architecture type (longword) */
#define SYI$_ARCH_NAME      0x020B  /* Architecture name (string) */
#define SYI$_HW_MODEL       0x020C  /* Hardware model (longword) */
#define SYI$_CLUSTER_MEMBER 0x020D  /* Cluster member flag (longword) */
#define SYI$_CLUSTER_NODES  0x020E  /* Number of cluster nodes (longword) */

#ifdef __cplusplus
}
#endif

#endif /* __PRCDEF_H */
