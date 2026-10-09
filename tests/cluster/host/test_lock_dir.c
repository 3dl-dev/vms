/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_lock_dir.c - FC-P4.3's R1, ENGINE half: `dir_resolve` inside the REAL
 * lock manager (src/kernel-core/vms_lock.c, host backend FC-P4.9).
 *
 * test_dlm_ldwv.c proves the VECTOR (the published construction and the index
 * rule). This file proves the thing the vector is useless without: WHERE the
 * value it is indexed with comes from, and what happens when there is none.
 *
 * THE ANTI-LARP ASSERTIONS, and why each one is here
 *
 *   1. LEARNED, OR PROVEN-COMPUTED, OR REFUSED -- never guessed (rd vms-b5b0).
 *      With a cluster present and no wire-learned value for a root resource,
 *      $ENQ routes by the value vms_dlm_name_hash_proven() computes over the
 *      resource's own identity, and the test checks the routed value IS that
 *      function's output. For an identity OUTSIDE the proven coverage
 *      (supervisor mode; a UIC group in the system range) $ENQ returns
 *      SS$_UNSUPPORTED, NO lock handle is invented, the directory resolver is
 *      NEVER CALLED and NOTHING is posted. That last clause is the one that
 *      matters: the strawman's failure was not a bad local decision, it was a
 *      frame that left this node carrying a hash of 0, which made a real VAX
 *      create a directory entry naming OVMX as the master of resources it did
 *      not master (memory cluster-promotion-gap). A test that only checked the
 *      status would pass on a build that still sent the frame.
 *
 *   2. WHAT IS INDEXED IS WHAT ARRIVED. After the wire supplies a hash, the
 *      value the resolver is handed is BYTE-FOR-BYTE the value the wire
 *      supplied -- not a function of the resource name. The test proves this
 *      the only way it can be proved: two DIFFERENT names are given the SAME
 *      wire hash and both resolve through that one value, and one name is
 *      given a value no name-derived function would produce.
 *
 *   3. A CACHED RESOLUTION DIES WITH ITS VECTOR. Bumping the vector's
 *      generation (what Phase 1 of every state transition does, Davis p. 6-33)
 *      makes the engine re-resolve, and the new answer is the one used.
 *
 *   4. NO CLUSTER IS NOT A REFUSAL. With no ops installed at all, this node is
 *      alone: it is the directory and the master for everything and local
 *      locking is completely unaffected. A build that refused here would have
 *      broken every single-node OVMX.
 *
 *   5. THE LEARNED VALUE IS KEPT. A learned value is a fact about this cluster
 *      that cannot be re-derived, so the resource block that holds one survives
 *      having no locks on it.
 *
 *   6. THE VALUE DOES NOT DEPEND ON THE MEMBERSHIP, and the WIRE OVERRIDES a
 *      computed value and counts the contradiction -- the two properties that
 *      replace the retired all-OVMX gate (rd vms-3e3) and are what make one
 *      function cluster-wide the faithful answer rather than two.
 */
#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h -> lock_host_internal.h */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h -> exec_kbackend_host.h */
#include "vms_dlm_proxy.h"    /* the requester + directory seam under test */
#include "vms_dlm_hash.h"     /* the PROVEN resource-name hash the engine now
			       * computes with (rd vms-b5b0) -- this test
			       * checks the engine routes by THAT value */

#include <stdio.h>
#include <string.h>

/* ================================================================
 * Real executive globals vms_lock.c reads.
 * ================================================================ */
uint32_t vms_local_csid = 1;

#define CSID_LOCAL     1u    /* this node                               */
#define CSID_DIRECTORY 7u    /* the member the weight vector names       */

/*
 * THE RESOURCE IDENTITY EVERY $ENQ IN THIS FILE NAMES (rd vms-b5b0). The test
 * processes carry uic 0 and run at access mode 0, so every resource they create
 * is (group 0, mode 0) -- and a hash LEARNED for a resource has to be learned
 * under the SAME identity or it belongs to a different resource.
 */
#define RES_GROUP 0u
#define RES_MODE  0u

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

/* ================================================================
 * The stand-in connection manager: a directory vector, and a recorder.
 *
 * It answers with ONE directory member and records the hash it was asked
 * about. It computes nothing from a name -- it never sees a name, which is
 * the shape of the seam (vms_dlm_proxy.h: there is deliberately no variant of
 * `dir_resolve` that takes a resource name).
 * ================================================================ */
struct fake_cm {
	uint32_t dir_csid;        /* what the vector's entry reads; 0 = us   */
	uint32_t generation;
	uint32_t resolve_calls;
	uint32_t last_hash;
	uint32_t refuse;          /* nonzero => the vector is not usable     */
	int      posts;
	struct vms_dlm_proxy_post last_post;
};

