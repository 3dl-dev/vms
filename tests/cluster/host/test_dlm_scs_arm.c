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
 *          port (rd vms-d7a3: the master now ORIGINATES an op-0x04 blocking AST
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
	has("vms_dlm_enq_response_build_grant(r->req_lkid, r->master_lkid,",
	    "the GRANT reply is built from the master result's own handles");
	has("r->granted_mode", "... and from the mode read off the LKB");
	absent("VMS_DLM_LKID_UNSET, ",
	       "no builder is ever called with the unset-lock-id sentinel");

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

	/*
	 * THE HASH BOOTSTRAP DEADLOCK IS RESOLVED behind the all-OVMX gate (rung
	 * A", vms-3e3), pinned as a PROPERTY of the shipped file. The deadlock was:
	 * installing the resolver turns on vms_lock.c's "no wire-learned hash ->
	 * SS$_UNSUPPORTED" refusal, and no OVMX-only member can originate the first
	 * cat-0x02 frame to teach a hash. The resolution: an all-proven-OVMX cluster
	 * grounds the first hash with OVMX's OWN directory hash -- never DEC's, never
	 * toward a real VAX -- and a mixed/single configuration masters locally
	 * exactly as before (no interop regression). All four directory ops are now
	 * installed unconditionally; the gate that keeps grounding narrow is DYNAMIC.
	 */
	has("d->eng_ops.dir_resolve    = dlm_arm_eng_dir_resolve;",
	    "the engine's directory resolver IS installed (rung A\" resolved)");
	has("d->eng_ops.dir_groundable = dlm_arm_eng_dir_groundable;",
	    "... behind the DYNAMIC all-OVMX gate, not a one-shot start switch");
	has("d->eng_ops.dir_ground     = dlm_arm_eng_dir_ground;",
	    "... with the gated name->hash grounding op installed too");
	has("return vms_ldwv_all_ovmx(&d->cl->club.ldwv);",
	    "the gate reads the connection manager's own vector -- all members "
	    "proven-OVMX, dynamically");
	has("if (!dlm_arm_eng_dir_groundable(ctx))\n\t\treturn SS__UNSUPPORTED;",
	    "dir_ground REFUSES unless the all-OVMX gate holds -- never a name->hash "
	    "against a real VAX (the 90b3bbbd storm cannot recur)");
	has("*out_hash16 = vms_dlm_ovmx_dir_hash(name, name_len);",
	    "... and the grounded value is OVMX's OWN directory hash, not DEC's");
	has("h *= 16777619u;           /* FNV-1a prime */",
	    "OVMX's own hash is FNV-1a over the name bytes, folded to 16 -- clean-"
	    "room, deterministic, identical on every OVMX node");
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
	 * THE BLOCKING AST IS EMITTED (op 0x04, rd vms-d7a3), AND EVERY FIELD
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

	/* The requester FSM's own new-shape gate reads the SAME fact. */
	has("d->req_ops.all_ovmx        = dlm_arm_all_ovmx_op;",
	    "the requester arm's op-0x03 gate is bound to the same all-OVMX "
	    "fact (one function, no cached copy), so the release and the "
	    "BLKAST cannot disagree about who may be emitted at");

	/* The departure path reaches the engine as a direct call. */
	has("vms_lock_dlm_member_departed((uint32_t)csid, &found)",
	    "a departure sweeps the engine's orphaned lock state directly");
	has("dlm_req_fsm_peer_gone(&d->req, csid)",
	    "... and fails every request outstanding at the departed member");
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
	has("if (!cnxman_dlm_peer_proven(cn, csb))",
	    "RULE C's EMISSION half: nothing DLM leaves for an unproven system");
	has("cn->dlm_foreign_refused++", "... and that refusal is counted");
	/* ORDER, because this is the gate under BOTH new emits (rd vms-d7a3):
	 * the release the requester arm originates and the BLKAST the master
	 * arm originates both arrive here, and both must be refused before a
	 * byte reaches the port. */
	before("if (!cnxman_dlm_peer_proven(cn, csb))",
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
	cnxman_legs();
	cluster_start_order();
	return ct_summary("test_dlm_scs_arm");
}
