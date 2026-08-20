/*
 * rms_search.c - RMS $SEARCH System Service
 *
 * Implements wildcard file searching. After $PARSE has established the filespec
 * and wildcard flags in the NAM block, $SEARCH returns successive matching
 * files, one per call, maintaining a wildcard-continuation context between
 * calls (freed when the directory is exhausted -- RMS$_NMF -- or by a fresh
 * $PARSE).
 *
 * ============================================================================
 * vms-481 (epic vms-208): $SEARCH reaches the directory through the Files-11
 * (ODS-2) ACP, NOT a POSIX readdir() of the /vms passthrough.
 *
 * On __linux__ (the product runtime) $SEARCH now $ASSIGNs an executive channel
 * to the mounted volume, resolves the directory FID (rms_acp_resolve_did), and
 * drives IO$_ACPCONTROL (VMS_ACP_CTL_SEARCH) -- the wildcard FIB$L_WCC
 * directory context the ACP answers (vms-a0b): each call returns the NEXT
 * matching {name.type;version, FID} in genuine ODS-2 order (name ascending,
 * version descending -- NOT POSIX readdir order), ending SS$_NOMOREFILES when
 * the directory is exhausted. This is the exact substrate RMS $OPEN/$CREATE use
 * (rms_core.c) and the primitive the DCL DIRECTORY / F$SEARCH lexical layer on
 * (VSI OpenVMS I/O User's Reference Manual, "ACP-QIO Interface"). The matched
 * File ID is recorded on the search context (rms_search_fid) so a caller
 * (DIRECTORY /FULL) can emit the real FID.
 *
 * FAIL-HONEST (CLAUDE.md Rule 9 / INV-6): no ACP-mounted volume / no /dev/vms
 * => the real RMS/SS$ error (RMS$_DNF from SS$_DEVNOTMOUNT/NOSUCHDEV/etc), never
 * a silent POSIX fallback. The passthrough readdir path remains ONLY for the
 * non-__linux__ netbsd-vax standalone cross, until VAX's own ACP re-target
 * (vms-d5d).
 * ============================================================================
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * $SEARCH cited vms-5b4 until vms-fab; it is closed. vms-407 owns it with the
 * rest of RMS.
 *
 * OVMX-PARTIAL: sys$search (vms-481) -- exec: on the product runtime (__linux__)
 *     the wildcard directory context is answered by the executive-resident
 *     Files-11 ODS-2 ACP (IO$_ACPCONTROL over /dev/vms, vms_kif_acp_acpcontrol);
 *     the executive holds the continuation cursor on the $ASSIGNed channel and
 *     returns each match's genuine {name, version, FID}. (On the netbsd-vax
 *     standalone cross the pre-ACP POSIX passthrough stands in until vms-d5d.)
 * OVMX-LOCAL: sys$search -- splits the expanded filespec and formats the
 *     resultant "DEV:[DIR]NAME.TYP;VER" into the caller's NAM in this process;
 *     no wildcard-continuation state lives here on the __linux__ ACP path.
 */

#include <stdio.h>
#include "rms_internal.h"
#include <stdlib.h>
#include <string.h>
#include "rms/rms.h"

/* Both search backends are compiled below (vms-5f0). The POSIX passthrough is
 * the netbsd-vax cross's backend AND the __linux__ atomic-flip defer: when the
 * executive / /dev/vms is absent (host ctest, plain-container gates) $SEARCH
 * over the ACP would fail RMS$_DNF, so the dispatcher falls back to a readdir()
 * of the /vms passthrough, exactly as rms_impl_open/create/erase defer. When
 * /dev/vms IS present the ACP backend runs and stays fail-honest (INV-6). */
#include <dirent.h>
#include <fnmatch.h>
#include "vmsfs/filespec.h"
#include "vmsfs/device.h"   /* vms-3a8: vmsfs_compose_ods2_candidates for $SEARCH */

/* First member of BOTH $SEARCH context layouts, so the rms_impl_search /
 * rms_search_fid / rms_search_end dispatchers can route a continuation call to
 * the backend that opened the context (0 = ACP, 1 = POSIX). */
