/*
 * scs_mscp.h - MSCP-over-SCS disk-client command builders (vms-760).
 *
 * THE GAP THIS CLOSES. Reaching SHOW CLUSTER status NEW as a pure server is not
 * enough for an ESTABLISHED VAX1 to promote OVMX to MEMBER: a real joiner is
 * ALSO a disk CLIENT. After NEW it opens its OWN SCS$DIRECTORY + MSCP$DISK
 * connections BACK to VAX1 and runs MSCP disk discovery -- SET CONTROLLER
 * CHARACTERISTICS then a GET UNIT STATUS enumeration -- proving it can reach the
 * cluster system disk. VAX1 will not admit a member that cannot. This module
 * builds the two MSCP command frames OVMX (the disk client) must SEND on its own
 * MSCP$DISK SCS connection.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8). MSCP (Mass Storage Control Protocol)
 * is a PUBLICLY documented DEC protocol. The field map, offsets, opcodes, command
 * modifiers, end-message flags and status/event codes below are transcribed from
 * the *MSCP Basic Disk Functions Manual* AA-L619A-TK v1.2 (Apr 1982), part of the
 * customer-orderable UDA50 Programmer's Doc Kit QP-905-GZ -- standard copyright
 * page, no confidential or restricted-distribution marking, so admissible public
 * documentation. Sections and tables are cited inline.
 * ⛔ EXCLUDED and NOT read: the bitsavers `dec/dsa/mscp` v2.4.0 / TMSCP 2.0.2
 * files, which are stamped DEC CONFIDENTIAL AND PROPRIETARY / RESTRICTED
 * DISTRIBUTION. No VSI/HPE source or binary was read, disassembled, or copied.
 *
 * THE CONFORMANCE ORACLE is the reference-lab wire: the golden JOINER command
 * frames of af2-firsttimer-established-20260728.pcap (112 frames, a first-timer
 * joining an established VAX1) and VAX1's END responses to them. Every numeric
 * OPCODE this header defines is BOTH published in Table A-1 AND observed on that
 * wire; Table A-1 lists nineteen opcodes and this header deliberately defines
 * only the ones the captures confirm (vms-533 / epic vms-600 Rule-8 discipline).
 *
 * ============ WHAT CHANGED IN vms-533 (MSCP epic Phase B) ==================
 *
 * The two commands used to be 94-byte byte-exact SCA-content TEMPLATES with four
 * fields punched into them. The MSCP body is now BUILT FROM FIELDS -- command
 * reference number, unit, opcode, modifiers and the per-opcode parameter area are
 * every one of them a named struct member laid down at a Table A-6 offset, and
 * NOT ONE byte of the 36-byte MSCP body is copied from a capture any more. What
 * remains a labeled REPLAY is the 58-byte SCA/PPD header ahead of the body: the
 * still-undecoded PPD/NISCA fields of the 0x4b class ([8:10], [16:18], [24:26],
 * [36:42]) are not this module's layer, and Phase A (vms-ec7) already promoted
 * the SCS envelope inside it. The change is WIRE-NEUTRAL by construction and the
 * unit test proves it: the field-built frames are byte-identical to the golden
 * af2 captures.
 *
 * THE SCS ENVELOPE is identical to the other 0x4b sequenced-application classes
 * (spec sec 4h): msgtype 0x4b / format 0x13 at [16:18], recv_ack [18:20] mirrored
 * at [26:28]/[34:36], send_seq [20:22] mirrored at [30:32], incarnation echo
 * [22:24], the Con.ID pair at [50:54]/[54:58], and the MSCP body starting at SCA
 * offset 58 (abs frame offset 72).
 *
 * THE MSCP MESSAGE that rides that body is AA-L619A-TK sec 5.1 (command) and
 * sec 5.5 (end message) -- one 12-byte header, then a parameter area:
 *
 *   body[0:4]   P.CRF  command reference number (u32; sec 5.1: unique, non-zero,
 *                      opaque, echoed verbatim into the end message)
 *   body[4:6]   P.UNIT unit number
 *   body[6:8]   reserved -- sec 5.2: a class driver MUST supply 0
 *   body[8]     P.OPCD opcode; an END message's "endcode" is command | OP.END
 *   body[9]     reserved in a command / P.FLGS end-message flags in an end message
 *   body[10:12] P.MOD  modifiers in a command / P.STS status in an end message
 *   body[12:..] opcode-specific parameter area (Table A-6 / Table A-7)
 */
