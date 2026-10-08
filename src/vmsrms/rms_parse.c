/*
 * rms_parse.c - RMS $PARSE System Service
 *
 * Parses a VMS filespec and fills in the NAM block with the
 * expanded name and its component parts (node, device, directory,
 * name, type, version). Uses vmsfs_parse_filespec() for the
 * heavy lifting and applies defaults from fab$l_dna.
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * $PARSE cited vms-5b4 until vms-fab, and it does NOT follow its rms_core.c
 * siblings to vms-407: what it lacks is not record arbitration but a device and
 * volume database to resolve against, which is vms-dv1's outcome.
 *
 * Was OVMX-USERSPACE until vms-d8e: $PARSE now reads a device logical's
 * CONCEALED / rooted-directory translation attribute (NAM$M_CNCL_DEV /
 * NAM$M_ROOT_DIR) via lnm_translate through LNM$FILE_DEV, so for an
 * executive-resident device logical it reaches the executive exactly as
 * sys$trnlnm's LNM$SYSTEM step does. It is therefore a mixture now.
 *
 * vms-240 completes the device-field logical-name resolution: $PARSE now
 * iteratively translates the DEVICE field of the expanded spec through
 * LNM$FILE_DEV via vmsfs_resolve_filespec_device() (the vmsfs wrapper over the
 * one filespec-aware, LNM$M_TERMINAL-honoring driver lnm_translate_filespec),
 * so a non-concealed device logical is substituted, an A -> B -> C chain
 * composes, and a terminal translation stops the chain. A concealed device is
 * still kept in the spec (concealment), which is why the CNCL_DEV/ROOT_DIR
 * reads below continue to see it. Resolution goes through vmsfs (not lnm
 * directly) so LIBVMSRMS$SHR keeps no rms -> lnm cross-image import.
 *
 * OVMX-PARTIAL: sys$parse (vms-d8e) -- exec: the CONCEALED / rooted-directory
 *     translation attribute of a device logical (set as NAM$M_CNCL_DEV /
 *     NAM$M_ROOT_DIR) is read from the logical's translation via
 *     vmsfs_device_concealed_rooted() -> lnm_translate() through LNM$FILE_DEV;
 *     when that device logical lives in an executive-resident table
 *     (LNM$JOB / LNM$GROUP / LNM$SYSTEM) the read is the executive's
 *     (vms_kif_lnm_translate), the same half sys$trnlnm's LNM$SYSTEM step is.
 * OVMX-LOCAL: sys$parse -- the filespec syntax expansion itself (component
 *     split, fab$l_dna defaults, NAM component pointers and lengths) is done
 *     in-process by vmsfs_parse_filespec(); a device logical defined in
 *     LNM$PROCESS_TABLE resolves locally too. There is still no executive
 *     device or volume database to resolve against (that is vms-dv1's deferred
 *     outcome) -- only the logical's concealed/rooted attribute crosses to the
 *     executive, and only when the logical is executive-resident.
 */

#include <stdio.h>
#include "rms_internal.h"
#include <string.h>
#include "rms/rms.h"
#include "vmsfs/filespec.h"
#include "vmsfs/device.h"
#include "ssdef.h"
#if defined(OVMX_HAVE_ACP)
#include "vms_kif.h"   /* vms_kif_ddir: the process default directory */
#endif

/* ==========================================================================
 * THE NAME ENGINE (rd vms-576 / vms-42f8 / vms-670 / vms-0a2): one parse of a
 * primary file specification against its default and the process default, as
 * VMS RMS $PARSE does it, used by $PARSE itself and by $OPEN / $CREATE /
 * $ERASE (rms_core.c) so every service resolves a name the same way.
 * Observed behaviour, VAX V7.3 and Alpha V8.4: docs/oracle/semantics/rms/.
 *
 *   - syntax: [node::][device:][directory]name[.type][;version] (a second '.'
 *     also starts a version); anything else is RMS$_SYN; upper-cased;
 *   - a device that is a logical name is translated through LNM$FILE_DEV --
 *     a translation carrying a directory supplies the directory (and counts as
 *     explicit), a primary spec with a directory of its own as well is
 *     RMS$_DIR, and a CONCEALED translation stops the walk with the logical's
 *     own name kept: NAM$M_CNCL_DEV, NAM$M_ROOT_DIR for a rooted "[x.]"
 *     equivalence, NAM$M_SEARCH_LIST when it has more than one;
 *   - missing parts come from the default spec (fab$l_dna), then from the
 *     process default device and directory ($SETDDIR); a relative directory
 *     ([.X], [-]) is merged onto the default one;
 *   - the expanded string always carries the type's '.' and the version's
 *     ';', empty or not;
 *   - NAM$L_FNB: EXP_DEV/DIR/NAME/TYPE/VER for what the PRIMARY spec gave,
 *     the WILD_* bits and WILDCARD, the device bits above.
 * ========================================================================== */
