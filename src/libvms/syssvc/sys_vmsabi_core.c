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

#include "dcdef.h"
#include "descrip.h"
#include "dvidef.h"
#include "lnmdef.h"
#include "ssdef.h"
#include "starlet.h"
#include "lib$routines.h"
#include "str$routines.h"
#include "libdef.h"
#include <sys/mman.h>
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

int ovmx_vmsabi_chan_is_record_device(uint16_t chan)
{
    uint32_t cls = DC$_UNKNOWN;
    struct item_list_3 il[2];
    memset(il, 0, sizeof il);
    il[0].buflen = sizeof cls;
    il[0].item_code = DVI$_DEVCLASS;
    il[0].bufaddr = &cls;
    uint32_t st = sys$getdviw(0, chan, NULL, il, NULL, NULL, 0, 0);
    return (st & 1) && (cls == DC$_TERM || cls == DC$_MAILBOX);
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

/* ------------------------------------------------ vms-3b3f batch 2 -------- */

/* A native descriptor for an argument, or NULL when it was omitted. */
static struct dsc$descriptor_s *dsc_of(struct dsc$descriptor_s *d,
                                       const struct ovmx_abi_str *s)
{
    if (!s || !s->given)
        return NULL;
    mkdsc(d, s->p, s->len);
    return d;
}

/* A native item list (item_list_3, zero-terminated) for n VMS-form items. */
static struct item_list_3 *items_native(const struct ovmx_abi_item *items, unsigned n)
{
    struct item_list_3 *il = calloc(n + 1, sizeof *il);
    if (!il)
        return NULL;
    for (unsigned i = 0; i < n; i++) {
        il[i].buflen = items[i].len;
        il[i].item_code = items[i].code;
        il[i].bufaddr = items[i].buf;
        il[i].retlen = items[i].retlen;
    }
    return il;
}

#define AST(a) ((void (*)(uint32_t))(uintptr_t)(a))

uint32_t ovmx_vmsabi_dellnm(const struct ovmx_abi_str *tab, const struct ovmx_abi_str *log,
                            const uint8_t *acmode)
{
    struct dsc$descriptor_s t, l;
    return sys$dellnm(dsc_of(&t, tab), dsc_of(&l, log), acmode);
}

uint32_t ovmx_vmsabi_crembx(int prmflg, uint16_t *chan, uint32_t maxmsg, uint32_t bufquo,
                            uint32_t promsk, uint32_t acmode, const struct ovmx_abi_str *log,
                            uint32_t flags)
{
    struct dsc$descriptor_s l;
    return sys$crembx(prmflg, chan, maxmsg, bufquo, promsk, acmode, dsc_of(&l, log), flags, NULL);
}

uint32_t ovmx_vmsabi_getjpi(int wait, uint32_t efn, const uint32_t *pidadr,
                            const struct ovmx_abi_str *prcnam,
                            const struct ovmx_abi_item *items, unsigned n, void *iosb,
                            unsigned long long astadr, unsigned long long astprm)
{
    struct dsc$descriptor_s p;
    struct item_list_3 *il = items_native(items, n);
    if (!il)
        return SS$_INSFMEM;
    uint32_t st = (wait ? sys$getjpiw : sys$getjpi)(efn, pidadr, dsc_of(&p, prcnam), il, iosb,
                                                   AST(astadr), (uint32_t)astprm);
    free(il);
    return st;
}

uint32_t ovmx_vmsabi_getsyi(int wait, uint32_t efn, uint32_t *csidadr,
                            const struct ovmx_abi_str *node,
                            const struct ovmx_abi_item *items, unsigned n, void *iosb,
                            unsigned long long astadr, unsigned long long astprm)
{
    struct dsc$descriptor_s d;
    struct item_list_3 *il = items_native(items, n);
    if (!il)
        return SS$_INSFMEM;
    uint32_t st = (wait ? sys$getsyiw : sys$getsyi)(efn, csidadr, dsc_of(&d, node), il, iosb,
                                                   AST(astadr), (uint32_t)astprm);
    free(il);
    return st;
}

uint32_t ovmx_vmsabi_getdvi(int wait, uint32_t efn, uint16_t chan,
                            const struct ovmx_abi_str *devnam,
                            const struct ovmx_abi_item *items, unsigned n, void *iosb,
                            unsigned long long astadr, unsigned long long astprm)
{
    struct dsc$descriptor_s d;
    struct item_list_3 *il = items_native(items, n);
    if (!il)
        return SS$_INSFMEM;
    uint32_t st = (wait ? sys$getdviw : sys$getdvi)(efn, chan, dsc_of(&d, devnam), il,
                                                   (struct _iosb *)iosb, AST(astadr),
                                                   (uint32_t)astprm, 0);
    free(il);
    return st;
}

uint32_t ovmx_vmsabi_enq(int wait, uint32_t efn, uint32_t lkmode, void *lksb, uint32_t flags,
                         const struct ovmx_abi_str *resnam, uint32_t parid,
                         unsigned long long astadr, unsigned long long astprm,
                         unsigned long long blkast, uint32_t acmode, uint32_t rsdm)
{
    struct dsc$descriptor_s r;
    return (wait ? sys$enqw : sys$enq)(efn, lkmode, lksb, flags, dsc_of(&r, resnam), parid,
                                       AST(astadr), (uint32_t)astprm, AST(blkast), acmode,
                                       rsdm, NULL);
}

uint32_t ovmx_vmsabi_prc(int op, const uint32_t *pidadr, const struct ovmx_abi_str *prcnam,
                         uint32_t arg)
{
    struct dsc$descriptor_s p;
    struct dsc$descriptor_s *pn = dsc_of(&p, prcnam);
    switch (op) {
    case OVMX_ABI_PRC_WAKE:   return sys$wake(pidadr, pn);
    case OVMX_ABI_PRC_RESUME: return sys$resume(pidadr, pn);
    case OVMX_ABI_PRC_SUSPND: return sys$suspnd(pidadr, pn, arg);
    case OVMX_ABI_PRC_FORCEX: return sys$forcex(pidadr, pn, arg);
    case OVMX_ABI_PRC_DELPRC: return sys$delprc(pidadr, pn, NULL);
    default:                  return SS$_BADPARAM;
    }
}

uint32_t ovmx_vmsabi_setpri(const uint32_t *pidadr, const struct ovmx_abi_str *prcnam,
                            uint32_t pri, uint32_t *prvpri, uint32_t pol, uint32_t *prevpol)
{
    struct dsc$descriptor_s p;
    return sys$setpri(pidadr, dsc_of(&p, prcnam), pri, prvpri, pol, prevpol);
}

uint32_t ovmx_vmsabi_asctoid(const struct ovmx_abi_str *name, uint32_t *id, uint32_t *attrib)
{
    struct dsc$descriptor_s n;
    return sys$asctoid(dsc_of(&n, name), id, attrib);
}

uint32_t ovmx_vmsabi_idtoasc(uint32_t id, uint16_t *namlen, char *out, unsigned cap,
                             uint32_t *resid, uint32_t *attrib, uint32_t *ctx)
{
    struct dsc$descriptor_s d;
    mkdsc(&d, out, cap);
    return sys$idtoasc(id, namlen, out ? &d : NULL, resid, attrib, ctx);
}

uint32_t ovmx_vmsabi_grantid(int revoke, const uint32_t *pidadr,
                             const struct ovmx_abi_str *prcnam, const uint32_t *id,
                             const struct ovmx_abi_str *name, uint32_t *prvatr,
                             uint32_t segment)
{
    struct dsc$descriptor_s p, n;
    return (revoke ? sys$revokid : sys$grantid)(pidadr, dsc_of(&p, prcnam), id,
                                                dsc_of(&n, name), prvatr, segment);
}

uint32_t ovmx_vmsabi_sndopr(const struct ovmx_abi_str *msg, uint16_t chan)
{
    struct dsc$descriptor_s m;
    return sys$sndopr(dsc_of(&m, msg), chan);
}

uint32_t ovmx_vmsabi_brkthru(int wait, uint32_t efn, const struct ovmx_abi_str *msg,
                             const struct ovmx_abi_str *sendto, uint32_t sndtyp, void *iosb,
                             uint32_t carcon, uint32_t flags, uint32_t reqid, uint32_t timout,
                             unsigned long long astadr, unsigned long long astprm)
{
    struct dsc$descriptor_s m, t;
    if (!wait)
        return SS$_UNSUPPORTED;     /* OVMX provides $BRKTHRUW only */
    return (sys$brkthruw)(efn, dsc_of(&m, msg), dsc_of(&t, sendto), sndtyp,
                          (struct _iosb *)iosb, carcon, flags, reqid, timout,
                          AST(astadr), (uint32_t)astprm);
}


/* ================= vms-3b3f batch 3: LIBRTL string / symbol / VM routines ====
 *
 * The VMS-ABI heap. A caller LINKed on OpenVMS holds addresses in longwords:
 * a class-D string's pointer field and LIB$GET_VM's base address are 32 bits.
 * So their storage comes from regions mapped below 2 GB (P0), never from the
 * process's 64-bit heap. Blocks carry a 16-byte header; a freed block keeps a
 * FREE mark, so freeing it again (or freeing an address this heap never gave)
 * is detected and refused, as LIB$FREE_VM refuses it with LIB$_BADBLOADR.
 * Not thread-safe: a native image's code runs on one thread.
 */
#define P0_LIVE 0x4C30504Fu
#define P0_FREE 0x4630504Fu
#define P0_CHUNK (1u << 20)
#define P0_LIMIT 0x80000000ull

struct p0hdr {
    uint32_t size;          /* payload bytes, a multiple of 16 */
    uint32_t magic;
    struct p0hdr *next;     /* free list link (free blocks only) */
};

static char *p0_cur, *p0_end;
static struct p0hdr *p0_freelist;

static void *p0_map(size_t len)
{
    for (unsigned long long hint = 0x40000000ull; hint + len <= P0_LIMIT; hint += 0x01000000ull) {
        void *m = mmap((void *)(uintptr_t)hint, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED)
            return NULL;
        if ((unsigned long long)(uintptr_t)m + len <= P0_LIMIT)
            return m;
        munmap(m, len);
    }
    return NULL;
}

void *ovmx_vmsabi_p0_alloc(uint32_t n)
{
    uint32_t want = (n + 15u) & ~15u;
    if (want == 0)
        want = 16;
    for (struct p0hdr **pp = &p0_freelist; *pp; pp = &(*pp)->next) {
        struct p0hdr *h = *pp;
        if (h->size >= want) {
            *pp = h->next;
            h->magic = P0_LIVE;
            h->next = NULL;
            return h + 1;
        }
    }
    size_t need = sizeof(struct p0hdr) + want;
    if (!p0_cur || (size_t)(p0_end - p0_cur) < need) {
        size_t len = need > P0_CHUNK ? (need + 0xFFFFu) & ~(size_t)0xFFFFu : P0_CHUNK;
        char *m = p0_map(len);
        if (!m)
            return NULL;
        p0_cur = m;
        p0_end = m + len;
    }
    struct p0hdr *h = (struct p0hdr *)p0_cur;
    p0_cur += need;
    h->size = want;
    h->magic = P0_LIVE;
    h->next = NULL;
    return h + 1;
}

int ovmx_vmsabi_p0_free(void *p)
{
    if (!p || ((uintptr_t)p & 15u) || (unsigned long long)(uintptr_t)p >= P0_LIMIT)
        return 0;
    struct p0hdr *h = (struct p0hdr *)p - 1;
    if (h->magic != P0_LIVE)
        return 0;
    h->magic = P0_FREE;
    h->next = p0_freelist;
    p0_freelist = h;
    return 1;
}

/* A native descriptor for an input argument (NULL when omitted). */
static struct dsc$descriptor_s *dx_in(struct dsc$descriptor_s *n, const struct ovmx_abi_dx *x)
{
    if (!x || !x->given)
        return NULL;
    n->dsc$w_length = x->len;
    n->dsc$b_dtype = x->dtype;
    n->dsc$b_class = x->cls;
    n->dsc$a_pointer = x->ptr;
    return n;
}

/* A destination: a fixed-length one is written in place; a class-D one is
 * built as a native dynamic string seeded with its current value, and
 * dst_close moves the result into the VMS-ABI heap. */
static uint32_t dst_open(struct dsc$descriptor_s *n, const struct ovmx_abi_dx *x)
{
    if (!x || !x->given)
        return SS$_BADPARAM;
    dx_in(n, x);
    if (x->cls != DSC$K_CLASS_D)
        return SS$_NORMAL;
    n->dsc$w_length = 0;
    n->dsc$a_pointer = NULL;
    if (x->len && x->ptr) {
        uint16_t l = x->len;
        return str$copy_r(n, &l, x->ptr);
    }
    return SS$_NORMAL;
}

static uint32_t dst_close(struct dsc$descriptor_s *n, struct ovmx_abi_dx *x, uint32_t st)
{
    if (!x || x->cls != DSC$K_CLASS_D)
        return st;
    char *p = NULL;
    if (n->dsc$w_length) {
        p = ovmx_vmsabi_p0_alloc(n->dsc$w_length);
        if (!p) {
            str$free1_dx((struct dsc$descriptor_d *)n);
            return LIB$_INSVIRMEM;
        }
        memcpy(p, n->dsc$a_pointer, n->dsc$w_length);
    }
    (void)ovmx_vmsabi_p0_free(x->ptr);
    x->ptr = p;
    x->len = n->dsc$w_length;
    str$free1_dx((struct dsc$descriptor_d *)n);
    return st;
}

uint32_t ovmx_vmsabi_str_dst(int op, struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src)
{
    struct dsc$descriptor_s d, s;
    uint32_t st = dst_open(&d, dst);
    if (!(st & 1))
        return st;
    switch (op) {
    case OVMX_ABI_STR_COPY_DX: st = str$copy_dx(&d, dx_in(&s, src)); break;
    case OVMX_ABI_STR_APPEND:  st = str$append(&d, dx_in(&s, src)); break;
    case OVMX_ABI_STR_PREFIX:  st = str$prefix(&d, dx_in(&s, src)); break;
    default:                   st = str$upcase(&d, dx_in(&s, src)); break;
    }
    return dst_close(&d, dst, st);
}

uint32_t ovmx_vmsabi_str_extract(int op, struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src,
                                 const void *a, const void *b)
{
    struct dsc$descriptor_s d, s;
    uint32_t st = dst_open(&d, dst);
    if (!(st & 1))
        return st;
    switch (op) {
    case OVMX_ABI_STR_LEFT:     st = str$left(&d, dx_in(&s, src), a); break;
    case OVMX_ABI_STR_RIGHT:    st = str$right(&d, dx_in(&s, src), a); break;
    case OVMX_ABI_STR_LEN_EXTR: st = str$len_extr(&d, dx_in(&s, src), a, b); break;
    default:                    st = str$pos_extr(&d, dx_in(&s, src), a, b); break;
    }
    return dst_close(&d, dst, st);
}

uint32_t ovmx_vmsabi_str_replace(struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src,
                                 const uint32_t *start, const uint32_t *end,
                                 const struct ovmx_abi_dx *rep)
{
    struct dsc$descriptor_s d, s, r;
    uint32_t st = dst_open(&d, dst);
    if (!(st & 1))
        return st;
    st = str$replace(&d, dx_in(&s, src), start, end, dx_in(&r, rep));
    return dst_close(&d, dst, st);
}

uint32_t ovmx_vmsabi_str_translate(struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src,
                                   const struct ovmx_abi_dx *tran, const struct ovmx_abi_dx *match)
{
    struct dsc$descriptor_s d, s, t, m;
    uint32_t st = dst_open(&d, dst);
    if (!(st & 1))
        return st;
    st = str$translate(&d, dx_in(&s, src), dx_in(&t, tran), dx_in(&m, match));
    return dst_close(&d, dst, st);
}

uint32_t ovmx_vmsabi_str_trim(struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src,
                              uint16_t *outlen)
{
    struct dsc$descriptor_s d, s;
    uint32_t st = dst_open(&d, dst);
    if (!(st & 1))
        return st;
    st = str$trim(&d, dx_in(&s, src), outlen);
    return dst_close(&d, dst, st);
}

uint32_t ovmx_vmsabi_str_dupl_char(struct ovmx_abi_dx *dst, const int32_t *len, const char *ch)
{
    struct dsc$descriptor_s d;
    uint32_t st = dst_open(&d, dst);
    if (!(st & 1))
        return st;
    st = str$dupl_char(&d, len, ch);
    return dst_close(&d, dst, st);
}

uint32_t ovmx_vmsabi_str_element(struct ovmx_abi_dx *dst, const uint32_t *elem,
                                 const struct ovmx_abi_dx *delim, const struct ovmx_abi_dx *src)
{
    struct dsc$descriptor_s d, l, s;
    uint32_t st = dst_open(&d, dst);
    if (!(st & 1))
        return st;
    st = str$element(&d, elem, dx_in(&l, delim), dx_in(&s, src));
    return dst_close(&d, dst, st);
}

/* STR$CONCAT: the native routine ends its source list with a NULL pointer. */
#define ABI_CONCAT_MAX 16
uint32_t ovmx_vmsabi_str_concat(struct ovmx_abi_dx *dst, const struct ovmx_abi_dx *src, unsigned n)
{
    struct dsc$descriptor_s d, s[ABI_CONCAT_MAX];
    struct dsc$descriptor_s *a[ABI_CONCAT_MAX + 1];
    if (n == 0 || n > ABI_CONCAT_MAX)
        return SS$_BADPARAM;
    for (unsigned i = 0; i < n; i++)
        a[i] = dx_in(&s[i], &src[i]);
    for (unsigned i = n; i <= ABI_CONCAT_MAX; i++)
        a[i] = NULL;
    uint32_t st = dst_open(&d, dst);
    if (!(st & 1))
        return st;
    st = (str$concat)(&d, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9],
                      a[10], a[11], a[12], a[13], a[14], a[15], a[16]);
    return dst_close(&d, dst, st);
}

