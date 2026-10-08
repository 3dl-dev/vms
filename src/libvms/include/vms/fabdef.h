/*
 * vms/fabdef.h - the RMS File Access Block in the VMS layout (vms-022).
 * Offsets and values: docs/oracle/alpha84-starlet-defs/FABDEF.txt.
 */
#ifndef __VMS_FABDEF_H
#define __VMS_FABDEF_H
#include "vms_abi.h"

#define FAB$C_BID   3
#define FAB$C_BLN   80
#define FAB$K_BLN   80
/* fab$l_fop */
#define FAB$M_ASY   1
#define FAB$M_MXV   2
#define FAB$M_SUP   4
#define FAB$M_TMP   8
#define FAB$M_TMD   16
#define FAB$M_DFW   32
#define FAB$M_SQO   64
#define FAB$M_RWO   128
#define FAB$M_POS   256
#define FAB$M_WCK   512
#define FAB$M_NEF   1024
#define FAB$M_RWC   2048
#define FAB$M_SPL   8192
#define FAB$M_SCF   16384
#define FAB$M_DLT   32768
#define FAB$M_UFO   131072
#define FAB$M_INP   524288
#define FAB$M_CTG   1048576
#define FAB$M_CBT   2097152
#define FAB$M_RCK   8388608
#define FAB$M_NAM   16777216
#define FAB$M_CIF   33554432
#define FAB$M_ESC   134217728
#define FAB$M_TEF   268435456
#define FAB$M_OFP   536870912
#define FAB$M_KFO   1073741824
/* fab$b_fac */
#define FAB$M_PUT   1
#define FAB$M_GET   2
#define FAB$M_DEL   4
#define FAB$M_UPD   8
#define FAB$M_TRN   16
#define FAB$M_BIO   32
#define FAB$M_BRO   64
#define FAB$M_EXE   128
/* fab$b_shr */
#define FAB$M_SHRPUT 1
#define FAB$M_SHRGET 2
#define FAB$M_SHRDEL 4
#define FAB$M_SHRUPD 8
#define FAB$M_MSE    16
#define FAB$M_NIL    32
#define FAB$M_UPI    64
/* fab$b_org */
#define FAB$C_SEQ   0
#define FAB$C_REL   16
#define FAB$C_IDX   32
/* fab$b_rat */
#define FAB$M_FTN   1
#define FAB$M_CR    2
#define FAB$M_PRN   4
#define FAB$M_BLK   8
/* fab$b_rfm */
#define FAB$C_UDF   0
#define FAB$C_FIX   1
#define FAB$C_VAR   2
#define FAB$C_VFC   3
#define FAB$C_STM   4
#define FAB$C_STMLF 5
#define FAB$C_STMCR 6

#pragma __required_pointer_size __save
#pragma __required_pointer_size __short

struct fabdef {
    unsigned char  fab$b_bid;           /*  0 */
    unsigned char  fab$b_bln;           /*  1 */
    unsigned short fab$w_ifi;           /*  2 internal file identifier */
    unsigned int   fab$l_fop;           /*  4 */
    unsigned int   fab$l_sts;           /*  8 */
    unsigned int   fab$l_stv;           /* 12 */
    unsigned int   fab$l_alq;           /* 16 */
    unsigned short fab$w_deq;           /* 20 */
    unsigned char  fab$b_fac;           /* 22 */
    unsigned char  fab$b_shr;           /* 23 */
    unsigned int   fab$l_ctx;           /* 24 */
    unsigned char  fab$b_rtv;           /* 28 */
    unsigned char  fab$b_org;           /* 29 */
    unsigned char  fab$b_rat;           /* 30 */
    unsigned char  fab$b_rfm;           /* 31 */
    unsigned char  fab$b_journal;       /* 32 */
    unsigned char  fab$b_ru_facility;   /* 33 */
    unsigned short fab$w_fopext;        /* 34 */
    void          *fab$l_xab;           /* 36 */
    void          *fab$l_nam;           /* 40 (also fab$l_naml) */
    char          *fab$l_fna;           /* 44 */
    char          *fab$l_dna;           /* 48 */
    unsigned char  fab$b_fns;           /* 52 */
    unsigned char  fab$b_dns;           /* 53 */
    unsigned short fab$w_mrs;           /* 54 */
    unsigned int   fab$l_mrn;           /* 56 */
    unsigned short fab$w_bls;           /* 60 */
    unsigned char  fab$b_bks;           /* 62 */
    unsigned char  fab$b_fsz;           /* 63 */
    unsigned int   fab$l_dev;           /* 64 */
    unsigned int   fab$l_sdc;           /* 68 */
    unsigned short fab$w_gbc;           /* 72 */
    unsigned char  fab$b_acmodes;       /* 74 */
    unsigned char  fab$b_rcf;           /* 75 */
    unsigned short fab$w_gbc_initial;   /* 76 */
    unsigned short fab$w_reserved_mbz;  /* 78 */
};

