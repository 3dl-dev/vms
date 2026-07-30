/*
 * term_map.c - SSH TERM -> OVMX terminal device-type label.
 *
 * See term_map.h for the Rule 8/Rule 10 label: this mapping is an OVMX
 * design choice, not a reproduction of any documented OpenVMS behavior.
 * Kept dependency-free (no libssh) so it links into both vmssshd and its
 * unit test (tests/vmsssh/test_term_mapping.c) unconditionally.
 */

#include "term_map.h"
#include <strings.h>   /* strncasecmp, strcasecmp */

const char *vmsssh_map_term_to_device_type(const char *term)
{
    if (!term || !term[0])
        return "VT100";

    /* vt400/vt420 family */
    if (strncasecmp(term, "vt4", 3) == 0)
        return "VT400";

    /* vt300/vt320 family */
    if (strncasecmp(term, "vt3", 3) == 0)
        return "VT300";

    /* vt200/vt220 family */
    if (strncasecmp(term, "vt2", 3) == 0)
        return "VT200";

    /* vt100 family */
    if (strncasecmp(term, "vt1", 3) == 0)
        return "VT100";

    /* xterm-256color -> VT300 (color capable) — OVMX heuristic, not VMS. */
    if (strcasecmp(term, "xterm-256color") == 0)
        return "VT300";

    /* xterm* -> VT100 — OVMX heuristic, not VMS. */
    if (strncasecmp(term, "xterm", 5) == 0)
        return "VT100";

    /* Fallback: dumb, unknown, anything else -> VT100 */
    return "VT100";
}
