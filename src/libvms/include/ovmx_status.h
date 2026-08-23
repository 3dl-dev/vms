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

/*
 * OVMX$_NOSUBPRC -- the command asked RUN to create a SUBPROCESS, and
 * OVMX's RUN cannot create one.
 *
 * ORACLE-PINNED, so that what is being refused is not guesswork.
 * HELP RUN Process on the reference lab (VAX2, OpenVMS VAX V7.3,
 * 2026-07-31) reads, verbatim:
 *
 *   Creates a subprocess or a detached process to run an image
 *   and deletes the process when the image completes execution. A
 *   subprocess is created if any of the qualifiers except the /UIC
 *   or the /DETACHED qualifier is specified. A detached process is
 *   created if the /UIC or the /DETACHED qualifier is specified and
 *   you have the IMPERSONATE user privilege.
 *
 * So on OpenVMS, RUN/PROCESS_NAME (or /INPUT, /OUTPUT, /ERROR, or any
 * of the other twenty-odd process qualifiers) WITHOUT /DETACHED is a
 * request to create a subprocess -- a real second process, named in the
 * executive's table, that the command does not run in.
 *
 * "ANY OF THE QUALIFIERS" MEANS ANY OF *THESE* QUALIFIERS, AND THE
 * DIFFERENCE IS THE WHOLE OF vms-47b ROUND 3. That sentence lives in
 * HELP RUN *Process*, and the HELP tree has a SECOND, SEPARATE topic,
 * HELP RUN *Image*, with its own qualifier list. Read without the
 * topic, the sentence licenses refusing every qualifier RUN can be
 * written with; read in its topic, it scopes to the process qualifiers
 * and no further. The list below is the RUN (Process) "Qualifiers"
 * index captured VERBATIM from the reference lab (VAX1, OpenVMS VAX
 * V7.3, 2026-07-31, `HELP/NOPROMPT RUN Process`) and is mirrored, name
 * for name, by run_process_qualifiers[] in src/vmsdcl/dcl_cmd_process.c:
 *
 *   /ACCOUNTING /AST_LIMIT /AUTHORIZE /BUFFER_LIMIT /DELAY /DETACHED
 *   /DUMP /ENQUEUE_LIMIT /ERROR /EXTENT /FILE_LIMIT /INPUT /INTERVAL
 *   /IO_BUFFERED /IO_DIRECT /JOB_TABLE_QUOTA /MAILBOX
 *   /MAXIMUM_WORKING_SET /ON /OUTPUT /PAGE_FILE /PRIORITY /PRIVILEGES
 *   /PROCESS_NAME /QUEUE_LIMIT /RESOURCE_WAIT /SCHEDULE
 *   /SERVICE_FAILURE /SUBPROCESS_LIMIT /SWAPPING /TIME_LIMIT /TRUSTED
 *   /UIC /WORKING_SET
 *
 * RAISING THIS CONDITION FOR A QUALIFIER OUTSIDE THAT LIST IS ITSELF A
 * RULE 10 VIOLATION -- it tells the user they asked for a subprocess
 * when OpenVMS says they asked for something else entirely. Refusing
 * what VMS accepts is not the safe direction of the same mistake; it
 * is the mirror of it, and it shipped as one: RUN/NODEBUG on an image
 * ran nothing at all under the unscoped test.
 *
 * WHY THE STATE EXISTS IN OVMX: OVMX's RUN implements only the two
 * forms it can honour -- RUN <image>, which runs the image and waits
 * for it, and RUN/DETACHED, which creates a detached process through
 * $CREPRC. There is no subprocess form. A process qualifier supplied
 * without /DETACHED therefore has nothing to act on.
 *
 * WHY OPENVMS CANNOT REACH IT: on OpenVMS the subprocess form is not
 * optional -- $CREPRC creates a subprocess by default, and RUN is a
 * thin caller of it. A documented process qualifier is never
 * unimplementable there, so OpenVMS has no condition value meaning "I
 * will not create the subprocess you asked for".
 *
 * WHY THE ALTERNATIVE WAS WORSE: RUN used to accept these qualifiers
 * and silently discard them, running the image in a plain fork()ed
 * child and waiting for it. The user was told their /PROCESS_NAME was
 * accepted; nothing was named, and no second process existed to name.
 * That is CLAUDE.md Rule 10's illegal third answer -- a plausible
 * handler for a condition VMS never faces -- so the qualifier is now
 * refused instead of honoured-in-appearance.
 */
#define OVMX$_NOSUBPRC  OVMX_STATUS(STS$K_SEVERE, 2)

