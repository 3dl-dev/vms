/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_scs_arm.c - R1 for the DLM's WIRE ARM (src/kernel-core/vms_dlm_scs.c,
 * rd vms-1ee blocker 2) and for the two places the connection manager carries
 * it (vms_cnxman.c's send + receive legs, vms_devtab.c's CLUSTER_START).
 *
 * TWO PROOFS, on the terms test_cnxman_glue.c and test_scs_glue_conn.c already
 * established for a glue TU that is NOT host-linkable (the arm names
 * exec_kbackend.h, vms_internal.h and the FC-P0.5 fork API):
 *
 *   1. THE TRUST ANCHOR, against the REAL object that holds it. The arm's
 *      split-brain gate reads ONE fact -- `csb->peer_is_ours` -- and this file
 *      drives the real vms_cnxman_csb.c derivation that sets it, including the
 *      two ways it reads 0. That is the whole predicate both halves of the gate
 *      turn on, proven against the shipping code rather than restated.
 *
 *   2. THE WIRING AND ITS ORDER, read out of the THREE SHIPPING FILES. Some of
 *      the arm's most important properties are about ORDER, and order is not
 *      something a call-site scan can assert by presence alone -- so this file
 *      compares OFFSETS in the shipped text:
 *        - the RULE C gate is BELOW the op-0x0d rebuild branch (a real VAX's
 *          rebuild record must still reach the grounded verbatim echo; gating it
 *          would make the barrier skip the echo and strand that VAX);
 *        - the all-OVMX gate is ABOVE the BLKAST emit, and RULE C is ABOVE the
 *          port (rd vms-d7a3: the master now ORIGINATES an op-0x05 blocking AST
 *          at a remote holder, and a new outbound frame shape that could reach
 *          a system this tree has not proved runs this implementation is the
 *          whole peer-crash vector this program exists to not have);
 *        - the DLM receive leg is AFTER the join/barrier/coordinator dispatch
 *          (so the barrier keeps the op-0x0d record it owns inside a
 *          transition);
 *        - CLUSTER_START registers the delivery proc BEFORE it starts the arm
 *          (so no inbound request can find the arm up and its owner missing).
 *
 * A future edit that drops a binding, moves the gate above the rebuild branch,
 * or routes the DLM before the barrier reddens this file.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"

#include "vms_cluster.h"
#include "vms_cnxman_csb.h"

/* ==========================================================================
 * 1. THE TRUST ANCHOR -- csb->peer_is_ours, against the real derivation
 *
 * "Is this member PROVABLY running the same implementation we are?" is answered
 * by exactly one comparison, in the one place both tokens are in scope: the
 * peer's advertised software-version token (spec §4(g), copied out of the
 * port's circuit) against THIS node's own advertised token. A real VAX
 * advertises its real "VMS Vx.y" and never matches.
 *
 * BOTH ZERO CASES MATTER. "advertised something else" and "advertised nothing"
 * are different facts about the cluster and the SAME fact about trust: neither
 * is proof (INV-6), and the gate treats them identically.
 * ========================================================================== */
static const uint8_t OWN_TOKEN[8]   = { 'O','V','M','X',' ','0','.','6' };
static const uint8_t OTHER_TOKEN[8] = { 'V','M','S',' ','V','7','.','3' };

static void trust_anchor_is_the_advertised_version(void)
{
	struct vms_csb csb;

	printf("-- the trust anchor: csb->peer_is_ours (the E80 identity) --\n");

	memset(&csb, 0, sizeof(csb));
	cnxman_csb_set_swver(&csb, OWN_TOKEN, (uint8_t)sizeof(OWN_TOKEN),
			     OWN_TOKEN, (uint8_t)sizeof(OWN_TOKEN));
	ct_check(csb.peer_is_ours == 1u && csb.peer_swver_len == 8u,
		 "a byte-identical advertised version PROVES the peer is ours");

	memset(&csb, 0, sizeof(csb));
	cnxman_csb_set_swver(&csb, OTHER_TOKEN, (uint8_t)sizeof(OTHER_TOKEN),
			     OWN_TOKEN, (uint8_t)sizeof(OWN_TOKEN));
	ct_check(csb.peer_is_ours == 0u && csb.peer_swver_len == 8u,
		 "a DIFFERENT advertised version (a real VAX) is NOT proof -- "
		 "and the token it really sent is still recorded");

	memset(&csb, 0, sizeof(csb));
	cnxman_csb_set_swver(&csb, NULL, 0u, OWN_TOKEN,
			     (uint8_t)sizeof(OWN_TOKEN));
	ct_check(csb.peer_is_ours == 0u && csb.peer_swver_len == 0u,
		 "advertising NOTHING is not proof either, and is recorded as "
		 "nothing rather than as a match");

	/* The degenerate case a gate must not get wrong: a peer that advertised
	 * a PREFIX of our token is not us. */
	memset(&csb, 0, sizeof(csb));
	cnxman_csb_set_swver(&csb, OWN_TOKEN, 4u, OWN_TOKEN,
			     (uint8_t)sizeof(OWN_TOKEN));
	ct_check(csb.peer_is_ours == 0u,
		 "a PREFIX of our own token is not a match (length counts)");
}