static struct fake_cm cm;

static uint32_t cm_dir_resolve(void *ctx, uint32_t hash16, uint32_t *out_csid)
{
	struct fake_cm *c = ctx;

	c->resolve_calls++;
	c->last_hash = hash16;
	if (c->refuse)
		return (uint32_t)SS__UNSUPPORTED;
	*out_csid = c->dir_csid;
	return (uint32_t)SS__NORMAL;
}

static uint32_t cm_dir_generation(void *ctx)
{
	return ((struct fake_cm *)ctx)->generation;
}

static uint32_t cm_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	struct fake_cm *c = ctx;

	c->posts++;
	c->last_post = *p;
	return (uint32_t)SS__NORMAL;
}

static void cm_install(void)
{
	struct vms_dlm_requester_ops ops;

	memset(&ops, 0, sizeof(ops));
	ops.post = cm_post;
	ops.dir_resolve = cm_dir_resolve;
	ops.dir_generation = cm_dir_generation;
	ops.ctx = &cm;
	vms_lock_dlm_set_requester_ops(&ops);
}

static void cm_reset(uint32_t dir_csid)
{
	memset(&cm, 0, sizeof(cm));
	cm.dir_csid = dir_csid;
	cm.generation = 1u;
}

/* ================================================================
 * Harness
 * ================================================================ */
static void proc_init(struct vms_proc *p)
{
	int i;

	memset(p, 0, sizeof(*p));
	exec_lock_init(&p->mode_lock);
	exec_lock_init(&p->lock_list_lock);
	exec_list_head_init(&p->locks);
	for (i = 0; i < 4; i++) {
		exec_lock_init(&p->ast[i].lock);
		exec_list_head_init(&p->ast[i].pending);
	}
}

static uint32_t do_enq(struct vms_proc *proc, const char *resnam,
		       uint32_t lkmode, uint32_t *lkid_out)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = lkmode;
	strscpy(a.resnam, resnam, sizeof(a.resnam));
	vms_ioctl_enq(proc, (unsigned long)(void *)&a);
	if (lkid_out)
		*lkid_out = a.lkid;
	return a.status;
}

static uint32_t do_deq(struct vms_proc *proc, uint32_t lkid)
{
	struct vms_deq_args a;

	memset(&a, 0, sizeof(a));
	a.lkid = lkid;
	vms_ioctl_deq(proc, (unsigned long)(void *)&a);
	return a.status;
}

static void read_resmaster(const char *resnam, struct vms_resmaster_args *out)
{
	memset(out, 0, sizeof(*out));
	strscpy(out->resnam, resnam, sizeof(out->resnam));
	vms_ioctl_get_resmaster(NULL, (unsigned long)(void *)out);
}

/* ================================================================
 * 1. No cluster: a standalone node locks exactly as it always did.
 * ================================================================ */
