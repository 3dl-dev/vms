#ifndef __RMS_FAB_H
#define __RMS_FAB_H

#include <stdint.h>

/* File Access Block - main control block for file operations */

/* File organization */
#define FAB$C_SEQ   0   /* Sequential */
#define FAB$C_REL   1   /* Relative */
#define FAB$C_IDX   2   /* Indexed */

/* Record format */
#define FAB$C_UDF   0   /* Undefined */
#define FAB$C_FIX   1   /* Fixed length */
#define FAB$C_VAR   2   /* Variable length */
#define FAB$C_VFC   3   /* Variable with fixed control */
#define FAB$C_STM   4   /* Stream */
#define FAB$C_STMLF 5   /* Stream-LF */
#define FAB$C_STMCR 6   /* Stream-CR */

/* Record attributes */
#define FAB$M_FTN   0x01  /* FORTRAN carriage control */
#define FAB$M_CR    0x02  /* Implied carriage return */
#define FAB$M_PRN   0x04  /* Print file format */
#define FAB$M_BLK   0x08  /* Block-mode I/O */

/* File access (fab$b_fac) */
#define FAB$M_GET   0x01  /* Read access */
#define FAB$M_PUT   0x02  /* Write access */
#define FAB$M_DEL   0x04  /* Delete access */
#define FAB$M_UPD   0x08  /* Update access */
#define FAB$M_TRN   0x10  /* Truncate access */
#define FAB$M_BIO   0x20  /* Block I/O */
#define FAB$M_BRO   0x40  /* Block+record I/O */

/* File share options (fab$b_shr) */
#define FAB$M_SHRGET 0x01
#define FAB$M_SHRPUT 0x02
#define FAB$M_SHRDEL 0x04
#define FAB$M_SHRUPD 0x08
#define FAB$M_MSE    0x10
#define FAB$M_NIL    0x20

/* File options (fab$l_fop) */
#define FAB$M_CIF   0x00000001  /* Create if non-existent */
#define FAB$M_DFW   0x00000002  /* Deferred write */
#define FAB$M_MXV   0x00000004  /* Maximize version */
#define FAB$M_SUP   0x00000008  /* Supersede */
#define FAB$M_TMP   0x00000010  /* Temporary file */
#define FAB$M_TMD   0x00000020  /* Temporary, delete on close */
#define FAB$M_DLT   0x00000040  /* Delete on close */
#define FAB$M_SCF   0x00000080  /* Submit as command file */
#define FAB$M_SPL   0x00000100  /* Spool on close */
#define FAB$M_NAM   0x00000200  /* Use NAM block */
#define FAB$M_CBT   0x00000400  /* Contiguous best try */
#define FAB$M_CTG   0x00000800  /* Contiguous */
#define FAB$M_SQO   0x00001000  /* Sequential only */

/* Block ID for FAB */
#define FAB$C_BID   3
#define FAB$C_BLN   sizeof(struct FAB)

struct RAB;
struct NAM;
struct XABKEY;

struct FAB {
    uint8_t  fab$b_bid;         /* Block ID (must be 3) */
    uint8_t  fab$b_bln;         /* Block length */
    uint16_t fab$w_ifi;         /* Internal file identifier */
    uint32_t fab$l_fop;         /* File options */
    uint32_t fab$l_sts;         /* Status (after operation) */
    uint32_t fab$l_stv;         /* Status value (secondary) */
    uint32_t fab$l_alq;         /* Allocation quantity */
    uint16_t fab$w_deq;         /* Default extension quantity */
    uint8_t  fab$b_fac;         /* File access */
    uint8_t  fab$b_shr;         /* File sharing */
    uint8_t  fab$b_org;         /* File organization */
    uint8_t  fab$b_rat;         /* Record attributes */
    uint8_t  fab$b_rfm;         /* Record format */
    uint8_t  fab$b_journal;     /* Journal flags */
    uint16_t fab$w_mrs;         /* Maximum record size */
    uint32_t fab$l_mrn;         /* Maximum record number (relative) */
    char    *fab$l_fna;         /* Filename address */
    uint8_t  fab$b_fns;         /* Filename size */
    char    *fab$l_dna;         /* Default filename address */
    uint8_t  fab$b_dns;         /* Default filename size */
    struct NAM *fab$l_nam;      /* Name block address */
    struct XABKEY *fab$l_xab;   /* XAB chain address */
    uint8_t  fab$b_fsz;         /* Fixed header size (VFC) */
    /* Internal state - not part of VMS FAB, used by our implementation */
    int      _linux_fd;         /* Underlying Linux file descriptor */
    char     _resolved_path[1024]; /* Resolved Linux path */
    void    *_rms_state;        /* Internal RMS state */
};

/* Initialization macro matching VMS cc$rms_fab */
#define cc$rms_fab (struct FAB){ \
    .fab$b_bid = FAB$C_BID, \
    .fab$b_bln = sizeof(struct FAB), \
    .fab$b_org = FAB$C_SEQ, \
    .fab$b_rfm = FAB$C_STMLF, \
    .fab$b_rat = FAB$M_CR, \
    .fab$b_fac = FAB$M_GET, \
    ._linux_fd = -1 \
}

#endif /* __RMS_FAB_H */