/* ==========================================================================
 * 2. THE WIRING, read out of the shipping files
 * ========================================================================== */
static char src[400000];
static const char *src_name;

static int read_src(const char *dir, const char *name)
{
	char path[512];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	n = fread(src, 1u, sizeof(src) - 1u, f);
	fclose(f);
	src[n] = '\0';
	src_name = name;
	return 0;
}

static void has(const char *needle, const char *what)
{
	ct_check(strstr(src, needle) != NULL, what);
}

static void absent(const char *needle, const char *what)
{
	ct_check(strstr(src, needle) == NULL, what);
}

/* `first` must appear BEFORE `second` in the shipped text. The order-bearing
 * half of this file: presence alone cannot state "the gate is below the rebuild
 * branch", and that ordering is the difference between a real VAX's rebuild
 * completing and being stranded. */
static void before(const char *first, const char *second, const char *what)
{
	const char *a = strstr(src, first);
	const char *b = strstr(src, second);

	ct_check(a != NULL && b != NULL && a < b, what);
}

static void arm_bindings(void)
{
	printf("-- the arm, read out of src/kernel-core/vms_dlm_scs.c --\n");
	if (read_src(OVMX_KCORE_DIR, "vms_dlm_scs.c") != 0) {
		ct_check(0, "could not open vms_dlm_scs.c");
		return;
	}

	/* Both directions are installed, and torn down again. */
	has("cnxman_set_dlm(cl, &d->role)",
	    "start: the role ops are registered with the connection manager");
	has("vms_lock_dlm_set_requester_ops(&d->eng_ops)",
	    "start: the engine's requester ops are installed");
	has("vms_lock_dlm_set_requester_ops(NULL)",
	    "stop: the engine goes back to purely local operation");
	has("cnxman_set_dlm(cl, NULL)",
	    "stop: the connection manager stops handing it messages");
	has("vms_lock_dlm_set_delivery_proc(NULL)",
	    "stop: the delivery-proc registration is cleared");

	/* The engine's post is QUEUED to the fork thread and REBUILT there --
	 * never built and sent on the $ENQ caller's own thread (the lock order
	 * forbids it, and a refill is a fresher executive read anyway). */
	has("cf_post(d->cl->fork, &w)",
	    "a post is QUEUED to the fork thread, not sent inline");
	has("dlm_arm_refill_post(d, req_lkid, op",
	    "... and REBUILT from the lock database on the fork thread");
	has("dlm_req_fsm_post(&d->req, &p)",
	    "... before the pure requester FSM is driven");

	/* RULE B: the reply's fields come from the engine's result object. */
	/*
	 * THE GRANT IS BUILT BY ECHOING THE REQUEST (rd vms-b5b0): the only
	 * field this node asserts is the handle its own engine assigned. The
	 * requester's handle, the resource identity, the directory hash and
	 * everything else in that frame are the REQUESTER's own bytes, handed
	 * back -- which is what a real master does, and what makes the
	 * requester able to correlate the completion. Reading the two handles
	 * out of OUR result object and writing them into our own slot order was
	 * the storm.
	 */
	has("vms_dlm_enq_response_build_grant(in->body, in->len,",
	    "the GRANT reply ECHOES THE REQUEST's own body");
	has("in->len, r->master_lkid,",
	    "... and the ONE value it asserts is the handle our engine "
	    "assigned, plus the master resource's value block");
	absent("r->granted_mode",
	       "*** no granted mode is written into a grant: a real grant "
	       "clears body[30] (38 of 38) and the requester grants the mode "
	       "its own LKB asked for ***");
	absent("VMS_DLM_LKID_UNSET, ",
	       "no builder is ever called with the unset-lock-id sentinel");

	/*
	 * THE DEFERRED GRANT IS ORIGINATED NOW (rd vms-f87), and from the
	 * requester's OWN frame. Measured cost of the old silence: a real VAX
	 * whose $ENQW queued at an OVMX master was never told it had been
	 * granted, and its process sat in RWSCS unkillable.
	 */
	has("vms_dlm_pending_keep(&d->pending",
	    "a request the engine QUEUED has its frame kept (the answer we owe)");
	has("dlm_arm_send_deferred_grant(d, &res);",
	    "*** and the RELEASE path really CALLS the origination -- not just "
	    "defines it ***");
	has("vms_dlm_pending_take(&d->pending, r->deferred_csid",
	    "... and the flip takes that frame back out, keyed by the waiter "
	    "the ENGINE named");
	has("vms_dlm_enq_response_build_grant(reqbody, n,",
	    "... so the deferred grant is the requester's OWN body, echoed -- "
	    "never a frame composed from fields here");
	has("d->deferred_grants_sent++",
	    "... and an originated grant is COUNTED");
	has("d->deferred_grants_no_body++",
	    "... while a flip whose frame is NOT held stays silent, counted "
	    "(INV-6: no invented frame to fill the gap)");
	absent("dlm_arm_count_deferred_grant",
	       "*** and the old count-and-say-nothing path is GONE ***");

	/* RULE A: the arm fills a buffer; it never sends a reply. */
	has("memcpy(reply->body, d->txframe + VMS_OFF_SYSAP_BODY",
	    "a reply is STAGED into the connection manager's buffer");
	absent("cnxman_dlm_send(d->cl, (vms_csid_t)r->deferred",
	       "the arm originates no grant of its own: there is no grounded "
	       "release to flip one (the counted gap, not a guess)");

	/* RULE C, and its ORDER. */
	has("if (!dlm_arm_peer_is_ours(d, req))",
	    "RULE C: a sender that is not proven ours is refused");
	has("d->foreign_refused++", "... and the refusal is COUNTED");
	before("VMS_DLM_WIREOP_REBUILD",
	       "if (!dlm_arm_peer_is_ours(d, req))",
	       "RULE C's gate is BELOW the op-0x0d rebuild branch -- a real "
	       "VAX's rebuild record still reaches the grounded echo");
	before("dlm_req_fsm_observe_body(&d->req, req->body",
	       "if (!dlm_arm_peer_is_ours(d, req))",
	       "... and the directory hash is learned from ANY sender, because "
	       "learning emits nothing (Davis p. 6-50)");

	/* rd vms-8219: THE DIRECTORY ROLE toward a real VMS system -- the one
	 * thing above RULE C that may answer, and only with the real
	 * directory's own answer shape. */
	has("if (dlm_arm_directory(d, req, reply) == 0)",
	    "the directory role is consulted for every cat-0x02 request");
	before("dlm_arm_dir_register(d, req);",
	       "if (dlm_arm_directory(d, req, reply) == 0)",
	       "op-0x0d registrations are recorded in the rebuild branch, which "
	       "still hands the answer to the grounded echo");
	before("if (dlm_arm_directory(d, req, reply) == 0)",
	       "if (!dlm_arm_peer_is_ours(d, req))",
	       "the directory role sits ABOVE RULE C (its answer is the real "
	       "directory's byte-for-byte; test_dlm_dir.c)");
	has("if (req->peer_is_ours || req->from_csid == 0u ||",
	    "... and serves ONLY a system that is not this implementation, with "
	    "a known CSID (between OVMX nodes the engine owns the directory)");
	has("if (vms_dlm_dir_answer_build(req->body, req->len, status,",
	    "every directory answer is built by the grounded builder");
	/*
	 * rd vms-025, THE 2026-10-09 LAB FINDING: THE REFUSAL AND THE VECTOR
	 * MUST BE READABLE FROM THE CONSOLE.
	 *
	 * The lab watched a real VAX's op-0x01 for a resource this node held
	 * AND mastered go unanswered under two console lines that between them
	 * named the wrong fault -- "this node ... does NOT master" (it did) and
	 * "a system that has not proved it runs this implementation" (true of
	 * every refusal here, so it distinguishes nothing) -- and nothing said
	 * what the weight vector had come out as. All three are pinned here, in
	 * the shipped text.
	 */
	has("d->dir_master_unserved++;",
	    "a resource this node DOES master and could not serve is counted "
	    "apart from one it does not master");
	has("for a resource this node MASTERS, and it could not be",
	    "... and SAID as that, never as 'does not master'");
	has("if (!dlm_arm_refusal_is_a_servable_shape(req->opcode)) {",
	    "RULE C's refusal distinguishes a shape this implementation has no "
	    "grounded answer for");
	has("d->refused_no_identity++;",
	    "... from a frame that states no resource identity at all");
	has("d->refused_unserved_res++;",
	    "... from a placeable resource this node neither masters nor holds "
	    "a directory entry for");
	has("check that every member agrees on LOCKDIRWT and on the",
	    "... and the last one NAMES LOCKDIRWT, the knob the lab could not "
	    "set");
	has("dlm_arm_say_ldwv(d);",
	    "every state transition SAYS what the lock directory weight vector "
	    "came out as -- the fact the lab had to infer");
	has("out->directory_vector_own = dlm_arm_own_dir_entries(",
	    "and the diagnostic projection carries how many of the vector's "
	    "entries are this node's (CNXTRACE dirvec_own=)");

	has("if (dlm_arm_dir_name_held(id)) {",
	    "a name THIS node holds locks on is never answered 'you master it'");
	has("o = dlm_arm_dir_ask(d, id, req->from_csid, &master);",
	    "the outcome comes from this node's own directory entries");
	has("vms_dlm_dir_lookup(&d->dir, id, from,",
	    "... through the one accessor that holds the table's leaf lock, "
	    "because the LOCK ENGINE reads the table too now (rd vms-025)");
	/* rd vms-629: a VAX's barrier lookup (op 0x08) is answered like an
	 * op-0x01 one -- unanswered, the VAX's barrier never releases. */
	has("req->opcode == (uint8_t)VMS_DLM_WIREOP_DIR_LOOKUP_TR)\n\t\treturn dlm_arm_dir_lookup(d, req, &id, reply);",
	    "an op-0x08 barrier lookup is routed to the directory's lookup");
	before("if (dlm_arm_dir_tr_redirect(d, req, o))",
	       "if (vms_dlm_dir_answer_build(req->body, req->len, status,",
	       "... and a redirect answer to one, never observed, is held back "
	       "before anything is built");

	/*
	 * THE HASH BOOTSTRAP DEADLOCK IS GONE, AND SO ARE THE TWO OPS THAT
	 * BRIDGED IT (rd vms-b5b0, retiring rung A"/vms-3e3). The deadlock was:
	 * installing the resolver turned on vms_lock.c's "no wire-learned hash ->
	 * SS$_UNSUPPORTED" refusal, and no OVMX-only member could originate the
	 * first cat-0x02 frame to teach a value. The bridge was OVMX's OWN hash
	 * behind an all-OVMX gate (`dir_groundable` + `dir_ground`). The VMS
	 * function is now determined and proven, the engine computes it directly,
	 * and keeping a second hash would make a name's master change the moment a
	 * VAX joined -- so both ops are DELETED, which is what these three
	 * assertions pin.
	 */
	has("d->eng_ops.dir_resolve    = dlm_arm_eng_dir_resolve;",
	    "the engine's directory resolver IS installed");
	/* rd vms-b5b0: the ONE question the vector can answer without a value,
	 * and the reason an uncovered identity is not simply mastered blind. */
	has("d->eng_ops.dir_all_ours   = dlm_arm_eng_dir_all_ours;",
	    "...and so is the read that asks whether the vector directs "
	    "EVERYTHING here -- the one answer that needs no value");
	has("return vms_ldwv_directs_everything_here(&d->cl->club.ldwv);",
	    "...which is one read of the connection manager's own committed "
	    "vector, derived every time it is asked");
	absent("dir_groundable",
	      "*** the all-OVMX GATE on routing is GONE from this arm "
	      "(rd vms-b5b0) ***");
	absent("vms_dlm_ovmx_dir_hash",
	      "*** and so is OVMX's OWN directory hash: one function "
	      "cluster-wide, VMS's own ***");
	absent("16777619u",
	      "... not even its FNV-1a constant is left in this file (the echo "
	      "guard's private signature lives in its OWN pure TU, "
	      "vms_dlm_echo_guard.c, where no wire value can reach it)");

	/*
	 * THE ECHO GUARD is asked on the ONE path every reply passes through
	 * (rd vms-b5b0) -- not in one shape's builder, because a reply loop is
	 * a loop whether the repeated answer is a grant, a deny or a directory
	 * answer.
	 */
	has("if (!dlm_arm_echo_ok(d, in))",
	    "the ECHO GUARD is asked before any reply is staged");
	has("vms_dlm_echo_admit(&d->echo",
	    "... through the pure guard TU's own entry point");
	has("d->answers_capped++",
	    "... and a withheld answer is COUNTED");
	has("d->eng_ops.post           = dlm_arm_post;",
	    "the engine's POST op is installed too -- the remote route it serves is "
	    "now reachable behind the gate");

	/*
	 * THIS NODE'S CLUSTER IDENTITY. The engine's vms_local_csid is each
	 * substrate's insmod placeholder until something binds it to the
	 * cluster's own assignment -- measured on the two-node rig, BOTH MEMBERs
	 * reported local_csid=0x00000001 while really holding 0x00010001 and
	 * 0x00010002. The arm syncs it from the CLUB, at the two moments the
	 * CLUB can have learned one.
	 */
	has("vms_lock_dlm_set_local_csid((uint32_t)d->cl->club.local_csid)",
	    "the engine's CSID is synced from the CLUB's own assignment");
	has("if (d->cl == NULL || !d->cl->club.local_csid_valid)",
	    "... and ONLY when the cluster has really assigned one (INV-6)");

	/* CONDITION 4 and the honest floors. */
	has("if (!vms_lock_dlm_have_delivery_proc())",
	    "vms-c27 cond.4: no delivery proc, no service");
	has("d->no_delivery_proc++", "... counted");

	/*
	 * ===================================================================
	 * THE BLOCKING AST IS EMITTED (op 0x05, rd vms-d7a3), AND EVERY FIELD
	 * OF IT IS AN EXECUTIVE READ.
	 *
	 * This is the half of the item this file can prove: the arm is not
	 * host-linkable, so what is asserted is the SOURCING and the GATE
	 * ORDER, read out of the shipped text. The codec builder it calls is
	 * driven with real values by test_codec_dlm.c, and the trust fact the
	 * emission gate turns on is driven against the real derivation at the
	 * top of this file.
	 *
	 * A regression that sources a lock id from the REQUEST instead of the
	 * blocking LKB, that invents the OBSERVED-not-pinned mode context, or
	 * that reaches the port without passing the two gates, reddens this.
	 * ===================================================================
	 */
	has("vms_dlm_blkast_build(&b, d->txframe",
	    "the BLKAST frame is built by the SHIPPING codec, never by hand");
	has("b.master_lkid    = r->blocking_master_lkid;",
	    "body[24:28] is the BLOCKING LKB's own lock id, as this master "
	    "minted it -- read off the granted queue, never echoed");
	has("b.req_lkid       = r->blocking_req_lkid;",
	    "body[20:24] is the HOLDER's own handle, stamped on that LKB when "
	    "this master served the holder's request");
	has("b.mode_ctx_valid = 0u;",
	    "the OBSERVED-but-NOT-PINNED mode context at body[30:32] is "
	    "OMITTED -- the honest omission, never two invented bytes");
	absent("b.mode_ctx[",
	       "... and nothing writes a value into it either");
	has("dlm_arm_send(d, (vms_csid_t)r->blocking_csid,",
	    "it is sent to the HOLDER's own CSID, through the seam that "
	    "applies RULE C per destination (cnxman_dlm_send)");
	has("if (!dlm_arm_all_ovmx(d) || dlm_arm_build_blkast(d, r) != 0 ||",
	    "... and ONLY when the cluster is all-proven-OVMX: the gate is "
	    "evaluated BEFORE a frame is even built");
	before("if (!dlm_arm_all_ovmx(d)",
	       "dlm_arm_send(d, (vms_csid_t)r->blocking_csid",
	       "the all-OVMX gate is UPSTREAM of the send, not beside it");
	has("d->blkasts_no_wire_op++",
	    "every way the notification can fail to go out -- off-gate, no "
	    "route, RULE C, a refused lock id -- is COUNTED, nothing sent");
	/*
	 * ... AND THE SOLE-DIRECTORY INTERIM DOES NOT OPEN IT (rd vms-025/db2a).
	 * That configuration lets this node serve a real VMS system as master,
	 * and the master OWES a blocking AST to a remote holder it blocks --
	 * which this executive STILL withholds, because op-0x05's mode-context
	 * pair at body[30:32] is observed-and-not-pinned and OVMX would have to
	 * put a zero where every real frame carries data. The engine's decision
	 * is real and proven (test_dlm_mixed_master.c names the holder from the
	 * blocking LKB); the FRAME is the gap, and this is the assertion that
	 * keeps the gap from being closed by accident instead of by a capture.
	 */
	absent("dlm_arm_sole_directory(d) || dlm_arm_build_blkast",
	       "*** the BLKAST gate is NOT widened by the sole-directory "
	       "interim: body[30:32] is unpinned, so it stays all-OVMX only "
	       "***");
	has("d->blkasts_sent++",
	    "... and one that really went out is counted separately");
	absent("scs_send_msg",
	       "*** the arm never reaches the PORT directly: every byte it "
	       "originates goes through cnxman_dlm_send, which is where RULE "
	       "C's emission half lives ***");

	/*
	 * ===================================================================
	 * THE RELEASE IS STAGED, NOT REBUILT (rd vms-49f8) -- and the ORDER is
	 * the whole fix.
	 *
	 * Every other post is queued by lock id and REBUILT on the fork thread
	 * from the lock database (asserted above). A $DEQ cannot be: the release
	 * IS the destruction of the proxy LKB, so the rebuild answered "no such
	 * lock" and #1165's op-0x03 emit had NO reachable caller at all
	 * (measured on the live 2-node rig: releases_sent=0, posts_lock_gone=2).
	 *
	 * What is pinned here is what a source scan is the right tool for: that
	 * the SNAPSHOT is taken BEFORE the work item is queued -- i.e. in the
	 * releaser's own context, while the LKB is still real -- and that the
	 * fork thread's DEQ branch CLAIMS that snapshot instead of refilling.
	 * The behaviour those two lines produce is driven end-to-end against the
	 * REAL engine, the REAL queue, the REAL FSM and the REAL codec in
	 * test_dlm_deq_reachable.c.
	 * ===================================================================
	 */
	has("return dlm_arm_post_release(d, p);",
	    "a RELEASE takes the staged path, not the rebuild path");
	has("st = dlm_arm_relq_stage(d, p, &slot, &seq);",
	    "... which SNAPSHOTS the post the engine just read from the live "
	    "LKB");
	before("st = dlm_arm_relq_stage(d, p, &slot, &seq);",
	       "if (dlm_arm_queue_work(d, DLM_ARM_WORK_POST_DEQ, slot, seq) != 0)",
	       "*** the snapshot is taken BEFORE the work is queued -- in the "
	       "releaser's own context, which is the last moment the lock "
	       "exists ***");
	has("dlm_arm_relq_abandon(d, slot, seq);",
	    "a work item the fork queue would not take gives the slot back -- "
	    "no staged release is orphaned");
	has("st = dlm_relq_claim(&d->relq, slot, seq, out);",
	    "the fork thread CLAIMS the snapshot");
	before("dlm_arm_run_release(d, w->arg0, w->arg1);",
	       "dlm_arm_run_post(d, w->kind, w->arg0, w->arg1);",
	       "... and the work handler routes a release to the claim BEFORE "
	       "the rebuild path it must never take");
	has("if (op == 0u || op == VMS_DLM_POST_DEQ)",
	    "*** the rebuild path REFUSES a release outright: there is no lock "
	    "left to read, and a frame about a lock that no longer exists is a "
	    "frame with no object behind it ***");
	has("d->releases_staged++",
	    "a staged release is counted");
	has("d->releases_no_slot++",
	    "... a queue-full refusal is counted, and NOTHING is sent");
	has("d->releases_stale++",
	    "... and a work item naming no staged release emits nothing, "
	    "counted (a release goes out once or not at all)");
	has("exec_lock_init(&d->relq_lock);",
	    "the queue is a THREAD CROSSING and carries its own executive lock "
	    "-- never the fork mutex, which a lock-manager path may not take");
	has("dlm_relq_init(&d->relq);",
	    "... and it starts empty, before the engine's ops are installed");

	/* The requester FSM's op-0x03 gate RETIRED with the ops behind it. */
	absent("d->req_ops.all_ovmx",
	       "*** the requester arm's op-0x03 membership gate is GONE "
	       "(rd vms-b5b0): a lock taken at a real VAX master has to be "
	       "releasable, and which shapes may face an unproven member is "
	       "now ONE codec predicate applied per destination ***");
	absent("d->req_ops.mixed_dlm_ok",
	       "... and so is its sole-directory escape hatch");
	has("dlm_arm_all_ovmx(d) || dlm_arm_build_blkast(d, r) != 0",
	    "the all-OVMX read that REMAINS is the op-0x05 BLKAST's, whose "
	    "body[30:32] is observed-and-not-pinned -- the one shape the codec "
	    "does not clear for an unproven member either");

	/*
	 * ===================================================================
	 * THE RECEIVE HALF IS WIRED (op 0x03 and op 0x05, rd vms-c72), AND
	 * BOTH PATHS ARE BELOW RULE C.
	 *
	 * Measured on the live 2-node rig, both emits fired and the PEER threw
	 * them away: the inbound dispatch routed ONLY op-0x01/op-0x07 to the
	 * engine and everything else fell to a counted decline
	 * (blkasts_received=0, blkasts_delivered=0, unparsed rising). The
	 * codec could parse both already -- the gap was the ROUTING and the
	 * engine-delivery call.
	 *
	 * What a source scan is the right tool for is exactly what is pinned
	 * here: that the two opcodes are routed at all, that each reaches the
	 * right engine door, and that BOTH sit BELOW the RULE C gate -- an
	 * op-0x03 destroys real lock state and an op-0x05 fires a real
	 * user-mode AST, and neither may happen for a system that has not
	 * proved it runs this implementation. What those doors DO to real lock
	 * state is driven end-to-end against the REAL engine, the REAL FSM and
	 * the REAL codec in test_dlm_recv_arm.c.
	 * ===================================================================
	 */
	has("if (req->opcode == (uint8_t)VMS_DLM_WIREOP_DEQ)",
	    "an inbound op-0x03 $DEQ is ROUTED, not declined unparsed");
	has("return dlm_arm_serve_deq(d, req);",
	    "... to the master-side serve path");
	has("if (req->opcode == (uint8_t)VMS_DLM_WIREOP_BLKAST)",
	    "an inbound op-0x05 BLKAST is ROUTED too");
	has("return dlm_arm_deliver_blkast(d, req);",
	    "... to the holder-side delivery path");
	before("if (!dlm_arm_peer_is_ours(d, req))",
	       "if (req->opcode == (uint8_t)VMS_DLM_WIREOP_BLKAST)",
	       "*** RULE C is evaluated BEFORE the inbound BLKAST -- an AST is "
	       "never fired on behalf of an unproven system ***");

	/*
	 * ===================================================================
	 * ... AND THE ONE WAY PAST IT, STATED EXACTLY (rd vms-025 / vms-db2a).
	 *
	 * This used to assert that RULE C was evaluated before the inbound
	 * op-0x03 was served -- "a release from an unproven system releases
	 * NOTHING". That is no longer the whole truth and the assertion is
	 * REPLACED rather than deleted: in the SOLE-DIRECTORY configuration
	 * this node acts as MASTER for a real VMS system, and a master that
	 * takes a system's lock requests but refuses its releases hands that
	 * system a resource it can never let go of.
	 *
	 * So the claim is narrowed to what the code actually does, and every
	 * clause of it is pinned here: the door is ONE function, it is gated on
	 * the configuration BEFORE it looks at an opcode, and the AUTHORITY is
	 * still the engine's -- the LKB's own req_csid tag, which refuses a
	 * release of a lock held for anybody else (driven against the real
	 * engine in test_dlm_recv_arm.c, "a peer may not release another
	 * node's lock", and in the mixed flows in test_dlm_mixed_master.c).
	 * The op-0x05 BLKAST is NOT in that door, which is why its RULE C
	 * ordering assertion above is untouched.
	 * ===================================================================
	 */
	has("if (dlm_arm_serve_mixed_held(d, req, reply) == 0)",
	    "the mixed-cluster master role is ONE door in the dispatch");
	before("if (dlm_arm_serve_mixed_held(d, req, reply) == 0)",
	       "if (!dlm_arm_peer_is_ours(d, req))",
	       "it sits ABOVE RULE C, deliberately, like the directory role");
	/*
	 * THE SOLE-DIRECTORY GATE THAT STOOD HERE IS GONE (rd vms-b5b0), and the
	 * AUTHORITY it stood in for is pinned instead. The door used to be shut
	 * unless this node was the cluster's sole lock-directory node; that was
	 * standing in for "the routing decision is sound", which it had to be
	 * while the vector could not be consulted per resource. It can now, and
	 * the authority for every op in this door is an EXECUTIVE FACT about a
	 * lock -- the LKB's own req_csid tag, which refuses a release or a
	 * convert of a lock held for anybody else (driven against the real engine
	 * in test_dlm_recv_arm.c, "a peer may not release another node's lock",
	 * and in the mixed flows in test_dlm_mixed_master.c).
	 */
	absent("dlm_arm_sole_directory",
	      "*** the SOLE-DIRECTORY configuration gate is GONE from this arm "
	      "(rd vms-b5b0): the authority is the LKB's own req_csid tag, not a "
	      "LOCKDIRWT arrangement ***");
	absent("vms_ldwv_sole_directory",
	      "... and the vector predicate it read is not called here either");
	has("d->mixed_served++",
	    "a request from a real VMS system served as MASTER is counted");
	has("d->mixed_replies_taken++",
	    "... and so is an answer from one, matched to a request of ours");
	has("VMS_WIRE_RESPONSE_BIT) &&\n\t    !req->peer_is_ours) {",
	    "an unproven system's REPLY is taken in any configuration -- it has "
	    "to be, since an OVMX $ENQ can now be addressed at its directory -- "
	    "and the FSM matches it to a request of OURS by a handle this "
	    "executive minted");

	/* The $DEQ's fields, and where each one comes from (RULE B). */
	has("vms_dlm_deq_parse_body(in->body, in->len, &q)",
	    "the release is read by the SHIPPING codec, never by hand -- which "
	    "is also what refuses a zero lock id in either field (fc8540ae)");
	has("out->req_csid = (uint32_t)in->from_csid;",
	    "WHO IS RELEASING comes from the connection manager's own "
	    "identification of the sender, never from the body");
	has("out->master_lkid = q->master_lkid;",
	    "body[24:28] -- OUR handle -- is what names the LKB to release");
	has("out->req_lkid = q->req_lkid;",
	    "body[20:24] is the releaser's own handle");
	has("out->op = VMS_DLM_MREQ_DEQ;",
	    "... and the request is typed a RELEASE, with no resource name "
	    "composed for it: a real op-0x03 carries none, and the engine "
	    "identifies the lock by handle");
	has("vms_lock_dlm_master_serve(&mr, &res)",
	    "the parsed release reaches the ENGINE's master-side door");
	has("d->releases_received++",
	    "a release the engine really performed is counted");
	has("d->releases_refused++",
	    "... and one naming no lock of ours is counted as a refusal, "
	    "never silently treated as a success");
	has("d->deferred_grants_no_wire_op++",
	    "a release that FLIPPED a queued waiter counts the grant this "
	    "master now owes it");

	/* The BLKAST's receive path. */
	has("dlm_req_fsm_blkast_body(&d->req, in->from_csid, in->body,",
	    "the BLKAST body goes to the requester FSM's own entry, which "
	    "parses it and finds the proxy by the handle THIS node minted");

	/* The departure path reaches the engine as a direct call. */
	has("vms_lock_dlm_member_departed((uint32_t)csid, &found)",
	    "a departure sweeps the engine's orphaned lock state directly");
	has("dlm_req_fsm_peer_gone(&d->req, csid)",
	    "... and fails every request outstanding at the departed member");
}

