/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_lock_dir.c - FC-P4.3's R1, ENGINE half: `dir_resolve` inside the REAL
 * lock manager (src/kernel-core/vms_lock.c, host backend FC-P4.9).
 *
 * test_dlm_ldwv.c proves the VECTOR (the published construction and the index
 * rule). This file proves the thing the vector is useless without: that the
 * executive never produces the hash it indexes with.
 *
 * THE ANTI-LARP ASSERTIONS, and why each one is here
 *
 *   1. NEVER COMPUTED. With a cluster present and no wire-learned hash for a
 *      root name, $ENQ returns SS$_UNSUPPORTED, NO lock handle is invented,
 *      the directory resolver is NEVER CALLED, and NOTHING is posted. That
 *      last clause is the one that matters: the strawman's failure was not a
 *      bad local decision, it was a frame that left this node carrying a hash
 *      of 0, which made a real VAX create a directory entry naming OVMX as the
 *      master of resources it did not master (memory cluster-promotion-gap).
 *      A test that only checked the status would pass on a build that still
 *      sent the frame.
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
 *   5. THE LEARNED VALUE IS KEPT. A hash cannot be recomputed, so the resource
 *      block that holds one survives having no locks on it.
 */
#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h -> lock_host_internal.h */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h -> exec_kbackend_host.h */
#include "vms_dlm_proxy.h"    /* the requester + directory seam under test */

#include <stdio.h>
#include <string.h>

/* ================================================================
 * Real executive globals vms_lock.c reads.
 * ================================================================ */
uint32_t vms_local_csid = 1;

#define CSID_LOCAL     1u    /* this node                               */
#define CSID_DIRECTORY 7u    /* the member the weight vector names       */

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
	uint16_t last_hash;
	uint32_t refuse;          /* nonzero => the vector is not usable     */
	int      posts;
	struct vms_dlm_proxy_post last_post;
};

static struct fake_cm cm;

static uint32_t cm_dir_resolve(void *ctx, uint16_t hash16, uint32_t *out_csid)
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
 * 2. In a cluster with no wire-learned hash: refused, and NOTHING SENT.
 * ================================================================ */
static void no_wire_hash_refuses_and_sends_nothing(void)
{
	struct vms_proc proc;
	struct vms_resmaster_args rm;
	uint32_t lkid = 0, st;

	printf("--- in a cluster, a root name with no wire hash is REFUSED ---\n");
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	cm_reset(CSID_DIRECTORY);
	cm_install();
	proc_init(&proc);

	st = do_enq(&proc, "NOVELROOT1", LCK_K_EXMODE, &lkid);
	ct_check_eq_u32(st, SS__UNSUPPORTED,
			"$ENQ on a root name this cluster has never named -> "
			"SS$_UNSUPPORTED");
	ct_check_eq_u32(lkid, 0u, "no lock handle was invented");
	ct_check_eq_u32((unsigned long)cm.posts, 0u,
			"and NOTHING was put on the wire (the anti-LARP clause)");
	ct_check_eq_u32(cm.resolve_calls, 0u,
			"the vector was not even consulted: there was no hash to "
			"index it with");

	read_resmaster("NOVELROOT1", &rm);
	ct_check_eq_u32(rm.dir_csid, 0u,
			"and the readback reports NO directory rather than a "
			"computed one (INV-6)");
	ct_check_eq_u32(rm.master_csid, 0u, "and no master");

	vms_lock_cleanup();
}

/* ================================================================
 * 3. The wire supplies the hash: the lookup routes, with THAT value.
 * ================================================================ */
static void wire_hash_routes_the_lookup(void)
{
	/* Two values chosen so that neither is derivable from its name by any
	 * plausible function -- and so the two names SHARE one, which no
	 * name-derived hash would ever produce. */
	const uint16_t WIRE_HASH = 0xBEEFu;
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

	st = vms_lock_dlm_learn_dir_hash("SHAREDROOT", WIRE_HASH);
	ct_check_eq_u32(st, SS__NORMAL, "a cat-02 frame named SHAREDROOT");
	st = vms_lock_dlm_learn_dir_hash("OTHERROOT", WIRE_HASH);
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

	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("CONFLICT1", 0x1234u),
			SS__NORMAL, "the first value is learned");
	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("CONFLICT1", 0x1234u),
			SS__NORMAL, "the same value again is fine");
	ct_check_eq_u32(vms_lock_dlm_dir_hash_conflicts(), before,
			"and counts no conflict");

	st = vms_lock_dlm_learn_dir_hash("CONFLICT1", 0x5678u);
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

	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash(NULL, 1u), SS__BADPARAM,
			"a null name is refused");
	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("", 1u), SS__BADPARAM,
			"an empty name is refused");
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

	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("GENROOT", 0x0101u),
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
	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("REBUILTROOT", 0x0F0Fu),
			SS__NORMAL, "learned from a rebuild record");

	/* Much later, a local $ENQ. If the block had been reclaimed the value
	 * would be gone and this would be SS$_UNSUPPORTED forever. */
	cm.last_hash = 0;
	st = do_enq(&proc, "REBUILTROOT", LCK_K_EXMODE, &lkid);
	ct_check_eq_u32(st, SS__NORMAL, "a later $ENQ still routes");
	ct_check_eq_u32(cm.last_hash, 0x0F0Fu, "with the value the wire gave");

	/* And when the vector says the entry is OURS, we master it locally. */
	cm.generation++;
	cm.dir_csid = 0u;   /* our own entry (p. 6-32) */
	{
		struct vms_resmaster_args rm;
		uint32_t lkid2 = 0;

		ct_check_eq_u32(vms_lock_dlm_learn_dir_hash("OURSROOT", 0x2222u),
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

int main(void)
{
	printf("=== test_lock_dir (FC-P4.3 dir_resolve in the real engine, R1) ===\n");
	standalone_still_locks();
	no_wire_hash_refuses_and_sends_nothing();
	wire_hash_routes_the_lookup();
	conflicting_learn_is_counted();
	generation_invalidates_the_cache();
	learned_hash_survives_reclaim();
	return ct_summary("test_lock_dir");
}
