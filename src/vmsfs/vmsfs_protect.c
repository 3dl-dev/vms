/*
 * vmsfs_protect.c - VMS File Protection (SOGW) Mapping
 *
 * VMS uses a 16-bit protection word with four 4-bit groups:
 *   System (S), Owner (O), Group (G), World (W)
 *
 * Each group has four access bits:
 *   R = Read, W = Write, E = Execute, D = Delete
 *
 * IMPORTANT: In VMS, a SET bit means access is DENIED.
 * This is the opposite of Unix, where a set bit means access is ALLOWED.
 *
 * Protection word layout (16 bits):
 *   Bits 15-12: World  (W)
 *   Bits 11-8:  Group  (G)
 *   Bits  7-4:  Owner  (O)
 *   Bits  3-0:  System (S)
 *
 * Mapping to Unix:
 *   VMS Owner  -> Unix User  (owner)
 *   VMS Group  -> Unix Group
 *   VMS World  -> Unix Other
 *   VMS System -> (no direct Unix equivalent, maps to root/setuid)
 *
 * VMS RWED -> Unix rwx:
 *   R -> r (read)
 *   W -> w (write)
 *   E -> x (execute)
 *   D -> w (delete maps to write permission on parent directory)
 *
 * The VMS_PROT_* bit values and shifts below are PINNED, not chosen here
 * -- see src/libvms/include/ovmx_fileprot.h for the citation (VSI OpenVMS
 * Wiki $CRMPSC + the FA00 default-protection worked example) and for why
 * this is the single shared definition src/libvms/syssvc/sys_security.c
 * also includes (vms-f81: the two used to disagree).
 *
 * OVMX Project - VMS Filesystem Semantics Layer
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "vmsfs/filespec.h"
#include "ssdef.h"
#include "ovmx_fileprot.h"

/*
 * vmsfs_protection_to_mode - Convert VMS SOGW protection to Unix mode_t.
 *
 * VMS protection semantics: set bit = DENIED
 * Unix permission semantics: set bit = ALLOWED
 *
 * Mapping:
 *   VMS Owner R/W/E/D -> Unix user r/w/x (D maps to w)
 *   VMS Group R/W/E/D -> Unix group r/w/x
 *   VMS World R/W/E/D -> Unix other r/w/x
 *   VMS System -> not directly mapped (system always has access via root)
 *
 * Parameters:
 *   vms_prot - 16-bit VMS protection word
 *
 * Returns the corresponding Unix mode_t permission bits.
 */
mode_t vmsfs_protection_to_mode(uint16_t vms_prot)
{
    mode_t mode = 0;

    /*
     * Owner (VMS Owner -> Unix user permissions)
     * Invert the bits: VMS set = denied, Unix set = allowed
     */
    uint8_t owner = (uint8_t)((vms_prot >> VMS_PROT_OWNER_SHIFT) & 0x0F);
    if (!(owner & VMS_PROT_R)) mode |= S_IRUSR;
    if (!(owner & VMS_PROT_W)) mode |= S_IWUSR;
    if (!(owner & VMS_PROT_E)) mode |= S_IXUSR;
    /* D (delete) also implies write for Unix purposes */
    if (!(owner & VMS_PROT_D) && !(mode & S_IWUSR)) mode |= S_IWUSR;

    /*
     * Group (VMS Group -> Unix group permissions)
     */
    uint8_t group = (uint8_t)((vms_prot >> VMS_PROT_GROUP_SHIFT) & 0x0F);
    if (!(group & VMS_PROT_R)) mode |= S_IRGRP;
    if (!(group & VMS_PROT_W)) mode |= S_IWGRP;
    if (!(group & VMS_PROT_E)) mode |= S_IXGRP;
    if (!(group & VMS_PROT_D) && !(mode & S_IWGRP)) mode |= S_IWGRP;

    /*
     * World (VMS World -> Unix other permissions)
     */
    uint8_t world = (uint8_t)((vms_prot >> VMS_PROT_WORLD_SHIFT) & 0x0F);
    if (!(world & VMS_PROT_R)) mode |= S_IROTH;
    if (!(world & VMS_PROT_W)) mode |= S_IWOTH;
    if (!(world & VMS_PROT_E)) mode |= S_IXOTH;
    if (!(world & VMS_PROT_D) && !(mode & S_IWOTH)) mode |= S_IWOTH;

    return mode;
}

