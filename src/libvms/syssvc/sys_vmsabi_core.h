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
/* 1 if `chan` is assigned to a terminal or a mailbox, whose driver answers
 * IO$_ACCESS/IO$_DEACCESS itself; 0 otherwise (a file-structured device, where
 * they are ACP functions taking the VMS ACP-QIO arguments, or a channel
 * $GETDVI cannot describe). */
int ovmx_vmsabi_chan_is_record_device(uint16_t chan);
uint32_t ovmx_vmsabi_qio(int wait, uint32_t efn, uint16_t chan, uint32_t func,
                         void *iosb, unsigned long long astadr,
                         unsigned long long astprm, const unsigned long long p[6]);
/* LIB$PUT_OUTPUT of `len` bytes at `p` (vms-3b3f). */
uint32_t ovmx_vmsabi_put_output(const char *p, unsigned len);

/* vms-3b3f: the services an image LINKed on OpenVMS Alpha reaches through
 * SYS$PUBLIC_VECTORS that take a descriptor, given here as address + length
 * (an output buffer as address + capacity). */
uint32_t ovmx_vmsabi_ascefc(uint32_t efn, const char *name, unsigned namelen,
                            uint32_t prot, uint32_t perm);
uint32_t ovmx_vmsabi_dlcefc(const char *name, unsigned namelen);
uint32_t ovmx_vmsabi_bintim(const char *s, unsigned len, void *timadr);
uint32_t ovmx_vmsabi_asctim(uint16_t *timlen, char *out, unsigned outcap,
                            const void *timadr, uint32_t cvtflg);
uint32_t ovmx_vmsabi_getmsg(uint32_t msgid, uint16_t *msglen, char *out,
                            unsigned outcap, uint32_t flags, uint8_t *outadr);
/* The $FAO engine in the VMS argument forms (sys_fao.c). */
uint32_t ovmx_fao_vmsabi(const char *ctr, unsigned ctrlen, uint16_t *outlen,
                         char *out, unsigned outcap, const uint64_t *prm);
int count_fao_args(const char *ctrl, uint16_t len);

/* A string argument as address + length; `p == NULL && !len` is "omitted". */
struct ovmx_abi_str {
    const char *p;
    unsigned    len;
    int         given;
};

uint32_t ovmx_vmsabi_dellnm(const struct ovmx_abi_str *tab, const struct ovmx_abi_str *log,
                            const uint8_t *acmode);
uint32_t ovmx_vmsabi_crembx(int prmflg, uint16_t *chan, uint32_t maxmsg, uint32_t bufquo,
                            uint32_t promsk, uint32_t acmode, const struct ovmx_abi_str *log,
                            uint32_t flags);
/* $GETJPI(W) / $GETSYI(W) / $GETDVI(W): the item list rebuilt natively. */
uint32_t ovmx_vmsabi_getjpi(int wait, uint32_t efn, const uint32_t *pidadr,
                            const struct ovmx_abi_str *prcnam,
                            const struct ovmx_abi_item *items, unsigned n, void *iosb,
                            unsigned long long astadr, unsigned long long astprm);
uint32_t ovmx_vmsabi_getsyi(int wait, uint32_t efn, uint32_t *csidadr,
                            const struct ovmx_abi_str *node,
                            const struct ovmx_abi_item *items, unsigned n, void *iosb,
                            unsigned long long astadr, unsigned long long astprm);
uint32_t ovmx_vmsabi_getdvi(int wait, uint32_t efn, uint16_t chan,
                            const struct ovmx_abi_str *devnam,
                            const struct ovmx_abi_item *items, unsigned n, void *iosb,
                            unsigned long long astadr, unsigned long long astprm);
uint32_t ovmx_vmsabi_enq(int wait, uint32_t efn, uint32_t lkmode, void *lksb, uint32_t flags,
                         const struct ovmx_abi_str *resnam, uint32_t parid,
                         unsigned long long astadr, unsigned long long astprm,
                         unsigned long long blkast, uint32_t acmode, uint32_t rsdm);
