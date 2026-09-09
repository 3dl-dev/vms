/*
 * dnet_cterm.h - DECnet Phase IV CTERM (Command Terminal) message protocol,
 *                the terminal-service LAYERED PRODUCT behind $ SET HOST
 *                (engine rung 3, rd vms-4d2 / epic vms-30e; north-star demo
 *                leg vms-e4dc).
 *
 * WHAT THIS IS. CTERM is the DECnet application protocol that carries an
 * interactive terminal session between two nodes: it is what $ SET HOST 0::NODE
 * speaks. The local node where SET HOST is typed becomes a *slave terminal* for
 * the remote node; the remote node runs LOGINOUT/DCL and drives that terminal.
 * This module owns the CTERM protocol -- the Bind that establishes the terminal
 * session, the terminal-characteristics negotiation, and the read/write/OOB
 * messages that carry keystrokes up and screen output down -- riding on an
 * established NSP logical link (dnet_link.{c,h}, rd vms-c23).
 *
 * LAYER BOUNDARY. Like the NSP codec (dnet_nsp) and the logical-link FSM
 * (dnet_link) below it, this is a PURE byte/state library: no socket, no
 * thread, no allocation, no wall clock. A CTERM PDU is the opaque higher-layer
 * payload that rides *inside* an NSP data segment: the caller (a SET HOST client
 * / a CTERM server object driven by the engine) builds a CTERM PDU here and
 * hands it to dnet_engine_link_send(); on a DNET_LINK_EV_DATA it hands the
 * delivered bytes (engine rx_data) to dnet_cterm_rx() here. This module never
 * touches an NSP header, a routing header, or the wire. It sits at design
 * sec-4 layer 4 (Session Control) / 6 (SET HOST), above the engine boundary.
 *
 * RULE 1 -- "do it like VMS, or HIDE it." The VMS-visible face of this protocol
 * is $ SET HOST / an interactive terminal session (connect to a node, type, see
 * output, log out), never a raw CTERM frame or a socket. The client/server that
 * drive this FSM present the SET HOST surface a DCL user sees.
 *
 * SCOPE (rung 3, rd vms-4d2): the CTERM PROTOCOL + the client side of $ SET HOST
 * -- initiate a terminal session to a remote node's CTERM object, negotiate
 * terminal characteristics, carry keystrokes (Read Data / OOB) and screen output
 * (Write / Start Read), and tear the session down (Unbind). The protocol server
 * side (accept an inbound Bind, negotiate, exchange terminal I/O) is here too so
 * the two-engine on-wire proof exercises both ends; but WIRING THE SERVER TO A
 * REAL LOGIN -- spawning a PTY + LOGINOUT/DCL and bridging it to the CTERM
 * read/write messages, the way src/vmsssh/vmssshd.c forkpty()s vmsdcl --login --
 * is filed as a child of vms-30e, not built in this rung. The live SET HOST vs a
 * real lab VAX/Alpha is the vms-aac0-class bracket (a later coordinated lab run).
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8, HARD; docs/decnet-provenance-
 * register.md sec 4.7). CTERM is ENTIRELY SPEC-DERIVED: there is NO oracle
 * specimen for it. The vms-3be lab capture (register sec 4.6) recorded only
 * Connect Initiate frames -- VAX1's SET HOST to the unconfigured VAX2 never
 * completed a logical link, so no CTERM byte ever crossed the captured wire. The
 * message SET and their FUNCTION (Bind/Unbind, characteristics negotiation,
 * Start Read/Read Data, Write/Write Complete, Out-of-Band) mirror the public DEC
 * DNA Phase IV Command Terminal (CTERM) Message Protocol Functional
 * Specification. The specific numeric message-type codes and body field layouts
 * below are an OVMX-ASSIGNED, self-consistent CTERM namespace derived from that
 * public functional description; because no second public source or lab specimen
 * cross-checks the exact bytes, they are LABELLED spec-derived / OVMX-assigned
 * and are proven ONLY by the encode/decode round-trip and the two-endpoint
 * session round-trip (test_dnet_cterm.c) -- never presented as oracle-verified
 * VMS-authentic bytes, and no specimen bytes are fabricated. The well-known
 * CTERM session-control OBJECT NUMBER (42) is a published DNA well-known object
 * number. This mirrors exactly how dnet_link labels its spec-derived CC/data/
 * DI/DC choreography. No VSI/HPE/DEC source or binary was disassembled,
 * decompiled, or copied. When the vms-aac0 live bracket captures a real CTERM
 * session, these layouts become oracle-checkable and any delta is a tracked fix.
 */
