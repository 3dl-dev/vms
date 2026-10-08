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
 * OVMX-USERSPACE: sys$getmsg (vms-546) -- answers from the message catalog
 *     observed on OpenVMS Alpha V8.4 (docs/oracle/messages/, the generated
 *     src/libvms/include/ovmx_msgcat.inc): every SYSTEM message and the
 *     facility-specific messages of the facilities captured. A real VMS maps
 *     message sections; this is the same catalog compiled in, so a message of
 *     a facility not captured is SS$_MSGNOTFND (NOMSG), never invented text.
 * OVMX-USERSPACE: sys$putmsg (vms-916) -- formats from the same compiled-in
 *     table and writes to the caller's own output; the facnam facility-name
 *     override is honored by rewriting the %FACILITY token of the formatted
 *     message (vms-7a2). (OVMX messages carry no FAO directives, so the FAO
 *     argument list has nothing to substitute -- honest, not a facade.)
 *
 * vms-916 is the message facility's item -- "real idents, no invented ones",
 * whose done-condition is an audit of every emittable ident against the oracle.
 * known_codes[] is the catalog these two services read, so that audit is what
 * decides their answer. Its file list names src/vmsdcl/dcl_messages.c and not
 * src/libvms/status.c; whoever works it should widen the list rather than treat
 * these two as covered by it today.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
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

/* ---- the message catalog (rd vms-546) ------------------------------------
 * GENERATED from the catalog observed on OpenVMS Alpha V8.4
 * (tools/oracle/messages/gen_msgcat.py, docs/oracle/messages/): every SYSTEM
 * message, every facility-specific message of the facilities captured, and the
 * facility names. */
struct ovmx_msgdef { uint32_t code; uint8_t faocnt; const char *ident; const char *text; };
struct ovmx_msgfac { uint16_t fac; const char *name; };
#include "ovmx_msgcat.inc"

static const struct ovmx_msgdef *msgcat_find(uint32_t code)
{
    size_t lo = 0, hi = sizeof ovmx_msgcat / sizeof ovmx_msgcat[0];
    code &= ~7u;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ovmx_msgcat[mid].code == code) return &ovmx_msgcat[mid];
        if (ovmx_msgcat[mid].code < code) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static const char *msgcat_facility(uint32_t fac)
{
    for (size_t i = 0; i < sizeof ovmx_msgfacs / sizeof ovmx_msgfacs[0]; i++)
        if (ovmx_msgfacs[i].fac == fac)
            return ovmx_msgfacs[i].name;
    return NULL;
}

/*
 * sys$getmsg - the message text for a condition value, as OpenVMS's $GETMSG
 * returns it (observed on VAX V7.3 and Alpha V8.4, docs/oracle/semantics/fao/):
 *
 *   - a facility-specific message number (bit 12 of the message number) is the
 *     facility's own message; any other number is the SHARED message of that
 *     number, which every facility uses (the RMS facility's ACCVIO is
 *     "%RMS-F-ACCVIO, access violation, ..."), named with the condition's
 *     facility -- "NONAME" when the facility is not one this system knows;
 *   - the severity letter is the CONDITION's (W S E I F, '?' for 5-7), not the
 *     message's: SS$_WASSET's message number is ACCVIO's;
 *   - the text is returned unformatted, FAO directives and all, and outadr
 *     receives { 0, the message's FAO argument count, 0, 0 };
 *   - a condition with no message (0, or an unknown facility-specific number)
 *     is "%NONAME-<sev>-NOMSG, Message number <hex>" with SS$_MSGNOTFND;
 *   - flags pick the parts: text 1, ident 2, severity 4, facility 8 (0 = all);
 *     the prefix parts are joined by '-' after one '%', and the text follows
 *     ", " when there is a prefix;
 *   - a buffer too small for the message is filled and SS$_BUFFEROVF returned.
 */
