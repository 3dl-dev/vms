/*
 * dnet_ncb.h - parse a DECnet task-to-task Network Connect Block (rd vms-22c,
 *              a1-2, the qio_net_op client side).
 *
 * The NCB is the LOCAL API structure an application hands to $QIO IO$_ACCESS on
 * a _NET: channel to name the remote task it wants to connect to -- the
 * connect-string form documented in the public Guide to DECnet-VAX Networking /
 * OpenVMS I/O User's Reference:
 *
 *     node-spec::"task-spec"
 *     e.g.  MYNODE::"TASK=SERVER"   0=SERVER (named task, object 0)
 *           MYNODE::"17"            a well-known object NUMBER
 *           1.11::"TASK=ECHO"       an area.node literal target
 *
 * IT NEVER GOES ON THE WIRE. qio_net_op parses it here into {node, task/object}
 * and then builds the Session Control connect descriptor
 * (dnet_cterm_sc_connect_build[_task]) -- and THAT wire form is already
 * oracle-grounded byte-exact vs a real VAX. So this parser is a LOCAL API
 * contract, SPEC-DERIVED from the public manual (Rule 8), not a wire-field: it
 * cannot crash a peer, and it is grounded against the documented connect-string
 * form (re-ground if a real capture ever contradicts it). Fully BOUNDS-CHECKED:
 * a malformed NCB is refused, never over-read; *out is zeroed on any failure.
 */
#ifndef DNET_NCB_H
#define DNET_NCB_H

#include <stddef.h>
#include <stdint.h>

#define DNET_NCB_OK        0
#define DNET_NCB_EINVAL  (-1)   /* null argument                                 */
#define DNET_NCB_ETRUNC  (-2)   /* no "::" separator / empty node or object      */
#define DNET_NCB_EBADLEN (-3)   /* a field over its bound                        */
#define DNET_NCB_ENOTACC (-4)   /* not the inbound-accept NCB form (below)       */

/* DECnet Phase IV node names are <= 6 chars; the "area.node" literal form is a
 * little longer. A task/object name is <= 16 (DNA Session Control end-user
 * descriptor). Generous caps with a NUL. */
#define DNET_NCB_MAXNODE  16
#define DNET_NCB_MAXTASK  16
/* Access-control strings (DNA Session Control RQSTRID / PASSWRD / ACCOUNT are
 * counted fields of at most 39 bytes). */
#define DNET_NCB_MAXACC   39

struct dnet_ncb {
    char     node[DNET_NCB_MAXNODE + 1];  /* target node NAME or "area.node" literal */
    char     task[DNET_NCB_MAXTASK + 1];  /* named task/object (is_named == 1)       */
    unsigned object;                       /* object NUMBER (is_named == 0)           */
    int      is_named;                     /* 1 = named task (format 1), 0 = number   */
    /* Optional access control, NODE"user password account"::  (rd vms-dda). */
    int      has_access;
    char     user[DNET_NCB_MAXACC + 1];
    char     password[DNET_NCB_MAXACC + 1];
    char     account[DNET_NCB_MAXACC + 1];
};

/*
 * dnet_ncb_parse - parse ncb[0..len-1] into *out. Returns DNET_NCB_OK, or
 * DNET_NCB_EINVAL / ETRUNC (no "::", empty node/object) / EBADLEN (a field over
 * its bound). Never reads past ncb[len-1]; *out is zeroed first.
 *
 * Node-spec grammar (before "::"):
 *   <node>                           -> no access control
 *   <node>"<user> [<password> [<account>]]"
 *                                    -> access control (has_access = 1), the
 *                                       quoted string split on blanks
 *
 * Object-spec grammar (inside the optional quotes after "::"):
 *   TASK=<name> | 0=<name>   -> a NAMED task (is_named = 1, task = <name>)
 *   <digits>                 -> an object NUMBER (is_named = 0, object = value)
 *   <digits>=                -> an object NUMBER, the documented "17=" form
 *   <name>                   -> a NAMED task (is_named = 1, task = <name>)
 * An optional "/<connect data>" tail on the object spec is accepted and ignored.
 */
int dnet_ncb_parse(const char *ncb, size_t len, struct dnet_ncb *out);

/*
 * THE INBOUND-ACCEPT NCB (rd vms-0a7 S3) -- OVMX's OWN DOCUMENTED TEXT FORM,
 * LABELLED: it is NOT VMS's binary NCB layout and claims nothing about it.
 *
 * On VMS, NETACP creates a network object's server process with the logical
 * name SYS$NET translating to an NCB that names the inbound link; the server
 * $ASSIGNs _NET: and issues IO$_ACCESS with that NCB to ACCEPT the link
 * (VMS's NCB carries the remote node, the object, the user and, after a '/',
 * the link identification and the connect data). OVMX's form keeps that SHAPE,
 * as text a $TRNLNM can return and a $QIO can carry:
 *
 *     <remote-node>::"<object-number>=<user>/#<link handle, 8 hex digits>"
 *     e.g.  VAX1::"17=GUEST/#9A3C01F2"
 *
 * The handle is the NETACP-issued, unguessable link handle of the waiting
 * inbound link; NETACP accepts the link only for the process it created for it
 * (the executive-stamped sender PID), so a guessed NCB from any other process
 * is refused. The "/#" tail is what makes the form unambiguous: the outbound
 * connect NCB above never has a '#' after its '/'.
 *
 * dnet_ncb_accept_parse - parse ncb[0..len-1]. DNET_NCB_OK, or EINVAL (null),
 * ENOTACC (not this form), EBADLEN (a field over its bound). Bounds-checked,
 * never reads past ncb[len-1]; *out is zeroed on any failure. The text is
 * untrusted (any local process can hand any bytes to IO$_ACCESS): every byte
 * is checked, fuzzed by the broker selftest.
 *
 * dnet_ncb_accept_format - the same form into buf (cap with the NUL); *outlen
 * the length without it. Refuses (EBADLEN / ENOTACC) a field the parser would
 * not read back exactly, so format -> parse always round-trips.
 */
#define DNET_NCB_ACC_MAXOBJ  255u

struct dnet_ncb_accept {
    char     node[DNET_NCB_MAXNODE + 1];   /* the remote node (name or address)   */
    unsigned object;                       /* the object number, 1..255           */
    char     user[DNET_NCB_MAXACC + 1];    /* the authenticated user              */
    uint32_t handle;                       /* NETACP's link handle, never 0       */
};

int dnet_ncb_accept_parse(const char *ncb, size_t len, struct dnet_ncb_accept *out);
int dnet_ncb_accept_format(const char *node, unsigned object, const char *user,
                           uint32_t handle, char *buf, size_t cap, size_t *outlen);

#endif /* DNET_NCB_H */
