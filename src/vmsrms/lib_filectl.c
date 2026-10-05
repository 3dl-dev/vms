/*
 * lib_filectl.c - LIB$DELETE_FILE and LIB$RENAME_FILE.
 *
 * They live with RMS (LIBVMSRMS$SHR), not in LIBVMS: both drive RMS
 * $PARSE/$SEARCH/$ERASE/$RENAME, and LIBVMSRMS links LIBVMS, not the reverse.
 *
 * Clean-room: semantics from the public OpenVMS RTL Library (LIB$) Manual.
 */
#include <stdint.h>
#include <string.h>
#include "ssdef.h"
#include "descrip.h"
#include "rmsdef.h"
#include "rms.h"
#include "lib$routines.h"

/* ================================================================
 * LIB$DELETE_FILE / LIB$RENAME_FILE
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual -- LIB$DELETE_FILE,
 * LIB$RENAME_FILE.  Both walk the RMS wildcard context ($PARSE/$SEARCH) of the
 * file specification and act on each match with $ERASE / $RENAME; the optional
 * user procedures are called per file:
 *   confirm(name, [new,] arg)  -> success to proceed, failure to skip the file
 *   success(name, [new,] arg)  -> after the file was acted on
 *   error  (name, [new,] rms_status, rms_stv, arg) -> when $ERASE/$RENAME failed
 * With no error procedure the first RMS failure ends the call and is returned.
 * ================================================================ */

typedef uint32_t (*fc_name_proc)(const struct dsc$descriptor_s *, void *);

struct fc_search {
    struct FAB fab;
    struct NAM nam;
    char spec[256], dflt[256], esa[256], rsa[256], rel_esa[256];
};

static void fc_dsc_to_str(const struct dsc$descriptor_s *d, char *buf, size_t sz)
{
    buf[0] = '\0';
    if (d && d->dsc$a_pointer && d->dsc$w_length) {
        size_t n = d->dsc$w_length < sz - 1 ? d->dsc$w_length : sz - 1;
        memcpy(buf, d->dsc$a_pointer, n);
        buf[n] = '\0';
    }
}

static uint32_t fc_open_search(struct fc_search *fs,
                               const struct dsc$descriptor_s *spec,
                               const struct dsc$descriptor_s *dflt)
{
    memset(fs, 0, sizeof *fs);
    fc_dsc_to_str(spec, fs->spec, sizeof fs->spec);
    fc_dsc_to_str(dflt, fs->dflt, sizeof fs->dflt);
    if (fs->spec[0] == '\0')
        return SS$_BADPARAM;
    fs->fab = cc$rms_fab;
    fs->fab.fab$l_fna = fs->spec;
    fs->fab.fab$b_fns = (uint8_t)strlen(fs->spec);
    fs->fab.fab$l_dna = fs->dflt;
    fs->fab.fab$b_dns = (uint8_t)strlen(fs->dflt);
    fs->nam = cc$rms_nam;
    fs->nam.nam$l_esa = fs->esa;
    fs->nam.nam$b_ess = 255;
    fs->nam.nam$l_rsa = fs->rsa;
    fs->nam.nam$b_rss = 255;
    fs->fab.fab$l_nam = &fs->nam;
    return sys$parse(&fs->fab, 0, 0);
}

static void fc_name_dsc(struct dsc$descriptor_s *out, const char *s, size_t n)
{
    out->dsc$w_length = (uint16_t)n;
    out->dsc$b_dtype = DSC$K_DTYPE_T;
    out->dsc$b_class = DSC$K_CLASS_S;
    out->dsc$a_pointer = (char *)s;
}

static void fc_return_name(struct dsc$descriptor_s *dst, const char *s, size_t n)
{
    if (dst) {
        uint16_t len = (uint16_t)n;
        (void)lib$scopy_r_dx(&len, s, dst);
    }
}

uint32_t (lib$delete_file)(const struct dsc$descriptor_s *filespec,
                           const struct dsc$descriptor_s *default_filespec,
                           const struct dsc$descriptor_s *related_filespec,
                           uint32_t (*user_success_procedure)(const struct dsc$descriptor_s *, void *),
                           uint32_t (*user_error_procedure)(const struct dsc$descriptor_s *, uint32_t, uint32_t, void *),
                           uint32_t (*user_confirm_procedure)(const struct dsc$descriptor_s *, void *),
                           void *user_specified_argument,
                           struct dsc$descriptor_s *resultant_name,
                           uint32_t *file_scan_context,
                           const uint32_t *flags)
{
    (void)related_filespec; (void)flags; (void)file_scan_context;
    if (!filespec)
        return SS$_BADPARAM;

    struct fc_search fs;
    uint32_t st = fc_open_search(&fs, filespec, default_filespec);
    if (st != RMS$_NORMAL) {
        rms_search_end(&fs.nam);
        return st;
    }

    uint32_t result = RMS$_NORMAL;
    int matched = 0;
    for (;;) {
        st = sys$search(&fs.fab, 0, 0);
        if (st == RMS$_NMF || st == RMS$_FNF) {
            if (!matched)
                result = st;        /* nothing matched: the RMS verdict stands */
            break;
        }
        if (st != RMS$_NORMAL) { result = st; break; }
        matched++;

        struct dsc$descriptor_s nd;
        fc_name_dsc(&nd, fs.rsa, fs.nam.nam$b_rsl);
        fs.rsa[fs.nam.nam$b_rsl] = '\0';

        if (user_confirm_procedure &&
            !(user_confirm_procedure(&nd, user_specified_argument) & 1))
            continue;

        /* $ERASE a FAB naming exactly the matched file (resultant spec). */
        struct FAB ef = cc$rms_fab;
        ef.fab$l_fna = fs.rsa;
        ef.fab$b_fns = fs.nam.nam$b_rsl;
        uint32_t es = sys$erase(&ef, 0, 0);
        if (es & 1) {
            fc_return_name(resultant_name, fs.rsa, fs.nam.nam$b_rsl);
            if (user_success_procedure)
                (void)user_success_procedure(&nd, user_specified_argument);
        } else if (user_error_procedure) {
            (void)user_error_procedure(&nd, es, ef.fab$l_stv, user_specified_argument);
            result = es;
        } else {
            result = es;
            break;
        }
    }
    rms_search_end(&fs.nam);
    return result;
}