uint32_t sys$getmsg(uint32_t msgid, uint16_t *msglen,
                    struct dsc$descriptor_s *bufadr,
                    uint32_t flags, uint32_t *outadr_l) {
    uint8_t *outadr = (uint8_t *)outadr_l;
    static const char sevch[8] = { 'W', 'S', 'E', 'I', 'F', '?', '?', '?' };

    if (!bufadr || !bufadr->dsc$a_pointer) return SS$_BADPARAM;
    if ((flags & 15) == 0) flags = 15;

    uint32_t fac = (msgid >> 16) & 0xFFF, num = (msgid >> 3) & 0x1FFF;
    const struct ovmx_msgdef *m = NULL;
    if (msgid != 0)
        m = msgcat_find((num & 0x1000) ? msgid : (num << 3));
    const char *facname = msgcat_facility(fac);

    char text[300], ident[40], facbuf[40];
    uint32_t status = SS$_NORMAL;
    uint8_t faocnt = 0;
    if (m) {
        snprintf(ident, sizeof ident, "%s", m->ident);
        snprintf(text, sizeof text, "%s", m->text);
        snprintf(facbuf, sizeof facbuf, "%s", facname ? facname : "NONAME");
        faocnt = m->faocnt;
    } else {
        snprintf(ident, sizeof ident, "NOMSG");
        snprintf(text, sizeof text, "Message number %08X", (unsigned)msgid);
        snprintf(facbuf, sizeof facbuf, "NONAME");
        status = SS$_MSGNOTFND;
    }

    char buf[400];
    int pos = 0;
    const char *sep = "%";
    if (flags & 8) { pos += snprintf(buf + pos, sizeof buf - pos, "%s%s", sep, facbuf); sep = "-"; }
    if (flags & 4) { pos += snprintf(buf + pos, sizeof buf - pos, "%s%c", sep, sevch[msgid & 7]); sep = "-"; }
    if (flags & 2) { pos += snprintf(buf + pos, sizeof buf - pos, "%s%s", sep, ident); sep = "-"; }
    if (flags & 1)
        pos += snprintf(buf + pos, sizeof buf - pos, "%s%s", pos ? ", " : "", text);

    uint16_t outlen = (uint16_t)pos;
    if (outlen > bufadr->dsc$w_length) {
        outlen = bufadr->dsc$w_length;
        status = SS$_BUFFEROVF;
    }
    memcpy(bufadr->dsc$a_pointer, buf, outlen);
    if (msglen) *msglen = outlen;
    if (outadr) {
        outadr[0] = 0;
        outadr[1] = m ? faocnt : 0;
        outadr[2] = 0;
        outadr[3] = 0;
    }
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
    if (!msgvec) return SS$_BADPARAM;

    uint32_t arg_count = msgvec[0];
    if (arg_count < 1) return SS$_BADPARAM;

    uint32_t msgid = msgvec[1];

    /* Format the message: "%<FACILITY>-<severity>-<IDENT>, <text>". */
    char buf[512];
    vms_status_string(msgid, buf, sizeof(buf));

    /* Facility-name override (VMS $PUTMSG facnam): replace the %FACILITY token
     * of the formatted message with the caller-supplied name -- e.g.
     * "%SYSTEM-F-ABORT, ..." with facnam="MYAPP" becomes "%MYAPP-F-ABORT, ...".
     * Previously facnam was silently discarded, so a caller's override never
     * took effect while $PUTMSG still reported success (vms-7a2). */
    if (facnam && facnam->dsc$a_pointer && facnam->dsc$w_length > 0 &&
        buf[0] == '%') {
        const char *dash = strchr(buf + 1, '-');
        if (dash) {
            char fac[32];
            uint16_t fl = facnam->dsc$w_length;
            if (fl > (uint16_t)(sizeof(fac) - 1)) fl = (uint16_t)(sizeof(fac) - 1);
            memcpy(fac, facnam->dsc$a_pointer, fl);
            fac[fl] = '\0';
            /* VMS facility names are upper case. */
            for (char *p = fac; *p; p++)
                *p = (char)toupper((unsigned char)*p);
            char rebuilt[512];
            snprintf(rebuilt, sizeof(rebuilt), "%%%s%s", fac, dash);
            strncpy(buf, rebuilt, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
        }
    }

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