/*
 * THE DIAGNOSTIC ROW'S TWO HAND-MAINTAINED COPIES (rd vms-025).
 *
 * `struct vms_dlm_scs_view_wire` is declared TWICE by hand -- once per rind --
 * because each ioctl header must stay includable with no kernel-core dependency.
 * The Linux copy's agreement with kernel-core is pinned by a _Static_assert in
 * vms_devtab.c; the NetBSD-vax copy's is pinned by NOTHING the Linux build
 * compiles, so a field added to one and not the other is an arch-asymmetric
 * break an x86_64-only proof cannot see.
 *
 * rd vms-025 took the row's `pad0` byte for `directory_vector_own`, so both
 * copies are scanned for it in the same slot (after `connected`, before the
 * 32-bit generation).
 */
static void the_diag_row_agrees_across_both_rinds(void)
{
	printf("-- the DLM diagnostic row, in BOTH rinds' ioctl headers --\n");

	if (read_src(OVMX_KLINUX_DIR, "vms_ioctl.h") != 0) {
		ct_check(0, "could not open src/kernel/vms_ioctl.h");
		return;
	}
	has("    uint8_t  connected;\n    uint8_t  directory_vector_own;",
	    "the Linux rind's row carries directory_vector_own in the former "
	    "pad byte, right after connected");
	absent("    uint8_t  connected;\n    uint8_t  pad0;",
	       "... and no pad byte is left there to drift");

	if (read_src(OVMX_KNETBSD_DIR, "vms_lock_nb.h") != 0) {
		ct_check(0, "could not open src/kernel-netbsd/vms_lock_nb.h");
		return;
	}
	has("\tuint8_t  connected;\n\tuint8_t  directory_vector_own;",
	    "*** and the NetBSD-vax rind's row carries it in the SAME slot -- "
	    "the arch-asymmetric break an x86_64-only proof cannot see ***");
	absent("\tuint8_t  connected;\n\tuint8_t  pad0;",
	       "... with no pad byte left there either");
}

