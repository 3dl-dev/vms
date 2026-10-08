/*
 * sys_vmsabi.c - system services by their upper-case (VMS-ABI) names, taking
 * the VMS argument forms (vms-38b): SYS$ASSIGN, SYS$DASSGN, SYS$TRNLNM,
 * SYS$CRELNM.
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

#include <vms/descrip.h>
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
