/*
 * sys_vmsabi.c - system services by their upper-case (VMS-ABI) names, taking
 * the VMS argument forms (vms-38b): SYS$ASSIGN, SYS$DASSGN, SYS$TRNLNM,
 * SYS$CRELNM, and SYS$QIO(W) for the ACP functions IO$_ACCESS / IO$_DEACCESS
 * on a file-class channel; vms-3b3f adds SYS$QIO(W) for every other function,
 * LIB$PUT_OUTPUT and SYS$IMGSTA, which an image LINKed on real OpenVMS Alpha
 * reaches through OVMX's SYS$PUBLIC_VECTORS / LIBRTL (src/vmslink/vms_vectors/).
 *
 * A string argument is a VMS descriptor: the 8-byte 32-bit form
 * (vms/descrip.h), or the 64-bit form, recognised as VMS services recognise
 * it by its MBO word (1) and MBMO longword (-1). An item list is a list of
 * 12-byte ILE3 entries (length word, code word, 32-bit buffer address,
 * 32-bit return-length address), or 64-bit ILEB_64 entries recognised the
 * same way, ended by a zero longword. The values go to the same services
 * OVMX's own code calls, through sys_vmsabi_core.c.
 *
 * Built only for the Alpha: the 32-bit address fields need the alpha-dec-vms
 * compiler's #pragma __required_pointer_size.
 */
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <vms/atrdef.h>
#include <vms/descrip.h>
#include <vms/iodef.h>
#include <vms/ssdef.h>
#include <vms/starlet.h>
#include "sys_vmsabi_core.h"
#include "libdef.h"       /* LIB$_BADBLOADR, LIB$_BADBLOSIZ, LIB$_INSVIRMEM */

#define ABI_MAXITEMS 64

#pragma __required_pointer_size __save
#pragma __required_pointer_size __short
struct ile3_32 {
    unsigned short len;
    unsigned short code;
    void           *buf;
    unsigned short *retlen;
};
#pragma __required_pointer_size __restore

/* The 64-bit forms (DSC64 / ILEB_64): MBO word, MBMO longword, quadwords. */
struct dsc64 {
    unsigned short mbo;
    unsigned char  dtype, cls;
    int            mbmo;
    unsigned long long len;
    unsigned long long ptr;
};
struct ileb64 {
    unsigned short mbo;
    unsigned short code;
    int            mbmo;
    unsigned long long len;
    unsigned long long buf;
    unsigned long long retlen;
};

/* A string descriptor's address and length, either form. */
static int dsc_string(const void *d, const char **p, unsigned *len)
{
    if (!d)
        return 0;
    const struct dsc64 *d64 = d;
    if (d64->mbo == 1 && d64->mbmo == -1) {
        *p = (const char *)(uintptr_t)d64->ptr;
        *len = (unsigned)d64->len;
        return 1;
    }
    const struct dsc$descriptor_s *d32 = d;
    *p = d32->dsc$a_pointer;
    *len = d32->dsc$w_length;
    return 1;
}

/* Read a VMS item list into native entries; -1 if it does not end. */
static int items_in(const void *list, struct ovmx_abi_item *out, unsigned max)
{
    unsigned n = 0;
    if (!list)
        return 0;
    const unsigned char *p = list;
    for (;;) {
        const struct ileb64 *e64 = (const struct ileb64 *)p;
        if (e64->mbo == 1 && e64->mbmo == -1) {
            if (n == max)
                return -1;
            out[n].len = (uint16_t)e64->len;
            out[n].code = e64->code;
            out[n].buf = (void *)(uintptr_t)e64->buf;
            out[n].retlen = (uint16_t *)(uintptr_t)e64->retlen;
            n++;
            p += sizeof *e64;
            continue;
        }
        const struct ile3_32 *e = (const struct ile3_32 *)p;
        if (e->len == 0 && e->code == 0)
            return (int)n;                       /* the terminating longword */
        if (n == max)
            return -1;
        out[n].len = e->len;
        out[n].code = e->code;
        out[n].buf = e->buf;
        out[n].retlen = e->retlen;
        n++;
        p += sizeof *e;
    }
}

int SYS$ASSIGN(void *devnam, unsigned short *chan, ...)
{
    va_list ap;
    va_start(ap, chan);
    unsigned int acmode = va_arg(ap, unsigned int);
    void *mbxnam = va_arg(ap, void *);
    unsigned int flags = va_arg(ap, unsigned int);
    va_end(ap);
    const char *dev, *mbx = NULL;
    unsigned devlen, mbxlen = 0;
    if (!chan || !dsc_string(devnam, &dev, &devlen))
        return SS$_BADPARAM;
    if (mbxnam)
        dsc_string(mbxnam, &mbx, &mbxlen);
    uint16_t c = 0;
    uint32_t st = ovmx_vmsabi_assign(dev, devlen, &c, acmode, mbx, mbxlen, flags);
    if (st & 1)
        *chan = c;
    return (int)st;
}

int SYS$DASSGN(unsigned short chan)
{
    return (int)ovmx_vmsabi_dassgn(chan);
}

static int lnm(int create, unsigned int *attr, void *tabnam, void *lognam,
               unsigned char *acmode, void *itmlst)
{
    const char *tab, *log;
    unsigned tablen, loglen;
    if (!dsc_string(tabnam, &tab, &tablen) || !dsc_string(lognam, &log, &loglen))
        return SS$_BADPARAM;
    struct ovmx_abi_item items[ABI_MAXITEMS];
    int n = items_in(itmlst, items, ABI_MAXITEMS);
    if (n < 0)
        return SS$_BADPARAM;
    return (int)ovmx_vmsabi_lnm(create, attr, tab, tablen, log, loglen, acmode,
                                items, (unsigned)n);
}

int SYS$TRNLNM(unsigned int *attr, void *tabnam, void *lognam,
               unsigned char *acmode, void *itmlst)
{
    return lnm(0, attr, tabnam, lognam, acmode, itmlst);
}

int SYS$CRELNM(unsigned int *attr, void *tabnam, void *lognam,
               unsigned char *acmode, void *itmlst)
{
    return lnm(1, attr, tabnam, lognam, acmode, itmlst);
}

/* ------------------------------------------------ $QIO: ACP functions ------ */

