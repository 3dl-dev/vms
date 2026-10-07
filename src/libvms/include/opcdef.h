/*
 * opcdef.h - OPCOM (Operator Communication Manager) Definitions
 *
 * Defines the OPC$ message structure and constants used by sys$sndopr.
 *
 * Reference: OpenVMS System Services Reference Manual — SYS$SNDOPR
 *            OpenVMS Programming Concepts Manual — Operator Communication
 */

#ifndef __OPCDEF_H
#define __OPCDEF_H

#include <stdint.h>
#include <stddef.h>     /* offsetof -- OPC$K_MS_HDRLEN below */

/* ================================================================
 * OPC$ Message Type Codes (opc$b_ms_type)
 * ================================================================ */

#define OPC$_RQ_RQST    3   /* User request to operator */
#define OPC$_RQ_REPLY   4   /* Operator reply to user request */
#define OPC$_RQ_CANCEL  5   /* Cancel a pending request */
#define OPC$_RQ_ENABLE  101 /* [OVMX] enable operator terminal (not a V7.3 request type) */
#define OPC$_RQ_DISABLE 102 /* [OVMX] disable operator terminal (not a V7.3 request type) */
#define OPC$_RQ_STATUS  6   /* Request operator status */
#define OPC$_RQ_LOGFIL  103 /* [OVMX] change operator log file (not a V7.3 request type) */

/* ================================================================
 * OPC$ Operator Class Bitmasks (opc$b_ms_target / OPC$M_NM_*)
 * ================================================================ */

#define OPC$M_NM_CENTRL  0x0001  /* CENTRAL operator class */
#define OPC$M_NM_PRINT   0x0002  /* PRINTER operator class */
#define OPC$M_NM_TAPES   0x0004  /* TAPE operator class */
#define OPC$M_NM_DISKS   0x0008  /* DISK operator class */
#define OPC$M_NM_DEVICE  0x0010  /* DEVICE operator class */
#define OPC$M_NM_CARDS   0x0020  /* CARD operator class */
#define OPC$M_NM_NETWORK 0x0040  /* NETWORK operator class */
#define OPC$M_NM_CLUSTER 0x0080  /* CLUSTER operator class */
#define OPC$M_NM_SECURITY 0x0100 /* SECURITY operator class */
#define OPC$M_NM_REPLY    0x0200  /* REPLY operator class */
#define OPC$M_NM_SOFTWARE 0x0400  /* SOFTWARE class */
#define OPC$M_NM_LICENSE  0x0800  /* LICENSE class */
#define OPC$M_NM_NTWORK   OPC$M_NM_NETWORK  /* the $OPCDEF spelling */
#define OPC$M_NM_OPER1   0x1000  /* OPER1 operator class */
#define OPC$M_NM_OPER2   0x2000  /* OPER2 operator class */
#define OPC$M_NM_OPER3   0x4000  /* OPER3 operator class */
#define OPC$M_NM_OPER4   0x8000  /* OPER4 operator class */
#define OPC$M_NM_OPER5   0x10000  /* OPER5 operator class */
#define OPC$M_NM_OPER6   0x20000  /* OPER6 operator class */
#define OPC$M_NM_OPER7   0x40000  /* OPER7 operator class */
#define OPC$M_NM_OPER8   0x80000
#define OPC$M_NM_OPER9   0x100000
#define OPC$M_NM_OPER10  0x200000
#define OPC$M_NM_OPER11  0x400000
#define OPC$M_NM_OPER12  0x800000

/* ================================================================
 * OPC$ Message Buffer Structure
 *
 * This is the buffer layout for sys$sndopr msgbuf parameter.
 * The descriptor must point to one of these structures.
 * ================================================================ */

/*
 * Layout per the OPCDEF oracle offsets (rd vms-f811 follow-up; docs/oracle/*-starlet-defs/
 * OPCDEF.txt): OPC$B_MS_TYPE @0, OPC$B_MS_TARGET @1 with OPC$S_MS_TARGET_CLASSES == 3 (the
 * target operator classes are a 24-bit mask in bytes 1..3 -- the first byte is the member
 * programs assign, the rest overlays the reply status word -- which is why OPC$M_NM_OPER1..12
 * run up to bit 23; OPC$W_MS_STATUS overlays bytes 2..3 in a reply), OPC$L_MS_RQSTID @4,
 * OPC$L_MS_TEXT @8. The earlier struct had a one-byte target and an invented length word at +2
 * that nothing read, so every class above bit 7 was silently truncated.
 */
struct opcdef {
    uint8_t  opc$b_ms_type;      /* @0  Message type (OPC$_RQ_*) */
    uint8_t  opc$b_ms_target;    /* @1  Target operator classes, bits 7:0 (programs assign this byte) */
    uint16_t opc$w_ms_target_hi; /* @2  classes bits 23:8 (OVMX name; $OPCDEF overlays OPC$W_MS_STATUS here) */
    uint32_t opc$l_ms_rqstid;    /* @4  Request ID (set by sys$sndopr) */
    /* Message text follows immediately (variable length) */
    char     opc$l_ms_text[1];   /* @8  First byte of message text */
};
typedef struct opcdef OPCDEF;

/* Store / fetch the 24-bit target-class mask (OPC$M_NM_*). */
#define OPC$SET_TARGET(hdr, mask) do { \
        (hdr).opc$b_ms_target    = (uint8_t)((mask));          \
        (hdr).opc$w_ms_target_hi = (uint16_t)((mask) >> 8);    \
    } while (0)
#define OPC$GET_TARGET(hdr) \
    ((uint32_t)(hdr).opc$b_ms_target | ((uint32_t)(hdr).opc$w_ms_target_hi << 8))

#include <stddef.h>
_Static_assert(offsetof(struct opcdef, opc$b_ms_target) == 1, "OPC$B_MS_TARGET @1 per the OPCDEF oracle");
_Static_assert(offsetof(struct opcdef, opc$l_ms_rqstid) == 4, "OPC$L_MS_RQSTID @4 per the OPCDEF oracle");
_Static_assert(offsetof(struct opcdef, opc$l_ms_text) == 8, "OPC$L_MS_TEXT @8 per the OPCDEF oracle");

/*
 * THE SIZE OF THE HEADER THAT PRECEDES THE MESSAGE TEXT, DERIVED FROM THE
 * STRUCTURE RATHER THAN WRITTEN DOWN (rd vms-2d37).
 *
 * Every $SNDOPR caller has to say how long the block is, and every reader has
 * to know where the text starts. Before this existed, six sites spelled that
 * as the literal 8 -- five callers computing `8 + n` and one reader that did
 * not skip the header at all -- so the two halves could disagree without
 * anything noticing, and they did: the reader treated the block as a C string
 * and stopped at the first NUL inside the header, putting two control bytes in
 * OPERATOR.LOG where the message should have been.
 *
 * offsetof() means the callers and the reader cannot drift apart, and it means
 * a correction to the layout moves every site at once.
 *
 * The offsets of type/target/rqstid/text now match the V7.3 and V8.4 $OPCDEF
 * (static-asserted above); the OPC$_RQ_* request types OVMX uses beyond the V7.3 set
 * are labelled [OVMX] in this header.
 */
#define OPC$K_MS_HDRLEN ((uint16_t)offsetof(struct opcdef, opc$l_ms_text))

#endif /* __OPCDEF_H */
