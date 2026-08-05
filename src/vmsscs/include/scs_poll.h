/*
 * scs_poll.h - the SCS Process Poller (vms-66f), VAXcluster Principles p. 2-50.
 *
 * WHAT THE BOOK REQUIRES, and every clause below is implemented or explicitly
 * marked as not:
 *
 *   "So how does SYSAP_A on NODE_A become aware that it has a counterpart
 *    SYSAP_X on NODE_X that is listening for connect requests? SCA requires
 *    that there be an SCS Directory Service on each node that answers 'Yes' or
 *    'No' when asked if a particular SYSAP name is present in its list of
 *    listening SYSAPs. While not required, SCA also suggests that the actual
 *    inquiry mechanism be implemented by means of polling; and VMS follows this
 *    suggestion." (p. 2-50)
 *
 *   "Periodically, the Process Poller on VAX_A connects to the Directory
 *    Service on NODE_X, and the Directory Service accepts the connection. The
 *    Process Poller then sends messages to the Directory Service, asking if
 *    there is a SYSAP_X or a SYSAP_W in NODE_X's list of listening SYSAPs. The
 *    Directory Service answers 'Yes' ... but 'No' ... SYSAP_A is notified about
 *    the discovery of SYSAP_X ... However, SYSAP_B is not notified about the
 *    absence of SYSAP_W since doing so would be a waste of CPU time and
 *    resources on VAX_A. After the Process Poller has received replies to all
 *    of its inquires, the Process Poller and Directory Service disconnect from
 *    each other." (p. 2-50)
 *
 *   "Both of them are SYSAPs, and thus have SYSAP names. The SCS Directory
 *    Service is called SCS$DIRECTORY, and the SCS Process Poller is called
 *    SCS$DIR_LOOKUP." (p. 2-50)  -> SCS_DIR_SYSAP_POLLER / _DIRECTORY.
 *
 *   "The SYSGEN parameter PRCPOLINTERVAL specifies in seconds the SCS process
 *    polling interval ... A VMS system will poll each other node that it can
 *    see approximately once, but not more than once, during each such interval.
 *    The word approximate is used here since only one node will be polled at a
 *    time. If two nodes are both due for polling, one of them will have to wait
 *    approximately one second." (p. 2-50)
 *    -> scs_poll_interval_sec(), SCS_POLL_ONE_AT_A_TIME_MS, and the single
 *       `state`/`cur` pair that makes ONE cycle in flight structurally
 *       impossible to exceed.
 *
 *   "When requesting the Process Poller to inquire about a particular SYSAP
 *    name, the option exists to request polling on a specific node, or on all
 *    nodes. VMS software, in fact, requests polling for SYSAP names on all
 *    nodes." (p. 2-50)  -> scs_poll_request(..., node == NULL) is all-nodes.
 *
 *   "Once SYSAP_A has established a connection with SYSAP_X, it will disable
 *    polling for SYSAP_X on NODE_X. But VAX_A's Process Poller will continue to
 *    look for SYSAP_X on nodes other than NODE_X. ... If that connection is
 *    lost, SYSAP_A has the option of once again requesting its Process Poller
 *    to look for SYSAP_X on NODE_X." (p. 2-50)
 *    -> scs_poll_connected() / scs_poll_connection_lost(), per (name,node) pair.
 *
 * WHAT THIS IS NOT. The poller does not open the discovered connection itself;
 * it NOTIFIES ("SYSAP_A is notified about the discovery of SYSAP_X; and SYSAP_A
 * can then connect"). The connect is the interested SYSAP's, through
 * scs_connect(). The poller's OWN connection to the remote SCS$DIRECTORY is a
 * real SCS connection made with the same service (vms-561), and it is torn down
 * with scs_disconnect() -- whose wire frame vms-591 owns, see the DISCONNECT
 * note on scs_poll_answer().
 *
 * PRCPOLINTERVAL IS GROUNDED, NOT GUESSED. Measured on the reference VAX 7.3
 * (lab-2 replica `vaxlab-1`, node VAX1, 2026-08-05) with documented tool output,
 * which is an oracle CLAUDE.md rule 8 allows:
 *
 *     $ MC SYSGEN SHOW/SCS
 *     Parameter Name        Current  Default   Min.    Max.    Unit   Dynamic
 *     PRCPOLINTERVAL             30       30      1   32767  Seconds        D
 *
 * so SCS_POLL_PRCPOLINTERVAL_DEFAULT is 30 with a [1,32767] range, all four
 * numbers read off a real VMS system rather than inferred from the prose.
 * Raw evidence kept at
 * /data/training/vax/k8s-labs/vaxlab-1/logs/sysgen-scs-vax1-20260805.txt.
 *
 * IT IS OFF BY DEFAULT, AND THAT IS A MEASURED DECISION, NOT TIMIDITY. Enabled
 * against the reference VAX it costs OVMX its cluster join outright; the
 * bracketed before/after numbers and the two wire facts that bound the failure
 * are in the comment above scs_poll_enabled() in scs_poll.c, and in
 * docs/cluster-protocol-spec.md sec 4(h). Turn it on with
 * OVMX_PROCESS_POLLER=1.
 *
 * KILL-SWITCH (wire-visible change, project standing constraint):
 * OVMX_NO_PROCESS_POLLER=1 forces scs_poll_tick() to a no-op from any state --
 * no CONNECT_REQ and no inquiry is built or sent and the poller emits nothing
 * at all. It overrides OVMX_PROCESS_POLLER.
 * OVMX_PRCPOLINTERVAL=<seconds> overrides the interval -- an OVMX addition
 * (VMS sets it with SYSGEN), used by the tests and by short lab runs.
 */