#ifndef DNET_CTERM_H
#define DNET_CTERM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The well-known DECnet Phase IV Session Control OBJECT NUMBER for the command
 * terminal (CTERM) service. A published DNA well-known object number: $ SET HOST
 * originates an NSP Connect Initiate whose session-control connect message names
 * destination object 42. (Object 0 = task-to-task, 17 = FAL -- register sec 4.)
 */
#define DNET_CTERM_OBJECT   42

/*
 * The well-known DNA Session Control object numbers NETACP's object table
 * dispatches (design vms-515 §3.3; register sec 4). CTERM (42) is served today;
 * TASK (0, task-to-task) and FAL (17, File Access Listener) are DECLARED so the
 * executive object table can answer "a known object, not yet built" HONESTLY
 * (INV-6: an unbuilt object is refused, never faked into a fabricated session).
 */
#define DNET_OBJ_TASK    0
#define DNET_OBJ_FAL     17
#define DNET_OBJ_CTERM   DNET_CTERM_OBJECT   /* 42 */

/*
 * CTERM message types (OVMX-assigned within the CTERM namespace; spec-derived
 * from the public DNA CTERM functional description -- see the provenance block).
 * The first byte of every CTERM PDU. Split into the Foundation (session setup)
 * group and the terminal-I/O group, per the CTERM protocol structure.
 */
enum dnet_cterm_msgtype {
    /* --- Foundation: establish / negotiate / release the terminal session --- */
    DNET_CTERM_MSG_BIND        = 1,  /* terminal -> host: establish the session   */
    DNET_CTERM_MSG_BIND_ACCEPT = 2,  /* host -> terminal: session accepted         */
    DNET_CTERM_MSG_UNBIND      = 3,  /* either -> peer: release the session         */
    DNET_CTERM_MSG_CHARACTERISTICS = 4, /* either -> peer: terminal characteristics */
    /* --- terminal I/O ----------------------------------------------------- */
    DNET_CTERM_MSG_START_READ  = 5,  /* host -> terminal: solicit input (+ prompt) */
    DNET_CTERM_MSG_READ_DATA   = 6,  /* terminal -> host: input line + terminator  */
    DNET_CTERM_MSG_OOB         = 7,  /* terminal -> host: out-of-band control char */
    DNET_CTERM_MSG_WRITE       = 8,  /* host -> terminal: screen output            */
    DNET_CTERM_MSG_WRITE_COMPLETE = 9, /* terminal -> host: output drained         */
    DNET_CTERM_MSG_CLEAR_INPUT = 10, /* host -> terminal: flush type-ahead         */
    DNET_CTERM_MSG_DISCARD     = 11  /* host -> terminal: discard pending output   */
};

/* CTERM protocol version this OVMX implementation advertises in Bind (the DNA
 * CTERM version triple V.ECO.USER; OVMX-labelled values). */
#define DNET_CTERM_VER_V        1
#define DNET_CTERM_VER_ECO      0
#define DNET_CTERM_VER_USER     0

/* Bind mode: command-terminal mode (the only mode $ SET HOST uses). */
#define DNET_CTERM_MODE_COMMAND 0

/* Terminal-characteristics flag bits (OVMX-assigned; the DNA terminal
 * characteristics set, reduced to the ones SET HOST negotiates). */
#define DNET_CTERM_CH_ECHO        0x00000001u  /* host echoes input           */
#define DNET_CTERM_CH_WRAP        0x00000002u  /* wrap at right margin        */
#define DNET_CTERM_CH_BROADCAST   0x00000004u  /* broadcasts enabled          */
#define DNET_CTERM_CH_EIGHTBIT    0x00000008u  /* 8-bit terminal              */

/* Start Read flag bits (host's read request). */
#define DNET_CTERM_RD_NOECHO      0x0001u  /* do not echo (password prompt)   */
#define DNET_CTERM_RD_TIMED       0x0002u  /* timeout field is meaningful     */

