/*
 * sys_vmsabi_core.c - the OVMX-service half of the VMS-ABI system services
 * (vms-38b): rebuild OVMX's native descriptors and item lists from the plain
 * values sys_vmsabi.c extracted from the caller's VMS forms, and run the same
 * services OVMX's own code calls ($ASSIGN, $DASSGN, $TRNLNM, $CRELNM, and the
 * executive ACP's IO$_ACCESS / IO$_DEACCESS for $QIO on a file channel).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "descrip.h"
#include "lnmdef.h"
#include "ssdef.h"
#include "starlet.h"
#include "lib$routines.h"
#include "sys_vmsabi_core.h"
#include "vms_kif.h"

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

/* ---------------------------------------------- $QIO on a file channel ---- */

extern int vms$$chan_is_file(uint16_t chan);
extern uint32_t vms$$chan_exec_chan(uint16_t chan);

#define FIB_M_WRITE 256u           /* FIB$M_WRITE (FIBDEF) */

uint32_t ovmx_vmsabi_acp_access(uint16_t chan, uint32_t acctl, const uint16_t did[3],
                                const uint16_t fid[3], const char *name, unsigned namelen,
                                int keep, struct ovmx_abi_fileattr *out)
{
    if (!vms$$chan_is_file(chan))
        return SS$_ILLIOFUNC;
    struct vms_acp_access_args a;
    memset(&a, 0, sizeof a);
    a.chan = vms$$chan_exec_chan(chan);
    if (acctl & FIB_M_WRITE)
        a.acctl = VMS_ACP_ACCTL_WRITE;
    if (did[0] == 0 && did[1] == 0 && did[2] == 0 && (fid[0] || fid[1])) {
        a.fidmode = 1;
        a.fid_num = fid[0];
        a.fid_seq = fid[1];
        a.fid_rvn = (uint8_t)(fid[2] & 0xFF);
        a.fid_nmx = (uint8_t)(fid[2] >> 8);
    } else {
        /* FIB$W_DID: number, sequence, then RVN (low byte) + NMX (high byte). */
        a.did_num = did[0];
        a.did_seq = did[1];
        a.did_rvn = (uint8_t)(did[2] & 0xFF);
        a.did_nmx = (uint8_t)(did[2] >> 8);
        unsigned n = 0, v = 0;
        while (n < namelen && name[n] != ';')
            n++;
        if (n == 0 || n >= sizeof a.name)
            return SS$_BADPARAM;
        for (unsigned i = n + 1; i < namelen && name[i] >= '0' && name[i] <= '9'; i++)
            v = v * 10 + (unsigned)(name[i] - '0');
        if (v > 32767)
            return SS$_BADPARAM;
        for (unsigned i = 0; i < n; i++) {
            char c = name[i];
            a.name[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
        a.version = (uint16_t)v;
    }
    uint32_t st = vms_kif_acp_access(&a);
    if (!(st & 1))
        return st;
    memset(out, 0, sizeof *out);
    out->uchar = a.attr.filechar;
    out->fpro = a.attr.fileprot;
    out->uic_member = a.attr.uic_member;
    out->uic_group = a.attr.uic_group;
    memcpy(out->recattr, a.attr.recattr, sizeof out->recattr);
    memcpy(out->credate, a.attr.credate, 8);
    memcpy(out->revdate, a.attr.revdate, 8);
    memcpy(out->expdate, a.attr.expdate, 8);
    memcpy(out->bakdate, a.attr.bakdate, 8);
    out->fid[0] = a.fid_num;
    out->fid[1] = a.fid_seq;
    out->fid[2] = (uint16_t)(a.fid_rvn | (a.fid_nmx << 8));
    a.name[sizeof a.name - 1] = '\0';
    int k = snprintf(out->name, sizeof out->name, "%s;%u", a.name, (unsigned)a.out_version);
    out->namelen = k > 0 && (unsigned)k < sizeof out->name ? (unsigned)k : 0;
    if (!keep)
        vms_kif_acp_deaccess(a.chan);
    return SS$_NORMAL;
}

uint32_t ovmx_vmsabi_acp_deaccess(uint16_t chan)
{
    if (!vms$$chan_is_file(chan))
        return SS$_ILLIOFUNC;
    return vms_kif_acp_deaccess(vms$$chan_exec_chan(chan));
}

void ovmx_vmsabi_io_complete(uint32_t efn, void (*astadr)(unsigned long long), unsigned long long astprm)
{
    if ((efn & 0xFFu) < 128)
        sys$setef(efn);
    if (astadr)
        astadr(astprm);
}

uint32_t ovmx_vmsabi_qio(int wait, uint32_t efn, uint16_t chan, uint32_t func,
                         void *iosb, unsigned long long astadr,
                         unsigned long long astprm, const unsigned long long p[6])
{
    void (*ast)(uint32_t) = (void (*)(uint32_t))(uintptr_t)astadr;
    void *p1 = (void *)(uintptr_t)p[0];
    if (wait)
        return sys$qiow(efn, chan, func, iosb, ast, (uint32_t)astprm, p1,
                        (uint32_t)p[1], (uint32_t)p[2], (uint32_t)p[3],
                        (uint32_t)p[4], (uint32_t)p[5]);
    return sys$qio(efn, chan, func, iosb, ast, (uint32_t)astprm, p1,
                   (uint32_t)p[1], (uint32_t)p[2], (uint32_t)p[3],
                   (uint32_t)p[4], (uint32_t)p[5]);
}

uint32_t ovmx_vmsabi_put_output(const char *p, unsigned len)
{
    struct dsc$descriptor_s d;
    mkdsc(&d, p, len);
    return lib$put_output(&d);
}

uint32_t ovmx_vmsabi_ascefc(uint32_t efn, const char *name, unsigned namelen,
                            uint32_t prot, uint32_t perm)
{
    struct dsc$descriptor_s d;
    mkdsc(&d, name, namelen);
    return sys$ascefc(efn, &d, prot, perm);
}

uint32_t ovmx_vmsabi_dlcefc(const char *name, unsigned namelen)
{
    struct dsc$descriptor_s d;
    mkdsc(&d, name, namelen);
    return sys$dlcefc(&d);
}

uint32_t ovmx_vmsabi_bintim(const char *s, unsigned len, void *timadr)
{
    struct dsc$descriptor_s d;
    mkdsc(&d, s, len);
    return sys$bintim(&d, (struct _generic_64 *)timadr);
}

uint32_t ovmx_vmsabi_asctim(uint16_t *timlen, char *out, unsigned outcap,
                            const void *timadr, uint32_t cvtflg)
{
    struct dsc$descriptor_s d;
    mkdsc(&d, out, outcap);
    return sys$asctim(timlen, &d, (const uint64_t *)timadr, cvtflg);
}

uint32_t ovmx_vmsabi_getmsg(uint32_t msgid, uint16_t *msglen, char *out,
                            unsigned outcap, uint32_t flags, uint8_t *outadr)
{
    struct dsc$descriptor_s d;
    mkdsc(&d, out, outcap);
    return sys$getmsg(msgid, msglen, &d, flags, (uint32_t *)outadr);
}