/*
 * OVMX$_NOPRCUIC -- the command asked for a created process to carry a
 * UIC of the caller's choosing, and OVMX cannot give it one.
 *
 * ORACLE-PINNED. HELP RUN Process /UIC on the reference lab (VAX2,
 * OpenVMS VAX V7.3, 2026-07-31) reads, verbatim:
 *
 *   Specifies that the created process be a detached process and
 *   assigns it a user identification code (UIC). Specify the UIC by
 *   using standard UIC format as described in the OpenVMS Guide to
 *   System Security.
 *
 * and HELP RUN Process /PROCESS_NAME reads:
 *
 *   Specifies a name of 1 to 15 characters for the created process.
 *   The process name is implicitly qualified by the group number of
 *   the process's user identification code (UIC). By default, the
 *   name is null.
 *
 * So /UIC is not decoration: it selects the UIC GROUP that scopes the
 * created process's name, which is the whole subject of the lab
 * transcript quoted in tests/qemu/test_kmod_procnam.c (two processes
 * in different groups may hold the same name; a third in the same
 * group is refused SS$_DUPLNAM).
 *
 * NO LONGER RAISED (vms-d31d). This condition existed because $CREPRC's
 * uic argument once reached only the created process's OWN userspace PCB
 * (vms_pcb_set_identity()), so a caller-chosen UIC changed nothing another
 * process could observe. vms-d31d closed that: $CREPRC now stamps the
 * created process's EXECUTIVE row with the requested UIC and privileges
 * (via vms_kif_setident), so RUN/DETACHED/UIC=[g,m] gives the process a UIC
 * every other process and the Files-11 reference monitor can see -- exactly
 * as OpenVMS does, subject to the creator's privilege. RUN therefore
 * HONOURS /UIC now (src/vmsdcl/dcl_cmd_process.c run_detached) and never
 * emits this condition. The value is retained (defined here, named in
 * src/libvms/status.c) so any historical reference still resolves, but no
 * code path raises it.
 */
#define OVMX$_NOPRCUIC  OVMX_STATUS(STS$K_SEVERE, 3)

/*
 * OVMX$_NODEBUGGER -- the command asked RUN to execute the image under
 * the debugger, and OVMX has no debugger for it to run under.
 *
 * ORACLE-PINNED. /DEBUG is not a process qualifier: it belongs to the
 * OTHER RUN topic. `HELP/NOPROMPT RUN Image` on the reference lab
 * (VAX1, OpenVMS VAX V7.3, 2026-07-31) reads, verbatim:
 *
 *   Executes an image within the context of your process. You can
 *   abbreviate the RUN command to a single letter, R.
 *   ...
 *   Additional information available:
 *   Parameter  Qualifier
 *   /DEBUG
 *   Examples
 *
 * and `HELP/NOPROMPT RUN Image Qualifier` lists exactly one qualifier,
 * /DEBUG (with its /NODEBUG negation) and nothing else:
 *
 *   /DEBUG
 *   /NODEBUG
 *   Executes the image under control of the debugger. The default is
 *   the /DEBUG qualifier if the image is linked with the /DEBUG
 *   qualifier and the /NODEBUG qualifier if the image is linked
 *   without the /DEBUG qualifier. ...
 *
 * So the RUN (Image) form creates NO process at all -- it runs the
 * image "within the context of your process" -- and its one qualifier
 * selects whether the debugger is in the picture. /NODEBUG is
 * therefore satisfied by OVMX exactly as VMS satisfies it, by running
 * the image; it needs no condition value and gets none.
 *
 * WHY THE STATE EXISTS IN OVMX: /DEBUG asks for a debugger, and OVMX
 * has no debugger image, no debugger shareable, and no image-header
 * bit that would say whether the image was linked for one. There is
 * nothing to run the image under.
 *
 * WHY OPENVMS CANNOT REACH IT: the debugger is part of OpenVMS. It was
 * present on the oracle -- SYS$COMMON:[SYSLIB]DEBUG.EXE, DEBUGSHR.EXE,
 * DEBUGSRVSHR.EXE and DEBUGUISHR.EXE all exist on VAX1 -- so "there is
 * no debugger on this system" is not a state OpenVMS has a condition
 * value for. The only /DEBUG failure OpenVMS documents is about the
 * IMAGE ("the /DEBUG qualifier is invalid if the image is linked with
 * the /NOTRACEBACK qualifier"), which is a different claim entirely
 * and one OVMX has not measured about any image. Borrowing it would be
 * asserting a fact about the user's image that OVMX never looked up.
 *
 * WHY NOT OVMX$_NOSUBPRC: because that message says "subprocess
 * creation is not implemented", and the RUN (Image) form does not
 * create a process. Reporting it here would tell the user OpenVMS
 * read their command as a process creation when the oracle says it
 * does not. That substitution is exactly what this constant exists to
 * stop, and it was shipped once.
 */
#define OVMX$_NODEBUGGER  OVMX_STATUS(STS$K_SEVERE, 4)

#ifdef __cplusplus
}
#endif

#endif /* __OVMX_STATUS_H */
