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

/*
 * dnet_cterm_sc_connect_build - build the minimal DNA Session Control CONNECT
 * message that $ SET HOST puts in the NSP Connect Initiate's connect data to
 * address the remote CTERM object: a destination object descriptor naming
 * `dst_object` (DNET_CTERM_OBJECT for SET HOST), a source object descriptor
 * (object 0), and the access-control username/password/account as counted
 * strings. SPEC-DERIVED (public DNA Session Control connect format); the
 * access-control username field is the one the vms-3be capture observed carrying
 * plaintext "SYSTEM" in specimen #3 (register sec 4.6). `username`/`password`/
 * `account` may be NULL (encoded as zero-length). Writes the length to *outlen.
 * Returns DNET_CTERM_OK / ENOSPACE / EINVAL.
 */
int dnet_cterm_sc_connect_build(uint8_t dst_object,
                                const char *username, const char *password,
                                const char *account,
                                uint8_t *buf, size_t cap, size_t *outlen);

/* Parse the destination object number out of a Session Control connect message
 * (the inbound-Bind server dispatch: which object is being connected to).
 * Returns the object number (>=0) or DNET_CTERM_EINVAL on a malformed message. */
int dnet_cterm_sc_connect_object(const uint8_t *buf, size_t len);

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
