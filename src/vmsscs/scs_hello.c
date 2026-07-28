/*
 * scs_hello.c - Build a spec-valid 0x6007 multicast HELLO frame (vms-b62).
 * See scs_hello.h for the field-by-field provenance of every byte written
 * here (which are GROUNDED, which are inferred-constant reproductions of
 * an observed wire value, and which are per-sender identity fields).
 */
#include "scs_hello.h"

#include <string.h>

void scs_hello_multicast_addr(uint16_t group, uint8_t mac_out[6])
{
    /* AB-00-04-01-<group><group> -- GROUNDED for group==1 (see scs_hello.h). */
    mac_out[0] = 0xAB;
    mac_out[1] = 0x00;
    mac_out[2] = 0x04;
    mac_out[3] = 0x01;
    mac_out[4] = (uint8_t)(group & 0xFF);
    mac_out[5] = (uint8_t)(group & 0xFF);
}

int scs_hello_build_frame(const struct scs_hello_params *p,
                           uint8_t out[SCS_HELLO_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    size_t namelen = strnlen(p->node_name, SCS_HELLO_NODENAME_LEN + 1);
    if (namelen > SCS_HELLO_NODENAME_LEN) {
        return -1;
    }

    memset(out, 0, SCS_HELLO_FRAME_LEN);

    /* --- Ethernet header (abs 0-13) --- */
    memcpy(out + 0, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07; /* ethertype 0x6007, DEC SCA/LAVC (spec sec 2, GROUNDED) */

    /* --- SCA discovery header, shared HELLO/SOLICIT template
     * (abs 14-71, spec sec 4a) --- */
    out[14] = 0x76;
    out[15] = 0x00; /* LE length field 118 -> total_SCA_content 120 (GROUNDED, sec 2) */

    memcpy(out + 16, p->dst_mac, 6); /* dest/group logical addr (GROUNDED) */

    out[22] = 0x01;
    out[23] = 0x00; /* connect flag, constant 0x0001 (observed constant) */

    memcpy(out + 24, p->src_mac, 6); /* src logical addr, sender's own (GROUNDED location;
                                       * OVMX uses its real HW MAC here -- see scs_hello.h) */

    out[30] = 0xa0;
    out[31] = 0x00; /* per-frame word, multicast value (inferred-constant, sec 4a) */

    out[32] = 0x08;
    out[33] = 0x00;
    out[34] = 0x00;
    out[35] = 0x80; /* constant prefix (inferred-constant, sec 4a) */

    out[36] = 0x05; /* message-class byte: HELLO (inferred label, GROUNDED 100% split, sec 4a) */

    out[37] = 0x01;
    out[38] = 0x00;
    out[39] = 0x00; /* constant suffix (inferred-constant, sec 4a) */

    out[40] = SCS_HELLO_NODENAME_LEN; /* node-name length prefix, GROUNDED == 6 in every
                                        * observed HELLO (SCSNODE max length) */
    {
        char namebuf[SCS_HELLO_NODENAME_LEN];
        memset(namebuf, ' ', sizeof(namebuf));
        memcpy(namebuf, p->node_name, namelen);
        memcpy(out + 41, namebuf, SCS_HELLO_NODENAME_LEN); /* node name, ASCII space-padded (GROUNDED) */
    }

    /* Constant capability/version-ish span, abs 47-63 (inferred-constant,
     * sec 4a) -- byte-exact against both scs-idle-baseline.pcap frame 1
     * (VAX1) and frame 4 (VAX2). */
    static const uint8_t cap_span[17] = {
        0x00, 0x80, 0x01, 0xff, 0x83, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18
    };
    memcpy(out + 47, cap_span, sizeof(cap_span));

    out[64] = 0x03; /* constant (inferred-constant, sec 4a) */
    out[65] = 0x00;
    out[66] = 0x00;
    out[67] = 0x00; /* zero (inferred-constant, sec 4a) */

    out[68] = 0x00;
    out[69] = 0x00;
    out[70] = 0x00;
    out[71] = 0x00; /* connect/join nonce = 0 -- GROUNDED for multicast HELLO (sec 4a) */

    /* --- HELLO-specific tail (abs 72-133, spec sec 4b) --- */
    /* abs 72-91: zero padding (already memset above) */

    out[92] = 0x00;
    out[93] = 0x00; /* directed-HELLO flag = 0x0000 -- GROUNDED for multicast (sec 4b) */

    out[94] = 0x92;
    out[95] = 0x05; /* constant trailer 0x9205 (inferred-constant, sec 4b) */

    out[96] = (uint8_t)(p->timer_tick & 0xff);
    out[97] = (uint8_t)((p->timer_tick >> 8) & 0xff);
    out[98] = (uint8_t)((p->timer_tick >> 16) & 0xff);
    out[99] = (uint8_t)((p->timer_tick >> 24) & 0xff); /* changing value, LE (inferred/unknown
                                                          * semantics, per-sender/per-time, sec 4b) */

    /* Constant tail, abs 100-111 (inferred-constant, sec 4b) -- byte-exact
     * against both baseline frames. */
    static const uint8_t tail_const[12] = {
        0x99, 0x00, 0xbc, 0x00, 0x03, 0x58, 0x51, 0x41, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(out + 100, tail_const, sizeof(tail_const));

    /* abs 112-119: zero padding (already memset above) */

    memcpy(out + 120, p->src_mac, 6); /* sender's real HW LAN MAC (GROUNDED, sec 4b) */

    out[126] = 0x26;
    out[127] = 0x00; /* constant trailer 0x2600 (inferred-constant, sec 4b) */

    out[128] = 0x00;
    out[129] = 0x00; /* poller-sweep/directed marker = 0x0000 -- GROUNDED for multicast (sec 4b) */

    out[130] = 0x64;
    out[131] = 0x00; /* constant 0x0064 (inferred-constant, sec 4b) */

    out[132] = 0x00;
    out[133] = 0x00; /* trailer, always 0x0000 (inferred-constant, sec 4b) */

    return 0;
}
