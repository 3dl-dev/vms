/*
 * dnet_dap.h - DECnet Phase IV DAP (Data Access Protocol) message codec, the
 * presentation layer that rides an established NSP logical link to move a file
 * (rd vms-8c2 -> re-grounded rd vms-a8a, epic vms-30e; north-star vms-e4dc).
 * This is the layer behind `$ COPY node"user pw"::file localfile` and the FAL
 * (File Access Listener, DECnet object 17) server.
 *
 * ================== CLEAN-ROOM PROVENANCE (Rule 8) ==================
 * Every byte here traces to one of two PUBLIC sources -- nothing is invented
 * and nothing comes from a VSI disassembly:
 *
 *   1. The DEC "DECnet DIGITAL Network Architecture Data Access Protocol
 *      Functional Specification, Version 5.6.0" (AA-K177A-TK, Oct 1980): the
 *      generic message format (OPERATOR = TYPE + FLAGS, optional STREAMID /
 *      LENGTH / LEN256 / BITCNT / SYSPEC), the field types (B = binary,
 *      EX-n = extensible bitmap of up to n bytes with bit 7 = "more",
 *      I-n = image field: 1 count byte + up to n bytes), and every message's
 *      field list and order (sec. 3.3 - 3.17), the setup + transfer sequences
 *      (sec. 5.1, 5.2.1, 5.2.2) and the MACCODE table (sec. 3.11).
 *
 *   2. Real OpenVMS VAX V7.3 FAL behaviour on the wire: the vms-cd3 oracle
 *      (docs/oracle/vax-copy-fal-dap.*) and the vms-a8a lab captures
 *      (tests/lab/captures/decnet-fal-dap-20261004/) in which an independent
 *      probe and then this codec drove a real VAX FAL through GET and PUT.
 *
 * WHAT THE PREVIOUS CUT GOT WRONG (rd vms-a8a, recorded so it is not repeated):
 * the vms-8c2 codec used an OVMX-invented frame (FLAGS always LENGTH, fixed-
 * width fields, NAME = 10 where the spec's NAME is 15 and 10 is KEY DEFINITION,
 * OSTYPE 1 = RT-11 for "VMS"), so it self-round-tripped but could not talk to a
 * real VMS FAL. This codec decodes every captured real-VMS DAP message field by
 * field (tests/vmsdecnet/test_dnet_dap.c anchors them) and its encoder output
 * was accepted by a real VMS FAL for a full GET and PUT.
 *
 * VERSION / SCOPE (INV-6). OVMX speaks DAP 5.6 and advertises in SYSCAP only
 * what it implements: sequential organisation, sequential FILE TRANSFER
 * (RAC = 3), blocking up to response (it can receive several messages in one
 * segment), the 2-byte LENGTH, and the NAME message. Messages and fields
 * beyond that are decoded (and bounded) but never served or faked:
 * the extended-attribute messages (KEY DEFINITION, ALLOCATION, SUMMARY,
 * DATE/TIME, PROTECTION, ACL) decode as "known type, body skipped";
 * segmented messages (FLAGS bit 6) are refused DNET_DAP_EUNSUP.
 *
 * PURITY / SECURITY. A PURE byte library: no socket, no fd, no clock, no
 * allocation. EVERY decode path is fully BOUNDED against hostile input (the
 * inbound FAL side is attacker-driven before and after auth): it never reads
 * past buf[len-1], refuses (does not clip) an over-long image field, and a
 * malformed message yields a negative code the caller turns into an NSP
 * disconnect -- "OVMX never crashes a peer", both directions.
 */
#ifndef DNET_DAP_H
#define DNET_DAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DAP message TYPE codes (spec sec. 3; operator field TYPE). */
enum dnet_dap_op {
    DNET_DAP_MSG_UNKNOWN     = 0,  /* a well-formed message of an unknown type */
    DNET_DAP_CONFIG          = 1,  /* Configuration                            */
    DNET_DAP_ATTRIBUTES      = 2,  /* Attributes (main)                        */
    DNET_DAP_ACCESS          = 3,  /* Access                                   */
    DNET_DAP_CONTROL         = 4,  /* Control                                  */
    DNET_DAP_CONTINUE        = 5,  /* Continue Transfer                        */
    DNET_DAP_ACKNOWLEDGE     = 6,  /* Acknowledge                              */
    DNET_DAP_ACCESS_COMPLETE = 7,  /* Access Complete                          */
    DNET_DAP_DATA            = 8,  /* Data                                     */
    DNET_DAP_STATUS          = 9,  /* Status                                   */
    DNET_DAP_KEYDEF          = 10, /* Key Definition Attributes Extension      */
    DNET_DAP_ALLOC           = 11, /* Allocation Attributes Extension          */
    DNET_DAP_SUMMARY         = 12, /* Summary Attributes Extension             */
    DNET_DAP_DATETIME        = 13, /* Date and Time Attributes Extension       */
    DNET_DAP_PROTECTION      = 14, /* Protection Attributes Extension          */
    DNET_DAP_NAME            = 15, /* Name                                     */
    DNET_DAP_ACL             = 16  /* Access Control List Attributes Extension */
};

