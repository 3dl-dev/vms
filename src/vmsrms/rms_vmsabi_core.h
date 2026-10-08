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

#endif /* RMS_VMSABI_CORE_H */