/* Write flag bits (host's output request). */
#define DNET_CTERM_WR_PREFIX_NL   0x0001u  /* emit newline before the data    */
#define DNET_CTERM_WR_POSTFIX_NL  0x0002u  /* emit newline after the data     */
#define DNET_CTERM_WR_NOFORMAT    0x0004u  /* pass-through, no formatting      */

/* Unbind reason codes (OVMX-assigned; SET HOST uses NORMAL for a logout). */
#define DNET_CTERM_UNBIND_NORMAL   0
#define DNET_CTERM_UNBIND_ABORT    1

/* Bounds. A CTERM PDU rides in one NSP data segment; keep it well under the
 * negotiated segment size. Strings are counted (1-byte length + bytes). */
#define DNET_CTERM_MAX_DATA     512   /* Write/Read payload cap                */
#define DNET_CTERM_MAX_NAME     32    /* Bind terminal-identifier string cap   */
#define DNET_CTERM_MAX_PROMPT   64    /* Start Read prompt cap                 */
#define DNET_CTERM_MAX_PDU      (DNET_CTERM_MAX_DATA + 128) /* encoded PDU cap  */

/* Return codes (distinct namespace from the NSP/link codecs). */
#define DNET_CTERM_OK        0
#define DNET_CTERM_ETRUNC  (-1)   /* input too short for the PDU               */
#define DNET_CTERM_EBADLEN (-2)   /* declared/derived length inconsistent      */
#define DNET_CTERM_ENOSPACE (-3)  /* output buffer too small                   */
#define DNET_CTERM_EINVAL  (-4)   /* null argument / bad field                 */
#define DNET_CTERM_EBADTYPE (-5)  /* unrecognised message type                 */
#define DNET_CTERM_ESTATE  (-6)   /* operation invalid in the current state    */

/*
 * A decoded / to-be-encoded CTERM message (a flat tagged record, the same shape
 * discipline as struct dnet_nsp_msg). Only the fields relevant to `type` are
 * meaningful; the rest are zero after a decode.
 */
struct dnet_cterm_msg {
    uint8_t  type;          /* enum dnet_cterm_msgtype */

    /* Bind / Bind Accept */
    uint8_t  ver_v, ver_eco, ver_user; /* CTERM protocol version triple */
    uint8_t  mode;          /* Bind: DNET_CTERM_MODE_* */
    uint8_t  status;        /* Bind Accept: 0 = accepted, else reject reason */
    char     name[DNET_CTERM_MAX_NAME + 1]; /* terminal identifier (NUL-terminated) */

    /* Characteristics */
    uint8_t  term_type;     /* terminal type code (OVMX-labelled) */
    uint16_t width;         /* columns */
    uint16_t page;          /* page length (rows); 0 = /PAGE=0 */
    uint32_t char_flags;    /* DNET_CTERM_CH_* */

    /* Start Read */
    uint16_t rd_flags;      /* DNET_CTERM_RD_* */
    uint16_t rd_maxlen;     /* max input length */
    uint16_t rd_timeout;    /* seconds (meaningful iff RD_TIMED) */
    char     prompt[DNET_CTERM_MAX_PROMPT + 1]; /* prompt to display */

    /* Read Data */
    uint8_t  terminator;    /* terminating character (e.g. 0x0d CR) */

    /* OOB */
    uint8_t  oob_char;      /* the out-of-band control character */

    /* Write */
    uint16_t wr_flags;      /* DNET_CTERM_WR_* */

    /* Unbind */
    uint8_t  reason;        /* DNET_CTERM_UNBIND_* */

    /* Opaque terminal payload: Write output bytes or Read Data input bytes. */
    uint16_t datalen;
    uint8_t  data[DNET_CTERM_MAX_DATA];
};

/*
 * dnet_cterm_decode - decode one CTERM PDU from `buf` (which starts at the
 * message-type byte -- the caller has already delivered it out of the NSP data
 * segment). Fills *out and returns DNET_CTERM_OK; writes the consumed byte count
 * to *consumed when non-NULL. Returns a negative DNET_CTERM_E* on malformed
 * input. Never reads past buf[len-1].
 */
