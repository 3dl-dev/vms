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

    memcpy(out + 24, p->src_logical, 6); /* src-logical LAVC addr, sender's own (GROUNDED, sec 4a):
                                           * the cluster-LOGICAL addr aa:00:04:00:<sysid>, NOT the
                                           * raw HW MAC -- VAX1's PEDRIVER keys peer identity here
                                           * (vms-9f3). eth-src (out+6) + HW-MAC-tail (out+120)
                                           * keep the raw src_mac. */

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

    /* abs 96-101: 48-bit little-endian monotonic timer, 100ns per tick
     * (spec sec 4b offset-96 "changing value"; GROUNDED sec 4k as the ONLY
     * field that varies across a channel-verify retransmit). Clean-room,
     * derived from the reference-lab wire (labeled inferred): the field is a
     * 6-byte LE counter advancing at ~1e7 ticks/s (measured 9.998e6/s across
     * af2-firsttimer/ci3 padded HELLOs = 100ns/tick, the VMS system-time unit),
     * a per-sender running clock -- each node carries its OWN current value
     * (VAX1 and the joiner differ below the shared high word), they do NOT echo
     * each other. The caller supplies OVMX's own local 100ns tick.
     *
     * vms-9f3 FIX: the high 2 bytes (abs 100-101) were previously baked into the
     * constant tail as 0x0099 -- that was a frozen snapshot of THIS timer's high
     * word at scs-idle-baseline capture time, NOT a constant. Freezing it made
     * OVMX's channel-verify timer look dead (never advancing at 100ns rate),
     * which is why VAX1's PEDRIVER rejected OVMX's padded HELLO and stepped the
     * probe size down instead of converging. All 6 bytes are now the live tick. */
    for (int i = 0; i < 6; i++) {
        out[96 + i] = (uint8_t)((p->timer_tick >> (8 * i)) & 0xffu);
    }

    /* Constant tail, abs 102-111 (inferred-constant, sec 4b) -- byte-exact
     * against both idle-baseline HELLOs (scs-idle-baseline frames 1/4) AND the
     * golden padded directed HELLOs (af2-established-rejoin idx 8659/8665). */
    static const uint8_t tail_const[10] = {
        0xbc, 0x00, 0x03, 0x58, 0x51, 0x41, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(out + 102, tail_const, sizeof(tail_const));

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

int scs_hello_build_directed_frame(const struct scs_hello_params *p,
                                    const uint8_t peer_mac[6],
                                    const uint8_t nonce[4],
                                    uint16_t incarnation,
                                    uint8_t out[SCS_HELLO_FRAME_LEN])
{
    if (p == NULL || peer_mac == NULL || nonce == NULL || out == NULL) {
        return -1;
    }

    /* Lay down the shared HELLO template with the peer's MAC as the dst;
     * scs_hello_build_frame() mirrors dst_mac into both the Ethernet dst
     * (abs 0) and the SCA dest logical addr (abs 16), matching the real
     * directed HELLO (scs-idle-baseline.pcap frame 2). */
    struct scs_hello_params dp = *p;
    memcpy(dp.dst_mac, peer_mac, 6);
    int rc = scs_hello_build_frame(&dp, out);
    if (rc != 0) {
        return rc;
    }

    /* Patch the directed-specific fields (spec sec 4a/4b). */
    out[30] = 0xb3;
    out[31] = 0x00;             /* per-frame word: directed value (ungrounded, REPLAYED, sec 4a) */

    memcpy(out + 68, nonce, 4); /* join nonce (GROUNDED present/stable; value REPLAYED, sec 4g) */

    /* directed-HELLO flag / node-incarnation counter (abs 92-93), LE u16 --
     * the incarnation this sender attributes to the peer (spec sec 4b/4i.B).
     * Echoes the value the member advertised for us (read off the wire); for a
     * fresh/first contact that is 1, byte-exact to every fresh-formation
     * specimen. GROUNDED. */
    out[92] = (uint8_t)(incarnation & 0xff);
    out[93] = (uint8_t)((incarnation >> 8) & 0xff);

    out[128] = 0x1f;
    out[129] = 0x00;            /* poller-sweep marker = 31 (GROUNDED, sec 4b) */

    return 0;
}

int scs_hello_build_padded_directed_frame(const struct scs_hello_params *p,
                                          const uint8_t peer_mac[6],
                                          const uint8_t nonce[4],
                                          uint16_t incarnation,
                                          uint16_t total_sca_len,
                                          uint8_t *out, size_t out_cap,
                                          size_t *frame_len_out)
{
    if (p == NULL || peer_mac == NULL || nonce == NULL || out == NULL ||
        frame_len_out == NULL) {
        return -1;
    }
    /* Must span at least the full 120-byte directed HELLO, and no more than
     * NISCS_MAX_PKTSZ (+2) of SCA content -- the GROUNDED probe ceiling (sec 4k). */
    if (total_sca_len < SCS_HELLO_SCA_LEN || total_sca_len > SCS_HELLO_PADDED_MAX_SCA) {
        return -1;
    }
    size_t frame_len = (size_t)14 + (size_t)total_sca_len;
    if (out_cap < frame_len) {
        return -1;
    }

    /* Zero the WHOLE frame first so the pad tail (abs 134..) is pure zeros;
     * scs_hello_build_directed_frame only memsets its own 134-byte window. */
    memset(out, 0, frame_len);

    int rc = scs_hello_build_directed_frame(p, peer_mac, nonce, incarnation, out);
    if (rc != 0) {
        return rc;
    }

    /* Rewrite ONLY the SCA length field (abs 14-15) to encode the padded total:
     * LE16 = total_sca_len - 2 (spec sec 2 length identity). Every other byte of
     * the 120-byte directed HELLO (abs 16-133) is unchanged, and abs 134.. stays
     * zero -- the padded frame is a genuine directed HELLO plus a zero tail
     * (GROUNDED sec 4k: smaller probes are byte-identical prefixes of the 1500B
     * frame apart from this length field). */
    uint16_t lenword = (uint16_t)(total_sca_len - 2u);
    out[14] = (uint8_t)(lenword & 0xff);
    out[15] = (uint8_t)((lenword >> 8) & 0xff);

    *frame_len_out = frame_len;
    return 0;
}
