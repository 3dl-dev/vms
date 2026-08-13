#ifndef __RMS_NAM_H
#define __RMS_NAM_H

#include <stdint.h>

/*
 * NAM - Name Block
 *
 * Used with $PARSE and $SEARCH to analyze and resolve filespecs.
 */

#define NAM$C_BID      4
#define NAM$C_BLN      sizeof(struct NAM)
#define NAM$C_MAXRSS   255   /* Max resultant string size */
#define NAM$C_MAXESS   255   /* Max expanded string size */

struct NAM {
    uint8_t  nam$b_bid;        /* Block ID (NAM$C_BID) */
    uint8_t  nam$b_bln;        /* Block length */
    uint32_t nam$l_sts;        /* Status */
    /* Expanded string (after $PARSE, wildcards not resolved) */
    char    *nam$l_esa;        /* Expanded string area address */
    uint8_t  nam$b_ess;        /* Expanded string area size (buffer) */
    uint8_t  nam$b_esl;        /* Expanded string length (returned) */
    /* Resultant string (after $SEARCH, wildcards resolved) */
    char    *nam$l_rsa;        /* Resultant string area address */
    uint8_t  nam$b_rss;        /* Resultant string size (buffer) */
    uint8_t  nam$b_rsl;        /* Resultant string length (returned) */
    /* Component sizes (set by $PARSE) */
    uint8_t  nam$b_node;       /* Node name length */
    uint8_t  nam$b_dev;        /* Device name length */
    uint8_t  nam$b_dir;        /* Directory length */
    uint8_t  nam$b_name;       /* Filename length */
    uint8_t  nam$b_type;       /* File type length */
    uint8_t  nam$b_ver;        /* Version length */
    /* Component pointers (offsets into expanded string, set by $PARSE) */
    char    *nam$l_node;       /* Node name start */
    char    *nam$l_dev;        /* Device name start */
    char    *nam$l_dir;        /* Directory start */
    char    *nam$l_name;       /* Filename start */
    char    *nam$l_type;       /* File type start */
    char    *nam$l_ver;        /* Version start */
    /* Wildcard/status flags */
    uint32_t nam$l_fnb;        /* Filename status flags */
    uint32_t nam$l_wcc;        /* Wildcard context (internal) */
    /* Internal */
    void    *nam$$l_context;   /* Wildcard search context (internal) */
    /* Parse-control / related-file / device-id fields (vms-ec70: needed by
     * callers such as MadGoat MMK that set NAM$M_SYNCHK for syntax-only
     * $PARSE, chain a related-file NAM, or read the device-id back).  Appended
     * at the end so all pre-existing field offsets are unchanged. */
    uint8_t  nam$b_nop;        /* $PARSE options (NAM$M_SYNCHK etc.) */
    struct NAM *nam$l_rlf;     /* Related-file NAM for relative $PARSE */
    char     nam$t_dvi[16];    /* Device-id (counted string) after $PARSE */
};

/* NAM flags (nam$l_fnb) */
#define NAM$M_WILDCARD    0x0001  /* Filespec contains wildcards */
#define NAM$M_EXP_DEV     0x0002  /* Device was explicitly specified */
#define NAM$M_EXP_DIR     0x0004  /* Directory was explicitly specified */
#define NAM$M_EXP_NAME    0x0008  /* Name was explicitly specified */
#define NAM$M_EXP_TYPE    0x0010  /* Type was explicitly specified */
#define NAM$M_EXP_VER     0x0020  /* Version was explicitly specified */
#define NAM$M_NODE        0x0040  /* Node was specified */
#define NAM$M_CNCL_DEV    0x0080  /* Device is concealed */
#define NAM$M_ROOT_DIR    0x0100  /* Rooted directory */
#define NAM$M_SEARCH_LIST 0x0200  /* Device uses search list */
#define NAM$M_WILD_NAME   0x0400  /* Wildcard in name field */
#define NAM$M_WILD_TYPE   0x0800  /* Wildcard in type field */
#define NAM$M_WILD_VER    0x1000  /* Wildcard in version field */
#define NAM$M_WILD_DIR    0x2000  /* Wildcard in directory field */

/* NAM initialization macro */
#define cc$rms_nam (struct NAM){ \
    .nam$b_bid = NAM$C_BID, \
    .nam$b_bln = sizeof(struct NAM) \
}

/* nam$b_nop $PARSE option flags */
#define NAM$M_SYNCHK      0x08    /* Syntax-only parse (no device/dir check) */
#define NAM$M_PWD         0x10    /* Parse-with-directory (search list) */

#endif /* __RMS_NAM_H */
