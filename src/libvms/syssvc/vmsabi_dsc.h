/*
 * vmsabi_dsc.h - the two forms of a VMS string descriptor, as a VMS-ABI entry
 * point (sys_vmsabi.c: SYS$/LIB$/STR$ by their upper-case names) tells them
 * apart (vms-7b3).
 *
 * The 32-bit form (DSC$K_CLASS_S and friends, 8 bytes):
 *     +0 word     DSC$W_LENGTH
 *     +2 byte     DSC$B_DTYPE
 *     +3 byte     DSC$B_CLASS
 *     +4 longword DSC$A_POINTER (a 32-bit address, sign-extended)
 * The 64-bit form (DSC64, 24 bytes):
 *     +0 word     DSC64$W_MBO   = 1
 *     +2 byte     DSC64$B_DTYPE
 *     +3 byte     DSC64$B_CLASS
 *     +4 longword DSC64$L_MBMO  = -1
 *     +8 quadword DSC64$Q_LENGTH
 *    +16 quadword DSC64$PQ_POINTER
 * A VMS routine accepts either and recognises the 64-bit form by its MBO and
 * MBMO fields -- never by the caller's pointer size or the entry name (OpenVMS
 * Calling Standard, "64-bit descriptors"). A 32-bit descriptor of length 1
 * whose pointer is -1 would read as DSC64; -1 is never a valid 32-bit string
 * address, which is why the calling standard can use the pattern.
 *
 * Plain byte offsets, no pointer-size pragmas: the same code builds for the
 * Alpha shareables and for the host unit test (tests/libvms/test_vmsabi_dsc.c).
 */
#ifndef OVMX_VMSABI_DSC_H
#define OVMX_VMSABI_DSC_H

#include <stdint.h>
#include <string.h>

struct ovmx_vmsabi_dsc {
    uint64_t len;
    uint64_t ptr;
    uint8_t  dtype;
    uint8_t  cls;
    uint8_t  is64;
};

/* 1 when d is a DSC64 (MBO word 1 and MBMO longword -1), else 0. */
static inline int ovmx_vmsabi_is_dsc64(const void *d)
{
    uint16_t mbo;
    int32_t mbmo;
    memcpy(&mbo, d, sizeof mbo);
    memcpy(&mbmo, (const unsigned char *)d + 4, sizeof mbmo);
    return mbo == 1 && mbmo == -1;
}

/* Read a descriptor of either form. */
static inline void ovmx_vmsabi_dsc_read(const void *d, struct ovmx_vmsabi_dsc *x)
{
    const unsigned char *b = d;
    x->dtype = b[2];
    x->cls = b[3];
    x->is64 = (uint8_t)ovmx_vmsabi_is_dsc64(d);
    if (x->is64) {
        memcpy(&x->len, b + 8, sizeof x->len);
        memcpy(&x->ptr, b + 16, sizeof x->ptr);
        return;
    }
    uint16_t l;
    int32_t p;
    memcpy(&l, b, sizeof l);
    memcpy(&p, b + 4, sizeof p);
    x->len = l;
    x->ptr = (uint64_t)(int64_t)p;          /* a 32-bit address sign-extends */
}

/* Store a length and address into a descriptor of either form (a class-D
 * result written back to the caller). */
static inline void ovmx_vmsabi_dsc_write(void *d, uint64_t len, uint64_t ptr)
{
    unsigned char *b = d;
    if (ovmx_vmsabi_is_dsc64(d)) {
        memcpy(b + 8, &len, sizeof len);
        memcpy(b + 16, &ptr, sizeof ptr);
        return;
    }
    uint16_t l = (uint16_t)len;
    uint32_t p = (uint32_t)ptr;
    memcpy(b, &l, sizeof l);
    memcpy(b + 4, &p, sizeof p);
}

#endif /* OVMX_VMSABI_DSC_H */