struct search_ctx_hdr { int is_posix; };

static uint32_t rms_posix_search(void *fab_ptr);
static int      rms_posix_search_fid(void *nam_ptr, uint16_t *num, uint16_t *seq,
                                     uint8_t *rvn, uint8_t *nmx);
static void     rms_posix_search_end(void *nam_ptr);
#if defined(__linux__)
static uint32_t rms_acp_search(void *fab_ptr);
static int      rms_acp_search_fid(void *nam_ptr, uint16_t *num, uint16_t *seq,
                                   uint8_t *rvn, uint8_t *nmx);
static void     rms_acp_search_end(void *nam_ptr);
#endif

#if defined(__linux__)

#include "vms_kif.h"     /* Files-11 ODS-2 ACP over /dev/vms (vms_kif_acp_*) */
#include "ssdef.h"

/*
 * Internal: wildcard search context maintained across $SEARCH calls. On Linux
 * the wildcard-continuation cursor lives in the executive (keyed on the
 * $ASSIGNed channel); this context only holds the channel plus the pieces
 * needed to reconstruct the resultant VMS spec and the last matched FID.
 */
struct acp_search_context {
    int      is_posix;          /* search_ctx_hdr discriminator: 0 == ACP     */
    uint32_t chan;              /* executive channel $ASSIGNed to the volume */
    int      assigned;          /* 1 once the channel is $ASSIGNed           */
    int      opened;            /* 1 once the wildcard context is (re)opened  */
    char     devnam[16];        /* "DKA0:"  (for the resultant spec)         */
    char     dirpath[256];      /* "A.B.C"  (raw, no brackets)               */
    /* Last match FID -- so DIRECTORY /FULL can emit the real File ID. */
    uint16_t fid_num, fid_seq;
    uint8_t  fid_rvn, fid_nmx;
    uint16_t version;
};

/*
 * Split a VMS filespec (the $PARSE-expanded string) into device / directory /
 * wildcard-pattern for the ACP search. Unlike rms_acp_spec_from_fab (rms_core.c)
 * this PRESERVES the whole "NAME.TYPE;VER" tail (including any version wildcard)
 * as the pattern the ACP parses -- the ACP's own wildcard-version semantics
 * (no ";"/";*" => all versions; ";0" => highest; ";N" => exact) apply to it.
 * VMS "%" (one char) is passed through as-is (the ACP takes "*"/"%").
 */
