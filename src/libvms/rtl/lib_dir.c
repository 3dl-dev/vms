/*
 * lib_dir.c - LIB$CREATE_DIR.
 *
 * LIB$CREATE_DIR(spec, [owner_uic], [prot_enable], [prot_value], [max_versions],
 *                [rvn], [initial_alloc])
 *
 * Creates the directory named by `spec` (and any missing parents) as ODS-2 NAME.DIR;1
 * files over the executive's Files-11 ACP, the primitive CREATE/DIRECTORY uses. A
 * relative spec ("[.LOG]") is completed from the process default directory the
 * executive holds ($SETDDIR). Returns SS$_CREATED when the leaf directory was made,
 * SS$_NORMAL when it already existed.
 *
 *   owner_uic     (longword, group<<16|member) -> the directory's owner, else the process UIC.
 *   prot_enable / prot_value
 *                 which bits of the 16-bit protection word are taken from prot_value; the
 *                 rest come from the process default protection ($SETDFPROT).
 *   max_versions  the directory's default version limit (FAT$W_VERSIONS).
 *   initial_alloc blocks allocated to the new directory (1..64).
 *   rvn           only relative volume 0 exists: a non-zero value is SS$_BADPARAM.
 *
 * No /dev/vms -> SS$_NOSUCHDEV from the ACP wrappers; nothing is faked.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "starlet.h"
#include "lnmdef.h"
#include "vms_kif.h"

extern uint32_t sys$setddir(const struct dsc$descriptor_s *new_dir, unsigned short *old_len,
                           struct dsc$descriptor_s *old_dir);

#define DIRFLAG_CHAR 0x2000u   /* FH2$M_DIRECTORY (ODS2_FH2_M_DIRECTORY) */

/* Split "[dev:]<dir>" into device (with colon) and the dotted tree. Sets *rel when the
 * tree starts with '.' (relative to the default directory). */
static int split_spec(const char *spec, char *dev, size_t devcap, char *tree, size_t treecap, int *rel)
{
    const char *colon = strchr(spec, ':');
    const char *lb = strpbrk(spec, "[<");
    const char *rb;
    size_t n;

    dev[0] = tree[0] = '\0';
    *rel = 0;
    if (!lb)
        return 0;
    if (colon && colon < lb) {
        n = (size_t)(colon - spec) + 1;
        if (n >= devcap)
            return 0;
        memcpy(dev, spec, n);
        dev[n] = '\0';
    }
    rb = strpbrk(lb + 1, "]>");
    if (!rb || rb == lb + 1)
        return 0;
    n = (size_t)(rb - lb - 1);
    if (n >= treecap)
        return 0;
    memcpy(tree, lb + 1, n);
    tree[n] = '\0';
    if (tree[0] == '.') {
        *rel = 1;
        memmove(tree, tree + 1, strlen(tree));   /* keep the part after the dot */
    }
    return 1;
}

