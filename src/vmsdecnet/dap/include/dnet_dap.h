/*
 * dnet_dap.h - DECnet Phase IV DAP (Data Access Protocol) message codec, the
 * presentation layer that rides an established NSP logical link to move a file
 * (rd vms-8c2, epic vms-30e; north-star demo leg vms-e4dc). This is the layer
 * behind `$ COPY node"user pw"::file localfile` and the FAL (File Access
 * Listener, DECnet object 17) server.
 *
 * ================== CLEAN-ROOM PROVENANCE (Rule 8) ==================
 * TWO sources, and every byte here traces to one of them -- nothing is invented
 * and nothing comes from a VSI disassembly:
 *
 *   1. docs/oracle/vax-copy-fal-dap.{md,wire.txt,hex.txt} (rd vms-cd3) -- a REAL
 *      OpenVMS VAX V7.3 -> V7.3 `$ COPY` over DECnet, captured on the lab. It
 *      FIXES the ground truth this codec is forbidden to contradict:
 *        - the connect names Session Control OBJECT 17 (FAL) and CARRIES the
 *          username + password in the access-control fields (handled by the
 *          FAL server + the existing dnet_cterm_sc_connect_build, not here);
 *        - the message SEQUENCE over the link: CONFIGURATION (both ways) ->
 *          ATTRIBUTES/NAME (filename, resolved full spec, owner UIC, RMS
 *          attributes) -> CONTROL -> DATA (the file records VERBATIM) ->
 *          STATUS / ACCESS-COMPLETE -> clean NSP disconnect;
 *        - the file records travel VERBATIM as counted records inside DATA
 *          messages, and the resolved full spec / owner UIC travel as counted
 *          strings inside the attributes/name exchange (the oracle's hex shows
 *          "OVMXDAP_R.TXT;", "SYS$SYSROOT:[SYSMGR]OVMXDAP_R.TXT;1" and
 *          "[000001,000004]" in the clear, locatable by their ASCII).
 *
 *   2. The PUBLIC DEC DAP (Data Access Protocol) functional specification
 *      (AA-K177A-TK and successors) -- the message TYPES (CONFIGURATION,
 *      ATTRIBUTES, ACCESS, CONTROL, CONTINUE, ACKNOWLEDGE, ACCESS COMPLETE,
 *      DATA, STATUS, NAME) and the generic message framing (an OPERATOR byte, a
 *      FLAGS byte, an optional LENGTH field, then typed fields; counted "image"
 *      fields for strings; extensible bitmap fields).
 *
 * HONEST SCOPE (INV-6). The oracle §3 states plainly that it fixes the ground-
 * truth BYTES and the credential/object/SEQUENCE semantics but "does not hand-
 * transcribe every DAP sub-field". So the per-field FRAMING of each message
 * here is coded against the PUBLIC SPEC (source 2) and is self-round-tripping;
 * it is NOT asserted byte-identical to the real-VAX DAP sub-framing. Two OVMX
 * nodes interoperate over this codec faithfully (the sequence + the carried
 * values are the oracle's); byte-level wire interop with a stock VAX FAL is a
 * separate, harder rung (exact VAX DAP sub-field reversing beyond the oracle's
 * transcription) and is a FILED follow-on, never faked as done here.
 *
 * PURITY / SECURITY. Like the NSP and CTERM codecs, this is a PURE byte library:
 * no socket, no fd, no clock, no allocation beyond memcpy/memset. It links
 * equally into the daemon, the FAL server and the deterministic unit test.
 * EVERY decode path is fully BOUNDED against hostile input: DAP rides an NSP
 * link that, on the inbound (FAL) side, an unauthenticated attacker can drive,
 * so a malformed message must be REJECTED cleanly (a negative code the caller
 * turns into an NSP disconnect), never over-read -- "OVMX never crashes a peer",
 * both directions. The decoder never reads past buf[len-1] and refuses (does
 * not clip) an over-long counted field.
 */
#ifndef DNET_DAP_H
#define DNET_DAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DAP message OPERATOR (type) codes -- the public DAP specification message
 * set. Only the members this rung's sequential-file COPY needs are served;
 * others decode to DNET_DAP_MSG_UNKNOWN honestly rather than being faked. */