#include <ctype.h>
#include "starlet.h"
#include "lnmdef.h"

struct fspec {
    char node[64], dev[64], dir[256], name[256], type[256], ver[16];
    int  has_node, has_dev, has_dir, has_name, has_type, has_ver;
};

static int fs_namech(int c)
{
    return isalnum(c) || c == '$' || c == '_' || c == '-' || c == '*' || c == '%';
}

/* Split and upcase one file specification; RMS$_SYN on a malformed one. */
static uint32_t fs_split(const char *s, size_t n, struct fspec *f)
{
    size_t i = 0, k;
    memset(f, 0, sizeof *f);
    /* node:: -- a node name, optionally with a quoted access-control string
     * (NODE"user password"::), kept verbatim */
    for (k = 0; k + 1 < n; k++) {
        if (s[k] == '"') {                              /* skip the quoted part */
            size_t q = k + 1;
            while (q < n && s[q] != '"') q++;
            if (q + 2 < n && s[q + 1] == ':' && s[q + 2] == ':') { k = q; continue; }
            break;
        }
        if (s[k] == ':' && s[k + 1] == ':') {
            if (k == 0 || k >= sizeof f->node) return RMS$_SYN;
            int inq = 0;
            for (size_t j = 0; j < k; j++) {
                if (s[j] == '"') inq = !inq;
                else if (!inq && !isalnum((unsigned char)s[j]) && s[j] != '$' && s[j] != '_')
                    return RMS$_SYN;
                f->node[j] = inq || s[j] == '"' ? s[j] : (char)toupper((unsigned char)s[j]);
            }
            f->has_node = 1;
            i = k + 2;
            break;
        }
        if (!fs_namech((unsigned char)s[k])) break;
    }
    /* device: */
    for (k = i; k < n; k++) {
        if (s[k] == ':') {
            if (k == i || k - i >= sizeof f->dev) return RMS$_SYN;
            for (size_t j = i; j < k; j++) {
                if (!fs_namech((unsigned char)s[j])) return RMS$_SYN;
                f->dev[j - i] = (char)toupper((unsigned char)s[j]);
            }
            f->has_dev = 1;
            i = k + 1;
            break;
        }
        if (!fs_namech((unsigned char)s[k])) break;
    }
    /* [directory] or <directory> */
    if (i < n && (s[i] == '[' || s[i] == '<')) {
        char close = s[i] == '[' ? ']' : '>';
        size_t j = i + 1, d = 0;
        while (j < n && s[j] != close) {
            int c = (unsigned char)s[j];
            if (!(fs_namech(c) || c == '.')) return RMS$_SYN;
            if (d + 2 >= sizeof f->dir) return RMS$_SYN;
            f->dir[++d] = (char)toupper(c);
            j++;
        }
        if (j >= n) return RMS$_SYN;                 /* no closing bracket */
        f->dir[0] = '[';
        f->dir[++d] = ']';
        f->has_dir = 1;
        i = j + 1;
    }
    /* name */
    k = 0;
    while (i < n && fs_namech((unsigned char)s[i])) {
        if (k + 1 >= sizeof f->name) return RMS$_SYN;
        f->name[k++] = (char)toupper((unsigned char)s[i++]);
    }
    f->has_name = k > 0;
    /* .type */
    if (i < n && s[i] == '.') {
        i++;
        k = 0;
        while (i < n && fs_namech((unsigned char)s[i])) {
            if (k + 1 >= sizeof f->type) return RMS$_SYN;
            f->type[k++] = (char)toupper((unsigned char)s[i++]);
        }
        f->has_type = 1;
    }
    /* ;version, or .version after a type */
    if (i < n && (s[i] == ';' || (s[i] == '.' && f->has_type))) {
        int dotver = s[i] == '.';
        i++;
        k = 0;
        while (i < n && (isdigit((unsigned char)s[i]) || s[i] == '*' || s[i] == '-')) {
            if (k + 1 >= sizeof f->ver) return RMS$_SYN;
            f->ver[k++] = s[i++];
        }
        if (dotver && k == 0) return RMS$_SYN;
        f->has_ver = 1;
    }
    if (i != n)
        return RMS$_SYN;                                /* stray characters */
    return RMS$_NORMAL;
}

