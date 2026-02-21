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

/* ================================================================
 * OPC$ Message Type Codes (opc$b_ms_type)
 * ================================================================ */

#define OPC$_RQ_RQST    1   /* User request to operator */
#define OPC$_RQ_REPLY   2   /* Operator reply to user request */
#define OPC$_RQ_CANCEL  3   /* Cancel a pending request */
#define OPC$_RQ_ENABLE  4   /* Enable operator terminal */
#define OPC$_RQ_DISABLE 5   /* Disable operator terminal */
#define OPC$_RQ_STATUS  6   /* Request operator status */
#define OPC$_RQ_LOGFIL  7   /* Change operator log file */

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
#define OPC$M_NM_OPER1   0x0200  /* OPER1 operator class */
#define OPC$M_NM_OPER2   0x0400  /* OPER2 operator class */
#define OPC$M_NM_OPER3   0x0800  /* OPER3 operator class */
#define OPC$M_NM_OPER4   0x1000  /* OPER4 operator class */
#define OPC$M_NM_OPER5   0x2000  /* OPER5 operator class */
#define OPC$M_NM_OPER6   0x4000  /* OPER6 operator class */
#define OPC$M_NM_OPER7   0x8000  /* OPER7 operator class */

/* ================================================================
 * OPC$ Message Buffer Structure
 *
 * This is the buffer layout for sys$sndopr msgbuf parameter.
 * The descriptor must point to one of these structures.
 * ================================================================ */

struct opcdef {
    uint8_t  opc$b_ms_type;    /* Message type (OPC$_RQ_*) */
    uint8_t  opc$b_ms_target;  /* Target operator class bitmask */
    uint16_t opc$w_ms_rqstlen; /* Total message length (set by caller) */
    uint32_t opc$l_ms_rqstid;  /* Request ID (set by sys$sndopr) */
    /* Message text follows immediately (variable length) */
    char     opc$l_ms_text[1]; /* First byte of message text */
};

#endif /* __OPCDEF_H */
