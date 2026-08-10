/*
 * scs_quorum.h - connection-manager CLUSTER EXPECTED VOTES (CEVOTES) and
 * QUORUM computation, and the quorum gate (vms-7a9).
 *
 * WHAT THIS IS. The connection manager, at every cluster state transition (a
 * node joins or leaves, or a quorum disk is recognised), recomputes two dynamic
 * values over the SELECTED membership:
 *
 *   New CEVOTES = max{ EXPECTED_VOTES(selected) ;
 *                      SUM VOTES(selected) ;
 *                      Old CEVOTES }
 *   QUORUM      = (New CEVOTES + 2) / 2       (integer division, "rounded down")
 *
 * and then compares the present cluster votes (SUM VOTES over the members that
 * are currently up) against QUORUM:
 *
 *   present votes >= QUORUM  -> cluster RUNS
 *   present votes <  QUORUM  -> quorum is LOST: every surviving member SUSPENDS
 *                               all process activity and I/O to cluster-accessible
 *                               devices and WAITS until enough votes return. The
 *                               connection manager NEVER decreases CEVOTES/QUORUM
 *                               on its own, and it does NOT reconfigure to drop a
 *                               member while quorum is lost.
 *
 * GROUNDING (CLAUDE.md rule 8). The ALGORITHM is public-documented, not
 * wire-mined:
 *   - VMScluster Systems for OpenVMS, sec 2.3.5 "Calculating Cluster Votes"
 *     (the max-of-three quorum recomputation and the "never decreases" rule),
 *     sec 2.3.3 quorum definition, sec 2.3.6 the 3-node worked example.
 *   - The mined connection-manager transcript, ch7-part01 pp. 7-5..7-7, carries
 *     the same recomputation as `New CEVOTES = max{EXPECTED_VOTES; SUM VOTES;
 *     Old CEVOTES}`, `QUORUM = (New CEVOTES + 2)/2`, plus the 5-node worked
 *     example this module's unit test reproduces (HOST-ONLY transcript, cited by
 *     page, never committed).
 *
 * WHY THE DYNAMICS ARE NOT WIRE-GROUNDED (vms-41d / vms-2d6). Every reference-lab
 * capture ran a single vote configuration (VOTES 1/0/0, EXPECTED_VOTES held at 1
 * in every config, F$GETSYI QUORUM=1), so there is NO wire contrast that binds
 * the quorum arithmetic to any byte. The quorum DYNAMICS therefore come from the
 * documented algorithm above, proven by the unit test against the worked
 * example -- NOT from a fabricated wire value. The ONLY wire-grounded piece is
 * the per-node VOTES field itself, at the 190-byte VC body[22:24] (spec sec 4j,
 * grounded across four vote configurations) -- consumed here, advertised by
 * scs_member_build_params(). vms-2d6 grounded the quorum-loss BEHAVIOUR: when the
 * only voting node was killed, the survivors went silent and did NOT reconfigure.
 * scs_quorum_gate() models exactly that: block-and-wait, no transition.
 *
 * SCOPE. Pure state + arithmetic: builds no frame, parses no frame, opens no
 * socket. The connection manager feeds it each member's advertised VOTES and
 * up/down state and consults the gate before cluster activity.
 */
#ifndef SCS_QUORUM_H
#define SCS_QUORUM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded member table. A VMScluster tops out well under this; the connection
 * manager tracks at most a handful of nodes plus the (optional) quorum disk. */
#define SCS_QUORUM_MAX_MEMBERS 96

/* One selected cluster member's vote contribution. */
struct scs_quorum_member {
    uint32_t node_id;        /* stable per-member key (SCSSYSTEMID or Con.ID) */
    uint16_t votes;          /* this member's VOTES (VC body[22:24], sec 4j)  */
    uint16_t expected_votes; /* this member's EXPECTED_VOTES (RE gap on wire;
                              * see header -- held at 1 in every capture) */
    int      up;             /* 1 = currently a member; 0 = departed/failed.
                              * A down member still counts toward EXPECTED_VOTES
                              * history via Old CEVOTES (never decreases) but
                              * contributes 0 to PRESENT votes. */
};

/* The connection manager's quorum model for one cluster. */
struct scs_quorum {
    struct scs_quorum_member members[SCS_QUORUM_MAX_MEMBERS];
    size_t   n_members;
    uint32_t quorum_disk_votes; /* QDSKVOTES if a quorum disk is present & up */
    int      quorum_disk_present;

    /* Recomputed by scs_quorum_recompute(): */
    uint32_t cevotes;       /* current CLUSTER EXPECTED VOTES (monotonic up)   */
    uint32_t quorum;        /* (cevotes + 2) / 2                               */
    uint32_t present_votes; /* SUM VOTES over up members (+ quorum disk if up) */
};

/* The gate decision the connection manager acts on. */
enum scs_quorum_gate_result {
    SCS_QUORUM_RUN   = 0, /* present_votes >= quorum: cluster activity proceeds */
    SCS_QUORUM_BLOCK = 1, /* present_votes <  quorum: suspend activity and WAIT
                           * (do NOT reconfigure -- vms-2d6) */
};

/* Reset the model to an empty membership with CEVOTES/QUORUM at zero. */
void scs_quorum_init(struct scs_quorum *q);

/*
 * scs_quorum_set_member - add or update a member's vote contribution, keyed by
 * node_id. Returns 0, -1 on NULL/overflow. Does NOT recompute; call
 * scs_quorum_recompute() after a batch of updates (i.e. once per transition).
 */
int scs_quorum_set_member(struct scs_quorum *q, uint32_t node_id,
                          uint16_t votes, uint16_t expected_votes, int up);

/* Mark a known member up (1) or down (0). Returns 0, -1 if node_id unknown. */
int scs_quorum_set_up(struct scs_quorum *q, uint32_t node_id, int up);

/* Configure the (optional) quorum disk contribution. */
void scs_quorum_set_disk(struct scs_quorum *q, uint32_t qdskvotes, int present);

/*
 * scs_quorum_recompute - the documented state-transition recomputation:
 *   New CEVOTES = max{ max EXPECTED_VOTES(all) ; SUM VOTES(all) ; Old CEVOTES }
 *   QUORUM      = (New CEVOTES + 2) / 2
 * "all" = every member in the table (up OR down) plus the quorum disk, because
 * CEVOTES is a historical high-water mark that the connection manager never
 * decreases. PRESENT votes counts only UP members (+ an up quorum disk).
 * Updates q->cevotes, q->quorum, q->present_votes. Idempotent.
 */
void scs_quorum_recompute(struct scs_quorum *q);

/* 1 if present_votes >= quorum (cluster has quorum), else 0. Reads the last
 * recompute; call scs_quorum_recompute() first after any change. */
int scs_quorum_present(const struct scs_quorum *q);

/* The gate the connection manager consults before cluster activity. */
enum scs_quorum_gate_result scs_quorum_gate(const struct scs_quorum *q);

#ifdef __cplusplus
}
#endif

#endif /* SCS_QUORUM_H */
