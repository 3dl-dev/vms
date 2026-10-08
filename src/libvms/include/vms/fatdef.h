/*
 * vms/fatdef.h - the Files-11 record attribute area (FAT) in the VMS layout
 * (vms-022): the 32 bytes an ACP returns for ATR$C_RECATTR, as laid down in
 * the ODS-2 file header (Files-11 On-Disk Structure Specification; the same
 * layout as src/vmsfs/include/vmsfs/ods2.h struct ods2_recattr).
 */
#ifndef __VMS_FATDEF_H
#define __VMS_FATDEF_H
#include "vms_abi.h"

#define FAT$K_LENGTH 32
#define FAT$C_LENGTH 32

typedef struct fatdef {
    union {
        unsigned char fat$b_rtype;          /*  0: record type + file org  */
        struct {
            unsigned char fat$v_rtype   : 4;    /* record format (FAB$C_*) */
            unsigned char fat$v_fileorg : 4;    /* organization            */
        };
    };
    unsigned char  fat$b_rattrib;           /*  1: record attributes       */
    unsigned short fat$w_rsize;             /*  2: record size             */
    union {
        unsigned int fat$l_hiblk;           /*  4: highest allocated VBN   */
        struct {
            unsigned short fat$w_hiblkh;    /*     (high word first)       */
            unsigned short fat$w_hiblkl;
        };
    };
    union {
        unsigned int fat$l_efblk;           /*  8: end-of-file VBN         */
        struct {
            unsigned short fat$w_efblkh;
            unsigned short fat$w_efblkl;
        };
    };
    unsigned short fat$w_ffbyte;            /* 12: first free byte          */
    unsigned char  fat$b_bktsize;           /* 14: bucket size              */
    unsigned char  fat$b_vfcsize;           /* 15: VFC control size         */
    unsigned short fat$w_maxrec;            /* 16: maximum record size      */
    unsigned short fat$w_defext;            /* 18: default extend quantity  */
    unsigned short fat$w_gbc;               /* 20: global buffer count      */
    unsigned char  fat$b_reserved[8];       /* 22                           */
    unsigned short fat$w_versions;          /* 30: default version limit    */
} FAT;

__VMS_ABI_SIZE(struct fatdef, 32);
__VMS_ABI_OFFSET(struct fatdef, fat$b_rattrib, 1);
__VMS_ABI_OFFSET(struct fatdef, fat$w_rsize, 2);
__VMS_ABI_OFFSET(struct fatdef, fat$w_hiblkh, 4);
__VMS_ABI_OFFSET(struct fatdef, fat$w_hiblkl, 6);
__VMS_ABI_OFFSET(struct fatdef, fat$w_efblkh, 8);
__VMS_ABI_OFFSET(struct fatdef, fat$w_efblkl, 10);
__VMS_ABI_OFFSET(struct fatdef, fat$w_ffbyte, 12);
__VMS_ABI_OFFSET(struct fatdef, fat$b_bktsize, 14);
__VMS_ABI_OFFSET(struct fatdef, fat$w_maxrec, 16);
__VMS_ABI_OFFSET(struct fatdef, fat$w_gbc, 20);
__VMS_ABI_OFFSET(struct fatdef, fat$w_versions, 30);

#endif /* __VMS_FATDEF_H */
