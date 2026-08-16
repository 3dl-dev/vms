/*
 * rms_search.c - RMS $SEARCH System Service
 *
 * Implements wildcard file searching. After $PARSE has established
 * the filespec and wildcard flags in the NAM block, $SEARCH
 * iterates through matching files using readdir() + pattern matching.
 *
 * The wildcard context is maintained in nam$$l_context between
 * successive $SEARCH calls. The context is freed when no more
 * matches are found (RMS$_NMF returned) or explicitly by calling
 * $PARSE again.
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * $SEARCH cited vms-5b4 until vms-fab; it is closed. vms-407 owns it with the
 * rest of RMS.
 *
 * OVMX-PARTIAL: sys$search (vms-96e2) -- exec: the VMS filespec is resolved to a
 *     Linux directory through the executive-resident LNM$SYSTEM table
 *     (vmsfs_to_linux_path -> lnm_translate -> vms_kif_lnm_translate) for system
 *     logical names before the directory walk begins.
 * OVMX-LOCAL: sys$search -- for a non-SYS$DISK directory it walks the host
 *     directory with readdir(2) and matches in this process; the wildcard
 *     context lives in nam$$l_context in the caller's own NAM block, so the
 *     enumeration is a private snapshot with no executive file-system interlock
 *     behind it.
 *
 * ODS-2 runtime flip (epic vms-5eb, rung R5b -- vms-dca): a SYS$DISK
 *     ("/vms/...") directory is enumerated through the genuine-ODS-2 volume
 *     handle (ods2_sysdisk_list_dir), so the names, VERSIONS and FIDs come from
 *     the real Master File Directory / FID chains, not opendir/readdir on the
 *     /vms passthrough. Fail-honest (Rule 9 / INV-6): with no ODS-2 SYS$DISK
 *     volume registered $SEARCH returns SS$_DEVNOTMOUNT -- never a silent POSIX
 *     fallback. Linux only; the netbsd-vax cross keeps the POSIX walk.
 */

#include <stdio.h>
#include "rms_internal.h"
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fnmatch.h>
#include "rms/rms.h"
#include "vmsfs/filespec.h"
#if defined(__linux__)
#include "vmsfs/sysdisk.h"   /* ODS-2 runtime flip (epic vms-5eb, rung R5b) */
#endif

/*
 * Internal: wildcard search context maintained across $SEARCH calls.
 */
#if defined(__linux__)
/*
 * ODS-2 runtime flip (epic vms-5eb, rung R5b -- vms-dca). One matched genuine-
 * ODS-2 directory record: its "NAME.EXT;version" filename (built from the real
 * Master File Directory / FID chains, NOT a POSIX d_name) and numeric version.
 */
struct ods2_match {
    char     vms_name[256];     /* "NAME.EXT;version" from the ODS-2 record */
    uint16_t version;
};
#endif

struct search_context {
    DIR *dir;                   /* Open directory handle (POSIX branch) */
    char directory[1024];       /* Linux directory path being searched */
    char pattern[256];          /* fnmatch-compatible pattern (POSIX branch) */
    int  active;                /* Context is valid */
#if defined(__linux__)
    /*
     * SYS$DISK ODS-2 branch. When the resolved directory is on the genuine-
     * ODS-2 SYS$DISK ("/vms/..."), the enumeration reads the real ODS-2
     * directory records through the volume handle instead of opendir/readdir
     * on the /vms passthrough. ods2_sysdisk_list_dir delivers every entry in
     * one callback sweep, so the wildcard-matched entries are collected up
     * front on the first call and returned one per subsequent call -- the
     * $SEARCH "next match per call" contract is preserved over match_index.
     */
    int   ods2;                 /* SYS$DISK ODS-2 branch active */
    struct ods2_match *matches; /* wildcard-matched ODS-2 records, in reader order */
    int   match_count;
    int   match_index;          /* next match to return */
#endif
};

