/*
 * vmsfs_translate.c - VMS Filespec <-> Linux Path Translation
 *
 * Translates between VMS-style filespecs and Linux paths:
 *   NODE::DEVICE:[DIR.SUBDIR]NAME.TYPE;VERSION
 *
 * Examples:
 *   SYS$LOGIN:LOGIN.COM;1  ->  /home/user/login.com;1
 *   [DIR.SUB]FILE.TXT;3    ->  /vms/root/dir/sub/FILE.TXT;3
 *   DKA0:[USERS]FOO.DAT    ->  /vms/dka0/users/FOO.DAT
 *
 * OVMX Project - VMS Filesystem Semantics Layer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "vmsfs/filespec.h"
#include "vmsfs/device.h"
#include "vmsfs/version.h"
#include "ovmx_layout.h"
#include "ssdef.h"
#include "rmsdef.h"
#include "lnmdef.h"
#include "vms/logical.h"

/* Default VMS root on Linux filesystem — fallback only */
static const char *vms_default_root = SYSDISK_MOUNT;

/* Forward declaration for case-insensitive lookup (from vmsfs_case.c) */
extern int vmsfs_find_case_insensitive(const char *dir_path, const char *name,
                                        char *result, size_t result_size);
extern int vmsfs_resolve_path_case(const char *full_path, char *resolved,
                                    size_t resolved_size);

/*
 * Check if a string contains VMS wildcard characters.
 */
static int contains_wildcard(const char *s)
{
    if (!s) return 0;
    while (*s) {
        if (*s == '*' || *s == '%' || *s == '?') return 1;
        /* VMS ellipsis wildcard [...] */
        if (*s == '.' && *(s + 1) == '.' && *(s + 2) == '.') return 1;
        s++;
    }
    return 0;
}

/*
 * Convert a string to uppercase in place.
 */
static void str_upcase(char *s)
{
    while (*s) {
        *s = (char)toupper((unsigned char)*s);
        s++;
    }
}

/*
 * Convert a string to lowercase in place.
 */
static void str_downcase(char *s)
{
    while (*s) {
        *s = (char)tolower((unsigned char)*s);
        s++;
    }
}

/*
 * vmsfs_parse_filespec - Parse a VMS filespec string into components.
 *
 * Format: NODE::DEVICE:[DIRECTORY]NAME.TYPE;VERSION
 *
 * All parts are optional. The parser handles:
 *   - NODE:: prefix (double colon)
 *   - DEVICE: prefix (single colon, not part of ::)
 *   - [DIRECTORY] or [DIR.SUBDIR.SUB2] in brackets
 *   - NAME.TYPE filename with extension
 *   - ;VERSION suffix
 *
 * Returns SS$_NORMAL on success, RMS$_SYN on syntax error.
 */
int vmsfs_parse_filespec(const char *spec, vmsfs_filespec_t *result)
{
    if (!spec || !result) {
        return SS$_BADPARAM;
    }

    memset(result, 0, sizeof(*result));
    result->version = -1;  /* Not specified */

    const char *p = spec;
    const char *end_of_spec = spec + strlen(spec);

    /* Skip leading whitespace */
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) {
        return SS$_NORMAL;  /* Empty spec is valid (all defaults) */
    }

    /*
     * Step 1: Look for NODE:: (double colon).
     * The node name is everything before the first "::".
     */
    const char *dcolon = strstr(p, "::");
    if (dcolon) {
        size_t len = (size_t)(dcolon - p);
        if (len > VMSFS_MAX_NODE) {
            return RMS$_SYN;
        }
        memcpy(result->node, p, len);
        result->node[len] = '\0';
        str_upcase(result->node);
        result->has_node = 1;
        if (contains_wildcard(result->node)) {
            result->is_wildcard = 1;
        }
        p = dcolon + 2;  /* Skip past :: */
    }

    /*
     * Step 2: Look for DEVICE: (single colon, not part of ::).
     * Scan forward for a colon that is not followed by another colon
     * and is before any bracket or other delimiter.
     */
    const char *colon = NULL;
    for (const char *s = p; s < end_of_spec; s++) {
        if (*s == '[' || *s == '<') break;  /* Directory starts - no device */
        if (*s == ':') {
            /* Make sure it's not part of :: */
            if (s + 1 < end_of_spec && *(s + 1) == ':') {
                break;  /* This is a :: which shouldn't happen after node parse */
            }
            colon = s;
            break;
        }
    }

    if (colon) {
        size_t len = (size_t)(colon - p);
        if (len > VMSFS_MAX_DEVICE) {
            return RMS$_SYN;
        }
        memcpy(result->device, p, len);
        result->device[len] = '\0';
        str_upcase(result->device);
        result->has_device = 1;
        if (contains_wildcard(result->device)) {
            result->is_wildcard = 1;
        }
        p = colon + 1;  /* Skip past : */
    }

    /*
     * Step 3: Look for [DIRECTORY] (square brackets or angle brackets).
     */
    if (*p == '[' || *p == '<') {
        char close_bracket = (*p == '[') ? ']' : '>';
        p++;  /* Skip opening bracket */

        const char *bracket_end = strchr(p, close_bracket);
        if (!bracket_end) {
            return RMS$_SYN;  /* Unclosed bracket */
        }

        size_t len = (size_t)(bracket_end - p);
        if (len > VMSFS_MAX_DIRECTORY) {
            return RMS$_SYN;
        }
        memcpy(result->directory, p, len);
        result->directory[len] = '\0';
        str_upcase(result->directory);
        result->has_directory = 1;
        if (contains_wildcard(result->directory)) {
            result->is_wildcard = 1;
        }
        p = bracket_end + 1;  /* Skip past closing bracket */
    }

    /*
     * Step 4: Parse NAME.TYPE;VERSION from the remainder.
     * First, find version ;N at the end.
     */
    const char *semi = strrchr(p, ';');
    const char *name_end = semi ? semi : end_of_spec;

    if (semi) {
        const char *ver_str = semi + 1;
        if (*ver_str == '*') {
            /* Wildcard version */
            result->version = -1;
            result->has_version = 1;
            result->is_wildcard = 1;
        } else if (*ver_str == '0' || (*ver_str >= '1' && *ver_str <= '9')) {
            result->version = atoi(ver_str);
            result->has_version = 1;
        } else if (*ver_str == '\0') {
            /* Bare semicolon with nothing after = version 0 (highest) */
            result->version = 0;
            result->has_version = 1;
        } else {
            return RMS$_SYN;  /* Invalid version */
        }
    }

    /*
     * Step 5: Split the name.type portion.
     * Find the last dot to separate name from type.
     */
    if (p < name_end) {
        const char *dot = NULL;
        for (const char *s = p; s < name_end; s++) {
            if (*s == '.') dot = s;
        }

        if (dot) {
            /* NAME is before the dot */
            size_t namelen = (size_t)(dot - p);
            if (namelen > VMSFS_MAX_NAME) {
                return RMS$_SYN;
            }
            if (namelen > 0) {
                memcpy(result->name, p, namelen);
                result->name[namelen] = '\0';
                str_upcase(result->name);
                result->has_name = 1;
                if (contains_wildcard(result->name)) {
                    result->is_wildcard = 1;
                }
            }

            /* TYPE is after the dot (not including the dot itself) */
            size_t typelen = (size_t)(name_end - dot - 1);
            if (typelen > VMSFS_MAX_TYPE) {
                return RMS$_SYN;
            }
            if (typelen > 0) {
                memcpy(result->type, dot + 1, typelen);
                result->type[typelen] = '\0';
                str_upcase(result->type);
            }
            result->has_type = 1;  /* Dot was present, even if type is empty */
            if (contains_wildcard(result->type)) {
                result->is_wildcard = 1;
            }
        } else {
            /* No dot - entire remainder is the name */
            size_t namelen = (size_t)(name_end - p);
            if (namelen > VMSFS_MAX_NAME) {
                return RMS$_SYN;
            }
            if (namelen > 0) {
                memcpy(result->name, p, namelen);
                result->name[namelen] = '\0';
                str_upcase(result->name);
                result->has_name = 1;
                if (contains_wildcard(result->name)) {
                    result->is_wildcard = 1;
                }
            }
        }
    }

    return SS$_NORMAL;
}

