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
#include <string.h>
#include "rms/rms.h"
#include "vmsfs/filespec.h"
#include "vmsfs/device.h"
#include "ssdef.h"

/*
 * Apply defaults from default_spec to spec using vmsfs parse/compose.
 * Missing fields in spec are filled from default_spec.
 * Returns 0 on success, -1 on failure.
 */
static int apply_filespec_defaults(const char *spec, const char *default_spec,
                                   char *result, size_t resultlen)
{
    vmsfs_filespec_t parsed, defaults;

    if (vmsfs_parse_filespec(spec, &parsed) != 0)
        return -1;

    if (default_spec) {
        if (vmsfs_parse_filespec(default_spec, &defaults) == 0) {
            if (!parsed.has_device && defaults.has_device) {
                memcpy(parsed.device, defaults.device, sizeof(parsed.device));
                parsed.has_device = 1;
            }
            if (!parsed.has_directory && defaults.has_directory) {
                memcpy(parsed.directory, defaults.directory, sizeof(parsed.directory));
                parsed.has_directory = 1;
            }
            if (!parsed.has_name && defaults.has_name) {
                memcpy(parsed.name, defaults.name, sizeof(parsed.name));
                parsed.has_name = 1;
            }
            if (!parsed.has_type && defaults.has_type) {
                memcpy(parsed.type, defaults.type, sizeof(parsed.type));
                parsed.has_type = 1;
            }
            if (!parsed.has_version && defaults.has_version) {
                parsed.version = defaults.version;
                parsed.has_version = 1;
            }
        }
    }

    if (vmsfs_compose_filespec(&parsed, result, resultlen) != 0)
        return -1;

    return 0;
}

/*
 * sys$parse - Parse a filespec.
 *
 * Takes the filespec from fab$l_fna, applies defaults from
 * fab$l_dna, and fills in the NAM block with the expanded
 * string and component information.
 *
 * The expanded string is stored in the buffer pointed to by
 * nam$l_esa, with the length in nam$b_esl. Component pointers
 * (nam$l_node, nam$l_dev, etc.) point into the expanded string,
 * and component lengths are set accordingly.
 *
 * Wildcard flags are set in nam$l_fnb.
 *
 * Returns:
 *   RMS$_NORMAL - Parse successful
 *   RMS$_FAB    - Invalid FAB
 *   RMS$_NAM    - No NAM block
 *   RMS$_SYN    - Syntax error in filespec
 */