/*
 * Convert a VMS wildcard pattern to an fnmatch-compatible pattern.
 *
 * VMS uses:
 *   * - match any sequence of characters
 *   % - match exactly one character
 *
 * fnmatch uses:
 *   * - match any sequence
 *   ? - match one character
 */
static void vms_to_glob(const char *vms, char *glob, size_t globlen)
{
    char *p = glob;
    char *end = glob + globlen - 1;

    while (*vms && p < end) {
        switch (*vms) {
            case '%':
                *p++ = '?';
                break;
            default:
                *p++ = *vms;
                break;
        }
        vms++;
    }
    *p = '\0';
}

#if defined(__linux__)
/*
 * ods2_collect_ctx / ods2_collect_cb - collect the genuine-ODS-2 directory
 * records of a SYS$DISK directory that match the $SEARCH VMS wildcard pattern,
 * exactly as DCL's DIRECTORY reroute (dir_collect_ods2, rung R3) does: every
 * record's name, VERSION and FID come from the real MFD/FID chains via
 * ods2_sysdisk_list_dir, and the SAME version-aware VMS matcher
 * (vmsfs_wildcard_match) filters them -- never fnmatch over a POSIX d_name.
 * Entries are appended in the reader's delivery order (no re-sort), so the
 * $SEARCH iteration order is the genuine ODS-2 directory order, not opendir's.
 */
struct ods2_collect_ctx {
    const char        *pattern;   /* raw VMS filename pattern (may carry ;ver) */
    struct ods2_match *matches;
    int                count;
    int                capacity;
    int                oom;
};

static int ods2_collect_cb(const char *name, unsigned name_len,
                           uint16_t version, const ods2_fid_t *fid, void *vctx)
{
    struct ods2_collect_ctx *c = (struct ods2_collect_ctx *)vctx;
    (void)fid;

    char ename[256];
    if (name_len >= sizeof(ename)) name_len = (unsigned)(sizeof(ename) - 1);
    memcpy(ename, name, name_len);
    ename[name_len] = '\0';

    /* Build "NAME.EXT;version" so the SAME version-aware VMS wildcard matcher
     * used across OVMX applies here unchanged (bare/absent version in the
     * pattern matches every version; an explicit ;N narrows to that one). */
    char matchname[300];
    snprintf(matchname, sizeof(matchname), "%s;%u", ename, (unsigned)version);

    if (c->pattern && *c->pattern && !vmsfs_wildcard_match(c->pattern, matchname))
        return 0;

    if (c->count >= c->capacity) {
        int nc = c->capacity ? c->capacity * 2 : 32;
        struct ods2_match *tmp = realloc(c->matches,
                                         (size_t)nc * sizeof(*c->matches));
        if (!tmp) { c->oom = 1; return 1; }   /* stop; caller reports RMS$_DME */
        c->matches = tmp;
        c->capacity = nc;
    }

    struct ods2_match *m = &c->matches[c->count++];
    memset(m, 0, sizeof(*m));
    strncpy(m->vms_name, matchname, sizeof(m->vms_name) - 1);
    m->version = version;
    return 0;
}
#endif /* __linux__ */

/*
 * sys$search - Find the next file matching a wildcard pattern.
 *
 * On the first call (no active context), initializes the search:
 *   1. Takes the expanded string from the NAM block
 *   2. Converts the VMS filespec to a Linux path
 *   3. Splits into directory and filename pattern
 *   4. Opens the directory with opendir()
 *
 * On subsequent calls, reads the next directory entry and checks
 * against the pattern using fnmatch().
 *
 * When a match is found:
 *   - Converts the Linux path back to a VMS filespec
 *   - Stores it in the NAM resultant string (nam$l_rsa)
 *   - Returns RMS$_NORMAL
 *
 * When no more matches exist:
 *   - Closes the directory
 *   - Frees the context
 *   - Returns RMS$_NMF
 *
 * Returns:
 *   RMS$_NORMAL - Match found
 *   RMS$_NMF    - No more files matching
 *   RMS$_FAB    - Invalid FAB
 *   RMS$_NAM    - Invalid NAM block
 *   RMS$_DNF    - Directory not found
 *   RMS$_DME    - Memory exhausted
 */
