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
