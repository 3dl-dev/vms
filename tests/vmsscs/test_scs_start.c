/*
 * test_scs_start.c - unit tests for the vms-21e phase-2 START/config (0x41)
 * builder/parser and the SCS sequenced-message counter state machine.
 *
 * Every asserted byte value is either GROUNDED (spec sec 4g phase 2, validated
 * byte-exact in vms-cd0/vms-21e) or a documented REPLAY of a real captured
 * joiner frame (formation-ci1-joinwindow.pcap raw frames 24/28, and the
 * established-node frames 23/27 used to ground the parser). This is the
 * REQUIRED targeted unit test; it does NOT replace the live-wire SDA proof
 * (item DONE condition), nor is it replaced by it.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scs_start.h"

static int failures = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  OK: %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

static void check_bytes(const uint8_t *got, const uint8_t *want, size_t n, const char *what)
{
    check(memcmp(got, want, n) == 0, what);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* OVMX test identity. */
static const uint8_t ovmx_mac[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
/* VAX1 (DECnet node): Ethernet src == its logical LAVC addr. */
static const uint8_t vax1_mac[6] = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 };

/* Byte-exact real established-node round-0 START (raw frame 23, VAX1->VAX2,
 * SCSSYSTEMID 1025) and round-2 ACK (raw frame 27), full Ethernet frames. */
static const uint8_t real_start_vax1[120] = {
    0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
    0x60, 0x07, 0x68, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x41, 0x13, 0x00, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x40, 0x02, 0xd8, 0x00,
    0x56, 0x4d, 0x53, 0x20, 0x56, 0x37, 0x2e, 0x33, 0x66, 0x15, 0x66, 0x7a,
    0x93, 0x00, 0xbc, 0x00, 0x56, 0x41, 0x58, 0x20, 0x06, 0x00, 0x00, 0x0a,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77, 0x00, 0x56, 0x41, 0x58, 0x31,
    0x20, 0x20, 0x20, 0x20, 0x80, 0x98, 0xb1, 0x55, 0x96, 0x00, 0xbc, 0x00,
};
static const uint8_t real_ack_vax1[60] = {
    0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
    0x60, 0x07, 0x2c, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x41, 0x13, 0x00, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,
};

static void test_seq_state(void)
{
    printf("[SCS seq/ack state machine]\n");
    struct scs_seq_state s;
    scs_seq_init(&s);
    check(s.send_seq == 1 && s.recv_seq == 0, "init: send_seq=1 recv_seq=0 (fresh joiner)");

    /* advance returns the current send_seq then increments (per NEW message). */
    check(scs_seq_advance(&s) == 1, "advance #1 returns 1");
    check(s.send_seq == 2, "send_seq now 2");
    check(scs_seq_advance(&s) == 2, "advance #2 returns 2");
    check(s.send_seq == 3, "send_seq now 3");

    /* note_recv tracks the high-water peer send-sequence (ack side). */
    scs_seq_note_recv(&s, 5);
    check(s.recv_seq == 5, "note_recv(5) -> recv_seq=5");
    scs_seq_note_recv(&s, 3);
    check(s.recv_seq == 5, "note_recv(3) does not regress recv_seq");
    scs_seq_note_recv(&s, 7);
    check(s.recv_seq == 7, "note_recv(7) -> recv_seq=7");

    /* NULL-safety. */
    scs_seq_init(NULL);
    scs_seq_note_recv(NULL, 1);
    check(scs_seq_advance(NULL) == 0, "advance(NULL) returns 0 (no crash)");
}

