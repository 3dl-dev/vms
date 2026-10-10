/*
 * rms_vmsabi.c - the RMS services by their upper-case (VMS-ABI) names over
 * the VMS-layout control blocks (vms-692): SYS$PARSE and SYS$SEARCH; vms-8b5
 * adds the file and record services (end of file).
 *
 * A DEC C client -- GCC's VMS-host vmsdbgout.cc among them -- includes
 * <vms/rms.h> and passes a FAB/NAM in the VMS byte layout (every address
 * field 32 bits, src/libvms/include/vms/). These entry points read that
 * layout, run the request on the RMS engine through rms_vmsabi_core.c (which
 * owns OVMX's internal blocks, keyed by NAM$L_WCC), and write the results
 * back in the VMS layout: the expanded/resultant string into the caller's
 * ESA/RSA, the component lengths and addresses into the NAM (addresses
 * within the caller's own string area), NAM$L_FNB, NAM$T_DVI, NAM$W_FID and
 * NAM$W_DID, FAB$L_STS/STV. The optional err/suc completion routines are
 * called with the FAB, as VMS RMS does for a synchronous request.
 *
 * Built only for the Alpha (32-bit address fields need the alpha-dec-vms
 * compiler's #pragma __required_pointer_size); OVMX's own code keeps the
 * lower-case sys$parse/sys$search over its internal blocks.
 */
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <vms/rms.h>
#include "rmsdef.h"
#include "rms_vmsabi_core.h"

static int fab_ok(struct fabdef *fab)
{
    return fab && fab->fab$b_bid == FAB$C_BID && fab->fab$b_bln >= FAB$C_BLN;
}

static struct namdef *fab_nam(struct fabdef *fab)
{
    struct namdef *nam = (struct namdef *)fab->fab$l_nam;
    if (nam && (nam->nam$b_bid != NAM$C_BID || nam->nam$b_bln < NAM$C_BLN))
        return (struct namdef *)-1;
    return nam;
}

/* The optional err/suc completion routines. A caller LINKed on OpenVMS passes
 * a counted argument list and usually omits them (CALLS #1), so only the
 * arguments the call passed are read: the count is the low byte of the
 * argument information (R25) OTS$HOME_ARGS stores two quadwords below where
 * va_start points for one named argument -- DEC C's va_count, as
 * src/vmsrms/crtl_rms_fd.c and src/libvms/syssvc/sys_vmsabi.c read it. A
 * macro used in the variadic function itself (vms-45f). (vms-8b5) */
struct cmpl {
    void (*err)(void *);
    void (*suc)(void *);
};
static inline unsigned rms_va_count(const void *apv)
{
    const uint64_t *p = apv;
    __asm__("" : "+r"(p));              /* opaque to the stdarg pass (vms-45f) */
    return (unsigned)(p[-2] & 0xFF);
}
#define CMPL_TAKE(ap, c)                                                    \
    do {                                                                    \
        unsigned n_ = rms_va_count((const void *)(ap));                     \
        (c).err = n_ >= 2 ? va_arg(ap, void (*)(void *)) : 0;               \
        (c).suc = n_ >= 3 ? va_arg(ap, void (*)(void *)) : 0;               \
    } while (0)

static int complete(struct fabdef *fab, int st, const struct cmpl *c)
{
    fab->fab$l_sts = (unsigned)st;
    if ((st & 1) && c->suc)
        c->suc(fab);
    else if (!(st & 1) && c->err)
        c->err(fab);
    return st;
}

/* Copy the string and its components into a VMS NAM's string area. */
static int to_nam(struct namdef *nam, char *area, unsigned size,
                  unsigned char *lenp, const struct ovmx_rmsabi_name *io,
                  int with_ids)
{
    if (!area || size == 0)
        return 0;                              /* no area: nothing returned */
    if (io->len > size)
        return -1;
    memcpy(area, io->str, io->len);
    *lenp = (unsigned char)io->len;
    nam->nam$b_node = io->node_len;
    nam->nam$b_dev  = io->dev_len;
    nam->nam$b_dir  = io->dir_len;
    nam->nam$b_name = io->name_len;
    nam->nam$b_type = io->type_len;
    nam->nam$b_ver  = io->ver_len;
    nam->nam$l_node = area + io->node_off;
    nam->nam$l_dev  = area + io->dev_off;
    nam->nam$l_dir  = area + io->dir_off;
    nam->nam$l_name = area + io->name_off;
    nam->nam$l_type = area + io->type_off;
    nam->nam$l_ver  = area + io->ver_off;
    nam->nam$l_fnb  = io->fnb;
    memcpy(nam->nam$t_dvi, io->dvi, sizeof nam->nam$t_dvi);
    if (with_ids) {
        memcpy(nam->nam$w_fid, io->fid, sizeof nam->nam$w_fid);
        memcpy(nam->nam$w_did, io->did, sizeof nam->nam$w_did);
    }
    return 0;
}