/* The attribute-list item (vms/atrdef.h, 32-bit address) and the IOSB. */
struct abi_iosb { unsigned short status, count; unsigned int devdep; };

#define ABI_MAXCHAN 256
static unsigned char chan_write[ABI_MAXCHAN];   /* accessed for write (IO$M_ACCESS) */

/* Is every attribute code in the list one this service fills? */
static int atr_known(unsigned short type)
{
    switch (type) {
    case ATR$C_UCHAR: case ATR$C_RECATTR: case ATR$C_ASCNAME:
    case ATR$C_CREDATE: case ATR$C_REVDATE: case ATR$C_EXPDATE: case ATR$C_BAKDATE:
    case ATR$C_UIC: case ATR$C_FPRO:
        return 1;
    default:
        return 0;
    }
}

static void put_le(unsigned char *dst, unsigned size, const unsigned char *src, unsigned have)
{
    unsigned n = size < have ? size : have;
    memcpy(dst, src, n);
    if (size > n)
        memset(dst + n, 0, size - n);
}

/* Fill one attribute item from the file's attributes. ASCNAME is the file's
 * NAME.TYP;VER padded with spaces to the item size, as the OpenVMS ACP returns
 * it (docs/oracle/alpha84-probes/acp_access_attributes.md). */
static void atr_fill(const ATRDEF *e, const struct ovmx_abi_fileattr *a)
{
    unsigned char *d = (unsigned char *)e->atr$l_addr;
    unsigned size = e->atr$w_size;
    unsigned char tmp[4];
    switch (e->atr$w_type) {
    case ATR$C_UCHAR:
        memcpy(tmp, &a->uchar, 4);
        put_le(d, size, tmp, 4);
        break;
    case ATR$C_RECATTR: put_le(d, size, a->recattr, 32); break;
    case ATR$C_CREDATE: put_le(d, size, a->credate, 8); break;
    case ATR$C_REVDATE: put_le(d, size, a->revdate, 8); break;
    case ATR$C_EXPDATE: put_le(d, size, a->expdate, 8); break;
    case ATR$C_BAKDATE: put_le(d, size, a->bakdate, 8); break;
    case ATR$C_UIC:
        memcpy(tmp, &a->uic_member, 2);
        memcpy(tmp + 2, &a->uic_group, 2);
        put_le(d, size, tmp, 4);
        break;
    case ATR$C_FPRO:
        put_le(d, size, (const unsigned char *)&a->fpro, 2);
        break;
    case ATR$C_ASCNAME: {
        unsigned n = a->namelen < size ? a->namelen : size;
        memcpy(d, a->name, n);
        memset(d + n, ' ', size - n);
        break;
    }
    }
}

static unsigned atr_count(const ATRDEF *l)
{
    unsigned n = 0;
    while (l && (l[n].atr$w_size || l[n].atr$w_type))
        n++;
    return n;
}

static void io_done(struct abi_iosb *iosb, unsigned short st)
{
    if (iosb) {
        iosb->status = st;
        iosb->count = 0;
        iosb->devdep = 0;
    }
}

/* SYS$QIO(W) for IO$_ACCESS and IO$_DEACCESS on a file-class channel, with the
 * VMS ACP-QIO arguments: P1 = the FIB descriptor, P2 = the file name
 * descriptor (NAME.TYP;VER), P3 = the resultant length word, P4 = the
 * resultant name descriptor, P5 = the attribute list. The service returns
 * whether the request was accepted; the I/O status is in the IOSB. */
static int acp_qio(unsigned int efn, unsigned short chan, unsigned int func,
                   void *iosbp, va_list ap)
{
    struct abi_iosb *iosb = iosbp;
    void (*astadr)(unsigned long long) = va_arg(ap, void (*)(unsigned long long));
    unsigned long long astprm = va_arg(ap, unsigned long long);
    void *p1 = (void *)(uintptr_t)va_arg(ap, unsigned long long);
    void *p2 = (void *)(uintptr_t)va_arg(ap, unsigned long long);
    unsigned short *p3 = (unsigned short *)(uintptr_t)va_arg(ap, unsigned long long);
    void *p4 = (void *)(uintptr_t)va_arg(ap, unsigned long long);
    const ATRDEF *atr = (const ATRDEF *)(uintptr_t)va_arg(ap, unsigned long long);
    unsigned fcode = func & IO$M_FCODE;
    if ((fcode != IO$_ACCESS && fcode != IO$_DEACCESS) ||
        (func & (IO$M_CREATE | IO$M_DELETE)))
        return SS$_ILLIOFUNC;
    unsigned n_atr = atr_count(atr);
    unsigned short st;
    if (fcode == IO$_DEACCESS) {
        /* Writing attributes back on IO$_DEACCESS is not implemented: an
         * attribute list is refused for a file accessed for write, never
         * accepted and dropped. */
        if (n_atr && (chan >> 4) < ABI_MAXCHAN && chan_write[chan >> 4])
            st = SS$_BADATTRIB;
        else
            st = (unsigned short)ovmx_vmsabi_acp_deaccess(chan);
        if ((chan >> 4) < ABI_MAXCHAN)
            chan_write[chan >> 4] = 0;
    } else {
        const char *fibp, *name = "";
        unsigned fiblen, namelen = 0;
        unsigned char fib[16];
        if (!p1 || !dsc_string(p1, &fibp, &fiblen) || !fibp)
            return SS$_BADPARAM;
        memset(fib, 0, sizeof fib);
        memcpy(fib, fibp, fiblen < sizeof fib ? fiblen : sizeof fib);
        if (p2 && !dsc_string(p2, &name, &namelen))
            return SS$_BADPARAM;
        unsigned i;
        for (i = 0; i < n_atr; i++)
            if (!atr_known(atr[i].atr$w_type))
                break;
        if (i < n_atr) {
            st = SS$_BADATTRIB;
        } else {
            unsigned acctl;
            unsigned short fid[3], did[3];
            memcpy(&acctl, fib, 4);
            memcpy(fid, fib + 4, 6);
            memcpy(did, fib + 10, 6);
            struct ovmx_abi_fileattr a;
            int keep = (func & IO$M_ACCESS) != 0;
            st = (unsigned short)ovmx_vmsabi_acp_access(chan, acctl, did, fid, name, namelen,
                                                        keep, &a);
            if (st & 1) {
                if (fiblen >= 10)
                    memcpy((char *)fibp + 4, a.fid, 6);
                for (i = 0; i < n_atr; i++)
                    atr_fill(&atr[i], &a);
                if (p4) {
                    const char *rp;
                    unsigned rl;
                    if (dsc_string(p4, &rp, &rl) && rp) {
                        unsigned n = a.namelen < rl ? a.namelen : rl;
                        memcpy((char *)rp, a.name, n);
                        if (p3)
                            *p3 = (unsigned short)n;
                    }
                }
                if (keep && (chan >> 4) < ABI_MAXCHAN)
                    chan_write[chan >> 4] = (acctl & 256u) != 0;   /* FIB$M_WRITE */
            }
        }
    }
    io_done(iosb, st);
    ovmx_vmsabi_io_complete(efn, (void (*)(unsigned long long))astadr, astprm);
    return SS$_NORMAL;
}