int dnet_cterm_decode(const uint8_t *buf, size_t len,
                      struct dnet_cterm_msg *out, size_t *consumed);

/*
 * dnet_cterm_encode - encode `msg` into `buf` as a CTERM PDU (message-type byte
 * onward -- no NSP header). Writes the byte count to *outlen when non-NULL.
 * Returns DNET_CTERM_OK, DNET_CTERM_ENOSPACE if `cap` is too small, or
 * DNET_CTERM_EINVAL/EBADTYPE on a malformed message.
 */
int dnet_cterm_encode(const struct dnet_cterm_msg *msg,
                      uint8_t *buf, size_t cap, size_t *outlen);

/* ======================================================================
 * DNA Session Control CONNECT message -- the inbound SET HOST's addressing
 * and access-control fields (rd vms-f40).
 *
 * ORACLE-GROUNDED (Rule 8): docs/oracle/vax-sethost-cterm.{md,pcap,wire.txt}
 * (rd vms-558), a real OpenVMS VAX V7.3 -> V7.3 `$ SET HOST VAX2` captured on
 * the lab bridge. The connect data of the Connect Initiate (pcap frame 5 and
 * its retransmission frame 46) is, byte for byte, TWENTY bytes:
 *
 *     00 2a | 02 00 1a 02 20 20 06 'S' 'Y' 'S' 'T' 'E' 'M' | 27 00 00 00 00
 *     ^^^^^   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^   ^^^^^^^^^^^^^^^
 *     DSTNAME  SRCNAME                                       MENUVER + the
 *                                                            THREE ACCESS-
 *                                                            CONTROL STRINGS
 *
 *   - DSTNAME  = FORMAT 0 (object number only), OBJTYPE 0x2a = 42 = CTERM.
 *     NOTE the format byte is 0, NOT 1: OVMX's first cut (#1013) emitted
 *     format 1 for "object number", which a real VAX would not have read as
 *     an object at all. Corrected here against the specimen.
 *   - SRCNAME  = FORMAT 2 (coded), OBJTYPE 0, GRPCODE 0x021a, USRCODE 0x2020,
 *     then a COUNTED STRING of 6 bytes: "SYSTEM" -- the SOURCE end user.
 *   - MENUVER  = 0x27 in the specimen, then FOUR ZERO BYTES: three (four with
 *     USRDATA) EMPTY counted strings. i.e. RQSTRID, PASSWRD and ACCOUNT are
 *     ALL EMPTY ON THE WIRE.
 *
 * THAT LAST FACT IS THE WHOLE SECURITY ARGUMENT OF vms-f40. The identity a
 * SET HOST carries ("SYSTEM") arrives in the SOURCE DESCRIPTOR, and the
 * access-control fields carry NOTHING -- there is no password on the wire to
 * authenticate with, and the oracle's console transcript agrees: the remote
 * prompts FRESH for Username AND Password and surfaces the carried identity
 * only as `Remote Port Info: 1025::SYSTEM`. An implementation that logged a
 * SET HOST in from the carried username would be admitting a session on ZERO
 * credential material. So: the source identity is PROXY/ACCOUNTING info, and
 * LOGINOUT authenticates from scratch.
 *
 * WE DELIBERATELY DO NOT RETAIN THE PASSWORD BYTES. The parser records that a
 * PASSWRD field was present and how long it was, and drops its content on the
 * floor. Nothing in OVMX may authenticate from it (the oracle says real VMS
 * does not), so keeping attacker-supplied plaintext alive in a long-lived
 * struct would be a liability with no consumer -- and its ABSENCE from the
 * struct is what makes "OVMX cannot auto-login from the wire" a structural
 * fact rather than a policy comment.
 *
 * EVERY FIELD IS BOUNDED. These bytes are ATTACKER-CONTROLLED (they arrive off
 * the wire before anyone has authenticated), and "OVMX never crashes a peer"
 * cuts both ways -- a malformed connect must be REJECTED cleanly, never
 * over-read. The parser never reads past buf[len-1], caps every counted string
 * and rejects (rather than truncates) an over-long one.
 * ====================================================================== */

/* DNA Session Control end-user descriptor FORMAT codes (oracle-confirmed for
 * 0 and 2 by the frame above; 1 is the published named-task form). */