/*
 * vmsfs_mode_to_protection - Convert Unix mode_t to VMS protection word.
 *
 * Reverse of vmsfs_protection_to_mode.
 *
 * VMS System category defaults to no access denied (full access),
 * matching the typical VMS convention that SYSTEM has full access.
 *
 * Parameters:
 *   mode - Unix mode_t permission bits
 *
 * Returns the 16-bit VMS protection word.
 */
uint16_t vmsfs_mode_to_protection(mode_t mode)
{
    uint16_t prot = 0;

    /*
     * System: default to full access (no bits denied = 0x00)
     * This matches VMS convention where SYSTEM always has RWED.
     */
    /* prot |= (0x00 << VMS_PROT_SYSTEM_SHIFT); -- already zero */

    /*
     * Owner: start with all denied, then clear bits for allowed access.
     */
    uint8_t owner = VMS_PROT_R | VMS_PROT_W | VMS_PROT_E | VMS_PROT_D;
    if (mode & S_IRUSR) owner &= ~VMS_PROT_R;
    if (mode & S_IWUSR) owner &= (uint8_t)~(VMS_PROT_W | VMS_PROT_D);
    if (mode & S_IXUSR) owner &= ~VMS_PROT_E;
    prot |= (uint16_t)((uint16_t)owner << VMS_PROT_OWNER_SHIFT);

    /*
     * Group
     */
    uint8_t group = VMS_PROT_R | VMS_PROT_W | VMS_PROT_E | VMS_PROT_D;
    if (mode & S_IRGRP) group &= ~VMS_PROT_R;
    if (mode & S_IWGRP) group &= (uint8_t)~(VMS_PROT_W | VMS_PROT_D);
    if (mode & S_IXGRP) group &= ~VMS_PROT_E;
    prot |= (uint16_t)((uint16_t)group << VMS_PROT_GROUP_SHIFT);

    /*
     * World
     */
    uint8_t world = VMS_PROT_R | VMS_PROT_W | VMS_PROT_E | VMS_PROT_D;
    if (mode & S_IROTH) world &= ~VMS_PROT_R;
    if (mode & S_IWOTH) world &= (uint8_t)~(VMS_PROT_W | VMS_PROT_D);
    if (mode & S_IXOTH) world &= ~VMS_PROT_E;
    prot |= (uint16_t)((uint16_t)world << VMS_PROT_WORLD_SHIFT);

    return prot;
}

/*
 * vmsfs_format_protection - Format a VMS protection word as a display string.
 *
 * Output format: "(S:RWED,O:RWED,G:RE,W:)"
 *
 * Each category lists the access types that are ALLOWED (bits NOT set
 * in the protection word, since set = denied in VMS).
 *
 * Parameters:
 *   prot    - 16-bit VMS protection word
 *   buf     - Output buffer for the formatted string
 *   bufsize - Size of the output buffer
 *
 * Returns SS$_NORMAL on success, SS$_RESULTOVF if buffer too small.
 */
int vmsfs_format_protection(uint16_t prot, char *buf, size_t bufsize)
{
    if (!buf || bufsize == 0) {
        return SS$_BADPARAM;
    }

    static const char *categories[] = { "S", "O", "G", "W" };
    static const int shifts[] = {
        VMS_PROT_SYSTEM_SHIFT,
        VMS_PROT_OWNER_SHIFT,
        VMS_PROT_GROUP_SHIFT,
        VMS_PROT_WORLD_SHIFT
    };

    char *p = buf;
    char *end = buf + bufsize - 1;

    if (p >= end) return SS$_RESULTOVF;
    *p++ = '(';

    for (int i = 0; i < 4 && p < end; i++) {
        if (i > 0 && p < end) {
            *p++ = ',';
        }

        /* Write category label and colon */
        int n = snprintf(p, (size_t)(end - p), "%s:", categories[i]);
        if (n < 0 || p + n >= end) return SS$_RESULTOVF;
        p += n;

        /* Extract the 4-bit protection for this category */
        uint8_t bits = (uint8_t)((prot >> shifts[i]) & 0x0F);

        /*
         * List the ALLOWED access types.
         * In VMS, a clear bit means access is allowed.
         */
        if (!(bits & VMS_PROT_R) && p < end) *p++ = 'R';
        if (!(bits & VMS_PROT_W) && p < end) *p++ = 'W';
        if (!(bits & VMS_PROT_E) && p < end) *p++ = 'E';
        if (!(bits & VMS_PROT_D) && p < end) *p++ = 'D';
    }

    if (p < end) *p++ = ')';
    *p = '\0';

    return SS$_NORMAL;
}

