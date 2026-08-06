/*
 * scs_env.h - THE SCS MESSAGE ENVELOPE, as a layer (vms-ec7, MSCP epic Phase A).
 *
 * WHY THIS EXISTS. Every SCS message OVMX emits or receives carries the same
 * six-field header at the same SCA-content offsets. Before this module the tree
 * knew that in prose and nowhere in code: the RECEIVE side had one decoder
 * (scs_rx.c, vms-7c0) and the SEND side had SIX independent copies of the same
 * stores, one per message class --
 *
 *     scs_disc.c   put_le16(pl + 42, sca_len - 44); put_le32(pl + 50/54, ...)
 *     scs_dir.c    put_le16(out + 14 + 46, op);     put_le32(out + 14 + 50/54, ...)
 *     scs_mscp.c   put_le32(out + 14 + 50/54, ...)
 *     scs_connect.c put_le32(out + 14 + 50/54, ...)
 *     scs_member.c put_le32(out + 14 + 50/54, ...)
 *
 * -- and, for the two fields no builder wrote at all, the MTYPE and the credit,
 * whatever byte the captured template happened to carry. A message class was
 * therefore identified on the wire by a byte nobody in the program could name.
 * That is the per-class template special-casing docs/design-mscp-direction.md
 * sec 3 "Phase A" exists to dissolve, and it is the precondition for Phases B
 * (MSCP client on documented fields), C (types 8/9) and D (disk server): none of
 * them can put a message on the wire without first being able to say which
 * message type it is.
 *
 * WHAT THIS MODULE IS. One build path, one parse path, one set of offsets, one
 * MTYPE namespace, one conformance test. It is PURE: no socket, no allocation,
 * no environment, no logging. Every function is a total function of its
 * arguments over a caller-supplied buffer.
 *
 * ===================== SOURCE AND GROUNDING (rule 8) =====================
 *
 * PUBLIC DOC. Roy G. Davis, *VAXcluster Principles*, Digital Press 1993:
 *   - p. 4-13 defines the SCS MTYPE taxonomy: application message, application
 *     datagram, SCS control message.
 *   - Figure 4-5 (p. 4-14) draws the nesting -- an MSCP command under an SCS
 *     header reading `CREDIT -- SCS MTYPE, DESTINATION CONID, SOURCE CONID`.
 *   - p. 4-15 grounds DISPATCH ON MTYPE: a message goes to the message input
 *     routine in the CDT named by the destination CONID.
 *   - p. 4-68: "This field [credit] must contain a 0 in datagram packets."
 *
 * OBSERVATION. docs/cluster-protocol-spec.md sec 4(h)(1b) and
 * docs/design-mscp-direction.md sec 1.1, measured over the full lab-1 corpus
 * (163 pcaps: cluster/work, cluster/captures, clean-cluster/captures) -- every
 * SCS message, from the 58-content control shorts to the 190-content add-member
 * class to the 94-content MSCP command frames, shares:
 *
 *     [42:44] inner length == total SCA content - 44   (0x0E on 58, 0x32 on 94,
 *                                                       0x42 on 110, 0x92 on 190)
 *     [44:46] format word  == 0x0004                   (every frame)
 *     [46:48] MTYPE, LE u16
 *     [48:50] credit                                   (sec 4(g) WIRE VERDICT)
 *     [50:54] DESTINATION Con.ID                       (sec 4(g) ph.4 / 4(h))
 *     [54:58] SOURCE Con.ID                            (swapped by direction)
 *     [58:..] the SYSAP payload
 *
 * THE BUILD PATH IS MEASURED, NOT ASSERTED. tools/cluster/scs_env_measure.py
 * part (D) walks every envelope-conformant frame in the lab-1 library, rebuilds
 * content[42:58] from the parsed fields using scs_env_build()'s own rule --
 * inner length DERIVED as total-44, format word a CONSTANT, then MTYPE, credit
 * and the Con.ID pair written back -- and requires the sixteen bytes to come
 * back byte-identical to the capture. Result: **0 mismatches over 319,575
 * frames** (48 pcaps, 2026-08-06). The frame count is a FLOOR the gate
 * re-checks, not a total to be reproduced: the reference lab keeps capturing,
 * so the corpus only grows, and a SMALLER population means the claim is being
 * checked against less evidence than it was made on.
 *
 * Re-derive every one of those with tools/cluster/scs_env_measure.py on a host
 * that has the captures; ctest -R scs_env_figures is the gate.
 *
 * ================= WHAT IS *NOT* AN SCS MESSAGE ENVELOPE ==================
 *
 * The first two fields are the CONFORMANCE TEST, and it is load-bearing. These
 * classes fail it BY DESIGN and must never be read with these offsets
 * (spec sec 4(h)(1d)):
 *
 *   - the 106-content START / config class (marker 0x4113): [44:46] is the
 *     config-round counter and [46:48] is the SCSSYSTEMID -- see scs_start.c,
 *     which deliberately does NOT use this module.
 *   - the 120-content HELLO class -- see scs_hello.c, likewise.
 *   - the still-undecoded 70-content class: its [42:44] is 9..13, never
 *     total-44, and its [44:46] is 0x522f / 0x532f / 0x2abe / ...
 *
 * Reading an MTYPE out of one of those is the exact method confound
 * docs/design-mscp-direction.md sec 4 records ("an earlier scratch census read
 * [46:48] in the 70-content class and printed types 1..22"). scs_env_parse()
 * refuses them, and refusing is not an error -- the caller keeps handling such a
 * frame the way it always has.
 *
 * ========================= THE MTYPE NAMESPACE ============================
 *
 * Exactly {0..10} over ~1,000,000 envelope-conformant frames; 0..9 are the
 * connection-control messages spec sec 4(h)(1a) names and 10 is the p. 4-13
 * application message. 8 and 9 are OBSERVED and UNNAMED -- vms-f03 (Phase C)
 * owns their identification and this header does not guess at one. Anything
 * outside {0..10} routes to SCS_ENV_ROUTE_UNKNOWN, is COUNTED, and is NEVER
 * assumed to be an application datagram: no application-datagram MTYPE has ever
 * been observed on this wire, and picking a value would be a guess presented as
 * a decode.
 */