/*
 * vmsfs_compose_filespec - Build a VMS filespec string from parsed components.
 *
 * Reconstructs: NODE::DEVICE:[DIRECTORY]NAME.TYPE;VERSION
 * Only includes fields that have their has_* flag set.
 *
 * Returns SS$_NORMAL on success, SS$_RESULTOVF if buffer too small.
 */
int vmsfs_compose_filespec(const vmsfs_filespec_t *parts, char *result, size_t result_size)
{
    if (!parts || !result || result_size == 0) {
        return SS$_BADPARAM;
    }

    char *p = result;
    char *end = result + result_size - 1;
    int n;

    if (parts->has_node && parts->node[0]) {
        n = snprintf(p, (size_t)(end - p), "%s::", parts->node);
        if (n < 0 || p + n >= end) return SS$_RESULTOVF;
        p += n;
    }

    if (parts->has_device && parts->device[0]) {
        n = snprintf(p, (size_t)(end - p), "%s:", parts->device);
        if (n < 0 || p + n >= end) return SS$_RESULTOVF;
        p += n;
    }

    if (parts->has_directory) {
        n = snprintf(p, (size_t)(end - p), "[%s]", parts->directory);
        if (n < 0 || p + n >= end) return SS$_RESULTOVF;
        p += n;
    }

    if (parts->has_name && parts->name[0]) {
        n = snprintf(p, (size_t)(end - p), "%s", parts->name);
        if (n < 0 || p + n >= end) return SS$_RESULTOVF;
        p += n;
    }

    if (parts->has_type) {
        n = snprintf(p, (size_t)(end - p), ".%s", parts->type);
        if (n < 0 || p + n >= end) return SS$_RESULTOVF;
        p += n;
    }

    if (parts->has_version && parts->version >= 0) {
        n = snprintf(p, (size_t)(end - p), ";%d", parts->version);
        if (n < 0 || p + n >= end) return SS$_RESULTOVF;
        p += n;
    }

    *p = '\0';
    return SS$_NORMAL;
}

/* Maximum recursion depth for device/logical name resolution */
#define VMSFS_RESOLVE_MAX_DEPTH 10

/* Single-path translation (no search-list device fan-out) -- the classic
 * one-spec-in, one-path-out pipeline. vmsfs_to_linux_path() (below) wraps it
 * with the search-list fan-out; device-equivalence recursion inside the
 * resolver uses this directly so a single device equivalence never re-enters
 * the fan-out. */
static int vmsfs_to_linux_path_one(const char *vms_spec, char *linux_path,
                                   size_t path_size);

/*
 * Internal recursive device resolver with depth tracking.
 */