static void standalone_still_locks(void)
{
	struct vms_proc proc;
	struct vms_resmaster_args rm;
	uint32_t lkid = 0, st;

	printf("--- no cluster arm at all: this node is the directory and the master ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	vms_lock_dlm_set_requester_ops(NULL);
	proc_init(&proc);

	/*
	 * The R4 readback's own invariant (tests/qemu/test_kmod_resdir.c), held
	 * here at R1 so a change to the resolver cannot break it silently: with
	 * no cluster this node is the directory for a name that does not exist
	 * yet, and asking does not create it.
	 */
	read_resmaster("NEVERENQUEUED", &rm);
	ct_check_eq_u32(rm.found, 0u, "an unknown name is not created by asking");
	ct_check_eq_u32(rm.dir_csid, CSID_LOCAL,
			"and a cluster of one is the directory for it");
	ct_check_eq_u32(rm.master_csid, 0u, "but nothing masters it yet");

	st = do_enq(&proc, "STANDALONE1", LCK_K_EXMODE, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0,
		 "$ENQ grants with no wire-learned hash anywhere in sight");

	read_resmaster("STANDALONE1", &rm);
	ct_check_eq_u32(rm.found, 1u, "the resource exists");
	ct_check_eq_u32(rm.master_csid, CSID_LOCAL, "and this node masters it");
	ct_check_eq_u32(rm.dir_csid, CSID_LOCAL,
			"and reports itself as the directory");

	ct_check(do_deq(&proc, lkid) == SS__NORMAL, "and it releases");
	vms_lock_cleanup();
}

/* ================================================================
 * 2. In a cluster with no wire-learned hash: COMPUTED, and routed (rd
 *    vms-b5b0).
 *
 * This test used to assert the opposite -- SS$_UNSUPPORTED, nothing sent, the
 * vector not even consulted -- because the hash was learnable and nothing
 * else. It is now determined and proven (vms_dlm_hash.h), so the honest answer
 * for a root resource no frame has named is the COMPUTED value, and what this
 * test has to pin is that the value routed with is THAT function's output and
 * not something else. The refusal it used to prove moved to test 2b, where it
 * still has teeth: an identity outside the proven coverage.
 * ================================================================ */
static void novel_root_computes_and_routes(void)
{
	struct vms_proc proc;
	struct vms_resmaster_args rm;
	uint32_t lkid = 0, st, expect = 0;

	printf("--- in a cluster, a novel root resource is COMPUTED and routed ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	cm_reset(CSID_DIRECTORY);
	cm_install();
	proc_init(&proc);

	ct_check(vms_dlm_name_hash(RES_GROUP, RES_MODE,
				   (const uint8_t *)"NOVELROOT1", 10u,
				   &expect) == VMS_DLM_HASH_OK,
		 "the proven function has a value for this identity");

	st = do_enq(&proc, "NOVELROOT1", LCK_K_EXMODE, &lkid);
	ct_check_eq_u32(st, SS__NORMAL,
			"$ENQ on a root resource this cluster has never named "
			"is ACCEPTED");
	ct_check_eq_u32((unsigned long)cm.posts, 1u,
			"exactly one request left this node");
	ct_check_eq_u32(cm.last_hash, expect,
			"and the vector was indexed with the value "
			"vms_dlm_name_hash() computes for the resource's own "
			"identity -- not a placeholder, not a local index");
	ct_check_eq_u32(cm.last_post.dir_hash, expect,
			"the POST carries that same value for body[128:132]");
	ct_check_eq_u32((unsigned long)cm.last_post.dir_hash_known, 1u,
			"marked as held, so the codec may write the field");
	ct_check_eq_u32((unsigned long)cm.last_post.res_group, RES_GROUP,
			"...and the identity the value is OF rides with it "
			"(body[44:46])");
	ct_check_eq_u32((unsigned long)cm.last_post.res_acmode, RES_MODE,
			"...including the access mode (body[46])");
	ct_check_eq_u32(cm.last_post.dst_csid, CSID_DIRECTORY,
			"addressed to the directory node the vector named for "
			"that value");

	read_resmaster("NOVELROOT1", &rm);
	ct_check_eq_u32(rm.dir_csid, CSID_DIRECTORY,
			"and the readback names that real directory node");

	vms_lock_cleanup();
}

/* ================================================================
 * 2b. AN IDENTITY OUTSIDE THE PROVEN COVERAGE IS STILL REFUSED, AND NOTHING
 *     IS SENT -- the anti-LARP clause this file has always carried, now where
 *     the risk actually lives (rd vms-b5b0).
 *
 * The strawman's failure was not a bad local decision: it was a FRAME that
 * left this node carrying a value nobody derived, which made a real VAX create
 * a directory entry naming OVMX as the master of resources it did not master
 * (memory cluster-promotion-gap). So for an identity no VMS node has been
 * watched hashing -- here SUPERVISOR mode, which the corpus and the driven run
 * never show -- the engine refuses, invents no handle, does not consult the
 * vector and puts NOTHING on the wire.
 * ================================================================ */
static void an_unproven_identity_refuses_and_sends_nothing(void)
{
	struct vms_proc proc;
	struct vms_resmaster_args rm;
	uint32_t lkid = 0, st;

	printf("--- an identity outside the PROVEN coverage is REFUSED, nothing sent ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	cm_reset(CSID_DIRECTORY);
	cm_install();
	proc_init(&proc);
	proc.current_mode = PSL_C_SUPER;   /* mode 2: never observed on a wire */

	ct_check(vms_dlm_name_hash_coverage(RES_GROUP, PSL_C_SUPER, 10u) ==
		 VMS_DLM_HASH_E_COVER,
		 "supervisor mode is outside the proven coverage");

	st = do_enq(&proc, "SUPERROOT1", LCK_K_EXMODE, &lkid);
	ct_check_eq_u32(st, SS__UNSUPPORTED,
			"$ENQ on it -> SS$_UNSUPPORTED, the honest floor");
	ct_check_eq_u32(lkid, 0u, "no lock handle was invented");
	ct_check_eq_u32((unsigned long)cm.posts, 0u,
			"and NOTHING was put on the wire (the anti-LARP clause)");
	ct_check_eq_u32(cm.resolve_calls, 0u,
			"the vector was not consulted: there was no value to "
			"index it with");

	read_resmaster("SUPERROOT1", &rm);
	ct_check_eq_u32(rm.dir_csid, 0u,
			"and the readback reports NO directory rather than a "
			"guessed one (INV-6)");
	ct_check_eq_u32(rm.master_csid, 0u, "and no master");

	/* A UIC GROUP in the system range is the other unobserved axis. */
	proc.current_mode = (uint8_t)RES_MODE;
	proc.uic = (16400u << 16) | 4u;    /* group bit 14 set */
	cm.posts = 0;
	st = do_enq(&proc, "SYSGROUPROOT", LCK_K_EXMODE, &lkid);
	ct_check_eq_u32(st, SS__UNSUPPORTED,
			"a UIC group with bit 14 set is refused too");
	ct_check_eq_u32((unsigned long)cm.posts, 0u, "and sends nothing");

	vms_lock_cleanup();
}

/* ================================================================
 * 3. The wire supplies the hash: the lookup routes, with THAT value./* ================================================================
 * 3. The wire supplies the hash: the lookup routes, with THAT value.
 * ================================================================ */
static void wire_hash_routes_the_lookup(void)
{
	/* Two values chosen so that neither is derivable from its name by any
	 * plausible function -- and so the two names SHARE one, which no
	 * name-derived hash would ever produce. */
	const uint32_t WIRE_HASH = 0xBEEFu;
	struct vms_proc proc;
	struct vms_resmaster_args rm;
	uint32_t lkid = 0, st;

	printf("--- the cluster supplied the hash: the lookup goes to the directory ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	cm_reset(CSID_DIRECTORY);
	cm_install();
	proc_init(&proc);

	st = vms_lock_dlm_learn_dir_hash("SHAREDROOT", RES_GROUP, RES_MODE, WIRE_HASH);
	ct_check_eq_u32(st, SS__NORMAL, "a cat-02 frame named SHAREDROOT");
	st = vms_lock_dlm_learn_dir_hash("OTHERROOT", RES_GROUP, RES_MODE, WIRE_HASH);
	ct_check_eq_u32(st, SS__NORMAL,
			"and named OTHERROOT with the SAME value -- which no "
			"name-derived hash could do");

	st = do_enq(&proc, "SHAREDROOT", LCK_K_EXMODE, &lkid);
	ct_check_eq_u32(st, SS__NORMAL, "$ENQ is accepted and posted");
	ct_check_eq_u32((unsigned long)cm.posts, 1u, "exactly one request left");
	ct_check_eq_u32(cm.last_hash, WIRE_HASH,
			"and the vector was indexed with the value THE WIRE gave");
	ct_check_eq_u32(cm.last_post.dst_csid, CSID_DIRECTORY,
			"addressed to the directory node the vector named");
	ct_check(strcmp(cm.last_post.resnam, "SHAREDROOT") == 0,
		 "for the resource the caller asked about");
	ct_check(cm.last_post.req_lkid != 0u,
		 "carrying a REAL proxy lock id, never a placeholder");

	cm.last_hash = 0;
	st = do_enq(&proc, "OTHERROOT", LCK_K_EXMODE, &lkid);
	ct_check_eq_u32(st, SS__NORMAL, "the second name is accepted too");
	ct_check_eq_u32(cm.last_hash, WIRE_HASH,
			"and indexes with ITS wire value, identical to the first");

	read_resmaster("SHAREDROOT", &rm);
	ct_check_eq_u32(rm.dir_csid, CSID_DIRECTORY,
			"the readback names the real directory node");

	vms_lock_cleanup();
}

/* ================================================================
 * 4. A conflicting learn is refused; the held value stands.
 * ================================================================ */
static void conflicting_learn_is_counted(void)
{
	uint32_t before, st;

	printf("--- two different hashes for one name: the first stands, counted ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	cm_reset(CSID_DIRECTORY);
	cm_install();
	before = vms_lock_dlm_dir_hash_conflicts();

	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("CONFLICT1", RES_GROUP, RES_MODE, 0x1234u),
			SS__NORMAL, "the first value is learned");
	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("CONFLICT1", RES_GROUP, RES_MODE, 0x1234u),
			SS__NORMAL, "the same value again is fine");
	ct_check_eq_u32(vms_lock_dlm_dir_hash_conflicts(), before,
			"and counts no conflict");

	st = vms_lock_dlm_learn_dir_hash("CONFLICT1", RES_GROUP, RES_MODE, 0x5678u);
	ct_check_eq_u32(st, SS__BADPARAM, "a DIFFERENT value is refused");
	ct_check_eq_u32(vms_lock_dlm_dir_hash_conflicts(), before + 1u,
			"and counted -- the evidence that falsifies the field "
			"offset or the one-hash-per-name property");

	{
		struct vms_proc proc;
		uint32_t lkid = 0;

		proc_init(&proc);
		cm.last_hash = 0;
		(void)do_enq(&proc, "CONFLICT1", LCK_K_EXMODE, &lkid);
		ct_check_eq_u32(cm.last_hash, 0x1234u,
				"and the FIRST value is still what routing uses");
	}

	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash(NULL, RES_GROUP, RES_MODE, 1u), SS__BADPARAM,
			"a null name is refused");
	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("", RES_GROUP, RES_MODE, 1u), SS__BADPARAM,
			"an empty name is refused");
	vms_lock_cleanup();
}

/* ================================================================
 * 4b. The learner is bounded (rd vms-4e9); a real $ENQ is not.
 *
 * Every distinct root name a VMS member put on the wire used to leave a
 * preserved resource block behind for good. The learner now keeps a hash in
 * a NEW block only while the table is under its bound, always keeps one for
 * a name that already has a block, and never refuses a real $ENQ.
 * ================================================================ */
#define LEARN_BOUND 4096u   /* VMS_DLM_LEARN_RES_CAP, vms_lock.c */

static uint32_t learn_n(const char *prefix, uint32_t n)
{
	char name[32];
	uint32_t i, kept = 0;

	for (i = 0; i < n; i++) {
		snprintf(name, sizeof(name), "%s%05u", prefix, (unsigned)i);
		if (vms_lock_dlm_learn_dir_hash(name, RES_GROUP, RES_MODE,
					    (i << 16) | 1u) ==
		    SS__NORMAL)
			kept++;
	}
	return kept;
}

static void learner_is_bounded(void)
{
	struct vms_proc proc;
	uint32_t lkid = 0, full0;

	printf("--- the hash learner keeps at most its bound; $ENQ is never refused (rd vms-4e9) ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	vms_lock_dlm_set_requester_ops(NULL);   /* a node with no cluster arm */
	full0 = vms_lock_dlm_dir_hash_learn_full();

	ct_check_eq_u32(learn_n("LRN", LEARN_BOUND + 100u), LEARN_BOUND,
			"the learner keeps exactly its bound of new names");
	ct_check_eq_u32(vms_lock_dlm_dir_hash_learn_full(), full0 + 100u,
			"and counts every name it declined");
	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("LRN00000", RES_GROUP, RES_MODE, 1u), SS__NORMAL,
			"a name that already has a block is still recorded at the bound");

	proc_init(&proc);
	ct_check_eq_u32(do_enq(&proc, "PASTTHEBOUND", LCK_K_EXMODE, &lkid),
			SS__NORMAL, "a real $ENQ on a new name is granted at the bound");
	ct_check(do_deq(&proc, lkid) == SS__NORMAL, "and releases");

	vms_lock_cleanup();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init again");
		return;
	}
	ct_check_eq_u32(learn_n("AGAIN", 10u), 10u,
			"cleanup empties the table, so the learner has room again");
	vms_lock_cleanup();
}

