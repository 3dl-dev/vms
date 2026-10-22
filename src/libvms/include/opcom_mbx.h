/*
 * opcom_mbx.h - what $SNDOPR puts in OPCOM's mailbox (rd vms-798).
 *
 * On VMS, $SNDOPR does not write the operator log itself: it sends the
 * request to the OPCOM process, which numbers it, writes OPERATOR.LOG and
 * broadcasts it to every operator terminal enabled for its classes. OVMX does
 * the same: OPCOM (SYS$SYSTEM:OPCOM.EXE, src/ovmx_opcom) creates a permanent
 * mailbox and names it OPCOM_MBX_LOGNAM in LNM$PERMANENT_MAILBOX; sys$sndopr
 * (src/libvms/syssvc/sys_operator.c) writes one of these messages to it.
 *
 * WHO SENT IT is NOT in the message: OPCOM takes the sender's PID from the
 * mailbox read (the executive records the writer of every message) and asks
 * the executive for that process's user name. Nothing a requester can put in
 * the message decides the "from user" line.
 *
 * The VMS layout of this internal message is not public; this is OVMX's own
 * (clean-room, Rule 8): a small fixed header, then the caller's OPC block
 * exactly as it gave it to $SNDOPR. Header-only: it adds no symbol to any
 * shareable's vector.
 */
#ifndef OVMX_OPCOM_MBX_H
#define OVMX_OPCOM_MBX_H

#include <stdint.h>

#define OPCOM_MBX_LOGNAM   "OPCOM$MAILBOX"
#define OPCOM_MBX_MAGIC    0x314D504Fu      /* "OPM1" */
#define OPCOM_MBX_MAXBLK   1024u            /* largest OPC block carried */

struct opcom_mbx_msg {
    uint32_t magic;          /* OPCOM_MBX_MAGIC                              */
    uint32_t flags;          /* OPCOM_MBX_F_*                                */
    uint32_t blklen;         /* bytes of OPC block that follow (<= MAXBLK)   */
    uint32_t reserved;
    /* followed by blklen bytes: the caller's OPC block ($SNDOPR msgbuf) */
};

/* The requester supplied a reply channel: a numbered REQUEST, not a message. */
#define OPCOM_MBX_F_REPLY  0x1u

#define OPCOM_MBX_MAXMSG   ((uint32_t)sizeof(struct opcom_mbx_msg) + OPCOM_MBX_MAXBLK)

#endif /* OVMX_OPCOM_MBX_H */
