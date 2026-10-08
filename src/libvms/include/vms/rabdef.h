/*
 * vms/rabdef.h - the RMS Record Access Block in the VMS layout (vms-022).
 * Offsets and values: docs/oracle/alpha84-starlet-defs/RABDEF.txt.
 */
#ifndef __VMS_RABDEF_H
#define __VMS_RABDEF_H
#include "vms_abi.h"

#define RAB$C_BID  1
#define RAB$C_BLN  68
#define RAB$K_BLN  68
#define RAB$C_SEQ  0
#define RAB$C_KEY  1
#define RAB$C_RFA  2
/* rab$l_rop */
#define RAB$M_ASY  1
#define RAB$M_TPT  2
#define RAB$M_REA  4
#define RAB$M_RRL  8
#define RAB$M_UIF  16
#define RAB$M_MAS  32
#define RAB$M_FDL  64
#define RAB$M_REV  128
#define RAB$M_EOF  256
#define RAB$M_RAH  512
#define RAB$M_WBH  1024
#define RAB$M_BIO  2048
#define RAB$M_CDK  4096
#define RAB$M_LOA  8192
#define RAB$M_LIM  16384
#define RAB$M_LOC  65536
#define RAB$M_WAT  131072
#define RAB$M_ULK  262144
#define RAB$M_RLK  524288
#define RAB$M_NLK  1048576
#define RAB$M_KGE  2097152
#define RAB$M_KGT  4194304
#define RAB$M_NXR  8388608
#define RAB$M_RNE  16777216
#define RAB$M_TMO  33554432
#define RAB$M_CVT  67108864
#define RAB$M_RNF  134217728
#define RAB$M_ETO  268435456
#define RAB$M_PTA  536870912
#define RAB$M_PMT  1073741824
#define RAB$M_CCO  2147483648u

#pragma __required_pointer_size __save
#pragma __required_pointer_size __short

struct rabdef {
    unsigned char  rab$b_bid;           /*  0 */
    unsigned char  rab$b_bln;           /*  1 */
    unsigned short rab$w_isi;           /*  2 internal stream identifier */
    unsigned int   rab$l_rop;           /*  4 */
    unsigned int   rab$l_sts;           /*  8 */
    unsigned int   rab$l_stv;           /* 12 */
    unsigned short rab$w_rfa[3];        /* 16: RFA0 longword + RFA4 word */
    unsigned short rab$w_reserved;      /* 22 */
    unsigned int   rab$l_ctx;           /* 24 */
    unsigned short rab$w_rop_2;         /* 28 */
    unsigned char  rab$b_rac;           /* 30 */
    unsigned char  rab$b_tmo;           /* 31 */
    unsigned short rab$w_usz;           /* 32 */
    unsigned short rab$w_rsz;           /* 34 */
    char          *rab$l_ubf;           /* 36 */
    char          *rab$l_rbf;           /* 40 */
    char          *rab$l_rhb;           /* 44 */
    char          *rab$l_kbf;           /* 48 (also rab$l_pbf) */
    unsigned char  rab$b_ksz;           /* 52 (also rab$b_psz) */
    unsigned char  rab$b_krf;           /* 53 */
    unsigned char  rab$b_mbf;           /* 54 */
    unsigned char  rab$b_mbc;           /* 55 */
    unsigned int   rab$l_bkt;           /* 56 (also rab$l_dct) */
    void          *rab$l_fab;           /* 60 */
    void          *rab$l_xab;           /* 64 */
};

#pragma __required_pointer_size __restore

#define RAB rabdef

__VMS_ABI_SIZE(struct rabdef, 68);
__VMS_ABI_OFFSET(struct rabdef, rab$w_isi, 2);
__VMS_ABI_OFFSET(struct rabdef, rab$l_rop, 4);
__VMS_ABI_OFFSET(struct rabdef, rab$l_sts, 8);
__VMS_ABI_OFFSET(struct rabdef, rab$l_stv, 12);
__VMS_ABI_OFFSET(struct rabdef, rab$w_rfa, 16);
__VMS_ABI_OFFSET(struct rabdef, rab$l_ctx, 24);
__VMS_ABI_OFFSET(struct rabdef, rab$w_rop_2, 28);
__VMS_ABI_OFFSET(struct rabdef, rab$b_rac, 30);
__VMS_ABI_OFFSET(struct rabdef, rab$b_tmo, 31);
__VMS_ABI_OFFSET(struct rabdef, rab$w_usz, 32);
__VMS_ABI_OFFSET(struct rabdef, rab$w_rsz, 34);
__VMS_ABI_OFFSET(struct rabdef, rab$l_ubf, 36);
__VMS_ABI_OFFSET(struct rabdef, rab$l_rbf, 40);
__VMS_ABI_OFFSET(struct rabdef, rab$l_rhb, 44);
__VMS_ABI_OFFSET(struct rabdef, rab$l_kbf, 48);
__VMS_ABI_OFFSET(struct rabdef, rab$b_ksz, 52);
__VMS_ABI_OFFSET(struct rabdef, rab$b_krf, 53);
__VMS_ABI_OFFSET(struct rabdef, rab$b_mbf, 54);
__VMS_ABI_OFFSET(struct rabdef, rab$b_mbc, 55);
__VMS_ABI_OFFSET(struct rabdef, rab$l_bkt, 56);
__VMS_ABI_OFFSET(struct rabdef, rab$l_fab, 60);
__VMS_ABI_OFFSET(struct rabdef, rab$l_xab, 64);

#endif /* __VMS_RABDEF_H */