#define DNET_SC_FMT_OBJECT  0   /* OBJTYPE only: a well-known object number  */
#define DNET_SC_FMT_NAMED   1   /* OBJTYPE=0 + counted task/image name       */
#define DNET_SC_FMT_CODED   2   /* OBJTYPE=0 + GRPCODE + USRCODE + name      */

/* Counted-string cap. DNA counted strings are 1..255 bytes; a Session Control
 * end-user name / access-control string longer than this is refused, not
 * clipped -- a clipped identity that happens to resolve is exactly the class
 * of bug this decoder must not have. */
#define DNET_SC_MAX_STR     64

/*
 * A decoded Session Control CONNECT message. Everything here is UNTRUSTED
 * input: it is what a peer sent us before authenticating.
 */
struct dnet_cterm_sc_connect {
    /* Destination (what object the peer asked for; 42 = CTERM/SET HOST). */
    uint8_t  dst_format;                    /* DNET_SC_FMT_*                 */
    uint8_t  dst_object;                    /* object number (format 0)      */
    char     dst_task[DNET_SC_MAX_STR + 1]; /* task name (formats 1/2)       */

    /* Source end user -- PROXY / ACCOUNTING IDENTITY ONLY, NEVER A CREDENTIAL
     * (see the block above). This is what becomes "Remote Port Info". */
    uint8_t  src_format;
    uint8_t  src_object;
    uint16_t src_grpcode;                   /* format 2 group code           */
    uint16_t src_usrcode;                   /* format 2 user code            */
    char     src_user[DNET_SC_MAX_STR + 1]; /* e.g. "SYSTEM"                 */

    /* Access control. Present in the message but EMPTY in the oracle. */
    uint8_t  menuver;                       /* option byte (0x27 observed)   */
    int      have_access_control;           /* the fields were present at all */
    char     rqstrid[DNET_SC_MAX_STR + 1];  /* requestor id, usually empty   */
    char     account[DNET_SC_MAX_STR + 1];  /* accounting string             */
    /* PASSWRD: length + presence ONLY. The bytes are never retained -- see
     * "WE DELIBERATELY DO NOT RETAIN THE PASSWORD BYTES" above. */
    int      password_present;
    uint8_t  password_len;
};

/*
 * dnet_cterm_sc_connect_build - build the Session Control CONNECT message a
 * $ SET HOST puts in the NSP Connect Initiate's connect data, in the
 * ORACLE-OBSERVED shape: DSTNAME = format 0 + `dst_object`, SRCNAME =
 * format 2 + objtype 0 + `src_grpcode`/`src_usrcode` + counted `src_user`,
 * then the MENUVER byte and the RQSTRID / PASSWRD / ACCOUNT counted strings.
 *
 * `src_user` is the local user this SET HOST is FROM -- proxy/accounting info,
 * exactly what the real VAX put there. `username`/`password`/`account` are the
 * access-control fields; the oracle shows a plain `SET HOST node` sends all
 * three EMPTY, so NULL/"" is the faithful call and what OVMX uses. (They exist
 * because DNA carries them and `SET HOST node"user pass"` fills them in; OVMX
 * never CONSUMES them on the inbound side -- LOGINOUT authenticates fresh.)
 *
 * Writes the length to *outlen. Returns DNET_CTERM_OK / ENOSPACE / EINVAL.
 */
int dnet_cterm_sc_connect_build(uint8_t dst_object,
                                const char *src_user,
                                uint16_t src_grpcode, uint16_t src_usrcode,
                                const char *username, const char *password,
                                const char *account,
                                uint8_t *buf, size_t cap, size_t *outlen);

/*
 * dnet_cterm_sc_connect_parse - decode a Session Control CONNECT message.
 * Fully bounded: never reads past buf[len-1]; refuses (does not clip) an
 * over-long counted string; refuses an unknown descriptor format. *out is
 * zeroed first, so a failure leaves no half-filled identity behind.
 * Returns DNET_CTERM_OK, or DNET_CTERM_ETRUNC / EBADLEN / EINVAL.
 */
int dnet_cterm_sc_connect_parse(const uint8_t *buf, size_t len,
                                struct dnet_cterm_sc_connect *out);