int SYS$PARSE(void *fabp, ...)
{
    struct fabdef *fab = (struct fabdef *)fabp;
    va_list ap;
    va_start(ap, fabp);
    struct cmpl c;
    CMPL_TAKE(ap, c);
    va_end(ap);
    int st;
    if (!fab_ok(fab)) {
        return RMS$_FAB;
    }
    struct namdef *nam = fab_nam(fab);
    if (!nam || nam == (struct namdef *)-1) {
        st = complete(fab, RMS$_NAM, &c);
        return st;
    }
    struct ovmx_rmsabi_name io;
    memset(&io, 0, sizeof io);
    io.fna = fab->fab$l_fna;
    io.fns = fab->fab$b_fns;
    io.dna = fab->fab$l_dna;
    io.dns = fab->fab$b_dns;
    io.nop = nam->nam$b_nop;
    uint32_t wcc = nam->nam$l_wcc;
    st = (int)ovmx_rmsabi_parse(&wcc, &io);
    nam->nam$l_wcc = wcc;
    fab->fab$l_stv = io.stv;
    if (st & 1) {
        if (to_nam(nam, nam->nam$l_esa, nam->nam$b_ess, &nam->nam$b_esl, &io, 0) < 0)
            st = RMS$_ESS;
        else
            memset(nam->nam$w_did, 0, sizeof nam->nam$w_did);
    }
    st = complete(fab, st, &c);
    return st;
}

int SYS$SEARCH(void *fabp, ...)
{
    struct fabdef *fab = (struct fabdef *)fabp;
    va_list ap;
    va_start(ap, fabp);
    struct cmpl c;
    CMPL_TAKE(ap, c);
    va_end(ap);
    int st;
    if (!fab_ok(fab)) {
        return RMS$_FAB;
    }
    struct namdef *nam = fab_nam(fab);
    if (!nam || nam == (struct namdef *)-1) {
        st = complete(fab, RMS$_NAM, &c);
        return st;
    }
    struct ovmx_rmsabi_name io;
    memset(&io, 0, sizeof io);
    io.fna = fab->fab$l_fna;
    io.fns = fab->fab$b_fns;
    io.dna = fab->fab$l_dna;
    io.dns = fab->fab$b_dns;
    uint32_t wcc = nam->nam$l_wcc;
    st = (int)ovmx_rmsabi_search(&wcc, &io);
    nam->nam$l_wcc = wcc;
    fab->fab$l_stv = io.stv;
    if (st & 1) {
        if (to_nam(nam, nam->nam$l_rsa, nam->nam$b_rss, &nam->nam$b_rsl, &io, 1) < 0)
            st = RMS$_RSS;
    } else if (io.area == 1) {
        (void)to_nam(nam, nam->nam$l_esa, nam->nam$b_ess, &nam->nam$b_esl, &io, 0);
    }
    st = complete(fab, st, &c);
    return st;
}

/* ======================================================================
 * File and record services over the VMS-layout FAB/RAB/NAM (vms-8b5):
 * $CREATE, $OPEN, $CLOSE, $ERASE, $CONNECT, $DISCONNECT, $GET, $PUT, $FIND,
 * $UPDATE, $DELETE, $REWIND, $FLUSH. The FAB's IFI and the RAB's ISI carry
 * the handles of OVMX's internal blocks (rms_vmsabi_core.c).
 * ====================================================================== */

static int rab_ok(struct rabdef *rab)
{
    return rab && rab->rab$b_bid == RAB$C_BID && rab->rab$b_bln >= RAB$C_BLN;
}

static int rab_complete(struct rabdef *rab, int st, const struct cmpl *c)
{
    rab->rab$l_sts = (unsigned)st;
    if ((st & 1) && c->suc)
        c->suc(rab);
    else if (!(st & 1) && c->err)
        c->err(rab);
    return st;
}

