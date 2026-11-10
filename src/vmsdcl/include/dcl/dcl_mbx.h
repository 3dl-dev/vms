/*
 * dcl_mbx.h - bind DCL's SYS$OUTPUT to a mailbox device (vms-786). A
 * SYS$INPUT mailbox or file is read by dcl_in.c (rd vms-4d27).
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

#include <stddef.h>

/* Return-bit set from dcl_mbx_bind_std_streams(). */
#define DCL_MBX_BOUND_OUTPUT  0x2

/*
 * dcl_mbx_bind_std_streams - if SYS$OUTPUT resolves to a mailbox device,
 * bind DCL's output to that mailbox's real executive $QIO path.
 *
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
 * Returns DCL_MBX_BOUND_OUTPUT, or 0 if SYS$OUTPUT is not a mailbox.
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
 * dcl_mbx_output_drain_sync - relay every byte now in the image-output pipe to
 * the SYS$OUTPUT mailbox before returning. Called before DCL arms a terminal
 * read, so nothing written before the read can reach the console after its
 * echo -- the interleaved "d$ ir" of vms-195. Never blocks on an empty pipe; a
 * no-op when SYS$OUTPUT is not mailbox-bound.
 */
void dcl_mbx_output_drain_sync(void);

/*
 * dcl_mbx_put_record - one record of DCL's own SYS$OUTPUT to the bound
 * mailbox: $QIO IO$_WRITEVBLK on DCL's channel, after everything an image
 * wrote to the relay before it (rd vms-1b6a). 0 when the mailbox took it; -1
 * when SYS$OUTPUT is not a bound mailbox or the write failed.
 */
int dcl_mbx_put_record(const char *rec, size_t n);

#endif /* OVMX_DCL_MBX_H */
