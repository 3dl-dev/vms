/*
 * dnet_cterm_hostfsm.h - the CTERM HOST role, spoken the way a real OpenVMS
 *                        host speaks it (rd vms-a70 direction B).
 *
 * WHAT THIS IS. When a remote node types `$ SET HOST <this node>`, this node
 * is the CTERM *host*: it runs LOGINOUT/DCL and drives the remote *server*
 * (the terminal side). This module is the wire half of that role -- a PURE
 * FSM + codec, no socket, no process, no clock of its own, no allocation -- so
 * it links into NETACP (decnetd) and into the unit test and the lab harness
 * alike. It replaces, for the inbound path, the OVMX-invented flat PDU set
 * (dnet_cterm_bind_accept / dnet_cterm_write / the host arm of dnet_cterm_rx),
 * which a real VMS SET HOST client cannot parse and which waited for the
 * client to speak first. A real VMS client waits for the HOST.
 *
 * CLEAN-ROOM GROUND TRUTH (Rule 8). Every message layout below is read from
 * the public specs and checked byte-for-byte against real VMS hosts on the
 * wire. Nothing was disassembled, decompiled or copied.
 *   - DNA Network Virtual Terminal Foundation Services spec, AA-DY89A-TK
 *     (sec 4.4): Bind Request (1), Unbind (2), Bind Accept (4), Common Data
 *     (9) = MSGTYPE, FILL, then repeating {LENGTH(2), carried message}.
 *   - DNA Network Command Terminal (CTERM) spec, AA-DY88A-TK (sec 4.16):
 *     Initiate (1), Start Read (2), Read Data (3), Out-of-band (4), Write (7),
 *     Characteristics (11); sec 4.17 selectors.
 *   - Wire: docs/oracle/vax-sethost-cterm.pcap (VAX2 as host, VAX1 client) and
 *     tests/lab/captures/decnet-sethost-dcl-20260911/sethost-live-dcl.pcap
 *     (VAX1 as host, OVMX as client, full login + DCL + logout).
 *
 * THE HOST'S OPENING, as both real VAX hosts sent it (and as this FSM sends it):
 *   1. on link-up the HOST speaks first, a foundation Bind Request, truncated
 *      after SUPPORT exactly as VMS sends it:
 *        01 | 02 04 00 (version 2.4.0) | 07 00 (OPSYS 7 = VMS) | 10 00
 *        (SUPPORT bit 4 = "terminal communication protocol")
 *   2. the server answers Bind Accept (04 ...);
 *   3. the host sends two Common Data messages:
 *        09 00 | 17 00 <CTERM Initiate, 23 bytes> | 06 00 <Characteristics:
 *                selector 0x0208 INPUT-COUNT-STATE = 2>
 *        09 00 | 07 00 <Characteristics: selector 0x0202 CHARACTER-ATTRIBUTES,
 *                char ^C, mask 3b, attrs 00 = ^C is NOT out-of-band>
 *      (the earlier client-side codec read the first of these as one message
 *      whose length field "did not match" -- it is TWO carried messages, and
 *      the length field matches the first exactly);
 *   4. the server's own Initiate arrives (its max message size, input buffer
 *      size, supported-message bitmap and -- a VMS server -- its terminal's
 *      characteristics, parameter 4, decoded below and recorded on the RTAn:
 *      by NETACP); the host answers with the VMS message 23 (0x17) both real
 *      VAX hosts sent, reproduced literally. Neither VAX host ever sends a
 *      CTERM Read Characteristics (message 10) -- the host-sent message types
 *      in both captures are 1, 2, 7, 11, 15 and 23. What a VAX host does send
 *      is VMS message 15 when LOGINOUT/DCL issue $QIO SENSEMODE, and the reply
 *      repeats the blob the Initiate already carried. So the server
 *      volunteers its terminal at bind time and this host adds no message.
 * Then terminal I/O: output goes as Writes, input is SOLICITED with Start Read
 * (the prompt rides in the read, as VMS does it), the typed line comes back as
 * Read Data, and when the session process exits the host sends foundation
 * Unbind `02 03 00` (reason 3, "user unbind request").
 *
 * WHAT IS NOT REPRODUCED, AND WHY. A VMS host also sends VMS-private message 15
 * (0x0F, a $QIO SENSEMODE/SETMODE passthrough -- function codes 0x27/0x23 sit
 * in it) because LOGINOUT and DCL issue those QIOs. OVMX's session reaches the
 * host only as a byte stream, so it issues none; it ignores the 15/19 replies a
 * server might send unprompted. It never invents a field to fill the gap.
 *
 * BOUNDED. Every decoder here runs on bytes a peer sent before anyone
 * authenticated: no read past `len`, every carried LENGTH is checked against
 * what remains, every buffer is fixed-size and every append is capped.
 */