/* One step of LNM$FILE_DEV for a device name: 1 and the equivalence (index
 * 0), its attributes and highest index, or 0 when it is not a logical name. */
static int fs_trn(const char *name, char *eq, size_t eqsz, uint32_t *attr, int32_t *maxidx)
{
    /* through vmsfs: LIBVMSRMS$SHR keeps no rms -> lnm cross-image import */
    return (vmsfs_device_translate_step(name, eq, eqsz, attr, maxidx) & 1) ? 1 : 0;
}

/*
 * Translate a spec's device. dir_given: the spec had a directory of its own.
 * Sets *dir_from_lnm when a translation supplied the directory, *devbits to the
 * NAM$M_CNCL_DEV / ROOT_DIR / SEARCH_LIST of the device it stops at.
 */
static uint32_t fs_xlate_dev(struct fspec *f, int *dir_from_lnm, uint32_t *devbits)
{
    *devbits = 0;
    for (int depth = 0; f->has_dev && depth < 10; depth++) {
        char eq[256];
        uint32_t attr;
        int32_t maxidx;
        if (!fs_trn(f->dev, eq, sizeof eq, &attr, &maxidx))
            return RMS$_NORMAL;                         /* a device, not a logical */
        if (attr & LNM$M_CONCEALED) {
            const char *rb = strrchr(eq, ']');
            *devbits = NAM$M_CNCL_DEV |
                       ((rb && rb > eq && rb[-1] == '.') ? NAM$M_ROOT_DIR : 0) |
                       (maxidx > 0 ? NAM$M_SEARCH_LIST : 0);
            return RMS$_NORMAL;                         /* keep the concealed name */
        }
        struct fspec e;
        char *colon = strchr(eq, ':');
        if (!colon) {                                   /* "FOO" -> "BAR": a device */
            snprintf(f->dev, sizeof f->dev, "%s", eq);
            for (char *p = f->dev; *p; p++) *p = (char)toupper((unsigned char)*p);
        } else {
            if (fs_split(eq, strlen(eq), &e) != RMS$_NORMAL || !e.has_dev)
                return RMS$_NORMAL;                     /* not a device translation */
            memcpy(f->dev, e.dev, sizeof f->dev);
            if (e.has_dir) {
                if (f->has_dir)
                    return RMS$_DIR;                    /* two directories */
                memcpy(f->dir, e.dir, sizeof f->dir);
                f->has_dir = 1;
                *dir_from_lnm = 1;
            }
        }
        if (attr & LNM$M_TERMINAL)
            break;
    }
    return RMS$_NORMAL;
}

/* [.X] / [-] / [] onto an absolute default directory. */
static void fs_merge_reldir(char *dir, size_t dirsz, const char *ddir)
{
    if (!(dir[1] == '.' || dir[1] == '-' || dir[1] == ']') || ddir[0] != '[')
        return;
    char base[256], out[256];
    snprintf(base, sizeof base, "%.*s", (int)(strlen(ddir) - 2), ddir + 1);   /* no [] */
    const char *p = dir + 1;
    while (*p == '-') {                                 /* up one level per '-' */
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0'; else base[0] = '\0';
        p++;
        if (*p == '.') p++;
    }
    if (*p == '.') p++;
    size_t rl = strlen(p);                              /* rest incl. ']' */
    if (rl && p[rl - 1] == ']') rl--;
    if (rl)
        snprintf(out, sizeof out, "[%s%s%.*s]", base, base[0] ? "." : "", (int)rl, p);
    else
        snprintf(out, sizeof out, "[%s]", base[0] ? base : "000000");
    snprintf(dir, dirsz, "%s", out);
}

static uint32_t fs_wild(const char *s)
{
    return strpbrk(s, "*%") != NULL;
}

uint32_t rms_name_parse(const char *fna, size_t fns, const char *dna, size_t dns,
                        struct rms_pname *pn)
{
    struct fspec p, d, x;
    int dir_from_lnm = 0, dummy = 0;
    uint32_t pbits = 0, dbits = 0, xbits = 0, st;