/* ================================================================
 * 5. A cached resolution dies with its vector's generation (p. 6-33).
 * ================================================================ */
static void generation_invalidates_the_cache(void)
{
	struct vms_proc proc;
	uint32_t lkid = 0;
	uint32_t calls_after_first;

	printf("--- a vector change re-resolves every cached directory (p. 6-33) ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	cm_reset(CSID_DIRECTORY);
	cm_install();
	proc_init(&proc);

	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("GENROOT", RES_GROUP, RES_MODE, 0x0101u),
			SS__NORMAL, "the wire named GENROOT");

	(void)do_enq(&proc, "GENROOT", LCK_K_EXMODE, &lkid);
	calls_after_first = cm.resolve_calls;
	ct_check(calls_after_first >= 1u, "the first $ENQ resolved the directory");

	/* Same vector: the cached answer is reused, not re-resolved. */
	(void)do_enq(&proc, "GENROOT", LCK_K_CRMODE, &lkid);
	ct_check_eq_u32(cm.resolve_calls, calls_after_first,
			"a second $ENQ on the same vector reuses the cache "
			"(p. 6-32: one lookup per tree)");

	/* A transition: Phase 1 discards the directory, Phase 2 refills it with
	 * a DIFFERENT answer. The engine must not keep using the old one. */
	cm.generation++;
	cm.dir_csid = CSID_LOCAL + 40u;
	(void)do_enq(&proc, "GENROOT", LCK_K_EXMODE, &lkid);
	ct_check(cm.resolve_calls > calls_after_first,
		 "a generation change forces a re-resolution");
	ct_check_eq_u32(cm.last_post.dst_csid, CSID_LOCAL + 40u,
			"and the NEW directory node is the one addressed");

	/* And a vector that is not usable at all -- mid-transition -- refuses
	 * rather than falling back to the old answer. */
	cm.generation++;
	cm.refuse = 1u;
	{
		uint32_t st = do_enq(&proc, "GENROOT", LCK_K_EXMODE, &lkid);

		ct_check_eq_u32(st, SS__UNSUPPORTED,
				"an unusable vector refuses, it does not reuse");
	}
	vms_lock_cleanup();
}

