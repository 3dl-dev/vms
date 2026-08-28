/*
 * scs_dlm.h - Distributed Lock Manager over SCS: the DLM SYSAP message class
 * (vms-94c, DLM epic vms-7fa rung 1 -- the message TRANSPORT).
 *
 * WHAT THIS IS. The encode/decode for the four core DLM messages OVMX exchanges
 * with a peer node over a dedicated DLM SYSAP SCS connection:
 *
 *     ENQ    -- "master, please grant/convert this lock on this resource"
 *     GRANT  -- the master's status response (granted / queued / rejected)
 *     DEQ    -- "master, release this lock"
 *     BLKAST -- "your lock blocks another request" (blocking-AST notification)
 *
 * Every DLM message rides as a p. 4-13 SCS APPLICATION MESSAGE (MTYPE 10) with
 * its body at SCA content offset 58 -- exactly the nesting scs_mscp.c uses for
 * an MSCP command. The [0:42] NISCA sequenced-message header and the [42:58] SCS
 * envelope are the SHARED transport, identical to every other SYSAP application
 * message; only the [58:] body and the Con.ID pair are DLM's own.
 *
 * ===================== CLEAN-ROOM PROVENANCE (Rule 8) =====================
 *
 * TWO distinct provenance classes are combined here, and the split is
 * load-bearing:
 *
 *  (A) THE SEMANTIC FIELD VALUES are AUTHENTIC OpenVMS $LCKDEF, taken from the
 *      in-tree src/libvms/include/lckdef.h, itself oracle-pinned by two
 *      independent documented-tool methods (LIBRARIAN + MACRO-32) on the
 *      OpenVMS VAX V7.3 reference oracle -- see lckdef.h's header. The lock
 *      GRANT MODES (LCK$K_NLMODE..LCK$K_EXMODE, 0..5), the $ENQ FLAGS (LCK$M_*),
 *      and the 16-byte value-block length (LCK$C_VALBLK_LEN) carried in a DLM
 *      message are these documented values, unchanged. The public $ENQ/$DEQ
 *      system-service interface (OpenVMS System Services Reference; Programming
 *      Concepts, Lock Management) and the directory/master resolution ALGORITHM
 *      (VSI OpenVMS Cluster Systems manual; IDSM lock-management chapter --
 *      the documented 3-case directory lookup) define WHAT a lock request
 *      carries and HOW it is routed.
 *
 *  (B) THE BYTE LAYOUT of the DLM SCS message body below (offsets [58:138]) is
 *      an ⚠ OVMX DESIGN CHOICE, and is LABELLED as such -- it is NOT presented
 *      as VMS-authentic. VSI/HPE do NOT publish the byte-level layout of the
 *      lock-manager's SCS$DIR_LOOKUP / lock-request messages, and Rule 8
 *      forbids obtaining it by disassembly. So, exactly as
 *      docs/design-link-native-toolchain.md does for the image/symbol formats
 *      the linker docs leave unpublished, OVMX defines its OWN self-describing
 *      body layout, uses it OVMX<->OVMX, and records it in
 *      docs/compat/facilities/cluster-dlm.yaml as OVMX-observed/derived. When a
 *      real VAX DLM capture is obtained on the reference lab, the AUTHENTIC wire
 *      layout replaces this one (the field SET does not change -- resource name,
 *      mode, flags, LKID, status, CSIDs, value block -- only the byte offsets).
 *      That is the same "authenticity is the target" posture rung 1 states.
 *
 * NEVER disassemble, decompile, or copy VSI/HPE source or binaries.
 *
 * This module is PURE: no socket, no allocation, no environment, no logging.
 * Every function is a total function of its arguments over a caller-supplied
 * buffer, and tests/vmsscs/test_scs_dlm.c holds the build->parse round-trip.
 */
#ifndef SCS_DLM_H
#define SCS_DLM_H

#include <stddef.h>
#include <stdint.h>

#include "lckdef.h" /* authentic $LCKDEF: LCK$K_ modes, LCK$M_ flags, valblk len */

