/*
 * vms/fibdef.h - the ACP File Information Block in the VMS layout (vms-022).
 * Offsets: docs/oracle/alpha84-starlet-defs/FIBDEF.txt (FIB$K_LENGTH 96).
 */
#ifndef __VMS_FIBDEF_H
#define __VMS_FIBDEF_H
#include "vms_abi.h"

#define FIB$K_LENGTH 96
#define FIB$C_LENGTH 96

#pragma __required_pointer_size __save
#pragma __required_pointer_size __short

typedef struct fibdef {
    unsigned int   fib$l_acctl;         /*  0 access control (fib$b_wsize at 3) */
    unsigned short fib$w_fid[3];        /*  4 file ID */
    unsigned short fib$w_did[3];        /* 10 directory ID */
    unsigned int   fib$l_wcc;           /* 16 wildcard context */
    unsigned short fib$w_nmctl;         /* 20 name control */
    unsigned short fib$w_exctl;         /* 22 extend control (fib$w_cntrlfunc) */
    unsigned int   fib$l_exsz;          /* 24 (fib$l_cntrlval) */
    unsigned int   fib$l_exvbn;         /* 28 */
    unsigned char  fib$b_alopts;        /* 32 */
    unsigned char  fib$b_alalign;       /* 33 */
    unsigned short fib$w_alloc[5];      /* 34 allocation / locate FID+address */
    unsigned short fib$w_verlimit;      /* 44 */
    unsigned char  fib$b_agent_mode;    /* 46 */
    unsigned char  fib$b_ru_facility;   /* 47 */
    unsigned int   fib$l_aclctx;        /* 48 */
    unsigned int   fib$l_acl_status;    /* 52 */
    unsigned int   fib$l_status;        /* 56 */
    unsigned int   fib$l_alt_access;    /* 60 */
    unsigned int   fib$l_mov_svbn;      /* 64 */
    unsigned int   fib$l_mov_vbncnt;    /* 68 */
    unsigned short fib$w_file_hdrseq_incr;  /* 72 */
    unsigned short fib$w_dir_hdrseq_incr;   /* 74 */
    unsigned short fib$w_file_dataseq_incr; /* 76 */
    unsigned short fib$w_dir_dataseq_incr;  /* 78 */
    unsigned int   fib$l_caching_options;   /* 80 */
    unsigned short fib$w_sd_fid[3];     /* 84 */
    unsigned short fib$w_reserved;      /* 90 */
    unsigned char  fib$b_name_format_in;  /* 92 */
    unsigned char  fib$b_name_format_out; /* 93 */
    unsigned char  fib$b_ascname_format;  /* 94 */
    unsigned char  fib$b_reserved_2;      /* 95 */
} FIBDEF;

#pragma __required_pointer_size __restore

__VMS_ABI_SIZE(struct fibdef, 96);
__VMS_ABI_OFFSET(struct fibdef, fib$w_fid, 4);
__VMS_ABI_OFFSET(struct fibdef, fib$w_did, 10);
__VMS_ABI_OFFSET(struct fibdef, fib$l_wcc, 16);
__VMS_ABI_OFFSET(struct fibdef, fib$w_nmctl, 20);
__VMS_ABI_OFFSET(struct fibdef, fib$w_exctl, 22);
__VMS_ABI_OFFSET(struct fibdef, fib$l_exsz, 24);
__VMS_ABI_OFFSET(struct fibdef, fib$l_exvbn, 28);
__VMS_ABI_OFFSET(struct fibdef, fib$b_alopts, 32);
__VMS_ABI_OFFSET(struct fibdef, fib$w_alloc, 34);
__VMS_ABI_OFFSET(struct fibdef, fib$w_verlimit, 44);
__VMS_ABI_OFFSET(struct fibdef, fib$l_aclctx, 48);
__VMS_ABI_OFFSET(struct fibdef, fib$l_status, 56);
__VMS_ABI_OFFSET(struct fibdef, fib$l_caching_options, 80);
__VMS_ABI_OFFSET(struct fibdef, fib$w_sd_fid, 84);
__VMS_ABI_OFFSET(struct fibdef, fib$b_name_format_in, 92);

#endif /* __VMS_FIBDEF_H */