static int vmsfs_resolve_device_r(const char *device, char *linux_dir,
                                   size_t dir_size, int depth)
{
    if (!device || !linux_dir || dir_size == 0) {
        return SS$_BADPARAM;
    }

    if (depth >= VMSFS_RESOLVE_MAX_DEPTH) {
        return SS$_RESULTOVF;  /* Logical name translation loop */
    }

    /* Upcase the device name for lookup */
    char dev_upper[VMSFS_MAX_DEVICE + 1];
    strncpy(dev_upper, device, sizeof(dev_upper) - 1);
    dev_upper[sizeof(dev_upper) - 1] = '\0';
    str_upcase(dev_upper);

    /*
     * Priority 1: Check the device table.
     * Physical device names (DKA0:, DKB0:) resolve to mount points here.
     * This is the ONE place where Unix paths are produced.
     */
    if (vmsfs_device_resolve(dev_upper, linux_dir, dir_size) == SS$_NORMAL) {
        return SS$_NORMAL;
    }

    /*
     * Priority 2: Try to translate as a logical name using the LNM manager.
     * Logical device names (SYS$SYSDEVICE → DKA0:) resolve here,
     * then recurse to hit the device table.
     */
    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        char equiv[256];
        uint16_t equiv_len = 0;
        uint32_t attrs = 0;
        uint32_t status = lnm_translate(mgr, LNM_FILE_DEV, dev_upper,
                                         equiv, sizeof(equiv),
                                         &equiv_len, &attrs);
        if ($VMS_STATUS_SUCCESS(status)) {
            equiv[equiv_len] = '\0';

            if (equiv[0] == '/') {
                /* Legacy Linux path equivalence — use directly */
                size_t elen = strlen(equiv);
                if (elen > 1 && equiv[elen - 1] == '/') {
                    equiv[elen - 1] = '\0';
                }
                strncpy(linux_dir, equiv, dir_size - 1);
                linux_dir[dir_size - 1] = '\0';
                return SS$_NORMAL;
            }

            /*
             * VMS filespec equivalence (contains '[' directory spec).
             * e.g. SYS$SYSTEM → SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSEXE]
             * Translate the entire VMS spec to a Linux path.
             */
            if (strchr(equiv, '[')) {
                return vmsfs_to_linux_path_one(equiv, linux_dir, dir_size);
            }

            /* Equivalence is another logical or device name — recurse.
             * Uses the same single-value lnm_translate() above, so a
             * search-list device's OWN equivalences are already the
             * per-member specs the fan-out substituted; there is no second
             * fan-out here. */
            size_t elen = strlen(equiv);
            if (elen > 0 && equiv[elen - 1] == ':') {
                equiv[elen - 1] = '\0';  /* Strip trailing colon */
            }
            return vmsfs_resolve_device_r(equiv, linux_dir, dir_size, depth + 1);
        }
    }

    /*
     * Priority 3: Fallback — assume system disk.
     * This handles the case where no device table or LNM is set up yet
     * (early boot, standalone DCL).
     */
    strncpy(linux_dir, vms_default_root, dir_size - 1);
    linux_dir[dir_size - 1] = '\0';

    return SS$_NORMAL;
}

/*
 * vmsfs_resolve_device - Look up a VMS device name as a logical name.
 *
 * Uses the logical name manager to translate device names like SYS$DISK,
 * SYS$LOGIN, DKA0, etc. to Linux directory paths.
 *
 * Falls back to /vms/<device> if the logical name is not defined.
 *
 * Returns SS$_NORMAL on success, SS$_RESULTOVF on recursive loop.
 */
int vmsfs_resolve_device(const char *device, char *linux_dir, size_t dir_size)
{
    return vmsfs_resolve_device_r(device, linux_dir, dir_size, 0);
}

/*
 * equiv_is_rooted - Does an equivalence string name a rooted directory?
 *
 * A rooted-directory equivalence ends in "dev:[root.]" -- a '.' immediately
 * before the closing ']' (VSI OpenVMS User's Manual, "Rooted Directories":
 * the trailing dot marks the directory as a root onto which a subdirectory
 * is concatenated). "DKA100:[USERS.]" is rooted; "DKA100:[USERS]" is not.
 */
static int equiv_is_rooted(const char *equiv)
{
    const char *rb = strrchr(equiv, ']');
    if (!rb || rb == equiv)
        return 0;
    return rb[-1] == '.';
}

/*
 * vmsfs_device_concealed_rooted - see vmsfs/device.h.
 *
 * Reads the logical's LNM$M_CONCEALED translation attribute (returned by
 * lnm_translate as LNM_ATTR_CONCEALED) rather than inferring "concealed"
 * from the equivalence string's shape -- an ordinary logical that happens to
 * translate to "dev:[x.]" is NOT a concealed/rooted device, so the rooted
 * property is reported only when the CONCEALED attribute is actually set.
 */
uint32_t vmsfs_device_concealed_rooted(const char *device,
                                       int *is_concealed, int *is_rooted)
{
    if (is_concealed)
        *is_concealed = 0;
    if (is_rooted)
        *is_rooted = 0;

    if (!device || !device[0])
        return SS$_BADPARAM;

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr)
        return SS$_NOLOGNAM;

    /* Upcase the device name and strip a trailing colon for the lookup. */
    char dev_up[VMSFS_MAX_DEVICE + 1];
    strncpy(dev_up, device, sizeof(dev_up) - 1);
    dev_up[sizeof(dev_up) - 1] = '\0';
    size_t dl = strlen(dev_up);
    if (dl > 0 && dev_up[dl - 1] == ':')
        dev_up[dl - 1] = '\0';
    str_upcase(dev_up);

    char equiv[256];
    uint16_t equiv_len = 0;
    uint32_t attrs = 0;
    uint32_t status = lnm_translate(mgr, LNM_FILE_DEV, dev_up, equiv,
                                    sizeof(equiv), &equiv_len, &attrs);
    if (!$VMS_STATUS_SUCCESS(status))
        return SS$_NOLOGNAM;
    if (equiv_len >= sizeof(equiv))
        equiv_len = sizeof(equiv) - 1;
    equiv[equiv_len] = '\0';

    int concealed = (attrs & LNM_ATTR_CONCEALED) != 0;
    if (is_concealed)
        *is_concealed = concealed;
    /* A rooted directory is a concealed device whose equivalence is a rooted
     * directory spec. Honor the CONCEALED attribute (flag), not the string. */
    if (is_rooted)
        *is_rooted = (concealed && equiv_is_rooted(equiv)) ? 1 : 0;
    return SS$_NORMAL;
}

