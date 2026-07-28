/*
 * scs_start.h - Build/parse the VMS$VAXcluster phase-2 START/config exchange
 * (opcode 0x41) and drive OVMX's own SCS sequenced-message counters
 * (vms-21e), spec sec 4(g) phase 2.
 *
 * WHY THIS EXISTS. After the directed-HELLO NISCA channel forms (vms-5fe),
 * an established VMScluster node sends the joiner a stream of opcode-0x41
 * START/config frames and WAITS for the joiner to answer with its own 0x41
 * START before it will proceed to the 0x4b connect (spec sec 4g phase 2->4).
 * OVMX previously never answered 0x41, so the real VAX retransmitted round-0
 * START forever (observed in ovmx-5fe-channel-formed-20260728.pcap: VAX1 sent
 * 18 round-0 STARTs to OVMX, 0 replies, config-round never advanced). This
 * module is the missing joiner-side START responder.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md rule 8 / spec sec 0). The 106-byte START
 * body and 46-byte ACK body baked in here are the byte-exact contents of two
 * real JOINER frames captured on the reference lab wire and dumped verbatim:
 *   - START (106B) = formation-ci1-joinwindow.pcap raw frame 24
 *                    (SCA#16, VAX2->VAX1, the JOINER's round-0 START;
 *                     SCSSYSTEMID 1026, node "VAX2    ")
 *   - ACK   (46B)  = formation-ci1-joinwindow.pcap raw frame 28
 *                    (SCA#20, VAX2->VAX1, the JOINER's round-2 ack)
 * OVMX plays the SAME role VAX2 plays in that capture (the joiner), so these
 * are the correct role-templates. No VSI/HPE source or binary was read.
 *
 * GROUNDED fields (spec sec 4g phase 2, validated byte-exact in vms-cd0 and
 * re-validated against these frames in vms-21e) that this module
 * positions/substitutes:
 *   - [16] opcode 0x41, [17] format constant 0x13
 *   - [24:26] 0x0012 = 18 = SYSGEN NISCS_LAN_OVRHD
 *   - [42:44] inner length = payload_len - 44 (62 for START, 2 for ACK)
 *   - [44:46] config-round counter (0,1 for START rounds; 2 for the ACK)
 *   - [46:48] SCSSYSTEMID (LE u16) -- OVMX's, from the vms-ci.8 SYSGEN store
 *   - [58:66] software version "VMS V7.3" (ASCII, from template)
 *   - [74:78] hardware type "VAX " (ASCII, from template)
 *   - [90:98] node name (ASCII, 8-byte blank-padded, left-justified) -- OVMX's SCSNODE
 *
 * SEQ/ACK COUNTERS -- "a real state machine, NOT a replay" (item bar).
 * OVMX computes the SCS sequenced-message counters from its OWN tracked
 * scs_seq_state, not by echoing a captured frame's bytes. The START is
 * sequenced-message #1, so a fresh joiner emits send_seq=1 at [20:22] (and
 * the mirror at [30:32]) and cnt_b=1 at [22:24] -- byte-exact to the joiner
 * VAX2 emitted and the established VAX accepted (GROUNDED). The leading
 * counter [18:20] is 0 during the pre-VC START phase (GROUNDED observed; its
 * exact CSB role is inferred per spec sec 4d/4g). scs_seq_state advances
 * send_seq per NEW sequenced message and tracks the peer's send_seq for the
 * later 0x4b connect phase (spec sec 4g phase 4).
 *
 * REPLAYED (ungrounded, spec sec 4g/sec 5) fields left at their captured
 * values, labeled REPLAY: the [54:56]=0x0240 / [56:58]=0x00d8 constant pair,
 * and the two per-boot incarnation tokens [66:71] and [98:104]. The spec
 * grounds these as per-boot/incarnation timestamps that CHANGE across reboots
 * of the same node and are NOT node identity; the established VAX accepted
 * two different nodes' differing token values, so they are not peer-validated.
 * OVMX replays the template's values (the documented lab-shortcut posture,
 * mirroring the HELLO nonce replay in scs_hello.h). A veracity adversary
 * should treat every non-substituted, non-counter byte here as a labeled
 * replay of an observed constant, inventing no new field semantics.
 */
#ifndef SCS_START_H
#define SCS_START_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCS_START_SCA_LEN       106 /* total SCA content bytes (106-byte START class) */
#define SCS_START_FRAME_LEN     120 /* 14-byte Ethernet header + SCS_START_SCA_LEN */
#define SCS_START_ACK_SCA_LEN   46  /* total SCA content bytes (46-byte round-2 ack) */
#define SCS_START_ACK_FRAME_LEN 60  /* 14-byte Ethernet header + SCS_START_ACK_SCA_LEN */

