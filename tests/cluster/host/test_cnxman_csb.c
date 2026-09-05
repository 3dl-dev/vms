/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_csb.c - the CLUB/CSB model and the CSB ten-state connectivity
 * ladder (FC-P3.6, test-ladder rung R1).
 *
 * WHAT IS GROUNDED, AND WHERE. Every state name and every edge asserted below
 * cites *VAXcluster Principles* (Davis 1993) by page: the ten connectivity
 * states and their descriptions are pp. 7-23/7-24, the last-gasp departure is
 * p. 7-29, the reconnect rules are p. 7-30, the CLUB's contents are p. 7-26,
 * and "the total number of members ... is simply the total number of CSBs that
 * have their SELECTED flag set" is p. 7-49. The transcript is host-only and
 * copyrighted: page cites only, never text (clean-room rule 8). No VSI/HPE
 * source or binary was read.
 *
 * WHAT THIS PROVES. Two different things, deliberately kept apart:
 *
 *   1. THE TABLE, EXHAUSTIVELY. Every (state, event) cell is dispatched.
 *      A cell the book puts an edge in must produce exactly that next state and
 *      that action; EVERY OTHER CELL must leave the CSB untouched, return NONE,
 *      and raise the CLUB's ignored-event counter. That last half is the real
 *      test: it is what stops a future edit from quietly inventing an edge the
 *      published description does not have.
 *
 *   2. THE MODEL'S INV-6 DISCIPLINE. A CSB whose CSID was never learned matches
 *      no CSID lookup -- including a lookup for 0 -- and projects with its
 *      validity flag clear. A full CSB table refuses rather than overwriting.
 *
 * The BEHAVIOURAL proofs (a real breakage, held membership, the once-a-second
 * cadence, expiry) are in test_cnxman_recnx.c, which never hand-sets a state.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "cnxman_fake_ops.h"

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_recnx_fsm.h"   /* CNXMAN_RECNX_DEFAULT_SECS */
#include "vms_cluster_codec_cm.h"   /* VMS_CM_BODY_LEN */

static struct vms_cluster g_cl;   /* the CLUB carries a 96-slot CSB table */
static struct cnxman_ops  g_ops;
static struct fake_cnx    g_fake;

static const uint8_t NODE_OVMX[6] = { 'O', 'V', 'M', 'X', '0', '1' };

static void cluster_reset(uint16_t recnxinterval)
{
	memset(&g_cl, 0, sizeof(g_cl));
	fake_ops_init(&g_ops, &g_fake);

	memcpy(g_cl.params.scsnode, NODE_OVMX, sizeof(NODE_OVMX));
	g_cl.params.scsnode_len = 6;
	g_cl.params.scssystemid = 0x000004000103ull;
	g_cl.params.votes = 0;             /* design D-10: non-voting first */
	g_cl.params.expected_votes = 3;
	g_cl.params.qdskvotes = 0;
	g_cl.params.lockdirwt = 0;
	g_cl.params.recnxinterval = recnxinterval;
}

/* ==========================================================================
 * 1. The ten states are the book's ten, in the book's order
 * ========================================================================== */
static void test_state_vocabulary(void)
{
	static const char *const want[10] = {
		"NEW", "CONNECT", "ACCEPT", "OPEN", "DISCONNECT",
		"WAIT", "RECONNECT", "REACCEPT", "DEAD", "LOCAL"
	};
	int i, ok = 1;

	printf("[csb] the ten connectivity states, pp. 7-23/7-24\n");
	ct_check_eq_u32(VMS_CNXMAN_CSB_STATE__COUNT, 10,
			"there are exactly ten connectivity states");
	ct_check_eq_u32(VMS_CNXMAN_CSB_NEW, 0, "NEW is the first (p. 7-23)");
	ct_check_eq_u32(VMS_CNXMAN_CSB_LOCAL, 9, "LOCAL is the last (p. 7-24)");

	for (i = 0; i < 10; i++) {
		if (strcmp(cnxman_csb_state_name((enum vms_cnxman_csb_state)i),
			   want[i]) != 0)
			ok = 0;
	}
	ct_check(ok, "all ten are named exactly as the book enumerates them");
	ct_check(strcmp(cnxman_csb_state_name((enum vms_cnxman_csb_state)99),
			"?") == 0,
		 "an out-of-range state name is '?', never a read past the table");
	ct_check(cnxman_csb_event_name(CNXMAN_CSB_EV_LAST_GASP) != NULL &&
		 cnxman_csb_action_name(CNXMAN_CSB_ACT_LAST_GASP) != NULL,
		 "event and action names are never NULL");
}

/* ==========================================================================
 * 2. The table, exhaustively
 * ========================================================================== */
struct ladder_case {
	uint8_t from;     /* enum vms_cnxman_csb_state */
	uint8_t ev;       /* enum cnxman_csb_event */
	uint8_t to;       /* enum vms_cnxman_csb_state */
	uint8_t act;      /* enum cnxman_csb_action */
	const char *cite;
};

/*
 * Every edge the published description puts in the table, with its page. Three
 * are marked INFERRED in vms_cnxman_csb.c (an initial connect that never
 * completes returns to NEW); they are asserted here too, so the inference is
 * visible and reviewable rather than buried.
 */