enum dnet_dap_op {
    DNET_DAP_CONFIG        = 1,  /* CONFIGURATION: buffer size, OS/filesys, version */
    DNET_DAP_ATTRIBUTES    = 2,  /* ATTRIBUTES: RMS org/rfm/rat/mrs/allocation      */
    DNET_DAP_ACCESS        = 3,  /* ACCESS: open/create a file by name              */
    DNET_DAP_CONTROL       = 4,  /* CONTROL: initiate data transfer (GET/PUT)       */
    DNET_DAP_CONTINUE      = 5,  /* CONTINUE-TRANSFER: resume/skip/ack              */
    DNET_DAP_ACKNOWLEDGE   = 6,  /* ACKNOWLEDGE: positive ack of ACCESS/CONTROL     */
    DNET_DAP_ACCESS_COMPLETE = 7,/* ACCESS COMPLETE: end of file access             */
    DNET_DAP_DATA          = 8,  /* DATA: one file record, verbatim                 */
    DNET_DAP_STATUS        = 9,  /* STATUS: MACRO/MICRO condition (RMS-style)       */
    DNET_DAP_NAME          = 10, /* NAME: resolved full spec / owner (attribute set)*/
    DNET_DAP_MSG_UNKNOWN   = 0   /* decoded a known-format frame of an unserved op  */
};

/* DAP FLAGS byte bits (public spec). This codec always emits LENGTH-present so
 * every message is self-delimiting on the wire -- the property that makes the
 * bounded decoder possible when several messages ride one NSP segment. */
#define DNET_DAP_FLAG_STREAMID  0x01
#define DNET_DAP_FLAG_LENGTH    0x02
#define DNET_DAP_FLAG_LEN256    0x04
#define DNET_DAP_FLAG_BITCNT    0x08

/* CONTROL CTLFUNC values (public spec: the operation the transfer performs). */
#define DNET_DAP_CTL_GET     1   /* transfer records FROM the remote file (read)  */
#define DNET_DAP_CTL_PUT     3   /* transfer records TO the remote file (write)   */
#define DNET_DAP_CTL_CONNECT 8   /* establish the record stream                   */

/* ACCESS ACCFUNC values (public spec). */
#define DNET_DAP_ACC_OPEN    1   /* open an existing file (a GET/read source)     */
#define DNET_DAP_ACC_CREATE  2   /* create a file       (a PUT/write sink)        */

/* RMS file-organization / record-format bytes carried in ATTRIBUTES (public
 * spec ORG/RFM). This rung serves SEQUENTIAL, variable-length records -- the
 * oracle's captured case. Other org/rfm decode honestly and are refused with
 * DNET_DAP_EUNSUP by the server (INV-6: indexed/relative are filed follow-ons). */
#define DNET_DAP_ORG_SEQ     0x00
#define DNET_DAP_RFM_VAR     0x02   /* variable-length records                    */
#define DNET_DAP_RFM_STMLF   0x05   /* stream-LF                                  */

/* Field caps. A DAP filespec / record longer than these is REFUSED (not
 * clipped) by the bounded decoder -- a clipped filespec that happens to resolve
 * is exactly the class of bug this decoder must not have. */
#define DNET_DAP_MAX_SPEC    255    /* a counted DAP image field is 1..255 bytes  */
#define DNET_DAP_MAX_REC     512    /* one sequential record this rung carries    */
#define DNET_DAP_MAX_MSG     600    /* an encoded message never exceeds this      */

/* Decode / encode return codes. OK is 0; every error is negative so a caller
 * can `if (rc < 0)` and turn it into an NSP disconnect (INV-6, never crash). */
#define DNET_DAP_OK          0
#define DNET_DAP_ETRUNC     (-1)   /* message runs past the buffer end            */
#define DNET_DAP_EBADLEN    (-2)   /* a counted field exceeds its cap / the msg   */
#define DNET_DAP_EINVAL     (-3)   /* malformed / unsupported framing             */
#define DNET_DAP_ENOSPACE   (-4)   /* encode: output buffer too small             */
#define DNET_DAP_EUNSUP     (-5)   /* well-formed but an unserved feature         */

