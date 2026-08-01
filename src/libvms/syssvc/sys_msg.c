/*
 * sys_msg.c - Message System Services (SYS$GETMSG, SYS$PUTMSG)
 *
 * Implements VMS message retrieval and display services. On real VMS,
 * messages are stored in .MSG files compiled into message sections.
 * In OVMX, we use the known_codes table from status.c.
 *
 * SYS$GETMSG retrieves the text for a condition value.
 * SYS$PUTMSG formats and outputs one or more messages.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-USERSPACE: sys$getmsg (vms-5b4) -- looks the condition value up in the
 *     compiled-in known_codes[] table in src/libvms/status.c via
 *     vms$status_message(); there is no message-file section to map, so a code
 *     absent from that array yields "unknown status code" rather than a
 *     lookup failure the caller can act on.
 * OVMX-USERSPACE: sys$putmsg (vms-5b4) -- formats from the same compiled-in
 *     table and writes to the caller's own output; facnam is discarded.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "starlet.h"
#include "msgdef.h"

/* Imported from status.c */
extern int vms_status_string(uint32_t status, char *buf, size_t bufsize);
extern const char *vms$status_message(uint32_t code);
extern const char *vms$status_ident(uint32_t code);

/* Severity letter lookup */
static char severity_char(uint32_t sev) {
    switch (sev & 7) {
        case 0: return 'W';
        case 1: return 'S';
        case 2: return 'E';
        case 3: return 'I';
        case 4: return 'F';
        default: return '?';
    }
}

/* Get facility name from condition value */
static const char *facility_name(uint32_t msgid) {
    /* A customer-defined condition value is not a VMS one (bit 27,
     * STS$V_CUST_DEF). In OVMX the only such values are OVMX's own, and
     * naming them "NONAME" here made an OVMX condition print in the
     * shape of a VMS system message. See src/libvms/include/ovmx_status.h. */
    if ($VMS_STATUS_CUST_DEF(msgid))
        return "OVMX";

    uint32_t fac = $VMS_STATUS_FAC_NO(msgid);
    switch (fac) {
        case 0:  return "SYSTEM";
        case 1:  return "RMS";
        case 3:  return "CLI";
        case 8:  return "LIB";
        case 9:  return "MTH";
        case 10: return "OTS";
        default: return "NONAME";
    }
}

/*
 * sys$getmsg - Get message text for a condition value.
 *
 * Retrieves the formatted message for msgid and writes it into
 * the bufadr descriptor. The flags parameter controls which
 * components are included:
 *   MSG$M_TEXT     (0x01) - message text
 *   MSG$M_IDENT   (0x02) - message ident (FACILITY-S-IDENT)
 *   MSG$M_SEVERITY(0x04) - severity prefix
 *   MSG$M_FACILITY(0x08) - facility name prefix
 *
 * If flags is 0x0F (all), produces: %FACILITY-S-IDENT, text
 * If flags is 0x01 (text only), produces: text
 *
 * Parameters:
 *   msgid  - Condition value
 *   msglen - Receives output length
 *   bufadr - Output buffer descriptor
 *   flags  - Component flags (MSG$M_ bits, 0x0F = all)
 *   outadr - Optional result vector (unused, may be NULL)
 */
uint32_t sys$getmsg(uint32_t msgid, uint16_t *msglen,
                    struct dsc$descriptor_s *bufadr,
                    uint32_t flags, uint32_t *outadr) {
    (void)outadr;

    if (!bufadr || !bufadr->dsc$a_pointer) return SS$_BADPARAM;

    /* Default flags: include everything */
    if (flags == 0) flags = MSG$M_ALL;

    const char *text = vms$status_message(msgid);
    const char *ident = vms$status_ident(msgid);
    const char *fac = facility_name(msgid);
    uint32_t sev = $VMS_STATUS_SEVERITY(msgid);
    char sev_c = severity_char(sev);

    char buf[512];
    int pos = 0;

    /* Build the message according to flags */
    if ((flags & MSG$M_FACILITY) && (flags & MSG$M_SEVERITY) && (flags & MSG$M_IDENT)) {
        /* Full prefix: %FACILITY-S-IDENT, */
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                        "%%%s-%c-%s", fac, sev_c, ident);
        if (flags & MSG$M_TEXT) {
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ", %s", text);
        }
    } else {
        /* Partial prefix combinations */
        if (flags & MSG$M_FACILITY) {
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%%%s", fac);
        }
        if (flags & MSG$M_SEVERITY) {
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                            "%s%c", pos > 0 ? "-" : "%", sev_c);
        }
        if (flags & MSG$M_IDENT) {
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                            "%s%s", pos > 0 ? "-" : "", ident);
        }
        if (flags & MSG$M_TEXT) {
            if (pos > 0) {
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ", %s", text);
            } else {
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s", text);
            }
        }
    }

    /* Copy to output descriptor */
    uint16_t outlen = (uint16_t)pos;
    uint32_t status = SS$_NORMAL;

    if (outlen > bufadr->dsc$w_length) {
        outlen = bufadr->dsc$w_length;
        status = SS$_BUFFEROVF;
    }
    memcpy(bufadr->dsc$a_pointer, buf, outlen);

    /* Pad with spaces if CLASS_S */
    if (bufadr->dsc$b_class == DSC$K_CLASS_S && outlen < bufadr->dsc$w_length) {
        memset(bufadr->dsc$a_pointer + outlen, ' ',
               bufadr->dsc$w_length - outlen);
    }

    if (msglen) *msglen = outlen;
    return status;
}

/*
 * sys$putmsg - Output formatted message(s).
 *
 * Formats and outputs messages described by the message vector.
 * The message vector layout:
 *   msgvec[0] = argument count (number of longwords following)
 *   msgvec[1] = message code (condition value)
 *   msgvec[2] = FAO argument count (currently unused)
 *   msgvec[3..N] = FAO arguments (currently unused)
 *
 * If actrtn is provided, it is called with each formatted line
 * and actprm. If actrtn returns non-zero, the message is suppressed.
 *
 * Parameters:
 *   msgvec - Message vector
 *   actrtn - Optional action routine
 *   facnam - Optional facility name override
 *   actprm - Action routine parameter
 */
uint32_t sys$putmsg(const uint32_t *msgvec,
                    uint32_t (*actrtn)(struct dsc$descriptor_s *, uint32_t),
                    const struct dsc$descriptor_s *facnam,
                    uint32_t actprm) {
    (void)facnam;  /* TODO: facility name override */

    if (!msgvec) return SS$_BADPARAM;

    uint32_t arg_count = msgvec[0];
    if (arg_count < 1) return SS$_BADPARAM;

    uint32_t msgid = msgvec[1];

    /* Format the message */
    char buf[512];
    vms_status_string(msgid, buf, sizeof(buf));

    if (actrtn) {
        /* Call action routine with formatted message */
        struct dsc$descriptor_s msg_dsc = {
            .dsc$w_length = (uint16_t)strlen(buf),
            .dsc$b_dtype = DSC$K_DTYPE_T,
            .dsc$b_class = DSC$K_CLASS_S,
            .dsc$a_pointer = buf
        };
        uint32_t action_result = actrtn(&msg_dsc, actprm);
        if (action_result != 0) return SS$_NORMAL;  /* Suppressed */
    }

    /* Output to stderr (VMS SYS$ERROR equivalent) */
    fprintf(stderr, "%s\n", buf);

    return SS$_NORMAL;
}