/* The services that name a process by PID address or process-name descriptor. */
#define OVMX_ABI_PRC_WAKE   1
#define OVMX_ABI_PRC_RESUME 2
#define OVMX_ABI_PRC_SUSPND 3
#define OVMX_ABI_PRC_FORCEX 4
#define OVMX_ABI_PRC_DELPRC 5
uint32_t ovmx_vmsabi_prc(int op, const uint32_t *pidadr, const struct ovmx_abi_str *prcnam,
                         uint32_t arg);
uint32_t ovmx_vmsabi_setpri(const uint32_t *pidadr, const struct ovmx_abi_str *prcnam,
                            uint32_t pri, uint32_t *prvpri, uint32_t pol, uint32_t *prevpol);
uint32_t ovmx_vmsabi_asctoid(const struct ovmx_abi_str *name, uint32_t *id, uint32_t *attrib);
uint32_t ovmx_vmsabi_idtoasc(uint32_t id, uint16_t *namlen, char *out, unsigned cap,
                             uint32_t *resid, uint32_t *attrib, uint32_t *ctx);
uint32_t ovmx_vmsabi_grantid(int revoke, const uint32_t *pidadr,
                             const struct ovmx_abi_str *prcnam, const uint32_t *id,
                             const struct ovmx_abi_str *name, uint32_t *prvatr,
                             uint32_t segment);
/* $CHKPRO / $CREATE_USER_PROFILE (vms-8b5): `have_list` is 0 for a null item
 * list (the service answers it); a profile is a string, by descriptor in
 * $CHKPRO and by address + length in $CREATE_USER_PROFILE. */
uint32_t ovmx_vmsabi_chkpro(int have_list, const struct ovmx_abi_item *items, unsigned n,
                            const struct ovmx_abi_str *objpro,
                            const struct ovmx_abi_str *subjpro);
uint32_t ovmx_vmsabi_create_user_profile(const struct ovmx_abi_str *usrnam, int have_list,
                                         const struct ovmx_abi_item *items, unsigned n,
                                         uint32_t flags, void *usrpro, uint32_t *usrprolen,
                                         uint32_t *contxt);
uint32_t ovmx_vmsabi_sndopr(const struct ovmx_abi_str *msg, uint16_t chan);
uint32_t ovmx_vmsabi_brkthru(int wait, uint32_t efn, const struct ovmx_abi_str *msg,
                             const struct ovmx_abi_str *sendto, uint32_t sndtyp, void *iosb,
                             uint32_t carcon, uint32_t flags, uint32_t reqid, uint32_t timout,
                             unsigned long long astadr, unsigned long long astprm);

/* ---------------- vms-3b3f batch 3: LIBRTL string / symbol / VM routines ----
 * A VMS descriptor as the shim read it (either form). For a class-D
 * destination, ptr is storage from the VMS-ABI heap below (a 32-bit address),
 * which the core half reallocates and the shim writes back. */
struct ovmx_abi_dx {
    uint16_t len;
    uint8_t  dtype;
    uint8_t  cls;
    char    *ptr;
    int      given;
};

/* Storage a 32-bit caller can address (below 2 GB), for class-D strings and
 * LIB$GET_VM. NULL when none is available. */
void    *ovmx_vmsabi_p0_alloc(uint32_t n);
int      ovmx_vmsabi_p0_free(void *p);          /* 1 freed, 0 not a live block */

#define OVMX_ABI_STR_COPY_DX  1
#define OVMX_ABI_STR_APPEND   2
#define OVMX_ABI_STR_PREFIX   3
#define OVMX_ABI_STR_UPCASE   4
uint32_t ovmx_vmsabi_str_dst(int op, struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src);
#define OVMX_ABI_STR_LEFT     1
#define OVMX_ABI_STR_RIGHT    2
#define OVMX_ABI_STR_LEN_EXTR 3
#define OVMX_ABI_STR_POS_EXTR 4
uint32_t ovmx_vmsabi_str_extract(int op, struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src,
                                 const void *a, const void *b);
