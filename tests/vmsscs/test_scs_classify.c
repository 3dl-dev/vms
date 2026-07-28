/*
 * test_scs_classify.c - unit test for the GROUNDED SCA length classifier.
 *
 * Exercises the exact length classes documented in
 * docs/cluster-protocol-spec.md section 2, Table 2 (message-class census
 * over formation-ci1.pcap, 18541 frames): 120=HELLO, 190=SCS fixed
 * message, 78=SOLICIT, plus representative OTHER-class sizes (41=short
 * ack/credit, 70=connect/directory-lookup/MSCP, 1500=near
 * NISCS_MAX_PKTSZ=1498 block transfer) and the RUNT edge case.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scs_classify.h"

/* Fill buf[0:2] with the LE SCA length field such that
 * scs_classify_sca_payload() will decode total_SCA_content == total. */
static void set_lenword(uint8_t *buf, uint16_t total)
{
    uint16_t lenword = (uint16_t)(total - 2);
    buf[0] = (uint8_t)(lenword & 0xff);
    buf[1] = (uint8_t)(lenword >> 8);
}

int main(void)
{
    uint8_t buf[2048];
    uint16_t total;

    memset(buf, 0, sizeof(buf));

    /* HELLO: total SCA content 120 bytes. */
    set_lenword(buf, 120);
    total = 0;
    assert(scs_classify_sca_payload(buf, 120, &total) == SCS_CLASS_HELLO);
    assert(total == 120);
    assert(strcmp(scs_class_name(SCS_CLASS_HELLO), "HELLO") == 0);

    /* SCS fixed message: 190 bytes -- dominant DLM/directory class. */
    set_lenword(buf, 190);
    total = 0;
    assert(scs_classify_sca_payload(buf, 190, &total) == SCS_CLASS_SCS_FIXED);
    assert(total == 190);

    /* SOLICIT: 78 bytes -- satellite boot disk-serve request. */
    set_lenword(buf, 78);
    total = 0;
    assert(scs_classify_sca_payload(buf, 78, &total) == SCS_CLASS_SOLICIT);
    assert(total == 78);

    /* SCS short ack/credit: 41 bytes -> OTHER (spec Table 2). */
    set_lenword(buf, 41);
    total = 0;
    assert(scs_classify_sca_payload(buf, 41, &total) == SCS_CLASS_OTHER);
    assert(total == 41);

    /* Connect/directory-lookup & MSCP req/resp class: 70 bytes -> OTHER. */
    set_lenword(buf, 70);
    total = 0;
    assert(scs_classify_sca_payload(buf, 70, &total) == SCS_CLASS_OTHER);
    assert(total == 70);

    /* Large block-transfer frame near NISCS_MAX_PKTSZ=1498 -> OTHER. */
    set_lenword(buf, 1500);
    total = 0;
    assert(scs_classify_sca_payload(buf, 1500, &total) == SCS_CLASS_OTHER);
    assert(total == 1500);

    /* Runt: fewer than 2 bytes available -- can't read the length field.
     * total_sca_len_out must be left untouched (NULL is also accepted). */
    total = 0xDEAD;
    assert(scs_classify_sca_payload(buf, 1, &total) == SCS_CLASS_RUNT);
    assert(total == 0xDEAD);
    assert(scs_classify_sca_payload(buf, 0, NULL) == SCS_CLASS_RUNT);
    assert(scs_classify_sca_payload(NULL, 0, NULL) == SCS_CLASS_RUNT);

    printf("test_scs_classify: all assertions passed\n");
    return 0;
}
