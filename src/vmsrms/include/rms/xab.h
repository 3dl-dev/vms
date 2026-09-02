#ifndef __RMS_XAB_H
#define __RMS_XAB_H

#include <stdint.h>

/*
 * XAB - Extended Attribute Block
 *
 * Provides additional file attributes beyond what FAB holds.
 * XABs are chained together via the nxt field.
 */

/* XAB type codes */
#define XAB$C_KEY   21   /* Key definition (for indexed files) */
#define XAB$C_DAT   18   /* Date/time attributes */
#define XAB$C_PRO   19   /* Protection attributes */
#define XAB$C_FHC   29   /* File header characteristics */
#define XAB$C_ALL   20   /* Allocation attributes */
/* Item XAB (XABITM). ORACLE-PINNED 2026-08-13 on OpenVMS VAX V7.3
 * (lab-2 node VAX1): $XABDEF/$XABITMDEF assembled as GLOBAL symbols,
 * exact values read from the object GSD via ANALYZE/OBJECT/GSD
 * (documented tool output, Rule 8). Anchor XAB$C_KEY=21 matched. */
#define XAB$C_ITM   36   /* Item-list XAB (XABITM) type code */

/* XABITM control (xab$b_mode) and structure size. ORACLE-PINNED as above. */
#define XAB$K_SENSEMODE  1   /* xab$b_mode: sense (read) items */
#define XAB$K_SETMODE    2   /* xab$b_mode: set (write) items */
#define XAB$K_ITMLEN     32  /* Length in bytes of a XABITM structure */

/* Key data types */
#define XAB$C_STG    0   /* String */
#define XAB$C_IN2    1   /* Signed word (16-bit) */
#define XAB$C_BN2    2   /* Unsigned word (16-bit binary) */
#define XAB$C_IN4    3   /* Signed longword (32-bit) */
#define XAB$C_BN4    4   /* Unsigned longword (32-bit binary) */
#define XAB$C_PAC    5   /* Packed decimal */
#define XAB$C_IN8    6   /* Signed quadword (64-bit) */
#define XAB$C_BN8    7   /* Unsigned quadword (64-bit binary) */

/* Key flags */
#define XAB$M_DUP       0x01  /* Duplicates allowed */
#define XAB$M_CHG       0x02  /* Key value may change */
#define XAB$M_NUL       0x04  /* Null key value allowed */
#define XAB$M_IDX_NCMPR 0x08  /* Index not compressed */
#define XAB$M_DAT_NCMPR 0x10  /* Data not compressed */
#define XAB$M_KEY_NCMPR 0x20  /* Key not compressed */

/* Maximum key segments */
#define XAB$C_MAXSEG    8

/* Key definition XAB */
struct XABKEY {
    uint8_t  xab$b_cod;       /* XAB type code (XAB$C_KEY) */
    uint8_t  xab$b_bln;       /* Block length */
    void    *xab$l_nxt;       /* Next XAB in chain */
    uint8_t  xab$b_ref;       /* Key of reference (0 = primary) */
    uint8_t  xab$b_dtp;       /* Key data type */
    uint16_t xab$w_flg;       /* Key flags */
    /* Key segment positions (up to 8 segments) */
    uint16_t xab$w_pos0;      /* Key segment 0 position */
    uint16_t xab$w_pos1;      /* Key segment 1 position */
    uint16_t xab$w_pos2;      /* Key segment 2 position */
    uint16_t xab$w_pos3;      /* Key segment 3 position */
    uint16_t xab$w_pos4;      /* Key segment 4 position */
    uint16_t xab$w_pos5;      /* Key segment 5 position */
    uint16_t xab$w_pos6;      /* Key segment 6 position */
    uint16_t xab$w_pos7;      /* Key segment 7 position */
    /* Key segment sizes */
    uint8_t  xab$b_siz0;      /* Key segment 0 size */
    uint8_t  xab$b_siz1;      /* Key segment 1 size */
    uint8_t  xab$b_siz2;      /* Key segment 2 size */
    uint8_t  xab$b_siz3;      /* Key segment 3 size */
    uint8_t  xab$b_siz4;      /* Key segment 4 size */
    uint8_t  xab$b_siz5;      /* Key segment 5 size */
    uint8_t  xab$b_siz6;      /* Key segment 6 size */
    uint8_t  xab$b_siz7;      /* Key segment 7 size */
    uint8_t  xab$b_nseg;      /* Number of key segments */
    uint16_t xab$w_ian;       /* Index area number */
    uint8_t  xab$b_dan;       /* Data area number */
    uint8_t  xab$b_lvl;       /* Index level count (output) */
    uint8_t  xab$b_ibs;       /* Index bucket size */
    uint8_t  xab$b_dbs;       /* Data bucket size */
    uint32_t xab$l_dvb;       /* First data bucket VBN */
    char    *xab$l_knm;       /* Key name (32 chars max) */
    uint16_t xab$w_tks;       /* Total key size (sum of all segments) */
};