uint32_t ovmx_vmsabi_str_free1(struct ovmx_abi_dx *dst)
{
    if (!dst || !dst->given)
        return SS$_BADPARAM;
    if (dst->cls != DSC$K_CLASS_D)
        return SS$_NORMAL;
    (void)ovmx_vmsabi_p0_free(dst->ptr);
    dst->ptr = NULL;
    dst->len = 0;
    return SS$_NORMAL;
}

uint32_t ovmx_vmsabi_str_in(int op, const struct ovmx_abi_dx *a, const struct ovmx_abi_dx *b,
                            const uint32_t *start)
{
    struct dsc$descriptor_s x, y;
    struct dsc$descriptor_s *pa = dx_in(&x, a), *pb = dx_in(&y, b);
    switch (op) {
    case OVMX_ABI_CMP_COMPARE:     return (uint32_t)str$compare(pa, pb);
    case OVMX_ABI_CMP_COMPARE_EQL: return (uint32_t)str$compare_eql(pa, pb);
    case OVMX_ABI_CMP_POSITION:    return (uint32_t)str$position(pa, pb, start);
    case OVMX_ABI_CMP_FFIS:        return (uint32_t)str$find_first_in_set(pa, pb);
    case OVMX_ABI_CMP_FFNIS:       return (uint32_t)str$find_first_not_in_set(pa, pb);
    case OVMX_ABI_CMP_INDEX:       return (uint32_t)lib$index(pa, pb);
    case OVMX_ABI_CMP_LOCC:        return (uint32_t)lib$locc(pa, pb);
    case OVMX_ABI_CMP_MATCHC:      return (uint32_t)lib$matchc(pa, pb);
    default:                       return (uint32_t)lib$skpc(pa, pb);
    }
}