static const struct ladder_case ladder[] = {
	/* NEW -- "just been allocated ... a newly discovered remote CM" 7-23 */
	{ VMS_CNXMAN_CSB_NEW, CNXMAN_CSB_EV_CONNECT_SENT,
	  VMS_CNXMAN_CSB_CONNECT, CNXMAN_CSB_ACT_NONE, "7-24 CONNECT" },
	{ VMS_CNXMAN_CSB_NEW, CNXMAN_CSB_EV_CONNECT_RCVD,
	  VMS_CNXMAN_CSB_ACCEPT, CNXMAN_CSB_ACT_NONE, "7-24 ACCEPT" },
	{ VMS_CNXMAN_CSB_NEW, CNXMAN_CSB_EV_NEW_INCARNATION,
	  VMS_CNXMAN_CSB_DEAD, CNXMAN_CSB_ACT_NONE, "7-24 DEAD" },

	/* CONNECT -- our initial CONNECT is outstanding */
	{ VMS_CNXMAN_CSB_CONNECT, CNXMAN_CSB_EV_CONN_OPEN,
	  VMS_CNXMAN_CSB_OPEN, CNXMAN_CSB_ACT_NONE, "7-24 OPEN" },
	{ VMS_CNXMAN_CSB_CONNECT, CNXMAN_CSB_EV_CONN_LOST,
	  VMS_CNXMAN_CSB_NEW, CNXMAN_CSB_ACT_NONE, "INFERRED, see the .c" },
	{ VMS_CNXMAN_CSB_CONNECT, CNXMAN_CSB_EV_DISCONNECT,
	  VMS_CNXMAN_CSB_DISCONNECT, CNXMAN_CSB_ACT_NONE, "7-24 DISCONNECT" },
	{ VMS_CNXMAN_CSB_CONNECT, CNXMAN_CSB_EV_NEW_INCARNATION,
	  VMS_CNXMAN_CSB_DEAD, CNXMAN_CSB_ACT_NONE, "7-24 DEAD" },

	/* ACCEPT -- we are accepting their initial CONNECT */
	{ VMS_CNXMAN_CSB_ACCEPT, CNXMAN_CSB_EV_CONN_OPEN,
	  VMS_CNXMAN_CSB_OPEN, CNXMAN_CSB_ACT_NONE, "7-24 OPEN" },
	{ VMS_CNXMAN_CSB_ACCEPT, CNXMAN_CSB_EV_CONN_LOST,
	  VMS_CNXMAN_CSB_NEW, CNXMAN_CSB_ACT_NONE, "INFERRED, see the .c" },
	{ VMS_CNXMAN_CSB_ACCEPT, CNXMAN_CSB_EV_DISCONNECT,
	  VMS_CNXMAN_CSB_DISCONNECT, CNXMAN_CSB_ACT_NONE, "7-24 DISCONNECT" },
	{ VMS_CNXMAN_CSB_ACCEPT, CNXMAN_CSB_EV_NEW_INCARNATION,
	  VMS_CNXMAN_CSB_DEAD, CNXMAN_CSB_ACT_NONE, "7-24 DEAD" },

	/* E81: the peer's REJECT of our INITIAL connect. No reconnect window
	 * exists and there is no membership to hold, so the CSB rests in NEW --
	 * h_connect_abandoned's own grounding, with the reject counted. */
	{ VMS_CNXMAN_CSB_CONNECT, CNXMAN_CSB_EV_CONNECT_REJECTED,
	  VMS_CNXMAN_CSB_NEW, CNXMAN_CSB_ACT_NONE, "2-25/D12 + 7-24 CONNECT" },

	/* OPEN -- "the normal state of a CSB" */
	{ VMS_CNXMAN_CSB_OPEN, CNXMAN_CSB_EV_DISCONNECT,
	  VMS_CNXMAN_CSB_DISCONNECT, CNXMAN_CSB_ACT_NONE, "7-24 DISCONNECT" },
	{ VMS_CNXMAN_CSB_OPEN, CNXMAN_CSB_EV_CONN_LOST,
	  VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_ACT_NONE, "7-24 WAIT / 7-30" },
	{ VMS_CNXMAN_CSB_OPEN, CNXMAN_CSB_EV_LAST_GASP,
	  VMS_CNXMAN_CSB_DISCONNECT, CNXMAN_CSB_ACT_PROPOSE_TRANSITION, "7-29" },
	{ VMS_CNXMAN_CSB_OPEN, CNXMAN_CSB_EV_NEW_INCARNATION,
	  VMS_CNXMAN_CSB_DEAD, CNXMAN_CSB_ACT_NONE, "7-24 DEAD" },

	/* DISCONNECT -- also where a departed system's retained CSB rests */
	{ VMS_CNXMAN_CSB_DISCONNECT, CNXMAN_CSB_EV_NEW_INCARNATION,
	  VMS_CNXMAN_CSB_DEAD, CNXMAN_CSB_ACT_NONE, "7-24 DEAD / 7-25" },

	/* WAIT -- the p. 7-30 timeout is running */
	{ VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_EV_RECNX_ATTEMPT,
	  VMS_CNXMAN_CSB_RECONNECT, CNXMAN_CSB_ACT_RECONNECT, "7-24/7-30" },
	{ VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_EV_RECNX_EXPIRED,
	  VMS_CNXMAN_CSB_DISCONNECT, CNXMAN_CSB_ACT_PROPOSE_TRANSITION, "7-30" },
	{ VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_EV_CONNECT_RCVD,
	  VMS_CNXMAN_CSB_REACCEPT, CNXMAN_CSB_ACT_NONE, "7-24 REACCEPT" },
	{ VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_EV_CONN_OPEN,
	  VMS_CNXMAN_CSB_OPEN, CNXMAN_CSB_ACT_NONE, "7-24 WAIT -> OPEN" },
	{ VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_EV_LAST_GASP,
	  VMS_CNXMAN_CSB_DISCONNECT, CNXMAN_CSB_ACT_PROPOSE_TRANSITION, "7-29" },
	{ VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_EV_NEW_INCARNATION,
	  VMS_CNXMAN_CSB_DEAD, CNXMAN_CSB_ACT_NONE, "7-24 DEAD" },
	/* E81: a reject can land after the beat has already stepped the CSB
	 * back to WAIT under the outstanding attempt. */
	{ VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_EV_CONNECT_REJECTED,
	  VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_ACT_NONE, "2-25/D12 + 7-30" },

	/* RECONNECT -- our attempt is in flight */
	{ VMS_CNXMAN_CSB_RECONNECT, CNXMAN_CSB_EV_RECNX_ATTEMPT,
	  VMS_CNXMAN_CSB_RECONNECT, CNXMAN_CSB_ACT_RECONNECT, "7-30 cadence" },
	{ VMS_CNXMAN_CSB_RECONNECT, CNXMAN_CSB_EV_CONN_OPEN,
	  VMS_CNXMAN_CSB_OPEN, CNXMAN_CSB_ACT_NONE, "7-24 OPEN" },
	{ VMS_CNXMAN_CSB_RECONNECT, CNXMAN_CSB_EV_RECNX_FAILED,
	  VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_ACT_NONE, "7-24 WAIT, repeated" },
	{ VMS_CNXMAN_CSB_RECONNECT, CNXMAN_CSB_EV_RECNX_EXPIRED,
	  VMS_CNXMAN_CSB_DISCONNECT, CNXMAN_CSB_ACT_PROPOSE_TRANSITION, "7-30" },
	{ VMS_CNXMAN_CSB_RECONNECT, CNXMAN_CSB_EV_CONNECT_RCVD,
	  VMS_CNXMAN_CSB_REACCEPT, CNXMAN_CSB_ACT_NONE, "7-24 REACCEPT" },
	{ VMS_CNXMAN_CSB_RECONNECT, CNXMAN_CSB_EV_LAST_GASP,
	  VMS_CNXMAN_CSB_DISCONNECT, CNXMAN_CSB_ACT_PROPOSE_TRANSITION, "7-29" },
	{ VMS_CNXMAN_CSB_RECONNECT, CNXMAN_CSB_EV_NEW_INCARNATION,
	  VMS_CNXMAN_CSB_DEAD, CNXMAN_CSB_ACT_NONE, "7-24 DEAD" },
	/* E81: the peer ANSWERED our reconnect. Back to WAIT, and this ladder
	 * stops asking for the rest of the window (proved behaviourally in
	 * test_cnxman_recnx.c). */
	{ VMS_CNXMAN_CSB_RECONNECT, CNXMAN_CSB_EV_CONNECT_REJECTED,
	  VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_ACT_NONE, "2-25/D12 + 7-30" },

	/* REACCEPT -- the peer is reconnecting to us */
	{ VMS_CNXMAN_CSB_REACCEPT, CNXMAN_CSB_EV_CONN_OPEN,
	  VMS_CNXMAN_CSB_OPEN, CNXMAN_CSB_ACT_NONE, "7-24 OPEN" },
	{ VMS_CNXMAN_CSB_REACCEPT, CNXMAN_CSB_EV_RECNX_FAILED,
	  VMS_CNXMAN_CSB_WAIT, CNXMAN_CSB_ACT_NONE, "7-24 WAIT, repeated" },
	{ VMS_CNXMAN_CSB_REACCEPT, CNXMAN_CSB_EV_RECNX_EXPIRED,
	  VMS_CNXMAN_CSB_DISCONNECT, CNXMAN_CSB_ACT_PROPOSE_TRANSITION, "7-30" },
	{ VMS_CNXMAN_CSB_REACCEPT, CNXMAN_CSB_EV_LAST_GASP,
	  VMS_CNXMAN_CSB_DISCONNECT, CNXMAN_CSB_ACT_PROPOSE_TRANSITION, "7-29" },
	{ VMS_CNXMAN_CSB_REACCEPT, CNXMAN_CSB_EV_NEW_INCARNATION,
	  VMS_CNXMAN_CSB_DEAD, CNXMAN_CSB_ACT_NONE, "7-24 DEAD" },

	/* DEAD and LOCAL have no outgoing edge at all -- p. 7-25 deallocates a
	 * DEAD CSB rather than reviving it, and p. 7-24 reserves LOCAL for the
	 * local connection manager, which has no SCS connection to itself. */
};