/* Every other function (a terminal or mailbox read/write, a device
 * function...): P1-P6 are values or buffer addresses, not descriptors, so the
 * request goes to the same $QIO OVMX's own code calls, unchanged (vms-3b3f). */
static int is_acp_function(unsigned int func)
{
    unsigned fcode = func & IO$M_FCODE;
    return fcode == IO$_ACCESS || fcode == IO$_DEACCESS;
}

static int dev_qio(int wait, unsigned int efn, unsigned short chan, unsigned int func,
                   void *iosb, va_list ap)
{
    unsigned long long astadr = va_arg(ap, unsigned long long);
    unsigned long long astprm = va_arg(ap, unsigned long long);
    unsigned long long p[6];
    for (int i = 0; i < 6; i++)
        p[i] = va_arg(ap, unsigned long long);
    return (int)ovmx_vmsabi_qio(wait, efn, chan, func, iosb, astadr, astprm, p);
}

int SYS$QIOW(unsigned int efn, unsigned short chan, unsigned int func, void *iosb, ...)
{
    va_list ap;
    va_start(ap, iosb);
    int st = is_acp_function(func) ? acp_qio(efn, chan, func, iosb, ap)
                                   : dev_qio(1, efn, chan, func, iosb, ap);
    va_end(ap);
    return st;
}

int SYS$QIO(unsigned int efn, unsigned short chan, unsigned int func, void *iosb, ...)
{
    va_list ap;
    va_start(ap, iosb);
    int st = is_acp_function(func) ? acp_qio(efn, chan, func, iosb, ap)
                                   : dev_qio(0, efn, chan, func, iosb, ap);
    va_end(ap);
    return st;
}

/* ------------------------------------------ LIB$PUT_OUTPUT (LIBRTL) ------ */

/* The message is a VMS descriptor, either form (vms-3b3f). */
int LIB$PUT_OUTPUT(void *message)
{
    const char *p;
    unsigned len;
    if (!dsc_string(message, &p, &len) || !p)
        return SS$_BADPARAM;
    return (int)ovmx_vmsabi_put_output(p, len);
}

/* ---------------------------------------------------- SYS$IMGSTA ------ */

/*
 * SYS$IMGSTA -- the first transfer address of every image LINKed with
 * traceback (its EIHD transfer vector names it in SYS$PUBLIC_VECTORS). The
 * activator calls it with the six-argument activation list whose first
 * argument is the transfer vector (procedure values, zero-terminated, this
 * routine first); it calls the next transfer address with the same list, as
 * LIB$INITIALIZE does (src/vmslink/starlet/lib_initialize.c): the remaining
 * vector when another entry follows, else the main program's own procedure
 * value. VMS also establishes the traceback condition handler here; OVMX's
 * condition handling has no traceback handler to establish yet, so an
 * unhandled condition is reported by OVMX's default handling instead (the
 * compat register records SYS$IMGSTA as partial).
 */
typedef int (*vmsabi_xfer_fn)(void *, void *, void *, void *, unsigned int,
                              unsigned int);

int SYS$IMGSTA(void *xfervec, void *cli_util, void *imghdr, void *imgfile,
               unsigned int linkflag, unsigned int cliflag)
{
    unsigned long long *vec = xfervec;
    if (!vec || !vec[1])
        return SS$_BADPARAM;
    vmsabi_xfer_fn next = (vmsabi_xfer_fn)vec[1];
    void *a0 = vec[2] ? (void *)&vec[1] : (void *)vec[1];
    return next(a0, cli_util, imghdr, imgfile, linkflag, cliflag);
}

/* ------------------------------------------- descriptor services (vms-3b3f) */

/* A descriptor's buffer for the service to write into (either form). */
static int dsc_buffer(void *d, char **p, unsigned *len)
{
    const char *cp;
    if (!dsc_string(d, &cp, len) || !cp)
        return 0;
    *p = (char *)cp;
    return 1;
}

int SYS$ASCEFC(unsigned int efn, void *name, unsigned int prot, unsigned int perm)
{
    const char *p;
    unsigned len;
    if (!dsc_string(name, &p, &len))
        return SS$_BADPARAM;
    return (int)ovmx_vmsabi_ascefc(efn, p, len, prot, perm);
}

int SYS$DLCEFC(void *name)
{
    const char *p;
    unsigned len;
    if (!dsc_string(name, &p, &len))
        return SS$_BADPARAM;
    return (int)ovmx_vmsabi_dlcefc(p, len);
}

int SYS$BINTIM(void *timbuf, void *timadr)
{
    const char *p;
    unsigned len;
    if (!dsc_string(timbuf, &p, &len))
        return SS$_BADPARAM;
    return (int)ovmx_vmsabi_bintim(p, len, timadr);
}

int SYS$ASCTIM(unsigned short *timlen, void *timbuf, void *timadr, unsigned int cvtflg)
{
    char *p;
    unsigned cap;
    if (!dsc_buffer(timbuf, &p, &cap))
        return SS$_BADPARAM;
    return (int)ovmx_vmsabi_asctim(timlen, p, cap, timadr, cvtflg);
}

