/*
 * rms_vmsabi.c - the RMS services by their upper-case (VMS-ABI) names over
 * the VMS-layout control blocks (vms-692): SYS$PARSE and SYS$SEARCH.
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

typedef void (*rms_completion)(struct fabdef *);

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

static int complete(struct fabdef *fab, int st, va_list ap)
{
    rms_completion err = va_arg(ap, rms_completion);
    rms_completion suc = va_arg(ap, rms_completion);
    fab->fab$l_sts = (unsigned)st;
    if ((st & 1) && suc)
        suc(fab);
    else if (!(st & 1) && err)
        err(fab);
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
    int st;
    if (!fab_ok(fab)) {
        va_end(ap);
        return RMS$_FAB;
    }
    struct namdef *nam = fab_nam(fab);
    if (!nam || nam == (struct namdef *)-1) {
        st = complete(fab, RMS$_NAM, ap);
        va_end(ap);
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
    st = complete(fab, st, ap);
    va_end(ap);
    return st;
}

int SYS$SEARCH(void *fabp, ...)
{
    struct fabdef *fab = (struct fabdef *)fabp;
    va_list ap;
    va_start(ap, fabp);
    int st;
    if (!fab_ok(fab)) {
        va_end(ap);
        return RMS$_FAB;
    }
    struct namdef *nam = fab_nam(fab);
    if (!nam || nam == (struct namdef *)-1) {
        st = complete(fab, RMS$_NAM, ap);
        va_end(ap);
        return st;
    }
    struct ovmx_rmsabi_name io;
    memset(&io, 0, sizeof io);
    uint32_t wcc = nam->nam$l_wcc;
    st = (int)ovmx_rmsabi_search(&wcc, &io);
    nam->nam$l_wcc = wcc;
    fab->fab$l_stv = io.stv;
    if (st & 1) {
        if (to_nam(nam, nam->nam$l_rsa, nam->nam$b_rss, &nam->nam$b_rsl, &io, 1) < 0)
            st = RMS$_RSS;
    }
    st = complete(fab, st, ap);
    va_end(ap);
    return st;
}