static const struct ladder_case *ladder_find(uint8_t from, uint8_t ev)
{
	size_t i;

	for (i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
		if (ladder[i].from == from && ladder[i].ev == ev)
			return &ladder[i];
	}
	return NULL;
}

/* Put a fresh CSB in `state` for one edge's worth of dispatch. Hand-setting the
 * ORIGIN is what makes an exhaustive table test possible; nothing about the
 * OUTCOME is hand-set, and the behavioural proofs in test_cnxman_recnx.c reach
 * every state by driving real events. */
static struct vms_csb *ladder_csb(uint8_t state)
{
	struct vms_csb *csb;

	cluster_reset(20);
	(void)cnxman_club_init(&g_cl);
	csb = cnxman_club_alloc_csb(&g_cl.club, 0x000004000102ull, 1);
	if (csb == NULL)
		return NULL;
	csb->state = state;
	csb->deadline_ms = 60000u;      /* far away: expiry must come from the event */
	csb->next_attempt_ms = 60000u;
	return csb;
}

static void test_ladder_exhaustive(void)
{
	const unsigned cells = (unsigned)VMS_CNXMAN_CSB_STATE__COUNT *
			       (unsigned)CNXMAN_CSB_EV__COUNT;
	unsigned covered = 0, ignored_cells = 0;
	int st, ev;
	int all_ok = 1;

	printf("[csb] every one of the %u (state, event) cells\n", cells);

	for (st = 0; st < VMS_CNXMAN_CSB_STATE__COUNT; st++) {
		for (ev = 0; ev < CNXMAN_CSB_EV__COUNT; ev++) {
			const struct ladder_case *c =
				ladder_find((uint8_t)st, (uint8_t)ev);
			struct vms_csb *csb = ladder_csb((uint8_t)st);
			enum cnxman_csb_action act;
			uint32_t ignored_before;

			if (csb == NULL) {
				all_ok = 0;
				continue;
			}
			ignored_before = cnxman_club_ignored_events(&g_cl.club);
			act = cnxman_csb_dispatch(&g_cl.club, csb,
						  (enum cnxman_csb_event)ev,
						  &g_ops);

			if (c != NULL) {
				covered++;
				if (csb->state != c->to || (uint8_t)act != c->act) {
					printf("  FAIL %s + %s -> %s/%s, want %s/%s (%s)\n",
					       cnxman_csb_state_name(
						       (enum vms_cnxman_csb_state)st),
					       cnxman_csb_event_name(
						       (enum cnxman_csb_event)ev),
					       cnxman_csb_state_name(
						       (enum vms_cnxman_csb_state)csb->state),
					       cnxman_csb_action_name(act),
					       cnxman_csb_state_name(
						       (enum vms_cnxman_csb_state)c->to),
					       cnxman_csb_action_name(
						       (enum cnxman_csb_action)c->act),
					       c->cite);
					all_ok = 0;
				}
				if (cnxman_club_ignored_events(&g_cl.club) !=
				    ignored_before) {
					printf("  FAIL a real edge was counted as ignored\n");
					all_ok = 0;
				}
			} else {
				ignored_cells++;
				if (csb->state != (uint8_t)st ||
				    act != CNXMAN_CSB_ACT_NONE) {
					printf("  FAIL %s + %s invented an edge -> %s/%s\n",
					       cnxman_csb_state_name(
						       (enum vms_cnxman_csb_state)st),
					       cnxman_csb_event_name(
						       (enum cnxman_csb_event)ev),
					       cnxman_csb_state_name(
						       (enum vms_cnxman_csb_state)csb->state),
					       cnxman_csb_action_name(act));
					all_ok = 0;
				}
				if (cnxman_club_ignored_events(&g_cl.club) !=
				    ignored_before + 1u) {
					printf("  FAIL an ignored event was not counted\n");
					all_ok = 0;
				}
			}
		}
	}

	ct_check(all_ok, "every cell behaves exactly as the table declares");
	ct_check_eq_u32(covered, sizeof(ladder) / sizeof(ladder[0]),
			"every declared edge was reached");
	ct_check_eq_u32(ignored_cells, cells - (unsigned)(sizeof(ladder) /
							 sizeof(ladder[0])),
			"every other cell is an honestly ignored event");
}