#ifndef SCS_POLL_H
#define SCS_POLL_H

#include <stddef.h>
#include <stdint.h>

#include "scs_cdt.h"
#include "scs_credit.h"
#include "scs_dir.h"
#include "scs_svc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GROUNDED from SYSGEN SHOW/SCS on the reference VAX (see header note). */
#define SCS_POLL_PRCPOLINTERVAL_DEFAULT 30u
#define SCS_POLL_PRCPOLINTERVAL_MIN     1u
#define SCS_POLL_PRCPOLINTERVAL_MAX     32767u

/* p. 2-50: "only one node will be polled at a time. If two nodes are both due
 * for polling, one of them will have to wait approximately one second." */
#define SCS_POLL_ONE_AT_A_TIME_MS 1000u

/* OVMX CHOICE, labeled (SCA states no bound). A cycle whose CONNECT is never
 * accepted, or whose inquiries are never all answered, is abandoned after this
 * so the single-cycle-at-a-time rule above cannot wedge the poller forever.
 * This is a local timeout only; it puts nothing on the wire. */
#define SCS_POLL_CYCLE_TIMEOUT_MS 5000u

#define SCS_POLL_MAX_NAMES 8
#define SCS_POLL_MAX_NODES 8

enum scs_poll_state {
    SCS_POLL_IDLE = 0,     /* between cycles */
    SCS_POLL_CONNECTING,   /* CONNECT_REQ sent to a remote SCS$DIRECTORY */
    SCS_POLL_INQUIRING,    /* connection open, inquiries outstanding */
    SCS_POLL_DISCONNECTING /* all inquiries answered, DISCONNECT invoked */
};

const char *scs_poll_state_name(enum scs_poll_state s);

/* p. 2-50's "SYSAP_A is notified about the discovery of SYSAP_X". Called ONLY
 * for an affirmative answer. */
typedef void (*scs_poll_found_fn)(const char *sysap, const uint8_t node[6], void *ctx);

/* The poller's message emitter: put ONE inquiry for `sysap` on `cdt` (which
 * rides the connection to `node`). Returns 1 if a frame went out, 0 if not.
 * Separate from scs_svc_emit_fn because an inquiry is a SYSAP message on an
 * open connection, not one of the five services' connection-control actions. */