/* Parse the destination object number out of a Session Control connect message
 * (the inbound-Bind server dispatch: which object is being connected to).
 * Returns the object number (>=0) or DNET_CTERM_EINVAL on a malformed message. */
int dnet_cterm_sc_connect_object(const uint8_t *buf, size_t len);

/*
 * dnet_cterm_remote_port_info - render the VMS "Remote Port Info" string for a
 * decoded connect: "<decimal DECnet address>::<source user>", the shape the
 * oracle's SHOW TERMINAL printed ("Remote Port Info: 1025::SYSTEM", where 1025
 * = area 1 node 1). `src_addr` is the peer's Phase IV address as the routing
 * header carried it (executive/engine state -- NOT a value copied out of the
 * connect message). Returns DNET_CTERM_OK or DNET_CTERM_ENOSPACE/EINVAL.
 */
int dnet_cterm_remote_port_info(const struct dnet_cterm_sc_connect *sc,
                                uint16_t src_addr, char *out, size_t cap);

/*
 * ================= THE A2/A8 ISOLATION SEAM (design vms-515 §3.4) ============
 *
 * A VALIDATED, TYPED inbound-connect descriptor. This is the ONLY thing the
 * privileged control path (NETACP's session creation -- dnet_cterm_host_open_desc,
 * which mints an RTAn: and $CREPRCs LOGINOUT) is allowed to see. Attacker-
 * controlled wire bytes are parsed at LOW privilege by
 * dnet_conn_descriptor_from_wire() and NEVER cross into the privileged path.
 *
 * THE SEAM IS THE STRUCT'S SHAPE, not a promise:
 *   - there is NO pointer to, and NO length of, the wire frame here -- the
 *     privileged side has nothing raw to re-parse and cannot be handed a longer
 *     or differently-shaped buffer than the parser already bounded;
 *   - there are NO password bytes and no access-control material here -- the
 *     parser measured and dropped them (dnet_cterm.c), so no credential a peer
 *     supplied can reach a decision;
 *   - every string is already bounded (DNET_SC_MAX_STR) and printable-filtered;
 *   - `validated` is set ONLY by a successful bounded parse. A descriptor that
 *     did not come out of the parser is all-zero, validated == 0, and the
 *     privileged consumer REFUSES it (SS$_BADPARAM) rather than acting on a
 *     zero object. A fuzzed/malformed frame therefore produces a descriptor the
 *     privileged path rejects at its own front door -- it never mints a device
 *     or creates a process from hostile input.
 */
struct dnet_conn_descriptor {
    int      validated;       /* nonzero IFF produced by a successful bounded parse   */
    int      dst_is_object;   /* 1 = well-known object (format 0); 0 = named task     */
    uint8_t  dst_object;      /* destination object number (0..255), meaningful iff ^ */
    uint16_t peer_addr;       /* engine-decoded routing address -- NOT wire-supplied  */
    char     proxy_user[DNET_SC_MAX_STR + 1]; /* accounting/proxy identity ONLY       */
    char     proxy_task[DNET_SC_MAX_STR + 1]; /* named-task target (format 1/2)       */
};

/*
 * dnet_conn_descriptor_from_wire - the LOW-PRIVILEGE parse boundary. Decode the
 * untrusted NSP-connect bytes with the fully-bounded dnet_cterm_sc_connect_parse
 * and distil them into a validated descriptor. `peer_addr` is the engine's own
 * decode of the routing header (executive/engine state, never a value the peer
 * wrote into the connect), and is the ONLY address that reaches the descriptor.
 *
 * On success sets out->validated = 1 and returns DNET_CTERM_OK. On ANY parse
 * failure *out is left all-zero (validated == 0) and the parser's negative
 * error code is returned -- the caller must not, and cannot usefully, hand an
 * unvalidated descriptor to the privileged path.
 */
int dnet_conn_descriptor_from_wire(const uint8_t *conn_data, size_t conn_len,
                                   uint16_t peer_addr,
                                   struct dnet_conn_descriptor *out);

/*
 * dnet_conn_descriptor_port_info - render "<addr>::<user>" (the oracle's Remote
 * Port Info form) from a validated descriptor, for the accounting/human surface.
 * Uses the descriptor's already-filtered proxy_user and engine-decoded peer_addr.
 * Returns DNET_CTERM_OK or DNET_CTERM_EINVAL/ENOSPACE.
 */
