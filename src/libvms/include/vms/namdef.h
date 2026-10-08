/*
 * vms/namdef.h - the RMS Name Block in the VMS layout (vms-022).
 * Offsets and values: docs/oracle/alpha84-starlet-defs/NAMDEF.txt.
 */
#ifndef __VMS_NAMDEF_H
#define __VMS_NAMDEF_H
#include "vms_abi.h"

#define NAM$C_BID        2
#define NAM$C_BLN        96
#define NAM$K_BLN        96
#define NAM$C_MAXRSS     255
#define NAM$C_MAXRSSLCL  255
/* nam$b_nop */
#define NAM$M_PWD        1
#define NAM$M_SYNCHK     8
#define NAM$M_NOCONCEAL  16
#define NAM$M_SLPARSE    32
#define NAM$M_SRCHXABS   64
/* nam$l_fnb */
#define NAM$M_EXP_VER     1
#define NAM$M_EXP_TYPE    2
#define NAM$M_EXP_NAME    4
#define NAM$M_WILD_VER    8
#define NAM$M_WILD_TYPE   16
#define NAM$M_WILD_NAME   32
#define NAM$M_EXP_DIR     64
#define NAM$M_EXP_DEV     128
#define NAM$M_WILDCARD    256
#define NAM$M_SEARCH_LIST 2048
#define NAM$M_CNCL_DEV    4096
#define NAM$M_ROOT_DIR    8192
#define NAM$M_LOWVER      16384
#define NAM$M_HIGHVER     32768
#define NAM$M_PPF         65536
#define NAM$M_NODE        131072
#define NAM$M_QUOTED      262144
#define NAM$M_GRP_MBR     524288
#define NAM$M_WILD_DIR    1048576

#pragma __required_pointer_size __save
#pragma __required_pointer_size __short

struct namdef {
    unsigned char  nam$b_bid;           /*  0 */
    unsigned char  nam$b_bln;           /*  1 */
    unsigned char  nam$b_rss;           /*  2 */
    unsigned char  nam$b_rsl;           /*  3 */
    char          *nam$l_rsa;           /*  4 */
    unsigned char  nam$b_nop;           /*  8 */
    unsigned char  nam$b_rfs;           /*  9 */
    unsigned char  nam$b_ess;           /* 10 */
    unsigned char  nam$b_esl;           /* 11 */
    char          *nam$l_esa;           /* 12 */
    void          *nam$l_rlf;           /* 16 */
    char           nam$t_dvi[16];       /* 20 */
    unsigned short nam$w_fid[3];        /* 36: number, sequence, RVN/NMX */
    unsigned short nam$w_did[3];        /* 42 */
    unsigned int   nam$l_wcc;           /* 48 */
    unsigned int   nam$l_fnb;           /* 52 */
    unsigned char  nam$b_node;          /* 56 */
    unsigned char  nam$b_dev;           /* 57 */
    unsigned char  nam$b_dir;           /* 58 */
    unsigned char  nam$b_name;          /* 59 */
    unsigned char  nam$b_type;          /* 60 */
    unsigned char  nam$b_ver;           /* 61 */
    unsigned char  nam$b_nmc;           /* 62 */
    unsigned char  nam$b_reserved;      /* 63 */
    char          *nam$l_node;          /* 64 */
    char          *nam$l_dev;           /* 68 */
    char          *nam$l_dir;           /* 72 */
    char          *nam$l_name;          /* 76 */
    char          *nam$l_type;          /* 80 */
    char          *nam$l_ver;           /* 84 */
    unsigned short nam$w_first_wild_dir;/* 88 */
    unsigned short nam$w_long_dir_levels;/* 90 */
    unsigned int   nam$l_reserved2;     /* 92 */
};

#pragma __required_pointer_size __restore

#define NAM namdef

__VMS_ABI_SIZE(struct namdef, 96);
__VMS_ABI_OFFSET(struct namdef, nam$b_rss, 2);
__VMS_ABI_OFFSET(struct namdef, nam$b_rsl, 3);
__VMS_ABI_OFFSET(struct namdef, nam$l_rsa, 4);
__VMS_ABI_OFFSET(struct namdef, nam$b_nop, 8);
__VMS_ABI_OFFSET(struct namdef, nam$b_rfs, 9);
__VMS_ABI_OFFSET(struct namdef, nam$b_ess, 10);
__VMS_ABI_OFFSET(struct namdef, nam$b_esl, 11);
__VMS_ABI_OFFSET(struct namdef, nam$l_esa, 12);
__VMS_ABI_OFFSET(struct namdef, nam$l_rlf, 16);
__VMS_ABI_OFFSET(struct namdef, nam$t_dvi, 20);
__VMS_ABI_OFFSET(struct namdef, nam$w_fid, 36);
__VMS_ABI_OFFSET(struct namdef, nam$w_did, 42);
__VMS_ABI_OFFSET(struct namdef, nam$l_wcc, 48);
__VMS_ABI_OFFSET(struct namdef, nam$l_fnb, 52);
__VMS_ABI_OFFSET(struct namdef, nam$b_node, 56);
__VMS_ABI_OFFSET(struct namdef, nam$b_dev, 57);
__VMS_ABI_OFFSET(struct namdef, nam$b_dir, 58);
__VMS_ABI_OFFSET(struct namdef, nam$b_name, 59);
__VMS_ABI_OFFSET(struct namdef, nam$b_type, 60);
__VMS_ABI_OFFSET(struct namdef, nam$b_ver, 61);
__VMS_ABI_OFFSET(struct namdef, nam$b_nmc, 62);
__VMS_ABI_OFFSET(struct namdef, nam$l_node, 64);
__VMS_ABI_OFFSET(struct namdef, nam$l_dev, 68);
__VMS_ABI_OFFSET(struct namdef, nam$l_dir, 72);
__VMS_ABI_OFFSET(struct namdef, nam$l_name, 76);
__VMS_ABI_OFFSET(struct namdef, nam$l_type, 80);
__VMS_ABI_OFFSET(struct namdef, nam$l_ver, 84);
__VMS_ABI_OFFSET(struct namdef, nam$w_first_wild_dir, 88);
__VMS_ABI_OFFSET(struct namdef, nam$w_long_dir_levels, 90);

#endif /* __VMS_NAMDEF_H */
