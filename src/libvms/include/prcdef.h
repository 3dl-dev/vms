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

/* V7.3 $PRCDEF values (rd vms-f811; docs/oracle/vax73-starlet-defs/PRCDEF.txt) */
#define PRC$M_SSRWAIT              0x00000001
#define PRC$M_SSFEXCU              0x00000002
#define PRC$M_PSWAPM               0x00000004
#define PRC$M_NOACNT               0x00000008
#define PRC$M_BATCH                0x00000010
#define PRC$M_HIBER                0x00000020
#define PRC$M_NOUAF                0x00000040
#define PRC$M_NETWRK               0x00000080
#define PRC$M_DISAWS               0x00000100
#define PRC$M_DETACH               0x00000200
#define PRC$M_INTER                0x00000400
#define PRC$M_IMGDMP               0x00000800
#define PRC$M_CLISPEC              0x00001000
#define PRC$M_NOPASSWORD           0x00002000
#define PRC$M_DEBUG                0x00004000
#define PRC$M_DBGTRU               0x00008000
#define PRC$M_SUBSYSTEM            0x00010000
#define PRC$M_TCB                  0x00020000
#define PRC$M_NO_IMAGE_PRIVS       0x00040000
#define PRC$M_PERM_SUBSYSTEM       0x00080000
#define PRC$M_PARSE_EXTENDED       0x00100000
#define PRC$M_INHERIT_PERSONA      0x00200000
#define PRC$M_LOGIN                0x00000040

/*
 * [OVMX] creation flags V7.3 does not define. Bits 28..30, above every V7.3 bit,
 * so no oracle flag is aliased. Labelled [OVMX], not VMS values.
 *   PRC$M_LOGINOUT   the created image is LOGINOUT (session login path)
 *   PRC$M_NOCLISYM   no CLI symbol table
 *   PRC$M_HOME_RAD   use home RAD
 */
#define PRC$M_LOGINOUT             0x40000000
#define PRC$M_NOCLISYM             0x20000000
#define PRC$M_HOME_RAD             0x400000

/* Legacy alias (not in $PRCDEF) */
#define PRC$M_NETWORK              PRC$M_NETWRK

/* ================================================================
 * Process status flag bit positions (V7.3 $PRCDEF)
 * ================================================================ */

#define PRC$V_SSRWAIT              0
#define PRC$V_SSFEXCU              1
#define PRC$V_PSWAPM               2
#define PRC$V_NOACNT               3
#define PRC$V_BATCH                4
#define PRC$V_HIBER                5
#define PRC$V_NOUAF                6
#define PRC$V_NETWRK               7
#define PRC$V_DISAWS               8
#define PRC$V_DETACH               9
#define PRC$V_INTER                10
#define PRC$V_IMGDMP               11
#define PRC$V_CLISPEC              12
#define PRC$V_NOPASSWORD           13
#define PRC$V_DEBUG                14
#define PRC$V_DBGTRU               15
#define PRC$V_SUBSYSTEM            16
#define PRC$V_TCB                  17
#define PRC$V_NO_IMAGE_PRIVS       18
#define PRC$V_PERM_SUBSYSTEM       19
#define PRC$V_PARSE_EXTENDED       20
#define PRC$V_INHERIT_PERSONA      21
#define PRC$V_LOGIN                6
#define PRC$V_LOGINOUT             30  /* [OVMX] */
#define PRC$V_NOCLISYM             29  /* [OVMX] */
#define PRC$V_HOME_RAD             22  /* [OVMX] */

/* $CREPRC item-list codes (V7.3 $PRCDEF) */
#define PRC$_LISTEND               0
#define PRC$_PGFLCHAR              1
#define PRC$_PGFLINDEX             2
#define PRC$_INPUT_ATT             3
#define PRC$_OUTPUT_ATT            4
#define PRC$_ERROR_ATT             5
#define PRC$_CLASS                 6

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

