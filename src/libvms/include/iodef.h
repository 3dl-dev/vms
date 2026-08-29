/*
 * IODEF.H - VMS I/O Function Code Definitions
 *
 * OpenVMX compatibility layer - Defines the IO$_ function codes
 * used with SYS$QIO and SYS$QIOW system services.
 *
 * I/O function codes are 16-bit values:
 *   Bits 0-5:  Function code (IO$_xxx)
 *   Bits 6-15: Function modifier bits (IO$M_xxx)
 *
 * The function code identifies the operation (read, write, etc.)
 * and the modifier bits alter its behavior (no echo, purge, etc.).
 *
 * Reference: OpenVMS I/O User's Reference Manual
 *            OpenVMS System Services Reference Manual (SYS$QIO)
 */

#ifndef __IODEF_H
#define __IODEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Generic I/O function codes (bits 0-5)
 * ================================================================ */

#define IO$_NOP             0   /* No operation */
#define IO$_UNLOAD          1   /* Unload volume */
#define IO$_LOADMCODE       2   /* Load microcode */
#define IO$_DELETE          3   /* Delete file (mark for deletion) */
#define IO$_ACCESS          4   /* Access file (open) */
#define IO$_DEACCESS        5   /* Deaccess file (close) */
#define IO$_MODIFY          6   /* Modify file attributes */
#define IO$_ACPCONTROL      7   /* ACP control function */
#define IO$_MOUNT           8   /* Mount volume */
#define IO$_CREATE          9   /* Create file */
#define IO$_NETCONTROL      10  /* Network control function */
#define IO$_RDSTATS         12  /* Read statistics */
#define IO$_DIAGNOSE        14  /* Diagnose */

/* Sense/Set mode and characteristics */
#define IO$_SETCHAR         26  /* Set device characteristics */
#define IO$_SENSECHAR       27  /* Sense device characteristics */
#define IO$_SETMODE         35  /* Set device mode */
#define IO$_REWIND          36  /* Rewind */
#define IO$_SKIPFILE        37  /* Skip file */
#define IO$_SKIPRECORD      38  /* Skip record */
#define IO$_SENSEMODE       39  /* Sense device mode */
#define IO$_WRITEOF         40  /* Write end-of-file */
#define IO$_SPACEFILE       41  /* Space file */
#define IO$_FORMAT          44  /* Format */
#define IO$_SETPRFPATH      46  /* Set preferred path */
#define IO$_DISPLAY         47  /* Display */

/* Physical, logical, and virtual block I/O */
#define IO$_WRITEVBLK       48  /* Write virtual block */
#define IO$_READVBLK        49  /* Read virtual block */
#define IO$_READLBLK        50  /* Read logical block */
#define IO$_WRITELBLK       51  /* Write logical block */
#define IO$_READPBLK        52  /* Read physical block */
#define IO$_WRITEPBLK       53  /* Write physical block */

/* Terminal-specific I/O functions */
#define IO$_READPROMPT      55  /* Read with prompt (terminal) */
#define IO$_TTYREADALL      58  /* Read all characters (terminal) */
#define IO$_TTYREADPALL     59  /* Read passall (terminal) */
#define IO$_FLUSH           60  /* Flush buffers */

/* ================================================================
 * I/O function modifier bits (bits 6-15)
 * ================================================================ */

/* Terminal modifiers */
#define IO$M_NOECHO         0x0040  /* Bit 6: suppress echo */
#define IO$M_TRMNOECHO      0x0040  /* Bit 6: terminal no echo (alias) */
#define IO$M_PURGE          0x0080  /* Bit 7: purge type-ahead buffer */
#define IO$M_NOFILTR        0x0100  /* Bit 8: no filter (pass all characters) */
#define IO$M_TIMED          0x0200  /* Bit 9: timed I/O */
#define IO$M_CVTLOW         0x0400  /* Bit 10: convert lowercase to uppercase */
#define IO$M_ESCAPE         0x0800  /* Bit 11: escape processing enabled */
#define IO$M_EXTEND         0x1000  /* Bit 12: extended read */

/* File system modifiers */
#define IO$M_ACCESS         0x0040  /* Bit 6: access mode */
#define IO$M_CREATE         0x0080  /* Bit 7: create if nonexistent */
#define IO$M_FCODE          0x003F  /* Bits 0-5: function code mask */
#define IO$M_FMODIFIERS     0xFFC0  /* Bits 6-15: modifier mask */

/* Disk modifiers */
#define IO$M_INHRETRY       0x2000  /* Bit 13: inhibit retry */
#define IO$M_DATACHECK      0x4000  /* Bit 14: data check (verify) */
#define IO$M_SYNCSTS        0x8000  /* Bit 15: synchronous status */