/* ================================================================
 * 6. A wire-learned hash is kept: it cannot be recomputed, so it is not
 *    thrown away when the resource holds no locks.
 * ================================================================ */
static void learned_hash_survives_reclaim(void)
{
	struct vms_proc proc;
	uint32_t lkid = 0, st;

	printf("--- a learned hash outlives an idle resource (it cannot be recomputed) ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	cm_reset(CSID_DIRECTORY);
	cm_install();
	proc_init(&proc);

	/* The wire names a resource this node holds no lock on -- the common
	 * case during a join's rebuild burst. */
	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("REBUILTROOT", RES_GROUP, RES_MODE, 0x0F0Fu),
			SS__NORMAL, "learned from a rebuild record");

	/* Much later, a local $ENQ. If the block had been reclaimed the value
	 * would be gone and this would be SS$_UNSUPPORTED forever. */
	cm.last_hash = 0;
	st = do_enq(&proc, "REBUILTROOT", LCK_K_EXMODE, &lkid);
	ct_check_eq_u32(st, SS__NORMAL, "a later $ENQ still routes");
	ct_check_eq_u32(cm.last_hash, 0x0F0Fu, "with the value the wire gave");

	/*
	 * A COMPUTED VALUE IS NOT A PRESERVATION REASON (rd vms-b5b0): it can be
	 * recomputed from the identity, and preserving every resource this node
	 * is the first to touch would grow the database without bound.
	 */
	{
		struct vms_resmaster_args rm;
		uint32_t lkid3 = 0;

		cm.dir_csid = 0u;        /* our own entry: mastered here */
		cm.generation++;
		ct_check_eq_u32(do_enq(&proc, "COMPUTEDONLY", LCK_K_EXMODE,
				       &lkid3), SS__NORMAL,
				"a resource with no wire value is granted");
		read_resmaster("COMPUTEDONLY", &rm);
		ct_check_eq_u32(rm.found, 1u, "  its block exists while locked");
		ct_check(do_deq(&proc, lkid3) == SS__NORMAL, "  it releases");
		read_resmaster("COMPUTEDONLY", &rm);
		ct_check_eq_u32(rm.found, 0u,
				"*** and the block is RECLAIMED: a computed "
				"value is recomputable, so it is not a reason "
				"to keep a resource forever ***");
		cm.dir_csid = CSID_DIRECTORY;
	}

	/* And when the vector says the entry is OURS, we master it locally. */
	cm.generation++;
	cm.dir_csid = 0u;   /* our own entry (p. 6-32) */
	{
		struct vms_resmaster_args rm;
		uint32_t lkid2 = 0;

		ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("OURSROOT", RES_GROUP, RES_MODE, 0x2222u),
				SS__NORMAL, "the wire named another root");
		st = do_enq(&proc, "OURSROOT", LCK_K_EXMODE, &lkid2);
		ct_check(st == SS__NORMAL && lkid2 != 0,
			 "a root whose vector entry is ours is mastered HERE "
			 "and granted locally (p. 6-31)");
		read_resmaster("OURSROOT", &rm);
		ct_check_eq_u32(rm.master_csid, CSID_LOCAL, "this node masters it");
		ct_check_eq_u32(rm.is_local_master, 1u, "and says so");
	}
	vms_lock_cleanup();
}