#define JPI$_PRCNAM         0x031C  /* Process name (string) */
#define JPI$_PID            0x0319  /* Process ID (longword) */
#define JPI$_MASTER_PID     0x0325  /* Master PID (longword) */
#define JPI$_OWNER          0x0303  /* Owner PID (longword) */
#define JPI$_UIC            0x0304  /* UIC (longword) */
#define JPI$_NODENAME       809     /* Node name (string) -- oracle: VAX V7.3 JPIDEF (vms-619) */
#define JPI$_USERNAME       0x0202  /* Username (string, 12 chars) */
#define JPI$_ACCOUNT        0x0203  /* Account name (string, 8 chars) */
#define JPI$_GRP            0x0308  /* UIC group (word) */
#define JPI$_MEM            0x0307  /* UIC member (word) */
#define JPI$_STATE          0x0306  /* Process state (longword) */
#define JPI$_PRI            0x0302  /* Current priority (longword) */
#define JPI$_TERMINAL       0x031D  /* Terminal name (string) */
#define JPI$_IMAGNAME       0x0207  /* Image name (string) */
#define JPI$_CPUTIM         0x0407  /* CPU time in 10ms units (longword) */
#define JPI$_BUFIO          0x040C  /* Buffered I/O count (longword) */
#define JPI$_DIRIO          0x040B  /* Direct I/O count (longword) */
#define JPI$_PAGEFLTS       0x040A  /* Page fault count (longword) */
#define JPI$_PPGCNT         0x030D  /* Process page count (longword) */
#define JPI$_VIRTPEAK       0x0200  /* Peak virtual size (longword) */
#define JPI$_WSPEAK         0x0201  /* Peak working set (longword) */
#define JPI$_WSSIZE         0x0411  /* Working set size (longword) */
#define JPI$_LOGINTIM       0x0206  /* Login time (quadword) */
#define JPI$_MODE           0x0322  /* Process mode (longword) */
#define JPI$_CURPRIV        0x0400  /* Current privileges (quadword) */
#define JPI$_PROCPRIV       0x0204  /* Process privileges (quadword) */
#define JPI$_RIGHTS_SIZE    0x0331  /* Rights list size (longword) */
#define JPI$_RIGHTSLIST     0x0326  /* Rights list (array) */
#define JPI$_DFPROT         0x011B  /* Default protection (word) */
#define JPI$_DFDEV          0x011C  /* Default device (string) */
#define JPI$_DFDIR          0x011D  /* Default directory (string) */
#define JPI$_PRIB           0x0309  /* Base priority (longword) */
#define JPI$_APTCNT         0x030A  /* Active page table count (longword) */
#define JPI$_ASTLM          0x0409  /* AST limit (longword) */
#define JPI$_BIOLM          0x0310  /* Buffered I/O limit (longword) */
#define JPI$_DIOLM          0x0313  /* Direct I/O limit (longword) */
#define JPI$_ENQLM          0x0320  /* Enqueue limit (longword) */
#define JPI$_FILLM          0x040F  /* Open file limit (longword) */
#define JPI$_PGFLQUOTA      0x040E  /* Page file quota (longword) */
#define JPI$_PRCLM          0x0408  /* Subprocess limit (longword) */
#define JPI$_TQLM           0x0410  /* Timer queue limit (longword) */
#define JPI$_WSQUOTA        0x0402  /* Working set quota (longword) */
#define JPI$_WSEXTENT       0x0416  /* Working set extent (longword) */
#define JPI$_CLINAME        0x020A  /* CLI name (string) */
#define JPI$_TABLENAME      0x020B  /* CLI table name (string) */
#define JPI$_JOBTYPE        0x0323  /* Job type (longword) */
/* JPI$_MODE / JPI$_JOBTYPE values -- oracle-pinned, docs/oracle/alpha84-starlet-
 * defs/JPIDEF.txt ($EQU JPI$K_OTHER 0, NETWORK 1, BATCH 2, INTERACTIVE 3;
 * JPI$K_DETACHED 0, LOCAL 3). */
#define JPI$K_OTHER         0
#define JPI$K_NETWORK       1
#define JPI$K_BATCH         2
#define JPI$K_INTERACTIVE   3
#define JPI$K_DETACHED      0
#define JPI$K_LOCAL         3

/* ================================================================
 * SYI$_ item codes for SYS$GETSYI
 *
 * These item codes are used in item lists passed to SYS$GETSYI
 * to request system-level information.
 * ================================================================ */

#define SYI$_NODENAME       0x10D9  /* Node name (string) */
#define SYI$_BOOTTIME       0x10BF  /* Boot time (quadword) */
#define SYI$_VERSION        0x1000  /* VMS version string */
#define SYI$_SID            0x1001  /* System ID (longword) */
#define SYI$_HW_NAME        0x110A  /* Hardware name (string) */
#define SYI$_AVAILCPU_CNT   0x111D  /* Available CPU count (longword) */
#define SYI$_ACTIVECPU_CNT  0x111E  /* Active CPU count (longword) */
#define SYI$_MEMSIZE        0x116B  /* Physical memory size in pages (longword) */
#define SYI$_PAGE_SIZE      4452    /* Page size in bytes (oracle: VAX V7.3 SYIDEF, vms-619) */
#define SYI$_PAGEFILE_FREE  0x10F4  /* Free pagefile pages (longword) */
#define SYI$_SWAPFILE_FREE  0x10F5  /* Free swapfile pages (longword) */
#define SYI$_ARCH_TYPE      0x1165  /* Architecture type (longword) */
#define SYI$_ARCH_NAME      0x1166  /* Architecture name (string) */
#define SYI$_HW_MODEL       0x1109  /* Hardware model (longword) */
#define SYI$_CLUSTER_MEMBER 0x10CF  /* Cluster member flag (longword) */
#define SYI$_CLUSTER_NODES  0x10CA  /* Number of cluster nodes (longword) */
#define SYI$_SCSNODE        0x1067  /* Node's SCS system name (string); OVMX-private code; see vms-3ab */
#define SYI$_DEFPRI         4279    /* Default base priority (SYSGEN DEFPRI); STARLET dump */
#define SYI$_SCSSYSTEMID    0x1065  /* Node's cluster system ID (longword); OVMX-private code; see vms-3ab */
/* CPU-inventory item codes (vms-f16).  OVMX-private codes continuing the
 * scheme above: the 2026-08-13 oracle dump confirms OVMX's whole SYI$_
 * numbering is already private (SYI$_NODENAME is 0x0200 here vs 4313 on
 * real VMS V7.3), and SYI$_MAX_CPUS / *_CPU_BITMAP are not present in
 * VAX V7.3 $SYIDEF at all.  Labeled OVMX design choices, Rule 8. */
#define SYI$_MAX_CPUS           0x11B1  /* Maximum configurable CPU count (longword); OVMX-private code */
#define SYI$_ACTIVE_CPU_BITMAP  0x1274  /* Bitmap of active CPUs; OVMX-private code */
#define SYI$_AVAIL_CPU_BITMAP   0x1275  /* Bitmap of available CPUs; OVMX-private code */

#ifdef __cplusplus
}
#endif

#endif /* __PRCDEF_H */