#ifndef DNET_CTERM_HOSTFSM_H
#define DNET_CTERM_HOSTFSM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- foundation message types (AA-DY89A-TK sec 4.4) --------------------- */
#define DNET_CTH_F_BIND_REQUEST  1
#define DNET_CTH_F_UNBIND        2
#define DNET_CTH_F_BIND_ACCEPT   4
#define DNET_CTH_F_COMMON_DATA   9

/* Unbind REASON values (sec 4.4.2). */
#define DNET_CTH_UNBIND_USER     3   /* "user unbind request" -- VMS logout  */
#define DNET_CTH_UNBIND_PROTOCOL 7   /* "protocol error detected"            */

/* ---- CTERM message types (AA-DY88A-TK sec 4.16.1) ----------------------- */
#define DNET_CTH_M_INITIATE        1
#define DNET_CTH_M_START_READ      2
#define DNET_CTH_M_READ_DATA       3
#define DNET_CTH_M_OOB             4
#define DNET_CTH_M_WRITE           7
#define DNET_CTH_M_WRITE_COMPLETE  8
#define DNET_CTH_M_CHARACTERISTICS 11
#define DNET_CTH_M_VMS_QIO         15  /* VMS-private, observed: QIO passthrough */
#define DNET_CTH_M_VMS_19          19  /* VMS-private, observed server->host     */
#define DNET_CTH_M_VMS_23          23  /* VMS-private, observed both directions  */

/* Start Read FLAGS (3 bytes on the wire; sec 4.16.3, bit positions from the
 * spec's ".... ..EE ZZQT NDDD IIKV FCUU" diagram, numbered from bit 0). The
 * N/T/Q meanings follow the spec's listing order and are pinned by the wire:
 * the VAX's Password: read differs from its Username: read ONLY in bit 11
 * (no-echo), and its Username: read -- which the VAX did time out after 20 s
 * (oracle frame at t=19.8) -- is the one carrying bit 13 and TIMEOUT 0x14. */
#define DNET_CTH_RD_FORMAT       0x000008u /* F: LF after a CR                  */
#define DNET_CTH_RD_NOECHO       0x000800u /* N: do not echo this read          */
#define DNET_CTH_RD_TERM_ECHO    0x001000u /* T: echo the terminator            */
#define DNET_CTH_RD_TIMED        0x002000u /* Q: TIMEOUT is meaningful          */
#define DNET_CTH_RD_TERMSET_UNIV 0x008000u /* ZZ=2: universal terminator set    */

/* Write FLAGS (2 bytes; sec 4.16.8 "....TSQQ PPEB DLUU"). */
#define DNET_CTH_WR_LOCK_UNLOCK  0x0002u   /* UU=2: lock before, unlock after   */
#define DNET_CTH_WR_BOM          0x0010u   /* B: beginning of host message      */
#define DNET_CTH_WR_EOM          0x0020u   /* E: end of host message            */
#define DNET_CTH_WR_PREFIX_NL    0x0040u   /* PP=1: PREFIX-VALUE = newline count */
#define DNET_CTH_WR_POSTFIX_CHAR 0x0200u   /* QQ=2: POSTFIX-VALUE = a character */
/* The two Write shapes a VAX host was seen to send: a raw pass-through write
 * (`07 32 00 00 00 1b 3e`, the keypad-mode escape) and a VMS carriage-control
 * record (`07 72 02 01 0d <text>`: one newline before, CR after). */
