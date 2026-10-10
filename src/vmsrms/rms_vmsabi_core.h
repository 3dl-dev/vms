/*
 * rms_vmsabi_core.h - the internal half of the VMS-ABI RMS entry points
 * (vms-692). rms_vmsabi.c sees only the VMS-layout control blocks
 * (src/libvms/include/vms/); this interface, in plain C types, is how it
 * reaches the RMS engine (rms_vmsabi_core.c, which sees only OVMX's internal
 * blocks). The two layouts never meet in one translation unit.
 */
#ifndef RMS_VMSABI_CORE_H
#define RMS_VMSABI_CORE_H

#include <stdint.h>

/* One $PARSE/$SEARCH exchange, in native types. */
struct ovmx_rmsabi_name {
    /* in */
    const char *fna;
    unsigned    fns;
    const char *dna;
    unsigned    dns;
    unsigned    nop;            /* NAM$B_NOP */
    /* out */
    char        str[256];       /* $PARSE: expanded; $SEARCH: resultant */
    unsigned    len;
    uint32_t    fnb;            /* NAM$L_FNB (the VMS bit values) */
    uint16_t    fid[3];         /* NAM$W_FID ($SEARCH) */
    uint16_t    did[3];         /* NAM$W_DID */
    char        dvi[16];        /* NAM$T_DVI: counted device name */
    /* components of str: offset and length (0 length = absent) */
    uint8_t     node_off, node_len, dev_off, dev_len, dir_off, dir_len;
    uint8_t     name_off, name_len, type_off, type_len, ver_off, ver_len;
    uint32_t    stv;
};

/* $PARSE: *wcc is the NAM$L_WCC context handle (0 = none); it is (re)set to a
 * fresh context. Returns the RMS status. */
uint32_t ovmx_rmsabi_parse(uint32_t *wcc, struct ovmx_rmsabi_name *io);

/* $SEARCH on the context *wcc names; at the end of the search (RMS$_NMF /
 * RMS$_FNF) the context is released and *wcc set to 0. */
uint32_t ovmx_rmsabi_search(uint32_t *wcc, struct ovmx_rmsabi_name *io);

/* ---- File and record operations (vms-8b5) ------------------------------
 * A VMS FAB/RAB carries OVMX's handles in FAB$W_IFI / RAB$W_ISI; the internal
 * FAB/RAB/NAM live here, as VMS RMS keeps its internal structures behind IFI
 * and ISI. A handle is a slot plus a generation, so a stale IFI/ISI (after
 * $CLOSE / $DISCONNECT) is refused (RMS$_IFI / RMS$_ISI), never reused. */

/* What a NAM receives: the expanded and resultant strings and, for each
 * component (node, dev, dir, name, type, ver), which string it lies in
 * (0 none, 1 expanded, 2 resultant), its offset and length. */
struct ovmx_rmsabi_namout {
    char     esa[256];
    unsigned esl;
    char     rsa[256];
    unsigned rsl;
    uint32_t fnb;
    uint8_t  which[6], off[6], len[6];
    uint16_t fid[3], did[3];
    char     dvi[16];
};

struct ovmx_rmsabi_fab {
    /* in */
    const char *fna;
    unsigned    fns;
    const char *dna;
    unsigned    dns;
    int         nam;             /* the FAB names a NAM */
    unsigned    nop, ess, rss;   /* its options and string-area sizes */
    /* in and out (the file's attributes after $OPEN/$CREATE) */
    uint32_t    fop, alq, mrn;
    uint16_t    deq, mrs;
    uint8_t     fac, shr, org, rat, rfm, fsz;
    /* out */
    uint32_t    stv;
    struct ovmx_rmsabi_namout n;
};

uint32_t ovmx_rmsabi_open(int create, uint16_t *ifi, struct ovmx_rmsabi_fab *io);
uint32_t ovmx_rmsabi_close(uint16_t *ifi, uint32_t *stv);
uint32_t ovmx_rmsabi_erase(struct ovmx_rmsabi_fab *io);

struct ovmx_rmsabi_rab {
    /* in */
    uint32_t    rop;
    uint8_t     rac, krf, ksz;
    const void *kbf;
    void       *ubf;
    unsigned    usz;
    const void *rbf_in;          /* $PUT/$UPDATE: the record */
    unsigned    rsz_in;
    /* out */
    void       *rbf;             /* $GET: the record (in ubf, or the engine's) */
    unsigned    rsz;
    uint32_t    stv;
    uint16_t    rfa[3];
};

enum { OVMX_RMSABI_GET, OVMX_RMSABI_PUT, OVMX_RMSABI_FIND, OVMX_RMSABI_UPDATE,
       OVMX_RMSABI_DELETE, OVMX_RMSABI_REWIND, OVMX_RMSABI_FLUSH,
       OVMX_RMSABI_DISCONNECT };

uint32_t ovmx_rmsabi_connect(uint16_t ifi, uint16_t *isi, struct ovmx_rmsabi_rab *io);
uint32_t ovmx_rmsabi_record(int op, uint16_t *isi, struct ovmx_rmsabi_rab *io);

#endif /* RMS_VMSABI_CORE_H */