/* The two absorbing states, called out by name because getting either wrong is
 * a cluster-level bug: a revived DEAD CSB is a stale incarnation readmitted,
 * and a LOCAL CSB that accepts a connectivity event is this node proposing its
 * own removal. */
static void test_absorbing_states(void)
{
	struct vms_csb *csb;
	int ev, ok = 1;

	printf("[csb] DEAD and LOCAL absorb everything (pp. 7-24/7-25)\n");
	for (ev = 0; ev < CNXMAN_CSB_EV__COUNT; ev++) {
		csb = ladder_csb((uint8_t)VMS_CNXMAN_CSB_DEAD);
		(void)cnxman_csb_dispatch(&g_cl.club, csb,
					  (enum cnxman_csb_event)ev, &g_ops);
		if (csb->state != (uint8_t)VMS_CNXMAN_CSB_DEAD)
			ok = 0;
	}
	ct_check(ok, "no event revives a DEAD CSB -- p. 7-25 deallocates it instead");

	ok = 1;
	for (ev = 0; ev < CNXMAN_CSB_EV__COUNT; ev++) {
		cluster_reset(20);
		csb = cnxman_club_init(&g_cl);
		(void)cnxman_csb_dispatch(&g_cl.club, csb,
					  (enum cnxman_csb_event)ev, &g_ops);
		if (csb->state != (uint8_t)VMS_CNXMAN_CSB_LOCAL)
			ok = 0;
	}
	ct_check(ok, "the LOCAL CSB stays LOCAL whatever is dispatched at it");
}

/* ==========================================================================
 * 3. The CLUB
 * ========================================================================== */
static void test_club_init(void)
{
	struct vms_csb *local;

	printf("[club] init builds the LOCAL CSB from SYSGEN (pp. 7-23/7-26)\n");
	cluster_reset(20);
	local = cnxman_club_init(&g_cl);

	ct_check(local != NULL, "init returns the local system's CSB");
	ct_check(local == cnxman_club_local(&g_cl.club),
		 "and the CLUB holds it (p. 7-26)");
	ct_check_eq_u32(local->state, VMS_CNXMAN_CSB_LOCAL,
			"its connectivity state is LOCAL (p. 7-24)");
	ct_check((local->flags & VMS_CSB_F_LOCAL) != 0,
		 "and the p. 7-23 'this CSB is the local node' flag is set");
	ct_check(local->scsnode_len == 6 &&
		 memcmp(local->scsnode, NODE_OVMX, 6) == 0,
		 "SCSNODE came from SYSGEN, not from the wire");
	ct_check(local->sysid_valid && local->sysid == 0x000004000103ull,
		 "SCSSYSTEMID likewise");
	ct_check(local->params_valid && local->expected_votes == 3,
		 "VOTES/EXPECTED_VOTES/QDSKVOTES are recorded (p. 7-23)");
	ct_check(local->lockdirwt_valid && local->lockdirwt == 0,
		 "LOCKDIRWT is recorded for the local CSB (p. 7-23)");

	/* INV-6: the cluster assigns the CSID, so we do not have one yet. */
	ct_check(g_cl.club.local_csid_valid == 0,
		 "the local CSID is NOT set at init -- the cluster assigns it");
	ct_check(local->csid_valid == 0,
		 "and the local CSB carries no CSID either (absent, not zero)");
	ct_check_eq_u32(g_cl.club.recnxinterval, 20,
			"RECNXINTERVAL was copied from SYSGEN");
	ct_check(g_cl.club.recnxinterval_defaulted == 0,
		 "and is not marked defaulted when SYSGEN supplied it");
}

static void test_club_recnxinterval_default(void)
{
	printf("[club] a missing RECNXINTERVAL falls back, and says so\n");
	cluster_reset(0);
	(void)cnxman_club_init(&g_cl);
	ct_check_eq_u32(g_cl.club.recnxinterval, CNXMAN_RECNX_DEFAULT_SECS,
			"SYSGEN carried none -> the published OpenVMS default");
	ct_check_eq_u32(CNXMAN_RECNX_DEFAULT_SECS, 20,
			"and that default is 20 seconds");
	ct_check(g_cl.club.recnxinterval_defaulted == 1,
		 "the fallback is RECORDED, not silently substituted");
}