int SYS$GETMSG(unsigned int msgid, unsigned short *msglen, void *bufadr,
               unsigned int flags, unsigned char *outadr)
{
    char *p;
    unsigned cap;
    if (!dsc_buffer(bufadr, &p, &cap))
        return SS$_BADPARAM;
    return (int)ovmx_vmsabi_getmsg(msgid, msglen, p, cap, flags, outadr);
}

/* $FAOL: the parameter list is LONGWORDS (a 32-bit caller's values and
 * addresses); each is sign-extended as the Alpha loads it. An !AS parameter
 * names a VMS-form descriptor (ovmx_fao_vmsabi). */
#define FAO_MAXPRM 256
int SYS$FAOL(void *ctrstr, unsigned short *outlen, void *outbuf, int *prmlst)
{
    const char *c;
    char *o;
    unsigned clen, ocap;
    if (!dsc_string(ctrstr, &c, &clen) || !dsc_buffer(outbuf, &o, &ocap))
        return SS$_BADPARAM;
    int n = count_fao_args(c, (unsigned short)clen);
    if (n > FAO_MAXPRM)
        n = FAO_MAXPRM;
    unsigned long long prm[FAO_MAXPRM];
    for (int i = 0; i < n; i++)
        prm[i] = prmlst ? (unsigned long long)(long long)prmlst[i] : 0;
    return (int)ovmx_fao_vmsabi(c, clen, outlen, o, ocap,
                                (const uint64_t *)prm);
}

/* $FAO: the parameters follow in the argument list, one per quadword slot. */
int SYS$FAO(void *ctrstr, unsigned short *outlen, void *outbuf, ...)
{
    const char *c;
    char *o;
    unsigned clen, ocap;
    if (!dsc_string(ctrstr, &c, &clen) || !dsc_buffer(outbuf, &o, &ocap))
        return SS$_BADPARAM;
    int n = count_fao_args(c, (unsigned short)clen);
    if (n > FAO_MAXPRM)
        n = FAO_MAXPRM;
    unsigned long long prm[FAO_MAXPRM];
    va_list ap;
    va_start(ap, outbuf);
    for (int i = 0; i < n; i++)
        prm[i] = va_arg(ap, unsigned long long);
    va_end(ap);
    return (int)ovmx_fao_vmsabi(c, clen, outlen, o, ocap, (const uint64_t *)prm);
}

/* ------------------------------------------------ vms-3b3f batch 2 -------- */

/* An optional string argument (a VMS descriptor of either form, or 0). */
static void str_arg(void *d, struct ovmx_abi_str *s)
{
    s->p = NULL;
    s->len = 0;
    s->given = d && dsc_string(d, &s->p, &s->len);
}

int SYS$DELLNM(void *tabnam, void *lognam, unsigned char *acmode)
{
    struct ovmx_abi_str t, l;
    str_arg(tabnam, &t);
    str_arg(lognam, &l);
    return (int)ovmx_vmsabi_dellnm(&t, &l, acmode);
}

int SYS$CREMBX(unsigned int prmflg, unsigned short *chan, unsigned int maxmsg,
               unsigned int bufquo, unsigned int promsk, unsigned int acmode,
               void *lognam, unsigned int flags)
{
    struct ovmx_abi_str l;
    str_arg(lognam, &l);
    return (int)ovmx_vmsabi_crembx((int)prmflg, chan, maxmsg, bufquo, promsk, acmode, &l, flags);
}

#define ITEMS_OR_BAD(list, items, n) \
    struct ovmx_abi_item items[ABI_MAXITEMS]; \
    int n = items_in(list, items, ABI_MAXITEMS); \
    if (n < 0) return SS$_BADPARAM

static int getjpi(int wait, unsigned int efn, unsigned int *pidadr, void *prcnam,
                  void *itmlst, void *iosb, unsigned long long astadr,
                  unsigned long long astprm)
{
    struct ovmx_abi_str p;
    str_arg(prcnam, &p);
    ITEMS_OR_BAD(itmlst, items, n);
    return (int)ovmx_vmsabi_getjpi(wait, efn, pidadr, &p, items, (unsigned)n, iosb, astadr, astprm);
}
int SYS$GETJPI(unsigned int efn, unsigned int *pidadr, void *prcnam, void *itmlst,
               void *iosb, unsigned long long astadr, unsigned long long astprm)
{ return getjpi(0, efn, pidadr, prcnam, itmlst, iosb, astadr, astprm); }
int SYS$GETJPIW(unsigned int efn, unsigned int *pidadr, void *prcnam, void *itmlst,
                void *iosb, unsigned long long astadr, unsigned long long astprm)
{ return getjpi(1, efn, pidadr, prcnam, itmlst, iosb, astadr, astprm); }

static int getsyi(int wait, unsigned int efn, unsigned int *csidadr, void *node,
                  void *itmlst, void *iosb, unsigned long long astadr,
                  unsigned long long astprm)
{
    struct ovmx_abi_str d;
    str_arg(node, &d);
    ITEMS_OR_BAD(itmlst, items, n);
    return (int)ovmx_vmsabi_getsyi(wait, efn, csidadr, &d, items, (unsigned)n, iosb, astadr, astprm);
}
int SYS$GETSYI(unsigned int efn, unsigned int *csidadr, void *node, void *itmlst,
               void *iosb, unsigned long long astadr, unsigned long long astprm)
{ return getsyi(0, efn, csidadr, node, itmlst, iosb, astadr, astprm); }
int SYS$GETSYIW(unsigned int efn, unsigned int *csidadr, void *node, void *itmlst,
                void *iosb, unsigned long long astadr, unsigned long long astprm)
{ return getsyi(1, efn, csidadr, node, itmlst, iosb, astadr, astprm); }

static int getdvi(int wait, unsigned int efn, unsigned short chan, void *devnam,
                  void *itmlst, void *iosb, unsigned long long astadr,
                  unsigned long long astprm)
{
    struct ovmx_abi_str d;
    str_arg(devnam, &d);
    ITEMS_OR_BAD(itmlst, items, n);
    return (int)ovmx_vmsabi_getdvi(wait, efn, chan, &d, items, (unsigned)n, iosb, astadr, astprm);
}
int SYS$GETDVI(unsigned int efn, unsigned short chan, void *devnam, void *itmlst,
               void *iosb, unsigned long long astadr, unsigned long long astprm)
{ return getdvi(0, efn, chan, devnam, itmlst, iosb, astadr, astprm); }
int SYS$GETDVIW(unsigned int efn, unsigned short chan, void *devnam, void *itmlst,
                void *iosb, unsigned long long astadr, unsigned long long astprm)
{ return getdvi(1, efn, chan, devnam, itmlst, iosb, astadr, astprm); }