/*
 * A decoded DAP message. Everything reachable on the FAL (inbound) side is
 * UNTRUSTED input. The union is discriminated by `op`; only the members named
 * for that op are meaningful.
 */
struct dnet_dap_msg {
    enum dnet_dap_op op;

    union {
        struct {                       /* CONFIGURATION */
            uint16_t bufsiz;           /* buffer size the peer offers            */
            uint8_t  ostype;           /* OS type (OVMX identifies as VMS)       */
            uint8_t  filesys;          /* file system (RMS)                      */
            uint8_t  version;          /* DAP version (root)                     */
        } config;

        struct {                       /* ATTRIBUTES */
            uint8_t  org;              /* file organization (SEQ served)         */
            uint8_t  rfm;              /* record format (VAR/STMLF served)       */
            uint8_t  rat;             /* record attributes (CR carriage-control) */
            uint16_t mrs;              /* maximum record size                    */
            uint32_t alq;              /* allocation quantity (blocks/size hint) */
        } attr;

        struct {                       /* ACCESS */
            uint8_t  accfunc;          /* DNET_DAP_ACC_*                         */
            char     filespec[DNET_DAP_MAX_SPEC + 1]; /* the file being accessed */
        } access;

        struct {                       /* CONTROL */
            uint8_t  ctlfunc;          /* DNET_DAP_CTL_*                         */
        } control;

        struct {                       /* NAME */
            uint8_t  nametype;         /* 1 = full file spec, 2 = owner UIC      */
            char     namespec[DNET_DAP_MAX_SPEC + 1]; /* resolved spec / [g,m]   */
        } name;

        struct {                       /* DATA */
            uint16_t reclen;           /* record length (bounded by MAX_REC)     */
            uint8_t  rec[DNET_DAP_MAX_REC];  /* the record bytes, VERBATIM       */
        } data;

        struct {                       /* STATUS */
            uint16_t stscode;          /* MACRO<<12 | MICRO (RMS-style condition)*/
        } status;

        struct {                       /* ACCESS COMPLETE / CONTINUE / ACK */
            uint8_t  func;             /* completion / continue function code    */
        } complete;
    } u;
};

/* DAP STATUS codes this rung uses (public spec MACRO/MICRO split). SUCCESS is
 * the "operation completed" macro; the specific micro values are OVMX-chosen
 * within the spec's ranges and only carried between two OVMX nodes. */
#define DNET_DAP_STS_SUCCESS   0x0000  /* pending / normal                        */
#define DNET_DAP_STS_EOF       0x0A00  /* end of file on a GET                    */
#define DNET_DAP_STS_ACCFAIL   0x2800  /* access denied / could not open the file */

/*
 * dnet_dap_encode - encode `msg` into `buf` (OPERATOR .. end of body). Writes
 * the byte count to *outlen. Returns DNET_DAP_OK, DNET_DAP_ENOSPACE if `cap` is
 * too small, or DNET_DAP_EINVAL on a malformed message. Every encoder emits a
 * LENGTH-present frame so the decoder can walk a blocked stream.
 */
int dnet_dap_encode(const struct dnet_dap_msg *msg,
                    uint8_t *buf, size_t cap, size_t *outlen);

/*
 * dnet_dap_decode - decode ONE DAP message from `buf`/`len` into `out`. Fully
 * bounded: never reads past buf[len-1]; refuses (does not clip) an over-long
 * counted field or a LENGTH that overruns the buffer; refuses an unknown FLAGS
 * shape. On success sets *consumed to the bytes this message occupied (so the
 * caller can decode the next message in a blocked NSP segment) and returns
 * DNET_DAP_OK. *out is zeroed first, so a failure leaves nothing half-filled.
 * A well-formed message of an unserved OPERATOR decodes with op set to that
 * value (or DNET_DAP_MSG_UNKNOWN) and consumed advanced -- the caller decides
 * whether to serve or honestly refuse it, the decoder never faults on it.
 */
int dnet_dap_decode(const uint8_t *buf, size_t len,
                    struct dnet_dap_msg *out, size_t *consumed);

/* Human name of an operator, for logs. Never NULL. */
const char *dnet_dap_op_name(enum dnet_dap_op op);

#ifdef __cplusplus
}
#endif

#endif /* DNET_DAP_H */