#pragma __required_pointer_size __restore

#define FAB fabdef

__VMS_ABI_SIZE(struct fabdef, 80);
__VMS_ABI_OFFSET(struct fabdef, fab$w_ifi, 2);
__VMS_ABI_OFFSET(struct fabdef, fab$l_fop, 4);
__VMS_ABI_OFFSET(struct fabdef, fab$l_sts, 8);
__VMS_ABI_OFFSET(struct fabdef, fab$l_stv, 12);
__VMS_ABI_OFFSET(struct fabdef, fab$l_alq, 16);
__VMS_ABI_OFFSET(struct fabdef, fab$w_deq, 20);
__VMS_ABI_OFFSET(struct fabdef, fab$b_fac, 22);
__VMS_ABI_OFFSET(struct fabdef, fab$b_shr, 23);
__VMS_ABI_OFFSET(struct fabdef, fab$l_ctx, 24);
__VMS_ABI_OFFSET(struct fabdef, fab$b_rtv, 28);
__VMS_ABI_OFFSET(struct fabdef, fab$b_org, 29);
__VMS_ABI_OFFSET(struct fabdef, fab$b_rat, 30);
__VMS_ABI_OFFSET(struct fabdef, fab$b_rfm, 31);
__VMS_ABI_OFFSET(struct fabdef, fab$b_journal, 32);
__VMS_ABI_OFFSET(struct fabdef, fab$w_fopext, 34);
__VMS_ABI_OFFSET(struct fabdef, fab$l_xab, 36);
__VMS_ABI_OFFSET(struct fabdef, fab$l_nam, 40);
__VMS_ABI_OFFSET(struct fabdef, fab$l_fna, 44);
__VMS_ABI_OFFSET(struct fabdef, fab$l_dna, 48);
__VMS_ABI_OFFSET(struct fabdef, fab$b_fns, 52);
__VMS_ABI_OFFSET(struct fabdef, fab$b_dns, 53);
__VMS_ABI_OFFSET(struct fabdef, fab$w_mrs, 54);
__VMS_ABI_OFFSET(struct fabdef, fab$l_mrn, 56);
__VMS_ABI_OFFSET(struct fabdef, fab$w_bls, 60);
__VMS_ABI_OFFSET(struct fabdef, fab$b_bks, 62);
__VMS_ABI_OFFSET(struct fabdef, fab$b_fsz, 63);
__VMS_ABI_OFFSET(struct fabdef, fab$l_dev, 64);
__VMS_ABI_OFFSET(struct fabdef, fab$l_sdc, 68);
__VMS_ABI_OFFSET(struct fabdef, fab$w_gbc, 72);
__VMS_ABI_OFFSET(struct fabdef, fab$b_acmodes, 74);
__VMS_ABI_OFFSET(struct fabdef, fab$b_rcf, 75);
__VMS_ABI_OFFSET(struct fabdef, fab$w_gbc_initial, 76);

#endif /* __VMS_FABDEF_H */