static int enq(int wait, unsigned int efn, unsigned int lkmode, void *lksb, unsigned int flags,
               void *resnam, unsigned int parid, unsigned long long astadr,
               unsigned long long astprm, unsigned long long blkast, unsigned int acmode,
               unsigned int rsdm)
{
    struct ovmx_abi_str r;
    str_arg(resnam, &r);
    return (int)ovmx_vmsabi_enq(wait, efn, lkmode, lksb, flags, &r, parid, astadr, astprm,
                                blkast, acmode, rsdm);
}
int SYS$ENQ(unsigned int efn, unsigned int lkmode, void *lksb, unsigned int flags,
            void *resnam, unsigned int parid, unsigned long long astadr,
            unsigned long long astprm, unsigned long long blkast, unsigned int acmode,
            unsigned int rsdm)
{ return enq(0, efn, lkmode, lksb, flags, resnam, parid, astadr, astprm, blkast, acmode, rsdm); }
int SYS$ENQW(unsigned int efn, unsigned int lkmode, void *lksb, unsigned int flags,
             void *resnam, unsigned int parid, unsigned long long astadr,
             unsigned long long astprm, unsigned long long blkast, unsigned int acmode,
             unsigned int rsdm)
{ return enq(1, efn, lkmode, lksb, flags, resnam, parid, astadr, astprm, blkast, acmode, rsdm); }

static int prc(int op, unsigned int *pidadr, void *prcnam, unsigned int arg)
{
    struct ovmx_abi_str p;
    str_arg(prcnam, &p);
    return (int)ovmx_vmsabi_prc(op, pidadr, &p, arg);
}
int SYS$WAKE(unsigned int *pidadr, void *prcnam)
{ return prc(OVMX_ABI_PRC_WAKE, pidadr, prcnam, 0); }
int SYS$RESUME(unsigned int *pidadr, void *prcnam)
{ return prc(OVMX_ABI_PRC_RESUME, pidadr, prcnam, 0); }
int SYS$SUSPND(unsigned int *pidadr, void *prcnam, unsigned int flags)
{ return prc(OVMX_ABI_PRC_SUSPND, pidadr, prcnam, flags); }
int SYS$FORCEX(unsigned int *pidadr, void *prcnam, unsigned int code)
{ return prc(OVMX_ABI_PRC_FORCEX, pidadr, prcnam, code); }
int SYS$DELPRC(unsigned int *pidadr, void *prcnam)
{ return prc(OVMX_ABI_PRC_DELPRC, pidadr, prcnam, 0); }

int SYS$SETPRI(unsigned int *pidadr, void *prcnam, unsigned int pri, unsigned int *prvpri,
               unsigned int pol, unsigned int *prevpol)
{
    struct ovmx_abi_str p;
    str_arg(prcnam, &p);
    return (int)ovmx_vmsabi_setpri(pidadr, &p, pri, prvpri, pol, prevpol);
}

int SYS$ASCTOID(void *name, unsigned int *id, unsigned int *attrib)
{
    struct ovmx_abi_str n;
    str_arg(name, &n);
    return (int)ovmx_vmsabi_asctoid(&n, id, attrib);
}

int SYS$IDTOASC(unsigned int id, unsigned short *namlen, void *nambuf, unsigned int *resid,
                unsigned int *attrib, unsigned int *ctx)
{
    char *p = NULL;
    unsigned cap = 0;
    if (nambuf && !dsc_buffer(nambuf, &p, &cap))
        return SS$_BADPARAM;
    return (int)ovmx_vmsabi_idtoasc(id, namlen, p, cap, resid, attrib, ctx);
}

static int grant(int revoke, unsigned int *pidadr, void *prcnam, unsigned int *id,
                 void *name, unsigned int *prvatr, unsigned int segment)
{
    struct ovmx_abi_str p, n;
    str_arg(prcnam, &p);
    str_arg(name, &n);
    return (int)ovmx_vmsabi_grantid(revoke, pidadr, &p, id, &n, prvatr, segment);
}
int SYS$GRANTID(unsigned int *pidadr, void *prcnam, unsigned int *id, void *name,
                unsigned int *prvatr, unsigned int segment)
{ return grant(0, pidadr, prcnam, id, name, prvatr, segment); }
int SYS$REVOKID(unsigned int *pidadr, void *prcnam, unsigned int *id, void *name,
                unsigned int *prvatr, unsigned int segment)
{ return grant(1, pidadr, prcnam, id, name, prvatr, segment); }

int SYS$SNDOPR(void *msgbuf, unsigned short chan)
{
    struct ovmx_abi_str m;
    str_arg(msgbuf, &m);
    return (int)ovmx_vmsabi_sndopr(&m, chan);
}

int SYS$BRKTHRUW(unsigned int efn, void *msgbuf, void *sendto, unsigned int sndtyp,
                 void *iosb, unsigned int carcon, unsigned int flags, unsigned int reqid,
                 unsigned int timout, unsigned long long astadr, unsigned long long astprm)
{
    struct ovmx_abi_str m, t;
    str_arg(msgbuf, &m);
    str_arg(sendto, &t);
    return (int)ovmx_vmsabi_brkthru(1, efn, &m, &t, sndtyp, iosb, carcon, flags, reqid, timout,
                                    astadr, astprm);
}

/* ======================================== vms-3b3f batch 3 (LIBRTL) ======
 *
 * LIB$ and STR$ routines by their VMS-ABI names. A caller LINKed on OpenVMS
 * passes a counted argument list and may omit trailing optional arguments, so
 * each routine with optional arguments takes the count from the argument
 * information (R25, homed by OTS$HOME_ARGS -- DEC C's va_count; the same read
 * src/vmsrms/crtl_rms_fd.c makes) and reads only the arguments passed. A
 * descriptor is either form; a class-D result gets storage below 2 GB from the
 * VMS-ABI heap (sys_vmsabi_core.c), and its new length and address are
 * written back into the caller's descriptor.
 */