static void test_club_alloc_find_free(void)
{
	struct vms_csb *a, *b, *found;
	uint32_t i, allocated = 0;

	printf("[club] allocate, find, free (pp. 7-25/7-28)\n");
	cluster_reset(20);
	(void)cnxman_club_init(&g_cl);

	a = cnxman_club_alloc_csb(&g_cl.club, 0x000004000101ull, 1);
	b = cnxman_club_alloc_csb(&g_cl.club, 0x000004000102ull, 1);
	ct_check(a != NULL && b != NULL && a != b, "two remote CSBs allocated");
	ct_check_eq_u32(a->state, VMS_CNXMAN_CSB_NEW,
			"a discovered CM starts NEW (p. 7-23)");
	ct_check(cnxman_csb_is_member(a) == 0,
		 "a NEW CSB is not a member -- connectivity is not membership");

	found = cnxman_club_find_sysid(&g_cl.club, 0x000004000102ull);
	ct_check(found == b, "find by SCSSYSTEMID");
	ct_check(cnxman_club_find_sysid(&g_cl.club, 0x0000040000ffull) == NULL,
		 "an unknown SCSSYSTEMID finds nothing");

	/* INV-6, the load-bearing one: an unlearned CSID matches NOTHING. */
	ct_check(cnxman_club_find_csid(&g_cl.club, 0) == NULL,
		 "a CSID lookup for 0 matches no CSB, however many are unlearned");
	cnxman_csb_set_csid(b, 0x00010002u);
	ct_check(cnxman_club_find_csid(&g_cl.club, 0x00010002u) == b,
		 "once LEARNED, the CSID finds its CSB");
	ct_check(cnxman_club_find_csid(&g_cl.club, 0x00010009u) == NULL,
		 "and a CSID nobody was assigned still finds nothing");

	cnxman_club_free_csb(&g_cl.club, b);
	ct_check(cnxman_club_find_csid(&g_cl.club, 0x00010002u) == NULL,
		 "a freed CSB is gone (p. 7-25: the old CSB is deallocated)");
	ct_check(cnxman_club_find_sysid(&g_cl.club, 0x000004000101ull) == a,
		 "and freeing one did not disturb its neighbour");

	/* The table is finite and REFUSES rather than overwriting. */
	cluster_reset(20);
	(void)cnxman_club_init(&g_cl);
	for (i = 0; i < (uint32_t)VMS_CLUB_MAX_CSB + 4u; i++) {
		if (cnxman_club_alloc_csb(&g_cl.club,
					  0x000004001000ull + i, 1) != NULL)
			allocated++;
	}
	ct_check_eq_u32(allocated, (uint32_t)VMS_CLUB_MAX_CSB - 1u,
			"a full CSB table refuses (the local CSB holds slot 0)");
}

static void test_club_member_count(void)
{
	struct vms_csb *local, *a, *b;

	printf("[club] the member count is the SELECTED count (p. 7-49)\n");
	cluster_reset(20);
	local = cnxman_club_init(&g_cl);
	a = cnxman_club_alloc_csb(&g_cl.club, 0x000004000101ull, 1);
	b = cnxman_club_alloc_csb(&g_cl.club, 0x000004000102ull, 1);

	ct_check_eq_u32(cnxman_club_recount_members(&g_cl.club), 0,
			"nothing is SELECTED before a transition commits one");

	cnxman_csb_set_flags(local, VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER);
	cnxman_csb_set_flags(a, VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER);
	cnxman_csb_set_flags(b, VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER);
	ct_check_eq_u32(cnxman_club_recount_members(&g_cl.club), 3,
			"three SELECTED CSBs -> CLUSTER_NODES 3");
	ct_check_eq_u32(g_cl.club.cluster_nodes, 3,
			"and the count is stored in the CLUB (p. 7-26)");

	/*
	 * The load-bearing separation: losing connectivity to `b` clears this
	 * node's MEMBER view but must NOT move SELECTED, so the cluster's own
	 * member count does not dip while the reconnect window runs.
	 */
	b->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
	(void)cnxman_csb_dispatch(&g_cl.club, b, CNXMAN_CSB_EV_LAST_GASP, &g_ops);
	ct_check(cnxman_csb_is_member(b) == 0,
		 "a last gasp clears our MEMBER view of the departing node");
	ct_check_eq_u32(cnxman_club_recount_members(&g_cl.club), 3,
			"but SELECTED is a transition's business -- the count holds");
}

static void test_learn_local_csid(void)
{
	struct vms_csb *local;

	printf("[club] the local CSID is learned, never chosen\n");
	cluster_reset(20);
	local = cnxman_club_init(&g_cl);
	cnxman_club_learn_local_csid(&g_cl.club, 0x00010003u);
	ct_check(g_cl.club.local_csid_valid == 1 &&
		 g_cl.club.local_csid == 0x00010003u,
		 "the CLUB records the CSID the cluster assigned");
	ct_check(local->csid_valid == 1 && local->csid == 0x00010003u,
		 "and the local CSB carries the same fact, not a second one");
}

/* ==========================================================================
 * 4. Projections -- absent is not zero
 * ========================================================================== */
