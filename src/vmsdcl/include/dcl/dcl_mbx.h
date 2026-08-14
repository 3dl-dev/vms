/*
 * dcl_mbx.h - bind DCL's SYS$INPUT / SYS$OUTPUT to mailbox devices (vms-786)
 *
 * When DCL is a subprocess a parent drives over VMS mailboxes -- the shape
 * MMK's send_cmd_and_wait uses to keep one persistent DCL open and feed it
 * command lines (docs/design-mmk-exec-drive-ovmx.md, vms-b23) -- its
 * SYS$INPUT and SYS$OUTPUT are mailbox devices (MBAn:), not a terminal or a
 * file. VSI documents this: LIB$SPAWN accepts INPUT/OUTPUT arguments that name
 * mailboxes, and the created subprocess reads commands from SYS$INPUT and
 * writes results to SYS$OUTPUT (VSI OpenVMS RTL Library (LIB$) Manual,
 * LIB$SPAWN; VSI OpenVMS I/O User's Reference, Mailbox Driver -- a mailbox is a
 * record-oriented device read with $QIO IO$_READVBLK and written with
 * IO$_WRITEVBLK).
 *
 * This is an ADDED command source, not a replacement: it engages ONLY when the
 * SYS$INPUT / SYS$OUTPUT logical names translate to a mailbox device. Terminal
 * SYS$INPUT and @-procedure / -c file SYS$INPUT keep exactly their prior
 * fd/stdio path.
 */
#ifndef OVMX_DCL_MBX_H
#define OVMX_DCL_MBX_H

/* Return-bit set from dcl_mbx_bind_std_streams(). */
#define DCL_MBX_BOUND_INPUT   0x1
#define DCL_MBX_BOUND_OUTPUT  0x2

/*
 * dcl_mbx_bind_std_streams - if SYS$INPUT and/or SYS$OUTPUT resolve to a
 * mailbox device, bind DCL's stdin/stdout to that mailbox's real executive
 * $QIO path.
 *
 * INPUT: a reader thread blocks in sys$qiow(IO$_READVBLK) on the input mailbox
 *   and feeds each message (as a newline-terminated command record) into a pipe
 *   dup'd onto STDIN_FILENO -- so DCL's existing command-read loop reads
 *   commands, verbatim, that arrived over the mailbox.
 * OUTPUT: a writer thread drains a pipe dup'd onto STDOUT_FILENO and emits each
 *   chunk with sys$qiow(IO$_WRITEVBLK) to the output mailbox -- so every byte
 *   DCL writes to SYS$OUTPUT (WRITE, SHOW, image output) is delivered to the
 *   parent over the mailbox.
 *
 * Honest failure (Rule 9 / INV-6): reaching the mailbox goes through the
 * executive ($ASSIGN -> vms_kif_mbx_assign, $QIO -> vms_kif_mbx_read/write);
 * with no /dev/vms the $ASSIGN fails and this function simply binds nothing and
 * returns 0, leaving DCL on its ordinary stdin/stdout. It never fabricates a
 * per-process substitute for a mailbox.
 *
 * Returns a bitmask of DCL_MBX_BOUND_INPUT | DCL_MBX_BOUND_OUTPUT (0 if
 * neither stream resolved to a mailbox).
 */
int dcl_mbx_bind_std_streams(void);

/*
 * dcl_mbx_shutdown - flush and tear down any mailbox binding established by
 * dcl_mbx_bind_std_streams(). Drains DCL's remaining SYS$OUTPUT to the output
 * mailbox (so the parent's final read is not lost) before returning. Safe to
 * call unconditionally; a no-op if nothing was bound.
 */
void dcl_mbx_shutdown(void);

/*
 * dcl_mbx_output_is_mailbox - non-zero iff SYS$OUTPUT was bound to a mailbox by
 * dcl_mbx_bind_std_streams() (the async writer thread is running). DCL's
 * interactive prompt loop uses this to detect the diverted-output case -- a live
 * console login whose SYS$INPUT is still the terminal but whose SYS$OUTPUT leaves
 * over a mailbox -- and apply the synchronous-prompt discipline (vms-195).
 */
int dcl_mbx_output_is_mailbox(void);

/*
 * dcl_mbx_output_drain_sync - block until the writer thread has pushed every
 * byte currently in DCL's output pipe out through the SYS$OUTPUT mailbox. Call
 * it AFTER writing and fflush()'ing the prompt: it guarantees the newline-less
 * prompt is fully emitted over the mailbox before DCL issues the read that arms
 * the terminal's synchronous keystroke echo, closing the prompt/echo race that
 * produced the interleaved "d$ ir" on the interactive console (vms-195). A no-op
 * when SYS$OUTPUT is not mailbox-bound, so the ordinary terminal/file path is
 * unchanged.
 */
void dcl_mbx_output_drain_sync(void);

#endif /* OVMX_DCL_MBX_H */