/* The argument count: OTS$HOME_ARGS stores the argument information at home[0],
 * `named` + 1 quadwords below where va_start points. The empty asm keeps the
 * address opaque to the port compiler's stdarg pass, which otherwise fails on
 * the negative offset (an internal compiler error, vms-45f). */
static inline unsigned abi_va_count(const void *apv, int named)
{
    const unsigned long long *p = apv;
    __asm__("" : "+r"(p));
    return (unsigned)(p[-(named + 1)] & 0xFF);
}
#define ABI_VA_COUNT(ap, named) abi_va_count((const void *)(ap), (named))

/* Read a descriptor (either form) into the neutral form; given=0 for 0. */
static void dx_get(void *d, struct ovmx_abi_dx *x)
{
    memset(x, 0, sizeof *x);
    if (!d)
        return;
    const struct dsc64 *d64 = d;
    x->given = 1;
    if (d64->mbo == 1 && d64->mbmo == -1) {
        x->len = (uint16_t)d64->len;
        x->dtype = d64->dtype;
        x->cls = d64->cls;
        x->ptr = (char *)(uintptr_t)d64->ptr;
        return;
    }
    const struct dsc$descriptor_s *d32 = d;
    x->len = d32->dsc$w_length;
    x->dtype = d32->dsc$b_dtype;
    x->cls = d32->dsc$b_class;
    x->ptr = d32->dsc$a_pointer;
}

/* Write a class-D result's length and address back into the caller's
 * descriptor (a fixed-length destination keeps its own). */
static void dx_put(void *d, const struct ovmx_abi_dx *x)
{
    if (!d || !x->given || x->cls != DSC$K_CLASS_D)
        return;
    struct dsc64 *d64 = d;
    if (d64->mbo == 1 && d64->mbmo == -1) {
        d64->len = x->len;
        d64->ptr = (unsigned long long)(uintptr_t)x->ptr;
        return;
    }
    struct dsc$descriptor_s *d32 = d;
    d32->dsc$w_length = x->len;
    d32->dsc$a_pointer = x->ptr;
}

/* Take argument i (0-based, counting the named ones) into var when the caller
 * passed it; var keeps its 0 otherwise. A statement, not an expression: the
 * port compiler's stdarg pass cannot take va_arg inside a conditional
 * expression (vms-45f). */
#define ABI_OPT(ap, have, i, var) \
    do { if ((i) < (have)) (var) = va_arg(ap, void *); } while (0)

static int str_dst(int op, void *dst, void *src)
{
    struct ovmx_abi_dx d, s;
    dx_get(dst, &d);
    dx_get(src, &s);
    int st = (int)ovmx_vmsabi_str_dst(op, &d, &s);
    dx_put(dst, &d);
    return st;
}
int STR$COPY_DX(void *dst, void *src) { return str_dst(OVMX_ABI_STR_COPY_DX, dst, src); }
int STR$APPEND(void *dst, void *src)  { return str_dst(OVMX_ABI_STR_APPEND, dst, src); }
int STR$PREFIX(void *dst, void *src)  { return str_dst(OVMX_ABI_STR_PREFIX, dst, src); }
int STR$UPCASE(void *dst, void *src)  { return str_dst(OVMX_ABI_STR_UPCASE, dst, src); }
int LIB$SCOPY_DXDX(void *src, void *dst) { return str_dst(OVMX_ABI_STR_COPY_DX, dst, src); }

static int str_extract(int op, void *dst, void *src, void *a, void *b)
{
    struct ovmx_abi_dx d, s;
    dx_get(dst, &d);
    dx_get(src, &s);
    int st = (int)ovmx_vmsabi_str_extract(op, &d, &s, a, b);
    dx_put(dst, &d);
    return st;
}
int STR$LEFT(void *dst, void *src, int *end)  { return str_extract(OVMX_ABI_STR_LEFT, dst, src, end, 0); }
int STR$RIGHT(void *dst, void *src, int *beg) { return str_extract(OVMX_ABI_STR_RIGHT, dst, src, beg, 0); }
int STR$LEN_EXTR(void *dst, void *src, int *start, int *len)
{ return str_extract(OVMX_ABI_STR_LEN_EXTR, dst, src, start, len); }
int STR$POS_EXTR(void *dst, void *src, int *start, int *end)
{ return str_extract(OVMX_ABI_STR_POS_EXTR, dst, src, start, end); }

int STR$REPLACE(void *dst, void *src, unsigned int *start, unsigned int *end, void *rep)
{
    struct ovmx_abi_dx d, s, r;
    dx_get(dst, &d);
    dx_get(src, &s);
    dx_get(rep, &r);
    int st = (int)ovmx_vmsabi_str_replace(&d, &s, start, end, &r);
    dx_put(dst, &d);
    return st;
}

int STR$TRANSLATE(void *dst, void *src, void *tran, void *match)
{
    struct ovmx_abi_dx d, s, t, m;
    dx_get(dst, &d);
    dx_get(src, &s);
    dx_get(tran, &t);
    dx_get(match, &m);
    int st = (int)ovmx_vmsabi_str_translate(&d, &s, &t, &m);
    dx_put(dst, &d);
    return st;
}

int STR$TRIM(void *dst, void *src, ...)
{
    va_list ap;
    va_start(ap, src);
    unsigned n = ABI_VA_COUNT(ap, 2);
    unsigned short *outlen = 0;
    ABI_OPT(ap, n, 2u, outlen);
    va_end(ap);
    struct ovmx_abi_dx d, s;
    dx_get(dst, &d);
    dx_get(src, &s);
    int st = (int)ovmx_vmsabi_str_trim(&d, &s, outlen);
    dx_put(dst, &d);
    return st;
}

int STR$DUPL_CHAR(void *dst, ...)
{
    static const int one = 1;
    static const char blank = ' ';
    va_list ap;
    va_start(ap, dst);
    unsigned n = ABI_VA_COUNT(ap, 1);
    const int *len = 0;
    ABI_OPT(ap, n, 1u, len);
    const char *ch = 0;
    ABI_OPT(ap, n, 2u, ch);
    va_end(ap);
    struct ovmx_abi_dx d;
    dx_get(dst, &d);
    int st = (int)ovmx_vmsabi_str_dupl_char(&d, len ? len : &one, ch ? ch : &blank);
    dx_put(dst, &d);
    return st;
}