/* FLAGS byte (spec sec. 3.2). */
#define DNET_DAP_FLAG_STREAMID   0x01
#define DNET_DAP_FLAG_LENGTH     0x02
#define DNET_DAP_FLAG_LEN256     0x04
#define DNET_DAP_FLAG_BITCNT     0x08
#define DNET_DAP_FLAG_RSVD4      0x10
#define DNET_DAP_FLAG_SYSPEC     0x20
#define DNET_DAP_FLAG_SEGMENTED  0x40

/* CONFIGURATION OSTYPE / FILESYS values (spec sec. 3.3). */
#define DNET_DAP_OS_VAXVMS       7
#define DNET_DAP_FS_RMS32        3

/* SYSCAP bit numbers (spec sec. 3.3) OVMX tests / advertises. */
#define DNET_DAP_CAP_SEQ_ORG        1
#define DNET_DAP_CAP_SEQ_XFER       5
#define DNET_DAP_CAP_BLOCK_TO_RESP  18
#define DNET_DAP_CAP_LEN256         20
#define DNET_DAP_CAP_SEQ_RECORD     33
#define DNET_DAP_CAP_NAME_MSG       40

/* ATTMENU bits (spec sec. 3.4) in field order. */
enum {
    DNET_DAP_ATT_DATATYPE = 0, DNET_DAP_ATT_ORG = 1,  DNET_DAP_ATT_RFM = 2,
    DNET_DAP_ATT_RAT = 3,      DNET_DAP_ATT_BLS = 4,  DNET_DAP_ATT_MRS = 5,
    DNET_DAP_ATT_ALQ = 6,      DNET_DAP_ATT_BKS = 7,  DNET_DAP_ATT_FSZ = 8,
    DNET_DAP_ATT_MRN = 9,      DNET_DAP_ATT_RUNSYS = 10, DNET_DAP_ATT_DEQ = 11,
    DNET_DAP_ATT_FOP = 12,     DNET_DAP_ATT_BSZ = 13, DNET_DAP_ATT_DEV = 14,
    DNET_DAP_ATT_SDC = 15,     DNET_DAP_ATT_LRL = 16, DNET_DAP_ATT_HBK = 17,
    DNET_DAP_ATT_EBK = 18,     DNET_DAP_ATT_FFB = 19, DNET_DAP_ATT_SBN = 20,
    DNET_DAP_ATT_LAST_KNOWN = 20
};

/* DATATYPE bits, ORG, RFM, RAT values (spec sec. 3.4). */
#define DNET_DAP_DT_ASCII     0x01
#define DNET_DAP_DT_IMAGE     0x02
#define DNET_DAP_ORG_SEQ      0x00
#define DNET_DAP_ORG_REL      0x10
#define DNET_DAP_ORG_IDX      0x20
#define DNET_DAP_RFM_UDF      0
#define DNET_DAP_RFM_FIX      1
#define DNET_DAP_RFM_VAR      2
#define DNET_DAP_RFM_VFC      3
#define DNET_DAP_RFM_STM      4
#define DNET_DAP_RAT_FTN      0x01
#define DNET_DAP_RAT_CR       0x02
#define DNET_DAP_RAT_PRN      0x04

/* ACCESS ACCFUNC (spec sec. 3.5). */
#define DNET_DAP_ACC_OPEN     1
#define DNET_DAP_ACC_CREATE   2
#define DNET_DAP_ACC_RENAME   3
#define DNET_DAP_ACC_ERASE    4
#define DNET_DAP_ACC_DIRLIST  6

/* FAC / SHR bits (spec sec. 3.5). */
#define DNET_DAP_FB_PUT       0x01
#define DNET_DAP_FB_GET       0x02