uint32_t ovmx_vmsabi_set_symbol(const struct ovmx_abi_dx *sym, const struct ovmx_abi_dx *val,
                                const uint32_t *tbl)
{
    struct dsc$descriptor_s s, v;
    return lib$set_symbol(dx_in(&s, sym), dx_in(&v, val), tbl);
}

uint32_t ovmx_vmsabi_get_symbol(const struct ovmx_abi_dx *sym, struct ovmx_abi_dx *val,
                                uint16_t *len, uint32_t *tbl)
{
    struct dsc$descriptor_s s, v;
    uint32_t st = dst_open(&v, val);
    if (!(st & 1))
        return st;
    st = lib$get_symbol(dx_in(&s, sym), &v, len, tbl);
    return dst_close(&v, val, st);
}

uint32_t ovmx_vmsabi_delete_symbol(const struct ovmx_abi_dx *sym, const uint32_t *tbl)
{
    struct dsc$descriptor_s s;
    return lib$delete_symbol(dx_in(&s, sym), tbl);
}

uint32_t ovmx_vmsabi_find_file(const struct ovmx_abi_dx *spec, struct ovmx_abi_dx *result,
                               uint32_t *ctx, const struct ovmx_abi_dx *def,
                               const struct ovmx_abi_dx *rel, uint32_t *stv,
                               const uint32_t *flags)
{
    struct dsc$descriptor_s f, r, d, l;
    uint32_t st = dst_open(&r, result);
    if (!(st & 1))
        return st;
    st = lib$find_file(dx_in(&f, spec), &r, ctx, dx_in(&d, def), dx_in(&l, rel), stv, flags);
    return dst_close(&r, result, st);
}

