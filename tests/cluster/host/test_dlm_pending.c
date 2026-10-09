/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_pending.c - the unanswered-request table, and the deferred grant
 * built out of it (rd vms-f87, R1 host unit).
 *
 * WHAT HAPPENED. 2026-10-09 10:56Z: an OVMX master held EX on EVAC$WORKLOAD,
 * VAX1 $ENQW'd EX and was QUEUED at this master -- correctly, a real lock on a
 * real waiting queue. OVMX released, the engine flipped VAX1's request to
 * granted for real, and NOTHING WAS SENT. VAX1's process sat in RWSCS
 * indefinitely and could not be STOPped.
 *
 * The missing piece was not the decision -- the engine made it -- but the
 * FRAME: this codec builds a grant by ECHOING THE REQUEST (rd vms-b5b0, after a
 * 65,000-frame storm caused by laying the fields out by hand), and by the time
 * the queue advanced the request frame was long gone. This table keeps it.
 *
 * The second half of this file is the one that matters: the grant built from a
 * RETAINED request is byte-for-byte the grant the same request would have got
 * had it been grantable at once. That is what makes the deferred grant a real
 * answer to a real outstanding request rather than a frame this executive
 * composed.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_pending.h"

#define CSID_VAX1 0x00010002u
#define CSID_VAX2 0x00010003u
#define LK_VAX1   0x090003cdu
#define LK_VAX2   0x0a0b0002u