#ifndef SCS_ENV_H
#define SCS_ENV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- offsets: content-relative. "Content" is the SCA payload, absolute frame
 * offset 14, immediately after the Ethernet header -- the same convention as
 * scs_classify.h and tools/cluster/dissect_sca.py. THIS IS THE ONLY PLACE IN
 * THE TREE THESE NUMBERS ARE WRITTEN; scs_rx.h aliases them. ------------- */
#define SCS_ENV_OFF_INNER_LEN   42
#define SCS_ENV_OFF_FORMAT      44
#define SCS_ENV_OFF_MTYPE       46
#define SCS_ENV_OFF_CREDIT      48
#define SCS_ENV_OFF_DEST_CONID  50
#define SCS_ENV_OFF_SRC_CONID   54

/* The SYSAP payload begins here. The 58-content class -- envelope and nothing
 * else -- has inner length 14, which is exactly [44:58]. */
#define SCS_ENV_HDR_END         58

/* [44:46] on every envelope-conformant frame in the corpus. */
#define SCS_ENV_FORMAT_WORD     0x0004u

/* inner_len == total_sca_len - SCS_ENV_INNER_LEN_BIAS. The bias is the offset
 * the inner length is measured FROM, i.e. SCS_ENV_OFF_FORMAT. */
#define SCS_ENV_INNER_LEN_BIAS  44

/* Ethernet header ahead of the SCA content, for the frame-relative helpers. */
#define SCS_ENV_ETH_HDR_LEN     14

/*
 * The MTYPE namespace, {0..10}. 0..7 are the connection-control messages of
 * Figures 2-14/2-15/2-16 (spec sec 4(h)(1a)); the mapping onto the connection
 * state machine's events lives in scs_conn_event_for_msgtype() and is NOT
 * duplicated here. 8 and 9 are observed, paired, and UNIDENTIFIED (vms-f03).
 */
#define SCS_ENV_MTYPE_CONNECT_REQ     0u
#define SCS_ENV_MTYPE_CONNECT_RSP     1u
#define SCS_ENV_MTYPE_ACCEPT_REQ      2u
#define SCS_ENV_MTYPE_ACCEPT_RSP      3u
#define SCS_ENV_MTYPE_REJECT_REQ      4u
#define SCS_ENV_MTYPE_REJECT_RSP      5u
#define SCS_ENV_MTYPE_DISCONNECT_REQ  6u
#define SCS_ENV_MTYPE_DISCONNECT_RSP  7u
#define SCS_ENV_MTYPE_T8              8u  /* observed, UNNAMED -- vms-f03 */
#define SCS_ENV_MTYPE_T9              9u  /* observed, UNNAMED -- vms-f03 */
#define SCS_ENV_MTYPE_APP_MESSAGE    10u  /* p. 4-13 application message */

/* The highest MTYPE that is a connection-control message, and the highest that
 * has ever been observed at all. */
#define SCS_ENV_MTYPE_CONTROL_MAX     9u
#define SCS_ENV_MTYPE_MAX_OBSERVED   10u

/*
 * enum scs_env_route - THE p. 4-15 DISPATCH DECISION, and the only one this
 * module makes. It says WHERE a frame goes, never what it means.
 */
enum scs_env_route {
    /* MTYPE 0..9: an SCA connection-control message. Consumed by the
     * connection state machine (scs_conn.h); never delivered to a SYSAP. */
    SCS_ENV_ROUTE_CONTROL = 0,
    /* MTYPE 10: p. 4-15 -- "the message input routine in the CDT for the
     * connection named by the destination CONID" (scs_cdl_deliver_message). */
    SCS_ENV_ROUTE_MESSAGE = 1,
    /* Anything else. Never observed. Counted, never guessed at -- and in
     * particular NEVER assumed to be an application datagram. */
    SCS_ENV_ROUTE_UNKNOWN = 2
};