#ifndef SCS_MSCP_H
#define SCS_MSCP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MSCP-over-SCS disk-client command frame: 94 SCA-content bytes + 14-byte
 * Ethernet header (a 108-byte wire frame). */
#define SCS_MSCP_SCA_LEN    94
#define SCS_MSCP_FRAME_LEN  108

/* SCA-content offset of the MSCP body (body[0] = SCA offset 58 = abs 72). */
#define SCS_MSCP_BODY_OFF   58

/* SCS envelope constants (shared with the other 0x4b classes).
 *
 * ⚠ NAMING TRAP, and it is the reason this comment exists: SCS_MSCP_MSGTYPE is
 * the PPD/NISCA marker byte at content [16], NOT the SCS message type. The SCS
 * MTYPE is the LE u16 at content [46:48] and it is 10 on this class -- the
 * p. 4-13 APPLICATION MESSAGE (docs/design-mscp-direction.md sec 1.2, which
 * identifies these very golden frames as MTYPE 10 with credit 1). Since vms-ec7
 * the builder writes it through scs_env_build_frame() as
 * SCS_ENV_MTYPE_APP_MESSAGE; do not add a second spelling of it here. */
#define SCS_MSCP_MSGTYPE    0x4b
#define SCS_MSCP_FORMAT     0x13

/* The credit field ([48:50]) of the golden af2 joiner MSCP commands. A labeled
 * REPLAY, not a computed extension -- see the note at the build site. */
#define SCS_MSCP_ENV_CREDIT 1u

/* ---- MSCP message-body offsets, AA-L619A-TK Table A-6 (command) and Table A-7
 * (end message). Body-relative, i.e. from SCA content[58] / abs frame[72]. THIS
 * IS THE ONLY PLACE IN THE TREE THESE NUMBERS ARE WRITTEN. --------------- */
#define SCS_MSCP_P_CRF   0  /* command reference number, 4 bytes */
#define SCS_MSCP_P_UNIT  4  /* unit number, 2 */
#define SCS_MSCP_P_RSVD6 6  /* reserved, 2 -- sec 5.2: the host supplies 0 */
#define SCS_MSCP_P_OPCD  8  /* opcode / endcode, 1 */
#define SCS_MSCP_P_FLGS  9  /* end-message flags, 1 (reserved in a command) */
#define SCS_MSCP_P_MOD   10 /* command modifiers, 2 */
#define SCS_MSCP_P_STS   10 /* end-message status, 2 -- the SAME word as P.MOD */
#define SCS_MSCP_HDR_LEN 12 /* sec 5.1: the generic header is 12 bytes */

/* TRANSFER command parameter-area offsets (Table A-6, sec 5.3) -- the layout
 * READ and WRITE share. Defined here rather than beside the server responder
 * that reads them, because these are COMMAND offsets and the note above makes
 * this header the one place in the tree they are written. */
#define SCS_MSCP_P_BCNT 12 /* byte count, 4 -- must be a whole number of blocks */
#define SCS_MSCP_P_BUFF 16 /* buffer descriptor, 12 -- names the HOST buffer the
                            * data must cross into (Appendix D) */
#define SCS_MSCP_P_LBN  28 /* logical block number, 4 */

/* The MSCP body carried by this 94-content frame class: 12-byte header + a
 * 24-byte parameter area (sec 5.1 allows up to 36 bytes of parameters). */
#define SCS_MSCP_BODY_LEN (SCS_MSCP_SCA_LEN - SCS_MSCP_BODY_OFF) /* 36 */

/* SET CONTROLLER CHARACTERISTICS parameter-area offsets (Table A-6, sec 6.16). */
#define SCS_MSCP_P_VRSN 12 /* MSCP version, 2 -- sec 6.16: the host supplies 0 */
#define SCS_MSCP_P_CNTF 14 /* controller flags, 2 (Table A-4) */
#define SCS_MSCP_P_HTMO 16 /* host timeout in seconds, 2 */
#define SCS_MSCP_P_TIME 20 /* quad-word time and date, 8 */