static void search_split(const char *spec, char *dev, size_t devsz,
                         char *dir, size_t dirsz, char *pat, size_t patsz)
{
    const char *p = spec;
    const char *lb, *rb, *colon;

    dev[0] = '\0'; dir[0] = '\0'; pat[0] = '\0';

    /* DEVICE: text before the first ':' that precedes any '['. Keep the ':'. */
    lb = strchr(spec, '[');
    colon = strchr(spec, ':');
    if (colon && (!lb || colon < lb)) {
        size_t dl = (size_t)(colon - spec) + 1;
        if (dl < devsz) { memcpy(dev, spec, dl); dev[dl] = '\0'; }
        p = colon + 1;
    }

    /* DIRECTORY: text inside [ ] (or < >). */
    lb = strchr(p, '[');
    if (!lb) lb = strchr(p, '<');
    rb = lb ? (lb[0] == '[' ? strchr(lb, ']') : strchr(lb, '>')) : NULL;
    if (lb && rb && rb > lb + 1) {
        size_t dl = (size_t)(rb - lb - 1);
        if (dl >= dirsz) dl = dirsz - 1;
        memcpy(dir, lb + 1, dl);
        dir[dl] = '\0';
        p = rb + 1;
    }

    /* PATTERN: the remainder ("NAME.TYPE;VER"), upcased (ODS-2 is case-
     * insensitive; the on-disk records the codec decodes are upper-case). */
    {
        size_t i = 0;
        for (; p[i] && i < patsz - 1; i++) {
            char c = p[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            pat[i] = c;
        }
        pat[i] = '\0';
    }
}

/* Release the executive channel behind a search context and free it. */
static void search_ctx_free(struct acp_search_context *ctx)
{
    if (!ctx) return;
    if (ctx->assigned) vms_kif_dassgn(ctx->chan);
    free(ctx);
}

/* Map an ACP SS$_ status to the RMS $SEARCH status (fail-honest). */
static uint32_t rms_acp_search_status(uint32_t ss)
{
    switch (ss) {
        case SS$_NOMOREFILES: return RMS$_NMF;
        case SS$_NOSUCHFILE:  return RMS$_DNF;   /* directory not found        */
        case SS$_NOSUCHDEV:   return RMS$_DNF;   /* no such device (no ACP)    */
        case SS$_DEVNOTMOUNT: return RMS$_DNF;   /* nothing mounted on SYS$DISK*/
        case SS$_IVCHAN:      return RMS$_DNF;
        default:              return RMS$_DNF;
    }
}

static uint32_t rms_acp_search(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    struct NAM *nam = fab->fab$l_nam;
    if (!nam || nam->nam$b_bid != NAM$C_BID) {
        fab->fab$l_sts = RMS$_NAM;
        return RMS$_NAM;
    }

    struct acp_search_context *ctx = (struct acp_search_context *)nam->nam$$l_context;
    struct vms_acp_acpcontrol_args a;
    uint32_t st;

    if (!ctx) {
        /* First call -- $ASSIGN the volume, resolve the directory FID, and
         * (re)open the wildcard context with the expanded pattern. */
        char expanded[1024] = "";
        char pattern[VMS_ACP_NAME_SIZE];
        uint16_t dn, ds; uint8_t dr, dx;

        if (nam->nam$l_esa && nam->nam$b_esl > 0) {
            size_t len = nam->nam$b_esl;
            if (len >= sizeof(expanded)) len = sizeof(expanded) - 1;
            memcpy(expanded, nam->nam$l_esa, len);
            expanded[len] = '\0';
        } else {
            fab->fab$l_sts = RMS$_SYN;
            return RMS$_SYN;
        }

        ctx = calloc(1, sizeof(struct acp_search_context));
        if (!ctx) { fab->fab$l_sts = RMS$_DME; return RMS$_DME; }

        /* Resolve any concealed / rooted / directory device logical
         * (SYS$SYSTEM:, SYS$COMMON:[SYSEXE], SYS$SYSROOT: ...) in the expanded
         * spec into FULLY-COMPOSED physical ODS-2 candidate specs, EXACTLY as
         * the $OPEN/$CREATE path does (rms_acp_specs_from_fab ->
         * vmsfs_compose_ods2_candidates), instead of handing the still-concealed
         * device to the ACP. sys$parse KEEPS a concealed device concealed
         * (rms_parse.c), so before this DIRECTORY / F$SEARCH / $SEARCH on
         * SYS$SYSTEM:DCL.EXE $ASSIGNed a channel to the non-physical unit
         * "SYS$SYSTEM", resolved no directory, and matched nothing
         * (%DIRECT-W-NOFILES) -- while the very same file opened fine BY NAME
         * (boot activation, PRODUCT, @STARTUP.COM all worked). vms-3a8.
         * Candidates come back in search-list order (node member, then
         * SYSCOMMON); use the first whose directory the ACP resolves. */
        {
            #define VMS_SEARCH_MAX_CANDS 4
            char cands[VMS_SEARCH_MAX_CANDS][VMSFS_MAX_FILESPEC];
            int ncand = vmsfs_compose_ods2_candidates(expanded, cands,
                                                      VMS_SEARCH_MAX_CANDS);
            int chosen = 0;

            if (ncand <= 0) {
                /* No device logical to compose (device-less / plain physical
                 * spec): keep the pre-vms-3a8 behaviour on the expanded spec. */
                strncpy(cands[0], expanded, sizeof(cands[0]) - 1);
                cands[0][sizeof(cands[0]) - 1] = '\0';
                ncand = 1;
            }

            st = SS$_NOSUCHFILE;
            for (int ci = 0; ci < ncand; ci++) {
                char cdev[16], cdir[256], cpat[VMS_ACP_NAME_SIZE];
                search_split(cands[ci], cdev, sizeof(cdev), cdir, sizeof(cdir),
                             cpat, sizeof(cpat));
                if (cdev[0] == '\0')
                    strncpy(cdev, RMS_ACP_DEFAULT_DEV, sizeof(cdev) - 1);
                if (cpat[0] == '\0')
                    continue;

                st = vms_kif_acp_assign(cdev, &ctx->chan);
                if (!$VMS_STATUS_SUCCESS(st))
                    continue;                 /* try the next candidate */
                ctx->assigned = 1;

                st = rms_acp_resolve_did(ctx->chan, cdir, &dn, &ds, &dr, &dx);
                if ($VMS_STATUS_SUCCESS(st)) {
                    strncpy(ctx->devnam, cdev, sizeof(ctx->devnam) - 1);
                    ctx->devnam[sizeof(ctx->devnam) - 1] = '\0';
                    strncpy(ctx->dirpath, cdir, sizeof(ctx->dirpath) - 1);
                    ctx->dirpath[sizeof(ctx->dirpath) - 1] = '\0';
                    strncpy(pattern, cpat, sizeof(pattern) - 1);
                    pattern[sizeof(pattern) - 1] = '\0';
                    chosen = 1;
                    break;
                }
                /* Directory did not resolve on this member: release and retry. */
                vms_kif_dassgn(ctx->chan);
                ctx->assigned = 0; ctx->chan = 0;
            }

            if (!chosen) {
                uint32_t rs = rms_acp_search_status(st);
                search_ctx_free(ctx); nam->nam$$l_context = NULL;
                fab->fab$l_sts = rs; nam->nam$l_sts = rs; return rs;
            }
            #undef VMS_SEARCH_MAX_CANDS
        }

        memset(&a, 0, sizeof(a));
        a.chan = ctx->chan;
        a.func = VMS_ACP_CTL_SEARCH;
        a.did_num = dn; a.did_seq = ds; a.did_rvn = dr; a.did_nmx = dx;
        a.wcc_reset = 1;                              /* (re)open the context */
        strncpy(a.pattern, pattern, VMS_ACP_NAME_SIZE - 1);
        ctx->opened = 1;
        nam->nam$$l_context = ctx;
    } else {
        /* Subsequent call -- CONTINUE the executive-held wildcard context. */
        memset(&a, 0, sizeof(a));
        a.chan = ctx->chan;
        a.func = VMS_ACP_CTL_SEARCH;
        a.wcc_reset = 0;
    }

    st = vms_kif_acp_acpcontrol(&a);
    if (st != SS$_NORMAL) {
        /* SS$_NOMOREFILES => RMS$_NMF (and end the context); any other => the
         * fail-honest RMS status. Either way the context is spent. */
        uint32_t rs = rms_acp_search_status(st);
        search_ctx_free(ctx);
        nam->nam$$l_context = NULL;
        fab->fab$l_sts = rs;
        nam->nam$l_sts = rs;
        return rs;
    }

    /* Match found. Record the real FID (DIRECTORY /FULL reads it back). */
    ctx->fid_num = a.fid_num; ctx->fid_seq = a.fid_seq;
    ctx->fid_rvn = a.fid_rvn; ctx->fid_nmx = a.fid_nmx;
    ctx->version = a.out_version;

    /* Build the resultant VMS spec "DEV:[DIR]NAME.TYPE;VER" from the ACP's
     * resultant name (a.resnam == "NAME.TYPE;VERSION"). */
    {
        char vms_result[1024];
        int n;
        if (ctx->dirpath[0])
            n = snprintf(vms_result, sizeof(vms_result), "%s[%s]%s",
                         ctx->devnam, ctx->dirpath, a.resnam);
        else
            n = snprintf(vms_result, sizeof(vms_result), "%s[000000]%s",
                         ctx->devnam, a.resnam);
        if (n < 0) n = 0;

        if (nam->nam$l_rsa && nam->nam$b_rss > 0) {
            size_t rlen = strlen(vms_result);
            if (rlen > nam->nam$b_rss) rlen = nam->nam$b_rss;
            memcpy(nam->nam$l_rsa, vms_result, rlen);
            nam->nam$b_rsl = (uint8_t)rlen;
        }
        /* Also record it on the FAB (the ACP resolves by name from here on;
         * this is the VMS spec, not a Linux path). */
        strncpy(fab->_resolved_path, vms_result, sizeof(fab->_resolved_path) - 1);
        fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';
    }

    fab->fab$l_sts = RMS$_NORMAL;
    nam->nam$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
}

/*
 * rms_search_fid - read the File ID of the most recent $SEARCH match from a NAM
 * block's active wildcard context (DIRECTORY /FULL emits the real FID). Returns
 * 1 and fills the num/seq/rvn/nmx outputs if a match FID is available, else 0.
 */
static int rms_acp_search_fid(void *nam_ptr, uint16_t *num, uint16_t *seq,
                              uint8_t *rvn, uint8_t *nmx)
{
    struct NAM *nam = (struct NAM *)nam_ptr;
    if (!nam || nam->nam$b_bid != NAM$C_BID || !nam->nam$$l_context)
        return 0;
    struct acp_search_context *ctx = (struct acp_search_context *)nam->nam$$l_context;
    if (num) *num = ctx->fid_num;
    if (seq) *seq = ctx->fid_seq;
    if (rvn) *rvn = ctx->fid_rvn;
    if (nmx) *nmx = ctx->fid_nmx;
    return 1;
}

/*
 * rms_acp_search_end - release a NAM block's wildcard search context WITHOUT
 * running it to exhaustion. A caller that stops iterating early (DIRECTORY found
 * what it needed) uses this to $DASSGN the executive channel the context holds
 * -- the normal RMS$_NMF path frees it, but an early stop would otherwise leak.
 */
static void rms_acp_search_end(void *nam_ptr)
{
    struct NAM *nam = (struct NAM *)nam_ptr;
    if (!nam || nam->nam$b_bid != NAM$C_BID || !nam->nam$$l_context)
        return;
    search_ctx_free((struct acp_search_context *)nam->nam$$l_context);
    nam->nam$$l_context = NULL;
}

#endif /* __linux__ ACP $SEARCH backend */

/* ==========================================================================
 * POSIX $SEARCH backend (readdir of the /vms passthrough): the netbsd-vax
 * cross's backend AND the __linux__ vms-5f0 legacy defer (executive absent).
 * ========================================================================== */

struct posix_search_context {
    int  is_posix;              /* search_ctx_hdr discriminator: 1 == POSIX */
    DIR *dir;                   /* Open directory handle */
    char directory[1024];       /* Linux directory path being searched */
    char pattern[256];          /* fnmatch-compatible pattern */
    char vms_prefix[600];       /* "DEV:[DIR]" from the expanded spec, so the   */
                                /* resultant is DEV:[DIR]NAME (vms-5f0) rather  */
                                /* than round-tripped through vmsfs_to_vms_spec */
                                /* (which maps /vms/X/Y -> X:[000000]Y).        */
    int  active;                /* Context is valid */
};

/* Convert a VMS wildcard pattern ("*" any, "%" one) to fnmatch ("*"/"?"). */
static void vms_to_glob(const char *vms, char *glob, size_t globlen)
{
    char *p = glob;
    char *end = glob + globlen - 1;
    while (*vms && p < end) {
        *p++ = (*vms == '%') ? '?' : *vms;
        vms++;
    }
    *p = '\0';
}

static uint32_t rms_posix_search(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    struct NAM *nam = fab->fab$l_nam;
    if (!nam || nam->nam$b_bid != NAM$C_BID) {
        fab->fab$l_sts = RMS$_NAM;
        return RMS$_NAM;
    }

    struct posix_search_context *ctx =
        (struct posix_search_context *)nam->nam$$l_context;

    if (!ctx) {
        ctx = calloc(1, sizeof(struct posix_search_context));
        if (!ctx) { fab->fab$l_sts = RMS$_DME; return RMS$_DME; }
        ctx->is_posix = 1;

        char expanded[1024] = "";
        if (nam->nam$l_esa && nam->nam$b_esl > 0) {
            size_t len = nam->nam$b_esl;
            if (len >= sizeof(expanded)) len = sizeof(expanded) - 1;
            memcpy(expanded, nam->nam$l_esa, len);
            expanded[len] = '\0';
        } else {
            free(ctx);
            fab->fab$l_sts = RMS$_SYN;
            return RMS$_SYN;
        }

        /* Preserve the "DEV:[DIR]" of the expanded VMS spec (up to and including
         * the closing ']') so the $SEARCH resultant is composed as
         * DEV:[DIR]NAME -- the genuine device/directory the caller searched, not
         * vmsfs_to_vms_spec's positional guess. */
        {
            const char *rb = strchr(expanded, ']');
            if (rb) {
                size_t pl = (size_t)(rb - expanded) + 1;
                if (pl >= sizeof(ctx->vms_prefix)) pl = sizeof(ctx->vms_prefix) - 1;
                memcpy(ctx->vms_prefix, expanded, pl);
                ctx->vms_prefix[pl] = '\0';
            }
        }

        char linux_path[1024];
        int split_done = 0;
        /* The file part after the directory terminator ']' (empty if none). */
        const char *filepart = strchr(expanded, ']');
        filepart = filepart ? filepart + 1 : "";
        int file_wild = (strchr(filepart, '*') || strchr(filepart, '%'));

        /* vms-5f0 executive-absent WILDCARD defer: for a wildcard file part,
         * translate the DIRECTORY component ALONE, then glob the file part
         * separately. vmsfs_to_linux_path case-folds "DEV:[DIR]" and
         * "DEV:[DIR]NAME.TYP" against the real on-disk directory, but does NOT
         * case-fold the directory when the spec carries a WILDCARD file part
         * ("DEV:[DIR]*.*;*" -> "/vms/DIR/*.*", directory left upper-cased).
         * Passing the whole wildcard spec would then opendir an upper-cased
         * "/vms/DIR" that ENOENTs against a lower-case POSIX dir. A CONCRETE
         * (non-wildcard) spec is left to the whole-spec translate below, which
         * case-folds the whole path (directory AND name) and correctly resolves
         * rooted "[000000]" logicals. The pre-flip DCL DIRECTORY path
         * (dcl_resolve_path) listed the real dir either way; reproduce that so
         * the defer lists the SAME files. (Only the executive-absent POSIX
         * backend runs this; the /dev/vms-present path is rms_acp_search,
         * untouched -- INV-6.) */
        if (file_wild && ctx->vms_prefix[0] &&
            (vmsfs_to_linux_path(ctx->vms_prefix, linux_path,
                                 sizeof(linux_path)) & 1u)) {
            size_t dl = strlen(linux_path);
            while (dl > 1 && linux_path[dl - 1] == '/') linux_path[--dl] = '\0';
            if (dl >= sizeof(ctx->directory)) dl = sizeof(ctx->directory) - 1;
            memcpy(ctx->directory, linux_path, dl);
            ctx->directory[dl] = '\0';
            /* File pattern = the text after the directory terminator ']', with
             * the ";version" dropped (the /vms passthrough stores plain names,
             * no version field -- the classic path dropped it via
             * vmsfs_to_linux_path). vms_to_glob only maps '%'->'?', so strip the
             * version here or a trailing ";*" would never fnmatch a bare name. */
            const char *fp = strchr(expanded, ']');
            fp = fp ? fp + 1 : expanded;
            char filepat[256];
            size_t fi = 0;
            for (; fp[fi] && fp[fi] != ';' && fi < sizeof(filepat) - 1; fi++)
                filepat[fi] = fp[fi];
            filepat[fi] = '\0';
            if (filepat[0] == '\0')
                strncpy(filepat, "*.*", sizeof(filepat) - 1);
            vms_to_glob(filepat, ctx->pattern, sizeof(ctx->pattern));
            split_done = 1;
        }

        if (!split_done) {
            /* No "DEV:[DIR]" prefix (device-only or bare spec): translate the
             * whole spec and split on the last slash, as before. vmsfs_to_linux_path
             * returns a VMS status code (odd == success), NOT 0-on-success -- the
             * old `< 0` check never fired (the project's vmsfs_to_linux_path
             * gotcha). */
            if (!(vmsfs_to_linux_path(expanded, linux_path,
                                      sizeof(linux_path)) & 1u)) {
                strncpy(linux_path, expanded, sizeof(linux_path) - 1);
                linux_path[sizeof(linux_path) - 1] = '\0';
            }
            const char *last_slash = strrchr(linux_path, '/');
            if (last_slash) {
                size_t dlen = (size_t)(last_slash - linux_path);
                if (dlen >= sizeof(ctx->directory)) dlen = sizeof(ctx->directory) - 1;
                memcpy(ctx->directory, linux_path, dlen);
                ctx->directory[dlen] = '\0';
                vms_to_glob(last_slash + 1, ctx->pattern, sizeof(ctx->pattern));
            } else {
                strcpy(ctx->directory, ".");
                vms_to_glob(linux_path, ctx->pattern, sizeof(ctx->pattern));
            }
        }

        ctx->dir = opendir(ctx->directory);
        if (!ctx->dir) {
            free(ctx);
            fab->fab$l_sts = RMS$_DNF;
            return RMS$_DNF;
        }
        ctx->active = 1;
        nam->nam$$l_context = ctx;
    }

    struct dirent *entry;
    while ((entry = readdir(ctx->dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (strstr(entry->d_name, ".rms_meta") ||
            strstr(entry->d_name, ".rms_idx")) {
            continue;
        }
        if (fnmatch(ctx->pattern, entry->d_name, FNM_CASEFOLD) == 0) {
            /* Compose the resultant VMS spec DEV:[DIR]NAME (name upper-cased --
             * VMS specs are upper-case; the on-disk entry is lower-case). Prefer
             * the searched device/directory (ctx->vms_prefix); only when it is
             * unknown fall back to vmsfs_to_vms_spec on the Linux path. */
            char vms_result[1024];
            if (ctx->vms_prefix[0]) {
                char upname[512];
                size_t i = 0;
                for (; entry->d_name[i] && i < sizeof(upname) - 1; i++) {
                    char c = entry->d_name[i];
                    upname[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
                }
                upname[i] = '\0';
                snprintf(vms_result, sizeof(vms_result), "%s%s",
                         ctx->vms_prefix, upname);
            } else {
                char result_path[1024];
                snprintf(result_path, sizeof(result_path), "%s/%s",
                         ctx->directory, entry->d_name);
                if (!(vmsfs_to_vms_spec(result_path, vms_result,
                                        sizeof(vms_result)) & 1u)) {
                    strncpy(vms_result, result_path, sizeof(vms_result) - 1);
                    vms_result[sizeof(vms_result) - 1] = '\0';
                }
            }
            if (nam->nam$l_rsa && nam->nam$b_rss > 0) {
                size_t rlen = strlen(vms_result);
                if (rlen > nam->nam$b_rss) rlen = nam->nam$b_rss;
                memcpy(nam->nam$l_rsa, vms_result, rlen);
                nam->nam$b_rsl = (uint8_t)rlen;
            }
            /* The record engines resolve _resolved_path via the VMS spec (RMS
             * re-parses it), so store the composed VMS spec, not a Linux path. */
            strncpy(fab->_resolved_path, vms_result,
                    sizeof(fab->_resolved_path) - 1);
            fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';
            fab->fab$l_sts = RMS$_NORMAL;
            nam->nam$l_sts = RMS$_NORMAL;
            return RMS$_NORMAL;
        }
    }

    closedir(ctx->dir);
    free(ctx);
    nam->nam$$l_context = NULL;
    fab->fab$l_sts = RMS$_NMF;
    nam->nam$l_sts = RMS$_NMF;
    return RMS$_NMF;
}

/* No executive FID on the POSIX passthrough -- the resultant filespec carries
 * no genuine File ID (DIRECTORY /FULL omits it), so report "unavailable". */
static int rms_posix_search_fid(void *nam_ptr, uint16_t *num, uint16_t *seq,
                                uint8_t *rvn, uint8_t *nmx)
{
    (void)nam_ptr; (void)num; (void)seq; (void)rvn; (void)nmx;
    return 0;
}

/* POSIX passthrough: free the readdir context + handle if the caller stops early. */
static void rms_posix_search_end(void *nam_ptr)
{
    struct NAM *nam = (struct NAM *)nam_ptr;
    if (!nam || nam->nam$b_bid != NAM$C_BID || !nam->nam$$l_context)
        return;
    struct posix_search_context *ctx =
        (struct posix_search_context *)nam->nam$$l_context;
    if (ctx->dir) closedir(ctx->dir);
    free(ctx);
    nam->nam$$l_context = NULL;
}

/* ==========================================================================
 * Public $SEARCH dispatchers: route each call to the ACP or POSIX backend
 * (vms-5f0). A continuation call (context already open) routes by the context's
 * is_posix tag; a first call probes the executive on __linux__ -- SS$_NOSUCHDEV
 * (no /dev/vms) defers to POSIX, otherwise the ACP backend runs. On non-__linux__
 * there is only the POSIX backend.
 * ========================================================================== */

static uint32_t rms_impl_search(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
#if defined(__linux__)
    if (fab && fab->fab$b_bid == FAB$C_BID) {
        struct NAM *nam = fab->fab$l_nam;
        if (nam && nam->nam$b_bid == NAM$C_BID && nam->nam$$l_context) {
            /* Continuation: stay on the backend that opened this context. */
            return ((struct search_ctx_hdr *)nam->nam$$l_context)->is_posix
                       ? rms_posix_search(fab_ptr) : rms_acp_search(fab_ptr);
        }
    }
    /* First call: probe the executive. SS$_NOSUCHDEV -- and ONLY that -- means
     * /dev/vms is absent (userspace KIF); an unmounted or non-DKA0: unit with
     * the executive present comes back SS$_DEVNOTMOUNT and stays on the ACP path
     * (vms-03b: the probe reflects /dev/vms PRESENCE, never DKA0:'s mount state,
     * so an unmounted unit never masquerades into the /vms passthrough). */
    {
        uint32_t pchan = 0;
        uint32_t pst = vms_kif_acp_assign(RMS_ACP_DEFAULT_DEV, &pchan);
        if (pst == SS$_NOSUCHDEV)
            return rms_posix_search(fab_ptr);
        if ($VMS_STATUS_SUCCESS(pst))
            vms_kif_dassgn(pchan);
    }
    return rms_acp_search(fab_ptr);
#else
    return rms_posix_search(fab_ptr);
#endif
}

int rms_search_fid(void *nam_ptr, uint16_t *num, uint16_t *seq,
                   uint8_t *rvn, uint8_t *nmx)
{
#if defined(__linux__)
    struct NAM *nam = (struct NAM *)nam_ptr;
    if (nam && nam->nam$b_bid == NAM$C_BID && nam->nam$$l_context &&
        ((struct search_ctx_hdr *)nam->nam$$l_context)->is_posix == 0)
        return rms_acp_search_fid(nam_ptr, num, seq, rvn, nmx);
#endif
    return rms_posix_search_fid(nam_ptr, num, seq, rvn, nmx);
}

void rms_search_end(void *nam_ptr)
{
#if defined(__linux__)
    struct NAM *nam = (struct NAM *)nam_ptr;
    if (nam && nam->nam$b_bid == NAM$C_BID && nam->nam$$l_context &&
        ((struct search_ctx_hdr *)nam->nam$$l_context)->is_posix == 0) {
        rms_acp_search_end(nam_ptr);
        return;
    }
#endif
    rms_posix_search_end(nam_ptr);
}


/* ============================================================
 * Public RMS entry points: VMS three-argument form
 *   SYS$xxx cb ,[err] ,[suc]   (VSI OpenVMS RMS Reference, Part III)
 * Thin wrappers over the synchronous rms_impl_* bodies above that
 * dispatch the optional AST-level completion routine (rms_complete).
 * ============================================================ */
uint32_t sys$search(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_search(fab), fab, err, suc);
}