#define SCS_START_OPCODE        0x41 /* START/config (spec sec 4g phase 2) */
#define SCS_START_FORMAT        0x13 /* format/version constant (GROUNDED, spec sec 4g) */
#define SCS_START_ACK_ROUND     2    /* config-round of the terminating 46-byte ack */
#define SCS_START_NODENAME_LEN  8    /* fixed 8-byte blank-padded node-name field (spec sec 4g) */

/*
 * scs_seq_state - OVMX's own SCS sequenced-message counter state for one VC.
 * send_seq is OVMX's next-to-send sequence number (a fresh VC starts at 1);
 * recv_seq is the highest peer send-sequence OVMX has observed (starts 0).
 * These drive the START/connect handshakes from tracked state rather than by
 * replaying a captured frame's counters (spec sec 4g "non-replayable").
 */
struct scs_seq_state {
    uint16_t send_seq;
    uint16_t recv_seq;
};

/* Initialize a fresh VC: send_seq=1, recv_seq=0 (a joiner's starting point). */
void scs_seq_init(struct scs_seq_state *s);

/* Record a peer send-sequence number: recv_seq = max(recv_seq, peer_send_seq). */
void scs_seq_note_recv(struct scs_seq_state *s, uint16_t peer_send_seq);

/*
 * scs_seq_advance - consume the current send_seq for a NEW sequenced message
 * and advance to the next. Returns the sequence number to stamp on the
 * message just sent. (Retransmits of the SAME message at different
 * config-rounds do NOT advance -- call this once per new sequenced message.)
 */
uint16_t scs_seq_advance(struct scs_seq_state *s);

struct scs_start_params {
    uint8_t  dst_mac[6];      /* Ethernet dst = peer's observed src MAC */
    uint8_t  src_mac[6];      /* Ethernet src + SCA src-logical addr = OVMX HW MAC */
    uint8_t  peer_logical[6]; /* SCA dest-logical addr [2:8] = peer's advertised logical addr
                                 (its src-logical field; == dst_mac for a non-DECnet peer) */
    uint16_t scssystemid;     /* OVMX's SCSSYSTEMID (LE u16 at [46:48]) */
    char     node_name[SCS_START_NODENAME_LEN + 1]; /* NUL-terminated, <=8 chars, SCSNODE */
    uint16_t config_round;    /* config-round counter [44:46] */
    uint16_t send_seq;        /* SCS send-seq [20:22] + mirror [30:32] (from scs_seq_state) */
    uint16_t recv_ack;        /* leading counter [18:20] (0 during START; inferred CSB role) */
};

/*
 * scs_start_build - Fill out[SCS_START_FRAME_LEN] with a complete
 * Ethernet+SCA 106-byte 0x41 START/config frame carrying OVMX's identity and
 * the supplied counters/round (spec sec 4g phase 2).
 * Returns 0, or -1 if p/out is NULL or node_name exceeds SCS_START_NODENAME_LEN.
 */
int scs_start_build(const struct scs_start_params *p,
                    uint8_t out[SCS_START_FRAME_LEN]);

/*
 * scs_start_build_ack - Fill out[SCS_START_ACK_FRAME_LEN] with the terminating
 * 46-byte 0x41 round-2 ACK (spec sec 4g phase 2). Uses p->src_mac,
 * p->peer_logical, p->send_seq, p->recv_ack; config_round is forced to 2.
 * Returns 0, or -1 if p/out is NULL.
 */
int scs_start_build_ack(const struct scs_start_params *p,
                        uint8_t out[SCS_START_ACK_FRAME_LEN]);

/* Read-only view of a received 0x41 START/config or ACK frame. */
struct scs_start_view {
    uint16_t total_sca_len; /* LE u16 at abs 14 + 2 (106 START, 46 ACK) */
    uint8_t  opcode;        /* abs 30 (expect 0x41) */
    uint8_t  format;        /* abs 31 (expect 0x13) */
    uint8_t  is_ack;        /* 1 if the 46-byte ACK class, else 0 */
    uint16_t config_round;  /* [44:46] */
    uint16_t scssystemid;   /* [46:48] LE (valid only for the 106-byte START) */
    uint16_t send_seq;      /* [20:22] LE -- peer's send-sequence */
    uint16_t recv_ack;      /* [18:20] LE */
};

/*
 * scs_start_parse - Parse a received Ethernet+SCA 0x41 frame's fields into *v.
 * `frame` points at the Ethernet dst (abs 0); `len` is the captured length.
 * Returns 0 on success, -1 if frame/v is NULL, the frame is too short, or the
 * opcode at abs 30 is not 0x41.
 */
int scs_start_parse(const uint8_t *frame, size_t len, struct scs_start_view *v);

#ifdef __cplusplus
}
#endif

#endif /* SCS_START_H */
