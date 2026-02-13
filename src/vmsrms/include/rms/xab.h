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

#endif /* __RMS_XAB_H */