uint32_t (lib$rename_file)(const struct dsc$descriptor_s *old_filespec,
                           const struct dsc$descriptor_s *new_filespec,
                           const struct dsc$descriptor_s *default_filespec,
                           const struct dsc$descriptor_s *related_filespec,
                           const uint32_t *flags,
                           uint32_t (*user_success_procedure)(const struct dsc$descriptor_s *, const struct dsc$descriptor_s *, void *),
                           uint32_t (*user_error_procedure)(const struct dsc$descriptor_s *, const struct dsc$descriptor_s *, uint32_t, uint32_t, void *),
                           uint32_t (*user_confirm_procedure)(const struct dsc$descriptor_s *, const struct dsc$descriptor_s *, void *),
                           void *user_specified_argument,
                           struct dsc$descriptor_s *old_resultant_name,
                           struct dsc$descriptor_s *new_resultant_name,
                           uint32_t *file_scan_context)
{
    (void)related_filespec; (void)flags; (void)file_scan_context;
    if (!old_filespec || !new_filespec)
        return SS$_BADPARAM;

    char newspec[256];
    fc_dsc_to_str(new_filespec, newspec, sizeof newspec);
    if (newspec[0] == '\0')
        return SS$_BADPARAM;

    struct fc_search fs;
    uint32_t st = fc_open_search(&fs, old_filespec, default_filespec);
    if (st != RMS$_NORMAL) {
        rms_search_end(&fs.nam);
        return st;
    }

    uint32_t result = RMS$_NORMAL;
    int matched = 0;
    for (;;) {
        st = sys$search(&fs.fab, 0, 0);
        if (st == RMS$_NMF || st == RMS$_FNF) {
            if (!matched)
                result = st;
            break;
        }
        if (st != RMS$_NORMAL) { result = st; break; }
        matched++;
        fs.rsa[fs.nam.nam$b_rsl] = '\0';

        /* The new FAB defaults every field the new spec leaves out from the
         * OLD file's resultant name -- the RMS $RENAME rule. */
        struct NAM nn = cc$rms_nam;
        char nesa[256], nrsa[256];
        nn.nam$l_esa = nesa; nn.nam$b_ess = 255;
        nn.nam$l_rsa = nrsa; nn.nam$b_rss = 255;
        struct FAB nf = cc$rms_fab;
        nf.fab$l_fna = newspec;
        nf.fab$b_fns = (uint8_t)strlen(newspec);
        nf.fab$l_dna = fs.rsa;
        nf.fab$b_dns = fs.nam.nam$b_rsl;
        nf.fab$l_nam = &nn;

        struct FAB of = cc$rms_fab;
        of.fab$l_fna = fs.rsa;
        of.fab$b_fns = fs.nam.nam$b_rsl;

        struct dsc$descriptor_s od, ndsc;
        fc_name_dsc(&od, fs.rsa, fs.nam.nam$b_rsl);

        uint32_t rs = sys$parse(&nf, 0, 0);
        const char *newname = nn.nam$l_esa;
        size_t newlen = nn.nam$b_esl;
        if (rs & 1) {
            fc_name_dsc(&ndsc, newname, newlen);
            if (user_confirm_procedure &&
                !(user_confirm_procedure(&od, &ndsc, user_specified_argument) & 1)) {
                rms_search_end(&nn);
                continue;
            }
            rs = sys$rename(&of, 0, 0, &nf);
            if (rs & 1) {
                size_t rl = nn.nam$b_rsl ? nn.nam$b_rsl : newlen;
                const char *rn = nn.nam$b_rsl ? nn.nam$l_rsa : newname;
                fc_return_name(old_resultant_name, fs.rsa, fs.nam.nam$b_rsl);
                fc_return_name(new_resultant_name, rn, rl);
                fc_name_dsc(&ndsc, rn, rl);
                if (user_success_procedure)
                    (void)user_success_procedure(&od, &ndsc, user_specified_argument);
            }
        } else {
            fc_name_dsc(&ndsc, newspec, strlen(newspec));
        }
        rms_search_end(&nn);
        if (!(rs & 1)) {
            if (user_error_procedure) {
                (void)user_error_procedure(&od, &ndsc, rs, of.fab$l_stv,
                                           user_specified_argument);
                result = rs;
            } else {
                result = rs;
                break;
            }
        }
    }
    rms_search_end(&fs.nam);
    return result;
}