/* A real-shaped op-0x01 request body, built through the SHIPPING builder. */
static void a_request(uint8_t *body, uint32_t req_lkid, uint8_t mode,
		      const char *name)
{
	struct vms_dlm_enq_request rq;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t written = 0;
	size_t n = strlen(name);

	memset(&rq, 0, sizeof(rq));
	rq.mode = mode;
	rq.req_lkid = req_lkid;
	rq.name_len = (uint8_t)n;
	memcpy(rq.name, name, n);
	rq.res_group = 1u;
	rq.res_acmode = 3u;
	rq.res_ident_valid = 1u;
	memset(frame, 0, sizeof(frame));
	(void)vms_dlm_enq_request_build(&rq, VMS_DLM_WIREOP_ENQ, frame,
					(uint32_t)sizeof(frame), &written);
	memcpy(body, frame + VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
}

/* ==========================================================================
 * 1. The table
 * ========================================================================== */
static void the_table_keeps_one_answer_per_waiter(void)
{
	struct vms_dlm_pending p;
	uint8_t b1[VMS_CM_BODY_LEN], b2[VMS_CM_BODY_LEN];
	uint8_t out[VMS_CM_BODY_LEN];

	printf("-- one unanswered request per (system, handle)\n");
	vms_dlm_pending_init(&p);
	a_request(b1, LK_VAX1, VMS_LCK_EX, "EVAC$WORKLOAD");
	a_request(b2, LK_VAX2, VMS_LCK_EX, "EVAC$WORKLOAD");

	ct_check(vms_dlm_pending_keep(&p, CSID_VAX1, LK_VAX1, b1,
				      VMS_CM_BODY_LEN) == 1,
		 "VAX1's queued request is kept");
	ct_check(vms_dlm_pending_keep(&p, CSID_VAX2, LK_VAX2, b2,
				      VMS_CM_BODY_LEN) == 1,
		 "VAX2's too -- two waiters on one resource is the ci.6 shape");
	ct_check_eq_u32(vms_dlm_pending_held(&p), 2u, "both held");
	ct_check_eq_u32(p.kept, 2u, "and counted");

	/* TAKING IT IS AN ANSWER: the slot is freed, so one flip sends one
	 * grant and a second flip for the same waiter finds nothing. */
	ct_check_eq_u32(vms_dlm_pending_take(&p, CSID_VAX1, LK_VAX1, out,
					     sizeof(out)),
			(unsigned long)VMS_CM_BODY_LEN,
			"VAX1's frame comes back whole");
	ct_check(memcmp(out, b1, VMS_CM_BODY_LEN) == 0,
		 "*** byte for byte the frame VAX1 really sent ***");
	ct_check_eq_u32(vms_dlm_pending_take(&p, CSID_VAX1, LK_VAX1, out,
					     sizeof(out)), 0u,
			"*** and it is GONE: one owed answer, one grant ***");
	ct_check_eq_u32(vms_dlm_pending_held(&p), 1u, "VAX2's is untouched");

	/* A retransmit is the SAME request: it replaces, never duplicates. */
	ct_check(vms_dlm_pending_keep(&p, CSID_VAX2, LK_VAX2, b2,
				      VMS_CM_BODY_LEN) == 1, "VAX2 re-sends");
	ct_check_eq_u32(vms_dlm_pending_held(&p), 1u,
			"still ONE slot for it -- a retransmit is the same "
			"request, and the newest bytes are the ones to echo");
	ct_check_eq_u32(p.replaced, 1u, "counted as a replacement");
}

/* ==========================================================================
 * 2. What it refuses, and what it does when it is full
 * ========================================================================== */
static void nothing_unsendable_is_ever_kept(void)
{
	struct vms_dlm_pending p;
	uint8_t b[VMS_CM_BODY_LEN];
	uint32_t i;

	printf("-- a frame that could not be sent is not kept\n");
	vms_dlm_pending_init(&p);
	a_request(b, LK_VAX1, VMS_LCK_EX, "EVAC$WORKLOAD");

	ct_check(vms_dlm_pending_keep(&p, CSID_VAX1, LK_VAX1, NULL,
				      VMS_CM_BODY_LEN) == 0,
		 "no body: refused (there would be nothing to echo)");
	ct_check(vms_dlm_pending_keep(&p, CSID_VAX1, LK_VAX1, b, 8u) == 0,
		 "a SHORT body: refused -- a partial frame is not a frame");
	ct_check(vms_dlm_pending_keep(&p, CSID_VAX1, 0u, b,
				      VMS_CM_BODY_LEN) == 0,
		 "lock id 0: refused (the one value a handle never takes)");
	ct_check(vms_dlm_pending_keep(&p, 0u, LK_VAX1, b,
				      VMS_CM_BODY_LEN) == 0,
		 "and an unidentified requester: refused");
	ct_check_eq_u32(vms_dlm_pending_held(&p), 0u, "nothing was stored");
	ct_check_eq_u32(p.overflow, 0u, "and none of that is an overflow");

	/* FULL: the request is still queued in the lock database, so the
	 * behaviour is the pre-existing one -- answered on the next ask -- and
	 * the overflow is counted rather than felt. */
	printf("-- a full table is counted, not hidden\n");
	for (i = 0u; i < VMS_DLM_PENDING_MAX; i++) {
		ct_check(vms_dlm_pending_keep(&p, CSID_VAX1, 0x1000u + i, b,
					      VMS_CM_BODY_LEN) == 1,
			 i == 0u ? "the table fills" : NULL);
	}
	ct_check_eq_u32(vms_dlm_pending_held(&p), VMS_DLM_PENDING_MAX,
			"every slot in use");
	ct_check(vms_dlm_pending_keep(&p, CSID_VAX1, 0x2000u, b,
				      VMS_CM_BODY_LEN) == 0,
		 "*** one more is REFUSED, not silently dropped on top of "
		 "another waiter's frame ***");
	ct_check_eq_u32(p.overflow, 1u, "and counted");

	/* A system that has left is owed nothing. */
	ct_check_eq_u32(vms_dlm_pending_forget_system(&p, CSID_VAX1),
			VMS_DLM_PENDING_MAX,
			"a departed system's frames are all released -- a dead "
			"conversation is not held open");
	ct_check_eq_u32(vms_dlm_pending_held(&p), 0u, "the table is empty");
	ct_check(vms_dlm_pending_drop(&p, CSID_VAX1, 0x1000u) == 0,
		 "and dropping what is not there is not an error");
	vms_dlm_pending_init(NULL);                 /* must not fault */
	ct_check(vms_dlm_pending_keep(NULL, 1u, 1u, b, VMS_CM_BODY_LEN) == 0,
		 "no table at all: nothing is kept and nothing faults");
}

/* ==========================================================================
 * 3. *** THE POINT: the deferred grant IS the prompt grant ***
 * ========================================================================== */
static void the_deferred_grant_is_the_prompt_grant(void)
{
	struct vms_dlm_pending p;
	uint8_t reqbody[VMS_CM_BODY_LEN];
	uint8_t kept[VMS_CM_BODY_LEN];
	uint8_t prompt[VMS_CM_FRAME_LEN], deferred[VMS_CM_FRAME_LEN];
	struct vms_dlm_enq_response parsed;
	uint32_t written = 0, n;
	const uint32_t MASTER_LKID = 0x650006b0u;

	printf("-- *** the grant built from a RETAINED request is the grant "
	       "the requester would have got at once ***\n");
	vms_dlm_pending_init(&p);
	a_request(reqbody, LK_VAX1, VMS_LCK_EX, "EVAC$WORKLOAD");

	/* The grant this master WOULD have sent, had the lock been free. */
	memset(prompt, 0, sizeof(prompt));
	ct_check(vms_dlm_enq_response_build_grant(reqbody, VMS_CM_BODY_LEN,
						  MASTER_LKID, NULL, prompt,
						  (uint32_t)sizeof(prompt),
						  &written) == VMS_CODEC_OK,
		 "the PROMPT grant builds");

	/* Instead the request queued, so its frame was kept... */
	ct_check(vms_dlm_pending_keep(&p, CSID_VAX1, LK_VAX1, reqbody,
				      VMS_CM_BODY_LEN) == 1,
		 "the queued request's frame is kept");
	/* ...and when the holder released, this is the grant that goes out. */
	n = vms_dlm_pending_take(&p, CSID_VAX1, LK_VAX1, kept, sizeof(kept));
	ct_check_eq_u32(n, (unsigned long)VMS_CM_BODY_LEN,
			"the frame comes back when the queue advances");
	memset(deferred, 0, sizeof(deferred));
	ct_check(vms_dlm_enq_response_build_grant(kept, n, MASTER_LKID, NULL,
						  deferred,
						  (uint32_t)sizeof(deferred),
						  &written) == VMS_CODEC_OK,
		 "the DEFERRED grant builds");

	ct_check(memcmp(prompt + VMS_OFF_SYSAP_BODY,
			deferred + VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN) == 0,
		 "*** AND THE TWO ARE IDENTICAL, all 132 body bytes: a "
		 "deferred grant is not a new frame shape, it is the same "
		 "answer sent later ***");

	/* And it is readable as a grant carrying the REQUESTER's own handle --
	 * the correlation a real master's grant is matched by (27,513 of
	 * 27,513 in the f03 reference capture; tools/cluster/
	 * dlm_grant_correlation.py). */
	ct_check(vms_dlm_enq_response_parse_body(deferred + VMS_OFF_SYSAP_BODY,
						 VMS_CM_BODY_LEN,
						 &parsed) == VMS_CODEC_OK,
		 "it parses as an ENQ response");
	ct_check_eq_u32(parsed.outcome, (unsigned long)VMS_DLM_ENQ_GRANTED,
			"  a GRANT (the 0xfa outcome byte)");
	ct_check_eq_u32(parsed.req_lkid, LK_VAX1,
			"*** carrying the WAITER's own handle, which is how it "
			"matches the $ENQW still outstanding on that node ***");
	ct_check_eq_u32(parsed.master_lkid, MASTER_LKID,
			"  and our handle in the master's slot");
}

int main(void)
{
	printf("=== test_dlm_pending (rd vms-f87: the deferred grant's frame, "
	       "R1 host unit) ===\n");

	the_table_keeps_one_answer_per_waiter();
	nothing_unsendable_is_ever_kept();
	the_deferred_grant_is_the_prompt_grant();

	return ct_summary("test_dlm_pending");
}