uint32_t ovmx_vmsabi_str_replace(struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src,
                                 const uint32_t *start, const uint32_t *end,
                                 const struct ovmx_abi_dx *rep);
uint32_t ovmx_vmsabi_str_translate(struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src,
                                   const struct ovmx_abi_dx *tran, const struct ovmx_abi_dx *match);
uint32_t ovmx_vmsabi_str_trim(struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src,
                              uint16_t *outlen);
uint32_t ovmx_vmsabi_str_dupl_char(struct ovmx_abi_dx *dst, const int32_t *len, const char *ch);
uint32_t ovmx_vmsabi_str_element(struct ovmx_abi_dx *dst, const uint32_t *elem,
                                 const struct ovmx_abi_dx *delim, const struct ovmx_abi_dx *src);
uint32_t ovmx_vmsabi_str_concat(struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src, unsigned n);
uint32_t ovmx_vmsabi_str_free1(struct ovmx_abi_dx *dst);
#define OVMX_ABI_CMP_COMPARE     1
#define OVMX_ABI_CMP_COMPARE_EQL 2
#define OVMX_ABI_CMP_POSITION    3
#define OVMX_ABI_CMP_FFIS        4
#define OVMX_ABI_CMP_FFNIS       5
#define OVMX_ABI_CMP_INDEX       6
#define OVMX_ABI_CMP_LOCC        7
#define OVMX_ABI_CMP_MATCHC      8
#define OVMX_ABI_CMP_SKPC        9
uint32_t ovmx_vmsabi_str_in(int op, const struct ovmx_abi_dx *a, const struct ovmx_abi_dx *b,
                            const uint32_t *start);
uint32_t ovmx_vmsabi_set_symbol(const struct ovmx_abi_dx *sym, const struct ovmx_abi_dx *val,
                                const uint32_t *tbl);
uint32_t ovmx_vmsabi_get_symbol(const struct ovmx_abi_dx *sym, struct ovmx_abi_dx *val,
                                uint16_t *len, uint32_t *tbl);
uint32_t ovmx_vmsabi_delete_symbol(const struct ovmx_abi_dx *sym, const uint32_t *tbl);
uint32_t ovmx_vmsabi_find_file(const struct ovmx_abi_dx *spec, struct ovmx_abi_dx *result,
                               uint32_t *ctx, const struct ovmx_abi_dx *def,
                               const struct ovmx_abi_dx *rel, uint32_t *stv,
                               const uint32_t *flags);
#define OVMX_ABI_GETXXI_JPI 1
#define OVMX_ABI_GETXXI_SYI 2
#define OVMX_ABI_GETXXI_DVI 3
uint32_t ovmx_vmsabi_lib_getxxi(int op, const uint32_t *item, const uint32_t *pid,
                                uint16_t chan, const struct ovmx_abi_dx *name, void *resval,
                                struct ovmx_abi_dx *resstr, uint16_t *reslen, uint32_t *csid);
uint32_t ovmx_vmsabi_sys_fao(const struct ovmx_abi_dx *ctr, uint16_t *outlen,
                             struct ovmx_abi_dx *out, const uint64_t *prm);
uint32_t ovmx_vmsabi_creprc(uint32_t *pidadr, const struct ovmx_abi_str *image,
                            const struct ovmx_abi_str *input, const struct ovmx_abi_str *output,
                            const struct ovmx_abi_str *error, const void *prvadr,
                            const void *quota, const struct ovmx_abi_str *prcnam,
                            uint32_t baspri, uint32_t uic, uint32_t mbxunt, uint32_t stsflg,
                            const struct ovmx_abi_str *node);

#endif /* SYS_VMSABI_CORE_H */
