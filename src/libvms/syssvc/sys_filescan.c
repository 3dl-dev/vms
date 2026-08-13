/*
 * sys_filescan.c - $FILESCAN File Specification Scan System Service
 *
 * SYS$FILESCAN performs a lightweight, purely lexical parse of a VMS file
 * specification string into its component fields (node, node access-control
 * string, device, root directory, directory, name, type, version) and returns
 * the position and length of each requested field within the caller's source
 * string.
 *
 * Unlike SYS$PARSE (RMS), $FILESCAN does NO filesystem I/O, NO directory
 * lookup, NO logical-name translation, and NO defaulting. It is the field
 * splitter that utilities such as MMK use to break a description-file file
 * specification into its parts (self-host MMK spine, vms-4f3 / vms-52c anchor).
 *
 * ----------------------------------------------------------------------------
 * PROVENANCE (Rule 8 — clean-room)
 *
 * Semantics derived ONLY from public VSI/HP OpenVMS documentation:
 *   - VSI OpenVMS System Services Reference Manual, $FILESCAN entry, and
 *   - the VMS file-specification grammar in the OpenVMS User's Manual /
 *     DCL Dictionary (F$PARSE).
 * No VSI/HPE/DEC source or binary was consulted.
 *
 * Documented facts this implementation rests on:
 *   - Arguments are  srcstr, valuelst, [fldflags].
 *   - valuelst is a list of item descriptors, each a 2-word + longword entry
 *     (length word, item-code word, component-address longword) — the ILE2
 *     layout in iledef.h. On return $FILESCAN writes each requested field's
 *     length and a pointer into srcstr; the list is terminated by a zero
 *     item-code entry.
 *   - Each returned field INCLUDES its punctuation: the device field includes
 *     its trailing colon; the directory field includes its brackets ("[ ]" or
 *     "< >"). By the same documented rule this code returns the node field
 *     with its trailing "::", the type field with its leading ".", and the
 *     version field with its leading ";".
 *   - fldflags is a longword mask with one bit set per component found.
 *
 * OVMX DESIGN CHOICES (labelled, where the byte-level behavior is NOT published
 * in a citable form):
 *   - Rooted-directory split: when two consecutive bracketed groups appear and
 *     the first ends in ".]" / ".>" (e.g. "[SYS0.][SYSEXE]"), the first group
 *     is returned as FSCN$_ROOT and the second as FSCN$_DIRECTORY. A single
 *     bracketed group is always FSCN$_DIRECTORY (even "[SYS0.]").
 *   - Version delimiter recognized is ";" (the canonical VMS form). A trailing
 *     ".n" is treated as a file type, not a version.
 *   - A structurally malformed spec (unterminated node access-control quote,
 *     unbalanced brackets) returns SS$_BADPARAM. The distinct SS$_BADFILENAME
 *     condition value real $FILESCAN can raise is NOT emitted, because its
 *     numeric value is not oracle-pinned in this tree (ssdef.h / vms-8019);
 *     the error is honest and even (never faked success — INV-6 / INV-DCL).
 *
 * Reference: OpenVMS System Services Reference Manual ($FILESCAN)
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * $FILESCAN is a pure lexical transform of the caller's own source string into
 * component (position, length) spans. It reads NO process, system, device, or
 * filesystem state -- no RMS I/O, no directory lookup, no logical-name
 * translation, no defaulting (that is SYS$PARSE's job, not this one). Its
 * answer comes entirely from the bytes the caller handed in.
 *
 * OVMX-USERSPACE: sys$filescan (vms-4f3) -- scans the caller-supplied file
 *     specification descriptor into the caller's item list; reads no process,
 *     system, device, or filesystem state.
 */

#include <stdint.h>
#include <stddef.h>
#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "iledef.h"
#include "fscndef.h"

/*
 * Parsed result: one (pointer, length) span per field, plus the presence mask.
 * A NULL pointer / zero length means the field was not present in the input.
 */
typedef struct {
    const char *node;      size_t node_len;
    const char *acs;       size_t acs_len;
    const char *device;    size_t device_len;
    const char *root;      size_t root_len;
    const char *dir;       size_t dir_len;
    const char *name;      size_t name_len;
    const char *type;      size_t type_len;
    const char *version;   size_t version_len;
    const char *filespec;  size_t filespec_len;
    uint32_t    flags;
} fscn_parse_t;

/* Is c an opening directory bracket? */
static inline int is_open_bracket(char c)  { return c == '[' || c == '<'; }
/* Closing bracket that matches the given opener. */
static inline char close_bracket_for(char c) { return c == '[' ? ']' : '>'; }

