/*
 * sys_vmsabi_core.c - the OVMX-service half of the VMS-ABI system services
 * (vms-38b): rebuild OVMX's native descriptors and item lists from the plain
 * values sys_vmsabi.c extracted from the caller's VMS forms, and run the same
 * services OVMX's own code calls ($ASSIGN, $DASSGN, $TRNLNM, $CRELNM).
 */
#include <stdlib.h>
#include <string.h>

#include "descrip.h"
#include "lnmdef.h"
#include "ssdef.h"
#include "starlet.h"
#include "sys_vmsabi_core.h"

static void mkdsc(struct dsc$descriptor_s *d, const char *p, unsigned len)
{
    d->dsc$w_length = (uint16_t)len;
    d->dsc$b_dtype = DSC$K_DTYPE_T;
    d->dsc$b_class = DSC$K_CLASS_S;
    d->dsc$a_pointer = (char *)p;
}

uint32_t ovmx_vmsabi_assign(const char *dev, unsigned devlen, uint16_t *chan,
                            uint32_t acmode, const char *mbx, unsigned mbxlen,
                            uint32_t flags)
{
    struct dsc$descriptor_s d, m;
    mkdsc(&d, dev, devlen);
    if (mbx)
        mkdsc(&m, mbx, mbxlen);
    return sys$assign(&d, chan, acmode, mbx ? &m : NULL, flags);
}

uint32_t ovmx_vmsabi_dassgn(uint16_t chan)
{
    return sys$dassgn(chan);
}

uint32_t ovmx_vmsabi_lnm(int create, const uint32_t *attr,
                         const char *tab, unsigned tablen,
                         const char *log, unsigned loglen,
                         const uint8_t *acmode,
                         const struct ovmx_abi_item *items, unsigned n)
{
    struct dsc$descriptor_s t, l;
    mkdsc(&t, tab, tablen);
    mkdsc(&l, log, loglen);
    struct item_list_3 *il = calloc(n + 1, sizeof *il);
    if (!il)
        return SS$_INSFMEM;
    for (unsigned i = 0; i < n; i++) {
        il[i].buflen = items[i].len;
        il[i].item_code = items[i].code;
        il[i].bufaddr = items[i].buf;
        il[i].retlen = items[i].retlen;
    }
    uint32_t st = create ? sys$crelnm(attr, &t, &l, acmode, il)
                         : sys$trnlnm(attr, &t, &l, acmode, il);
    free(il);
    return st;
}