int STR$ELEMENT(void *dst, unsigned int *elem, void *delim, void *src)
{
    struct ovmx_abi_dx d, l, s;
    dx_get(dst, &d);
    dx_get(delim, &l);
    dx_get(src, &s);
    int st = (int)ovmx_vmsabi_str_element(&d, elem, &l, &s);
    dx_put(dst, &d);
    return st;
}

#define ABI_CONCAT_MAX 16
int STR$CONCAT(void *dst, ...)
{
    struct ovmx_abi_dx d, s[ABI_CONCAT_MAX];
    va_list ap;
    va_start(ap, dst);
    unsigned n = ABI_VA_COUNT(ap, 1);
    unsigned srcs = n > 1 ? n - 1 : 0;
    if (srcs > ABI_CONCAT_MAX) {
        va_end(ap);
        return SS$_BADPARAM;
    }
    for (unsigned i = 0; i < srcs; i++)
        dx_get(va_arg(ap, void *), &s[i]);
    va_end(ap);
    dx_get(dst, &d);
    int st = (int)ovmx_vmsabi_str_concat(&d, s, srcs);
    dx_put(dst, &d);
    return st;
}

int STR$FREE1_DX(void *dst)
{
    struct ovmx_abi_dx d;
    dx_get(dst, &d);
    int st = (int)ovmx_vmsabi_str_free1(&d);
    dx_put(dst, &d);
    return st;
}

static int str_in(int op, void *a, void *b, unsigned int *start)
{
    struct ovmx_abi_dx x, y;
    dx_get(a, &x);
    dx_get(b, &y);
    return (int)ovmx_vmsabi_str_in(op, &x, &y, start);
}
int STR$COMPARE(void *a, void *b)     { return str_in(OVMX_ABI_CMP_COMPARE, a, b, 0); }
int STR$COMPARE_EQL(void *a, void *b) { return str_in(OVMX_ABI_CMP_COMPARE_EQL, a, b, 0); }
int STR$FIND_FIRST_IN_SET(void *a, void *b)     { return str_in(OVMX_ABI_CMP_FFIS, a, b, 0); }
int STR$FIND_FIRST_NOT_IN_SET(void *a, void *b) { return str_in(OVMX_ABI_CMP_FFNIS, a, b, 0); }
int LIB$INDEX(void *a, void *b)  { return str_in(OVMX_ABI_CMP_INDEX, a, b, 0); }
int LIB$LOCC(void *a, void *b)   { return str_in(OVMX_ABI_CMP_LOCC, a, b, 0); }
int LIB$MATCHC(void *a, void *b) { return str_in(OVMX_ABI_CMP_MATCHC, a, b, 0); }
int LIB$SKPC(void *a, void *b)   { return str_in(OVMX_ABI_CMP_SKPC, a, b, 0); }
int STR$POSITION(void *src, void *sub, ...)
{
    va_list ap;
    va_start(ap, sub);
    unsigned n = ABI_VA_COUNT(ap, 2);
    unsigned int *start = 0;
    ABI_OPT(ap, n, 2u, start);
    va_end(ap);
    return str_in(OVMX_ABI_CMP_POSITION, src, sub, start);
}

int LIB$SET_SYMBOL(void *sym, void *val, ...)
{
    va_list ap;
    va_start(ap, val);
    unsigned n = ABI_VA_COUNT(ap, 2);
    unsigned int *tbl = 0;
    ABI_OPT(ap, n, 2u, tbl);
    va_end(ap);
    struct ovmx_abi_dx s, v;
    dx_get(sym, &s);
    dx_get(val, &v);
    return (int)ovmx_vmsabi_set_symbol(&s, &v, tbl);
}

int LIB$GET_SYMBOL(void *sym, void *val, ...)
{
    va_list ap;
    va_start(ap, val);
    unsigned n = ABI_VA_COUNT(ap, 2);
    unsigned short *len = 0;
    ABI_OPT(ap, n, 2u, len);
    unsigned int *tbl = 0;
    ABI_OPT(ap, n, 3u, tbl);
    va_end(ap);
    struct ovmx_abi_dx s, v;
    dx_get(sym, &s);
    dx_get(val, &v);
    int st = (int)ovmx_vmsabi_get_symbol(&s, &v, len, tbl);
    dx_put(val, &v);
    return st;
}

int LIB$DELETE_SYMBOL(void *sym, ...)
{
    va_list ap;
    va_start(ap, sym);
    unsigned n = ABI_VA_COUNT(ap, 1);
    unsigned int *tbl = 0;
    ABI_OPT(ap, n, 1u, tbl);
    va_end(ap);
    struct ovmx_abi_dx s;
    dx_get(sym, &s);
    return (int)ovmx_vmsabi_delete_symbol(&s, tbl);
}

int LIB$FIND_FILE(void *spec, void *result, unsigned int *ctx, ...)
{
    va_list ap;
    va_start(ap, ctx);
    unsigned n = ABI_VA_COUNT(ap, 3);
    void *def = 0;
    ABI_OPT(ap, n, 3u, def);
    void *rel = 0;
    ABI_OPT(ap, n, 4u, rel);
    unsigned int *stv = 0;
    ABI_OPT(ap, n, 5u, stv);
    unsigned int *flags = 0;
    ABI_OPT(ap, n, 6u, flags);
    va_end(ap);
    struct ovmx_abi_dx f, r, d, l;
    dx_get(spec, &f);
    dx_get(result, &r);
    dx_get(def, &d);
    dx_get(rel, &l);
    int st = (int)ovmx_vmsabi_find_file(&f, &r, ctx, &d, &l, stv, flags);
    dx_put(result, &r);
    return st;
}