    memset(pn, 0, sizeof *pn);
    if ((st = fs_split(fna ? fna : "", fna ? fns : 0, &p)) != RMS$_NORMAL)
        return st;
    if ((st = fs_xlate_dev(&p, &dir_from_lnm, &pbits)) != RMS$_NORMAL)
        return st;
    uint32_t fnb = (p.has_node ? NAM$M_NODE : 0) | (p.has_dev ? NAM$M_EXP_DEV : 0) |
                   ((p.has_dir || dir_from_lnm) ? NAM$M_EXP_DIR : 0) |
                   (p.has_name ? NAM$M_EXP_NAME : 0) | (p.has_type ? NAM$M_EXP_TYPE : 0) |
                   (p.has_ver ? NAM$M_EXP_VER : 0);
    uint32_t devbits = pbits;

    /* the default name */
    if (dna && dns) {
        if (fs_split(dna, dns, &d) != RMS$_NORMAL)
            return RMS$_DNA;
        (void)fs_xlate_dev(&d, &dummy, &dbits);
        if (!p.has_dev && d.has_dev) { memcpy(p.dev, d.dev, sizeof p.dev); p.has_dev = 1; devbits = dbits; }
        if (!p.has_dir && d.has_dir) { memcpy(p.dir, d.dir, sizeof p.dir); p.has_dir = 1; }
        if (!p.has_name && d.has_name) { memcpy(p.name, d.name, sizeof p.name); p.has_name = 1; }
        if (!p.has_type && d.has_type) { memcpy(p.type, d.type, sizeof p.type); p.has_type = 1; }
        if (!p.has_ver && d.has_ver && d.ver[0]) { memcpy(p.ver, d.ver, sizeof p.ver); }
    }

#if defined(OVMX_HAVE_ACP)
    /* the process default device and directory (the executive's, vms-872) */
    {
        char ddir[256] = "";
        if ((vms_kif_ddir(NULL, ddir, sizeof ddir) & 1) && ddir[0] &&
            fs_split(ddir, strlen(ddir), &x) == RMS$_NORMAL) {
            (void)fs_xlate_dev(&x, &dummy, &xbits);
            if (!p.has_dev && x.has_dev) {
                memcpy(p.dev, x.dev, sizeof p.dev); p.has_dev = 1; devbits = xbits;
                if (!p.has_dir && x.has_dir) { memcpy(p.dir, x.dir, sizeof p.dir); p.has_dir = 1; }
            }
            if (p.has_dir && x.has_dir && !strcmp(p.dev, x.dev))
                fs_merge_reldir(p.dir, sizeof p.dir, x.dir);
            else if (!p.has_dir && x.has_dir && !strcmp(p.dev, x.dev)) {
                memcpy(p.dir, x.dir, sizeof p.dir); p.has_dir = 1;
            }
        }
    }
#endif

    fnb |= devbits;
    if (fs_wild(p.dir)) fnb |= NAM$M_WILD_DIR | NAM$M_WILDCARD;
    if (fs_wild(p.name)) fnb |= NAM$M_WILD_NAME | NAM$M_WILDCARD;
    if (fs_wild(p.type)) fnb |= NAM$M_WILD_TYPE | NAM$M_WILDCARD;
    if (fs_wild(p.ver)) fnb |= NAM$M_WILD_VER | NAM$M_WILDCARD;

    /* the expanded string, with the parts' offsets */
    char *e = pn->esa;
    size_t cap = sizeof pn->esa, len = 0;
#define PUT(field, fmt, ...) do { \
        int w_ = snprintf(e + len, cap - len, fmt, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= cap - len) return RMS$_ESS; \
        pn->field##_off = (uint8_t)len; pn->field##_len = (uint8_t)w_; len += (size_t)w_; \
    } while (0)
    if (p.has_node) PUT(node, "%s::", p.node); else pn->node_off = 0;
    if (p.has_dev)  PUT(dev, "%s:", p.dev); else pn->dev_off = (uint8_t)len;
    if (p.has_dir)  PUT(dir, "%s", p.dir); else pn->dir_off = (uint8_t)len;
    PUT(name, "%s", p.name);
    PUT(type, ".%s", p.type);
    PUT(ver, ";%s", p.ver);
#undef PUT
    pn->esl = (uint8_t)len;
    pn->fnb = fnb;
    return RMS$_NORMAL;
}

/* Point a NAM's part descriptors into `base` (its ESA or RSA) at pn's
 * offsets, and set its FNB. */
void rms_nam_set_parts(struct NAM *nam, char *base, const struct rms_pname *pn)
{
    nam->nam$l_node = base + pn->node_off; nam->nam$b_node = pn->node_len;
    nam->nam$l_dev  = base + pn->dev_off;  nam->nam$b_dev  = pn->dev_len;
    nam->nam$l_dir  = base + pn->dir_off;  nam->nam$b_dir  = pn->dir_len;
    nam->nam$l_name = base + pn->name_off; nam->nam$b_name = pn->name_len;
    nam->nam$l_type = base + pn->type_off; nam->nam$b_type = pn->type_len;
    nam->nam$l_ver  = base + pn->ver_off;  nam->nam$b_ver  = pn->ver_len;
    nam->nam$l_fnb  = pn->fnb;
}

