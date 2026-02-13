#ifndef __RMS_RAB_H
#define __RMS_RAB_H

#include <stdint.h>
#include <sys/types.h>

/* Record access modes */
#define RAB$C_SEQ   0   /* Sequential access */
#define RAB$C_KEY   1   /* Keyed access */
#define RAB$C_RFA   2   /* Record file address */

/* Block ID for RAB */
#define RAB$C_BID   1
#define RAB$C_BLN   sizeof(struct RAB)

/* RAB options (rab$l_rop) */
#define RAB$M_EOF   0x0001  /* Position to EOF */
#define RAB$M_FDL   0x0002  /* Fast delete */
#define RAB$M_UIF   0x0004  /* Update if existent */
#define RAB$M_HSH   0x0008  /* Use hash */
#define RAB$M_LOA   0x0010  /* Follow load */
#define RAB$M_KGE   0x0020  /* Key >= */
#define RAB$M_KGT   0x0040  /* Key > */
#define RAB$M_NLK   0x0080  /* No lock */
#define RAB$M_RLK   0x0100  /* Read lock */
#define RAB$M_RAH   0x0200  /* Read ahead */
#define RAB$M_WBH   0x0400  /* Write behind */
#define RAB$M_LOC   0x0800  /* Locate mode */
#define RAB$M_PMT   0x1000  /* Prompt */
#define RAB$M_TPT   0x2000  /* Truncate put */
#define RAB$M_NXT   0x4000  /* Next record (skip deleted) */

/* Record File Address */
typedef struct {
    uint16_t rfa$w_area;
    uint16_t rfa$w_page;
    uint16_t rfa$w_offset;
} RFA;

struct FAB;

struct RAB {
    uint8_t  rab$b_bid;         /* Block ID (must be 1) */
    uint8_t  rab$b_bln;         /* Block length */
    uint16_t rab$w_isi;         /* Internal stream identifier */
    uint32_t rab$l_sts;         /* Status */
    uint32_t rab$l_stv;         /* Status value */
    uint32_t rab$l_rop;         /* Record options */
    struct FAB *rab$l_fab;      /* FAB address */
    char    *rab$l_ubf;         /* User buffer address */
    uint16_t rab$w_usz;         /* User buffer size */
    char    *rab$l_rbf;         /* Record buffer address (after $GET) */
    uint16_t rab$w_rsz;         /* Record size (after $GET) */
    uint8_t  rab$b_rac;         /* Record access mode */
    uint8_t  rab$b_krf;         /* Key of reference */
    char    *rab$l_kbf;         /* Key buffer address */
    uint8_t  rab$b_ksz;         /* Key buffer size */
    uint32_t rab$l_bkt;         /* Bucket (relative record number) */
    RFA      rab$w_rfa;         /* Record file address */
    void    *rab$l_ctx;         /* User context */
    /* Internal state */
    off_t    _current_offset;   /* Current file position */
    void    *_rms_stream;       /* Internal stream state */
    int      _eof;              /* EOF indicator */
    off_t    _last_rec_offset;  /* Offset of last record accessed */
    uint16_t _last_rec_size;    /* Size of last record accessed */
};

/* Initialization macro matching VMS cc$rms_rab */
#define cc$rms_rab (struct RAB){ \
    .rab$b_bid = RAB$C_BID, \
    .rab$b_bln = sizeof(struct RAB), \
    .rab$b_rac = RAB$C_SEQ \
}

#endif /* __RMS_RAB_H */