/* MSCP opcodes (P.OPCD) and the end-message flag, AA-L619A-TK Table A-1.
 *
 * ⚠ RULE 8 SCOPE. Table A-1 publishes nineteen opcodes; only the five below plus
 * OP.END are ALSO confirmed on the reference-lab wire, and only those are defined
 * here. docs/design-mscp-direction.md sec 2 records the cross-check: on the
 * VMS$DISK_CL_DRVR (MSCP client) connections of the 94-content type-10
 * population, 5245 of 5245 frames decode to a valid Table A-1 opcode at body[8],
 * and only 0x03 / 0x04 appear. Do not add an opcode here from the book alone --
 * add it when a capture shows it, and say which capture. */
#define SCS_MSCP_OP_GET_UNIT_STATUS 0x03 /* OP.GUS */
#define SCS_MSCP_OP_SET_CTLR_CHAR   0x04 /* OP.SCC */
#define SCS_MSCP_OP_ONLINE          0x09 /* OP.ONL -- not emitted by this client */
#define SCS_MSCP_OP_READ            0x21 /* OP.RD  -- Phase D (vms-291), not here */
#define SCS_MSCP_OP_WRITE           0x22 /* OP.WR  -- Phase D (vms-291), not here */
#define SCS_MSCP_END_BIT            0x80 /* OP.END: endcode = command | OP.END */
#define SCS_MSCP_OPCODE_MASK        0x7f /* strip OP.END to get the command back */

/* MSCP status/event codes, AA-L619A-TK sec 5.6 + Table B-1. The 16-bit status
 * word is NOT a flat code: bits 0-4 are the major status code (all controllers
 * must agree on it) and bits 5-15 are an 11-bit sub-code carrying fine detail.
 *
 * ⚠ THIS IS WHY THE WORD MUST NOT BE COMPARED WHOLE. "Unit-Offline" is major
 * code 3, but Table B-2 publishes Unit-Offline sub-codes 1, 2, 4 and 8 -- e.g.
 * 0x0023 is "Unit-Offline, no volume mounted or drive disabled via the RUN/STOP
 * switch". A `status == 3` test sees none of those as offline. Use
 * scs_mscp_status_major() (or struct scs_mscp_view's decoded status_major). */
#define SCS_MSCP_ST_MASK      0x001Fu /* ST.MSK -- the 5-bit major code */
#define SCS_MSCP_ST_SUB_SHIFT 5       /* ST.SUB == 32 == 1 << 5 */

/* Table B-1 major status/event codes. Capture-confirmed on the af2 wire:
 * 0 (SUCCESS, every SCC-END) and 4 (AVAILABLE, the GUS walk's unit answers). */
#define SCS_MSCP_ST_SUCCESS       0u  /* ST.SUC */
#define SCS_MSCP_ST_INVALID_CMD   1u  /* ST.CMD */
#define SCS_MSCP_ST_ABORTED       2u  /* ST.ABO */
#define SCS_MSCP_ST_OFFLINE       3u  /* ST.OFL -- GUS-walk end-of-list terminator */
#define SCS_MSCP_ST_AVAILABLE     4u  /* ST.AVL -- a real disk unit */
#define SCS_MSCP_ST_MEDIA_FMT_ERR 5u  /* ST.MFE */
#define SCS_MSCP_ST_WRITE_PROT    6u  /* ST.WPR */
#define SCS_MSCP_ST_COMPARE_ERR   7u  /* ST.CMP */
#define SCS_MSCP_ST_DATA_ERR      8u  /* ST.DAT */
#define SCS_MSCP_ST_HOST_BUF_ERR  9u  /* ST.HST */
#define SCS_MSCP_ST_CTLR_ERR      10u /* ST.CNT */
#define SCS_MSCP_ST_DRIVE_ERR     11u /* ST.DRV */
#define SCS_MSCP_ST_DIAGNOSTIC    31u /* ST.DIA -- from an internal diagnostic */

/* End-message flags (P.FLGS), AA-L619A-TK Table A-3. Disjoint from success or
 * failure: they report conditions alongside the status. */
#define SCS_MSCP_EF_BAD_BLOCK_REPORTED   0x80 /* EF.BBR */
#define SCS_MSCP_EF_BAD_BLOCK_UNREPORTED 0x40 /* EF.BBU */
#define SCS_MSCP_EF_ERROR_LOG_GENERATED  0x20 /* EF.LOG */