#define DNET_CTH_WR_RAW    (DNET_CTH_WR_LOCK_UNLOCK | DNET_CTH_WR_BOM | DNET_CTH_WR_EOM)
#define DNET_CTH_WR_RECORD (DNET_CTH_WR_RAW | DNET_CTH_WR_PREFIX_NL | DNET_CTH_WR_POSTFIX_CHAR)

/* Read Data completion codes (sec 4.16.4, FLAGS low nibble). */
#define DNET_CTH_RDC_TERMINATOR  0
#define DNET_CTH_RDC_TIMEOUT     5

/* ---- bounds ------------------------------------------------------------- */
/* Largest NSP segment this module emits. The peer's Initiate may lower it
 * (its "maximum message size" parameter). 1024 is OVMX's NSP data cap. */
#define DNET_CTH_SEG_MAX     1024
#define DNET_CTH_TXQ         32     /* queued segments awaiting the link      */
#define DNET_CTH_OUT_MAX     4096   /* terminal output awaiting a flush       */
#define DNET_CTH_IN_MAX      2048   /* typed input awaiting the terminal      */
#define DNET_CTH_PROMPT_MAX  128    /* longest tail sent as a read's prompt   */
#define DNET_CTH_READ_MAX    255    /* input characters solicited per read    */
#define DNET_CTH_ECHO_MAX    512    /* substrate echo we expect to discard    */
/* With nothing to prompt with, how long the host waits before it still
 * solicits input (an empty-prompt read), so a slow app's prompt can arrive. */
#define DNET_CTH_EMPTY_READ_MS 400

/* Return codes. */
#define DNET_CTH_OK        0
#define DNET_CTH_ETRUNC  (-1)   /* input shorter than its declared shape     */
#define DNET_CTH_EBADLEN (-2)   /* a length field overruns the input/buffer  */
#define DNET_CTH_ENOSPACE (-3)  /* output buffer / queue full                */
#define DNET_CTH_EINVAL  (-4)   /* null argument or bad field                */
#define DNET_CTH_EPROTO  (-5)   /* a message invalid in the current state    */
#define DNET_CTH_ESTATE  (-6)   /* call invalid in the current state         */

/* ---- codec (exported so tests and the lab harness pin exact bytes) ------ */

/* Foundation Bind Request, VMS-truncated form (8 bytes, see above). */
int dnet_cth_bind_request_build(uint8_t *buf, size_t cap, size_t *outlen);
/* Foundation Unbind: `02 <reason LE16>`. */
int dnet_cth_unbind_build(uint16_t reason, uint8_t *buf, size_t cap, size_t *outlen);

/* Common Data builder: start `09 00`, then append whole carried messages. */
struct dnet_cth_cd {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
};
int dnet_cth_cd_begin(struct dnet_cth_cd *cd, uint8_t *buf, size_t cap);
int dnet_cth_cd_add(struct dnet_cth_cd *cd, const uint8_t *msg, size_t mlen);

/* Common Data parser: iterate the carried messages, bounded. */
struct dnet_cth_cd_iter {
    const uint8_t *buf;
    size_t len;
    size_t off;
};
/* DNET_CTH_OK if `buf` is a Common Data message (09 00 ...), else EINVAL/ETRUNC. */
int dnet_cth_cd_iter_init(struct dnet_cth_cd_iter *it, const uint8_t *buf, size_t len);
/* 1 = one carried message in *msg and *mlen; 0 = end; DNET_CTH_EBADLEN when a
 * LENGTH overruns the segment (the rest of the segment is then untrusted). */