/*
 * Scan the file specification [s, s+n) into fields.
 * Returns SS$_NORMAL on a well-formed (scannable) spec, SS$_BADPARAM on a
 * structurally malformed one.
 */
static uint32_t filescan_parse(const char *s, size_t n, fscn_parse_t *out)
{
    const char *p   = s;
    const char *end = s + n;

    for (size_t i = 0; i < sizeof(*out); i++)
        ((char *)out)[i] = 0;

    /* FSCN$_FILESPEC: the whole scanned string. */
    if (n > 0) {
        out->filespec     = s;
        out->filespec_len = n;
    }

    /* ---- Node spec:  name["access"]::  --------------------------------- *
     * Locate a top-level "::" (a "::" that is not inside a quoted access-
     * control string). Everything up to and including the "::" is the node
     * field; the quoted "..." portion, if any, is the node access-control
     * string (FSCN$_NODE_ACS), returned WITH its surrounding quotes.        */
    {
        const char *q      = p;
        const char *dcolon = NULL;
        const char *acs_beg = NULL, *acs_end = NULL;
        while (q < end) {
            if (*q == '"') {
                const char *quote_start = q;
                q++;                       /* opening quote */
                /* Skip to the closing quote; "" is an embedded quote. */
                while (q < end) {
                    if (*q == '"') {
                        if (q + 1 < end && q[1] == '"') { q += 2; continue; }
                        break;             /* closing quote */
                    }
                    q++;
                }
                if (q >= end)
                    return SS$_BADPARAM;    /* unterminated access-control quote */
                acs_beg = quote_start;
                acs_end = q + 1;           /* include closing quote */
                q++;
                continue;
            }
            if (*q == ':' && q + 1 < end && q[1] == ':') {
                dcolon = q;
                break;
            }
            /* A single ':' before any "::" means there is no node — it is the
             * device delimiter. Stop looking for a node. */
            if (*q == ':')
                break;
            if (is_open_bracket(*q))
                break;                     /* directory begins; no node */
            q++;
        }
        if (dcolon) {
            out->node     = p;
            out->node_len = (size_t)(dcolon - p) + 2;   /* include "::" */
            out->flags   |= FSCN$M_NODE;
            if (acs_beg && acs_end && acs_beg < dcolon) {
                out->acs     = acs_beg;
                out->acs_len = (size_t)(acs_end - acs_beg);
                out->flags  |= FSCN$M_NODE_ACS;
            }
            p = dcolon + 2;                /* advance past "::" */
        }
    }

    /* ---- Device:  name:  ---------------------------------------------- *
     * A device is present when, scanning from p, the first structural char
     * is a single ':' (before any bracket). Returned WITH the trailing ':'. */
    {
        const char *q = p;
        while (q < end && *q != ':' && !is_open_bracket(*q))
            q++;
        if (q < end && *q == ':') {
            out->device     = p;
            out->device_len = (size_t)(q - p) + 1;      /* include ':' */
            out->flags     |= FSCN$M_DEVICE;
            p = q + 1;
        }
    }

    /* ---- Root + Directory:  [root.][dir]  ----------------------------- *
     * Parse up to two consecutive bracketed groups. Two groups where the
     * first ends in ".]" / ".>"  => (root, directory). One group => dir.    */
    if (p < end && is_open_bracket(*p)) {
        char open  = *p;
        char close = close_bracket_for(open);
        const char *g1 = p;
        const char *q  = p + 1;
        while (q < end && *q != close)
            q++;
        if (q >= end)
            return SS$_BADPARAM;           /* unbalanced directory bracket */
        const char *g1_end = q + 1;        /* include closing bracket */
        size_t g1_len = (size_t)(g1_end - g1);

        /* Does g1 look like a root ("...( . )]")? i.e. char before close is '.' */
        int g1_is_root = (g1_len >= 3 && *(g1_end - 2) == '.');

        /* Is there a second bracketed group immediately following? */
        const char *g2 = g1_end;
        if (g1_is_root && g2 < end && is_open_bracket(*g2)) {
            char open2  = *g2;
            char close2 = close_bracket_for(open2);
            const char *r = g2 + 1;
            while (r < end && *r != close2)
                r++;
            if (r >= end)
                return SS$_BADPARAM;       /* unbalanced directory bracket */
            const char *g2_end = r + 1;

            out->root     = g1;
            out->root_len = g1_len;
            out->flags   |= FSCN$M_ROOT;

            out->dir      = g2;
            out->dir_len  = (size_t)(g2_end - g2);
            out->flags   |= FSCN$M_DIRECTORY;
            p = g2_end;
        } else {
            out->dir      = g1;
            out->dir_len  = g1_len;
            out->flags   |= FSCN$M_DIRECTORY;
            p = g1_end;
        }
    }

    /* ---- name.type;version  ------------------------------------------- *
     * Remaining span is [p, end). Split off ';version' first (returned with
     * the leading ';'), then the LAST '.' introduces '.type' (with the
     * leading '.'); what precedes it is the name.                          */
    {
        const char *rest_beg = p;
        const char *rest_end = end;

        /* Version: from the first ';' to end. */
        const char *semi = rest_beg;
        while (semi < rest_end && *semi != ';')
            semi++;
        if (semi < rest_end) {             /* ';' found */
            out->version     = semi;
            out->version_len = (size_t)(rest_end - semi);   /* include ';' */
            out->flags      |= FSCN$M_VERSION;
            rest_end = semi;               /* name.type is before the ';' */
        }

        /* Type: last '.' in [rest_beg, rest_end). */
        const char *dot = NULL;
        for (const char *r = rest_beg; r < rest_end; r++)
            if (*r == '.')
                dot = r;

        const char *name_end = (dot ? dot : rest_end);
        if (name_end > rest_beg) {
            out->name     = rest_beg;
            out->name_len = (size_t)(name_end - rest_beg);
            out->flags   |= FSCN$M_NAME;
        }
        if (dot) {
            out->type     = dot;
            out->type_len = (size_t)(rest_end - dot);       /* include '.' */
            out->flags   |= FSCN$M_TYPE;
        }
    }

    return SS$_NORMAL;
}