/* Command modifiers (P.MOD), AA-L619A-TK Table A-2. Only the one this client
 * actually sets is defined; the generic modifiers are transfer-command concerns
 * and belong with the code that would use them. */
#define SCS_MSCP_MOD_NEXT_UNIT 0x0001 /* MD.NXU (GET UNIT STATUS): return the
                                       * next known unit >= the one named */

/* Controller flags (P.CNTF), AA-L619A-TK Table A-4 -- host-settable via SET
 * CONTROLLER CHARACTERISTICS, all clear by default. */
#define SCS_MSCP_CF_ATTN_MSGS   0x0080u /* CF.ATN enable attention messages */
#define SCS_MSCP_CF_MISC_ERRLOG 0x0040u /* CF.MSC enable misc error-log messages */
#define SCS_MSCP_CF_OTHER_HOSTS 0x0020u /* CF.OTH enable other hosts' error logs */
#define SCS_MSCP_CF_THIS_HOST   0x0010u /* CF.THS enable this host's error logs */
#define SCS_MSCP_CF_576_SECTORS 0x0001u /* CF.576 576-byte sectors */

/* ---- The SET CONTROLLER CHARACTERISTICS parameter values OVMX sends. Each was
 * a byte in the captured template and is now a NAMED value with its provenance
 * stated; scs_mscp_scc_defaults() loads them. -------------------------------- */

/* sec 6.16: the host MUST supply 0; a non-zero version is answered Invalid
 * Command. Not a replay -- the spec fixes this value. */
#define SCS_MSCP_SCC_VERSION_HOST 0x0000u

/* CF.ATN | CF.MSC | CF.THS. The af2 joiner's choice, READ OFF the golden SCC
 * command's P.CNTF and decoded against Table A-4 -- OVMX keeps it so its SCC is
 * the same request a real VMS disk class driver makes. */
#define SCS_MSCP_SCC_CTLR_FLAGS \
    (SCS_MSCP_CF_ATTN_MSGS | SCS_MSCP_CF_MISC_ERRLOG | SCS_MSCP_CF_THIS_HOST)

/* P.HTMO 0 == "host-access timeout disabled" (sec 6.16). The golden SCC's value. */
#define SCS_MSCP_SCC_HOST_TIMEOUT 0x0000u

/* ⚠ P.TIME: A FROZEN CAPTURE TIMESTAMP, and it is a labeled REPLAY, not a
 * design choice. sec 6.16 defines this quadword as VAX/VMS time -- 100 ns clunks
 * since 00:00 17-Nov-1858 -- "or 0 if unavailable". This constant decodes to
 * 2026-07-28 12:59:58.46 UTC, which is the wall-clock instant the golden af2
 * capture was taken; a live client sends the CURRENT time. Emitting it keeps
 * this Phase-B refactor wire-neutral, which is the whole point of the change;
 * replacing it with live host time is a WIRE change that wants a lab join to
 * confirm, and is filed as follow-up work, not smuggled in here. */
#define SCS_MSCP_SCC_TIME_AF2 ((uint64_t)0x00bc021975280bc0ULL)

/* GROUNDED seed tokens from af2-firsttimer-established.pcap. sec 5.1 makes the
 * whole 32-bit P.CRF opaque -- "unique, non-zero", echoed verbatim, unique only
 * across the commands outstanding on one connection. The low-word/high-word
 * split below is therefore OBSERVED VMS behaviour, not a protocol requirement:
 * the captured joiner puts a per-command-class constant in the low word and an
 * incrementing message-id in the high word. OVMX reproduces the observation. */
#define SCS_MSCP_SCC_CLASS   0x0002u
#define SCS_MSCP_GUS_CLASS   0x0001u
#define SCS_MSCP_SCC_MSGID0  0x81a3u
#define SCS_MSCP_GUS_MSGID0  0x7ee2u

/* Compose a P.CRF from the observed (class token, message id) pair. */
#define SCS_MSCP_CMD_REF(class_token, msg_id) \
    ((((uint32_t)(uint16_t)(msg_id)) << 16) | (uint32_t)(uint16_t)(class_token))

/*
 * scs_mscp_cmd - THE MSCP COMMAND MESSAGE, field by field (sec 5.1, Table A-6).
 *
 * This is the struct that replaced the byte-replayed body. Nothing in it is an
 * offset, a template index or a captured byte: the builder lays each member down
 * at its Table A-6 offset and zero-fills everything the tables call reserved.
 */