#ifdef __cplusplus
extern "C" {
#endif

/* --- frame geometry (content-relative offsets; content = abs frame 14) ----
 *
 * BODY_OFF 58 is SCS_ENV_HDR_END: the DLM body starts immediately after the
 * SCS envelope, the same place scs_mscp.c places the MSCP message. */
#define SCS_DLM_BODY_OFF    58
#define SCS_DLM_BODY_LEN    80                                   /* the OVMX DLM body (below) */
#define SCS_DLM_SCA_LEN     (SCS_DLM_BODY_OFF + SCS_DLM_BODY_LEN) /* 138 */
#define SCS_DLM_FRAME_LEN   (14 + SCS_DLM_SCA_LEN)               /* 152 (+ Ethernet) */

#define SCS_DLM_RESNAM_MAX  31   /* $ENQ resource name is <= 31 bytes */
#define SCS_DLM_RESNAM_FIELD 32  /* fixed on-wire field width for the name  */

/* --- DLM message opcodes (⚠ OVMX-derived, class (B) above) ----------------
 *
 * These name the FOUR message kinds rung 1 transports. They are OVMX values,
 * not a VMS wire encoding, and MUST NOT be described as VMS-authentic. */
#define SCS_DLM_OP_ENQ      1u   /* lock/convert request  -> master           */
#define SCS_DLM_OP_GRANT    2u   /* status response       <- master           */
#define SCS_DLM_OP_DEQ      3u   /* dequeue request       -> master           */
#define SCS_DLM_OP_BLKAST   4u   /* blocking-AST notify   <- master           */

/* --- DLM body field offsets (⚠ OVMX-derived byte layout, class (B)) --------
 *
 * All fields little-endian, matching the whole SCS wire (VAX/x86 LE). The
 * VALUES placed in `mode`, `flags` and the 16-byte value block are authentic
 * $LCKDEF (class (A)); the OFFSETS are OVMX's own. */
#define SCS_DLM_B_OP          0   /* u8   SCS_DLM_OP_*                          */
#define SCS_DLM_B_MODE        1   /* u8   LCK$K_ mode (requested on ENQ, granted on GRANT) */
#define SCS_DLM_B_FLAGS       2   /* u16  LCK$M_ $ENQ flags                     */
#define SCS_DLM_B_REQ_LKID    4   /* u32  requester's local lock handle         */
#define SCS_DLM_B_MASTER_LKID 8   /* u32  master's lock handle (0 on ENQ)       */
#define SCS_DLM_B_STATUS      12  /* u32  VMS status (0 on request; SS$_ on GRANT) */
#define SCS_DLM_B_REQ_CSID    16  /* u32  CSID of the requesting node           */
#define SCS_DLM_B_MASTER_CSID 20  /* u32  CSID of the mastering node (0=resolve)*/
#define SCS_DLM_B_NAMELEN     24  /* u8   resource name length (0..31)          */
#define SCS_DLM_B_PARENT_PRES 25  /* u8   1 if a sub-lock under a parent        */
#define SCS_DLM_B_RSVD        26  /* u16  reserved, must be 0                    */
#define SCS_DLM_B_PARENT_LKID 28  /* u32  parent lock handle when PARENT_PRES    */
#define SCS_DLM_B_VALBLK      32  /* u8[16] lock value block (LCK$C_VALBLK_LEN)  */
#define SCS_DLM_B_RESNAM      48  /* u8[32] resource name, namelen valid        */

/*
 * struct scs_dlm_msg - the decoded/desired DLM request or response, direction
 * independent. The builder consumes it; the parser fills it.
 */
struct scs_dlm_msg {
    uint8_t  op;            /* SCS_DLM_OP_* */
    uint8_t  mode;          /* LCK$K_ mode */
    uint16_t flags;         /* LCK$M_ flags */
    uint32_t req_lkid;      /* requester's local lock handle */
    uint32_t master_lkid;   /* master's lock handle */
    uint32_t status;        /* VMS status code (odd=success) */
    uint32_t req_csid;      /* requesting node CSID */
    uint32_t master_csid;   /* mastering node CSID (0 => resolve via directory) */
    uint8_t  parent_present;/* sub-lock flag */
    uint32_t parent_lkid;   /* parent lock handle */
    uint8_t  valblk[LCK$C_VALBLK_LEN]; /* 16-byte value block */
    uint8_t  namelen;       /* 0..SCS_DLM_RESNAM_MAX */
    uint8_t  resnam[SCS_DLM_RESNAM_FIELD]; /* resource name, blank/zero padded */
};

/*
 * scs_dlm_build_body - lay the OVMX DLM body into `body` (>= SCS_DLM_BODY_LEN).
 * Zero-fills first so every reserved byte and the unused name tail is 0 by
 * construction. Returns 0, or -1 on a NULL/short buffer, an unknown op, a mode
 * outside 0..LCK$K_EXMODE, or a namelen > SCS_DLM_RESNAM_MAX.
 */
int scs_dlm_build_body(const struct scs_dlm_msg *m, uint8_t *body, size_t body_len);

/*
 * scs_dlm_parse_body - decode an OVMX DLM body from `body` (>= SCS_DLM_BODY_LEN)
 * into *m. Returns 0, or -1 on a NULL/short buffer or a namelen field that
 * exceeds SCS_DLM_RESNAM_MAX (a malformed message, refused rather than
 * over-read).
 */
int scs_dlm_parse_body(const uint8_t *body, size_t body_len, struct scs_dlm_msg *m);

/*
 * struct scs_dlm_params - the SCS/identity half of one DLM frame. Mirrors
 * struct scs_mscp_params: the transport header fields scsd fills from the
 * connection state.
 */
struct scs_dlm_params {
    uint8_t  dst_mac[6];      /* Ethernet dst = peer's Ethernet src MAC */
    uint8_t  src_mac[6];      /* Ethernet src = OVMX HW MAC */
    uint8_t  src_logical[6];  /* SCA src-logical [10:16] = aa:00:04:00:<LE16(sysid)> */
    uint8_t  peer_logical[6]; /* SCA dst-logical [2:8]  = peer's advertised logical addr */
    uint32_t remote_conid;    /* [50:54] peer's DLM SYSAP Con.ID */
    uint32_t local_conid;     /* [54:58] OVMX's DLM SYSAP Con.ID */
    uint16_t recv_ack;        /* SCS recv_seq (envelope [18:20]/[26:28]/[34:36]) */
    uint16_t send_seq;        /* SCS send_seq (envelope [20:22]/[30:32]) */
    uint16_t incarnation;     /* envelope [22:24]; 0 => fresh-contact value 1 */
    uint16_t credit;          /* SCS envelope credit [48:50] (0 => the replayed default) */
};

/*
 * scs_dlm_build_frame - build a complete Ethernet+SCA DLM frame into
 * out[SCS_DLM_FRAME_LEN]: the shared NISCA header, identity + sequence
 * substitutions, the SCS envelope (MTYPE 10), and the DLM body. Returns 0, or
 * -1 on a NULL argument or a body that scs_dlm_build_body rejects.
 */
int scs_dlm_build_frame(const struct scs_dlm_params *p, const struct scs_dlm_msg *m,
                        uint8_t out[SCS_DLM_FRAME_LEN]);

/*
 * struct scs_dlm_view - the decoded view of a received DLM frame: the SCS
 * envelope fields the caller matches the connection on, plus the DLM body.
 */
struct scs_dlm_view {
    uint16_t total_sca_len;
    uint16_t scs_mtype;     /* [46:48] -- 10 on every DLM frame */
    uint16_t credit;        /* [48:50] */
    uint16_t recv_ack;      /* [18:20] */
    uint16_t send_seq;      /* [20:22] */
    uint32_t remote_conid;  /* [50:54] */
    uint32_t local_conid;   /* [54:58] */
    struct scs_dlm_msg msg; /* the decoded DLM body */
};

/*
 * scs_dlm_parse - classify a received frame as a DLM-over-SCS message and fill
 * *v. Returns 0 on a well-formed MTYPE-10 frame that holds the whole DLM body
 * (>= SCS_DLM_FRAME_LEN bytes and envelope-conformant), -1 otherwise. Does NOT
 * check the Con.ID pair -- the caller matches the connection (scs_cdl_lookup).
 */
int scs_dlm_parse(const uint8_t *frame, size_t len, struct scs_dlm_view *v);

/* Static, never-NULL names for logs. */
const char *scs_dlm_op_name(uint8_t op);
const char *scs_dlm_mode_name(uint8_t mode);

#ifdef __cplusplus
}
#endif

#endif /* SCS_DLM_H */
