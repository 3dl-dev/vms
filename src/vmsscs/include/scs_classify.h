/*
 * scs_classify.h - SCA frame classification by the GROUNDED length rule.
 *
 * Source: docs/cluster-protocol-spec.md section 2 ("Datalink / SCA framing
 * -- GROUNDED"), cross-validated against 24,570 SCA frames across 5
 * reference-lab pcaps (0 unexplained residuals). The dissector companion
 * is tools/cluster/dissect_sca.py:classify().
 *
 *   total_SCA_content = LE_uint16(bytes[14:16]) + 2
 *
 * bytes[14:16] is the 2-byte SCA length field immediately following the
 * 14-byte Ethernet header (dst[6] + src[6] + ethertype[2]). This header is
 * NOT a frame-type enum -- it is a payload length; see the spec for the
 * disambiguation proof. Table 2 of the spec gives the message-class
 * census by total_SCA_content size:
 *
 *   120 bytes -> HELLO (keepalive/discovery beacon)
 *   190 bytes -> SCS fixed message (dominant class: DLM + directory
 *                lookup traffic; the only class with a GROUNDED Con.ID
 *                location)
 *    78 bytes -> SOLICIT (satellite boot disk-serve request)
 *   everything else (41=short ack/credit, 58/62/66/70/86/94/106/110=
 *   connect/directory-lookup & MSCP req/resp, up to NISCS_MAX_PKTSZ=1498
 *   block-transfer frames, ...) -> OTHER. This skeleton does not decode
 *   OTHER further -- that is later item scope (see docs/design-cluster-
 *   node.md sec 3.1/7), not vms-294.
 */
#ifndef SCS_CLASSIFY_H
#define SCS_CLASSIFY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCS_CLASS_HELLO = 0,     /* total SCA content == 120 bytes */
    SCS_CLASS_SCS_FIXED = 1, /* total SCA content == 190 bytes */
    SCS_CLASS_SOLICIT = 2,   /* total SCA content == 78 bytes */
    SCS_CLASS_OTHER = 3,     /* any other length (short ack, connect/
                                 directory-lookup, MSCP req/resp, block
                                 transfer, ...) */
    SCS_CLASS_RUNT = 4       /* fewer than 2 bytes available -- can't even
                                 read the SCA length field */
} scs_class_t;

/* Returns a static, non-NULL string name for a class value. */
const char *scs_class_name(scs_class_t cls);

/*
 * scs_classify_sca_payload - classify one frame by the GROUNDED length rule.
 *
 * sca_payload must point at absolute frame offset 14 (i.e. the first byte
 * AFTER the 14-byte Ethernet header / immediately at the SCA length field),
 * matching the byte-offset convention used throughout
 * docs/cluster-protocol-spec.md and dissect_sca.py. len is the number of
 * bytes available starting there (typically recv()'d length - 14).
 *
 * Returns SCS_CLASS_RUNT (and leaves *total_sca_len_out untouched) if
 * sca_payload is NULL or len < 2.
 *
 * total_sca_len_out, if non-NULL, receives total_SCA_content (the decoded
 * LE_uint16 length-field value + 2) for every class except RUNT.
 */
scs_class_t scs_classify_sca_payload(const uint8_t *sca_payload, size_t len,
                                      uint16_t *total_sca_len_out);

#ifdef __cplusplus
}
#endif

#endif /* SCS_CLASSIFY_H */