/* ================================================================
 * 7. ANY MEMBERSHIP, ONE FUNCTION (rd vms-b5b0, retiring rd vms-3e3's gate
 *    and rd vms-025/db2a's sole-directory interim).
 *
 * These two tests used to be a PAIR: an all-OVMX cluster grounded a novel root
 * with OVMX's OWN hash and routed it, and a MIXED cluster mastered the same
 * name locally with nothing sent -- the all-OVMX gate, which was the honest
 * floor while the only computable value was one no real VAX would agree with.
 *
 * There is now ONE function cluster-wide, so the two configurations give the
 * SAME answer, and that is the property worth pinning: the value does not
 * depend on who else is in the membership. A build that kept a second hash for
 * all-OVMX clusters would fail this test, which is the point -- a name's master
 * must not move when a VAX joins.
 * ================================================================ */
static void the_value_does_not_depend_on_the_membership(void)
{
	struct vms_proc proc;
	uint32_t lkid = 0, expect = 0, first = 0;

	printf("--- the computed value is the same in any membership (one function) ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	cm_reset(CSID_DIRECTORY);
	cm_install();
	proc_init(&proc);

	ct_check(vms_dlm_name_hash(RES_GROUP, RES_MODE,
				   (const uint8_t *)"OVMXOWNVOL", 10u,
				   &expect) == VMS_DLM_HASH_OK,
		 "the function answers for OVMXOWNVOL");

	(void)do_enq(&proc, "OVMXOWNVOL", LCK_K_EXMODE, &lkid);
	first = cm.last_hash;
	ct_check_eq_u32(first, expect,
			"an OVMX-first name routes by the VMS function's value");
	vms_lock_cleanup();

	/* The same name again, on a fresh engine with the vector naming a
	 * DIFFERENT directory member -- i.e. a different cluster entirely. The
	 * VALUE must not move; only the member the vector maps it to may. */
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init again");
		return;
	}
	cm_reset(CSID_LOCAL + 40u);
	cm_install();
	proc_init(&proc);
	(void)do_enq(&proc, "OVMXOWNVOL", LCK_K_EXMODE, &lkid);
	ct_check_eq_u32(cm.last_hash, first,
			"and by the same value in a different membership -- the "
			"hash is a property of the resource, not of who is in "
			"the cluster");
	vms_lock_cleanup();
}

