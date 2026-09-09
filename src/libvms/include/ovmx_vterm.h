/*
 * ovmx_vterm.h - the VIRTUAL TERMINAL service: mint / release the RTAn: an
 * inbound network login session runs on (rd vms-f40, design
 * docs/design/faithful-sessions-and-network-subsystems.md sec 3.2 / 6-P4).
 *
 * WHY THIS EXISTS, AND WHY IT IS HERE AND NOT IN THE NETWORK DAEMON. The
 * design's first rule for sessions is a NEGATIVE property of the callers:
 * "Nothing above the VMS layer forks, execs, opens a pty, or dup2s" (sec 3.1).
 * $CREPRC honoured it for the process and the terminal BINDING -- but an
 * inbound SET HOST also has to CREATE the terminal, and there was nowhere
 * below the VMS layer to do that, which is exactly how the first CTERM cut
 * ended up openpty()ing and fork()+execvp()ing a shell in the DECnet daemon
 * with no authentication at all.
 *
 * So this is that missing floor. The DECnet CTERM host calls
 * ovmx_vterm_create(), receives a VMS DEVICE NAME the EXECUTIVE assigned, and
 * hands that name to $CREPRC. It never sees a pty, never names a unit, and
 * never learns a substrate path -- the same discipline JOB_CONTROL follows for
 * the console.
 *
 * WHAT IS AND IS NOT VMS HERE (Rule 1 / Rule 8). "A virtual terminal exists
 * for the duration of an inbound network login, is named RTAn:, is visible to
 * $GETDVI from any process, and is owned by the session's job" is VMS, and is
 * what this produces. "It is backed by a pseudo-terminal pair" is the OVMX
 * substrate choice, labelled as such: real VMS mints RTAn: inside RTTDRIVER at
 * NETACP's request and moves bytes by $QIO. The master handle this returns is
 * the substrate byte channel, and the ONE thing the layered product may hold;
 * the device identity, ownership and characteristics are the executive's.
 *
 * INV-6: every failure is honest. If the executive cannot be reached, or has
 * no free unit, no device is invented and no session is created -- the inbound
 * connect is rejected. There is no per-process fallback terminal.
 */
#ifndef OVMX_VTERM_H
#define OVMX_VTERM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ovmx_vterm_create - mint a virtual terminal for a network login session.
 *
 * On success (SS$_NORMAL) `devnam` holds the EXECUTIVE-ASSIGNED VMS device
 * name (e.g. "RTA0:", NUL-terminated) and *master_fd holds the substrate byte
 * channel for the session's terminal -- what the layered product reads the
 * session's output from and writes its input to.
 *
 * The caller passes NO name and NO path: the executive chooses the unit (so
 * two inbound sessions cannot collide, and a daemon cannot claim a unit it was
 * not given), and the substrate is opened here.
 *
 * Returns SS$_NORMAL, SS$_DEVALLOC (no free unit / the substrate refused a new
 * pair), SS$_DEVOFFLINE (the pty pair could not be prepared), SS$_BADPARAM, or
 * whatever status the executive returned. On any failure `devnam` is empty,
 * *master_fd is -1, and nothing was left half-created.
 */
uint32_t ovmx_vterm_create(char *devnam, size_t devnam_size, int *master_fd);

/*
 * ovmx_vterm_delete - release a virtual terminal minted by ovmx_vterm_create:
 * withdraw the executive's device row and close the substrate channel. Called
 * when the session ends, exactly as the row appeared when it began.
 * `master_fd` < 0 is accepted (already closed). Returns the executive's status
 * for the withdrawal; the substrate channel is closed either way.
 */
uint32_t ovmx_vterm_delete(const char *devnam, int master_fd);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VTERM_H */
