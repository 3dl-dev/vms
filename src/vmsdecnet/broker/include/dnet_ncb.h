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

#define DNET_NCB_OK        0
#define DNET_NCB_EINVAL  (-1)   /* null argument                                 */
#define DNET_NCB_ETRUNC  (-2)   /* no "::" separator / empty node or object      */
#define DNET_NCB_EBADLEN (-3)   /* a field over its bound                        */

/* DECnet Phase IV node names are <= 6 chars; the "area.node" literal form is a
 * little longer. A task/object name is <= 16 (DNA Session Control end-user
 * descriptor). Generous caps with a NUL. */
#define DNET_NCB_MAXNODE  16
#define DNET_NCB_MAXTASK  16

struct dnet_ncb {
    char     node[DNET_NCB_MAXNODE + 1];  /* target node NAME or "area.node" literal */
    char     task[DNET_NCB_MAXTASK + 1];  /* named task/object (is_named == 1)       */
    unsigned object;                       /* object NUMBER (is_named == 0)           */
    int      is_named;                     /* 1 = named task (format 1), 0 = number   */
};

/*
 * dnet_ncb_parse - parse ncb[0..len-1] into *out. Returns DNET_NCB_OK, or
 * DNET_NCB_EINVAL / ETRUNC (no "::", empty node/object) / EBADLEN (a field over
 * its bound). Never reads past ncb[len-1]; *out is zeroed first.
 *
 * Object-spec grammar (inside the optional quotes after "::"):
 *   TASK=<name> | 0=<name>   -> a NAMED task (is_named = 1, task = <name>)
 *   <digits>                 -> an object NUMBER (is_named = 0, object = value)
 *   <name>                   -> a NAMED task (is_named = 1, task = <name>)
 */
int dnet_ncb_parse(const char *ncb, size_t len, struct dnet_ncb *out);

#endif /* DNET_NCB_H */