/*
 * expand_rooted_concealed - structurally expand a concealed rooted-directory
 * device logical, in place, before device/directory resolution.
 *
 * VMS composes a rooted-directory logical by CONCATENATING the logical's root
 * directory with the caller-supplied subdirectory per the rooted rule, not by
 * a trailing-dot string coincidence (VSI OpenVMS User's Manual, "Rooted
 * Directories"; DCL Dictionary, DEFINE /TRANSLATION_ATTRIBUTES=CONCEALED):
 *
 *   WORK -> DKA100:[USERS.]  (concealed, rooted)
 *     WORK:[SMITH]FOO.DAT   ->  DKA100:[USERS.SMITH]FOO.DAT
 *     WORK:[SMITH.PROJ]     ->  DKA100:[USERS.SMITH.PROJ]
 *     WORK:FOO.DAT          ->  DKA100:[USERS]FOO.DAT
 *     WORK:[000000]         ->  DKA100:[USERS]
 *
 * If parsed->device is a concealed rooted logical, this rewrites parsed so
 * device = the root's physical device and directory = the merged directory,
 * then the normal single-path pipeline translates the fully-expanded spec.
 * Only a device carrying the CONCEALED attribute AND a rooted equivalence is
 * expanded (honor the flag); a plain logical translating to "dev:[x.]" is
 * left untouched. Returns 1 if an expansion happened, 0 otherwise.
 */
static int expand_rooted_concealed(vmsfs_filespec_t *parsed)
{
    if (!parsed->has_device || !parsed->device[0])
        return 0;

    int concealed = 0, rooted = 0;
    if (!$VMS_STATUS_SUCCESS(vmsfs_device_concealed_rooted(parsed->device,
                                                           &concealed, &rooted)))
        return 0;
    if (!rooted)
        return 0;   /* not a rooted concealed device -- nothing to merge */

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr)
        return 0;

    char dev_up[VMSFS_MAX_DEVICE + 1];
    strncpy(dev_up, parsed->device, sizeof(dev_up) - 1);
    dev_up[sizeof(dev_up) - 1] = '\0';
    str_upcase(dev_up);

    char equiv[256];
    uint16_t equiv_len = 0;
    uint32_t attrs = 0;
    if (!$VMS_STATUS_SUCCESS(lnm_translate(mgr, LNM_FILE_DEV, dev_up, equiv,
                                           sizeof(equiv), &equiv_len, &attrs)))
        return 0;
    if (equiv_len >= sizeof(equiv))
        equiv_len = sizeof(equiv) - 1;
    equiv[equiv_len] = '\0';

    vmsfs_filespec_t root;
    if (!$VMS_STATUS_SUCCESS(vmsfs_parse_filespec(equiv, &root)) ||
        !root.has_directory)
        return 0;

    /* Root directory content ends in '.' (rooted): strip it to the base. */
    char root_base[VMSFS_MAX_PATH];
    strncpy(root_base, root.directory, sizeof(root_base) - 1);
    root_base[sizeof(root_base) - 1] = '\0';
    size_t rl = strlen(root_base);
    if (rl > 0 && root_base[rl - 1] == '.')
        root_base[rl - 1] = '\0';        /* "USERS." -> "USERS" */

    /* Merge with the caller's directory per the rooted rule. */
    char merged[VMSFS_MAX_PATH];
    const char *sub = parsed->has_directory ? parsed->directory : NULL;
    if (sub && sub[0] && strcmp(sub, "000000") != 0) {
        if (sub[0] == '.')
            sub++;                        /* [.SMITH] relative form */
        if (root_base[0])
            snprintf(merged, sizeof(merged), "%s.%s", root_base, sub);
        else
            snprintf(merged, sizeof(merged), "%s", sub);
    } else {
        /* WORK:FOO.DAT or WORK:[000000] -> the root itself. */
        snprintf(merged, sizeof(merged), "%s", root_base);
    }

    /* Rewrite parsed: device <- root device, directory <- merged. */
    parsed->device[0] = '\0';
    parsed->has_device = 0;
    if (root.has_device && root.device[0]) {
        strncpy(parsed->device, root.device, sizeof(parsed->device) - 1);
        parsed->device[sizeof(parsed->device) - 1] = '\0';
        parsed->has_device = 1;
    }
    strncpy(parsed->directory, merged, sizeof(parsed->directory) - 1);
    parsed->directory[sizeof(parsed->directory) - 1] = '\0';
    parsed->has_directory = (merged[0] != '\0');
    return 1;
}