static uint32_t rms_impl_search(void *fab_ptr)
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

    struct search_context *ctx = (struct search_context *)nam->nam$$l_context;

    if (!ctx) {
        /*
         * First call - initialize the search context.
         */
        ctx = calloc(1, sizeof(struct search_context));
        if (!ctx) {
            fab->fab$l_sts = RMS$_DME;
            return RMS$_DME;
        }

        /* Get the expanded string from NAM (set by $PARSE) */
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

        /* Convert VMS filespec to Linux path */
        char linux_path[1024];
        if (vmsfs_to_linux_path(expanded, linux_path,
                                sizeof(linux_path)) < 0) {
            /* If conversion fails, try using expanded as-is */
            strncpy(linux_path, expanded, sizeof(linux_path) - 1);
            linux_path[sizeof(linux_path) - 1] = '\0';
        }

        /* Split into directory path and the (raw VMS) filename pattern. */
        const char *last_slash = strrchr(linux_path, '/');
        const char *filepart;
        if (last_slash) {
            size_t dlen = (size_t)(last_slash - linux_path);
            if (dlen >= sizeof(ctx->directory)) {
                dlen = sizeof(ctx->directory) - 1;
            }
            memcpy(ctx->directory, linux_path, dlen);
            ctx->directory[dlen] = '\0';
            filepart = last_slash + 1;
        } else {
            strcpy(ctx->directory, ".");
            filepart = linux_path;
        }

#if defined(__linux__)
        /*
         * ODS-2 runtime flip (epic vms-5eb, rung R5b -- vms-dca). A SYS$DISK
         * ("/vms/...") directory is enumerated through the genuine-ODS-2 volume
         * handle, NOT opendir/readdir on the /vms passthrough. Fail-honest
         * (Rule 9 / INV-6): with no ODS-2 SYS$DISK volume registered the list
         * returns SS$_DEVNOTMOUNT and this propagates it -- never a silent
         * POSIX fallback.
         */
        if (ods2_sysdisk_owns_path(ctx->directory)) {
            struct ods2_collect_ctx cc;
            memset(&cc, 0, sizeof(cc));
            cc.pattern = filepart;   /* raw VMS pattern (*, %, optional ;ver) */

            int st = ods2_sysdisk_list_dir(ctx->directory,
                                           ods2_collect_cb, &cc);
            if (st != SS$_NORMAL) {
                free(cc.matches);
                free(ctx);
                /* SS$_DEVNOTMOUNT (no volume), SS$_NOSUCHFILE (no such dir),
                 * SS$_BADPARAM -- surfaced honestly, no POSIX fallback. */
                fab->fab$l_sts = st;
                nam->nam$l_sts = st;
                return (uint32_t)st;
            }
            if (cc.oom) {
                free(cc.matches);
                free(ctx);
                fab->fab$l_sts = RMS$_DME;
                return RMS$_DME;
            }

            ctx->ods2 = 1;
            ctx->matches = cc.matches;
            ctx->match_count = cc.count;
            ctx->match_index = 0;
            ctx->active = 1;
            nam->nam$$l_context = ctx;
        } else
#endif
        {
            vms_to_glob(filepart, ctx->pattern, sizeof(ctx->pattern));

            /* Open the directory */
            ctx->dir = opendir(ctx->directory);
            if (!ctx->dir) {
                free(ctx);
                fab->fab$l_sts = RMS$_DNF;
                return RMS$_DNF;
            }

            ctx->active = 1;
            nam->nam$$l_context = ctx;
        }
    }