int LIB$GETJPI(unsigned int *item, ...)
{
    va_list ap;
    va_start(ap, item);
    unsigned n = ABI_VA_COUNT(ap, 1);
    unsigned int *pid = 0;
    ABI_OPT(ap, n, 1u, pid);
    void *prcnam = 0;
    ABI_OPT(ap, n, 2u, prcnam);
    void *resval = 0;
    ABI_OPT(ap, n, 3u, resval);
    void *resstr = 0;
    ABI_OPT(ap, n, 4u, resstr);
    unsigned short *reslen = 0;
    ABI_OPT(ap, n, 5u, reslen);
    va_end(ap);
    struct ovmx_abi_dx p, r;
    dx_get(prcnam, &p);
    dx_get(resstr, &r);
    int st = (int)ovmx_vmsabi_lib_getxxi(OVMX_ABI_GETXXI_JPI, item, pid, 0, &p, resval, &r,
                                         reslen, 0);
    dx_put(resstr, &r);
    return st;
}

int LIB$GETSYI(unsigned int *item, ...)
{
    va_list ap;
    va_start(ap, item);
    unsigned n = ABI_VA_COUNT(ap, 1);
    void *resval = 0;
    ABI_OPT(ap, n, 1u, resval);
    void *resstr = 0;
    ABI_OPT(ap, n, 2u, resstr);
    unsigned short *reslen = 0;
    ABI_OPT(ap, n, 3u, reslen);
    unsigned int *csid = 0;
    ABI_OPT(ap, n, 4u, csid);
    void *node = 0;
    ABI_OPT(ap, n, 5u, node);
    va_end(ap);
    struct ovmx_abi_dx nd, r;
    dx_get(node, &nd);
    dx_get(resstr, &r);
    int st = (int)ovmx_vmsabi_lib_getxxi(OVMX_ABI_GETXXI_SYI, item, 0, 0, &nd, resval, &r,
                                         reslen, csid);
    dx_put(resstr, &r);
    return st;
}

/* LIB$GETDVI's channel is a word passed by reference. */
int LIB$GETDVI(unsigned int *item, ...)
{
    va_list ap;
    va_start(ap, item);
    unsigned n = ABI_VA_COUNT(ap, 1);
    unsigned short *chan = 0;
    ABI_OPT(ap, n, 1u, chan);
    void *devnam = 0;
    ABI_OPT(ap, n, 2u, devnam);
    void *resval = 0;
    ABI_OPT(ap, n, 3u, resval);
    void *resstr = 0;
    ABI_OPT(ap, n, 4u, resstr);
    unsigned short *reslen = 0;
    ABI_OPT(ap, n, 5u, reslen);
    va_end(ap);
    struct ovmx_abi_dx d, r;
    dx_get(devnam, &d);
    dx_get(resstr, &r);
    int st = (int)ovmx_vmsabi_lib_getxxi(OVMX_ABI_GETXXI_DVI, item, 0, chan ? *chan : 0, &d,
                                         resval, &r, reslen, 0);
    dx_put(resstr, &r);
    return st;
}

/* LIB$SYS_FAO: the $FAO parameters follow in the argument list. */
int LIB$SYS_FAO(void *ctrstr, unsigned short *outlen, void *outbuf, ...)
{
    struct ovmx_abi_dx c, o;
    unsigned long long prm[FAO_MAXPRM];
    va_list ap;
    va_start(ap, outbuf);
    unsigned n = ABI_VA_COUNT(ap, 3);
    unsigned np = n > 3 ? n - 3 : 0;
    if (np > FAO_MAXPRM)
        np = FAO_MAXPRM;
    memset(prm, 0, sizeof prm);
    for (unsigned i = 0; i < np; i++)
        prm[i] = va_arg(ap, unsigned long long);
    va_end(ap);
    dx_get(ctrstr, &c);
    dx_get(outbuf, &o);
    int st = (int)ovmx_vmsabi_sys_fao(&c, outlen, &o, (const uint64_t *)prm);
    dx_put(outbuf, &o);
    return st;
}

/* LIB$GET_VM / LIB$FREE_VM: the base address is a longword. The default zone
 * is the VMS-ABI heap; a zone argument naming another zone is refused. */
int LIB$GET_VM(int *nbytes, unsigned int *base, ...)
{
    va_list ap;
    va_start(ap, base);
    unsigned n = ABI_VA_COUNT(ap, 2);
    unsigned int *zone = 0;
    ABI_OPT(ap, n, 2u, zone);
    va_end(ap);
    if (!nbytes || !base)
        return SS$_BADPARAM;
    if (zone && *zone)
        return SS$_UNSUPPORTED;         /* only the default zone is carried */
    if (*nbytes <= 0)
        return LIB$_BADBLOSIZ;
    void *p = ovmx_vmsabi_p0_alloc((unsigned)*nbytes);
    if (!p)
        return LIB$_INSVIRMEM;
    *base = (unsigned int)(uintptr_t)p;
    return SS$_NORMAL;
}

int LIB$FREE_VM(int *nbytes, unsigned int *base, ...)
{
    va_list ap;
    va_start(ap, base);
    unsigned n = ABI_VA_COUNT(ap, 2);
    unsigned int *zone = 0;
    ABI_OPT(ap, n, 2u, zone);
    va_end(ap);
    if (!nbytes || !base)
        return SS$_BADPARAM;
    if (zone && *zone)
        return SS$_UNSUPPORTED;         /* only the default zone is carried */
    if (*nbytes <= 0)
        return LIB$_BADBLOSIZ;
    if (!ovmx_vmsabi_p0_free((void *)(uintptr_t)*base))
        return LIB$_BADBLOADR;
    return SS$_NORMAL;
}

/* ------------------------------------------------------- SYS$CREPRC ----- */
int SYS$CREPRC(unsigned int *pidadr, void *image, void *input, void *output, void *error,
               void *prvadr, void *quota, void *prcnam, unsigned int baspri, unsigned int uic,
               unsigned short mbxunt, unsigned int stsflg, void *itmlst, void *node,
               unsigned int home_rad)
{
    struct ovmx_abi_str i, in, out, err, p, nd;
    (void)home_rad;
    if (itmlst)
        return SS$_UNSUPPORTED;         /* the item-list form is not carried yet */
    str_arg(image, &i);
    str_arg(input, &in);
    str_arg(output, &out);
    str_arg(error, &err);
    str_arg(prcnam, &p);
    str_arg(node, &nd);
    return (int)ovmx_vmsabi_creprc(pidadr, &i, &in, &out, &err, prvadr, quota, &p, baspri, uic,
                                   mbxunt, stsflg, &nd);
}