/*
 * struct scs_env - the decoded envelope of one received frame.
 */
struct scs_env {
    uint16_t total_sca_len; /* LE16(content[0:2]) + 2 */
    uint16_t inner_len;     /* [42:44] -- verified == total_sca_len - 44 */
    uint16_t mtype;         /* [46:48] */
    uint16_t credit;        /* [48:50] */
    uint32_t dest_conid;    /* [50:54] -- indexes OUR CDL (p. 2-29) */
    uint32_t src_conid;     /* [54:58] -- the sender's own handle */
    const uint8_t *payload; /* content + 58, or NULL when payload_len == 0 */
    size_t   payload_len;   /* total_sca_len - 58 */
    int      route;         /* enum scs_env_route */
};

/*
 * struct scs_env_fields - the envelope a builder is putting ON the wire.
 *
 * Note what is NOT here: the inner length. It is DERIVED from the class length
 * by scs_env_build() and can never be copied out of a template, which is the
 * one thing every captured template got right and every hand-written copy of
 * the store could get wrong. The format word is likewise a constant, not an
 * input.
 *
 * Every remaining field is EXPLICIT, and that is the point of the item: an OVMX
 * builder must now be able to say which SCS message type it is emitting and how
 * much credit it is extending. Where a value is a labeled REPLAY of a captured
 * template's byte, the builder names it as a constant and says so.
 */
struct scs_env_fields {
    uint16_t mtype;
    uint16_t credit;
    uint32_t dest_conid;
    uint32_t src_conid;
};

/*
 * scs_env_build - write the envelope into `content` (SCA offset 0) for a
 * message whose total SCA content length is `sca_len`.
 *
 * Writes exactly six fields at [42:58] and touches nothing else, so a caller
 * that has already laid down a captured template keeps every other byte of it.
 * Returns 0, or -1 if content is NULL, f is NULL, sca_len < SCS_ENV_HDR_END or
 * sca_len > 0xFFFF (the [0:2] SCA length field is a u16).
 *
 * It does NOT write the SCA length field at [0:2]: that belongs to the frame
 * class, not to the SCS envelope, and several classes carry a value the wire
 * grounds independently. scs_env_build_frame() is the same call addressed from
 * absolute frame offset 0.
 */
int scs_env_build(uint8_t *content, size_t sca_len,
                  const struct scs_env_fields *f);
int scs_env_build_frame(uint8_t *frame, size_t frame_len,
                        const struct scs_env_fields *f);

/*
 * scs_env_parse - decode and CONFORMANCE-TEST the envelope of one frame.
 *
 * `content` points at SCA offset 0 and `len` is the number of readable bytes
 * there. Returns 0 and fills *out only when the frame is envelope-conformant:
 *
 *   - at least SCS_ENV_HDR_END readable bytes, and
 *   - the declared total SCA content fits within `len`, and
 *   - content[44:46] == SCS_ENV_FORMAT_WORD, and
 *   - content[42:44] == total_sca_len - SCS_ENV_INNER_LEN_BIAS.
 *
 * Returns -1 otherwise, leaving *out untouched. A -1 is NOT an error -- see the
 * "what is not an SCS message envelope" list in the header comment.
 *
 * NOTHING here validates the Con.IDs. Whether a destination Con.ID names one of
 * OUR connections is scs_cdl_lookup()'s decision and it is the only place that
 * may index the CDL: the low 16 bits of a PEER-SUPPLIED value select a slot, so
 * the bound check, the slot-in-use check and the full-32-bit identity check all
 * belong to the lookup, not to this parser.
 */
int scs_env_parse(const uint8_t *content, size_t len, struct scs_env *out);
int scs_env_parse_frame(const uint8_t *frame, size_t len, struct scs_env *out);

/*
 * scs_env_mtype_of_frame - the SCS message type of an envelope-conformant
 * frame, addressed from absolute offset 0. Returns 1 and stores *out, or 0 if
 * the frame is not envelope-conformant.
 *
 * This exists so that no call site anywhere re-derives `buf[60] | buf[61] << 8`
 * for itself. Those open-coded reads were not merely duplication: they read the
 * MTYPE offset out of frames that had never been envelope-tested.
 */
int scs_env_mtype_of_frame(const uint8_t *frame, size_t len, uint16_t *out);

/*
 * scs_env_route_for_mtype - the p. 4-15 dispatch decision for a bare MTYPE.
 * Total; never fails.
 */
int scs_env_route_for_mtype(unsigned mtype);

/*
 * scs_env_mtype_name - a static, never-NULL name for logs. The names of 0..7
 * are spec sec 4(h)(1a)'s; 8 and 9 render as "type 8"/"type 9" because they are
 * NOT identified and naming them in a log would leak a guess into the record.
 */
const char *scs_env_mtype_name(unsigned mtype);

/* Static, non-NULL name for an enum scs_env_route value. */
const char *scs_env_route_name(int route);

#ifdef __cplusplus
}
#endif

#endif /* SCS_ENV_H */