/*
 * vmsfs_translate_directory - Convert VMS directory spec to Linux path.
 *
 * Translations:
 *   [DIR.SUBDIR]     -> dir/subdir
 *   [000000]         -> (root directory - empty string)
 *   [-]              -> ..
 *   [-.SUB]          -> ../sub
 *   [.SUB]           -> sub  (relative)
 *   [DIR.SUB1.SUB2]  -> dir/sub1/sub2
 *   [...]            -> (recursive wildcard - left as marker)
 *
 * Directory components are lowercased for the Linux filesystem.
 *
 * Returns SS$_NORMAL on success.
 */
int vmsfs_translate_directory(const char *vms_dir, char *linux_dir, size_t dir_size)
{
    if (!vms_dir || !linux_dir || dir_size == 0) {
        return SS$_BADPARAM;
    }

    linux_dir[0] = '\0';

    /* Handle [000000] - the MFD (Master File Directory) = root */
    if (strcmp(vms_dir, "000000") == 0) {
        linux_dir[0] = '\0';  /* Root is just empty (device root) */
        return SS$_NORMAL;
    }

    char *out = linux_dir;
    char *out_end = linux_dir + dir_size - 1;
    const char *p = vms_dir;

    /* Handle leading "-" for parent directory */
    if (*p == '-') {
        if (out + 2 >= out_end) return SS$_RESULTOVF;
        *out++ = '.';
        *out++ = '.';
        p++;
        if (*p == '.') {
            if (out + 1 >= out_end) return SS$_RESULTOVF;
            *out++ = '/';
            p++;
        }
        /* If just [-], we're done */
        if (*p == '\0') {
            *out = '\0';
            return SS$_NORMAL;
        }
    }

    /* Handle leading "." for relative directory */
    if (*p == '.') {
        p++;  /* Skip the leading dot - path is relative */
    }

    /* Convert remaining directory components: dots -> slashes, preserve case */
    while (*p && out < out_end) {
        if (*p == '.') {
            *out++ = '/';
        } else {
            *out++ = *p;
        }
        p++;
    }

    *out = '\0';

    if (*p != '\0') {
        return SS$_RESULTOVF;
    }

    return SS$_NORMAL;
}

/*
 * vmsfs_to_linux_path - Full VMS filespec to Linux path translation.
 *
 * Pipeline:
 *   1. Parse the VMS filespec into components
 *   2. Resolve device as a logical name
 *   3. Translate [DIR.SUBDIR] to /dir/subdir/
 *   4. Append name.type;version
 *   5. Attempt case-insensitive path resolution
 *
 * Returns SS$_NORMAL on success.
 */
static int vmsfs_to_linux_path_one(const char *vms_spec, char *linux_path,
                                   size_t path_size)
{
    if (!vms_spec || !linux_path || path_size == 0) {
        return SS$_BADPARAM;
    }

    vmsfs_filespec_t parsed;
    int status = vmsfs_parse_filespec(vms_spec, &parsed);
    if (!$VMS_STATUS_SUCCESS(status)) {
        return status;
    }

    /*
     * vms-d8e: structurally expand a concealed rooted-directory device logical
     * BEFORE resolving device/directory, so the logical's root and the
     * caller's subdirectory concatenate per the VMS rooted rule
     * ([USERS.] + [SMITH] -> [USERS.SMITH]) rather than colliding by trailing-
     * dot string luck. Only a device carrying the CONCEALED attribute AND a
     * rooted equivalence is touched; everything else falls straight through.
     */
    (void)expand_rooted_concealed(&parsed);

    char dir_prefix[VMSFS_MAX_PATH] = "";
    char dir_part[VMSFS_MAX_PATH] = "";

    /*
     * Step 1: Resolve device to a Linux directory prefix.
     */
    if (parsed.has_device && parsed.device[0]) {
        status = vmsfs_resolve_device(parsed.device, dir_prefix, sizeof(dir_prefix));
        if (!$VMS_STATUS_SUCCESS(status)) {
            return status;
        }
    }

    /*
     * Step 2: Translate VMS directory to Linux path component.
     */
    if (parsed.has_directory && parsed.directory[0]) {
        char translated_dir[VMSFS_MAX_PATH];
        status = vmsfs_translate_directory(parsed.directory, translated_dir,
                                           sizeof(translated_dir));
        if (!$VMS_STATUS_SUCCESS(status)) {
            return status;
        }

        if (dir_prefix[0]) {
            /* Combine device prefix with directory */
            if (translated_dir[0]) {
                snprintf(dir_part, sizeof(dir_part), "%s/%s", dir_prefix, translated_dir);
            } else {
                strncpy(dir_part, dir_prefix, sizeof(dir_part) - 1);
                dir_part[sizeof(dir_part) - 1] = '\0';
            }
        } else {
            /* No device - directory relative to VMS root */
            if (translated_dir[0] == '.') {
                /* Relative path (starts with .. or similar) */
                strncpy(dir_part, translated_dir, sizeof(dir_part) - 1);
                dir_part[sizeof(dir_part) - 1] = '\0';
            } else if (translated_dir[0]) {
                snprintf(dir_part, sizeof(dir_part), "%s/%s",
                         vms_default_root, translated_dir);
            } else {
                strncpy(dir_part, vms_default_root, sizeof(dir_part) - 1);
                dir_part[sizeof(dir_part) - 1] = '\0';
            }
        }
    } else if (dir_prefix[0]) {
        strncpy(dir_part, dir_prefix, sizeof(dir_part) - 1);
        dir_part[sizeof(dir_part) - 1] = '\0';
    }

    /*
     * Step 3: Build the filename portion (name.type;version).
     * VMS stores name and type separately; on Linux they combine with a dot.
     */
    char filename[VMSFS_MAX_FILESPEC] = "";
    if (parsed.has_name && parsed.name[0]) {
        char name_lower[VMSFS_MAX_NAME + 1];
        strncpy(name_lower, parsed.name, sizeof(name_lower) - 1);
        name_lower[sizeof(name_lower) - 1] = '\0';
        str_downcase(name_lower);

        if (parsed.has_type) {
            char type_lower[VMSFS_MAX_TYPE + 1];
            strncpy(type_lower, parsed.type, sizeof(type_lower) - 1);
            type_lower[sizeof(type_lower) - 1] = '\0';
            str_downcase(type_lower);

            if (parsed.has_version && parsed.version >= 0) {
                snprintf(filename, sizeof(filename), "%s.%s;%d",
                         name_lower, type_lower, parsed.version);
            } else {
                snprintf(filename, sizeof(filename), "%s.%s",
                         name_lower, type_lower);
            }
        } else {
            if (parsed.has_version && parsed.version >= 0) {
                snprintf(filename, sizeof(filename), "%s;%d",
                         name_lower, parsed.version);
            } else {
                strncpy(filename, name_lower, sizeof(filename) - 1);
            }
        }
    }

    /*
     * Step 4: Combine directory and filename.
     */
    if (dir_part[0] && filename[0]) {
        snprintf(linux_path, path_size, "%s/%s", dir_part, filename);
    } else if (dir_part[0]) {
        strncpy(linux_path, dir_part, path_size - 1);
        linux_path[path_size - 1] = '\0';
    } else if (filename[0]) {
        strncpy(linux_path, filename, path_size - 1);
        linux_path[path_size - 1] = '\0';
    } else {
        linux_path[0] = '\0';
    }

    /*
     * Step 5: Attempt case-insensitive resolution.
     * The VMS filespec was converted to lowercase, but the actual
     * file on Linux may have different casing. Try to find the real file.
     */
    if (linux_path[0] == '/' && !parsed.is_wildcard) {
        char resolved[VMSFS_MAX_PATH];
        if (vmsfs_resolve_path_case(linux_path, resolved, sizeof(resolved)) == 0) {
            strncpy(linux_path, resolved, path_size - 1);
            linux_path[path_size - 1] = '\0';
        }
        /* If case resolution fails, keep the lowercase path */
    }

    return SS$_NORMAL;
}