/* DISPLAY bits (spec sec. 3.5 / 3.6). */
#define DNET_DAP_DSP_MAIN     0x0001
#define DNET_DAP_DSP_NAME     0x0100

/* CONTROL CTLFUNC (spec sec. 3.6). */
#define DNET_DAP_CTL_GET      1
#define DNET_DAP_CTL_CONNECT  2
#define DNET_DAP_CTL_UPDATE   3
#define DNET_DAP_CTL_PUT      4
/* CTLMENU bits */
#define DNET_DAP_CTLM_RAC     0x01
#define DNET_DAP_CTLM_KEY     0x02
#define DNET_DAP_CTLM_KRF     0x04
#define DNET_DAP_CTLM_ROP     0x08
#define DNET_DAP_CTLM_HSH     0x10
#define DNET_DAP_CTLM_DISPLAY 0x20
#define DNET_DAP_CTLM_BLKCNT  0x40
/* RAC values */
#define DNET_DAP_RAC_SEQ      0
#define DNET_DAP_RAC_KEY      1
#define DNET_DAP_RAC_RFA      2
#define DNET_DAP_RAC_SEQFILE  3   /* sequential file transfer */
#define DNET_DAP_RAC_BLOCK    4
#define DNET_DAP_RAC_BLKFILE  5   /* block mode file transfer */

/* ACCESS COMPLETE CMPFUNC (spec sec. 3.9). */
#define DNET_DAP_CMP_CLOSE    1
#define DNET_DAP_CMP_RESPONSE 2
#define DNET_DAP_CMP_PURGE    3
#define DNET_DAP_CMP_EOS      4
#define DNET_DAP_CMP_SKIP     5

/* STATUS: STSCODE = MACCODE<<12 | MICCODE (spec sec. 3.11, Table 2). */
#define DNET_DAP_MAC(sts)     (((sts) >> 12) & 0xF)
#define DNET_DAP_MIC(sts)     ((sts) & 0x0FFF)
#define DNET_DAP_MAC_PENDING  0
#define DNET_DAP_MAC_SUCCESS  1
#define DNET_DAP_MAC_UNSUPP   2
#define DNET_DAP_MAC_OPEN     4   /* error before the file was opened          */
#define DNET_DAP_MAC_XFER     5   /* error after open (incl. EOF on a GET)     */
#define DNET_DAP_MAC_WARN     6
#define DNET_DAP_MAC_TERM     7
#define DNET_DAP_MAC_FORMAT   8
#define DNET_DAP_MAC_INVALID  9
#define DNET_DAP_MAC_SYNC     10
/* MICCODEs OVMX emits/recognises (spec Table 3, RMS-derived, octal in spec). */
#define DNET_DAP_MIC_EOF      047   /* end of file                              */
#define DNET_DAP_MIC_FNF      062   /* file not found                           */
#define DNET_DAP_MIC_PRV      0125  /* privilege violation                      */
#define DNET_DAP_MIC_CRE      030   /* ACP could not create file (STV = sys code) */
#define DNET_DAP_STS_EOF      ((DNET_DAP_MAC_XFER << 12) | DNET_DAP_MIC_EOF)

/* Caps. A field longer than these is REFUSED (never clipped). */
#define DNET_DAP_MAX_SPEC     255
#define DNET_DAP_MAX_REC      1500   /* one record in one DATA message          */
#define DNET_DAP_MAX_MSG      1600   /* an encoded message never exceeds this   */
#define DNET_DAP_MAX_EX       12     /* longest EX field (SYSCAP EX-12)         */

/* Return codes: OK is 0, every error negative. */
#define DNET_DAP_OK          0
#define DNET_DAP_ETRUNC     (-1)
#define DNET_DAP_EBADLEN    (-2)
#define DNET_DAP_EINVAL     (-3)
#define DNET_DAP_ENOSPACE   (-4)
#define DNET_DAP_EUNSUP     (-5)

/*
 * A decoded DAP message. EX bitmaps are returned as uint64_t (bit n = bit n
 * of the field); an EX field longer than 64 bits sets only what fits and its
 * presence is still bounded. I-n numeric fields are little-endian, up to 8
 * bytes. Everything on the FAL (inbound) side is UNTRUSTED input.
 */
struct dnet_dap_msg {
    enum dnet_dap_op op;
    uint8_t  type;              /* raw TYPE byte (also for unknown types)      */
    uint8_t  flags;             /* raw FLAGS                                    */
    uint8_t  streamid;          /* STREAMID if FLAGS bit 0, else 0              */
    uint8_t  bitcnt;            /* BITCNT if FLAGS bit 3                        */

