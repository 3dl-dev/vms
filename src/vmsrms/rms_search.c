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
 * OVMX-USERSPACE: sys$search (vms-5b4) -- walks the host directory with
 *     readdir(2) and matches in this process; the wildcard context lives in
 *     nam$$l_context in the caller's own NAM block, so the enumeration is a
 *     private snapshot with no executive file-system interlock behind it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fnmatch.h>
#include "rms/rms.h"
#include "vmsfs/filespec.h"

/*
 * Internal: wildcard search context maintained across $SEARCH calls.
 */
struct search_context {
    DIR *dir;                   /* Open directory handle */
    char directory[1024];       /* Linux directory path being searched */
    char pattern[256];          /* fnmatch-compatible pattern */
    int  active;                /* Context is valid */
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
uint32_t sys$search(void *fab_ptr)
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

        /* Split into directory and filename pattern */
        const char *last_slash = strrchr(linux_path, '/');
        if (last_slash) {
            size_t dlen = (size_t)(last_slash - linux_path);
            if (dlen >= sizeof(ctx->directory)) {
                dlen = sizeof(ctx->directory) - 1;
            }
            memcpy(ctx->directory, linux_path, dlen);
            ctx->directory[dlen] = '\0';
            vms_to_glob(last_slash + 1, ctx->pattern, sizeof(ctx->pattern));
        } else {
            strcpy(ctx->directory, ".");
            vms_to_glob(linux_path, ctx->pattern, sizeof(ctx->pattern));
        }

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