int dnet_cth_cd_iter_next(struct dnet_cth_cd_iter *it, const uint8_t **msg, size_t *mlen);

/* CTERM Initiate (sec 4.16.2): MSGTYPE 1, FLAGS 0, VERSION 1.4.0, an 8-byte
 * REVISION, then parameters 1 (max message size we accept, LE16) and 3 (the
 * supported-message bitmap the VAX hosts sent: fe ff ef 00). */
int dnet_cth_initiate_build(uint16_t max_msg, uint8_t *buf, size_t cap, size_t *outlen);

/* THE ORIGINATING TERMINAL (rd vms-14b). A VMS server's Initiate carries a
 * fourth parameter the public spec does not list (sec 4.16.2 defines 1-3): its
 * VALUE is the server terminal's characteristics in the layout the public
 * OpenVMS I/O User's Reference Manual documents for the terminal driver's
 * IO$_SENSEMODE/IO$_SETMODE characteristics buffer --
 *     byte 0      device class  (DC$_TERM = 66)
 *     byte 1      device type   (a DT$_ code)
 *     bytes 2-3   page width    (LE word)
 *     bytes 4-6   terminal characteristics (TT$), byte 7 page length
 *     bytes 8-11  extended terminal characteristics (TT2$)
 * Pinned on the wire, never assumed:
 *   - tests/lab/captures/decnet-sethost-inbound-20261005: VAX1's SET HOST from
 *     its LA36-typed console sends `04 18 42 20 84 00 a0 02 02 00 00 30 00 00
 *     ...` (24-byte value), and SHOW TERMINAL on the RTAn: of THAT session
 *     prints Device_Type LA36, Width 132, Page 0 (vax-rta-show-terminal.txt).
 *     0x42 = DC$_TERM 66 and 0x20 = DT$_LA36 32 per the V7.3 node's own DCDEF
 *     (docs/oracle/vax73-starlet-defs/DCDEF.txt); 0x0084 = 132; byte 7 = 0.
 *   - docs/oracle/vax-sethost-cterm.pcap, session 1: the same LA36 console at
 *     Page 24 sends byte 7 = 0x18 (and the earlier SET TERMINAL/PAGE=48 diff,
 *     dnet_cterm.h vms-bd0, moved exactly that byte to 0x30). Session 2 of
 *     that capture came from a terminal set otherwise: byte 1 = 0x60 =
 *     DT$_VT100 96 (DCDEF), width 0x0050 = 80 -- so type and width are
 *     per-session values at these offsets, not constants.
 * The TT$/TT2$ words are mapped onto the RTAn:'s characteristic vector by
 * dnet_cth_termchar_to_ttc() below, bit positions from the V7.3 node's own
 * $TTDEF/$TT2DEF (docs/oracle/vax73-starlet-defs/TTDEF.txt, TT2DEF.txt). VMS
 * message 19 in the same session carries TT2$ 0x3200 where this parameter
 * carries 0x3000: the difference is TT2$V_DCL_MAILBX (bit 9), which has no
 * SHOW TERMINAL name, so the two agree on every displayed characteristic. */
#define DNET_CTH_INIT_P_VMS_TERMCHAR 4
#define DNET_CTH_DC_TERM             66
struct dnet_cth_termchar {
    int      valid;         /* a DC$_TERM value of at least 8 bytes arrived */
    uint8_t  devclass;      /* DC$_ */
    uint8_t  devtype;       /* DT$_ */
    uint16_t width;
    uint8_t  page;
    int      have_tt2;      /* the value reached bytes 8-11                 */
    uint32_t ttchar;        /* TT$ bytes 4-6, RAW (not mapped)              */
    uint32_t tt2char;       /* TT2$ bytes 8-11, RAW (not mapped)            */
};
/* Decode one parameter-4 VALUE. DNET_CTH_OK with out->valid = 1 only for a
 * DC$_TERM value at least 8 bytes long; a shorter value is ETRUNC and a
 * different device class EINVAL (out->valid = 0 either way). */
