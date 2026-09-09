/*
 * dnet_cterm_host.h - the CTERM HOST SESSION: what an inbound $ SET HOST
 * actually reaches on this node (rd vms-f40, design
 * docs/design/faithful-sessions-and-network-subsystems.md sec 6-P4).
 *
 * ================== WHAT THIS FIXES, IN ONE PARAGRAPH ==================
 * The first CTERM host cut answered an inbound connect to Session Control
 * object 42 by openpty()ing a pty and fork()+execvp()ing `vmsdcl --login`.
 * A remote SET HOST therefore reached a BARE DCL PROMPT WITH NO
 * AUTHENTICATION AT ALL -- anyone who could put a DECnet frame on the wire
 * had a shell. This module is the replacement, and its whole shape is the
 * fix: an inbound object-42 connect MINTS AN RTAn: THROUGH THE EXECUTIVE and
 * then creates a process running LOGINOUT.EXE on it, via the same $CREPRC
 * PRC$M_INTER|PRC$M_LOGINOUT primitive the console login uses. LOGINOUT
 * challenges the remote user for Username and Password, refuses bad
 * credentials, and honours DISUSER -- because it is the same LOGINOUT, on a
 * real device, in a real process, with no special case for the network.
 * =======================================================================
 *
 * THE ORACLE SAYS THIS IS ALSO THE FAITHFUL SHAPE, not merely the safe one.
 * docs/oracle/vax-sethost-cterm.{md,pcap,console.txt} (rd vms-558) captured a
 * real OpenVMS VAX V7.3 -> V7.3 SET HOST. Two facts from it govern this file:
 *
 *   1. The connect carries the SOURCE node::user in the Session Control
 *      SOURCE DESCRIPTOR ("SYSTEM"), and its ACCESS-CONTROL fields are EMPTY.
 *      There is no password on the wire.
 *   2. The remote prompts FRESH for Username AND Password, and surfaces the
 *      carried identity only as `Remote Port Info: 1025::SYSTEM` on the
 *      virtual terminal.
 *
 * So the carried identity is PROXY / ACCOUNTING INFORMATION, never a
 * credential. This module records it (dnet_cterm_host_session.remote_port_info)
 * and passes it to NOTHING that decides anything. There is no code path here
 * that can turn a wire-supplied name into a logged-in session; the decoder
 * does not even retain the password bytes (see dnet_cterm.h).
 *
 * LAYER BOUNDARY. dnet_cterm.{c,h} stays PURE -- codec + FSM, no socket, no
 * process, no allocation. This file is the impure half that binds that FSM to
 * the executive, and it is deliberately a SEPARATE translation unit and a
 * separate library target so the purity of the codec is a build-enforced fact.
 * It touches no socket either: the caller owns the datalink and the NSP link,
 * and hands this module only the connect data and the session's bytes.
 *
 * NO LINUX MECHANICS LIVE HERE. No fork, no exec, no openpty, no dup2. The
 * terminal comes from ovmx_vterm_create() (src/libvms/syssvc/sys_vterm.c) and
 * the process from $CREPRC -- both below the VMS layer, both shared with the
 * console path. tests/integration/test_creprc_session_primitive.sh scans this
 * file for exactly those calls and fails if any reappears.
 */
#ifndef DNET_CTERM_HOST_H
#define DNET_CTERM_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "dnet_cterm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "Remote Port Info" is a human/accounting surface: "<addr>::<user>". */
#define DNET_CTERM_HOST_RPI_MAX   80
#define DNET_CTERM_HOST_DEVNAM    16

/*
 * One inbound SET HOST session. Everything identifying in here that came off
 * the wire is PROXY INFORMATION; the authenticated identity of the session
 * lives in the EXECUTIVE (the created process's row), put there by LOGINOUT
 * after it authenticated, and is read with $GETJPI like any other process's.
 */
struct dnet_cterm_host_session {
    struct dnet_cterm_session cterm;      /* the protocol FSM (HOST role)      */

    char     devnam[DNET_CTERM_HOST_DEVNAM]; /* executive-assigned "RTA0:"     */
    int      master_fd;                   /* substrate byte channel, or -1     */
    uint32_t session_pid;                 /* VMS pid $CREPRC returned          */
    int      active;                      /* a session is live                 */

    /* The proxy/accounting identity, RENDERED from the validated descriptor
     * (vms-515 §3.4). The privileged session object holds no parsed wire struct
     * and no credential material -- only this human/accounting string. */
    char     remote_port_info[DNET_CTERM_HOST_RPI_MAX];
};