static void cnxman_legs(void)
{
	printf("-- the two legs, read out of src/kernel-core/vms_cnxman.c --\n");
	if (read_src(OVMX_KCORE_DIR, "vms_cnxman.c") != 0) {
		ct_check(0, "could not open vms_cnxman.c");
		return;
	}

	has("int cnxman_dlm_send(struct vms_cluster *cl, vms_csid_t dst_csid,",
	    "the DLM's origination entry exists, addressed by CSID");
	has("if (!cnxman_dlm_peer_proven(cn, csb, body, len))",
	    "RULE C's EMISSION half: every origination is gated, per DESTINATION "
	    "and per FRAME");
	has("vms_dlm_shape_fit_for_any_member(body, len)",
	    "... and the per-FRAME half asks the CODEC which shapes may face an "
	    "unproven member (rd vms-b5b0) -- no byte of a body is read outside "
	    "the one TU that owns wire offsets");
	absent("vms_ldwv_sole_directory(&cl->club.ldwv)",
	    "*** and the CONFIGURATION test that used to let frames past is "
	    "GONE: a shape is fit or it is not, in any LOCKDIRWT arrangement "
	    "***");
	has("cn->dlm_foreign_refused++", "... and that refusal is counted");
	/* ORDER, because this is the gate under BOTH new emits (rd vms-d7a3):
	 * the release the requester arm originates and the BLKAST the master
	 * arm originates both arrive here, and both must be refused before a
	 * byte reaches the port. */
	before("if (!cnxman_dlm_peer_proven(cn, csb, body, len))",
	       "scs_send_msg(cl->scs, csb->cdt_conid, cn->dlm_tx",
	       "*** RULE C is evaluated BEFORE a byte reaches the port -- the "
	       "one gate under every DLM origination, old and new ***");
	has("cnxman_envelope_originate(csb, cn->dlm_tx, CNXMAN_ENV_REQUEST)",
	    "an origination stamps a fresh transaction on the peer's dialogue");
	has("cnxman_envelope_originate(csb, cn->dlm_tx, CNXMAN_ENV_RESPONSE)",
	    "a reply stamps a RESPONSE envelope, echoing the request's txn");
	has("vms_cm_body_build(body, len, cn->dlm_reply, reply.len",
	    "a reply is wrapped by the codec, which echoes the request's "
	    "txn/token -- the DLM never writes body[0:8]");
	has("req.peer_is_ours = (uint8_t)(csb != NULL ? csb->peer_is_ours : 0u)",
	    "the trust fact is READ OFF THE CSB and handed over, not re-derived");

	/* The ORDER of the receive legs. */
	before("cnxman_coord_rx_body(&cn->coord",
	       "cnxman_dlm_rx(cn, &env, body, len",
	       "the DLM receive leg runs AFTER the join/barrier/coordinator, so "
	       "the barrier keeps the op-0x0d record it owns in a transition");
	before("cnxman_dlm_rx(cn, &env, body, len",
	       "cn->frames_unrouted++",
	       "... and BEFORE the unrouted counter, so DLM traffic is not "
	       "reported as an ungrounded frame");
}

static void cluster_start_order(void)
{
	printf("-- CLUSTER_START's order, read out of vms_devtab.c --\n");
	if (read_src(OVMX_KCORE_DIR, "vms_devtab.c") != 0) {
		ct_check(0, "could not open vms_devtab.c");
		return;
	}

	has("vms_lock_dlm_set_delivery_proc(proc)",
	    "CLUSTER_START registers its OWN caller as the delivery proc");
	before("vms_cnxman_start(cl)", "vms_dlm_scs_start(cl)",
	       "the arm starts AFTER the connection manager it registers with");
	before("vms_lock_dlm_set_delivery_proc(proc)", "vms_dlm_scs_start(cl)",
	       "vms-c27 cond.4: the owner is registered BEFORE the arm is up, "
	       "so no inbound request finds the arm running and no owner");
}

int main(void)
{
	printf("=== test_dlm_scs_arm (the DLM wire arm's R1) ===\n");
	trust_anchor_is_the_advertised_version();
	arm_bindings();
	the_diag_row_agrees_across_both_rinds();
	cnxman_legs();
	cluster_start_order();
	return ct_summary("test_dlm_scs_arm");
}
