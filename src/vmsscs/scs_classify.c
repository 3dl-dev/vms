/*
 * scs_classify.c - SCA frame classification by the GROUNDED length rule.
 * See scs_classify.h for the rule and its provenance.
 */
#include "scs_classify.h"

const char *scs_class_name(scs_class_t cls)
{
    switch (cls) {
    case SCS_CLASS_HELLO:     return "HELLO";
    case SCS_CLASS_SCS_FIXED: return "SCS-FIXED";
    case SCS_CLASS_SOLICIT:   return "SOLICIT";
    case SCS_CLASS_OTHER:     return "OTHER";
    case SCS_CLASS_RUNT:      return "RUNT";
    }
    return "UNKNOWN";
}

scs_class_t scs_classify_sca_payload(const uint8_t *sca_payload, size_t len,
                                      uint16_t *total_sca_len_out)
{
    if (sca_payload == NULL || len < 2) {
        return SCS_CLASS_RUNT;
    }

    /* LE_uint16(bytes[14:16]) + 2 -- docs/cluster-protocol-spec.md sec 2 */
    uint16_t lenword = (uint16_t)(sca_payload[0] | ((uint16_t)sca_payload[1] << 8));
    uint16_t total = (uint16_t)(lenword + 2);

    if (total_sca_len_out != NULL) {
        *total_sca_len_out = total;
    }

    switch (total) {
    case 120: return SCS_CLASS_HELLO;
    case 190: return SCS_CLASS_SCS_FIXED;
    case 78:  return SCS_CLASS_SOLICIT;
    default:  return SCS_CLASS_OTHER;
    }
}
