/*
 * MSGDEF.H - VMS Message Definition Constants
 *
 * OpenVMX compatibility layer - Defines constants used with
 * SYS$GETMSG and SYS$PUTMSG for retrieving and displaying
 * system messages.
 *
 * The MSG$_ constants control which parts of a message are
 * returned by SYS$GETMSG.
 *
 * Reference: OpenVMS System Services Reference Manual (SYS$GETMSG)
 */

#ifndef __MSGDEF_H
#define __MSGDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Message flags for SYS$GETMSG (flags argument)
 *
 * These control which components of the message are returned.
 * By default (flags=0x0F), all components are included.
 * Setting a bit to 0 SUPPRESSES that component.
 * ================================================================ */

#define MSG$M_TEXT          0x01    /* Bit 0: Include message text */
#define MSG$M_IDENT         0x02    /* Bit 1: Include message ident (facility + severity) */
#define MSG$M_SEVERITY      0x04    /* Bit 2: Include severity text (%-W-, %-E-, etc.) */
#define MSG$M_FACILITY      0x08    /* Bit 3: Include facility name prefix */

/* Convenience: all components */
#define MSG$M_ALL           (MSG$M_TEXT | MSG$M_IDENT | MSG$M_SEVERITY | MSG$M_FACILITY)

/* ================================================================
 * Message severity prefix strings
 *
 * Used in formatted messages: %FACILITY-S-IDENT, text
 * ================================================================ */

#define MSG$K_SEV_WARNING   'W'     /* Warning */
#define MSG$K_SEV_SUCCESS   'S'     /* Success */
#define MSG$K_SEV_ERROR     'E'     /* Error */
#define MSG$K_SEV_INFO      'I'     /* Informational */
#define MSG$K_SEV_SEVERE    'F'     /* Fatal/Severe */

/* ================================================================
 * Message argument descriptor for SYS$PUTMSG
 *
 * The msgvec (message vector) passed to SYS$PUTMSG has the format:
 *   msgvec[0] = argument count (number of longwords following)
 *   msgvec[1] = primary message code
 *   msgvec[2] = FAO argument count for primary message
 *   msgvec[3..N] = FAO arguments
 *   Optional: additional message codes with their FAO args
 * ================================================================ */

struct msg$vector {
    uint16_t    msg$w_msg_count;    /* Number of message entries */
    uint16_t    msg$w_reserved;     /* Reserved (MBZ) */
    uint32_t    msg$l_msg_code;     /* First message code */
    uint16_t    msg$w_fao_count;    /* FAO argument count for this message */
    uint16_t    msg$w_msg_flags;    /* Per-message flags */
    /* FAO arguments follow as uint32_t values */
};

/* Per-message flags in msg$w_msg_flags */
#define MSG$V_NO_TEXT       0x01    /* Suppress text for this message */
#define MSG$V_NO_IDENT      0x02    /* Suppress ident for this message */
#define MSG$V_NO_SEV        0x04    /* Suppress severity for this message */
#define MSG$V_NO_FAC        0x08    /* Suppress facility for this message */

/* ================================================================
 * SYS$PUTMSG action routine return values
 * ================================================================ */

#define MSG$K_CONTINUE      0       /* Continue processing messages */
#define MSG$K_SUPPRESS      1       /* Suppress this message */

/* ================================================================
 * Facility number extraction from condition value
 *
 * The facility number is in bits 16-27 of the condition value.
 * The facility-specific bit is bit 15.
 * ================================================================ */

#define MSG$_FAC_SYSTEM     0       /* System facility (SS$_) */
#define MSG$_FAC_RMS        1       /* RMS facility */
#define MSG$_FAC_LIB        8       /* LIB$ facility */
#define MSG$_FAC_STR        8       /* STR$ (shares LIB$ facility) */
#define MSG$_FAC_MTH        9       /* MTH$ facility */
#define MSG$_FAC_OTS        10      /* OTS$ facility */

#ifdef __cplusplus
}
#endif

#endif /* __MSGDEF_H */
