/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_echo_guard.c - the echo guard, driven by THE REAL STORM
 * (rd vms-b5b0, R1 host unit).
 *
 * WHAT HAPPENED. In the 2026-10-09 evacuation lab a booted OVMX node mastered
 * EVAC$WORKLOAD (an OVMX process held NL on it) and VAX1 $ENQW'd EX. NL is
 * compatible with everything, so the request was grantable at once and OVMX
 * answered -- but its answer carried the two lock handles in each other's slots
 * and no grant record, so VAX1 could not match the completion to its own lock.
 * It re-sent the SAME request 65,356 times in 63.7 s; OVMX answered 65,340 of
 * them with the SAME bytes. An operator stopped it.
 *
 * WHAT THIS FILE PROVES, on the captured bytes themselves (the two fixtures
 * dlm-storm-request / dlm-storm-answer are frames 2081 and 2082 of
 * vms-b5b0-storm-window.pcap):
 *
 *   1. the storm, replayed, is CAPPED: the guard admits a bounded number of
 *      identical answers and then withholds, counting and flagging once;
 *   2. a dialogue that MOVES is never capped, however long it runs -- the
 *      property that makes this safe to put in front of every reply;
 *   3. one looping peer does not silence another;
 *   4. the cap is about SAMENESS, not rate: a thousand DIFFERENT answers to a
 *      thousand DIFFERENT requests all go out.
 *
 * The underlying codec bug is fixed and proven byte-for-byte elsewhere
 * (test_codec_dlm's test_real_grant_is_reproduced). This is the teeth: the
 * failure mode is self-limiting next time, whatever causes it.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "cluster_fixture.h"
#include "vms_cluster_codec_cm.h"
#include "vms_dlm_echo_guard.h"

#define CSID_VAX1 0x00010002u
#define CSID_VAX2 0x00010003u

/*
 * The corpus, loaded once with the loader's own clean-room custody check (it
 * refuses a specimen whose capture is not in the clean-room manifest and
 * whose bytes do not match the declared digest).
 */
static struct vms_fixture g_fx[VMS_FIXTURE_MAX_FILES];
static int g_n;

static const struct vms_fixture *fx(const char *name)
{
	int i;

	for (i = 0; i < g_n; i++) {
		if (strcmp(g_fx[i].name, name) == 0)
			return &g_fx[i];
	}
	return NULL;
}

/* ==========================================================================
 * 1. The storm, replayed off the wire
 * ========================================================================== */
static void the_real_storm_is_capped(void)
{
	const struct vms_fixture *rq = fx("dlm-storm-request");
	const struct vms_fixture *an = fx("dlm-storm-answer");
	struct vms_dlm_echo_guard g;
	const uint8_t *req, *ans;
	uint32_t i, admitted = 0, refused = 0, caps_said = 0;

	printf("-- the real 65,356-frame storm, replayed: the answer stops\n");
	ct_check(rq != NULL && an != NULL,
		 "the captured storm request and OVMX's answer both load, with "
		 "their capture in the clean-room manifest");
	if (rq == NULL || an == NULL)
		return;
	req = rq->bytes + VMS_OFF_SYSAP_BODY;
	ans = an->bytes + VMS_OFF_SYSAP_BODY;

	/* The negative control: these really are the storm's frames -- a reply
	 * to the same lock request, from the node that was asked. */
	ct_check_eq_u32(req[9], 0x01u, "the request is an op-0x01 ENQ");
	ct_check_eq_u32(ans[8], 0x82u, "OVMX's frame is a cat-0x82 response");
	ct_check(memcmp(req + 24, ans + 24, 4) != 0,
		 "*** and it carries NOTHING of the requester's own handle: "
		 "the reason VAX1 asked 65,356 times ***");

	vms_dlm_echo_guard_init(&g);
	for (i = 0u; i < 1000u; i++) {
		uint8_t first = 0u;

		if (vms_dlm_echo_admit(&g, CSID_VAX1, req, VMS_CM_BODY_LEN,
				       ans, VMS_CM_BODY_LEN, &first))
			admitted++;
		else
			refused++;
		caps_said += first;
	}
	ct_check_eq_u32(admitted, VMS_DLM_ECHO_MAX_SAME,
			"*** exactly VMS_DLM_ECHO_MAX_SAME identical answers "
			"went out of a thousand asked for ***");
	ct_check_eq_u32(refused, 1000u - VMS_DLM_ECHO_MAX_SAME,
			"*** every one after that was WITHHELD: 63 s of storm "
			"becomes 8 frames ***");
	ct_check_eq_u32(caps_said, 1u,
			"and the console line is flagged exactly ONCE, however "
			"long the peer keeps asking (65,000 identical console "
			"lines is its own denial of service)");
	ct_check_eq_u32(g.refused, 1000u - VMS_DLM_ECHO_MAX_SAME,
			"the guard's own counter agrees -- a real number for "
			"SHOW CLUSTER/diagnostics, not a log line");
	ct_check_eq_u32(g.runs_capped, 1u, "one conversation hit the bound");
}

/* ==========================================================================
 * 2. A dialogue that MOVES is never capped
 * ========================================================================== */
static void a_moving_dialogue_is_never_capped(void)
{
	struct vms_dlm_echo_guard g;
	uint8_t req[VMS_CM_BODY_LEN], ans[VMS_CM_BODY_LEN];
	uint32_t i, admitted = 0;

	printf("-- a dialogue that moves runs forever (the safety property)\n");
	vms_dlm_echo_guard_init(&g);
	memset(req, 0, sizeof(req));
	memset(ans, 0, sizeof(ans));
	for (i = 0u; i < 5000u; i++) {
		/* A different request each time, as a rebuild burst or a busy
		 * cluster produces: thousands of frames, none of them a loop. */
		req[24] = (uint8_t)(i & 0xffu);
		req[25] = (uint8_t)((i >> 8) & 0xffu);
		ans[24] = req[24];
		ans[25] = req[25];
		if (vms_dlm_echo_admit(&g, CSID_VAX1, req, sizeof(req),
				       ans, sizeof(ans), NULL))
			admitted++;
	}
	ct_check_eq_u32(admitted, 5000u,
			"*** all 5000 answers went out: the guard counts "
			"SAMENESS, not rate ***");
	ct_check_eq_u32(g.refused, 0u, "nothing was withheld");

	/* And an ANSWER that changes while the request repeats is progress too
	 * -- a master that answers the same question differently (a deny that
	 * becomes a grant) is working, not looping. */
	vms_dlm_echo_guard_init(&g);
	memset(req, 0x5au, sizeof(req));
	admitted = 0u;
	for (i = 0u; i < 100u; i++) {
		ans[30] = (uint8_t)i;
		if (vms_dlm_echo_admit(&g, CSID_VAX1, req, sizeof(req),
				       ans, sizeof(ans), NULL))
			admitted++;
	}
	ct_check_eq_u32(admitted, 100u,
			"a repeated request answered DIFFERENTLY each time is "
			"never capped either");
}

/* ==========================================================================
 * 3. One looping peer does not silence another
 * ========================================================================== */
static void a_looping_peer_does_not_silence_another(void)
{
	struct vms_dlm_echo_guard g;
	uint8_t req[VMS_CM_BODY_LEN], ans[VMS_CM_BODY_LEN];
	uint32_t i, vax2_admitted = 0;

	printf("-- the cap is PER SYSTEM: a storming peer does not mute a quiet "
	       "one\n");
	vms_dlm_echo_guard_init(&g);
	memset(req, 0x11, sizeof(req));
	memset(ans, 0x22, sizeof(ans));

	for (i = 0u; i < 500u; i++)
		(void)vms_dlm_echo_admit(&g, CSID_VAX1, req, sizeof(req),
					 ans, sizeof(ans), NULL);
	ct_check(g.refused > 0u, "VAX1's loop is being withheld");

	/* VAX2 asks the very same thing, once. It is a different system, so it
	 * gets its answer. */
	if (vms_dlm_echo_admit(&g, CSID_VAX2, req, sizeof(req), ans,
			       sizeof(ans), NULL))
		vax2_admitted++;
	ct_check_eq_u32(vax2_admitted, 1u,
			"*** VAX2's answer goes out: refusing to answer is "
			"never applied to a system that has not looped ***");
}

/* ==========================================================================
 * 4. The degenerate inputs admit -- a safety net never gates service
 * ========================================================================== */
static void nothing_to_compare_means_answer(void)
{
	struct vms_dlm_echo_guard g;
	uint8_t b[VMS_CM_BODY_LEN];

	printf("-- with nothing to compare, the answer GOES (Rule 9: a guard "
	       "must not become a refusal to serve)\n");
	vms_dlm_echo_guard_init(&g);
	memset(b, 0, sizeof(b));

	ct_check(vms_dlm_echo_admit(NULL, CSID_VAX1, b, sizeof(b), b,
				    sizeof(b), NULL) == 1,
		 "no guard state at all: admit");
	ct_check(vms_dlm_echo_admit(&g, CSID_VAX1, NULL, 0u, b, sizeof(b),
				    NULL) == 1,
		 "no request bytes: admit (there is no loop to see)");
	ct_check(vms_dlm_echo_admit(&g, CSID_VAX1, b, sizeof(b), NULL, 0u,
				    NULL) == 1,
		 "no answer bytes: admit");
	ct_check_eq_u32(g.refused, 0u, "and none of that counted as a refusal");
	vms_dlm_echo_guard_init(NULL);   /* must not fault */
}

/* ==========================================================================
 * 5. More conversing systems than slots
 * ========================================================================== */
static void more_peers_than_slots_still_caps_the_loop(void)
{
	struct vms_dlm_echo_guard g;
	uint8_t req[VMS_CM_BODY_LEN], ans[VMS_CM_BODY_LEN];
	uint32_t i, loud_refused;

	printf("-- more systems than slots: the loop is still the one that "
	       "gets capped\n");
	vms_dlm_echo_guard_init(&g);
	memset(req, 0x33, sizeof(req));
	memset(ans, 0x44, sizeof(ans));

	/* One loud system builds a run to the bound. */
	for (i = 0u; i < VMS_DLM_ECHO_MAX_SAME + 4u; i++)
		(void)vms_dlm_echo_admit(&g, CSID_VAX1, req, sizeof(req),
					 ans, sizeof(ans), NULL);
	loud_refused = g.refused;
	ct_check(loud_refused > 0u, "the loud system is capped");

	/* Now twice as many quiet systems as there are slots pass through, each
	 * asking once. The slot-reuse rule drops the SHORTEST runs, so the loud
	 * system's run survives... */
	for (i = 0u; i < VMS_DLM_ECHO_SLOTS * 2u; i++)
		(void)vms_dlm_echo_admit(&g, 0x20000000u + i, req, sizeof(req),
					 ans, sizeof(ans), NULL);
	(void)vms_dlm_echo_admit(&g, CSID_VAX1, req, sizeof(req), ans,
				 sizeof(ans), NULL);
	ct_check(g.refused > loud_refused,
		 "*** ...and the loud system is STILL capped after a crowd of "
		 "quiet ones passed through: a peer cannot flush the guard by "
		 "varying who asks ***");
}

/* ==========================================================================
 * 6. No number off the wire sets a loop bound
 * ========================================================================== */
static void a_bad_length_cannot_make_the_executive_loop(void)
{
	struct vms_dlm_echo_guard g;
	uint8_t b[VMS_CM_BODY_LEN];
	uint32_t i, admitted = 0;

	printf("-- a length from a received frame is CAPPED, not trusted\n");
	vms_dlm_echo_guard_init(&g);
	memset(b, 0x7e, sizeof(b));

	/*
	 * The length is the only caller-supplied loop bound on this path. Hand
	 * in a preposterous one -- what a mis-decoded frame would look like --
	 * and the guard must still return promptly and still SEE the sameness:
	 * if this test ever hangs, that is the bug it exists to catch (the
	 * suite's own 30 s ctest timeout is the detector).
	 */
	for (i = 0u; i < VMS_DLM_ECHO_MAX_SAME; i++) {
		if (vms_dlm_echo_admit(&g, CSID_VAX1, b, 0xffffffffu, b,
				       0xffffffffu, NULL))
			admitted++;
	}
	ct_check_eq_u32(admitted, VMS_DLM_ECHO_MAX_SAME,
			"the first answers go out (it returned at all: the "
			"4-billion-byte length did not become a loop)");
	ct_check(vms_dlm_echo_admit(&g, CSID_VAX1, b, 0xffffffffu, b,
				    0xffffffffu, NULL) == 0,
		 "*** and the run is still capped: capping the compared length "
		 "does not cost the guard its teeth ***");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("=== test_dlm_echo_guard (rd vms-b5b0: the real request storm, "
	       "R1 host unit) ===\n");

	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES, err,
				   sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}

	the_real_storm_is_capped();
	a_moving_dialogue_is_never_capped();
	a_looping_peer_does_not_silence_another();
	nothing_to_compare_means_answer();
	more_peers_than_slots_still_caps_the_loop();
	a_bad_length_cannot_make_the_executive_loop();

	return ct_summary("test_dlm_echo_guard");
}