uint32_t sys$parse(void *fab_ptr)
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

    /* Extract the filespec from the FAB */
    char spec[1024] = "";
    if (fab->fab$l_fna && fab->fab$b_fns > 0) {
        size_t len = fab->fab$b_fns;
        if (len >= sizeof(spec)) len = sizeof(spec) - 1;
        memcpy(spec, fab->fab$l_fna, len);
        spec[len] = '\0';
    } else {
        fab->fab$l_sts = RMS$_SYN;
        nam->nam$l_sts = RMS$_SYN;
        return RMS$_SYN;
    }

    /* Extract the default filespec */
    char default_spec[1024] = "";
    if (fab->fab$l_dna && fab->fab$b_dns > 0) {
        size_t len = fab->fab$b_dns;
        if (len >= sizeof(default_spec)) len = sizeof(default_spec) - 1;
        memcpy(default_spec, fab->fab$l_dna, len);
        default_spec[len] = '\0';
    }

    /* Apply defaults and expand the filespec */
    char combined[1024];
    if (apply_filespec_defaults(spec, default_spec[0] ? default_spec : NULL,
                               combined, sizeof(combined)) < 0) {
        /* If resolve fails, use the raw spec */
        strncpy(combined, spec, sizeof(combined) - 1);
        combined[sizeof(combined) - 1] = '\0';
    }

    /* Store expanded string in NAM buffer */
    if (nam->nam$l_esa && nam->nam$b_ess > 0) {
        size_t combined_len = strlen(combined);
        if (combined_len > nam->nam$b_ess) {
            combined_len = nam->nam$b_ess;
        }
        memcpy(nam->nam$l_esa, combined, combined_len);
        nam->nam$b_esl = (uint8_t)combined_len;
    }

    /*
     * Parse the expanded string into components.
     * We manually walk the expanded string to set the NAM
     * component pointers and lengths. These pointers reference
     * into the expanded string buffer (nam$l_esa).
     */
    char *esa = nam->nam$l_esa;
    uint8_t esl = nam->nam$b_esl;
    char *p = esa;
    char *end = esa + esl;

    /* Reset all component pointers and sizes */
    nam->nam$l_node = NULL; nam->nam$b_node = 0;
    nam->nam$l_dev  = NULL; nam->nam$b_dev  = 0;
    nam->nam$l_dir  = NULL; nam->nam$b_dir  = 0;
    nam->nam$l_name = NULL; nam->nam$b_name = 0;
    nam->nam$l_type = NULL; nam->nam$b_type = 0;
    nam->nam$l_ver  = NULL; nam->nam$b_ver  = 0;
    nam->nam$l_fnb = 0;

    if (!esa || esl == 0) {
        fab->fab$l_sts = RMS$_NORMAL;
        nam->nam$l_sts = RMS$_NORMAL;
        return RMS$_NORMAL;
    }

    /* Look for node:: */
    char *dcolon = NULL;
    for (char *s = p; s < end - 1; s++) {
        if (s[0] == ':' && s[1] == ':') {
            dcolon = s;
            break;
        }
    }
    if (dcolon) {
        nam->nam$l_node = p;
        size_t node_len = (size_t)(dcolon - p + 2);  /* Include :: */
        if (node_len > 255) {
            fab->fab$l_sts = RMS$_FNM;
            return RMS$_FNM;
        }
        nam->nam$b_node = (uint8_t)node_len;
        nam->nam$l_fnb |= NAM$M_NODE;
        p = dcolon + 2;
    }

    /* Look for device: (but not ::) */
    char *colon = NULL;
    for (char *s = p; s < end; s++) {
        if (*s == ':') {
            /* Make sure this isn't the second colon of :: */
            if (s + 1 < end && s[1] == ':') continue;
            colon = s;
            break;
        }
    }
    if (colon) {
        nam->nam$l_dev = p;
        size_t dev_len = (size_t)(colon - p + 1);  /* Include : */
        if (dev_len > 255) {
            fab->fab$l_sts = RMS$_FNM;
            return RMS$_FNM;
        }
        nam->nam$b_dev = (uint8_t)dev_len;
        nam->nam$l_fnb |= NAM$M_EXP_DEV;

        /*
         * vms-d8e: a concealed / rooted-directory device logical (defined
         * /TRANSLATION_ATTRIBUTES=CONCEALED, e.g. DISK$USER -> DKA100:[USERS.])
         * sets NAM$M_CNCL_DEV, and NAM$M_ROOT_DIR when the concealed
         * equivalence is a rooted directory. $PARSE reads the LNM$M_CONCEALED
         * translation attribute from the logical (via vmsfs_device_concealed_
         * rooted -> lnm_translate) -- it does not infer concealment from the
         * device string (VSI OpenVMS RMS Reference, NAM block nam$l_fnb;
         * VSI OpenVMS DCL Dictionary, DEFINE /TRANSLATION_ATTRIBUTES).
         */
        char devname[64];
        size_t dn = (size_t)(colon - nam->nam$l_dev);   /* excludes the ':' */
        if (dn >= sizeof(devname))
            dn = sizeof(devname) - 1;
        memcpy(devname, nam->nam$l_dev, dn);
        devname[dn] = '\0';

        int cncl = 0, root = 0;
        if ($VMS_STATUS_SUCCESS(
                vmsfs_device_concealed_rooted(devname, &cncl, &root))) {
            if (cncl)
                nam->nam$l_fnb |= NAM$M_CNCL_DEV;
            if (root)
                nam->nam$l_fnb |= NAM$M_ROOT_DIR;
        }

        p = colon + 1;
    }

    /* Look for [directory] */
    if (p < end && *p == '[') {
        char *bracket = NULL;
        for (char *s = p; s < end; s++) {
            if (*s == ']') { bracket = s; break; }
        }
        if (bracket) {
            nam->nam$l_dir = p;
            size_t dir_len = (size_t)(bracket - p + 1);  /* Include [] */
            if (dir_len > 255) {
                fab->fab$l_sts = RMS$_FNM;
                return RMS$_FNM;
            }
            nam->nam$b_dir = (uint8_t)dir_len;
            nam->nam$l_fnb |= NAM$M_EXP_DIR;

            /* Check for wildcards in directory */
            for (char *s = p; s <= bracket; s++) {
                if (*s == '*' || *s == '%') {
                    nam->nam$l_fnb |= NAM$M_WILDCARD | NAM$M_WILD_DIR;
                    break;
                }
            }
            p = bracket + 1;
        }
    }

    /* Find the version separator ; */
    char *semi = NULL;
    for (char *s = end - 1; s >= p; s--) {
        if (*s == ';') { semi = s; break; }
    }

    /* Find the type separator . (last dot before semicolon or end) */
    char *name_end = semi ? semi : end;
    char *dot = NULL;
    for (char *s = name_end - 1; s >= p; s--) {
        if (*s == '.') { dot = s; break; }
    }

    /* Name component */
    char *name_start = p;
    char *name_finish = dot ? dot : name_end;
    if (name_finish > name_start) {
        nam->nam$l_name = name_start;
        size_t name_len = (size_t)(name_finish - name_start);
        if (name_len > 255) {
            fab->fab$l_sts = RMS$_FNM;
            return RMS$_FNM;
        }
        nam->nam$b_name = (uint8_t)name_len;
        nam->nam$l_fnb |= NAM$M_EXP_NAME;

        /* Check for wildcards in name */
        for (char *s = name_start; s < name_finish; s++) {
            if (*s == '*' || *s == '%') {
                nam->nam$l_fnb |= NAM$M_WILDCARD | NAM$M_WILD_NAME;
                break;
            }
        }
    }

    /* Type component (includes the dot) */
    if (dot) {
        nam->nam$l_type = dot;
        size_t type_len = (size_t)(name_end - dot);
        if (type_len > 255) {
            fab->fab$l_sts = RMS$_FNM;
            return RMS$_FNM;
        }
        nam->nam$b_type = (uint8_t)type_len;
        nam->nam$l_fnb |= NAM$M_EXP_TYPE;

        /* Check for wildcards in type */
        for (char *s = dot; s < name_end; s++) {
            if (*s == '*' || *s == '%') {
                nam->nam$l_fnb |= NAM$M_WILDCARD | NAM$M_WILD_TYPE;
                break;
            }
        }
    }

    /* Version component (includes the semicolon) */
    if (semi) {
        nam->nam$l_ver = semi;
        size_t ver_len = (size_t)(end - semi);
        if (ver_len > 255) {
            fab->fab$l_sts = RMS$_FNM;
            return RMS$_FNM;
        }
        nam->nam$b_ver = (uint8_t)ver_len;
        nam->nam$l_fnb |= NAM$M_EXP_VER;

        /* Check for wildcard version ;* */
        if (semi + 1 < end && *(semi + 1) == '*') {
            nam->nam$l_fnb |= NAM$M_WILDCARD | NAM$M_WILD_VER;
        }
    }

    fab->fab$l_sts = RMS$_NORMAL;
    nam->nam$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
}
