/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_codec_mscp.h - MSCP-over-SCS (MSCP$DISK SYSAP) typed codec
 * entries (plan item FC-P6.2; design
 * docs/design-faithful-cluster-executive.md P6 "MSCP disk serving").
 *
 * SCOPE. The MSCP command and end-message BODIES that ride the MSCP$DISK
 * connection: SET CONTROLLER CHARACTERISTICS (SCC), GET UNIT STATUS (GUS),
 * ONLINE, READ and WRITE, at the wire's actual, MEASURED lengths -- not
 * Table A-7's on-paper ones, which two of these classes silently disagree
 * with (see below). This item builds/parses the BODY only (SYSAP body
 * span, abs 72..); it also ships a minimal, self-contained abs[0,72) link
 * builder (`struct vms_mscp_link`, the same division of labour
 * vms_cluster_codec_cm.h's `vms_cm_link` already established) because this
 * item is blocked-by ONLY FC-P0.6, not the later VC/SCS envelope items.
 * Serving STATE (units/hosts/backing store/dispatch) is FC-P6.3's
 * `vms_mscp_srv.c`, not this file -- this is a pure codec, like every
 * sibling.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8). MSCP (Mass Storage Control
 * Protocol) is publicly documented DEC protocol: the *MSCP Basic Disk
 * Functions Manual* AA-L619A-TK v1.2 (Apr 1982), part of the
 * customer-orderable UDA50 Programmer's Doc Kit QP-905-GZ -- standard
 * copyright page, no confidential or restricted-distribution marking,
 * therefore admissible clean-room source. Every offset and opcode below is
 * transcribed from that manual's Table A-1 (opcodes), Table A-2
 * (modifiers), Table A-3 (end-message flags), Table A-4 (controller
 * flags), Table A-5 (unit flags), Table A-6 (command layout), Table A-7
 * (end-message layout), Table B-1/B-2/B-3 (status/sub-codes) and sec 5.1/
 * 5.2/5.3/5.5/5.6/6.12/6.13/6.16. Nothing here was disassembled,
 * decompiled, or read from VSI/HPE source; the bitsavers `dec/dsa/mscp`
 * files (DEC CONFIDENTIAL AND PROPRIETARY / RESTRICTED DISTRIBUTION) were
 * not read.
 *
 * THE CONFORMANCE ORACLE, where this item goes beyond the book, is this
 * project's own prior clean-room capture work already in the tree
 * (src/vmsscs/include/scs_mscp.h, src/vmsscs/include/scs_mscp_srv.h --
 * reference ONLY, their wire-behaviour documentation and byte-exact golden
 * vectors, never their C code, per Rule 9's STRAWMAN posture): a real VAX's
 * MSCP server answers, censused over 489 lab pcaps and a dedicated vms-291
 * lab-2 serving capture (a real VAX serving a disk to another real VAX).
 * That capture is WHY the lengths below are measured, not merely quoted
 * from the manual:
 *
 *     SCC END      28 bytes   954/954 captured frames, byte-exact
 *     GUS END      52 bytes   18855/18855 captured frames -- Table A-7's
 *                             own arithmetic stops at 48; every real server
 *                             emits 52, including an RA81 on a different
 *                             controller from the RA92s the rest of the
 *                             corpus used
 *     ONLINE END   44 bytes   vms-291 lab-2 serving capture
 *     READ END     32 bytes   vms-291 lab-2 serving capture (matches
 *                             Table A-7's generic end arithmetic)
 *     WRITE END    36 bytes   vms-291 lab-2 serving capture -- four bytes
 *                             MORE than READ; the two are NOT the same
 *                             length and this codec does not assume so.
 *
 * The extra bytes GUS carries past Table A-7 (48..52) and WRITE carries
 * past READ (32..36) are REPRODUCED BY LENGTH ONLY: this codec zero-fills
 * them except for the one GUS tail word a real server writes on every
 * specimen (see VMS_MSCP_GUS_TAIL_OBSERVED below) -- INV-6, never an
 * invented value for a span nothing in the corpus explains.
 *
 * P.UNFL ECHO RULE (measured, vms-291 lab-2 capture). Table A-6 places a
 * unit-flags word in the ONLINE COMMAND at the SAME byte offset (14) Table
 * A-7 gives the ONLINE END's own P.UNFL -- and a real server's end message
 * ECHOES the host's command word, OR-ed with whatever flags the unit
 * itself always asserts (so a host cannot clear e.g. software write
 * protection by asking nicely for it). vms_mscp_online_unfl_compose()
 * below is that one derivation, pure and total; it does not know or care
 * WHERE either half came from -- that is FC-P6.3's job.
 *
 * STATUS MAJOR/SUB SPLIT (sec 5.6, Table B-1/B-2/B-3). The 16-bit P.STS
 * word is NOT a flat code: bits 0-4 are the major status (every controller
 * must agree on it) and bits 5-15 are an 11-bit sub-code. "Unit-Offline"
 * is major 3, but Table B-2 gives it sub-codes 1/2/4/8 -- a bare
 * `status == 3` test never matches any of them. vms_mscp_status_major()/
 * vms_mscp_status_subcode() are the mandatory split; VMS_MSCP_STATUS()
 * composes the two back into the wire word the spec's own note requires
 * ("(subcode * ST.SUB) + code").
 *
 * SELF-CONTAINED CLASSIFICATION, ON PURPOSE. The shared frame-class
 * registry (vms_cluster_codec.h sec 3) has no MSCP-specific row: every
 * class this file builds shares the format-0x13 sequenced-application
 * envelope (msgtype 0x4b) with the generic VMS_FCLS_SCS_SEQ catch-all, but
 * at SCA-content lengths (86/90/94/102/110) none of the existing rows
 * name, and the 94-byte length is AMBIGUOUS between an MSCP command and a
 * WRITE end message -- length alone cannot tell them apart (only the
 * opcode byte's END bit can, see vms_mscp_classify()). Widening the shared
 * registry's match-predicate machinery to add a byte-mask discriminator is
 * a change to the seam every other harvest item builds against, and this
 * item's own blocked-by list (P0.6 only) does not ask for it. So this file
 * does what vms_cluster_codec_cm.h did before VC/SCS landed: every parse
 * function still takes the caller's `fi` and still requires
 * fi->family==VMS_FFAM_SCS && fi->cls==VMS_FCLS_SCS_SEQ (the registry's
 * own, already-grounded confirmation that this is a valid format-0x13
 * sequenced-application frame) as a NECESSARY precondition, then resolves
 * the SPECIFIC MSCP class itself from the wire's own content length and
 * opcode byte -- redundant with the classifier on the SCS half (the same
 * "redundant with the classifier" discipline vms_hello_parse documents),
 * self-sufficient on the MSCP half.
 *
 * THE HONESTY RULE (INV-6). Every field that varies per command/response
 * (cmd_ref, unit, all status/flags/geometry/identity values) is
 * caller-supplied on BUILD, never a baked capture constant. The only bytes
 * this file bakes in are protocol-format constants the manual or the
 * capture census grounds as invariant across every specimen measured
 * (msgtype 0x4b/format 0x13, the reserved-must-be-zero spans sec 5.2
 * requires, and the two named "observed, not decoded" constants below,
 * each cited to the capture that grounds it).
 */
#ifndef OVMX_VMS_CLUSTER_CODEC_MSCP_H
#define OVMX_VMS_CLUSTER_CODEC_MSCP_H

#include "vms_cluster_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * sec 1  Frame-length constants (SCA content, and the Ethernet-frame
 * totals a builder actually emits). SCA-content offset of the MSCP body
 * is 58 (== VMS_OFF_SYSAP_BODY - VMS_ETH_HDR_LEN, spec/sec 5.1).
 * ------------------------------------------------------------------ */
#define VMS_MSCP_BODY_OFF_CONTENT 58u  /* SCA-content offset, body[0]      */

#define VMS_MSCP_CMD_BODY_LEN     36u  /* every command frame's body span  */
#define VMS_MSCP_CMD_SCA_LEN      (VMS_MSCP_BODY_OFF_CONTENT + VMS_MSCP_CMD_BODY_LEN) /* 94 */
#define VMS_MSCP_CMD_FRAME_LEN    (VMS_ETH_HDR_LEN + VMS_MSCP_CMD_SCA_LEN)

#define VMS_MSCP_SCC_END_LEN      28u  /* MEASURED, 954/954 captured       */
#define VMS_MSCP_ONLINE_END_LEN   44u  /* MEASURED, vms-291 lab-2 capture  */
#define VMS_MSCP_READ_END_LEN     32u  /* MEASURED, vms-291 lab-2 capture  */
#define VMS_MSCP_WRITE_END_LEN    36u  /* MEASURED, vms-291 lab-2 capture  */
#define VMS_MSCP_GUS_END_LEN      52u  /* MEASURED, 18855/18855 captured   */
#define VMS_MSCP_END_BODY_MAX     52u  /* the longest of the five above    */

#define VMS_MSCP_END_SCA_LEN(body_len) \
	((uint16_t)(VMS_MSCP_BODY_OFF_CONTENT + (body_len)))
#define VMS_MSCP_END_FRAME_LEN(body_len) \
	((uint32_t)(VMS_ETH_HDR_LEN + VMS_MSCP_END_SCA_LEN(body_len)))

/* ------------------------------------------------------------------ *
 * sec 2  The 12-byte generic header every command AND end message shares
 * (sec 5.1: P.CRF/P.UNIT/reserved/P.OPCD, then P.MOD in a command or
 * P.FLGS+P.STS in an end message -- the SAME word, different meaning).
 * ------------------------------------------------------------------ */
#define VMS_OFF_MSCP_CRF   (VMS_OFF_SYSAP_BODY + 0)   /* abs72, u32 LE     */
#define VMS_OFF_MSCP_UNIT  (VMS_OFF_SYSAP_BODY + 4)   /* abs76, u16 LE     */
#define VMS_OFF_MSCP_RSVD6 (VMS_OFF_SYSAP_BODY + 6)   /* abs78, MUST be 0  */
#define VMS_OFF_MSCP_OPCD  (VMS_OFF_SYSAP_BODY + 8)   /* abs80, u8         */
#define VMS_OFF_MSCP_FLGS  (VMS_OFF_SYSAP_BODY + 9)   /* abs81, END only   */
#define VMS_OFF_MSCP_MOD   (VMS_OFF_SYSAP_BODY + 10)  /* abs82, CMD, u16   */
#define VMS_OFF_MSCP_STS   (VMS_OFF_SYSAP_BODY + 10)  /* abs82, END, u16 -- SAME word as MOD */
#define VMS_MSCP_HDR_LEN   12u

/* Opcodes (Table A-1) this item's captures confirm; an endcode is
 * opcode | VMS_MSCP_END_BIT. */
#define VMS_MSCP_OP_GUS       0x03u  /* OP.GUS  GET UNIT STATUS           */
#define VMS_MSCP_OP_SCC       0x04u  /* OP.SCC  SET CONTROLLER CHARACTERISTICS */
#define VMS_MSCP_OP_ONLINE    0x09u  /* OP.ONL  ONLINE                    */
#define VMS_MSCP_OP_READ      0x21u  /* OP.RD   READ                      */
#define VMS_MSCP_OP_WRITE     0x22u  /* OP.WR   WRITE                     */
#define VMS_MSCP_END_BIT      0x80u  /* OP.END: endcode = command | this  */
#define VMS_MSCP_OPCODE_MASK  0x7fu  /* strip OP.END to recover the command*/

/* ------------------------------------------------------------------ *
 * sec 3  Status word (sec 5.6, Table B-1/B-2/B-3): NOT a flat code.
 * ------------------------------------------------------------------ */
#define VMS_MSCP_ST_MASK      0x001Fu  /* ST.MSK, the 5-bit major code    */
#define VMS_MSCP_ST_SUB_SHIFT 5u       /* ST.SUB == 1 << 5                */

/* Table B-1 major status/event codes this item's captures/tests use. */
enum vms_mscp_status_major {
	VMS_MSCP_ST_SUCCESS       = 0,  /* ST.SUC */
	VMS_MSCP_ST_INVALID_CMD   = 1,  /* ST.CMD */
	VMS_MSCP_ST_ABORTED       = 2,  /* ST.ABO */
	VMS_MSCP_ST_OFFLINE       = 3,  /* ST.OFL -- GUS-walk terminator      */
	VMS_MSCP_ST_AVAILABLE     = 4,  /* ST.AVL -- a real disk unit         */
	VMS_MSCP_ST_MEDIA_FMT_ERR = 5,  /* ST.MFE */
	VMS_MSCP_ST_WRITE_PROT    = 6,  /* ST.WPR */
	VMS_MSCP_ST_COMPARE_ERR   = 7,  /* ST.CMP */
	VMS_MSCP_ST_DATA_ERR      = 8,  /* ST.DAT */
	VMS_MSCP_ST_HOST_BUF_ERR  = 9,  /* ST.HST */
	VMS_MSCP_ST_CTLR_ERR      = 10, /* ST.CNT */
	VMS_MSCP_ST_DRIVE_ERR     = 11, /* ST.DRV */
	VMS_MSCP_ST_DIAGNOSTIC    = 31  /* ST.DIA */
};

/* Table B-2 "Success" / "Unit-Offline" / "Write Protected" sub-codes, and
 * Table B-3's Controller-Error sub-code 3 -- the ones the sibling
 * strawman's tests exercise and this file's own tests reproduce. */
#define VMS_MSCP_SUB_NORMAL         0u
#define VMS_MSCP_SUB_ALREADY_ONLINE 8u
#define VMS_MSCP_SUB_OFL_UNKNOWN    0u
#define VMS_MSCP_SUB_OFL_NO_VOLUME  1u
#define VMS_MSCP_SUB_WP_SOFTWARE    128u  /* -> composed word 0x1006      */
#define VMS_MSCP_SUB_WP_HARDWARE    256u  /* -> composed word 0x2006      */
#define VMS_MSCP_SUB_CNT_INCONSISTENT 3u

/* sec 5.6 note 1: "(subcode * ST.SUB) + code". */
#define VMS_MSCP_STATUS(major, subcode) \
	((uint16_t)((((uint16_t)(subcode)) << VMS_MSCP_ST_SUB_SHIFT) | \
		    ((uint16_t)(major) & VMS_MSCP_ST_MASK)))

/* The sec 5.6 split, total and never-failing. */
unsigned vms_mscp_status_major(uint16_t status);
unsigned vms_mscp_status_subcode(uint16_t status);

/* End-message flags (P.FLGS), Table A-3. */
#define VMS_MSCP_EF_BAD_BLOCK_REPORTED   0x80u  /* EF.BBR */
#define VMS_MSCP_EF_BAD_BLOCK_UNREPORTED 0x40u  /* EF.BBU */
#define VMS_MSCP_EF_ERROR_LOG_GENERATED  0x20u  /* EF.LOG */

/* Command modifiers (P.MOD), Table A-2. */
#define VMS_MSCP_MOD_NEXT_UNIT        0x0001u  /* MD.NXU (GUS)            */
#define VMS_MSCP_MOD_IGNORE_MEDIA_FMT 0x0002u  /* MD.IMF (ONLINE)         */
#define VMS_MSCP_MOD_SET_WRITE_PROT   0x0004u  /* MD.SWP (ONLINE)         */

/* Unit flags (P.UNFL), Table A-5 -- the subset this item's opcodes touch. */
#define VMS_MSCP_UF_COMPARE_READS  0x0001u  /* UF.CMR */
#define VMS_MSCP_UF_COMPARE_WRITES 0x0002u  /* UF.CMW */
#define VMS_MSCP_UF_576_SECTORS    0x0004u  /* UF.576 */
#define VMS_MSCP_UF_REMOVABLE      0x0080u  /* UF.RMV */
#define VMS_MSCP_UF_WRITE_PROT_SW  0x1000u  /* UF.WPS software write prot */
#define VMS_MSCP_UF_WRITE_PROT_HW  0x2000u  /* UF.WPH hardware write prot */

/* Controller flags (P.CNTF), Table A-4 -- host-settable via SCC. */
#define VMS_MSCP_CF_ATTN_MSGS   0x0080u  /* CF.ATN */
#define VMS_MSCP_CF_MISC_ERRLOG 0x0040u  /* CF.MSC */
#define VMS_MSCP_CF_OTHER_HOSTS 0x0020u  /* CF.OTH */
#define VMS_MSCP_CF_THIS_HOST   0x0010u  /* CF.THS */
#define VMS_MSCP_CF_576_SECTORS 0x0001u  /* CF.576 */

/*
 * vms_mscp_online_unfl_compose - THE P.UNFL ECHO RULE (measured, vms-291
 * lab-2 serving capture; see file header). A real server's ONLINE end
 * message ORs the host's ONLINE-command unit-flags word with the unit's
 * own non-host-settable flags, so a host cannot clear e.g. software write
 * protection by asking for it. Pure and total: this function does not
 * know where either half comes from (a live ONLINE command / a served
 * unit's own state, both FC-P6.3's concern), only the composition rule
 * the wire measured.
 */
uint16_t vms_mscp_online_unfl_compose(uint16_t host_unfl, uint16_t unit_flags);

/* ------------------------------------------------------------------ *
 * sec 4  The self-contained class check (see file header "SELF-CONTAINED
 * CLASSIFICATION"). Exposed so a caller (or this file's own tests) can
 * resolve which of the five classes a received frame is before picking
 * the matching parse function.
 * ------------------------------------------------------------------ */
enum vms_mscp_class {
	VMS_MSCP_CLS_UNKNOWN = 0,
	VMS_MSCP_CLS_CMD,        /* any command: SCC/GUS/ONLINE/READ/WRITE   */
	VMS_MSCP_CLS_SCC_END,
	VMS_MSCP_CLS_GUS_END,
	VMS_MSCP_CLS_ONLINE_END,
	VMS_MSCP_CLS_READ_END,
	VMS_MSCP_CLS_WRITE_END
};

/*
 * vms_mscp_classify - resolve *out from the wire's own content length and
 * opcode/END-bit byte. Requires fi->family==VMS_FFAM_SCS &&
 * fi->cls==VMS_FCLS_SCS_SEQ (the shared registry's own grounded
 * confirmation this is a valid format-0x13 sequenced-application frame --
 * see the file header) and, on top of that, resolves the SPECIFIC MSCP
 * class from content length plus the opcode byte's END bit (the only way
 * to break the 94-byte COMMAND vs WRITE-END-message ambiguity). Returns
 * VMS_CODEC_OK with VMS_MSCP_CLS_UNKNOWN (not an error) for a frame that
 * simply is not one of these five classes -- exactly vms_frame_classify's
 * own "unclassified is a legitimate, honest outcome" convention.
 */
vms_codec_status_t vms_mscp_classify(const uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     enum vms_mscp_class *out);

/* ------------------------------------------------------------------ *
 * sec 5  The minimal abs[0,72) link (mirrors vms_cm_link, see file header)
 * ------------------------------------------------------------------ */

/*
 * struct vms_mscp_link - the abs[0,72) span a builder needs: the shared
 * SCA header, the sequenced-message counters and their GROUNDED mirrors
 * (spec-grounded by src/vmsscs/include/scs_mscp.h's own envelope
 * description, cited there to the golden af2 captures: recv_ack@32
 * mirrored @40/@48, send_seq@34 mirrored @44), the SCS message-TYPE word
 * @60 (baked in as the GROUNDED constant 10, "the p. 4-13 APPLICATION
 * MESSAGE" -- scs_mscp.h's own citation; this is also what keeps a built
 * GUS-end frame, content 110, from colliding with the shared registry's
 * unrelated CONN_CTRL length class, which a zero at abs60 would have
 * matched), and the Con.ID pair @64/68.
 * credit@62 is the envelope's own live piggyback-credit field (scs_mscp.h
 * "the credit field of the golden af2 joiner MSCP commands"); this codec
 * threads it through as caller-supplied, never a silent default (INV-6) --
 * VMS_MSCP_ENV_CREDIT_OBSERVED below is a NAMED reference value for a
 * caller with no live credit source, exactly as scs_mscp.h documents for
 * its own SCS_MSCP_ENV_CREDIT constant, never baked in by this file.
 * Every other byte in abs[36,72) that this item's own sources do not
 * ground (the format word, the inner-length field the sibling SCS/P2.1
 * item -- not a dependency of this one -- documents for OTHER SCS
 * classes) is left an honest zero, exactly as vms_cm_link leaves its own
 * ungrounded span (see that header's "OUT OF SCOPE" note).
 */
struct vms_mscp_link {
	struct vms_sca_hdr hdr;   /* abs 0-31; hdr.word30 is IGNORED by the
				   * builder and overwritten with msgtype
				   * 0x4b / format 0x13                       */
	uint16_t recv_ack;        /* abs 32 (+ mirrors @40/@48)               */
	uint16_t send_seq;        /* abs 34 (+ mirror @44)                    */
	uint16_t credit;          /* abs 62, caller-supplied, see doc above   */
	uint32_t remote_conid;    /* abs 64: the PEER's own Con.ID            */
	uint32_t local_conid;     /* abs 68: OUR own Con.ID                   */
};

/* A NAMED reference value only -- see the struct doc comment. Not baked in
 * by any builder in this file. */
#define VMS_MSCP_ENV_CREDIT_OBSERVED 1u

/*
 * vms_mscp_link_build - write the abs[0,72) span into a buffer of at
 * least `sca_content_len` + 14 bytes. `sca_content_len` selects the frame
 * total (94 for a command, or one of the VMS_MSCP_*_END_LEN + 58 values
 * for an end message) -- the link itself does not know which body follows
 * it, so the caller (or one of sec 6/7's whole-frame builders below) names
 * it explicitly rather than this function guessing from body content.
 */
vms_codec_status_t vms_mscp_link_build(const struct vms_mscp_link *l,
				       uint16_t sca_content_len,
				       uint8_t *frame, uint32_t cap,
				       uint32_t *written);

/* ------------------------------------------------------------------ *
 * sec 6  Commands: the shared 12-byte header plus each opcode's own
 * Table A-6 parameter area.
 * ------------------------------------------------------------------ */

/* generic 12-byte command/end header, shared shape (sec 2 above documents
 * the offsets; this struct is the typed read/write of it). */
struct vms_mscp_hdr {
	uint32_t cmd_ref;   /* P.CRF -- sec 5.1: unique, non-zero, echoed    */
	uint16_t unit;      /* P.UNIT                                        */
	uint8_t  opcode;    /* P.OPCD (command) / endcode (end message)      */
};

/* SET CONTROLLER CHARACTERISTICS command parameter area (Table A-6, sec 6.16). */
#define VMS_OFF_MSCP_SCC_C_VRSN (VMS_OFF_SYSAP_BODY + 12) /* abs84, u16 -- host MUST supply 0 */
#define VMS_OFF_MSCP_SCC_C_CNTF (VMS_OFF_SYSAP_BODY + 14) /* abs86, u16, Table A-4 */
#define VMS_OFF_MSCP_SCC_C_HTMO (VMS_OFF_SYSAP_BODY + 16) /* abs88, u16, seconds */
#define VMS_OFF_MSCP_SCC_C_TIME (VMS_OFF_SYSAP_BODY + 20) /* abs92, u64, VMS absolute time or 0 */

struct vms_mscp_scc_cmd {
	struct vms_mscp_hdr hdr;    /* hdr.opcode ignored on build (forced SCC) */
	uint16_t version;           /* P.VRSN -- MUST be 0 (sec 6.16)          */
	uint16_t ctlr_flags;        /* P.CNTF, Table A-4                       */
	uint16_t host_timeout;      /* P.HTMO, seconds; 0 == disabled          */
	uint64_t time;              /* P.TIME, VMS absolute time or 0          */
};

vms_codec_status_t vms_mscp_scc_cmd_build(const struct vms_mscp_scc_cmd *c,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written);
vms_codec_status_t vms_mscp_scc_cmd_parse(const uint8_t *frame, uint32_t len,
					  struct vms_mscp_scc_cmd *out);

/* GET UNIT STATUS command: just the shared header plus P.MOD (MD.NXU); no
 * opcode-specific parameter area (Table A-6, sec 6.12). */
struct vms_mscp_gus_cmd {
	struct vms_mscp_hdr hdr;    /* hdr.opcode ignored on build (forced GUS) */
	uint16_t modifiers;         /* P.MOD, typically VMS_MSCP_MOD_NEXT_UNIT */
};

vms_codec_status_t vms_mscp_gus_cmd_build(const struct vms_mscp_gus_cmd *c,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written);
vms_codec_status_t vms_mscp_gus_cmd_parse(const uint8_t *frame, uint32_t len,
					  struct vms_mscp_gus_cmd *out);

/*
 * ONLINE command: P.MOD (MD.IMF/MD.SWP) plus a unit-flags word at the SAME
 * Table A-6 offset (14) the SCC command uses for P.CNTF -- MEASURED,
 * vms-291 lab-2 capture (see file header "P.UNFL ECHO RULE").
 */
#define VMS_OFF_MSCP_ONLINE_C_UNFL (VMS_OFF_SYSAP_BODY + 14) /* abs86, u16 */

struct vms_mscp_online_cmd {
	struct vms_mscp_hdr hdr;    /* hdr.opcode ignored on build (forced ONLINE) */
	uint16_t modifiers;         /* P.MOD: MD.IMF | MD.SWP                  */
	uint16_t unit_flags;        /* Table A-6 offset 14, host-requested P.UNFL */
};

vms_codec_status_t vms_mscp_online_cmd_build(const struct vms_mscp_online_cmd *c,
					     uint8_t *frame, uint32_t cap,
					     uint32_t *written);
vms_codec_status_t vms_mscp_online_cmd_parse(const uint8_t *frame, uint32_t len,
					     struct vms_mscp_online_cmd *out);

/* READ / WRITE (transfer) command parameter area, Table A-6 sec 5.3 --
 * shared by both opcodes. */
#define VMS_OFF_MSCP_XFER_C_BCNT (VMS_OFF_SYSAP_BODY + 12) /* abs84, u32, whole blocks */
#define VMS_OFF_MSCP_XFER_C_BUFF (VMS_OFF_SYSAP_BODY + 16) /* abs88, 12 bytes, buffer descriptor */
#define VMS_OFF_MSCP_XFER_C_LBN  (VMS_OFF_SYSAP_BODY + 28) /* abs100, u32, logical block number */
#define VMS_MSCP_XFER_BUFF_LEN   12u

struct vms_mscp_xfer_cmd {
	struct vms_mscp_hdr hdr;    /* hdr.opcode selects READ vs WRITE on build */
	uint32_t byte_count;        /* P.BCNT -- whole number of blocks (sec 5.3) */
	uint8_t  buffer_desc[VMS_MSCP_XFER_BUFF_LEN]; /* P.BUFF, opaque (Appendix D) */
	uint32_t lbn;                /* P.LBN                                    */
};

vms_codec_status_t vms_mscp_xfer_cmd_build(const struct vms_mscp_xfer_cmd *c,
					   uint8_t *frame, uint32_t cap,
					   uint32_t *written);
vms_codec_status_t vms_mscp_xfer_cmd_parse(const uint8_t *frame, uint32_t len,
					   struct vms_mscp_xfer_cmd *out);

/* ------------------------------------------------------------------ *
 * sec 7  End messages: the shared 12-byte header (endcode + P.FLGS +
 * P.STS) plus each class's own Table A-7 parameter area, at the
 * MEASURED lengths (see file header).
 * ------------------------------------------------------------------ */

struct vms_mscp_end_hdr {
	struct vms_mscp_hdr hdr;    /* hdr.opcode is the ENDCODE (cmd | 0x80) */
	uint8_t  flags;              /* P.FLGS, Table A-3                     */
	uint16_t status;              /* P.STS, RAW wire word                  */
	unsigned status_major;         /* sec 5.6 split, for convenience        */
	unsigned status_subcode;
};

/* SET CONTROLLER CHARACTERISTICS end message (Table A-7). */
#define VMS_OFF_MSCP_SCC_E_VRSN   (VMS_OFF_SYSAP_BODY + 12) /* abs84, u16 */
#define VMS_OFF_MSCP_SCC_E_CNTF   (VMS_OFF_SYSAP_BODY + 14) /* abs86, u16 */
#define VMS_OFF_MSCP_SCC_E_CTMO   (VMS_OFF_SYSAP_BODY + 16) /* abs88, u16 */
#define VMS_OFF_MSCP_SCC_E_RSVD18 (VMS_OFF_SYSAP_BODY + 18) /* abs90, u16 -- Table A-7 says reserved; OBSERVED */
#define VMS_OFF_MSCP_SCC_E_CNTI   (VMS_OFF_SYSAP_BODY + 20) /* abs92, 8 bytes, P.CNTI */

/* The two "OBSERVED, not explained by the 1982 manual" SCC-end constants,
 * measured 954/954 across the whole lab corpus with zero variation. Never
 * baked into a builder silently -- callers that want a server
 * indistinguishable from a real VMS server on the wire pass these
 * explicitly (see struct vms_mscp_scc_end doc). */
#define VMS_MSCP_SCC_CNTF_OBSERVED  0xa004u
#define VMS_MSCP_SCC_RSVD18_OBSERVED 0x0547u

struct vms_mscp_scc_end {
	struct vms_mscp_end_hdr eh;
	uint16_t version;       /* P.VRSN                                    */
	uint16_t ctlr_flags;    /* P.CNTF -- caller's choice; see the two    */
				 /* OBSERVED constants above for a           */
				 /* wire-indistinguishable-from-VMS profile   */
	uint16_t ctlr_timeout;  /* P.CTMO, seconds                           */
	uint16_t rsvd18;        /* Table A-7 RESERVED slot; see OBSERVED const */
	uint64_t ctlr_id;       /* P.CNTI                                    */
};

vms_codec_status_t vms_mscp_scc_end_build(const struct vms_mscp_scc_end *e,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written);
vms_codec_status_t vms_mscp_scc_end_parse(const uint8_t *frame, uint32_t len,
					  struct vms_mscp_scc_end *out);

/* GET UNIT STATUS end message (Table A-7), 52 bytes MEASURED (see file
 * header -- Table A-7's own field list stops at 48). */
#define VMS_OFF_MSCP_GUS_E_MLUN (VMS_OFF_SYSAP_BODY + 12) /* abs84, u16 */
#define VMS_OFF_MSCP_GUS_E_UNFL (VMS_OFF_SYSAP_BODY + 14) /* abs86, u16 */
#define VMS_OFF_MSCP_GUS_E_UNTI (VMS_OFF_SYSAP_BODY + 20) /* abs92, 8 bytes */
#define VMS_OFF_MSCP_GUS_E_MEDI (VMS_OFF_SYSAP_BODY + 28) /* abs100, u32 */
#define VMS_OFF_MSCP_GUS_E_SHUN (VMS_OFF_SYSAP_BODY + 32) /* abs104, u16 */
#define VMS_OFF_MSCP_GUS_E_TRCK (VMS_OFF_SYSAP_BODY + 36) /* abs108, u16 */
#define VMS_OFF_MSCP_GUS_E_GRP  (VMS_OFF_SYSAP_BODY + 38) /* abs110, u16 */
#define VMS_OFF_MSCP_GUS_E_CYL  (VMS_OFF_SYSAP_BODY + 40) /* abs112, u16 */
#define VMS_OFF_MSCP_GUS_E_RCTS (VMS_OFF_SYSAP_BODY + 44) /* abs116, u16 */
#define VMS_OFF_MSCP_GUS_E_RBNS (VMS_OFF_SYSAP_BODY + 46) /* abs118, u8  */
#define VMS_OFF_MSCP_GUS_E_RCTC (VMS_OFF_SYSAP_BODY + 47) /* abs119, u8  */
#define VMS_OFF_MSCP_GUS_E_TAIL (VMS_OFF_SYSAP_BODY + 48) /* abs120, u16 -- OBSERVED, [50:52) zero */

/* body[48:50] on EVERY GUS end message ever measured in the corpus,
 * including an RA81 on a different controller -- see file header
 * "CONFORMANCE ORACLE". COPIED, not composed: sec 5 register forbids
 * naming an undecoded field's meaning, and a length-echo vs plain-constant
 * reading are indistinguishable on a single-length population (this class
 * is always 110-content, and 0x6e == 110). body[50:52] is left zero: a
 * real server's own value there is visible stale garbage, not a value to
 * reproduce. */
#define VMS_MSCP_GUS_TAIL_OBSERVED 0x006eu

struct vms_mscp_gus_end {
	struct vms_mscp_end_hdr eh;
	uint16_t multi_unit;    /* P.MLUN                                    */
	uint16_t unit_flags;    /* P.UNFL, Table A-5                         */
	uint64_t unit_id;       /* P.UNTI -- sec 6.12: 0 means "virtually no  */
				 /* characteristics are valid" (walk terminator) */
	uint32_t media_id;      /* P.MEDI                                    */
	uint16_t shadow_unit;   /* P.SHUN -- sec 6.12: == the unit number    */
	uint16_t track_size;    /* P.TRCK                                    */
	uint16_t group_size;    /* P.GRP                                     */
	uint16_t cyl_size;      /* P.CYL                                     */
	uint16_t rct_size;      /* P.RCTS                                    */
	uint8_t  rbns;          /* P.RBNS                                    */
	uint8_t  rct_copies;    /* P.RCTC                                    */
};

vms_codec_status_t vms_mscp_gus_end_build(const struct vms_mscp_gus_end *e,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written);
vms_codec_status_t vms_mscp_gus_end_parse(const uint8_t *frame, uint32_t len,
					  struct vms_mscp_gus_end *out);

/* ONLINE / SET UNIT CHARACTERISTICS end message (Table A-7); shares
 * P.MLUN/P.UNFL/P.UNTI/P.MEDI with GUS, then diverges. 44 bytes MEASURED
 * (vms-291 lab-2 capture). abs[100,108) between P.MEDI and P.UNSZ is
 * Table A-7 RESERVED and left zero. */
#define VMS_OFF_MSCP_ONLINE_E_MLUN (VMS_OFF_SYSAP_BODY + 12) /* abs84, u16 */
#define VMS_OFF_MSCP_ONLINE_E_UNFL (VMS_OFF_SYSAP_BODY + 14) /* abs86, u16 */
#define VMS_OFF_MSCP_ONLINE_E_UNTI (VMS_OFF_SYSAP_BODY + 20) /* abs92, 8 bytes */
#define VMS_OFF_MSCP_ONLINE_E_MEDI (VMS_OFF_SYSAP_BODY + 28) /* abs100, u32 */
#define VMS_OFF_MSCP_ONLINE_E_UNSZ (VMS_OFF_SYSAP_BODY + 36) /* abs108, u32 */
#define VMS_OFF_MSCP_ONLINE_E_VSER (VMS_OFF_SYSAP_BODY + 40) /* abs112, u32 */

struct vms_mscp_online_end {
	struct vms_mscp_end_hdr eh;
	uint16_t multi_unit;    /* P.MLUN                                    */
	uint16_t unit_flags;    /* P.UNFL -- vms_mscp_online_unfl_compose()'s */
				 /* result belongs here on build              */
	uint64_t unit_id;       /* P.UNTI                                    */
	uint32_t media_id;      /* P.MEDI                                    */
	uint32_t unit_size;     /* P.UNSZ, logical blocks (Table A-7 off 36) */
	uint32_t volume_ser;    /* P.VSER (Table A-7 off 40)                 */
};

vms_codec_status_t vms_mscp_online_end_build(const struct vms_mscp_online_end *e,
					     uint8_t *frame, uint32_t cap,
					     uint32_t *written);
vms_codec_status_t vms_mscp_online_end_parse(const uint8_t *frame, uint32_t len,
					     struct vms_mscp_online_end *out);

/* READ / WRITE (transfer) end messages: the generic P.BCNT/P.FBBK pair
 * (Table A-7), at their two DIFFERENT measured lengths -- READ 32
 * (matches Table A-7's own generic-end arithmetic), WRITE 36 (four bytes
 * more, MEASURED, zero-filled -- see file header). abs[88,100) between
 * P.BCNT and P.FBBK is Table A-7 RESERVED and left zero; WRITE's trailing
 * four bytes past P.FBBK are likewise left zero (ungrounded). */
#define VMS_OFF_MSCP_XFER_E_BCNT (VMS_OFF_SYSAP_BODY + 12) /* abs84, u32 */
#define VMS_OFF_MSCP_XFER_E_FBBK (VMS_OFF_SYSAP_BODY + 28) /* abs100, u32, first bad block */

struct vms_mscp_xfer_end {
	struct vms_mscp_end_hdr eh;  /* eh.hdr.opcode selects READ vs WRITE on build */
	uint32_t byte_count;         /* P.BCNT -- bytes actually transferred   */
	uint32_t first_bad_block;    /* P.FBBK                                 */
};

vms_codec_status_t vms_mscp_read_end_build(const struct vms_mscp_xfer_end *e,
					   uint8_t *frame, uint32_t cap,
					   uint32_t *written);
vms_codec_status_t vms_mscp_read_end_parse(const uint8_t *frame, uint32_t len,
					   struct vms_mscp_xfer_end *out);
vms_codec_status_t vms_mscp_write_end_build(const struct vms_mscp_xfer_end *e,
					    uint8_t *frame, uint32_t cap,
					    uint32_t *written);
vms_codec_status_t vms_mscp_write_end_parse(const uint8_t *frame, uint32_t len,
					    struct vms_mscp_xfer_end *out);

/* ------------------------------------------------------------------ *
 * sec 8  The (SYSAP, category, opcode) allowlist rows this item
 * contributes (vms_cluster_codec.h sec 6). MSCP has no CM-style category
 * byte -- P.OPCD (body[8]) is the whole request key -- so `category` is
 * fixed 0x00 by this item's own convention (documented here, not a wire
 * fact) and `opcode` carries P.OPCD directly. Only the commands this
 * item's captures confirm a real class driver sends are listed; ONLINE/
 * READ/WRITE are RESPOND rows because FC-P6.3's responder answers them
 * (recipe ids are FC-P6.3's builder keys, opaque to this table per
 * vms_cluster_codec.h sec 6's own contract).
 * ------------------------------------------------------------------ */
#define VMS_MSCP_ALLOW_CATEGORY 0x00u

extern const struct vms_wire_allow_entry vms_mscp_allow_rows[];
extern const struct vms_wire_allow_table vms_mscp_allow_table;

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_CLUSTER_CODEC_MSCP_H */
