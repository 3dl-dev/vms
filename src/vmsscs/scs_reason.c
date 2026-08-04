/*
 * scs_reason.c - vms-6b3: the 16-bit REJECT/DISCONNECT reason code (p. 2-26).
 *
 * See scs_reason.h for the page cites, the measurement that placed the field,
 * the explicit statement that the offset and the code values are LABELED OVMX
 * design choices, and which half of this module has a production caller.
 *
 * This file allocates nothing, opens nothing and knows about exactly two bytes.
 */
#include "scs_reason.h"

#include <stdlib.h>

const char *scs_reason_name(uint16_t reason)
{
    switch (reason) {
    case SCS_REASON_NONE:            return "NONE";
    case SCS_REASON_NO_SUCH_SYSAP:   return "NO_SUCH_SYSAP";
    case SCS_REASON_NOT_LISTENING:   return "NOT_LISTENING";
    case SCS_REASON_NO_RESOURCES:    return "NO_RESOURCES";
    case SCS_REASON_CONNECT_DATA:    return "CONNECT_DATA";
    case SCS_REASON_SYSAP_SHUTDOWN:  return "SYSAP_SHUTDOWN";
    case SCS_REASON_VC_LOST:         return "VC_LOST";
    case SCS_REASON_PEER_DISCONNECT: return "PEER_DISCONNECT";
    default:                         break;
    }
    /* A code we did not define. It may be a real VMS value; we have never seen
     * one and will not invent a meaning for it. */
    return "UNKNOWN";
}

int scs_reason_enabled(void)
{
    /* Read fresh every call (see scs_reason.h), for the same reason
     * scs_conn_fsm_enabled() does: a cached answer could not be bracketed by a
     * test, and guardrail 23 requires the switch actually be RUN. */
    const char *v = getenv("OVMX_NO_REASON_CODE");
    if (v == NULL || v[0] == '\0') {
        return 1;
    }
    if (v[0] == '0' && v[1] == '\0') {
        return 1;
    }
    return 0;
}

int scs_reason_carried_by(unsigned msgtype)
{
    return msgtype == SCS_REASON_MSGTYPE_REJECT_REQ ||
           msgtype == SCS_REASON_MSGTYPE_DISCONNECT_REQ;
}

int scs_reason_put(uint8_t *frame, size_t len, unsigned msgtype, uint16_t reason)
{
    if (frame == NULL || !scs_reason_carried_by(msgtype)) {
        return -1;
    }
    if (len < (size_t)SCS_REASON_FRAME_OFF + 2u) {
        return -1;
    }
    if (!scs_reason_enabled()) {
        return 0; /* deliberately touches no byte */
    }
    frame[SCS_REASON_FRAME_OFF]     = (uint8_t)(reason & 0xffu);
    frame[SCS_REASON_FRAME_OFF + 1] = (uint8_t)((reason >> 8) & 0xffu);
    return 1;
}

int scs_reason_get(const uint8_t *frame, size_t len, unsigned msgtype, uint16_t *out)
{
    if (frame == NULL || out == NULL || !scs_reason_carried_by(msgtype)) {
        return -1;
    }
    if (len < (size_t)SCS_REASON_FRAME_OFF + 2u) {
        return -1;
    }
    if (!scs_reason_enabled()) {
        return 0;
    }
    *out = (uint16_t)((uint16_t)frame[SCS_REASON_FRAME_OFF] |
                      ((uint16_t)frame[SCS_REASON_FRAME_OFF + 1] << 8));
    return 1;
}