int dnet_cth_vms_termchar_parse(const uint8_t *v, size_t vlen, struct dnet_cth_termchar *out);

/*
 * dnet_cth_termchar_to_ttc - the conveyed TT$/TT2$ words as VMS_TTC_* bits to
 * set and to clear on the RTAn: (src/kernel/vms_ioctl.h). Every characteristic
 * the wire carries is set OR cleared from it; one the wire does not carry is
 * in neither mask, so the RTAn: keeps the value it was minted with. Not
 * carried: Interactive, VMS Style Input, DEC_CRT5, Ansi_Color (no TT$/TT2$ bit
 * in V7.3's TTDEF/TT2DEF), and Set_speed -- the oracle RTAn: prints Set_speed
 * while the conveyed TT2$V_SETSPEED is 0, so how that bit maps to the display
 * is not established and it is not used. Returns DNET_CTH_EINVAL unless
 * tc->valid; the TT2$ half is used only when tc->have_tt2.
 */
int dnet_cth_termchar_to_ttc(const struct dnet_cth_termchar *tc,
                             uint64_t *setchar, uint64_t *clrchar);

/* What a server's Initiate told us. Absent parameters stay 0. */
struct dnet_cth_peer_init {
    uint8_t  version[3];
    uint16_t max_msg;       /* parameter 1 */
    uint16_t input_buf;     /* parameter 2 */
    int      have_bitmap;   /* parameter 3 present */
    struct dnet_cth_termchar term;  /* parameter 4 (VMS), when it decoded */
};
int dnet_cth_initiate_parse(const uint8_t *msg, size_t len, struct dnet_cth_peer_init *out);

/* CTERM Characteristics carrying one selector/value pair. */
int dnet_cth_char_build(uint16_t selector, const uint8_t *value, size_t vlen,
                        uint8_t *buf, size_t cap, size_t *outlen);

/* CTERM Write: `07 <flags LE16> <prefix> <postfix> <data>`. */
int dnet_cth_write_build(uint16_t flags, uint8_t prefix, uint8_t postfix,
                         const uint8_t *data, size_t dlen,
                         uint8_t *buf, size_t cap, size_t *outlen);

/* CTERM Start Read. `prompt` becomes the buffer's read-only section (DATA,
 * END-OF-DATA = END-OF-PROMPT = its length). TERMINATION-SET goes as the one
 * empty byte both VAX hosts sent with a universal terminator set. */
struct dnet_cth_read_req {
    uint32_t flags;        /* DNET_CTH_RD_* (24 bits on the wire)          */
    uint16_t max_length;
    uint16_t timeout;      /* seconds; meaningful iff DNET_CTH_RD_TIMED    */
};
int dnet_cth_start_read_build(const struct dnet_cth_read_req *rq,
                              const uint8_t *prompt, size_t plen,
                              uint8_t *buf, size_t cap, size_t *outlen);
/* Decode a Start Read (the test reads the VAX host's own back). */
struct dnet_cth_start_read {
    uint32_t flags;
    uint16_t max_length, end_of_data, timeout, end_of_prompt, start_of_display, low_water;
    size_t   termset_len;
    const uint8_t *data;
    size_t   dlen;
};
int dnet_cth_start_read_parse(const uint8_t *msg, size_t len, struct dnet_cth_start_read *out);

/* Decode a Read Data (sec 4.16.4). `data` is the whole DATA field (terminator
 * included); `term_pos` counts the data characters before any terminator. */
struct dnet_cth_read_data {
    uint8_t  flags;          /* low nibble = completion code */
    uint16_t low_water;
    uint8_t  vpos, hpos;
    uint16_t term_pos;
    const uint8_t *data;
    size_t   dlen;
};
int dnet_cth_read_data_parse(const uint8_t *msg, size_t len, struct dnet_cth_read_data *out);

