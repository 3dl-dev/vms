/*
 * dnet_nodespec.h - split a VMS DECnet node-filespec into its parts.
 *
 * A DCL COPY (or any RMS file operation) can name a file on a remote DECnet
 * node with an access-control string:
 *
 *     NODE::dev:[dir]file.ext
 *     NODE"username password"::dev:[dir]file.ext
 *     NODE"username password account"::dev:[dir]file.ext
 *
 * The FAL/DAP client (dnet_fal_client_put/get) takes a BARE remote file spec
 * (no node prefix) and a transport that has already CONNECTED to the remote
 * node's object 17 carrying the access-control credentials -- so before it can
 * run, the node name, the username/password/account, and the remaining (node-
 * stripped) file spec have to be separated out. That decomposition is this
 * module: the username/password/account feed dnet_cterm_sc_connect_build (the
 * object-17 connect-data encoder) and the stripped spec feeds the FAL client.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): the syntax split on a top-level "::" (one not
 * inside a quoted access-control string) and the space-separated
 * username/password/account layout are the uncopyrightable, publicly specified
 * DECnet file-spec facts (mirrored from $FILESCAN's FSCN$_NODE handling,
 * src/libvms/syssvc/sys_filescan.c, and rms_parse.c). This C is OVMX's own.
 *
 * INV-6 (no facade, no silent truncation): every field is bounds-checked; an
 * over-long field, an unterminated access string, or a spec with no file part
 * is REFUSED with a distinct error, never clipped or invented.
 *
 * rd vms-ea8 (wire the outbound FAL client into DCL) / vms-6a4 (NODE"user pw"::
 * COPY via FAL); the FAL/DAP transfer engine it feeds is the merged vms-8c2.
 */
#ifndef DNET_NODESPEC_H
#define DNET_NODESPEC_H

#include <stddef.h>
#include "dnet_cterm.h"   /* DNET_SC_MAX_STR + the DNET_CTERM_* return codes */

/* Node names/addresses: Phase IV node names are <= 6 chars, addresses "a.n";
 * 64 is generous headroom and matches the wire string cap. */
#define DNET_NODESPEC_MAXNODE  64
/* The node-stripped file spec (dev:[dir]name.ext;ver) -- RMS's own limit. */
#define DNET_NODESPEC_MAXFILE  255

/* dnet_nodespec_parse returns this (a POSITIVE value, distinct from the
 * negative DNET_CTERM_* errors and from DNET_CTERM_OK==0) when the spec is
 * well-formed but carries NO node prefix -- the caller uses the local path. */
#define DNET_NODESPEC_NONODE   1

struct dnet_nodespec {
    char node[DNET_NODESPEC_MAXNODE + 1];    /* "OVMXR3" or "1.42"           */
    char username[DNET_SC_MAX_STR + 1];      /* RQSTRID, "" if none          */
    char password[DNET_SC_MAX_STR + 1];      /* PASSWRD, "" if none          */
    char account[DNET_SC_MAX_STR + 1];       /* ACCOUNT, "" if none          */
    char filespec[DNET_NODESPEC_MAXFILE + 1];/* node-stripped remote spec    */
    int  has_access;                         /* 1 iff a "..." string present */
};

/*
 * Parse `spec` into `*out`.
 *   DNET_CTERM_OK (0)      - parsed a node prefix; *out fully populated.
 *   DNET_NODESPEC_NONODE   - no top-level "::"; *out zeroed (use local path).
 *   DNET_CTERM_EINVAL      - null arg, empty node name, empty file part, a
 *                            stray token after a closed access string, or a
 *                            non-printable byte in a field.
 *   DNET_CTERM_EBADLEN     - an unterminated access string, or any field over
 *                            its bound (refused, never truncated).
 */
int dnet_nodespec_parse(const char *spec, struct dnet_nodespec *out);

/*
 * dnet_copy_plan - decide the direction of a two-argument DECnet file COPY.
 *
 * A `COPY <src> <dst>` where exactly ONE side carries a DECnet node prefix is an
 * outbound FAL/DAP transfer; the side WITHOUT a node is the local file:
 *   - remote SOURCE  -> GET  (remote -> local): out->is_get = 1
 *   - remote DEST    -> PUT  (local -> remote): out->is_get = 0
 * The remote side's node + access-control creds + node-stripped spec are lifted
 * into `out` (via dnet_nodespec_parse); the local side is taken verbatim as the
 * local file spec (the splitter reports a no-node spec as DNET_NODESPEC_NONODE
 * without parsing a filespec, so the raw local argument is used).
 *
 * This is the COPY-command policy layer the FAL client + the object-17 connect
 * builder sit behind; it feeds dnet_cterm_sc_connect_build (creds) and
 * dnet_fal_client_get/put (remote_spec/local_spec).
 */
struct dnet_copy_plan {
    int  is_get;                                 /* 1 GET remote->local, 0 PUT local->remote */
    char node[DNET_NODESPEC_MAXNODE + 1];        /* the remote node                          */
    char username[DNET_SC_MAX_STR + 1];          /* access-control creds, from the remote    */
    char password[DNET_SC_MAX_STR + 1];          /* spec's "..." string ("" if none)         */
    char account[DNET_SC_MAX_STR + 1];
    char remote_spec[DNET_NODESPEC_MAXFILE + 1]; /* node-stripped remote file spec           */
    char local_spec[DNET_NODESPEC_MAXFILE + 1];  /* the local file spec (verbatim)           */
    int  has_access;                             /* 1 iff the remote carried a "..." string  */
};

/*
 * Plan a COPY. Returns:
 *   DNET_CTERM_OK        - exactly one side is remote; *out filled.
 *   DNET_CTERM_EINVAL    - null arg, OR neither side has a node (a local COPY, not
 *                          this path), OR BOTH sides have a node (node-to-node
 *                          COPY is not this outbound-client path).
 *   DNET_CTERM_EBADLEN   - a field over its bound (propagated from the splitter,
 *                          or the local spec too long).
 * Any other splitter error (an unterminated access string, etc.) is propagated.
 */
int dnet_copy_plan(const char *src, const char *dst, struct dnet_copy_plan *out);

#endif /* DNET_NODESPEC_H */