    union {
        struct {                       /* CONFIGURATION */
            uint16_t bufsiz;
            uint8_t  ostype, filesys;
            uint8_t  vernum, econum, usrnum, softver, usrsoft;
            uint8_t  syscap[DNET_DAP_MAX_EX];
            uint8_t  syscap_len;
        } config;

        struct {                       /* ATTRIBUTES */
            uint64_t menu;
            uint64_t datatype;
            uint8_t  org, rfm;
            uint64_t rat;
            uint16_t bls, mrs;
            uint64_t alq;
            uint8_t  bks, fsz;
            uint64_t mrn;
            uint16_t deq;
            uint64_t fop;
            uint8_t  bsz;
            uint64_t dev, sdc;
            uint16_t lrl;
            uint64_t hbk, ebk;
            uint16_t ffb;
            uint64_t sbn;
        } attr;

        struct {                       /* ACCESS */
            uint8_t  accfunc;
            uint64_t accopt;
            char     filespec[DNET_DAP_MAX_SPEC + 1];
            int      have_fac, have_shr, have_display;
            uint64_t fac, shr, display;
        } access;

        struct {                       /* CONTROL */
            uint8_t  ctlfunc;
            uint64_t menu;
            uint8_t  rac;
            uint8_t  key[DNET_DAP_MAX_SPEC];
            uint8_t  keylen;
            uint8_t  krf;
            uint64_t rop;
            uint64_t display;
        } control;

        struct {                       /* CONTINUE TRANSFER */
            uint8_t  confunc;
        } cont;

        struct {                       /* ACCESS COMPLETE */
            uint8_t  cmpfunc;
            int      have_fop, have_check;
            uint64_t fop;
            uint16_t check;
        } complete;

        struct {                       /* DATA */
            uint64_t recnum;
            uint8_t  recnum_len;       /* 0 = absent (count byte 0)            */
            uint16_t reclen;
            uint8_t  rec[DNET_DAP_MAX_REC];
        } data;

        struct {                       /* STATUS */
            uint16_t stscode;
            uint64_t rfa, recnum, stv;
            int      have_stv;
        } status;

        struct {                       /* NAME */
            uint64_t nametype;         /* 1 = file spec, 2 = file name, ...    */
            char     namespec[DNET_DAP_MAX_SPEC + 1];
        } name;
    } u;
};

/*
 * dnet_dap_encode - encode `msg` (OPERATOR .. end of fields) into `buf`.
 * `with_length`: 0 emits the unblocked form (FLAGS = 0, the message runs to the
 * end of its NSP segment -- what OVMX sends, one message per segment); nonzero
 * emits FLAGS.LENGTH (+LEN256 when > 255) so it can be blocked. Field presence
 * is driven by the menu/have_* members. Returns DNET_DAP_OK and *outlen, or a
 * negative code.
 */
int dnet_dap_encode(const struct dnet_dap_msg *msg, int with_length,
                    uint8_t *buf, size_t cap, size_t *outlen);

/*
 * dnet_dap_decode - decode ONE DAP message at buf[0..len). Fully bounded.
 * A message without FLAGS.LENGTH extends to the end of `len` (spec: the last
 * message of a segment); with LENGTH it occupies exactly its header + operand.
 * Trailing bytes inside a message's operand beyond the fields this codec knows
 * are ignored (spec: later DAP versions append fields); unknown message TYPES
 * decode as op = DNET_DAP_MSG_UNKNOWN with type set. *consumed gets the bytes
 * this message occupied. On error *out is zeroed and a negative code returned.
 */
int dnet_dap_decode(const uint8_t *buf, size_t len,
                    struct dnet_dap_msg *out, size_t *consumed);

/* Test a SYSCAP bit in a decoded CONFIGURATION. */
int dnet_dap_syscap_has(const struct dnet_dap_msg *config, unsigned bit);

/* Build OVMX's own CONFIGURATION (DAP 5.6, VMS/RMS-32, OVMX SYSCAP). */
void dnet_dap_ovmx_config(struct dnet_dap_msg *m, uint16_t bufsiz);

/* Human name of an operator, for logs. Never NULL. */
const char *dnet_dap_op_name(enum dnet_dap_op op);

#ifdef __cplusplus
}
#endif

#endif /* DNET_DAP_H */