struct scs_mscp_cmd {
    uint32_t cmd_ref;   /* P.CRF  -- unique, non-zero (sec 5.1) */
    uint16_t unit;      /* P.UNIT -- binary unit select number */
    uint8_t  opcode;    /* P.OPCD -- Table A-1, and OP.END must NOT be set */
    uint16_t modifiers; /* P.MOD  -- Table A-2 */

    /* SET CONTROLLER CHARACTERISTICS parameter area (Table A-6, sec 6.16).
     * Ignored for every other opcode -- those parameter areas are zero on the
     * two commands this client sends. */
    uint16_t scc_version;      /* P.VRSN */
    uint16_t scc_ctlr_flags;   /* P.CNTF */
    uint16_t scc_host_timeout; /* P.HTMO */
    uint64_t scc_time;         /* P.TIME, VMS quadword time (0 = unavailable) */
};

/*
 * scs_mscp_params - the SCS/identity half of one MSCP disk-client command frame.
 * The MSCP message itself is struct scs_mscp_cmd.
 */
struct scs_mscp_params {
    uint8_t  dst_mac[6];      /* Ethernet dst = member's Ethernet src MAC */
    uint8_t  src_mac[6];      /* Ethernet src (abs 6) = OVMX HW MAC */
    uint8_t  src_logical[6];  /* SCA src-logical [10:16] (abs 24) = aa:00:04:00:<LE16(sysid)> */
    uint8_t  peer_logical[6]; /* SCA dest-logical [2:8] = member's advertised logical addr */
    uint32_t remote_conid;    /* [50:54] member's MSCP$DISK server Con.ID */
    uint32_t local_conid;     /* [54:58] OVMX's MSCP$DISK client Con.ID */
    uint16_t recv_ack;        /* SCS recv_seq (envelope [18:20]/[26:28]/[34:36]) */
    uint16_t send_seq;        /* SCS send_seq (envelope [20:22]/[30:32]) */
    uint16_t incarnation;     /* envelope [22:24]; 0 => the fresh-contact value 1 */
    uint32_t cmd_ref;         /* P.CRF -- see SCS_MSCP_CMD_REF() */
    uint16_t unit;            /* P.UNIT (GUS enumeration unit; 0 for SCC) */
};

/*
 * scs_mscp_build_body - lay one MSCP command message out at its Table A-6
 * offsets, into `body` (body_len must be >= SCS_MSCP_BODY_LEN). Zero-fills the
 * whole body first, so every reserved field and the unused tail of the parameter
 * area are 0 as sec 5.2 requires of a class driver. Returns 0, or -1 on NULL
 * args, a short buffer, a zero cmd_ref (sec 5.1 requires non-zero), or an opcode
 * with OP.END set (that would make it an end message, and this client sends
 * commands only -- serving is Phase D / vms-291).
 *
 * Exposed because it is the layer under both builders and the thing the unit
 * test can drive one field at a time.
 */
int scs_mscp_build_body(const struct scs_mscp_cmd *c, uint8_t *body,
                        size_t body_len);

/*
 * scs_mscp_scc_defaults / scs_mscp_gus_defaults - fill *c with the command OVMX
 * sends for SET CONTROLLER CHARACTERISTICS / GET UNIT STATUS: opcode, modifiers
 * and (for SCC) the sec 6.16 parameter values named above. `cmd_ref` and `unit`
 * come from the caller. Returns 0, or -1 if c is NULL.
 */
int scs_mscp_scc_defaults(struct scs_mscp_cmd *c, uint32_t cmd_ref);
int scs_mscp_gus_defaults(struct scs_mscp_cmd *c, uint32_t cmd_ref, uint16_t unit);

/*
 * scs_mscp_build_command - build a complete Ethernet+SCA frame carrying `c` on
 * the MSCP$DISK connection described by `p`. THE build path: scs_mscp_build_scc()
 * and scs_mscp_build_gus() are wrappers that call the *_defaults() above.
 * Returns 0, or -1 on NULL args or a command scs_mscp_build_body() rejects.
 */
int scs_mscp_build_command(const struct scs_mscp_params *p,
                           const struct scs_mscp_cmd *c,
                           uint8_t out[SCS_MSCP_FRAME_LEN]);