static void test_build_start(void)
{
    printf("[build 0x41 START]\n");
    struct scs_start_params sp;
    memset(&sp, 0, sizeof(sp));
    memcpy(sp.dst_mac, vax1_mac, 6);
    memcpy(sp.src_mac, ovmx_mac, 6);
    memcpy(sp.peer_logical, vax1_mac, 6);
    sp.scssystemid = 1030;
    strncpy(sp.node_name, "OVMX", sizeof(sp.node_name) - 1);
    sp.config_round = 1;
    sp.send_seq = 1;
    sp.recv_ack = 0;

    uint8_t out[SCS_START_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    check(scs_start_build(&sp, out) == 0, "scs_start_build succeeds");

    /* Ethernet header. */
    check_bytes(out + 0, vax1_mac, 6, "Ethernet dst == peer MAC");
    check_bytes(out + 6, ovmx_mac, 6, "Ethernet src == OVMX HW MAC");
    check(out[12] == 0x60 && out[13] == 0x07, "ethertype 0x6007");

    /* SCA envelope (abs = 14 + payload offset). */
    check(out[14] == 0x68 && out[15] == 0x00, "SCA length field 0x0068 (total 106)");
    check_bytes(out + 16, vax1_mac, 6, "SCA dest logical == peer_logical (abs 16)");
    check_bytes(out + 24, ovmx_mac, 6, "SCA src logical == OVMX HW MAC (abs 24)");
    check(out[30] == 0x41 && out[31] == 0x13, "opcode 0x41, format 0x13 (abs 30/31, GROUNDED)");

    /* Counters (state-machine driven, not replayed). */
    check(le16(out + 32) == 0, "leading counter [18:20]==recv_ack==0 (abs 32, GROUNDED)");
    check(le16(out + 34) == 1, "SCS send_seq [20:22]==1 (abs 34, GROUNDED joiner value)");
    check(le16(out + 36) == 1, "cnt_b [22:24]==send_seq==1 (abs 36)");
    check(le16(out + 44) == 1, "send_seq mirror [30:32]==1 (abs 44, GROUNDED)");

    /* GROUNDED constants + identity. */
    check(le16(out + 38) == 18, "NISCS_LAN_OVRHD [24:26]==18 (abs 38, GROUNDED)");
    check(le16(out + 56) == 62, "inner length [42:44]==62==paylen-44 (abs 56, GROUNDED)");
    check(le16(out + 58) == 1, "config-round [44:46]==1 (abs 58, substituted)");
    check(le16(out + 60) == 1030, "SCSSYSTEMID [46:48]==1030 (abs 60, OVMX identity)");
    check_bytes(out + 72, (const uint8_t *)"VMS V7.3", 8, "version [58:66]=='VMS V7.3' (abs 72, GROUNDED)");
    check_bytes(out + 88, (const uint8_t *)"VAX ", 4, "hardware [74:78]=='VAX ' (abs 88, GROUNDED)");
    check_bytes(out + 104, (const uint8_t *)"OVMX    ", 8,
                "node name [90:98]=='OVMX    ' 8-byte blank-padded (abs 104, GROUNDED encoding)");

    /* Round-trip through the parser. */
    struct scs_start_view v;
    check(scs_start_parse(out, sizeof(out), &v) == 0, "parse our START ok");
    check(v.total_sca_len == 106 && v.opcode == 0x41 && v.is_ack == 0, "round-trip: 106B START");
    check(v.config_round == 1 && v.scssystemid == 1030 && v.send_seq == 1,
          "round-trip: round/sysid/send_seq recovered");

    /* Exactly 8 chars fills the field; >8 (unterminated) is rejected. */
    struct scs_start_params ok8 = sp;
    memcpy(ok8.node_name, "ABCDEFGH", 9); /* 8 chars + NUL */
    check(scs_start_build(&ok8, out) == 0, "8-char node name accepted");
    struct scs_start_params bad = sp;
    memset(bad.node_name, 'X', sizeof(bad.node_name)); /* 9 non-NUL bytes */
    check(scs_start_build(&bad, out) == -1, "overlong (>8) node name rejected");
    check(scs_start_build(NULL, out) == -1, "NULL params rejected");
    check(scs_start_build(&sp, NULL) == -1, "NULL out rejected");
}

static void test_build_ack(void)
{
    printf("[build 0x41 round-2 ACK]\n");
    struct scs_start_params sp;
    memset(&sp, 0, sizeof(sp));
    memcpy(sp.dst_mac, vax1_mac, 6);
    memcpy(sp.src_mac, ovmx_mac, 6);
    memcpy(sp.peer_logical, vax1_mac, 6);
    sp.send_seq = 1;
    sp.recv_ack = 0;

    uint8_t out[SCS_START_ACK_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    check(scs_start_build_ack(&sp, out) == 0, "scs_start_build_ack succeeds");
    check_bytes(out + 0, vax1_mac, 6, "ACK Ethernet dst == peer MAC");
    check_bytes(out + 6, ovmx_mac, 6, "ACK Ethernet src == OVMX HW MAC");
    check(out[14] == 0x2c && out[15] == 0x00, "ACK SCA length field 0x002c (total 46)");
    check(out[30] == 0x41 && out[31] == 0x13, "ACK opcode 0x41, format 0x13");
    check(le16(out + 56) == 2, "ACK inner length [42:44]==2 (abs 56, GROUNDED)");
    check(le16(out + 58) == 2, "ACK config-round [44:46]==2 (abs 58, GROUNDED)");
    check(le16(out + 34) == 1, "ACK send_seq [20:22]==1 (abs 34)");

    struct scs_start_view v;
    check(scs_start_parse(out, sizeof(out), &v) == 0, "parse our ACK ok");
    check(v.is_ack == 1 && v.total_sca_len == 46 && v.config_round == 2,
          "round-trip: 46B round-2 ACK");
}

static void test_parse_real(void)
{
    printf("[parse real captured 0x41 frames]\n");
    struct scs_start_view v;

    check(scs_start_parse(real_start_vax1, sizeof(real_start_vax1), &v) == 0,
          "parse real VAX1 START ok");
    check(v.total_sca_len == 106, "real START total SCA len == 106");
    check(v.opcode == 0x41 && v.format == 0x13, "real START opcode 0x41 / format 0x13 (GROUNDED)");
    check(v.is_ack == 0, "real START is not an ack");
    check(v.config_round == 0, "real START config-round == 0 (GROUNDED)");
    check(v.scssystemid == 1025, "real START SCSSYSTEMID == 1025 (VAX1, GROUNDED)");
    check(v.send_seq == 1, "real START send_seq == 1 (GROUNDED joiner-phase value)");
    check(v.recv_ack == 0, "real START leading counter == 0 (GROUNDED)");

    check(scs_start_parse(real_ack_vax1, sizeof(real_ack_vax1), &v) == 0,
          "parse real VAX1 ACK ok");
    check(v.is_ack == 1, "real ACK flagged is_ack");
    check(v.total_sca_len == 46, "real ACK total SCA len == 46");
    check(v.config_round == 2, "real ACK config-round == 2 (GROUNDED)");

    /* Rejections: non-0x41 opcode, NULL, too-short. */
    uint8_t not41[120];
    memcpy(not41, real_start_vax1, sizeof(not41));
    not41[30] = 0x4b;
    check(scs_start_parse(not41, sizeof(not41), &v) == -1, "non-0x41 opcode rejected");
    check(scs_start_parse(NULL, 120, &v) == -1, "NULL frame rejected");
    check(scs_start_parse(real_start_vax1, 40, &v) == -1, "too-short frame rejected");
}

int main(void)
{
    printf("test_scs_start: phase-2 START/config + SCS seq/ack state machine (vms-21e)\n");
    test_seq_state();
    test_build_start();
    test_build_ack();
    test_parse_real();
    printf("test_scs_start: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