typedef int (*scs_poll_inquiry_fn)(void *ctx, struct scs_cdt *cdt,
                                   const uint8_t node[6], const char *sysap);

/* One registered interest: a SYSAP name, its scope, and the nodes on which
 * polling for it is currently disabled because a connection already exists. */
struct scs_poll_name {
    int      in_use;
    char     sysap[SCS_DIR_NAME_LEN + 1];
    int      all_nodes;                 /* 1 = every node; 0 = `node` only */
    uint8_t  node[6];
    scs_poll_found_fn on_found;
    void    *ctx;
    uint8_t  disabled[SCS_POLL_MAX_NODES][6];
    unsigned disabled_count;
};

/* A node this system "can see" -- one with an open virtual circuit. */
struct scs_poll_node {
    int      in_use;
    uint8_t  addr[6];        /* 48-bit SCS System Address (the CONFIG_SYS key) */
    uint64_t last_poll_ms;   /* start of the last cycle against this node; 0 = never */
    int      polled_once;
};

struct scs_poller {
    struct scs_svc_port *port;
    struct scs_config   *cfg;
    scs_svc_emit_fn      emit;      /* builds CONNECT_REQ / DISCONNECT_REQ */
    void                *emit_ctx;
    scs_poll_inquiry_fn  inquire;   /* builds one lookup REQUEST */
    void                *inquire_ctx;

    struct scs_poll_name names[SCS_POLL_MAX_NAMES];
    struct scs_poll_node nodes[SCS_POLL_MAX_NODES];

    /* THE ONE CYCLE. p. 2-50's "only one node will be polled at a time" is not
     * enforced by a check; there is simply one of each of these. */
    enum scs_poll_state state;
    unsigned            cur;             /* nodes[] slot of the cycle in flight */
    uint8_t             cur_node[6];
    struct scs_cdt     *cdt;
    uint64_t            cycle_start_ms;
    uint64_t            last_cycle_ms;   /* for SCS_POLL_ONE_AT_A_TIME_MS */
    unsigned            next_scan;       /* round-robin cursor, so no node starves */

    /* Inquiries sent on the current connection and not yet answered. */
    char     pending[SCS_POLL_MAX_NAMES][SCS_DIR_NAME_LEN + 1];
    unsigned pending_count;

    /* Counters -- every claim about what the poller DID is a number a test can
     * read, not a sentence. */
    unsigned long cycles_started;
    unsigned long cycles_completed;   /* reached DISCONNECT with all replies in */
    unsigned long cycles_abandoned;   /* timed out before every reply was in */
    unsigned long disconnects_unclosed; /* all replies in, DISCONNECT invoked, the
                                         * dialogue never reached CLOSED */
    unsigned long disconnects_closed;   /* ... and the ones that DID: the p. 2-26
                                         * dialogue completed and the descriptor
                                         * came back. The honest counterpart of
                                         * the line above -- without it a run log
                                         * could only report teardown FAILURES. */
    unsigned long connect_refused;    /* scs_connect() did not reach the wire */
    uint8_t last_refused_node[6];     /* and WHICH node key it was refused for */
    enum scs_svc_status last_connect_status; /* WHY the last refusal happened --
                                        * a bare count cannot tell "no open
                                        * circuit" (p. 2-47) from "no CDT"
                                        * (p. 2-30) from "the port declined" */
    unsigned long inquiries_sent;
    unsigned long answers_yes;
    unsigned long answers_no;
    unsigned long answers_unknown;
    unsigned long answers_unsolicited; /* an answer for a name we did not ask about */
    unsigned long notifications;       /* on_found calls -- YES answers only */
    unsigned long skipped_disabled;    /* (name,node) pairs skipped per p. 2-50 */
    unsigned long descriptors_forced;  /* CDTs released without reaching CLOSED --
                                        * see poll_release_cdt() in scs_poll.c */
};

/* Zero the poller and point it at the service port and the config database.
 * `cfg` may be NULL, in which case CONNECT must be given a circuit some other
 * way and will simply be refused (p. 2-47) -- never faked. */