int dnet_conn_descriptor_port_info(const struct dnet_conn_descriptor *desc,
                                   char *out, size_t cap);

/* ---- session state machine ---------------------------------------------- */

/* CTERM role: the SET HOST initiator is the TERMINAL (slave); the remote node is
 * the HOST (runs DCL / drives the terminal). */
enum dnet_cterm_role {
    DNET_CTERM_ROLE_TERMINAL = 0, /* $ SET HOST initiator (client) */
    DNET_CTERM_ROLE_HOST     = 1  /* remote CTERM server object    */
};

/* Session state. */
enum dnet_cterm_state {
    DNET_CTERM_S_CLOSED = 0, /* no session (link may be up, no Bind yet)    */
    DNET_CTERM_S_BINDING,    /* terminal: Bind sent, awaiting Bind Accept   */
    DNET_CTERM_S_BOUND,      /* session established: terminal I/O flows      */
    DNET_CTERM_S_UNBOUND     /* Unbind exchanged: session released           */
};

/* Higher-layer event a received CTERM PDU produced (drives the SET HOST face). */
enum dnet_cterm_event {
    DNET_CTERM_EV_NONE = 0,
    DNET_CTERM_EV_BIND_IND,       /* host: inbound Bind (a peer wants a session) */
    DNET_CTERM_EV_BOUND,          /* terminal: our Bind was accepted (session up) */
    DNET_CTERM_EV_CHARACTERISTICS,/* peer sent terminal characteristics           */
    DNET_CTERM_EV_START_READ,     /* terminal: host solicits input (prompt ready)  */
    DNET_CTERM_EV_READ_DATA,      /* host: terminal delivered an input line        */
    DNET_CTERM_EV_OOB,            /* host: terminal delivered an OOB control char  */
    DNET_CTERM_EV_WRITE,          /* terminal: host delivered screen output        */
    DNET_CTERM_EV_WRITE_COMPLETE, /* host: terminal drained the output             */
    DNET_CTERM_EV_CLEAR_INPUT,    /* terminal: host asked to flush type-ahead      */
    DNET_CTERM_EV_UNBOUND         /* peer released the session (Unbind in)         */
};

/*
 * A CTERM session. Pure state: no socket. Holds the role, the FSM state, the
 * negotiated characteristics, and honest counters. `last` carries the most
 * recently decoded inbound message so the caller can read its fields after an
 * event (the delivered terminal payload is in last.data / last.datalen).
 */
struct dnet_cterm_session {
    enum dnet_cterm_role  role;
    enum dnet_cterm_state state;

    /* Negotiated / advertised characteristics (the last set exchanged). */
    uint16_t width;
    uint16_t page;
    uint32_t char_flags;
    uint8_t  term_type;

    char     peer_name[DNET_CTERM_MAX_NAME + 1]; /* peer's Bind terminal id */

    struct dnet_cterm_msg last;  /* last decoded inbound message (post-rx) */

    /* Honest counters (reported on the SET HOST surface; never fabricated). */
    unsigned long writes_sent, writes_recv;
    unsigned long reads_sent, reads_recv;
    unsigned long oob_sent, oob_recv;
};

/*
 * dnet_cterm_session_init - initialise a CLOSED session in the given role.
 * Returns DNET_CTERM_OK or DNET_CTERM_EINVAL.
 */
int dnet_cterm_session_init(struct dnet_cterm_session *s, enum dnet_cterm_role role);

/*
 * dnet_cterm_bind - (TERMINAL role, CLOSED -> BINDING) build the Bind PDU that
 * opens a terminal session, advertising `term_name` as the terminal identifier.
 * Returns DNET_CTERM_OK, DNET_CTERM_ESTATE if not CLOSED/terminal, or E*.
 */
int dnet_cterm_bind(struct dnet_cterm_session *s, const char *term_name,
                    uint8_t *out, size_t cap, size_t *outlen);

/*
 * dnet_cterm_bind_accept - (HOST role, after a BIND_IND) build the Bind Accept
 * PDU (status 0), advertising `host_name`, and move the session to BOUND.
 * Returns DNET_CTERM_OK, DNET_CTERM_ESTATE, or E*.
 */