/* A NAM receives the strings and the component addresses within them. */
static void nam_fill(struct namdef *nam, const struct ovmx_rmsabi_namout *o)
{
    char *area[3] = { 0, nam->nam$l_esa, nam->nam$l_rsa };
    if (nam->nam$l_esa && nam->nam$b_ess) {
        unsigned n = o->esl < nam->nam$b_ess ? o->esl : nam->nam$b_ess;
        memcpy(nam->nam$l_esa, o->esa, n);
        nam->nam$b_esl = (unsigned char)n;
    }
    if (nam->nam$l_rsa && nam->nam$b_rss) {
        unsigned n = o->rsl < nam->nam$b_rss ? o->rsl : nam->nam$b_rss;
        memcpy(nam->nam$l_rsa, o->rsa, n);
        nam->nam$b_rsl = (unsigned char)n;
    }
#define NAM_COMP(K, LEN, PTR)                                   \
    do {                                                        \
        char *a_ = area[o->which[K]];                           \
        nam->LEN = a_ ? o->len[K] : 0;                          \
        nam->PTR = a_ ? a_ + o->off[K] : 0;                     \
    } while (0)
    NAM_COMP(0, nam$b_node, nam$l_node);
    NAM_COMP(1, nam$b_dev, nam$l_dev);
    NAM_COMP(2, nam$b_dir, nam$l_dir);
    NAM_COMP(3, nam$b_name, nam$l_name);
    NAM_COMP(4, nam$b_type, nam$l_type);
    NAM_COMP(5, nam$b_ver, nam$l_ver);
#undef NAM_COMP
    nam->nam$l_fnb = o->fnb;
    memcpy(nam->nam$w_fid, o->fid, sizeof nam->nam$w_fid);
    memcpy(nam->nam$w_did, o->did, sizeof nam->nam$w_did);
    memcpy(nam->nam$t_dvi, o->dvi, sizeof nam->nam$t_dvi);
}

/* The FAB's request, in native types; 0 if its NAM is not a valid NAM. */
static int fab_req(struct fabdef *fab, struct namdef *nam, struct ovmx_rmsabi_fab *io)
{
    memset(io, 0, sizeof *io);
    io->fna = fab->fab$l_fna;
    io->fns = fab->fab$b_fns;
    io->dna = fab->fab$l_dna;
    io->dns = fab->fab$b_dns;
    io->fop = fab->fab$l_fop;
    io->alq = fab->fab$l_alq;
    io->deq = fab->fab$w_deq;
    io->fac = fab->fab$b_fac;
    io->shr = fab->fab$b_shr;
    io->org = fab->fab$b_org;
    io->rat = fab->fab$b_rat;
    io->rfm = fab->fab$b_rfm;
    io->mrs = fab->fab$w_mrs;
    io->mrn = fab->fab$l_mrn;
    io->fsz = fab->fab$b_fsz;
    if (nam) {
        io->nam = 1;
        io->nop = nam->nam$b_nop;
        io->ess = nam->nam$l_esa ? nam->nam$b_ess : 0;
        io->rss = nam->nam$l_rsa ? nam->nam$b_rss : 0;
    }
    return 1;
}

static void fab_attrs(struct fabdef *fab, const struct ovmx_rmsabi_fab *io)
{
    fab->fab$l_alq = io->alq;
    fab->fab$w_deq = io->deq;
    fab->fab$b_org = io->org;
    fab->fab$b_rat = io->rat;
    fab->fab$b_rfm = io->rfm;
    fab->fab$w_mrs = io->mrs;
    fab->fab$l_mrn = io->mrn;
    fab->fab$b_fsz = io->fsz;
}

/* $CREATE / $OPEN / $ERASE: op 0 open, 1 create, 2 erase. */
static int file_op(int op, void *fabp, const struct cmpl *c)
{
    struct fabdef *fab = (struct fabdef *)fabp;
    if (!fab_ok(fab))
        return RMS$_FAB;
    struct namdef *nam = fab_nam(fab);
    if (nam == (struct namdef *)-1)
        return complete(fab, RMS$_NAM, c);
    struct ovmx_rmsabi_fab io;
    fab_req(fab, nam, &io);
    uint32_t st;
    if (op == 2) {
        st = ovmx_rmsabi_erase(&io);
    } else {
        uint16_t ifi = fab->fab$w_ifi;
        st = ovmx_rmsabi_open(op, &ifi, &io);
        if (st & 1) {
            fab->fab$w_ifi = ifi;
            fab_attrs(fab, &io);
        }
    }
    fab->fab$l_stv = io.stv;
    if (nam)
        nam_fill(nam, &io.n);
    return complete(fab, (int)st, c);
}

int SYS$OPEN(void *fabp, ...)
{
    va_list ap;
    va_start(ap, fabp);
    struct cmpl c;
    CMPL_TAKE(ap, c);
    va_end(ap);
    int st = file_op(0, fabp, &c);
    return st;
}

int SYS$CREATE(void *fabp, ...)
{
    va_list ap;
    va_start(ap, fabp);
    struct cmpl c;
    CMPL_TAKE(ap, c);
    va_end(ap);
    int st = file_op(1, fabp, &c);
    return st;
}

int SYS$ERASE(void *fabp, ...)
{
    va_list ap;
    va_start(ap, fabp);
    struct cmpl c;
    CMPL_TAKE(ap, c);
    va_end(ap);
    int st = file_op(2, fabp, &c);
    return st;
}