/*
 * scs_mscp_build_scc - build the SET CONTROLLER CHARACTERISTICS command
 * (OP.SCC) OVMX sends as the FIRST MSCP message on its disk-client connection.
 * scs_mscp_build_gus - build a GET UNIT STATUS command (OP.GUS, MD.NXU) for
 * p->unit. Each fills out[SCS_MSCP_FRAME_LEN] with a complete Ethernet+SCA
 * frame. Returns 0, or -1 on NULL args.
 */
int scs_mscp_build_scc(const struct scs_mscp_params *p,
                       uint8_t out[SCS_MSCP_FRAME_LEN]);
int scs_mscp_build_gus(const struct scs_mscp_params *p,
                       uint8_t out[SCS_MSCP_FRAME_LEN]);

/*
 * scs_mscp_status_major / scs_mscp_status_subcode - the sec 5.6 split of a
 * 16-bit MSCP status word. Total; never fail.
 * scs_mscp_status_name - a static, never-NULL Table B-1 name for a MAJOR code
 * (not for a whole status word). scs_mscp_opcode_name - likewise for an opcode
 * or endcode, over the capture-confirmed Table A-1 subset this header defines.
 */
unsigned scs_mscp_status_major(uint16_t status);
unsigned scs_mscp_status_subcode(uint16_t status);
const char *scs_mscp_status_name(unsigned major);
const char *scs_mscp_opcode_name(uint8_t opcode);

/*
 * scs_mscp_view - decoded view of a received MSCP-over-SCS frame (command or
 * END response). The caller matches the connection by the Con.ID pair.
 */
struct scs_mscp_view {
    uint16_t total_sca_len;
    /* vms-ec7: the SCS envelope's own two value fields, decoded through the
     * shared path. scs_mtype is the [46:48] SCS message type (10 = the p. 4-13
     * application message on every MSCP frame we hold); credit is [48:50]. */
    uint16_t scs_mtype;      /* [46:48] */
    uint16_t credit;         /* [48:50] */
    uint8_t  msgtype;        /* [16] -- the PPD marker byte, NOT the SCS MTYPE */
    uint8_t  format;         /* [17] */
    uint16_t recv_ack;       /* [18:20] */
    uint16_t send_seq;       /* [20:22] */
    uint32_t remote_conid;   /* [50:54] */
    uint32_t local_conid;    /* [54:58] */

    /* --- the MSCP message, sec 5.1 / sec 5.5 --- */
    uint32_t cmd_ref;        /* P.CRF  body[0:4] -- echoed back by the controller */
    uint16_t unit;           /* P.UNIT body[4:6] (END: the returned unit-word) */
    uint8_t  opcode;         /* P.OPCD body[8] AS RECEIVED (an END carries OP.END) */
    uint8_t  base_opcode;    /* opcode & ~OP.END -- the command it answers */
    int      is_end;         /* opcode & OP.END */

    /* body[9] and body[10:12] mean DIFFERENT THINGS in the two directions
     * (sec 5.1): a command carries reserved + modifiers, an end message carries
     * flags + status. Only the pair selected by is_end is filled; the other is 0.
     * Reading a status out of a command message is the exact category error this
     * split exists to stop. */
    uint16_t modifiers;      /* P.MOD  body[10:12], commands only (Table A-2) */
    uint8_t  end_flags;      /* P.FLGS body[9],     end messages only (Table A-3) */
    uint16_t status;         /* P.STS  body[10:12], end messages only, RAW */
    uint8_t  status_major;   /* sec 5.6: status & ST.MSK -- compare AGAINST THIS */
    uint16_t status_subcode; /* sec 5.6: status >> 5 */
};

/*
 * scs_mscp_parse - classify a received frame as an MSCP-over-SCS message and
 * fill *v. Returns 0 on a well-formed frame that holds the whole sec 5.1
 * 12-byte MSCP header (>= 84 bytes: 14 Ethernet + 58 SCA header + 12), -1
 * otherwise. Does NOT check the Con.ID pair -- the caller matches the
 * connection.
 */
int scs_mscp_parse(const uint8_t *frame, size_t len, struct scs_mscp_view *v);

#ifdef __cplusplus
}
#endif

#endif /* SCS_MSCP_H */