static void test_projection(void)
{
	struct vms_csb_view cv;
	struct vms_club_view lv;
	struct vms_csb *csb;

	printf("[view] projections carry validity, not defaults\n");
	cluster_reset(20);
	(void)cnxman_club_init(&g_cl);
	csb = cnxman_club_alloc_csb(&g_cl.club, 0x000004000102ull, 1);

	cnxman_csb_project(csb, &cv);
	ct_check(cv.csid_valid == 0 && cv.csid == 0,
		 "an unlearned CSID projects as ABSENT, not as csid 0");
	ct_check(cv.lockdirwt_valid == 0,
		 "an un-advertised LOCKDIRWT projects as absent (FC-P3.2)");
	ct_check(cv.peer_sysid_lo == 0x04000102u && cv.peer_sysid_hi == 0u,
		 "the SCSSYSTEMID splits low-half-first across the ABI");
	ct_check_eq_u32(cv.state, VMS_CNXMAN_CSB_NEW,
			"the projected state is the CSB's real one");

	cnxman_csb_set_csid(csb, 0x00010002u);
	cnxman_csb_set_lockdirwt(csb, 1);
	cnxman_csb_set_scsnode(csb, (const uint8_t *)"VAX2", 4);
	cnxman_csb_set_flags(csb, VMS_CSB_F_MEMBER | VMS_CSB_F_SELECTED |
				  VMS_CSB_F_STATUS_RCVD);
	cnxman_csb_project(csb, &cv);
	ct_check(cv.csid_valid == 1 && cv.csid == 0x00010002u,
		 "a learned CSID projects with its flag");
	ct_check(cv.lockdirwt_valid == 1 && cv.lockdirwt == 1,
		 "so does an advertised LOCKDIRWT");
	ct_check(cv.is_member && cv.is_selected && cv.status_rcvd,
		 "the SDA flag triple projects from the CSB's real flags");
	ct_check(cv.scsnode_len == 4 && memcmp(cv.scsnode, "VAX2", 4) == 0,
		 "SCSNODE projects as advertised");

	/* A free slot projects as nothing at all -- never as "system 0". */
	cnxman_club_free_csb(&g_cl.club, csb);
	memset(&cv, 0xee, sizeof(cv));
	cnxman_csb_project(csb, &cv);
	ct_check(cv.csid_valid == 0 && cv.csid == 0 && cv.state == 0 &&
		 cv.peer_sysid_lo == 0,
		 "a freed slot projects as an empty view, not as a phantom node");

	cnxman_club_learn_local_csid(&g_cl.club, 0x00010003u);
	g_cl.club.epoch = 7;
	g_cl.club.reformations = 2;
	cnxman_club_project(&g_cl.club, VMS_CLUSTER_MEMBER, &lv);
	ct_check(lv.local_csid_valid == 1 && lv.local_csid == 0x00010003u,
		 "the CLUB view carries the learned local CSID");
	ct_check_eq_u32(lv.state, VMS_CLUSTER_MEMBER, "and the node's state");
	ct_check(lv.epoch == 7 && lv.reformations == 2,
		 "and the epoch/reformation counters from the real CLUB");
	ct_check(lv.ftime_lo == 0 && lv.ftime_hi == 0,
		 "an unrecorded CLUSTER_FTIME projects as zero WITH no claim made");
}

/* ==========================================================================
 * 4b. THE DIALOGUE BELONGS TO THE CONNECTION (E77)
 *
 * GOLDEN GROUNDING, measured with tools/cluster/cm_dialogue_audit.py:
 *   vax3-2to3-established-join-20260730 -- 08:00:2b:78:56:b9 is at send_msg
 *     21078 on Con.ID pair 3551000a/a4980009 and opens 18e3000a/a498000d at
 *     send_msg=1 ack=0; aa:00:04:00:01:04 is at 15880 on the first pair and
 *     opens its next at 1/0 as well.
 *   formation-ci1 -- the SAME station pair's SECOND dialogue
 *     (3359000a/63080008) opens at send_msg=1 ack=0 after 17541 messages on
 *     the first, and the answer on it carries ack=1 once the peer's 1 arrived.
 *
 * So a fresh connection restarts BOTH cells, and the ack rises only to what
 * the peer really sent HERE. Carrying them across a teardown is what put a
 * send-msg# 8 on a brand-new Con.ID and bugchecked two real VAXes (E76/E77).
 * ========================================================================== */
static void test_dialogue_is_per_connection(void)
{
	struct vms_csb *csb;

	printf("[csb] E77: the send/ack dialogue restarts on a new Con.ID\n");
	cluster_reset(20);
	(void)cnxman_club_init(&g_cl);
	csb = cnxman_club_alloc_csb(&g_cl.club, 0x000004000101ull, 1);
	ct_check(csb != NULL, "a CSB for the peer");
	if (csb == NULL)
		return;

	/* Connection A: the executive minted it, so this block's dialogue is
	 * its dialogue -- and it has said and heard nothing on it yet. */
	cnxman_csb_bind_connection(csb, 0x4e620009u);
	ct_check_eq_u32(csb->cdt_conid, 0x4e620009u,
			"the binder is the writer of cdt_conid");
	ct_check_eq_u32(csb->cm_send_msg, 0u, "nothing sent on it yet");
	ct_check(cnxman_csb_dialogue_is_on(csb, 0x4e620009u),
		 "and the dialogue state IS this connection's");

	cnxman_csb_dialogue_sent(csb);
	ct_check_eq_u32(csb->cm_send_msg, 1u,
			"the first origination carries send-msg# 1 (sec 4(j))");
	cnxman_csb_dialogue_sent(csb);
	cnxman_csb_dialogue_sent(csb);
	cnxman_csb_dialogue_heard(csb, 5u);
	ct_check_eq_u32(csb->cm_send_msg, 3u, "three sent on A");
	ct_check_eq_u32(csb->cm_ack_msg, 5u, "and the peer's 5 heard on A");

	/* Re-binding the SAME Con.ID is what the glue does on every accept and
	 * on a reconnect that handed back the connection already held: it is
	 * not a new dialogue and must not disturb one in flight. */
	cnxman_csb_bind_connection(csb, 0x4e620009u);
	ct_check_eq_u32(csb->cm_send_msg, 3u,
			"re-binding the same connection changes nothing");
	ct_check_eq_u32(csb->cm_ack_msg, 5u, "... on either cell");
	ct_check_eq_u32(csb->cm_dialogue_resets, 0u, "... and is not a restart");

	/* Connection B: a different Con.ID is a different conversation. */
	cnxman_csb_bind_connection(csb, 0x4e62000fu);
	ct_check_eq_u32(csb->cm_send_msg, 0u,
			"a NEW Con.ID restarts the send side, so the next "
			"origination carries 1 -- what every fresh CDT in the "
			"golden captures opens at");
	ct_check_eq_u32(csb->cm_ack_msg, 0u,
			"... and acks NOTHING until this peer sends here: an "
			"ack for a message the peer never sent on this "
			"connection is the byte CNXMGRERR bugchecks on");
	ct_check_eq_u32(csb->cm_dialogue_resets, 1u, "... counted as a restart");
	ct_check(!cnxman_csb_dialogue_is_on(csb, 0x4e620009u),
		 "and the dead connection is no longer this block's dialogue");

	cnxman_csb_dialogue_sent(csb);
	ct_check_eq_u32(csb->cm_send_msg, 1u,
			"the first message on B is 1, not 4: the numbers burned "
			"on A belonged to a dialogue that no longer exists");
	cnxman_csb_dialogue_heard(csb, 1u);
	ct_check_eq_u32(csb->cm_ack_msg, 1u,
			"and the ack is the peer's REAL send-msg# on B");

	ct_check(!cnxman_csb_dialogue_is_on(csb, 0u),
		 "Con.ID 0 is 'no connection', never a dialogue");
	ct_check(!cnxman_csb_dialogue_is_on(NULL, 0x4e62000fu),
		 "and no CSB is no dialogue");
	cnxman_csb_bind_connection(NULL, 1u);   /* safe */
}