int SYS$CLOSE(void *fabp, ...)
{
    struct fabdef *fab = (struct fabdef *)fabp;
    va_list ap;
    va_start(ap, fabp);
    struct cmpl c;
    CMPL_TAKE(ap, c);
    va_end(ap);
    int st;
    if (!fab_ok(fab)) {
        return RMS$_FAB;
    }
    uint16_t ifi = fab->fab$w_ifi;
    uint32_t stv = 0;
    st = (int)ovmx_rmsabi_close(&ifi, &stv);
    if (st != RMS$_IFI) {
        fab->fab$w_ifi = ifi;
        fab->fab$l_stv = stv;
    }
    st = complete(fab, st, &c);
    return st;
}

static void rab_req(struct rabdef *rab, struct ovmx_rmsabi_rab *io)
{
    memset(io, 0, sizeof *io);
    io->rop = rab->rab$l_rop;
    io->rac = rab->rab$b_rac;
    io->krf = rab->rab$b_krf;
    io->ksz = rab->rab$b_ksz;
    io->kbf = rab->rab$l_kbf;
    io->ubf = rab->rab$l_ubf;
    io->usz = rab->rab$w_usz;
    io->rbf_in = rab->rab$l_rbf;
    io->rsz_in = rab->rab$w_rsz;
}

/* The record the engine returned, addressed in the caller's buffer. */
static void rab_record(struct rabdef *rab, const struct ovmx_rmsabi_rab *io)
{
    char *ubf = rab->rab$l_ubf;
    const char *r = io->rbf;
    rab->rab$w_rsz = (unsigned short)io->rsz;
    if (!r) {
        rab->rab$l_rbf = ubf;                /* moved into the user buffer */
    } else if (ubf && r >= ubf && r <= ubf + rab->rab$w_usz) {
        rab->rab$l_rbf = ubf + (r - ubf);
    } else if (r && ubf) {
        unsigned n = io->rsz < rab->rab$w_usz ? io->rsz : rab->rab$w_usz;
        memmove(ubf, r, n);
        rab->rab$l_rbf = ubf;
    }
    memcpy(rab->rab$w_rfa, io->rfa, sizeof rab->rab$w_rfa);
}

int SYS$CONNECT(void *rabp, ...)
{
    struct rabdef *rab = (struct rabdef *)rabp;
    va_list ap;
    va_start(ap, rabp);
    struct cmpl c;
    CMPL_TAKE(ap, c);
    va_end(ap);
    int st;
    if (!rab_ok(rab)) {
        return RMS$_RAB;
    }
    struct fabdef *fab = (struct fabdef *)rab->rab$l_fab;
    struct ovmx_rmsabi_rab io;
    rab_req(rab, &io);
    if (!fab_ok(fab)) {
        st = RMS$_FAB;
    } else {
        uint16_t isi = rab->rab$w_isi;
        st = (int)ovmx_rmsabi_connect(fab->fab$w_ifi, &isi, &io);
        if (st & 1)
            rab->rab$w_isi = isi;
        rab->rab$l_stv = io.stv;
    }
    st = rab_complete(rab, st, &c);
    return st;
}

static int record_op(int op, void *rabp, const struct cmpl *c)
{
    struct rabdef *rab = (struct rabdef *)rabp;
    if (!rab_ok(rab))
        return RMS$_RAB;
    struct ovmx_rmsabi_rab io;
    rab_req(rab, &io);
    uint16_t isi = rab->rab$w_isi;
    int st = (int)ovmx_rmsabi_record(op, &isi, &io);
    if (st != RMS$_ISI) {
        rab->rab$w_isi = isi;
        rab->rab$l_stv = io.stv;
        if (op == OVMX_RMSABI_GET || op == OVMX_RMSABI_FIND)
            rab_record(rab, &io);
    }
    return rab_complete(rab, st, c);
}

#define RECORD_SERVICE(NAME, OP)                     \
    int NAME(void *rabp, ...)                        \
    {                                                \
        va_list ap;                                  \
        va_start(ap, rabp);                          \
        struct cmpl c;                               \
        CMPL_TAKE(ap, c);                            \
        va_end(ap);                                  \
        return record_op(OP, rabp, &c);              \
    }
RECORD_SERVICE(SYS$GET, OVMX_RMSABI_GET)
RECORD_SERVICE(SYS$PUT, OVMX_RMSABI_PUT)
RECORD_SERVICE(SYS$FIND, OVMX_RMSABI_FIND)
RECORD_SERVICE(SYS$UPDATE, OVMX_RMSABI_UPDATE)
RECORD_SERVICE(SYS$DELETE, OVMX_RMSABI_DELETE)
RECORD_SERVICE(SYS$REWIND, OVMX_RMSABI_REWIND)
RECORD_SERVICE(SYS$FLUSH, OVMX_RMSABI_FLUSH)
RECORD_SERVICE(SYS$DISCONNECT, OVMX_RMSABI_DISCONNECT)