#if defined(__linux__)
    /*
     * SYS$DISK ODS-2 branch: return the next collected genuine-ODS-2 match
     * (name + version from the real directory records), one per call.
     */
    if (ctx->ods2) {
        if (ctx->match_index < ctx->match_count) {
            struct ods2_match *m = &ctx->matches[ctx->match_index++];

            /* Resolved "/vms/.../NAME.EXT;version" Linux path (for a following
             * $OPEN via _resolved_path). */
            char result_path[1024];
            snprintf(result_path, sizeof(result_path), "%s/%s",
                     ctx->directory, m->vms_name);

            /* Store the VMS resultant string in the NAM block. */
            if (nam->nam$l_rsa && nam->nam$b_rss > 0) {
                char vms_result[1024];
                if (vmsfs_to_vms_spec(result_path, vms_result,
                                      sizeof(vms_result)) < 0) {
                    strncpy(vms_result, result_path, sizeof(vms_result) - 1);
                    vms_result[sizeof(vms_result) - 1] = '\0';
                }
                size_t rlen = strlen(vms_result);
                if (rlen > nam->nam$b_rss) rlen = nam->nam$b_rss;
                memcpy(nam->nam$l_rsa, vms_result, rlen);
                nam->nam$b_rsl = (uint8_t)rlen;
            }

            snprintf(fab->_resolved_path, sizeof(fab->_resolved_path),
                     "%s/%s", ctx->directory, m->vms_name);

            fab->fab$l_sts = RMS$_NORMAL;
            nam->nam$l_sts = RMS$_NORMAL;
            return RMS$_NORMAL;
        }

        /* No more matches - clean up context. */
        free(ctx->matches);
        free(ctx);
        nam->nam$$l_context = NULL;

        fab->fab$l_sts = RMS$_NMF;
        nam->nam$l_sts = RMS$_NMF;
        return RMS$_NMF;
    }
#endif

    /* Iterate through directory entries looking for matches */
    struct dirent *entry;
    while ((entry = readdir(ctx->dir)) != NULL) {
        /* Skip hidden files (Unix convention) */
        if (entry->d_name[0] == '.') continue;

        /* Skip RMS sidecar files */
        if (strstr(entry->d_name, ".rms_meta") ||
            strstr(entry->d_name, ".rms_idx")) {
            continue;
        }

        /* Match against pattern (case-insensitive, VMS convention) */
        if (fnmatch(ctx->pattern, entry->d_name, FNM_CASEFOLD) == 0) {
            /* Match found - build full path */
            char result_path[1024];
            snprintf(result_path, sizeof(result_path), "%s/%s",
                     ctx->directory, entry->d_name);

            /* Store in NAM resultant string */
            if (nam->nam$l_rsa && nam->nam$b_rss > 0) {
                /* Convert back to VMS filespec */
                char vms_result[1024];
                if (vmsfs_to_vms_spec(result_path, vms_result,
                                      sizeof(vms_result)) < 0) {
                    /* Use Linux path if conversion fails */
                    strncpy(vms_result, result_path,
                            sizeof(vms_result) - 1);
                    vms_result[sizeof(vms_result) - 1] = '\0';
                }

                size_t rlen = strlen(vms_result);
                if (rlen > nam->nam$b_rss) rlen = nam->nam$b_rss;
                memcpy(nam->nam$l_rsa, vms_result, rlen);
                nam->nam$b_rsl = (uint8_t)rlen;
            }

            /* Also store the resolved Linux path in FAB for convenience */
            snprintf(fab->_resolved_path, sizeof(fab->_resolved_path),
                     "%s/%s", ctx->directory, entry->d_name);

            fab->fab$l_sts = RMS$_NORMAL;
            nam->nam$l_sts = RMS$_NORMAL;
            return RMS$_NORMAL;
        }
    }

    /* No more matches - clean up context */
    closedir(ctx->dir);
    free(ctx);
    nam->nam$$l_context = NULL;

    fab->fab$l_sts = RMS$_NMF;
    nam->nam$l_sts = RMS$_NMF;
    return RMS$_NMF;
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
