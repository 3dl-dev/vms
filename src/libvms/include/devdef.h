/*
 * DEVDEF.H - VMS Device Characteristics Bit Definitions
 *
 * OpenVMX compatibility layer - Defines the DEV$M_ device
 * characteristics bits returned by DVI$_DEVCHAR (see dvidef.h) via
 * sys$getdvi/sys$getdviw, used to test device state (mounted, write
 * locked, available, foreign-mounted, etc.).
 *
 * PROVENANCE: V7.3 oracle dump (see below). The earlier revision of this file gave
 * MNT/SWL/AVL/FOR best-recollection positions that disagreed with the real ones.
 *
 * Reference: OpenVMS I/O User's Reference Manual, "Device
 *            Characteristics and Status Bits"
 */

#ifndef __DEVDEF_H
#define __DEVDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * DEV$M_ -- device characteristics (DVI$_DEVCHAR, then DVI$_DEVCHAR2), the
 * observed OpenVMS VAX V7.3 values (rd vms-f811; $EQU lines the lab node's own
 * STARLET.MLB prints for $DEVDEF: docs/oracle/vax73-starlet-defs/DEVDEF.txt).
 * The second group (CLU .. POOL_MBR) lives in DVI$_DEVCHAR2 and so reuses bit
 * positions of the first.
 * ================================================================ */

#define DEV$M_REC                  0x00000001u
#define DEV$M_CCL                  0x00000002u
#define DEV$M_TRM                  0x00000004u
#define DEV$M_DIR                  0x00000008u
#define DEV$M_SDI                  0x00000010u
#define DEV$M_SQD                  0x00000020u
#define DEV$M_SPL                  0x00000040u
#define DEV$M_OPR                  0x00000080u
#define DEV$M_RCT                  0x00000100u
#define DEV$M_QSVD                 0x00000200u
#define DEV$M_QSVBL                0x00000400u
#define DEV$M_MULTI_SECONDARY      0x00000800u
#define DEV$M_FILL_3               0x00001000u
#define DEV$M_NET                  0x00002000u
#define DEV$M_FOD                  0x00004000u
#define DEV$M_DUA                  0x00008000u
#define DEV$M_SHR                  0x00010000u
#define DEV$M_GEN                  0x00020000u
#define DEV$M_AVL                  0x00040000u
#define DEV$M_MNT                  0x00080000u
#define DEV$M_MBX                  0x00100000u
#define DEV$M_DMT                  0x00200000u
#define DEV$M_ELG                  0x00400000u
#define DEV$M_ALL                  0x00800000u
#define DEV$M_FOR                  0x01000000u
#define DEV$M_SWL                  0x02000000u
#define DEV$M_IDV                  0x04000000u
#define DEV$M_ODV                  0x08000000u
#define DEV$M_RND                  0x10000000u
#define DEV$M_RTM                  0x20000000u
#define DEV$M_RCK                  0x40000000u
#define DEV$M_WCK                  0x80000000u
#define DEV$M_CLU                  0x00000001u
#define DEV$M_DET                  0x00000002u
#define DEV$M_RTT                  0x00000004u
#define DEV$M_CDP                  0x00000008u
#define DEV$M_2P                   0x00000010u
#define DEV$M_MSCP                 0x00000020u
#define DEV$M_SSM                  0x00000040u
#define DEV$M_SRV                  0x00000080u
#define DEV$M_RED                  0x00000100u
#define DEV$M_NNM                  0x00000200u
#define DEV$M_WBC                  0x00000400u
#define DEV$M_WTC                  0x00000800u
#define DEV$M_HOC                  0x00001000u
#define DEV$M_LOC                  0x00002000u
#define DEV$M_DFS                  0x00004000u
#define DEV$M_DAP                  0x00008000u
#define DEV$M_NLT                  0x00010000u
#define DEV$M_SEX                  0x00020000u
#define DEV$M_SHD                  0x00040000u
#define DEV$M_VRT                  0x00080000u
#define DEV$M_LDR                  0x00100000u
#define DEV$M_NOLB                 0x00200000u
#define DEV$M_NOCLU                0x00400000u
#define DEV$M_VMEM                 0x00800000u
#define DEV$M_SCSI                 0x01000000u
#define DEV$M_WLG                  0x02000000u
#define DEV$M_NOFE                 0x04000000u
#define DEV$M_AIP                  0x08000000u
#define DEV$M_CRAMIO               0x10000000u
#define DEV$M_DTN                  0x20000000u
#define DEV$M_MULTI_ENABLED        0x40000000u
#define DEV$M_POOL_MBR             0x80000000u

#define DEV$V_REC                  0
#define DEV$V_CCL                  1
#define DEV$V_TRM                  2
#define DEV$V_DIR                  3
#define DEV$V_SDI                  4
#define DEV$V_SQD                  5
#define DEV$V_SPL                  6
#define DEV$V_OPR                  7
#define DEV$V_RCT                  8
#define DEV$V_QSVD                 9
#define DEV$V_QSVBL                10
#define DEV$V_MULTI_SECONDARY      11
#define DEV$V_NET                  13
#define DEV$V_FOD                  14
#define DEV$V_DUA                  15
#define DEV$V_SHR                  16
#define DEV$V_GEN                  17
#define DEV$V_AVL                  18
#define DEV$V_MNT                  19
#define DEV$V_MBX                  20
#define DEV$V_DMT                  21
#define DEV$V_ELG                  22
#define DEV$V_ALL                  23
#define DEV$V_FOR                  24
#define DEV$V_SWL                  25
#define DEV$V_IDV                  26
#define DEV$V_ODV                  27
#define DEV$V_RND                  28
#define DEV$V_RTM                  29
#define DEV$V_RCK                  30
#define DEV$V_WCK                  31
#define DEV$V_CLU                  0
#define DEV$V_DET                  1
#define DEV$V_RTT                  2
#define DEV$V_CDP                  3
#define DEV$V_2P                   4
#define DEV$V_MSCP                 5
#define DEV$V_SSM                  6
#define DEV$V_SRV                  7
#define DEV$V_RED                  8
#define DEV$V_NNM                  9
#define DEV$V_WBC                  10
#define DEV$V_WTC                  11
#define DEV$V_HOC                  12
#define DEV$V_LOC                  13
#define DEV$V_DFS                  14
#define DEV$V_DAP                  15
#define DEV$V_NLT                  16
#define DEV$V_SEX                  17
#define DEV$V_SHD                  18
#define DEV$V_VRT                  19
#define DEV$V_LDR                  20
#define DEV$V_NOLB                 21
#define DEV$V_NOCLU                22
#define DEV$V_VMEM                 23
#define DEV$V_SCSI                 24
#define DEV$V_WLG                  25
#define DEV$V_NOFE                 26
#define DEV$V_AIP                  27
#define DEV$V_CRAMIO               28
#define DEV$V_DTN                  29
#define DEV$V_MULTI_ENABLED        30
#define DEV$V_POOL_MBR             31

#ifdef __cplusplus
}
#endif

#endif /* __DEVDEF_H */