uint32_t ovmx_vmsabi_lib_getxxi(int op, const uint32_t *item, const uint32_t *pid,
                                uint16_t chan, const struct ovmx_abi_dx *name, void *resval,
                                struct ovmx_abi_dx *resstr, uint16_t *reslen, uint32_t *csid)
{
    struct dsc$descriptor_s n, r, *pr = NULL;
    uint32_t st;
    if (resstr && resstr->given) {
        st = dst_open(&r, resstr);
        if (!(st & 1))
            return st;
        pr = &r;
    }
    switch (op) {
    case OVMX_ABI_GETXXI_JPI: st = lib$getjpi(item, pid, dx_in(&n, name), resval, pr, reslen); break;
    case OVMX_ABI_GETXXI_SYI: st = lib$getsyi(item, resval, pr, reslen, csid, dx_in(&n, name)); break;
    default:                  st = lib$getdvi(item, chan, dx_in(&n, name), resval, pr, reslen); break;
    }
    return pr ? dst_close(&r, resstr, st) : st;
}

/* LIB$SYS_FAO: $FAO with the VMS-form parameters, into a fixed or dynamic
 * output string. */
uint32_t ovmx_vmsabi_sys_fao(const struct ovmx_abi_dx *ctr, uint16_t *outlen,
                             struct ovmx_abi_dx *out, const uint64_t *prm)
{
    char buf[1024];
    uint16_t len = 0;
    if (!ctr || !ctr->given || !out || !out->given)
        return SS$_BADPARAM;
    if (out->cls != DSC$K_CLASS_D) {
        uint32_t st = ovmx_fao_vmsabi(ctr->ptr, ctr->len, &len, out->ptr, out->len, prm);
        if (outlen)
            *outlen = len;
        return st;
    }
    uint32_t st = ovmx_fao_vmsabi(ctr->ptr, ctr->len, &len, buf, sizeof buf, prm);
    if (!(st & 1))
        return st;
    char *p = len ? ovmx_vmsabi_p0_alloc(len) : NULL;
    if (len && !p)
        return LIB$_INSVIRMEM;
    if (len)
        memcpy(p, buf, len);
    (void)ovmx_vmsabi_p0_free(out->ptr);
    out->ptr = p;
    out->len = len;
    if (outlen)
        *outlen = len;
    return st;
}

uint32_t ovmx_vmsabi_creprc(uint32_t *pidadr, const struct ovmx_abi_str *image,
                            const struct ovmx_abi_str *input, const struct ovmx_abi_str *output,
                            const struct ovmx_abi_str *error, const void *prvadr,
                            const void *quota, const struct ovmx_abi_str *prcnam,
                            uint32_t baspri, uint32_t uic, uint32_t mbxunt, uint32_t stsflg,
                            const struct ovmx_abi_str *node)
{
    struct dsc$descriptor_s i, in, out, err, p, n;
    return sys$creprc(pidadr, dsc_of(&i, image), dsc_of(&in, input), dsc_of(&out, output),
                      dsc_of(&err, error), prvadr, quota, dsc_of(&p, prcnam), baspri, uic,
                      mbxunt, stsflg, NULL, dsc_of(&n, node));
}