/*
 * dnet_cterm_host_open - accept an inbound Session Control connect and create
 * the AUTHENTICATED session it asks for.
 *
 * `conn_data`/`conn_len` are the connect data exactly as the NSP Connect
 * Initiate delivered them -- UNTRUSTED, UNAUTHENTICATED bytes. `peer_addr` is
 * the peer's Phase IV address as the ENGINE decoded it from the routing header
 * (executive/engine state, not something the peer wrote into the connect
 * message), used only for the Remote Port Info surface.
 *
 * On success (SS$_NORMAL) a process running LOGINOUT.EXE exists, bound to the
 * RTAn: named in hs->devnam, and hs->master_fd is the byte channel to it. The
 * caller has NOT authenticated anyone and holds no credential: LOGINOUT will
 * do the challenging over that channel.
 *
 * Refusals are honest and leave nothing behind (INV-6):
 *   SS$_BADPARAM   malformed connect data (the bounded parse rejected it), or
 *                  a connect naming an object this module is not. There is no
 *                  SS$_NOSUCHOBJ in OVMX's ssdef.h and none is invented here
 *                  (Rule 8: an unpinned condition value must not be filed
 *                  beside measured ones). The object check is belt-and-braces
 *                  anyway -- the DISPATCH by object number belongs to the
 *                  caller, and the DNA-level answer a peer sees is the link
 *                  disconnect the caller then sends.
 *   SS$_DEVALLOC   the executive had no free RTAn: unit
 *   anything else  the executive's own status from the mint or the $CREPRC
 */
/*
 * dnet_cterm_host_open_desc - THE PRIVILEGED CONTROL PATH (design vms-515 §3.4).
 * Accept a VALIDATED, TYPED connect descriptor and create the AUTHENTICATED
 * session it asks for. This function parses NO wire bytes -- it is handed only a
 * `struct dnet_conn_descriptor` that the low-privilege parser already bounded
 * and validated. A descriptor that did not come from that parser has
 * validated == 0 and is refused (SS$_BADPARAM) before any device or process
 * exists; so is a descriptor naming any object but 42. This is the seam a
 * fuzzed/hostile inbound frame cannot cross: the attacker bytes are decoded far
 * from here, and only a typed, bounded, credential-free descriptor arrives.
 *
 * On success (SS$_NORMAL) a process running LOGINOUT.EXE exists, bound to the
 * RTAn: named in hs->devnam, and hs->master_fd is the byte channel to it.
 * Refusals are honest and leave nothing behind (INV-6), same statuses as below.
 */
uint32_t dnet_cterm_host_open_desc(struct dnet_cterm_host_session *hs,
                                   const struct dnet_conn_descriptor *desc);

/*
 * dnet_cterm_host_open - the thin low-privilege convenience entry for a caller
 * that holds the raw connect bytes: it runs the low-privilege parse
 * (dnet_conn_descriptor_from_wire) and hands the resulting descriptor to
 * dnet_cterm_host_open_desc above. NETACP's serve loop calls the two steps
 * explicitly instead, so the isolation seam is visible at the call site.
 */
uint32_t dnet_cterm_host_open(struct dnet_cterm_host_session *hs,
                              const uint8_t *conn_data, size_t conn_len,
                              uint16_t peer_addr);

/*
 * Move bytes between the session and the caller's CTERM/NSP link. read: what
 * LOGINOUT/DCL has written to the terminal (to be shipped as a CTERM Write);
 * write: what the remote terminal typed (delivered by a CTERM Read Data).
 * Both return the byte count, 0 for "nothing right now", or -1 when the
 * session has ended (its terminal channel closed). Neither blocks
 * indefinitely: the channel is non-blocking.
 */
long dnet_cterm_host_read(struct dnet_cterm_host_session *hs,
                          uint8_t *buf, size_t cap);
long dnet_cterm_host_write(struct dnet_cterm_host_session *hs,
                           const uint8_t *buf, size_t len);

/* The substrate channel to poll(), or -1. The ONE substrate detail a caller
 * needs, and the caller may do nothing with it but wait on it. */
int dnet_cterm_host_fd(const struct dnet_cterm_host_session *hs);

/*
 * Is the session process still alive? Read from the EXECUTIVE ($GETJPI on the
 * pid $CREPRC returned), never from a wait() on a child -- an interactive
 * process is ownerless, the top of its own job, so this caller has no child to
 * reap and must ask the executive like any other process would.
 */
int dnet_cterm_host_alive(const struct dnet_cterm_host_session *hs);

/*
 * dnet_cterm_host_close - end the session: withdraw the executive's RTAn: row
 * and close the byte channel. Returns the executive's withdrawal status.
 */
uint32_t dnet_cterm_host_close(struct dnet_cterm_host_session *hs);

#ifdef __cplusplus
}
#endif

#endif /* DNET_CTERM_HOST_H */
