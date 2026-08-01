/*
 * OVMX_STATUS.H - Condition values that are OVMX's, NOT OpenVMS's.
 *
 * ============================================================
 * NOTHING IN THIS FILE IS A VMS CONDITION VALUE. THAT IS THE
 * ENTIRE REASON THE FILE EXISTS SEPARATELY FROM ssdef.h.
 * ============================================================
 *
 * ssdef.h says of itself: "The actual numeric values below match the
 * real OpenVMS definitions." Every value in it is pinned to the
 * reference lab or to public OpenVMS documentation, and a value that
 * is not pinned must never be filed next to ones that are -- a reader
 * who cannot tell a measurement from an invention will eventually
 * quote the invention as VMS behaviour.
 *
 * WHY AN OVMX-DEFINED CONDITION IS EVER LEGAL (CLAUDE.md Rule 10).
 * Rule 10 gives two legal answers for any condition: reproduce what
 * VMS does, or make the condition unreachable. A codes-of-our-own
 * facility is NOT a third answer and must not be used as one. It is
 * for the narrow case where OVMX's implementation strategy creates a
 * state OpenVMS's does not have, the state is genuinely reachable,
 * and reporting it as a VMS condition would be a lie about VMS.
 * Every constant here must carry, in its own comment, WHY the state
 * exists in OVMX and WHY OpenVMS cannot reach it. If you cannot write
 * those two sentences, you are inventing a handler for a condition
 * VMS never faces, and the answer is to delete the path instead.
 *
 * HOW IT IS LABELLED, and why this labelling is VMS-native rather
 * than OVMX-shaped (CLAUDE.md Rule 8): OpenVMS's own status-value
 * layout reserves bit 27 (STS$V_CUST_DEF) for condition values
 * defined by someone other than the operating-system vendor -- see
 * the status-value layout in stsdef.h and the OpenVMS Programming
 * Concepts Manual. Setting it is the documented way to say "this
 * condition is not ours". So an OVMX status is self-describing: any
 * code, including a VMS one, can test $VMS_STATUS_CUST_DEF() and know
 * it is not looking at a system service's condition value.
 *
 * The facility NUMBER (OVMX_FACILITY below) is an OVMX design choice
 * and is labelled as such. Public OpenVMS documentation does not
 * publish an assignment for third-party facility numbers, so OVMX
 * picks one out of the customer-defined space rather than presenting
 * a number as authoritative.
 */

#ifndef __OVMX_STATUS_H
#define __OVMX_STATUS_H

#include <stdint.h>
#include "stsdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OVMX's facility number -- an OVMX DESIGN CHOICE, not a VMS
 * assignment. Every value below carries STS$M_CUST_DEF, so nothing
 * here can be mistaken for a SYSTEM-facility condition value however
 * the number is read.
 */
#define OVMX_FACILITY   2048

#define OVMX_STATUS(sev, msg)  STS$VALUE((sev), OVMX_FACILITY, (msg), 0, 1)

/*
 * OVMX$_PRCLOST -- $CREPRC's forked task died before the executive
 * entered it in the process table, so no VMS process was created.
 *
 * WHY THE STATE EXISTS IN OVMX: OVMX creates a process with fork(),
 * and an entry in the executive's process table is keyed by the new
 * task's own tgid -- so only the CHILD can enter itself, and it
 * reports the executive-assigned process ID back to $CREPRC over a
 * pipe (see the creation-handshake comment in
 * src/libvms/syssvc/sys_process.c). Between fork() returning and that
 * report there is a window in which the task can be destroyed by
 * something outside OVMX entirely -- an external SIGKILL, the OOM
 * killer. If it is, nothing was ever entered in the table: there is
 * no process name, no process ID, no row. Nothing exists to hand
 * back.
 *
 * WHY OPENVMS CANNOT REACH IT: on OpenVMS the EXECUTIVE creates the
 * process. The PCB exists, and its process ID is assigned, before
 * anything can run in it and therefore before anything can kill it,
 * so $CREPRC always has a process ID to return. A process that dies
 * immediately afterwards is a created process that died -- reported
 * to the creator through a termination mailbox, not through
 * $CREPRC's status. There is no OpenVMS condition value for "the
 * creator could not hear the created process", because the creator
 * never has to.
 *
 * WHAT IT REPLACED, and why that mattered: this path used to return
 * SS$_NORMAL with *pidadr left at zero. Success paired with a process
 * ID that names no process is a status combination OpenVMS does not
 * produce, and it is the "reports success while sharing nothing"
 * shape the executive-facility work exists to delete. Severity is
 * SEVERE, so the value is even and every existing `status & 1` caller
 * sees a failure.
 */
#define OVMX$_PRCLOST   OVMX_STATUS(STS$K_SEVERE, 1)

#ifdef __cplusplus
}
#endif

#endif /* __OVMX_STATUS_H */