/*
 * E81 -- THE RECONNECT LADDER'S OWN REBIND, AND THE REJECT THAT FOLLOWS IT.
 *
 * E77 proved the rule on the connection the JOIN adopts. The p. 7-30 reconnect
 * apparatus mints connections too (CNXMAN_CSB_ACT_RECONNECT -> scs_connect ->
 * cnxman_csb_bind_connection), and on join-e80refire that was the leg in play:
 * the CM connection closed PATHLOST, the ladder reconnected, and the VAX
 * refused the new connection. Two facts have to hold on that leg.
 *
 *   1. The envelope this block would stamp on the ladder's fresh Con.ID is
 *      send-msg# 1 / ack 0 -- READ BACK OUT OF THE BYTES, not asserted about
 *      the fields -- because the peer has said nothing on that connection.
 *   2. Once the peer REFUSES that connection, this block claims no connection
 *      at all, so no emitter can stamp an envelope for one that does not exist
 *      (the E76/E77 crash family). The glue releases it through the single
 *      writer; here that release is exercised directly.
 */
static void test_reconnect_dialogue_never_carries_the_old_ack(void)
{
	struct vms_csb *csb;
	uint8_t body[132];
	unsigned i;

	printf("[csb] E81: the LADDER's reconnect opens at send 1 / ack 0\n");
	cluster_reset(20);
	(void)cnxman_club_init(&g_cl);
	csb = cnxman_club_alloc_csb(&g_cl.club, 0x000004000101ull, 1);
	ct_check(csb != NULL, "a CSB for the peer");
	if (csb == NULL)
		return;

	/* The connection the peer opened and this node accepted, with a real
	 * dialogue on it: two originations of ours, five messages of theirs. */
	cnxman_csb_bind_connection(csb, 0x81290012u);
	cnxman_csb_dialogue_sent(csb);
	cnxman_csb_dialogue_sent(csb);
	cnxman_csb_dialogue_heard(csb, 5u);
	ct_check_eq_u32(csb->cm_ack_msg, 5u, "the accepted leg acks the peer's 5");

	/* It closes, and the ladder's reconnect mints a NEW Con.ID. */
	cnxman_csb_bind_connection(csb, 0x81290013u);

	for (i = 0; i < sizeof(body); i++)
		body[i] = 0xffu;          /* poison: nothing may survive */
	cnxman_envelope_originate(csb, body, CNXMAN_ENV_REQUEST);
	ct_check_eq_u32((uint32_t)body[0] | ((uint32_t)body[1] << 8), 1u,
			"body[0:2] on the reconnect is send-msg# 1");
	ct_check_eq_u32((uint32_t)body[2] | ((uint32_t)body[3] << 8), 0u,
			"body[2:4] is ack 0 -- the peer has sent NOTHING on "
			"this connection, and asserting the old leg's 5 here is "
			"the unbacked ack a real VAX bugchecks on");

	/* The peer refuses it. This node then holds no connection to that
	 * system, and says so. */
	cnxman_csb_bind_connection(csb, 0u);
	ct_check(!cnxman_csb_dialogue_is_on(csb, 0x81290013u),
		 "a refused connection is not this block's dialogue");
	ct_check_eq_u32(csb->cdt_conid, 0u,
			"and the block claims no Con.ID at all, so no emitter "
			"can stamp an envelope for it");
}

/* ==========================================================================
 * 4b. THE CORRELATION PAIR body[4:8] (E85)
 *
 * The cells the stamper reads and, until E85, NOTHING in this executive wrote:
 * every request this node ever originated carried (txn,token) = (0,0), and the
 * twelve barrier steps of a transition were therefore indistinguishable to the
 * coordinator that correlates its releases by them. The suite did not see it
 * because three test beds advanced the cells by hand.
 *
 * What the wire measures, and what these assertions pin: a REQUEST mints a
 * fresh nonzero token; a RESPONSE and a NOTIFY mint nothing; a new dialogue
 * takes a new transaction id and restarts the token.
 * ========================================================================== */