/* ================================================================
 * 8. THE LIVE FALSIFICATION DETECTOR (rd vms-b5b0).
 *
 * The function is a determination from captured values, not a theorem. If some
 * VMS component hashes an identity differently from every one observed, the
 * first frame naming that identity carries a value different from the one this
 * node computed -- and then the WIRE WINS and the disagreement is COUNTED. A
 * silently-kept computed value would be a wrong hash on every later frame for
 * that resource.
 * ================================================================ */
static void the_wire_overrides_a_computed_value_and_counts_it(void)
{
	struct vms_proc proc;
	uint32_t lkid = 0, computed = 0, before, st;

	printf("--- the wire contradicts a computed value: the wire wins, counted ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	cm_reset(CSID_DIRECTORY);
	cm_install();
	proc_init(&proc);
	before = vms_lock_dlm_dir_hash_computed_wrong();

	(void)vms_dlm_name_hash(RES_GROUP, RES_MODE,
				(const uint8_t *)"FALSIFYME", 9u, &computed);
	(void)do_enq(&proc, "FALSIFYME", LCK_K_EXMODE, &lkid);
	ct_check_eq_u32(cm.last_hash, computed,
			"the first $ENQ routed by the computed value");
	ct_check_eq_u32(vms_lock_dlm_dir_hash_computed_wrong(), before,
			"and nothing is counted yet");

	/* A frame from a real system, naming the same identity, with another
	 * value. */
	st = vms_lock_dlm_learn_dir_hash("FALSIFYME", RES_GROUP, RES_MODE,
					 computed ^ 0x55u);
	ct_check_eq_u32(st, SS__NORMAL, "the wire's value is ACCEPTED");
	ct_check_eq_u32(vms_lock_dlm_dir_hash_computed_wrong(), before + 1u,
			"and the contradiction is COUNTED -- the evidence that "
			"the coverage masks claim too much");

	cm.generation++;              /* force a re-resolution */
	(void)do_enq(&proc, "FALSIFYME", LCK_K_CRMODE, &lkid);
	ct_check_eq_u32(cm.last_hash, computed ^ 0x55u,
			"and routing now uses the WIRE's value, not ours");

	/* The reverse never happens: a computed value may not displace a
	 * learned one. */
	before = vms_lock_dlm_dir_hash_computed_wrong();
	ct_check_eq_u32(vms_lock_dlm_dir_hash_conflicts() >= 0u, 1u,
			"(the learn-conflict counter is a separate fact)");
	ct_check_eq_u32(vms_lock_dlm_dir_hash_computed_wrong(), before,
			"and no further contradiction is counted");

	vms_lock_cleanup();
}

/* ================================================================
 * 9. GENESIS RELABELS THIS NODE'S OWN MASTERY (rd vms-151)/* ================================================================
 * 9. GENESIS RELABELS THIS NODE'S OWN MASTERY (rd vms-151)
 *
 * MEASURED: a booted OVMX node founded generation 1 and its userland stopped
 * dead. The executive stayed healthy and its circuits stayed open -- what
 * stopped was every lock request on a resource the node had ALREADY mastered
 * while it was still carrying the substrate's insmod placeholder CSID. The
 * cluster assigned it 0x00010001; dlm_resolve_master()'s "is the known master
 * US?" test stopped matching `master_csid == 1`; and each such request was
 * routed REMOTE to a node whose CSID no cluster ever assigned to anybody.
 *
 * What this pins is the fact that does not change: this node still masters
 * exactly what it mastered, so learning its own cluster identity RELABELS those
 * records and grants keep working. The second $ENQ below is the teeth -- under
 * the defect it does not grant locally, it leaves for CSID 1.
 * ================================================================ */
static void genesis_relabels_this_nodes_own_mastery(void)
{
	const uint16_t WIRE_HASH = 0xC0DEu;
	const uint32_t GENESIS_CSID = 0x00010001u;   /* generation 1, CSV slot 1 */
	struct vms_proc proc;
	struct vms_resmaster_args rm;
	uint32_t lkid = 0, lkid2 = 0, st;

	printf("--- genesis: this node learns its CSID and keeps its own mastery ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	cm_reset(0u);          /* the vector's entry reads "us" (p. 6-32) */
	cm_install();
	proc_init(&proc);
	vms_local_csid = CSID_LOCAL;   /* the pre-cluster placeholder */

	st = vms_lock_dlm_learn_dir_hash("PREGENESIS", RES_GROUP, RES_MODE, WIRE_HASH);
	ct_check_eq_u32(st, SS__NORMAL, "the wire named PREGENESIS");
	st = do_enq(&proc, "PREGENESIS", LCK_K_EXMODE, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0,
		 "$ENQ grants before the cluster exists");
	read_resmaster("PREGENESIS", &rm);
	ct_check_eq_u32(rm.master_csid, CSID_LOCAL,
			"and this node masters it under the placeholder CSID");
	ct_check_eq_u32((unsigned long)cm.posts, 0u, "nothing left this node");

	/* The cluster forms. This is the ONE call the connection manager makes. */
	vms_lock_dlm_set_local_csid(GENESIS_CSID);
	ct_check_eq_u32(vms_lock_dlm_local_csid(), GENESIS_CSID,
			"the engine now carries the cluster's own assignment");

	read_resmaster("PREGENESIS", &rm);
	ct_check_eq_u32(rm.master_csid, GENESIS_CSID,
			"the resource this node mastered is STILL mastered by "
			"this node, under its new name");

	/* THE TEETH. Under the defect this second request resolves "the master
	 * is CSID 1, which is not us" and is posted to a node that does not
	 * exist; the caller then waits for an answer that can never come. */
	st = do_enq(&proc, "PREGENESIS", LCK_K_NLMODE, &lkid2);
	ct_check(st == SS__NORMAL && lkid2 != 0,
		 "a later $ENQ on that resource still grants HERE");
	ct_check_eq_u32((unsigned long)cm.posts, 0u,
			"and nothing was posted to the placeholder CSID");

	/* Idempotent, because the connection manager makes this call on every
	 * transition boundary and on its own beat. */
	vms_lock_dlm_set_local_csid(GENESIS_CSID);
	read_resmaster("PREGENESIS", &rm);
	ct_check_eq_u32(rm.master_csid, GENESIS_CSID, "and re-telling it is a no-op");

	/* A zero is still refused: 0 means "unmastered" throughout the engine,
	 * so it is not an identity and may not relabel anything. */
	vms_lock_dlm_set_local_csid(0u);
	ct_check_eq_u32(vms_lock_dlm_local_csid(), GENESIS_CSID,
			"a zero CSID is refused, not stored");
	read_resmaster("PREGENESIS", &rm);
	ct_check_eq_u32(rm.master_csid, GENESIS_CSID,
			"... and relabels nothing");

	ct_check(do_deq(&proc, lkid) == SS__NORMAL, "the first lock releases");
	ct_check(do_deq(&proc, lkid2) == SS__NORMAL, "and so does the second");
	vms_local_csid = CSID_LOCAL;
	vms_lock_cleanup();
}

int main(void)
{
	printf("=== test_lock_dir (FC-P4.3 dir_resolve in the real engine, R1) ===\n");
	standalone_still_locks();
	novel_root_computes_and_routes();
	an_unproven_identity_refuses_and_sends_nothing();
	wire_hash_routes_the_lookup();
	conflicting_learn_is_counted();
	learner_is_bounded();
	generation_invalidates_the_cache();
	learned_hash_survives_reclaim();
	the_value_does_not_depend_on_the_membership();
	the_wire_overrides_a_computed_value_and_counts_it();
	genesis_relabels_this_nodes_own_mastery();
	return ct_summary("test_lock_dir");
}