/* Map an FSCN$_ item code to its parsed (pointer, length) span. Returns 0 for
 * an unknown code, 1 otherwise (present or absent — absent yields NULL/0). */
static int fscn_field_for_code(const fscn_parse_t *pr, uint16_t code,
                               const char **ptr, size_t *len)
{
    switch (code) {
    case FSCN$_NODE:      *ptr = pr->node;     *len = pr->node_len;     return 1;
    case FSCN$_NODE_ACS:  *ptr = pr->acs;      *len = pr->acs_len;      return 1;
    case FSCN$_DEVICE:    *ptr = pr->device;   *len = pr->device_len;   return 1;
    case FSCN$_ROOT:      *ptr = pr->root;     *len = pr->root_len;     return 1;
    case FSCN$_DIRECTORY: *ptr = pr->dir;      *len = pr->dir_len;      return 1;
    case FSCN$_NAME:      *ptr = pr->name;     *len = pr->name_len;     return 1;
    case FSCN$_TYPE:      *ptr = pr->type;     *len = pr->type_len;     return 1;
    case FSCN$_VERSION:   *ptr = pr->version;  *len = pr->version_len;  return 1;
    case FSCN$_FILESPEC:  *ptr = pr->filespec; *len = pr->filespec_len; return 1;
    default:              *ptr = NULL;         *len = 0;                return 0;
    }
}

uint32_t sys$filescan(
    const struct dsc$descriptor_s *srcstr,
    ILE2                          *valuelst,
    uint32_t                      *fldflags)
{
    if (!srcstr || !valuelst)
        return SS$_BADPARAM;

    const char *s = srcstr->dsc$a_pointer;
    size_t      n = srcstr->dsc$w_length;
    if (n > 0 && !s)
        return SS$_BADPARAM;

    fscn_parse_t pr;
    uint32_t status = filescan_parse(s, n, &pr);
    if (!$VMS_STATUS_SUCCESS(status)) {
        /* Malformed spec: report no components and return the error. */
        if (fldflags)
            *fldflags = 0;
        return status;
    }

    /* First pass: validate every item code before writing anything, so an
     * invalid list is rejected atomically (no half-filled output). */
    for (ILE2 *e = valuelst; e->ile2$w_code != 0; e++) {
        const char *ptr; size_t len;
        if (!fscn_field_for_code(&pr, e->ile2$w_code, &ptr, &len))
            return SS$_BADPARAM;
    }

    /* Second pass: fill the caller's item list. */
    for (ILE2 *e = valuelst; e->ile2$w_code != 0; e++) {
        const char *ptr; size_t len;
        fscn_field_for_code(&pr, e->ile2$w_code, &ptr, &len);
        e->ile2$w_length   = (uint16_t)len;
        e->ile2$ps_bufaddr = (void *)ptr;   /* pointer into srcstr, or NULL */
    }

    if (fldflags)
        *fldflags = pr.flags;

    return SS$_NORMAL;
}