/*
 * vmsfs_to_linux_path - Full VMS filespec to Linux path, WITH search-list
 * device fan-out.
 *
 * When the device position of the filespec names a multi-valued (search-list)
 * logical -- e.g. DEFINE FOO DKA0:[X],DKA1:[Y] -- VMS RMS tries each
 * equivalence IN ORDER on $OPEN/$SEARCH until one names an existing file, and
 * $CREATE uses the first (VMS I/O User's Reference Manual, "Logical Name
 * Search Lists"; DCL Dictionary, DEFINE). This wrapper implements exactly
 * that: it substitutes each equivalence for the device, translates the
 * resulting per-member spec, and returns the FIRST member whose file exists;
 * if none exists it returns the first member's path, so $OPEN reports an
 * honest file-not-found and $CREATE lands in the primary member. A
 * single-valued device (n<=1), a physical device, or a name that is not a
 * logical at all all fall straight through to the classic single-path
 * pipeline unchanged.
 *
 * Wildcards are NOT fanned out here -- $SEARCH walks a directory itself, and
 * search-list-wide wildcard enumeration is a separate concern (vms-ed7's
 * $SEARCH fan-out item); the primary member is used, preserving today's
 * behavior. Returns a VMS status (SS$_NORMAL on success).
 */
int vmsfs_to_linux_path(const char *vms_spec, char *linux_path, size_t path_size)
{
    if (!vms_spec || !linux_path || path_size == 0) {
        return SS$_BADPARAM;
    }

    /*
     * Peek at the device: only a genuine multi-valued (>=2) search-list
     * logical triggers the fan-out. One lnm probe per translate; a miss (not
     * a logical) or a single value costs only that probe, then the classic
     * path runs exactly as before.
     */
    vmsfs_filespec_t parsed;
    int pst = vmsfs_parse_filespec(vms_spec, &parsed);
    if ($VMS_STATUS_SUCCESS(pst) && parsed.has_device && parsed.device[0] &&
        !parsed.is_wildcard && !parsed.has_node) {
        lnm_manager_t *mgr = lnm_get_manager();
        if (mgr) {
            char dev_up[VMSFS_MAX_DEVICE + 1];
            strncpy(dev_up, parsed.device, sizeof(dev_up) - 1);
            dev_up[sizeof(dev_up) - 1] = '\0';
            str_upcase(dev_up);

            char values[LNM_MAX_SEARCHLIST][LNM_MAX_VALUE + 1];
            uint8_t n = 0;
            uint32_t lst = lnm_translate_searchlist(mgr, dev_up, values,
                                                    LNM_MAX_SEARCHLIST, &n, NULL);
            if (lst == SS$_NORMAL && n >= 2) {
                /* rest = the original spec text after the device's colon (the
                 * directory/name/type/version the caller supplied). No node
                 * prefix is present here (guarded above), so the first colon
                 * is the device terminator. */
                const char *colon = strchr(vms_spec, ':');
                const char *rest = colon ? colon + 1 : "";

                char primary[VMSFS_MAX_PATH] = "";
                int have_primary = 0;

                for (uint8_t i = 0; i < n; i++) {
                    char cand_spec[VMSFS_MAX_PATH];
                    /* Substitute the equivalence for the device. An
                     * equivalence that already carries a device/directory
                     * (':' or '[') is spliced directly; a bare device name
                     * gets its colon restored so "DEVA" + "BAR.TXT" becomes
                     * "DEVA:BAR.TXT", not "DEVABAR.TXT". */
                    if (strchr(values[i], ':') || strchr(values[i], '[')) {
                        snprintf(cand_spec, sizeof(cand_spec), "%s%s",
                                 values[i], rest);
                    } else {
                        snprintf(cand_spec, sizeof(cand_spec), "%s:%s",
                                 values[i], rest);
                    }

                    char cand_path[VMSFS_MAX_PATH];
                    int cst = vmsfs_to_linux_path_one(cand_spec, cand_path,
                                                      sizeof(cand_path));
                    if (!$VMS_STATUS_SUCCESS(cst)) {
                        continue;   /* this member does not translate; try next */
                    }

                    if (!have_primary) {
                        strncpy(primary, cand_path, sizeof(primary) - 1);
                        primary[sizeof(primary) - 1] = '\0';
                        have_primary = 1;
                    }

                    struct stat sb;
                    if (stat(cand_path, &sb) == 0) {
                        /* First member whose file exists wins -- this IS the
                         * "try each in order" behavior. */
                        strncpy(linux_path, cand_path, path_size - 1);
                        linux_path[path_size - 1] = '\0';
                        return SS$_NORMAL;
                    }
                }

                if (have_primary) {
                    /* No member has the file: hand back the primary member's
                     * path (honest FNF on open, create-in-first on create). */
                    strncpy(linux_path, primary, path_size - 1);
                    linux_path[path_size - 1] = '\0';
                    return SS$_NORMAL;
                }
                /* Not a single member translated -- fall through to the
                 * classic path on the original spec for a consistent error. */
            }
        }
    }

    return vmsfs_to_linux_path_one(vms_spec, linux_path, path_size);
}