/*
 * vmsfs_parse_protection - Parse a VMS protection string, MERGING onto a base.
 *
 * Accepts formats like:
 *   "(S:RWED,O:RWED,G:RE,W:)"
 *   "S:RWED,O:RWED,G:RE,W:"     (without parentheses)
 *   "(S:RWED,O:RWD,G:R,W)"      (various combinations)
 *   "W:RE"                       (a single category, e.g. `SET PROTECTION=W:RE`)
 *
 * Within a category that IS named, each letter after the colon indicates
 * ALLOWED access, and any access type NOT listed is DENIED (its bit is set).
 *
 * A category that is NOT named is left at whatever value the caller placed
 * in *prot on entry -- this is the VMS SET PROTECTION rule: "Unspecified
 * categories retain their current protection." (VSI OpenVMS DCL Dictionary,
 * SET PROTECTION: the protection-code argument is [S|O|G|W]:code and "any
 * categories you do not specify are unchanged".) The caller therefore seeds
 * *prot with the object's CURRENT protection word before calling; to parse a
 * fresh, standalone spec instead (every unnamed category denied), seed *prot
 * with 0xFFFF first.
 *
 * Parameters:
 *   str  - Protection string to parse
 *   prot - In/out: 16-bit VMS protection word. On entry, the base value for
 *          categories the string omits; on return, the merged result.
 *
 * Returns SS$_NORMAL on success, SS$_BADPARAM on error.
 */
int vmsfs_parse_protection(const char *str, uint16_t *prot)
{
    if (!str || !prot) {
        return SS$_BADPARAM;
    }

    /* NOTE: *prot is NOT reset here -- it carries the base value so that
     * categories the string omits keep their current protection (VMS SET
     * PROTECTION merge semantics; see the header comment above). Callers that
     * want a fresh all-denied parse must set *prot = 0xFFFF before calling. */

    const char *p = str;

    /* Skip optional opening parenthesis */
    if (*p == '(') p++;

    while (*p && *p != ')') {
        /* Skip whitespace and commas */
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p || *p == ')') break;

        /* Determine which category this is */
        int shift = -1;
        char cat = (char)toupper((unsigned char)*p);
        switch (cat) {
            case 'S': shift = VMS_PROT_SYSTEM_SHIFT; break;
            case 'O': shift = VMS_PROT_OWNER_SHIFT;  break;
            case 'G': shift = VMS_PROT_GROUP_SHIFT;   break;
            case 'W': shift = VMS_PROT_WORLD_SHIFT;   break;
            default:
                /* Unknown category - skip to next comma or end */
                while (*p && *p != ',' && *p != ')') p++;
                continue;
        }

        p++;  /* Skip category letter */
        if (*p == ':') p++;  /* Skip colon */

        /*
         * Parse the access type letters.
         * Each letter ALLOWS that access (clears the denied bit).
         * Start with all denied for this category.
         */
        uint8_t bits = VMS_PROT_R | VMS_PROT_W | VMS_PROT_E | VMS_PROT_D;

        while (*p && *p != ',' && *p != ')' && *p != ' ') {
            switch (toupper((unsigned char)*p)) {
                case 'R': bits &= ~VMS_PROT_R; break;
                case 'W': bits &= ~VMS_PROT_W; break;
                case 'E': bits &= ~VMS_PROT_E; break;
                case 'D': bits &= ~VMS_PROT_D; break;
                default:
                    /* Unknown access type - skip */
                    break;
            }
            p++;
        }

        /* Apply the parsed bits to the protection word */
        *prot = (uint16_t)((*prot & ~(0x0F << shift)) | (uint16_t)(bits << shift));
    }

    return SS$_NORMAL;
}