void scs_poll_init(struct scs_poller *p, struct scs_svc_port *port,
                   struct scs_config *cfg);

/* Supply the port-driver halves. Either may be NULL; a NULL `emit` makes every
 * CONNECT unemitted (scs_svc.h's NOBUILDER contract) and a NULL `inquire` makes
 * every inquiry unsent -- both honest, neither silently successful. */
void scs_poll_set_emitters(struct scs_poller *p,
                           scs_svc_emit_fn emit, void *emit_ctx,
                           scs_poll_inquiry_fn inquire, void *inquire_ctx);

/*
 * p. 2-50: "SYSAP_A ... request[s] VAX_A's Process Poller to periodically ask
 * the SCS Directory Service on NODE_X about SYSAP_X". `node` NULL means all
 * nodes, which is what VMS software actually asks for.
 * Returns 1 if registered, 0 if the table is full or the arguments are bad.
 * Re-registering an existing name updates its callback and RE-ENABLES it
 * everywhere -- that is p. 2-50's "once again requesting its Process Poller to
 * look for SYSAP_X on NODE_X" after a connection is lost.
 */
int scs_poll_request(struct scs_poller *p, const char *sysap,
                     const uint8_t node[6], scs_poll_found_fn on_found, void *ctx);

/* Register / forget a node the poller "can see". scsd calls these when a
 * virtual circuit opens and when a peer departs. Adding a node already present
 * is a no-op that preserves its cadence. Returns 1 on success. */
int  scs_poll_add_node(struct scs_poller *p, const uint8_t node[6]);
void scs_poll_drop_node(struct scs_poller *p, const uint8_t node[6]);

/* p. 2-50's disable/re-enable, per (SYSAP, node) pair. */
void scs_poll_connected(struct scs_poller *p, const char *sysap, const uint8_t node[6]);
void scs_poll_connection_lost(struct scs_poller *p, const char *sysap, const uint8_t node[6]);

/* Would the poller ask NODE about SYSAP right now? 1/0. This is the p. 2-50
 * disable rule as a readable predicate. */
int scs_poll_polling(const struct scs_poller *p, const char *sysap, const uint8_t node[6]);

/*
 * The cadence. Starts at most ONE cycle: picks the least-recently-polled node
 * that is due (now - last_poll >= PRCPOLINTERVAL) and has at least one
 * still-enabled name, and invokes CONNECT on it. Does nothing at all while a
 * cycle is in flight, while less than SCS_POLL_ONE_AT_A_TIME_MS has passed
 * since the last one started, or when OVMX_NO_PROCESS_POLLER is set.
 */
void scs_poll_tick(struct scs_poller *p, uint64_t now_ms);

/*
 * The remote directory accepted the poller's connection ("and the Directory
 * Service accepts the connection"). Sends one inquiry per enabled name for the
 * node being polled. If nothing could be sent the cycle is closed immediately
 * rather than left hanging. Returns the number of inquiries sent.
 */
unsigned scs_poll_opened(struct scs_poller *p, uint64_t now_ms);

/*
 * One reply. `answer` comes from scs_dir_parse()'s view. YES notifies the
 * interested SYSAP; NO and UNKNOWN notify nobody (p. 2-50: "SYSAP_B is not
 * notified about the absence of SYSAP_W").
 *
 * When the last outstanding inquiry is answered this invokes scs_disconnect()
 * -- "After the Process Poller has received replies to all of its inquires, the
 * Process Poller and Directory Service disconnect from each other." WHAT
 * REACHES THE WIRE IS NOT THIS FILE'S CLAIM, it is the emitter's: this file
 * asks for SCS_CONN_ACT_SEND_DISCONNECT_REQ and counts whichever of the three
 * scs_svc.h answers it gets. As of vms-591 the DISCONNECT_REQ builder EXISTS
 * (src/vmsscs/scs_disc.c) and scsd.c's poller emitter drives it, so in the
 * daemon the frame goes out and port->emitted counts it; an embedding whose
 * emitter has no builder still gets the honest NOBUILDER path and
 * port->unemitted.
 *
 * Returns 1 if the answer matched an outstanding inquiry, 0 otherwise.
 */