/*
 * vmsfs_to_vms_spec - Translate a Linux path to a VMS filespec.
 *
 * Conversion rules:
 *   - Paths under /vms/<device>/... map to DEVICE:[DIR.SUBDIR]
 *   - Other absolute paths map to [path components]
 *   - Filename is uppercased
 *   - Version suffix ;N is preserved
 *   - Slashes become dots in directory spec
 *
 * Returns SS$_NORMAL on success.
 */
int vmsfs_to_vms_spec(const char *linux_path, char *vms_spec, size_t spec_size)
{
    if (!linux_path || !vms_spec || spec_size == 0) {
        return SS$_BADPARAM;
    }

    char *out = vms_spec;
    char *out_end = vms_spec + spec_size - 1;

    /* Separate the path into directory and filename */
    const char *last_slash = strrchr(linux_path, '/');
    const char *filename = NULL;
    char dirpart[VMSFS_MAX_PATH] = "";

    if (last_slash) {
        size_t dlen = (size_t)(last_slash - linux_path);
        if (dlen >= sizeof(dirpart)) dlen = sizeof(dirpart) - 1;
        memcpy(dirpart, linux_path, dlen);
        dirpart[dlen] = '\0';
        filename = last_slash + 1;
    } else {
        filename = linux_path;
    }

    /*
     * Convert directory part.
     * Check if path starts with /vms/ - extract device name.
     */
    if (dirpart[0]) {
        const char *dp = dirpart;
        int have_device = 0;

        /* Check for /vms/<device>/... pattern */
        if (strncmp(dp, "/vms/", 5) == 0) {
            dp += 5;
            /* Extract device name (up to next /) */
            const char *dev_end = strchr(dp, '/');
            if (dev_end) {
                size_t devlen = (size_t)(dev_end - dp);
                if (devlen > 0 && devlen <= VMSFS_MAX_DEVICE) {
                    char dev[VMSFS_MAX_DEVICE + 1];
                    memcpy(dev, dp, devlen);
                    dev[devlen] = '\0';
                    str_upcase(dev);
                    int n = snprintf(out, (size_t)(out_end - out), "%s:", dev);
                    if (n > 0) out += n;
                    have_device = 1;
                }
                dp = dev_end + 1;
            } else {
                /* Just /vms/<device> with no subdirectory */
                char dev[VMSFS_MAX_DEVICE + 1];
                strncpy(dev, dp, sizeof(dev) - 1);
                dev[sizeof(dev) - 1] = '\0';
                str_upcase(dev);
                int n = snprintf(out, (size_t)(out_end - out), "%s:", dev);
                if (n > 0) out += n;
                dp = dp + strlen(dp);
                have_device = 1;
            }
        } else if (dp[0] == '/') {
            dp++;  /* Skip leading slash for absolute paths */
        }

        /* Convert remaining directory path: slashes -> dots */
        if (*dp || have_device) {
            if (out < out_end) *out++ = '[';

            /* If no subdirectory after device, use 000000 (root) */
            if (!*dp && have_device) {
                int n = snprintf(out, (size_t)(out_end - out), "000000");
                if (n > 0) out += n;
            }

            while (*dp && out < out_end) {
                if (*dp == '/') {
                    *out++ = '.';
                } else {
                    *out++ = (char)toupper((unsigned char)*dp);
                }
                dp++;
            }

            if (out < out_end) *out++ = ']';
        }
    }

    /*
     * Convert filename: separate name, type, and version.
     * Uppercase the name and type portions.
     */
    if (filename && *filename) {
        /* Check for version suffix ;N */
        const char *semi = strrchr(filename, ';');
        const char *fname_end = semi ? semi : filename + strlen(filename);

        /* Find the last dot for name.type split */
        const char *dot = NULL;
        for (const char *s = filename; s < fname_end; s++) {
            if (*s == '.') dot = s;
        }

        /* Write name part */
        const char *name_end = dot ? dot : fname_end;
        while (filename < name_end && out < out_end) {
            *out++ = (char)toupper((unsigned char)*filename);
            filename++;
        }

        /* Write .TYPE part */
        if (dot) {
            if (out < out_end) *out++ = '.';
            filename = dot + 1;  /* Skip the dot */
            while (filename < fname_end && out < out_end) {
                *out++ = (char)toupper((unsigned char)*filename);
                filename++;
            }
        }

        /* Write ;VERSION part */
        if (semi) {
            if (out < out_end) *out++ = ';';
            filename = semi + 1;
            while (*filename && out < out_end) {
                *out++ = *filename++;
            }
        }
    }

    *out = '\0';
    return SS$_NORMAL;
}