/*
 * sys$parse over the name engine: the expanded string into the ESA (RMS$_ESS
 * if it does not fit), the parts and FNB into the NAM, NAM$W_DID resolved.
 */
/* An absolute POSIX path ("/bin/sh"): no VMS filespec can start with '/', so
 * this is OVMX's own extension -- the substrate path a host tool or a test
 * harness hands RMS (RUN /bin/sh, F$SEARCH("/tmp/x"), the POSIX $SEARCH
 * backend) -- passed through as the expanded string with no NAM parts, as the
 * pre-engine $PARSE did. */
static int rms_posix_passthrough(const char *fna, size_t fns)
{
    return fna && fns >= 2 && fna[0] == '/';
}

static uint32_t rms_impl_parse(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID)
        return RMS$_FAB;
    struct NAM *nam = fab->fab$l_nam;
    if (!nam || nam->nam$b_bid != NAM$C_BID) {
        fab->fab$l_sts = RMS$_NAM;
        return RMS$_NAM;
    }

    /* a new $PARSE ends any wildcard search the NAM was carrying */
    if (nam->nam$$l_context)
        rms_search_end(nam);

    nam->nam$l_node = nam->nam$l_dev = nam->nam$l_dir = NULL;
    nam->nam$l_name = nam->nam$l_type = nam->nam$l_ver = NULL;
    nam->nam$b_node = nam->nam$b_dev = nam->nam$b_dir = 0;
    nam->nam$b_name = nam->nam$b_type = nam->nam$b_ver = 0;
    nam->nam$l_fnb = 0;
    nam->nam$b_esl = 0;
    nam->nam$b_rsl = 0;
    memset(nam->nam$w_fid, 0, sizeof(nam->nam$w_fid));
    memset(nam->nam$w_did, 0, sizeof(nam->nam$w_did));

    struct rms_pname pn;
    uint32_t st = rms_name_parse(fab->fab$l_fna, fab->fab$b_fns,
                                 fab->fab$l_dna, fab->fab$b_dns, &pn);
    if (st == RMS$_SYN && rms_posix_passthrough(fab->fab$l_fna, fab->fab$b_fns)) {
        /* executive-absent host tooling only: an absolute POSIX path is
         * handed to the POSIX $SEARCH backend as is (no NAM parts) */
        if (nam->nam$l_esa && fab->fab$b_fns <= nam->nam$b_ess) {
            memcpy(nam->nam$l_esa, fab->fab$l_fna, fab->fab$b_fns);
            nam->nam$b_esl = fab->fab$b_fns;
        }
        st = RMS$_NORMAL;
        fab->fab$l_sts = st;
        nam->nam$l_sts = st;
        return st;
    }
    if (st == RMS$_NORMAL && nam->nam$l_esa) {
        if (pn.esl > nam->nam$b_ess) {
            st = RMS$_ESS;
        } else {
            memcpy(nam->nam$l_esa, pn.esa, pn.esl);
            nam->nam$b_esl = pn.esl;
            rms_nam_set_parts(nam, nam->nam$l_esa, &pn);
        }
    }
    if (st == RMS$_NORMAL && !nam->nam$l_esa)
        nam->nam$l_fnb = pn.fnb;

#if defined(OVMX_HAVE_ACP)
    /* NAM$W_DID: the directory's file ID, when it names one directory
     * (no wildcard) and the parse is not syntax-only (NAM$M_SYNCHK). */
    if (st == RMS$_NORMAL && nam->nam$b_esl &&
        !(nam->nam$b_nop & NAM$M_SYNCHK) && !(nam->nam$l_fnb & NAM$M_WILD_DIR))
        rms_parse_did(nam);
#endif

    fab->fab$l_sts = st;
    nam->nam$l_sts = st;
    return st;
}


/* ============================================================
 * Public RMS entry points: VMS three-argument form
 *   SYS$xxx cb ,[err] ,[suc]   (VSI OpenVMS RMS Reference, Part III)
 * Thin wrappers over the synchronous rms_impl_* bodies above that
 * dispatch the optional AST-level completion routine (rms_complete).
 * ============================================================ */
uint32_t sys$parse(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_parse(fab), fab, err, suc);
}