/* Date/time XAB */
struct XABDAT {
    uint8_t  xab$b_cod;       /* XAB$C_DAT */
    uint8_t  xab$b_bln;       /* Block length */
    void    *xab$l_nxt;       /* Next XAB in chain */
    uint64_t xab$q_cdt;       /* Creation date/time */
    uint64_t xab$q_rdt;       /* Revision date/time */
    uint64_t xab$q_edt;       /* Expiration date/time */
    uint64_t xab$q_bdt;       /* Backup date/time */
    uint16_t xab$w_rvn;       /* Revision number */
};

/* Protection XAB */
struct XABPRO {
    uint8_t  xab$b_cod;       /* XAB$C_PRO */
    uint8_t  xab$b_bln;       /* Block length */
    void    *xab$l_nxt;       /* Next XAB in chain */
    uint16_t xab$w_pro;       /* Protection mask (SOGW format) */
    uint32_t xab$l_uic;       /* UIC of file owner */
};

/*
 * File-header-characteristics XAB (output on $OPEN/$DISPLAY). RMS fills these
 * from the file's ODS-2 FAT (ATR$C_RECATTR); every field has a genuine on-disk
 * source read over the executive ACP -- vms-dfa. OVMX-native field layout
 * (Rule 8), like XABKEY/XABDAT/XABPRO; the semantics are the real header values.
 */
struct XABFHC {
    uint8_t  xab$b_cod;       /* XAB$C_FHC */
    uint8_t  xab$b_bln;       /* Block length */
    void    *xab$l_nxt;       /* Next XAB in chain */
    uint8_t  xab$b_rfm;       /* Record format (FAB$C_FIX/VAR/STMLF/...) */
    uint8_t  xab$b_atr;       /* Record attributes */
    uint16_t xab$w_lrl;       /* Longest record length */
    uint32_t xab$l_hbk;       /* Highest allocated VBN (highest block) */
    uint32_t xab$l_ebk;       /* End-of-file block (VBN) */
    uint16_t xab$w_ffb;       /* First free byte in the EOF block */
    uint8_t  xab$b_bkz;       /* Bucket size (blocks) */
    uint16_t xab$w_mrz;       /* Maximum record size */
    uint16_t xab$w_dxq;       /* Default extension quantity */
    uint16_t xab$w_gbc;       /* Global buffer count */
    uint16_t xab$w_verlimit;  /* Version limit */
};

/*
 * Allocation-control XAB. Input controls (aop/aln/loc/...) drive $CREATE; on
 * $OPEN/$DISPLAY RMS reports the file's realized allocation (alq/bkz/deq) from
 * the ODS-2 FAT over the ACP -- vms-dfa. Fields with no on-disk source (the
 * create-time input controls) are left 0/default (honest).
 */
struct XABALL {
    uint8_t  xab$b_cod;       /* XAB$C_ALL */
    uint8_t  xab$b_bln;       /* Block length */
    void    *xab$l_nxt;       /* Next XAB in chain */
    uint8_t  xab$b_aop;       /* Allocation options (input) */
    uint8_t  xab$b_aln;       /* Alignment boundary type (input) */
    uint8_t  xab$b_bkz;       /* Bucket size (blocks) */
    uint32_t xab$l_alq;       /* Allocation quantity (blocks) */
    uint16_t xab$w_deq;       /* Default extension quantity */
    uint32_t xab$l_loc;       /* Location (input: start VBN/LBN) */
    uint16_t xab$w_vol;       /* Related volume number (input) */
};

/* Initialization macros */
#define cc$rms_xabkey (struct XABKEY){ \
    .xab$b_cod = XAB$C_KEY, \
    .xab$b_bln = sizeof(struct XABKEY), \
    .xab$b_dtp = XAB$C_STG, \
    .xab$b_nseg = 1 \
}

#define cc$rms_xabdat (struct XABDAT){ \
    .xab$b_cod = XAB$C_DAT, \
    .xab$b_bln = sizeof(struct XABDAT) \
}

#define cc$rms_xabpro (struct XABPRO){ \
    .xab$b_cod = XAB$C_PRO, \
    .xab$b_bln = sizeof(struct XABPRO), \
    .xab$w_pro = 0xFF00 \
}

#define cc$rms_xabfhc (struct XABFHC){ \
    .xab$b_cod = XAB$C_FHC, \
    .xab$b_bln = sizeof(struct XABFHC) \
}

#define cc$rms_xaball (struct XABALL){ \
    .xab$b_cod = XAB$C_ALL, \
    .xab$b_bln = sizeof(struct XABALL) \
}

#endif /* __RMS_XAB_H */