/*
 * wildcard_match_core - Match a name against a VMS wildcard pattern,
 * with no notion of file versions (pure character-level matching).
 *
 * VMS wildcard characters:
 *   *  - Match zero or more characters
 *   %  - Match exactly one character
 *   ... - Match any directory levels (in directory specs)
 *
 * The match is case-insensitive, following VMS convention.
 *
 * Returns 1 if the name matches the pattern, 0 if not.
 */
static int wildcard_match_core(const char *pattern, const char *name)
{
    const char *p = pattern;
    const char *n = name;

    while (*p && *n) {
        if (*p == '*') {
            /* Star: match zero or more characters */
            p++;
            if (!*p) return 1;  /* Trailing * matches everything */

            /* Try matching the rest of the pattern at each position */
            while (*n) {
                if (wildcard_match_core(p, n)) return 1;
                n++;
            }
            return wildcard_match_core(p, n);
        } else if (*p == '%' || *p == '?') {
            /* Percent or question mark: match exactly one character */
            p++;
            n++;
        } else {
            /* Literal character - compare case-insensitively */
            if (toupper((unsigned char)*p) != toupper((unsigned char)*n)) {
                return 0;
            }
            p++;
            n++;
        }
    }

    /* Handle trailing wildcards */
    while (*p == '*') p++;

    return (*p == '\0' && *n == '\0') ? 1 : 0;
}

/*
 * Split a trailing ";N" / ";" / ";*" version marker off a filespec
 * component. Only a marker whose digits are all numeric (or empty, or a
 * bare wildcard character) is recognized as a version — anything else
 * (e.g. a literal ';' that isn't a VMS version) is left in the base
 * string, matching vmsfs_parse_filespec()'s own rules.
 *
 * On return, *base_len is the length of the name/pattern before the
 * version marker (or the full length if there was none), *has_version
 * indicates whether a recognized marker was present, and *version is:
 *   - the numeric value, when the marker was one or more digits
 *   - 0, when the marker was bare (";") or wildcard (";*"/";%") — VMS
 *     convention for "unspecified" / "highest"
 */
static void split_version_marker(const char *s, size_t *base_len,
                                  int *has_version, long *version)
{
    size_t len = strlen(s);
    *base_len = len;
    *has_version = 0;
    *version = 0;

    const char *semi = strrchr(s, ';');
    if (!semi) return;

    const char *v = semi + 1;
    if (*v == '\0' || *v == '*' || *v == '%') {
        *base_len = (size_t)(semi - s);
        *has_version = 1;
        *version = 0;
        return;
    }

    int alldigits = 1;
    for (const char *q = v; *q; q++) {
        if (!isdigit((unsigned char)*q)) { alldigits = 0; break; }
    }
    if (!alldigits) return;  /* Not a recognized version marker */

    *base_len = (size_t)(semi - s);
    *has_version = 1;
    *version = strtol(v, NULL, 10);
}

/*
 * vmsfs_wildcard_match - Match a filename against a VMS wildcard pattern.
 *
 * Version-aware: VMS files carry a ";N" version on disk (see
 * vmsfs_version.c). A pattern with no version, a bare ";", or ";*"/";0"
 * matches ANY version of a name (VMS: "DIRECTORY FOO.DAT" lists every
 * version; ";0" means "the highest version", not literal version 0). A
 * pattern with an explicit ";N" (N>0) matches only that exact version. A
 * name with no version marker at all (e.g. a plain directory) matches on
 * its own without any version filtering.
 *
 * Returns 1 if the name matches the pattern, 0 if not.
 */
int vmsfs_wildcard_match(const char *pattern, const char *name)
{
    if (!pattern || !name) return 0;

    size_t pat_base_len, name_base_len;
    int pat_has_version, name_has_version;
    long pat_version, name_version;

    split_version_marker(pattern, &pat_base_len, &pat_has_version, &pat_version);
    split_version_marker(name, &name_base_len, &name_has_version, &name_version);

    /* A specific requested version (N>0) must match the name's version
     * exactly, when the name has one. A pattern version of 0 (bare ";"
     * or ";0") means "highest" — for wildcard/listing purposes that
     * matches any version, so it imposes no filter here. */
    if (pat_has_version && pat_version > 0 &&
        name_has_version && name_version != pat_version) {
        return 0;
    }

    char pat_base[VMSFS_MAX_FILESPEC];
    char name_base[VMSFS_MAX_FILESPEC];

    if (pat_base_len >= sizeof(pat_base)) pat_base_len = sizeof(pat_base) - 1;
    memcpy(pat_base, pattern, pat_base_len);
    pat_base[pat_base_len] = '\0';

    if (name_base_len >= sizeof(name_base)) name_base_len = sizeof(name_base) - 1;
    memcpy(name_base, name, name_base_len);
    name_base[name_base_len] = '\0';

    return wildcard_match_core(pat_base, name_base);
}