int scs_poll_answer(struct scs_poller *p, const char *sysap,
                    enum scs_dir_answer answer, uint64_t now_ms);

/* The poller's connection was lost / the cycle must end. Releases the cycle
 * without claiming any frame went out. */
void scs_poll_abandon(struct scs_poller *p);

/*
 * p. 2-26's release, told to the SYSAP that owned the connection.
 *
 * WHY THIS EXISTS AND IS NOT A POLLING CHECK. The poller does not own the
 * receive path; scsd.c does. When the peer's DISCONNECT_RSP and matching
 * DISCONNECT_REQ arrive, scs_svc_deliver() takes the connection to CLOSED and
 * RELEASES the CDT there and then -- the poller's own scs_svc_close_if_closed()
 * retry in scs_poll_tick() would find `in_use == 0` and answer "not closed",
 * and the cycle would sit in DISCONNECTING until its timeout and be counted in
 * disconnects_unclosed. That is a teardown that COMPLETED being reported as one
 * that did not, so the release has to be pushed, not polled.
 *
 * IDENTITY, NOT STATE. It compares the CDT POINTER, and the caller must call it
 * at the moment of release, before the CDL can hand the slot to another
 * connection -- a later "is it still in use?" test cannot tell a released
 * descriptor from a recycled one.
 *
 * Returns 1 if `cdt` was this poller's cycle descriptor (and the cycle was
 * ended), 0 otherwise -- so a caller can tell whose connection it just closed.
 */
int scs_poll_cdt_released(struct scs_poller *p, const struct scs_cdt *cdt);

/*
 * THE PEER IS DEPARTING AND ITS PATH BLOCK IS ABOUT TO BE SWEPT (vms-096).
 *
 * THE BUG THIS CLOSES, WHICH WAS REAL. scs_pb_depart() releases EVERY CDT
 * queued to a departing peer's Path Block -- including the poller's in-flight
 * cycle descriptor, because the poller's connection is an ordinary CDT on that
 * circuit. It does not, and cannot, know the poller exists: src/vmsscs
 * layering puts scs_depart.c under scs_poll.c, not over it. So the poller kept
 * `p->cdt` pointing at a released slot and stayed in CONNECTING / INQUIRING /
 * DISCONNECTING. The next scs_poll_abandon() -- a timeout, or the next tick's
 * cycle change -- then called scs_cdl_release() on WHATEVER now occupied that
 * CDL slot. If another connection had been allocated into it in the meantime,
 * that connection was torn down under its owner and its MFREEQ/DFREEQ deposit
 * was subtracted from the port: a recycled-slot release.
 *
 * WHY A SEPARATE ENTRY POINT rather than reusing scs_poll_cdt_released(): the
 * sweep must be told BEFORE the release, while the CDT is still queued to the
 * Path Block and can still be recognised as being on it. Afterwards
 * scs_cdl_release() has memset the descriptor and the `pb` link is gone.
 *
 * Call it immediately before scs_pb_depart() on the same Path Block.
 *
 * Returns 1 if the poller's cycle was on `pb` (the cycle is ended and counted
 * as abandoned, and NOTHING is released here -- the sweep does that), else 0.
 */
int scs_poll_pb_departing(struct scs_poller *p, const struct scs_pb *pb);

enum scs_poll_state scs_poll_state_of(const struct scs_poller *p);
unsigned scs_poll_pending(const struct scs_poller *p);
const uint8_t *scs_poll_current_node(const struct scs_poller *p);

/* PRCPOLINTERVAL in seconds, clamped to the GROUNDED [1,32767] SYSGEN range.
 * Read fresh on every call (never cached) so a test can bracket one call with
 * setenv -- the scs_svc_descriptors_available() convention. */
unsigned scs_poll_interval_sec(void);

/* 0 when OVMX_NO_PROCESS_POLLER is set. Read fresh on every call. */
int scs_poll_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* SCS_POLL_H */