static void test_correlation_pair_is_maintained(void)
{
	struct vms_csb *csb;
	uint8_t body[VMS_CM_BODY_LEN];
	uint16_t t1, t2, t3, txn_a, txn_b;
	uint32_t i;

	printf("-- E85: body[4:8] is minted by the executive, per REQUEST --\n");
	(void)cnxman_club_init(&g_cl);
	csb = cnxman_club_alloc_csb(&g_cl.club, 0x000004000101ull, 1);
	ct_check(csb != NULL, "a CSB for the peer");
	if (csb == NULL)
		return;

	cnxman_csb_bind_connection(csb, 0x81290012u);
	txn_a = csb->cm_txn;
	ct_check(txn_a != 0u,
		 "binding a dialogue mints a NONZERO transaction id");

	for (i = 0; i < sizeof(body); i++)
		body[i] = 0xffu;
	cnxman_envelope_originate(csb, body, CNXMAN_ENV_REQUEST);
	t1 = (uint16_t)(body[6] | ((uint16_t)body[7] << 8));
	ct_check(t1 != 0u, "the first REQUEST carries a nonzero token");
	ct_check_eq_u32((uint32_t)(body[4] | ((uint16_t)body[5] << 8)),
			(uint32_t)txn_a,
			"... and this dialogue's own transaction id");

	cnxman_envelope_originate(csb, body, CNXMAN_ENV_REQUEST);
	t2 = (uint16_t)(body[6] | ((uint16_t)body[7] << 8));
	ct_check(t2 != t1 && t2 != 0u,
		 "the next REQUEST carries a DIFFERENT nonzero token -- twelve "
		 "barrier steps must not share one");

	/* A RESPONSE carries the peer's pair back and mints nothing. */
	body[4] = 0x34; body[5] = 0x12; body[6] = 0x78; body[7] = 0x56;
	cnxman_envelope_originate(csb, body, CNXMAN_ENV_RESPONSE);
	ct_check_eq_u32((uint32_t)(body[6] | ((uint16_t)body[7] << 8)), 0x5678u,
			"a RESPONSE leaves the peer's echoed token exactly "
			"where the codec put it");
	ct_check_eq_u32((uint32_t)csb->cm_token, (uint32_t)t2,
			"... and consumes no token of ours");

	/* A NOTIFY is an origination the peer never answers: nothing minted. */
	body[4] = 0; body[5] = 0; body[6] = 0; body[7] = 0;
	cnxman_envelope_originate(csb, body, CNXMAN_ENV_NOTIFY);
	ct_check_eq_u32((uint32_t)(body[4] | ((uint16_t)body[5] << 8)), 0u,
			"a NOTIFY keeps the zero pair the builder built "
			"(125/125 real GOs, 1104/1104 real RELEASEs)");
	ct_check_eq_u32((uint32_t)(body[6] | ((uint16_t)body[7] << 8)), 0u,
			"... in BOTH cells");
	ct_check_eq_u32((uint32_t)csb->cm_token, (uint32_t)t2,
			"... and consumes no token either");

	/* A NEW dialogue: new transaction id, token back to the start. */
	cnxman_csb_bind_connection(csb, 0x81290013u);
	txn_b = csb->cm_txn;
	ct_check(txn_b != txn_a && txn_b != 0u,
		 "a new connection is a new dialogue and takes a new "
		 "transaction id");
	cnxman_envelope_originate(csb, body, CNXMAN_ENV_REQUEST);
	t3 = (uint16_t)(body[6] | ((uint16_t)body[7] << 8));
	ct_check(t3 != 0u,
		 "and its first REQUEST carries a nonzero token again -- "
		 "never a number inherited from the dialogue that died");

	/* The wrap never presents the absent value. */
	csb->cm_token = 0xffffu;
	cnxman_csb_transaction_opened(csb);
	ct_check_eq_u32((uint32_t)csb->cm_token, 1u,
			"the 16-bit wrap skips 0 and lands on 1, so no cycle "
			"ever asserts 'nothing here'");
}

/* ==========================================================================
 * 5. NULL / edge safety
 * ========================================================================== */
static void test_null_safety(void)
{
	struct vms_csb *csb;

	printf("[csb] NULL and out-of-range safety\n");
	cluster_reset(20);
	(void)cnxman_club_init(&g_cl);
	csb = cnxman_club_alloc_csb(&g_cl.club, 1, 1);

	ct_check(cnxman_csb_dispatch(NULL, csb, CNXMAN_CSB_EV_CONN_OPEN,
				     &g_ops) == CNXMAN_CSB_ACT_NONE,
		 "dispatch with no CLUB does nothing");
	ct_check(cnxman_csb_dispatch(&g_cl.club, NULL, CNXMAN_CSB_EV_CONN_OPEN,
				     &g_ops) == CNXMAN_CSB_ACT_NONE,
		 "dispatch with no CSB does nothing");
	ct_check(cnxman_csb_dispatch(&g_cl.club, csb,
				     (enum cnxman_csb_event)99,
				     &g_ops) == CNXMAN_CSB_ACT_NONE,
		 "an out-of-range event never indexes the table");
	csb->state = 99;
	ct_check(cnxman_csb_dispatch(&g_cl.club, csb, CNXMAN_CSB_EV_CONN_OPEN,
				     &g_ops) == CNXMAN_CSB_ACT_NONE,
		 "an out-of-range state never indexes the table");
	csb->state = (uint8_t)VMS_CNXMAN_CSB_NEW;

	/* No ops at all: the machine must still be safe, because a caller that
	 * forgot to inject a clock is a bug, not a licence to read one. */
	ct_check(cnxman_csb_dispatch(&g_cl.club, csb,
				     CNXMAN_CSB_EV_CONNECT_SENT,
				     NULL) == CNXMAN_CSB_ACT_NONE,
		 "dispatch with NULL ops is safe");
	ct_check_eq_u32(csb->state, VMS_CNXMAN_CSB_CONNECT,
			"and still made the transition");

	ct_check(cnxman_club_init(NULL) == NULL, "init(NULL) is safe");
	ct_check(cnxman_club_local(NULL) == NULL, "local(NULL) is safe");
	ct_check(cnxman_club_alloc_csb(NULL, 1, 1) == NULL, "alloc(NULL) is safe");
	ct_check(cnxman_club_find_csid(NULL, 1) == NULL, "find_csid(NULL) is safe");
	ct_check(cnxman_club_csb_at(&g_cl.club, 9999) == NULL,
		 "walking past the table end returns NULL");
	ct_check_eq_u32(cnxman_club_csb_index(&g_cl.club, NULL),
			(uint32_t)VMS_CLUB_MAX_CSB,
			"index(NULL) is the out-of-range sentinel");
	ct_check_eq_u32(cnxman_club_ignored_events(NULL), 0,
			"ignored_events(NULL) is 0");
	cnxman_csb_set_csid(NULL, 1);
	cnxman_csb_set_scsnode(NULL, NULL, 0);
	cnxman_csb_project(NULL, NULL);
	cnxman_club_project(NULL, VMS_CLUSTER_OFF, NULL);
	ct_check(cnxman_csb_is_member(NULL) == 0, "is_member(NULL) is 0");
}

int main(void)
{
	printf("=== test_cnxman_csb: the CLUB/CSB model + the ten-state ladder ===\n");
	test_state_vocabulary();
	test_ladder_exhaustive();
	test_absorbing_states();
	test_club_init();
	test_club_recnxinterval_default();
	test_club_alloc_find_free();
	test_club_member_count();
	test_learn_local_csid();
	test_projection();
	test_dialogue_is_per_connection();
	test_reconnect_dialogue_never_carries_the_old_ack();
	test_correlation_pair_is_maintained();
	test_null_safety();
	return ct_summary("test_cnxman_csb");
}