/* ---- the host session FSM ------------------------------------------------ */

enum dnet_cth_state {
    DNET_CTH_S_IDLE = 0,     /* link up, nothing sent                          */
    DNET_CTH_S_BIND_SENT,    /* Bind Request out, awaiting Bind Accept         */
    DNET_CTH_S_INIT_SENT,    /* our Initiate out, awaiting the server's        */
    DNET_CTH_S_BOUND,        /* terminal I/O flows                             */
    DNET_CTH_S_UNBOUND       /* Unbind sent or received: session over          */
};

struct dnet_cth {
    enum dnet_cth_state state;
    struct dnet_cth_peer_init peer;
    size_t   seg_max;                 /* our emit cap after the peer's Initiate */
    unsigned idle_ms;                 /* output quiet time before a flush       */

    int      read_active;             /* a Start Read is outstanding            */
    uint64_t last_out_ms;             /* when terminal output last arrived      */
    uint64_t last_event_ms;           /* last output/read-completion            */

    uint8_t  out[DNET_CTH_OUT_MAX];   /* terminal output not yet on the wire    */
    size_t   outlen;

    uint8_t  in[DNET_CTH_IN_MAX];     /* typed input not yet given the terminal */
    size_t   inlen;

    uint8_t  echo[DNET_CTH_ECHO_MAX]; /* substrate echo to drop (see .c)        */
    size_t   echolen, echopos;

    uint8_t  txq[DNET_CTH_TXQ][DNET_CTH_SEG_MAX];
    size_t   txlen[DNET_CTH_TXQ];
    unsigned txhead, txcount;

    int      peer_unbound;            /* the server sent Unbind                 */
    uint16_t peer_unbind_reason;
    unsigned long rx_msgs, rx_ignored, reads_sent, reads_done, writes_sent;
};

/* Initialise. `idle_ms` = how long terminal output must be quiet before it is
 * flushed (and, when no read is outstanding, a Start Read issued). */
void dnet_cth_init(struct dnet_cth *h, unsigned idle_ms);

/* Link is up: the host speaks first (queues the Bind Request). */
int dnet_cth_open(struct dnet_cth *h);

/* One NSP data segment from the server. DNET_CTH_EPROTO means the session
 * must end (the caller unbinds/disconnects); any other error is a dropped,
 * malformed segment. */
int dnet_cth_rx(struct dnet_cth *h, const uint8_t *seg, size_t len, uint64_t now_ms);

/* Bytes the session process wrote to its terminal. Never blocks; returns the
 * count accepted (the buffer is bounded; a full buffer forces a flush). */
size_t dnet_cth_term_output(struct dnet_cth *h, const uint8_t *bytes, size_t n,
                            uint64_t now_ms);

/* Advance time. `term_echo` = the session terminal currently echoes (a read
 * issued now is a normal read, else a no-echo read -- the Password: case). */
int dnet_cth_tick(struct dnet_cth *h, uint64_t now_ms, int term_echo);

/* Take typed input for the session terminal. If `term_echo`, the substrate
 * terminal will echo what is written to it; the remote server has ALREADY
 * echoed it (CTERM echo is the server's job), so that echo is expected and
 * dropped from the next output. Returns the byte count. */
size_t dnet_cth_term_input(struct dnet_cth *h, uint8_t *buf, size_t cap, int term_echo);

/* The session process exited: flush output, then Unbind (reason 3). */
int dnet_cth_close(struct dnet_cth *h);

/* Pop one segment to send. Returns 1 with *outlen set, 0 when empty. */
int dnet_cth_tx_pop(struct dnet_cth *h, uint8_t *buf, size_t cap, size_t *outlen);

int dnet_cth_is_bound(const struct dnet_cth *h);
int dnet_cth_is_over(const struct dnet_cth *h);

#ifdef __cplusplus
}
#endif

#endif /* DNET_CTERM_HOSTFSM_H */