int dnet_cterm_bind_accept(struct dnet_cterm_session *s, const char *host_name,
                           uint8_t *out, size_t cap, size_t *outlen);

/*
 * dnet_cterm_send_characteristics - (either role, BOUND) build a Characteristics
 * PDU advertising the terminal's type/width/page and flag bits. Also records the
 * advertised values on the session. Returns DNET_CTERM_OK / ESTATE / E*.
 */
int dnet_cterm_send_characteristics(struct dnet_cterm_session *s,
                                    uint8_t term_type, uint16_t width,
                                    uint16_t page, uint32_t char_flags,
                                    uint8_t *out, size_t cap, size_t *outlen);

/*
 * dnet_cterm_start_read - (HOST role, BOUND) build a Start Read PDU soliciting
 * input, carrying the prompt to display and the read parameters. `prompt` may be
 * NULL/empty. Returns DNET_CTERM_OK / ESTATE / E*.
 */
int dnet_cterm_start_read(struct dnet_cterm_session *s, const char *prompt,
                          uint16_t rd_flags, uint16_t maxlen, uint16_t timeout,
                          uint8_t *out, size_t cap, size_t *outlen);

/*
 * dnet_cterm_read_data - (TERMINAL role, BOUND) build a Read Data PDU carrying an
 * input line (the user's keystrokes) and its terminating character. Returns
 * DNET_CTERM_OK / ESTATE / E*.
 */
int dnet_cterm_read_data(struct dnet_cterm_session *s, const uint8_t *data,
                         size_t len, uint8_t terminator,
                         uint8_t *out, size_t cap, size_t *outlen);

/*
 * dnet_cterm_oob - (TERMINAL role, BOUND) build an Out-of-Band PDU carrying a
 * single control character (e.g. ^C 0x03, ^Y 0x19). Returns OK / ESTATE / E*.
 */
int dnet_cterm_oob(struct dnet_cterm_session *s, uint8_t oob_char,
                   uint8_t *out, size_t cap, size_t *outlen);

/*
 * dnet_cterm_write - (HOST role, BOUND) build a Write PDU carrying screen output
 * bytes with the given write flags. Returns DNET_CTERM_OK / ESTATE / E*.
 */
int dnet_cterm_write(struct dnet_cterm_session *s, const uint8_t *data,
                     size_t len, uint16_t wr_flags,
                     uint8_t *out, size_t cap, size_t *outlen);

/*
 * dnet_cterm_unbind - (either role, BOUND -> UNBOUND) build an Unbind PDU that
 * releases the terminal session (e.g. the remote logged out). Returns
 * DNET_CTERM_OK / ESTATE / E*.
 */
int dnet_cterm_unbind(struct dnet_cterm_session *s, uint8_t reason,
                      uint8_t *out, size_t cap, size_t *outlen);

/*
 * dnet_cterm_rx - feed one inbound CTERM PDU (the payload delivered out of an NSP
 * data segment) into the session FSM. Decodes it into s->last, advances the
 * state, and reports the higher-layer event in *event (may be NULL). A message
 * invalid for the current state/role is reported honestly as DNET_CTERM_EV_NONE
 * with no fabricated transition. Returns DNET_CTERM_OK, or a negative
 * DNET_CTERM_E* on a null/undecodable PDU.
 */
int dnet_cterm_rx(struct dnet_cterm_session *s, const uint8_t *buf, size_t len,
                  enum dnet_cterm_event *event);

/* State accessors / names for the VMS-faithful surface + logs. */
static inline enum dnet_cterm_state dnet_cterm_state_of(const struct dnet_cterm_session *s)
{
    return s ? s->state : DNET_CTERM_S_CLOSED;
}
static inline int dnet_cterm_is_bound(const struct dnet_cterm_session *s)
{
    return s && s->state == DNET_CTERM_S_BOUND;
}
const char *dnet_cterm_state_name(enum dnet_cterm_state st);
const char *dnet_cterm_msgtype_name(enum dnet_cterm_msgtype t);

#ifdef __cplusplus
}
#endif

#endif /* DNET_CTERM_H */