/*
 * BGn: (INET pseudo-device) IO$_SETMODE socket-kind selector, passed in P2 when
 * IO$_SETMODE creates the channel's socket (vms-80b). 0 (the default, and the
 * value every existing caller already passes) selects the AF_INET/SOCK_STREAM
 * TCP client socket; IO$K_SOCK_ICMP selects a raw ICMP socket for PING. The
 * executive maps the selector to the concrete host socket the seam mints; the
 * socket is thereafter driven by the ordinary IO$_ACCESS/WRITEVBLK/READVBLK.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): OVMX DESIGN CHOICE, stated rather than implied.
 * On VMS the socket type/protocol is a create-time socket CHARACTERISTIC; the
 * public docs do not publish the byte-level IO$_SETMODE parameter layout, so
 * OVMX defines its own selector (a P2 integer) and LABELS it as OVMX's, never
 * presented as a VMS-authentic value -- the same posture vms_bg.h takes for its
 * OVMX-defined transfer cap.
 */
#define IO$K_SOCK_STREAM    0   /* default: AF_INET/SOCK_STREAM TCP client */
#define IO$K_SOCK_ICMP      1   /* AF_INET/SOCK_RAW/IPPROTO_ICMP (PING) */

/* Network modifiers */
#define IO$M_NOW            0x0040  /* Bit 6: immediate (no wait) */
#define IO$M_INTERRUPT      0x0080  /* Bit 7: interrupt message */
#define IO$M_MULTIPLE       0x0100  /* Bit 8: multiple */
#define IO$M_ACCEPT         0x0200  /* Bit 9: accept */
#define IO$M_ABORT          0x0400  /* Bit 10: abort */

/*
 * Mailbox modifiers (IO$_SETMODE / IO$_SETCHAR).
 *
 * IO$M_WRTATTN sets a WRITE-ATTENTION AST on a mailbox channel: when another
 * process writes a message to the mailbox, the process that set the AST is
 * notified. IO$M_READATTN is the read-attention counterpart (notify when the
 * mailbox is read). Both are ONE-SHOT: the AST is delivered once and must be
 * re-established by another IO$_SETMODE|IO$M_WRTATTN to be notified again.
 * P1 carries the AST routine address, P2 the AST parameter.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): these bit values and the one-shot,
 * re-arm-on-SETMODE semantics are from the public VSI OpenVMS I/O User's
 * Reference Manual (mailbox driver, "Setting Mailbox Attention ASTs") and the
 * public $IODEF ($EQU IO$M_WRTATTN ^X100, IO$M_READATTN ^X200) -- never from
 * disassembly. The same two values are used by the tier-4 VMS example source
 * carried under tests/corpus/ (sp_mgr.b32) and the MMK compat shim
 * (tests/corpus/tier3-mmk/ovmx/ovmx_mmk_compat.h).
 */
#define IO$M_WRTATTN        0x0100  /* Bit 8: enable write-attention AST */
#define IO$M_READATTN       0x0200  /* Bit 9: enable read-attention AST */

/* Miscellaneous modifiers */
#define IO$M_INHSEEK        0x0200  /* Bit 9: inhibit seek */
#define IO$M_ERASE          0x0100  /* Bit 8: erase */
#define IO$M_ALLOWFAST      0x8000  /* Bit 15: allow fast path */
#define IO$M_DELETE         0x0100  /* Bit 8: delete on close */
#define IO$M_DMOUNT         0x0200  /* Bit 9: dismount */

/* ================================================================
 * I/O Status Block (IOSB) structures
 *
 * Every QIO/QIOW operation returns status in an IOSB.
 * The first word is the completion status; the remaining
 * words are device-dependent.
 *
 * These definitions are guarded by _IOSB_STRUCT_DEFINED so that
 * iosbdef.h (which provides the extended union form with the
 * iosb$l_getxxi_status field) can be included first without
 * causing redefinition errors.
 * ================================================================ */

#ifndef _IOSB_STRUCT_DEFINED
#define _IOSB_STRUCT_DEFINED

struct _iosb {
    uint16_t  iosb$w_status;      /* Completion status */
    uint16_t  iosb$w_bcnt;        /* Byte/transfer count */
    uint32_t  iosb$l_dev_depend;  /* Device-dependent information */
};

typedef struct _iosb IOSB;

/* Terminal IOSB (more specific layout) */
struct _tt_iosb {
    uint16_t  iosb$w_status;      /* Completion status */
    uint16_t  iosb$w_bcnt;        /* Byte count transferred */
    uint16_t  iosb$w_terminator;  /* Terminator character */
    uint16_t  iosb$w_termsize;    /* Terminator size */
};

typedef struct _tt_iosb TT_IOSB;

/* Disk IOSB */
struct _dk_iosb {
    uint16_t  iosb$w_status;      /* Completion status */
    uint16_t  iosb$w_bcnt;        /* Byte count transferred */
    uint32_t  iosb$l_dev_depend;  /* Device-dependent longword */
};

typedef struct _dk_iosb DK_IOSB;

#endif /* _IOSB_STRUCT_DEFINED */

/* ================================================================
 * Convenience macros
 * ================================================================ */

/* Extract function code (strip modifier bits) */
#define IO$FUNC(code)      ((code) & IO$M_FCODE)

/* Extract modifier bits (strip function code) */
#define IO$MODS(code)      ((code) & IO$M_FMODIFIERS)

/* Combine function code and modifiers */
#define IO$COMBINE(func, mods) ((func) | (mods))

#ifdef __cplusplus
}
#endif

#endif /* __IODEF_H */
