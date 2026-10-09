/*
 * sys_vmsabi_core.h - the internal half of the VMS-ABI system services
 * (vms-38b). sys_vmsabi.c sees only the VMS argument forms (32-bit
 * descriptors and item lists, src/libvms/include/vms/); this interface, in
 * plain native types, is how it reaches the services OVMX implements with its
 * own structures (sys_vmsabi_core.c). The two never meet in one TU.
 */
#ifndef SYS_VMSABI_CORE_H
#define SYS_VMSABI_CORE_H

#include <stdint.h>

/* One item-list entry with native addresses. */
struct ovmx_abi_item {
    uint16_t  len;
    uint16_t  code;
    void     *buf;
    uint16_t *retlen;
};

uint32_t ovmx_vmsabi_assign(const char *dev, unsigned devlen, uint16_t *chan,
                            uint32_t acmode, const char *mbx, unsigned mbxlen,
                            uint32_t flags);
uint32_t ovmx_vmsabi_dassgn(uint16_t chan);

/* $TRNLNM (create == 0) or $CRELNM (create == 1); items[n] terminated by the
 * caller's count. */
uint32_t ovmx_vmsabi_lnm(int create, const uint32_t *attr,
                         const char *tab, unsigned tablen,
                         const char *log, unsigned loglen,
                         const uint8_t *acmode,
                         const struct ovmx_abi_item *items, unsigned n);

/* $QIO IO$_ACCESS on a file-class channel (vms-38b): the file's attributes as
 * the executive ACP reports them, in native form. */
struct ovmx_abi_fileattr {
    uint32_t uchar;            /* ATR$C_UCHAR */
    uint16_t fpro;             /* ATR$C_FPRO */
    uint16_t uic_member;       /* ATR$C_UIC: member, then group */
    uint16_t uic_group;
    uint8_t  recattr[32];      /* ATR$C_RECATTR: the FAT, verbatim */
    uint8_t  credate[8], revdate[8], expdate[8], bakdate[8];
    uint16_t fid[3];           /* the resolved File ID (FIB$W_FID) */
    char     name[96];         /* resultant NAME.TYP;VER */
    unsigned namelen;
};

/* IO$_ACCESS: resolve `name` (NAME.TYP[;VER]) in directory `did`, or open by
 * `fid` when did is 0/0/0 and fid is not; acctl is FIB$L_ACCTL. keep != 0
 * leaves the file accessed on the channel (IO$M_ACCESS) until IO$_DEACCESS.
 * Returns the I/O status (SS$_ILLIOFUNC on a channel that is not a file-class
 * channel). */
uint32_t ovmx_vmsabi_acp_access(uint16_t chan, uint32_t acctl, const uint16_t did[3],
                                const uint16_t fid[3], const char *name, unsigned namelen,
                                int keep, struct ovmx_abi_fileattr *out);
/* IO$_DEACCESS on a file-class channel. */
uint32_t ovmx_vmsabi_acp_deaccess(uint16_t chan);
/* Complete a $QIO: set the event flag and deliver the AST, if one was given. */
void ovmx_vmsabi_io_complete(uint32_t efn, void (*astadr)(unsigned long long), unsigned long long astprm);

/* $QIO / $QIOW (wait != 0) of any non-ACP function, P1-P6 as the caller
 * passed them (vms-3b3f). */
uint32_t ovmx_vmsabi_qio(int wait, uint32_t efn, uint16_t chan, uint32_t func,
                         void *iosb, unsigned long long astadr,
                         unsigned long long astprm, const unsigned long long p[6]);
/* LIB$PUT_OUTPUT of `len` bytes at `p` (vms-3b3f). */
uint32_t ovmx_vmsabi_put_output(const char *p, unsigned len);

#endif /* SYS_VMSABI_CORE_H */