uint32_t lib$create_dir(const struct dsc$descriptor_s *spec_d, const uint32_t *owner,
                        const uint32_t *prot_ena, const uint32_t *prot_val,
                        const uint32_t *max_versions, const uint32_t *rvn,
                        const uint32_t *init_alloc)
{
    char spec[256], dev[128], tree[256], ddir[256], full[512];
    int rel;
    uint32_t st, chan = 0;
    size_t n;
    uint16_t pdn = 0, pds = 0;
    uint8_t pdr = 0, pdx = 0;
    int created_leaf = 0;
    char *save = NULL, *tok;
    uint32_t ialloc = 1;

    if (!spec_d || !spec_d->dsc$a_pointer || spec_d->dsc$w_length == 0 ||
        spec_d->dsc$w_length >= sizeof(spec))
        return SS$_BADPARAM;
    if (rvn && *rvn != 0)
        return SS$_BADPARAM;                  /* one relative volume only */
    if (init_alloc) {
        if (*init_alloc < 1 || *init_alloc > 64)
            return SS$_BADPARAM;
        ialloc = *init_alloc;
    }
    n = spec_d->dsc$w_length;
    memcpy(spec, spec_d->dsc$a_pointer, n);
    spec[n] = '\0';

    if (!split_spec(spec, dev, sizeof(dev), tree, sizeof(tree), &rel))
        return SS$_BADPARAM;

    /* The process default directory completes a relative tree and a missing device. It is
     * the executive's ($SETDDIR); a process that never set one has the logical SYS$LOGIN:
     * as its default, which is translated to the DEV:[DIR] it names. */
    ddir[0] = '\0';
    if (rel || !dev[0]) {
        struct dsc$descriptor_s dd = { sizeof(ddir) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, ddir };
        unsigned short dl = 0;
        int hops;

        (void)sys$setddir(NULL, &dl, &dd);
        ddir[dl < sizeof(ddir) ? dl : sizeof(ddir) - 1] = '\0';
        for (hops = 0; hops < 8 && ddir[0] && !strpbrk(ddir, "[<"); hops++) {
            char name[128], eq[256];
            struct dsc$descriptor_s nd = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_S, name };
            struct item_list_3 il[2];
            unsigned short el = 0;
            size_t nl = strcspn(ddir, ":");

            if (nl == 0 || nl >= sizeof(name))
                break;
            memcpy(name, ddir, nl);
            name[nl] = '\0';
            nd.dsc$w_length = (unsigned short)nl;
            il[0].buflen = sizeof(eq) - 1; il[0].item_code = LNM$_STRING;
            il[0].bufaddr = eq;            il[0].retlen = &el;
            il[1].buflen = 0; il[1].item_code = 0; il[1].bufaddr = NULL; il[1].retlen = NULL;
            {
                static const struct dsc$descriptor_s tab = { 12, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                                             (char *)"LNM$FILE_DEV" };
                if (!(sys$trnlnm(NULL, &tab, &nd, NULL, il) & 1))
                    break;
            }
            eq[el < sizeof(eq) ? el : sizeof(eq) - 1] = '\0';
            snprintf(ddir, sizeof(ddir), "%s", eq);
        }
    }
    if (rel || !dev[0]) {
        char ddev[128], dtree[256];
        int drel;
        if (split_spec(ddir, ddev, sizeof(ddev), dtree, sizeof(dtree), &drel)) {
            if (!dev[0])
                snprintf(dev, sizeof(dev), "%s", ddev);
            if (rel) {
                char joined[512];
                snprintf(joined, sizeof(joined), "%s.%s", dtree, tree);
                if (strlen(joined) >= sizeof(tree))
                    return SS$_BADPARAM;
                strcpy(tree, joined);
            }
        } else if (rel) {
            return SS$_BADPARAM;              /* relative spec, no default directory to resolve it in */
        }
    }
    if (!dev[0])
        snprintf(dev, sizeof(dev), "SYS$DISK:");
    snprintf(full, sizeof(full), "%s", tree);

    st = vms_kif_acp_assign(dev, &chan);
    if (!(st & 1))
        return st;

    {
        char *comp[64];
        int ncomp = 0, ci;

        for (tok = strtok_r(full, ".", &save); tok && ncomp < 64; tok = strtok_r(NULL, ".", &save))
            if (tok[0])
                comp[ncomp++] = tok;
        if (ncomp == 0) {
            (void)vms_kif_dassgn(chan);
            return SS$_BADPARAM;
        }

        for (ci = 0; ci < ncomp; ci++) {
            char nm[VMS_ACP_NAME_SIZE];
            struct vms_acp_access_args a;
            struct vms_acp_fileop_args fop;
            size_t tl = strlen(comp[ci]), k;
            int leaf = (ci == ncomp - 1);

            if (tl > VMS_ACP_NAME_SIZE - 5)
                tl = VMS_ACP_NAME_SIZE - 5;
            for (k = 0; k < tl; k++) {
                char c = comp[ci][k];
                nm[k] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
            }
            nm[tl] = '\0';
            strncat(nm, ".DIR", sizeof(nm) - strlen(nm) - 1);

            memset(&a, 0, sizeof(a));
            a.chan = chan;
            a.did_num = pdn; a.did_seq = pds; a.did_rvn = pdr; a.did_nmx = pdx;
            a.version = 1;
            strncpy(a.name, nm, VMS_ACP_NAME_SIZE - 1);
            st = vms_kif_acp_access(&a);
            if (st & 1) {                       /* already there: step into it */
                pdn = a.fid_num; pds = a.fid_seq; pdr = a.fid_rvn; pdx = a.fid_nmx;
                (void)vms_kif_acp_deaccess(chan);
                created_leaf = 0;
                continue;
            }
            if (st != SS$_NOSUCHFILE) {
                (void)vms_kif_dassgn(chan);
                return st;
            }

            memset(&fop, 0, sizeof(fop));
            fop.chan = chan;
            fop.func = VMS_ACP_FOP_CREATE;
            fop.modifiers = VMS_ACP_M_CREATE;
            fop.did_num = pdn; fop.did_seq = pds; fop.did_rvn = pdr; fop.did_nmx = pdx;
            fop.version = 1;
            fop.attr.filechar = DIRFLAG_CHAR;
            fop.exsz = leaf ? ialloc : 1;
            strncpy(fop.name, nm, VMS_ACP_NAME_SIZE - 1);
            if (leaf) {                         /* the arguments describe the directory asked for */
                if (prot_ena && prot_val) {
                    uint16_t dfl = 0xFF00;      /* S:RWED,O:RWED,G:,W: without a $SETDFPROT value */
                    (void)vms_kif_dfprot(NULL, &dfl);
                    fop.attr_ctl |= VMS_ACP_ATTR_PROT;
                    fop.attr.fileprot = (uint16_t)((dfl & ~(*prot_ena & 0xFFFFu)) | (*prot_val & *prot_ena & 0xFFFFu));
                }
                if (owner) {
                    fop.attr_ctl |= VMS_ACP_ATTR_OWNER;
                    fop.attr.uic_group = (uint16_t)(*owner >> 16);
                    fop.attr.uic_member = (uint16_t)(*owner & 0xFFFFu);
                }
                if (max_versions && (*max_versions & 0xFFFFu)) {
                    fop.attr_ctl |= VMS_ACP_ATTR_VERSIONS;
                    fop.attr.recattr[30] = (uint8_t)(*max_versions & 0xFF);
                    fop.attr.recattr[31] = (uint8_t)((*max_versions >> 8) & 0xFF);
                }
            }
            st = vms_kif_acp_fileop(&fop);
            if (!(st & 1)) {
                (void)vms_kif_dassgn(chan);
                return st;
            }
            pdn = fop.fid_num; pds = fop.fid_seq; pdr = fop.fid_rvn; pdx = fop.fid_nmx;
            created_leaf = 1;
        }
    }
    (void)vms_kif_dassgn(chan);
    return created_leaf ? SS$_CREATED : SS$_NORMAL;
}
