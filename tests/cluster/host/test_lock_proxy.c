// SPDX-License-Identifier: GPL-2.0
/*
 * test_lock_proxy.c - R1 host-unit proof of the PROXY LKB (rd FC-P4.4), driven
 * against src/kernel-core/vms_lock.c -- the REAL lock engine -- on the FC-P4.9
 * host backend. No kernel, no boot, no wire: a fake MASTER is injected through
 * the one seam the engine has for reaching the cluster
 * (vms_lock_dlm_set_requester_ops, src/kernel-core/vms_dlm_proxy.h), and every
 * assertion reads REAL executive state back out through the engine's own entry
 * points ($ENQ/$ENQW/$DEQ/$CONVERT/$GETLKI/DLM_ENUM_WAITS/DLM_XNODE).
 *
 * WHAT IS BEING PROVEN (the plan's FC-P4.4 done-condition, item by item):
 *
 *   1. A $ENQ for a resource this node does NOT master creates a PROXY LKB and
 *      POSTS a request -- it does not grant locally. The posted request's every
 *      field is read out of that LKB (INV-6): the fake master asserts req_lkid
 *      IS the proxy's lock id, req_csid IS this node's CSID, the mode IS the
 *      one asked for, the name IS the RSB's.
 *   2. Routing follows the book's three-outcome lookup: the FIRST request for a
 *      tree goes to the DIRECTORY node; once the cluster has told us who
 *      masters it, every later request goes STRAIGHT TO THE MASTER (Davis
 *      p. 6-32, one lookup per tree).
 *   3. $GETLKI, convert, $DEQ, BLKAST delivery and the value block all operate
 *      on that one object -- which is the whole point of replacing the
 *      vms_dlm_origin side list.
 *   4. A synchronous $ENQW on a proxy really SLEEPS (in the engine's own
 *      enq_wait_sync, on the LKB's condition variable) and is woken by the
 *      master's grant arriving on another thread. Not a poll, not a fake.
 *   5. IDEMPOTENT RETRANSMIT, both directions: a duplicate GRANT completes the
 *      SAME proxy (never a second lock under one handle), and a duplicate
 *      inbound request at the MASTER returns the SAME master handle instead of
 *      minting a fresh one -- the property whose absence was the measured
 *      ~35/sec re-request storm.
 *   6. VMS_DLM_LKID_UNSET is REFUSED: no request is ever posted with lock id 0,
 *      and a "grant" that claims a mode while carrying no master handle is
 *      rejected. A placeholder lock id bugchecked a real VAX (INVLOCKID).
 *   7. With no cluster arm installed the engine REFUSES a remote-mastered
 *      resource (SS$_UNSUPPORTED) instead of granting it locally -- Rule 9,
 *      fail-honest, no fallback that fakes the service.
 *
 * HOW "REMOTE" IS ARRANGED WITHOUT FABRICATING A CLUSTER (revised by FC-P4.3).
 * The engine reaches the cluster's Lock Directory Weight Vector through the
 * injected `dir_resolve`/`dir_generation` ops (vms_dlm_proxy.h), so this test
 * plays the connection manager: its resolver answers with ONE directory member,
 * CSID 2, which is NOT this node (CSID 1) -- a genuine and entirely legitimate
 * VMS configuration (a satellite with LOCKDIRWT 0, design D-DLM-1). The static
 * insmod membership vector this test used to poke is gone with the exec_jhash
 * directory it fed.
 *
 * AND THE HASH COMES FROM "THE WIRE". FC-P4.3 makes a directory lookup
 * impossible without a 16-bit hash the CLUSTER supplied for that exact name
 * (Davis p. 6-50). This test is the cluster: wire_learn() below hands the
 * executive a value per resource name, exactly as vms_dlm_scs.c will hand it
 * one parsed out of a cat-0x02 frame. Nothing in the executive computes it, and
 * the honest refusal when it is absent is proved in test_lock_dir.c.
 */

#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h -> lock_host_internal.h */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h -> exec_kbackend_host.h */
#include "vms_dlm_proxy.h"    /* the requester seam under test */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ================================================================
 * Real executive globals vms_lock.c reads. Rewritten per scenario below --
 * they are the module's own controlled configuration inputs, not a pretend
 * cluster: nothing here claims a member that is not configured (INV-6).
 * ================================================================ */
uint32_t vms_local_csid = 1;

#define CSID_LOCAL     1u    /* this node                                    */
#define CSID_DIRECTORY 2u    /* the only directory-participating member       */
#define CSID_MASTER    3u    /* the node the directory names as tree master   */
#define CSID_REQUESTER 4u    /* a fourth member, whose request arrives HERE   */

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;   /* the AST queue is inspected directly below */
}

/* ================================================================
 * The fake MASTER: the smallest possible stand-in for the cluster's DLM arm.
 * It records what the engine posted and nothing else -- it never answers by
 * itself, because in the real system the answer comes back as a separate
 * inbound message (vms_dlm_scs.h RULE A: a reply never leaves by itself).
 * ================================================================ */
struct fake_master {
	pthread_mutex_t         lock;
	pthread_cond_t          arrived;
	int                     posts;
	struct vms_dlm_proxy_post last;
	uint32_t                fail_with;   /* nonzero => refuse every post   */
	int                     saw_zero_lkid;
};

static struct fake_master fm;

static void fm_init(void)
{
	memset(&fm, 0, sizeof(fm));
	pthread_mutex_init(&fm.lock, NULL);
	pthread_cond_init(&fm.arrived, NULL);
}

static uint32_t fm_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	struct fake_master *m = ctx;
	uint32_t st;

	pthread_mutex_lock(&m->lock);
	if (p->req_lkid == 0)
		m->saw_zero_lkid = 1;
	m->last = *p;
	m->posts++;
	st = m->fail_with ? m->fail_with : (uint32_t)SS__NORMAL;
	pthread_cond_broadcast(&m->arrived);
	pthread_mutex_unlock(&m->lock);
	return st;
}

/*
 * This test's stand-in for the connection manager's directory vector
 * (FC-P4.3). `test_dir_csid` is what the vector's entry at `hash mod n` reads:
 * 0 means "this node's own entry" (Davis p. 6-32), anything else names another
 * member. The generation is what the engine keys its cached resolution on.
 */
static uint32_t test_dir_csid;
static uint32_t test_dir_gen = 1;

static uint32_t fm_dir_resolve(void *ctx, uint32_t hash16, uint32_t *out_csid)
{
	(void)ctx;
	(void)hash16;   /* one directory member: every hash lands on it */
	*out_csid = test_dir_csid;
	return (uint32_t)SS__NORMAL;
}

static uint32_t fm_dir_generation(void *ctx)
{
	(void)ctx;
	return test_dir_gen;
}

static void fm_ops_fill(struct vms_dlm_requester_ops *ops)
{
	memset(ops, 0, sizeof(*ops));
	ops->post = fm_post;
	ops->dir_resolve = fm_dir_resolve;
	ops->dir_generation = fm_dir_generation;
	ops->ctx = &fm;
}

static void fm_install(void)
{
	struct vms_dlm_requester_ops ops;

	fm_ops_fill(&ops);
	vms_lock_dlm_set_requester_ops(&ops);
}

/*
 * Play the wire: give the executive the cluster's hash for `resnam`. A distinct
 * deterministic value per name, produced HERE in the test (which is standing in
 * for the sending VMS system), never by the executive.
 */
static void wire_learn(const char *resnam)
{
	uint16_t h = 0x4000u;
	const char *c;

	for (c = resnam; *c != '\0'; c++)
		h = (uint16_t)((h << 1) ^ (uint8_t)*c);
	/* Under the identity the test's own processes enqueue at (uic 0, access
	 * mode 0): a learned value belongs to an identity, not to a name
	 * (rd vms-b5b0). */
	(void)vms_lock_dlm_learn_dir_hash(resnam, 0u, 0u, 0u, h);
}

/* Wait (bounded) until at least `n` requests have been posted, then copy the
 * most recent one out. Returns 0 on timeout. */
static int fm_wait_posts(int n, struct vms_dlm_proxy_post *out)
{
	int i, ok = 0;

	for (i = 0; i < 200; i++) {
		pthread_mutex_lock(&fm.lock);
		if (fm.posts >= n) {
			if (out)
				*out = fm.last;
			ok = 1;
		}
		pthread_mutex_unlock(&fm.lock);
		if (ok)
			return 1;
		usleep(5 * 1000);
	}
	return 0;
}

/* ================================================================
 * Thin wrappers over the engine's real entry points.
 * ================================================================ */
static void proc_init(struct vms_proc *p)
{
	int i;

	memset(p, 0, sizeof(*p));
	exec_lock_init(&p->mode_lock);
	exec_lock_init(&p->lock_list_lock);
	exec_list_head_init(&p->locks);
	/* Every PCB the executive makes carries a process id, and the lock
	 * manager puts it at body[20:24] of a cross-node request that has no
	 * master handle yet (rd vms-c55). A PCB with none is one the executive
	 * never made, so the harness gives each one its own. */
	{
		static uint32_t next_vms_pid = 0x10000001u;

		p->vms_pid = next_vms_pid++;
	}
	for (i = 0; i < 4; i++) {
		exec_lock_init(&p->ast[i].lock);
		exec_list_head_init(&p->ast[i].pending);
	}
}

static uint32_t do_enq_full(struct vms_proc *proc, const char *resnam,
			    uint32_t lkmode, uint32_t flags, uint64_t blkastadr,
			    uint32_t *lkid_out)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = lkmode;
	a.flags = flags;
	a.blkastadr = blkastadr;
	strscpy(a.resnam, resnam, sizeof(a.resnam));
	wire_learn(resnam);   /* the cluster has named this resource (FC-P4.3) */
	vms_ioctl_enq(proc, (unsigned long)(void *)&a);
	if (lkid_out)
		*lkid_out = a.lkid;
	return a.status;
}

static uint32_t do_enq(struct vms_proc *proc, const char *resnam,
		       uint32_t lkmode, uint32_t flags, uint32_t *lkid_out)
{
	return do_enq_full(proc, resnam, lkmode, flags, 0, lkid_out);
}

static uint32_t do_convert(struct vms_proc *proc, uint32_t lkid,
			   uint32_t lkmode, uint32_t flags)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkid = lkid;
	a.lkmode = lkmode;
	a.flags = flags;
	vms_ioctl_convert(proc, (unsigned long)(void *)&a);
	return a.status;
}

static uint32_t do_deq(struct vms_proc *proc, uint32_t lkid)
{
	struct vms_deq_args d;

	memset(&d, 0, sizeof(d));
	d.lkid = lkid;
	vms_ioctl_deq(proc, (unsigned long)(void *)&d);
	return d.status;
}

static uint32_t do_getlki(struct vms_proc *proc, uint32_t lkid,
			  struct vms_getlki_args *out)
{
	memset(out, 0, sizeof(*out));
	out->lkid = lkid;
	vms_ioctl_getlki(proc, (unsigned long)(void *)out);
	return out->status;
}

/* One inbound cluster message, exactly as the DLM's wire arm will deliver it. */
static uint32_t do_xnode(struct vms_proc *proc, struct vms_dlm_xnode_args *a)
{
	return vms_lock_dlm_xnode_dispatch(proc, a);
}

/* The master's answer to a request: a grant (mode > NL) or a queued-reply (NL). */
static uint32_t deliver_grant(struct vms_proc *proc, uint32_t req_lkid,
			      uint32_t mode, uint32_t master_lkid,
			      const char *resnam, const uint8_t *valblk,
			      uint64_t blkastadr, uint64_t blkastprm)
{
	struct vms_dlm_xnode_args a;

	memset(&a, 0, sizeof(a));
	a.op = VMS_DLM_OP_GRANT;
	a.lkmode = mode;
	a.req_lkid = req_lkid;
	a.master_lkid = master_lkid;
	a.req_csid = CSID_LOCAL;
	a.master_csid = CSID_MASTER;
	a.blkastadr = blkastadr;
	a.blkastprm = blkastprm;
	strscpy(a.resnam, resnam, sizeof(a.resnam));
	if (valblk)
		memcpy(a.valblk, valblk, LCK_VALBLK_SIZE);
	return do_xnode(proc, &a);
}

/* Configure the executive as a node whose directory member is somebody else. */
static void configure_remote_directory(void)
{
	vms_local_csid = CSID_LOCAL;
	test_dir_csid = CSID_DIRECTORY;
	test_dir_gen++;
}

static void configure_local_only(void)
{
	vms_local_csid = CSID_LOCAL;
	test_dir_csid = 0;   /* our own entry: this node is the directory */
	test_dir_gen++;
}

/* ================================================================
 * 1. Lifecycle: enqueue -> post -> grant -> convert -> DEQ, all on ONE object.
 * ================================================================ */
static void proxy_lifecycle(void)
{
	static const uint8_t master_lvb[LCK_VALBLK_SIZE] = {
		0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
		0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C
	};
	const uint32_t MASTER_LKID = 0x00004444u;
	struct vms_dlm_proxy_post post;
	struct vms_getlki_args gk;
	struct vms_proc proc;
	uint32_t lkid = 0, lkid2 = 0, st;

	printf("--- proxy lifecycle: enqueue, grant, convert, DEQ ---\n");
	configure_remote_directory();
	fm_init();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	fm_install();
	proc_init(&proc);

	/* (1) $ENQ for a tree this node does not master: a proxy + a posted
	 * request, NOT a local grant. */
	st = do_enq(&proc, "PROXYRES1", LCK_K_EXMODE, 0, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0,
		 "$ENQ on a remote-mastered resource returns a real lock handle");
	ct_check(fm_wait_posts(1, &post), "the executive POSTED a request");
	ct_check_eq_u32(post.op, VMS_DLM_OP_ENQ, "posted op is ENQ");
	ct_check_eq_u32(post.dst_csid, CSID_DIRECTORY,
			"first request for the tree goes to the DIRECTORY node");
	ct_check_eq_u32(post.req_csid, CSID_LOCAL,
			"req_csid is read from the executive's own CSID");
	ct_check_eq_u32(post.req_lkid, lkid,
			"req_lkid IS the proxy LKB's lock id (not a wire echo)");
	ct_check_eq_u32(post.lkmode, LCK_K_EXMODE, "posted mode is the mode asked for");
	ct_check_eq_u32(post.master_lkid, 0,
			"no master handle is asserted before the master gave one");
	ct_check(strcmp(post.resnam, "PROXYRES1") == 0,
		 "posted resource name is the RSB's own name");

	/* (2) It is PENDING, honestly: granted NL until the master says otherwise. */
	st = do_getlki(&proc, lkid, &gk);
	ct_check(st == SS__NORMAL && gk.granted_mode == LCK_K_NLMODE &&
		 gk.requested_mode == LCK_K_EXMODE,
		 "$GETLKI on the proxy: pending (granted NL, requested EX)");
	ct_check(strcmp(gk.resnam, "PROXYRES1") == 0,
		 "$GETLKI reports the resource name from the proxy's RSB");

	/* (3) It is an outstanding wait the deadlock search can see. */
	{
		struct vms_dlm_enum_waits_args w;

		memset(&w, 0, sizeof(w));
		vms_ioctl_dlm_enum_waits(&proc, (unsigned long)(void *)&w);
		ct_check_eq_u32(w.count, 1, "DLM_ENUM_WAITS sees exactly one pending wait");
		ct_check(w.count == 1 && w.ent[0].req_lkid == lkid &&
			 strcmp(w.ent[0].resnam, "PROXYRES1") == 0,
			 "the enumerated wait names this proxy and its resource");
	}

	/* (4) The master's grant lands: the proxy flips NL -> EX and its value
	 * block becomes the master's (the LVB read crossing, on the proxy). */
	st = deliver_grant(&proc, lkid, LCK_K_EXMODE, MASTER_LKID, "PROXYRES1",
			   master_lvb, 0, 0);
	ct_check_eq_u32(st, SS__NORMAL, "the master's GRANT is accepted");
	st = do_getlki(&proc, lkid, &gk);
	ct_check(st == SS__NORMAL && gk.granted_mode == LCK_K_EXMODE,
		 "$GETLKI: the proxy FLIPPED NL -> EX, driven only by the master");
	ct_check(memcmp(gk.valblk, master_lvb, LCK_VALBLK_SIZE) == 0,
		 "$GETLKI returns the value block the master's grant carried (LVB)");
	{
		struct vms_dlm_enum_waits_args w;

		memset(&w, 0, sizeof(w));
		vms_ioctl_dlm_enum_waits(&proc, (unsigned long)(void *)&w);
		ct_check_eq_u32(w.count, 0, "a GRANTED proxy is a hold, not a wait-for edge");
	}

	/* (5) The cluster told us the master, so the NEXT request for this tree
	 * goes straight there (Davis p. 6-32: one lookup per tree). */
	st = do_enq(&proc, "PROXYRES1", LCK_K_CRMODE, 0, &lkid2);
	ct_check(st == SS__NORMAL && lkid2 != 0 && lkid2 != lkid,
		 "a second $ENQ on the tree gets its own proxy handle");
	ct_check(fm_wait_posts(2, &post), "the second request was posted");
	ct_check_eq_u32(post.dst_csid, CSID_MASTER,
			"the second request goes STRAIGHT TO THE MASTER, not the directory");
	ct_check(do_deq(&proc, lkid2) == SS__NORMAL, "the second proxy releases");

	/* (6) Convert operates on the proxy and carries the master's handle. */
	st = do_convert(&proc, lkid, LCK_K_PRMODE, 0);
	ct_check_eq_u32(st, SS__NORMAL, "$CONVERT on a proxy is accepted (posted)");
	ct_check(fm_wait_posts(4, &post), "the conversion was posted");
	ct_check_eq_u32(post.lkmode, LCK_K_PRMODE, "the posted conversion carries the new mode");
	ct_check_eq_u32(post.master_lkid, MASTER_LKID,
			"the conversion carries the MASTER's handle, as the master gave it");
	ct_check_eq_u32(post.dst_csid, CSID_MASTER, "the conversion is addressed to the master");
	st = deliver_grant(&proc, lkid, LCK_K_PRMODE, MASTER_LKID, "PROXYRES1",
			   master_lvb, 0, 0);
	ct_check_eq_u32(st, SS__NORMAL, "the master's conversion grant is accepted");
	st = do_getlki(&proc, lkid, &gk);
	ct_check(st == SS__NORMAL && gk.granted_mode == LCK_K_PRMODE,
		 "$GETLKI: the proxy is now granted at the converted mode (PR)");

	/* (7) $DEQ releases AT THE MASTER first, naming the master's handle. */
	st = do_deq(&proc, lkid);
	ct_check_eq_u32(st, SS__NORMAL, "$DEQ on the proxy succeeds");
	ct_check(fm_wait_posts(5, &post), "the release was posted to the master");
	ct_check_eq_u32(post.op, VMS_DLM_OP_DEQ, "posted op is DEQ");
	ct_check_eq_u32(post.master_lkid, MASTER_LKID, "the release names the master's handle");
	ct_check_eq_u32(post.dst_csid, CSID_MASTER, "the release is addressed to the master");
	ct_check_eq_u32(do_deq(&proc, lkid), SS__IVLOCKID,
			"the proxy is gone: a second $DEQ is SS$_IVLOCKID");
	ct_check(fm.saw_zero_lkid == 0,
		 "no request was ever posted with lock id 0 (VMS_DLM_LKID_UNSET)");

	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* ================================================================
 * 2. The synchronous wait: $ENQW posts, SLEEPS on the LKB, and the master's
 *    grant -- arriving on another thread -- wakes it.
 * ================================================================ */
struct sync_result {
	struct vms_proc *proc;
	uint32_t         status;
	uint32_t         lkid;
	uint32_t         granted;
};

static void *sync_enq_thread(void *arg)
{
	struct sync_result *r = arg;
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = LCK_K_EXMODE;
	a.flags = LCK_M_SYNC;
	strscpy(a.resnam, "PROXYSYNC", sizeof(a.resnam));
	wire_learn("PROXYSYNC");   /* the cluster has named it (FC-P4.3) */
	vms_ioctl_enq(r->proc, (unsigned long)(void *)&a);
	r->status = a.status;
	r->lkid = a.lkid;
	r->granted = a.lk_status;
	return NULL;
}

static void proxy_sync_wait(void)
{
	struct vms_dlm_proxy_post post;
	struct sync_result sr;
	struct vms_proc proc;
	pthread_t th;

	printf("--- proxy $ENQW: post, sleep on the LKB, the grant wakes it ---\n");
	configure_remote_directory();
	fm_init();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	fm_install();
	proc_init(&proc);

	memset(&sr, 0, sizeof(sr));
	sr.proc = &proc;
	if (pthread_create(&th, NULL, sync_enq_thread, &sr) != 0) {
		ct_check(0, "pthread_create for the synchronous proxy $ENQW");
		vms_lock_cleanup();
		return;
	}

	/* The request must reach the master BEFORE any grant exists -- that is
	 * what makes the wake below a real wake and not an already-granted
	 * request returning immediately. */
	ct_check(fm_wait_posts(1, &post),
		 "$ENQW posted its request and did not return");
	usleep(30 * 1000);
	ct_check(sr.status == 0,
		 "the caller is still ASLEEP in the engine (no status yet)");

	/* Now the master answers, from this (the delivery) thread. */
	ct_check_eq_u32(deliver_grant(&proc, post.req_lkid, LCK_K_EXMODE,
				      0x00005555u, "PROXYSYNC", NULL, 0, 0),
			SS__NORMAL, "the master's GRANT is delivered");
	pthread_join(th, NULL);
	ct_check(sr.status == SS__NORMAL && sr.lkid == post.req_lkid &&
		 sr.granted == LCK_K_EXMODE,
		 "$ENQW WOKE and returned granted at EX (real cv wait/wake on the LKB)");

	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* ================================================================
 * 3. Blocking-AST delivery on the proxy: a BLKAST from the master fires the
 *    holder's own routine, on the process that owns the proxy.
 * ================================================================ */
static int drain_one_ast(struct vms_proc *proc, uint64_t *astadr, uint64_t *astprm)
{
	struct vms_ast_state *st = &proc->ast[PSL_C_USER];
	struct vms_ast_entry *e;
	int found = 0;

	exec_lock(&st->lock);
	exec_list_for_each_entry(e, &st->pending, list) {
		*astadr = e->astadr;
		*astprm = e->astprm;
		found = 1;
		break;
	}
	exec_unlock(&st->lock);
	return found;
}

static void proxy_blkast(void)
{
	const uint64_t HOLDER_BLKAST = 0xC0DE1234BEEF0000ull;
	const uint64_t HOLDER_PRM    = 0x00000000000000A1ull;
	struct vms_dlm_xnode_args x;
	struct vms_dlm_proxy_post post;
	struct vms_proc proc;
	uint64_t astadr = 0, astprm = 0;
	uint32_t lkid = 0, st;

	printf("--- proxy BLKAST: the master's blocking AST fires the holder's routine ---\n");
	configure_remote_directory();
	fm_init();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	fm_install();
	proc_init(&proc);

	st = do_enq_full(&proc, "PROXYBLK1", LCK_K_EXMODE, 0, HOLDER_BLKAST, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0, "$ENQ with a blocking-AST routine");
	ct_check(fm_wait_posts(1, &post), "the request was posted");
	ct_check_eq_u32(deliver_grant(&proc, lkid, LCK_K_EXMODE, 0x00006666u,
				      "PROXYBLK1", NULL, HOLDER_BLKAST, HOLDER_PRM),
			SS__NORMAL, "the master GRANTS it, holder established");

	/* A BLKAST naming a handle that is not one of our proxies is refused. */
	memset(&x, 0, sizeof(x));
	x.op = VMS_DLM_OP_BLKAST;
	x.req_lkid = lkid + 0x1000u;
	x.req_csid = CSID_LOCAL;
	ct_check_eq_u32(do_xnode(&proc, &x), SS__UNSUPPORTED,
			"BLKAST for an unknown handle -> SS$_UNSUPPORTED (no fake AST)");
	ct_check_eq_u32(x.blkast_delivered, 0, "and nothing was delivered");

	/* The real one FIRES. */
	memset(&x, 0, sizeof(x));
	x.op = VMS_DLM_OP_BLKAST;
	x.req_lkid = lkid;
	x.req_csid = CSID_LOCAL;
	strscpy(x.resnam, "PROXYBLK1", sizeof(x.resnam));
	ct_check_eq_u32(do_xnode(&proc, &x), SS__NORMAL,
			"BLKAST on the proxy -> SS$_NORMAL");
	ct_check_eq_u32(x.blkast_delivered, 1, "a real blocking AST was queued");
	ct_check(drain_one_ast(&proc, &astadr, &astprm),
		 "the AST is on the owning process's USER-mode queue");
	ct_check(astadr == HOLDER_BLKAST && astprm == HOLDER_PRM,
		 "the queued AST carries the routine + parameter the holder registered");

	ct_check(do_deq(&proc, lkid) == SS__NORMAL, "the holder releases its proxy");
	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* ================================================================
 * 4. Idempotent retransmit + the LKID_UNSET refusals.
 * ================================================================ */
static void proxy_idempotent_and_lkid_unset(void)
{
	const uint32_t MASTER_LKID = 0x00007777u;
	struct vms_dlm_xnode_args x;
	struct vms_dlm_proxy_post post;
	struct vms_getlki_args gk;
	struct vms_proc proc;
	uint32_t lkid = 0, st;

	printf("--- requester-side idempotency + VMS_DLM_LKID_UNSET refusal ---\n");
	configure_remote_directory();
	fm_init();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	fm_install();
	proc_init(&proc);

	st = do_enq(&proc, "PROXYDUP1", LCK_K_EXMODE, 0, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0, "$ENQ creates the proxy");
	ct_check(fm_wait_posts(1, &post), "the request was posted");

	/* A grant, then the SAME grant again (a retransmit). */
	ct_check_eq_u32(deliver_grant(&proc, lkid, LCK_K_EXMODE, MASTER_LKID,
				      "PROXYDUP1", NULL, 0, 0),
			SS__NORMAL, "the master's GRANT completes the proxy");
	ct_check_eq_u32(deliver_grant(&proc, lkid, LCK_K_EXMODE, MASTER_LKID,
				      "PROXYDUP1", NULL, 0, 0),
			SS__NORMAL, "a DUPLICATE GRANT is accepted idempotently");
	st = do_getlki(&proc, lkid, &gk);
	ct_check(st == SS__NORMAL && gk.granted_mode == LCK_K_EXMODE,
		 "the proxy is still granted at EX -- not re-granted, not doubled");
	/* Exactly ONE object exists under that handle: it releases once. */
	ct_check_eq_u32(do_deq(&proc, lkid), SS__NORMAL,
			"the (single) proxy releases");
	ct_check_eq_u32(do_deq(&proc, lkid), SS__IVLOCKID,
			"no SECOND lock was minted by the duplicate grant");

	/* VMS_DLM_LKID_UNSET, both halves. */
	memset(&x, 0, sizeof(x));
	x.op = VMS_DLM_OP_GRANT;
	x.lkmode = LCK_K_EXMODE;
	x.req_lkid = 0;
	x.master_lkid = MASTER_LKID;
	x.req_csid = CSID_LOCAL;
	strscpy(x.resnam, "PROXYDUP1", sizeof(x.resnam));
	ct_check_eq_u32(do_xnode(&proc, &x), SS__BADPARAM,
			"a GRANT with NO requester handle is refused");

	memset(&x, 0, sizeof(x));
	x.op = VMS_DLM_OP_GRANT;
	x.lkmode = LCK_K_EXMODE;
	x.req_lkid = 0x00090077u;
	x.master_lkid = VMS_DLM_LKID_UNSET;
	x.req_csid = CSID_LOCAL;
	strscpy(x.resnam, "PROXYDUP1", sizeof(x.resnam));
	ct_check_eq_u32(do_xnode(&proc, &x), SS__BADPARAM,
			"a GRANT claiming a mode with NO master handle is refused "
			"(the fc8540ae placeholder-lock-id crasher)");

	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* ================================================================
 * 5. The MASTER side of idempotency: a retransmitted request gets the SAME
 *    handle instead of a fresh one (the anti-storm property).
 * ================================================================ */
static uint32_t master_enq(struct vms_proc *proc, uint32_t req_csid,
			   uint32_t req_lkid, uint32_t mode, const char *resnam,
			   uint32_t *master_lkid)
{
	struct vms_dlm_xnode_args a;
	uint32_t st;

	memset(&a, 0, sizeof(a));
	a.op = VMS_DLM_OP_ENQ;
	a.lkmode = mode;
	a.req_lkid = req_lkid;
	a.req_csid = req_csid;
	strscpy(a.resnam, resnam, sizeof(a.resnam));
	st = do_xnode(proc, &a);
	if (master_lkid)
		*master_lkid = a.master_lkid;
	return st;
}

static void master_idempotent_retransmit(void)
{
	const uint32_t PEER_A = 1025u, PEER_B = 1026u;
	struct vms_proc proc;
	uint32_t l1 = 0, l1b = 0, l2 = 0, l2b = 0, st;

	printf("--- master-side retransmit idempotency (the anti-storm property) ---\n");
	configure_local_only();      /* this node masters what it is sent */
	fm_init();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	proc_init(&proc);

	st = master_enq(&proc, PEER_A, 0x00000011u, LCK_K_EXMODE, "MASTERIDEM", &l1);
	ct_check(st == SS__NORMAL && l1 != 0, "peer A's cross-node $ENQ is GRANTED");

	st = master_enq(&proc, PEER_A, 0x00000011u, LCK_K_EXMODE, "MASTERIDEM", &l1b);
	ct_check_eq_u32(st, SS__NORMAL, "the RETRANSMIT is granted again");
	ct_check_eq_u32(l1b, l1,
			"and returns the SAME master handle -- no fresh lock was minted");

	st = master_enq(&proc, PEER_B, 0x00000022u, LCK_K_EXMODE, "MASTERIDEM", &l2);
	ct_check(st == (uint32_t)VMS_DLM_STS_QUEUED && l2 != 0 && l2 != l1,
		 "peer B's incompatible $ENQ QUEUES with its own handle");

	st = master_enq(&proc, PEER_B, 0x00000022u, LCK_K_EXMODE, "MASTERIDEM", &l2b);
	ct_check(st == (uint32_t)VMS_DLM_STS_QUEUED,
		 "B's RETRANSMIT is still QUEUED");
	ct_check_eq_u32(l2b, l2,
			"and returns the SAME queued handle -- the request was not double-queued");

	/* The master's own locks are not proxies, and a peer may not complete one
	 * as if it were this node's requester-side image. */
	{
		struct vms_dlm_xnode_args g;

		memset(&g, 0, sizeof(g));
		g.op = VMS_DLM_OP_GRANT;
		g.lkmode = LCK_K_EXMODE;
		g.req_lkid = l1;
		g.master_lkid = 0x00001234u;
		g.req_csid = PEER_A;
		strscpy(g.resnam, "MASTERIDEM", sizeof(g.resnam));
		ct_check_eq_u32(do_xnode(&proc, &g), SS__IVLOCKID,
				"a GRANT naming one of OUR OWN locks is refused, "
				"never converted into a proxy");
	}

	{
		uint32_t found = 0, n_granted = 0;
		struct vms_resmaster_args rm;

		memset(&rm, 0, sizeof(rm));
		strscpy(rm.resnam, "MASTERIDEM", sizeof(rm.resnam));
		vms_ioctl_get_resmaster(&proc, (unsigned long)(void *)&rm);
		found = rm.found;
		n_granted = rm.n_granted;
		ct_check(found == 1 && n_granted == 1,
			 "the master still holds exactly ONE grant after both retransmits");
	}

	vms_lock_cleanup();
}

/* ================================================================
 * 6. No cluster arm installed: the honest refusal (Rule 9, no fallback).
 * ================================================================ */
static void no_cluster_arm_refuses(void)
{
	struct vms_proc proc;
	uint32_t lkid = 0, st;

	printf("--- no cluster arm: a remote-mastered resource is REFUSED, not granted ---\n");
	configure_remote_directory();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	/*
	 * A node that IS in a cluster (the directory resolves, to somebody else)
	 * whose DLM wire arm cannot send. FC-P4.3 keeps the two halves separate
	 * on purpose: `post` absent is "cannot reach the cluster", while ops
	 * absent altogether is "there is no cluster", and only the first is a
	 * refusal -- a standalone node must still lock.
	 */
	{
		struct vms_dlm_requester_ops ops;

		fm_ops_fill(&ops);
		ops.post = NULL;
		vms_lock_dlm_set_requester_ops(&ops);
	}
	proc_init(&proc);

	st = do_enq(&proc, "PROXYNOARM", LCK_K_EXMODE, 0, &lkid);
	ct_check_eq_u32(st, SS__UNSUPPORTED,
			"$ENQ on a remote-mastered resource with no cluster arm "
			"-> SS$_UNSUPPORTED");
	ct_check_eq_u32(lkid, 0, "and no lock handle was invented");

	/* The same node still locks perfectly well for trees it masters. */
	configure_local_only();
	st = do_enq(&proc, "PROXYLOCAL", LCK_K_EXMODE, 0, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0,
		 "a locally-mastered resource still grants normally");
	ct_check(do_deq(&proc, lkid) == SS__NORMAL, "and releases");

	vms_lock_cleanup();
}

/* ================================================================
 * 7. THE DIRECTORY REDIRECT (rd vms-b96): an inbound cross-node request that
 *    reached a node which does not master the tree is answered with the
 *    MASTER'S CSID (Davis p. 6-31 outcome 2) instead of being declined blind.
 *
 * THE FOUR ASSERTIONS, and each one is a way the redirect could have been a
 * LARP instead of an answer:
 *
 *   (a) IT REDIRECTS, AND TO THE RIGHT NODE. The target is the master THIS
 *       EXECUTIVE HOLDS -- and it is arranged the only way it can be, by the
 *       cluster GRANTING and naming that master, so the value under test came
 *       off the wire into the lock database and back out again.
 *   (b) IT MINTS NOTHING. A redirect is an answer, not a lock: no handle is
 *       echoed and no lock appears on the resource for the requester. Two
 *       masters for one tree is how a real cluster breaks (D-DLM-4).
 *   (c) THE LOOP GUARD. A redirect that would send the request straight back
 *       to the node that sent it is DECLINED, not sent. That is the only way
 *       one hop of a reply-based redirect can turn round on itself.
 *   (d) NO MASTER HELD, NO TARGET INVENTED. This node's own weight vector
 *       resolves a DIRECTORY (CSID 2) for every name here. A directory is not
 *       a master, so it is never named as one -- the request is declined and
 *       master_csid stays 0. This is the assertion that fails on a build that
 *       "helpfully" redirects to whatever dir_resolve returned.
 * ================================================================ */
static void inbound_redirect(void)
{
	const uint32_t MASTER_LKID = 0x00008888u;
	struct vms_dlm_xnode_args x;
	struct vms_resmaster_args rm;
	struct vms_proc proc;
	uint32_t lkid = 0, st;

	printf("--- inbound REDIRECT: a mis-addressed cross-node $ENQ is told "
	       "who masters the tree ---\n");
	configure_remote_directory();
	fm_init();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	fm_install();
	proc_init(&proc);

	/*
	 * Give this node a REAL master for the tree, the only way it can get
	 * one: a $ENQ posts a lookup and the CLUSTER answers with a GRANT that
	 * names the master. After this, res->master_csid is a value the cluster
	 * supplied -- which is exactly (and only) what a redirect may name.
	 */
	st = do_enq(&proc, "REDIRTREE", LCK_K_EXMODE, 0, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0,
		 "$ENQ posts a lookup for a tree mastered elsewhere");
	ct_check_eq_u32(deliver_grant(&proc, lkid, LCK_K_EXMODE, MASTER_LKID,
				      "REDIRTREE", NULL, 0, 0),
			SS__NORMAL,
			"the cluster GRANTS and names CSID 3 as the tree's master");

	/* (a) THE REDIRECT. A fourth member asks US for a lock on that tree. */
	memset(&x, 0, sizeof(x));
	x.op = VMS_DLM_OP_ENQ;
	x.lkmode = LCK_K_EXMODE;
	x.req_lkid = 0x00A10001u;
	x.req_csid = CSID_REQUESTER;
	strscpy(x.resnam, "REDIRTREE", sizeof(x.resnam));
	st = do_xnode(&proc, &x);
	ct_check_eq_u32(st, (uint32_t)VMS_DLM_STS_REDIRECT,
			"*** a mis-addressed inbound $ENQ REDIRECTS (it was "
			"SS$_UNSUPPORTED) ***");
	ct_check_eq_u32(x.master_csid, CSID_MASTER,
			"*** and names the master THIS EXECUTIVE HOLDS -- not "
			"the directory node its vector resolved ***");

	/* (b) IT MINTS NOTHING. */
	ct_check_eq_u32(x.master_lkid, 0u,
			"no handle is echoed: this node holds no lock for it");
	ct_check_eq_u32(x.queued, 0u, "and nothing was queued");
	memset(&rm, 0, sizeof(rm));
	strscpy(rm.resnam, "REDIRTREE", sizeof(rm.resnam));
	vms_ioctl_get_resmaster(&proc, (unsigned long)(void *)&rm);
	ct_check_eq_u32(rm.is_local_master, 0u,
			"this node still does not master the tree");
	ct_check_eq_u32(rm.n_granted, 0u,
			"and granted NOTHING to the requester (D-DLM-4: never "
			"two masters for one tree)");

	/* (c) THE LOOP GUARD. */
	memset(&x, 0, sizeof(x));
	x.op = VMS_DLM_OP_ENQ;
	x.lkmode = LCK_K_EXMODE;
	x.req_lkid = 0x00A10002u;
	x.req_csid = CSID_MASTER;      /* the very node we would redirect TO */
	strscpy(x.resnam, "REDIRTREE", sizeof(x.resnam));
	st = do_xnode(&proc, &x);
	ct_check_eq_u32(st, (uint32_t)SS__UNSUPPORTED,
			"*** a redirect that would bounce the request back to "
			"its sender is DECLINED, never sent ***");
	ct_check_eq_u32(x.master_csid, 0u, "and it names nobody");

	/* (d) NO MASTER HELD, NO TARGET INVENTED. */
	wire_learn("REDIRUNKNOWN");    /* the cluster named it; we master nothing */
	memset(&x, 0, sizeof(x));
	x.op = VMS_DLM_OP_ENQ;
	x.lkmode = LCK_K_EXMODE;
	x.req_lkid = 0x00A10003u;
	x.req_csid = CSID_REQUESTER;
	strscpy(x.resnam, "REDIRUNKNOWN", sizeof(x.resnam));
	st = do_xnode(&proc, &x);
	ct_check_eq_u32(st, (uint32_t)SS__UNSUPPORTED,
			"an inbound request for a tree we hold no master for "
			"is DECLINED");
	ct_check_eq_u32(x.master_csid, 0u,
			"*** and the DIRECTORY the vector resolved is NEVER "
			"named as the master (INV-6) ***");

	ct_check(do_deq(&proc, lkid) == SS__NORMAL, "the proxy releases");
	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* $ENQ with NO wire-learned hash: the engine computes the proven one itself
 * (rd vms-b5b0), so the resource block is reclaimed at its last lock. */
static uint32_t do_enq_computed(struct vms_proc *proc, const char *resnam,
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

/*
 * rd vms-e09: A MASTER THAT FREES ITS RESOURCE REMOVES ITS DIRECTORY ENTRY.
 *
 * Lab DC1b: OVMXA mastered F11B$sOVMXSYS on VAX1's 0xf9, freed it, and told
 * nobody. VAX1 then sent OVMXB to OVMXA with an 0xf8, and OVMXA had nothing to
 * serve. A real master sends the directory node a named op-0x04 as the
 * resource goes (8106 real removals, PREDICTION.md R1/R3).
 */
static void e09_master_removes_its_directory_entry(void)
{
	struct vms_dlm_proxy_post enq, rm;
	struct vms_proc proc;
	uint32_t lkid = 0, st;
	int before;

	printf("--- rd vms-e09: the master removes its directory entry as the "
	       "resource goes ---\n");
	configure_remote_directory();
	fm_init();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	fm_install();
	proc_init(&proc);

	st = do_enq_computed(&proc, "F11B$sE09SYS", LCK_K_EXMODE, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0,
		 "$ENQ posts a lookup to the remote directory node");
	if (!fm_wait_posts(1, &enq)) {
		ct_check(0, "the lookup was posted");
		vms_lock_dlm_set_requester_ops(NULL);
		vms_lock_cleanup();
		return;
	}
	ct_check(enq.to_directory && enq.dst_csid == CSID_DIRECTORY &&
		 enq.dir_hash_known,
		 "the lookup goes to CSID 2 carrying a hash the engine holds");

	/* The directory answers 0xf9: "nobody masters it -- you do". */
	ct_check_eq_u32(vms_lock_dlm_assume_mastery("F11B$sE09SYS", 0u, lkid),
			SS__NORMAL, "this node assumes mastery on the 0xf9");

	pthread_mutex_lock(&fm.lock);
	before = fm.posts;
	pthread_mutex_unlock(&fm.lock);
	ct_check(do_deq(&proc, lkid) == SS__NORMAL, "the last lock goes");
	if (!fm_wait_posts(before + 1, &rm)) {
		ct_check(0, "*** freeing the mastered resource posts something ***");
		vms_lock_dlm_set_requester_ops(NULL);
		vms_lock_cleanup();
		return;
	}
	ct_check_eq_u32(rm.op, VMS_DLM_POST_DIR_REMOVE,
			"*** it is the directory-entry removal ***");
	ct_check_eq_u32(rm.dst_csid, CSID_DIRECTORY,
			"... to the directory node that holds the entry");
	ct_check(strcmp(rm.resnam, "F11B$sE09SYS") == 0,
		 "... for the resource that went");
	ct_check(rm.dir_hash_known && rm.dir_hash == enq.dir_hash,
		 "... with the hash the lookup carried (R1)");
	ct_check(rm.res_group == enq.res_group &&
		 rm.res_acmode == enq.res_acmode,
		 "... and the identity the lookup carried (R1)");
	ct_check_eq_u32(rm.req_lkid, 0u,
			"it names no lock: the resource is gone");
	ct_check_eq_u32(vms_lock_dlm_dir_removes_posted(), 1u,
			"and the engine counted one removal posted");

	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* ...and NOTHING when this node is the directory itself, or not the master. */
static void e09_no_removal_without_a_remote_entry(void)
{
	struct vms_proc proc;
	uint32_t lkid = 0;
	int before;

	printf("--- rd vms-e09: no removal when no remote directory holds an "
	       "entry naming this node ---\n");
	configure_local_only();
	fm_init();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	fm_install();
	proc_init(&proc);

	(void)do_enq_computed(&proc, "F11B$sE09OWN", LCK_K_EXMODE, &lkid);
	pthread_mutex_lock(&fm.lock);
	before = fm.posts;
	pthread_mutex_unlock(&fm.lock);
	ct_check(do_deq(&proc, lkid) == SS__NORMAL, "the lock goes");
	usleep(50 * 1000);
	pthread_mutex_lock(&fm.lock);
	ct_check_eq_u32((uint32_t)fm.posts, (uint32_t)before,
			"this node is the directory: nothing goes on the wire");
	pthread_mutex_unlock(&fm.lock);

	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/*
 * rd vms-c55: NO REMOVAL FOR AN ENTRY NO DIRECTORY EVER HELD. A resource this
 * node mastered while no remote directory was in play (before the cluster
 * committed a vector) was registered at nobody. When its last lock goes after
 * the vector names a remote directory, telling that node to forget it is a
 * removal VMS never sends: the real V7.3 answers cat-0x02 op-0x19 and
 * bugchecks LOCKMGRERR (4 of 4 captured crashes, 0 of ~190 clean removals).
 */
static void c55_no_removal_for_an_unregistered_entry(void)
{
	struct vms_proc proc;
	uint32_t lkid = 0;
	int before;

	printf("--- rd vms-c55: no removal for a resource no directory holds ---\n");
	configure_local_only();
	fm_init();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	fm_install();
	proc_init(&proc);

	(void)do_enq_computed(&proc, "F11B$sC55PRE", LCK_K_EXMODE, &lkid);
	configure_remote_directory();    /* the vector now names another node */
	pthread_mutex_lock(&fm.lock);
	before = fm.posts;
	pthread_mutex_unlock(&fm.lock);
	ct_check(do_deq(&proc, lkid) == SS__NORMAL, "the lock goes");
	usleep(50 * 1000);
	pthread_mutex_lock(&fm.lock);
	ct_check_eq_u32((uint32_t)fm.posts, (uint32_t)before,
			"*** a resource mastered before any directory held an "
			"entry for it is NOT removed from the directory ***");
	pthread_mutex_unlock(&fm.lock);

	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}


/*
 * ==========================================================================
 * rd vms-326: A COUNTED, BINARY RESOURCE NAME IS ITS OWN RESOURCE.
 *
 * THE DEFECT THIS CLOSES, measured on a real cluster and in a real demo. VMS
 * names RMS's file-access lock
 *
 *     'RMS$' | 6 FID bytes | 0x02 | 12-byte space-padded volume label | 00 00 00
 *
 * -- 26 bytes with three NULs in it, on 1,130 real cat-0x02 frames
 * (tools/cluster/dlm_rms_name_hash_check.py). The executive keyed resources by a
 * NUL-TERMINATED C string, so the two names below -- the SAME file id on two
 * DIFFERENT volumes -- compared equal at their first NUL and became ONE
 * cluster-wide resource. Node A writing NETACP.LOG then refused node B's read
 * of a different file on another disk (rd vms-acf3), and the 7-byte truncation
 * is what went on the wire.
 *
 * WHAT IS PROVEN HERE, against the real engine:
 *   1. two names that differ only AFTER an embedded NUL are two resources --
 *      both $ENQ EX requests are granted;
 *   2. and that is not vacuous: the SAME name from another process at EX with
 *      NOQUEUE is refused SS$_NOTQUEUED, so these resources really do conflict;
 *   3. the POST the wire arm would transmit carries the COUNT (26) and all 26
 *      bytes -- read off the resource block by dlm_proxy_fill_post, the INV-6
 *      chokepoint -- and the two names' directory hashes DIFFER, so the two
 *      lookups go to the identity the cluster would compute for each;
 *   4. $GETLKI reads the 26-byte name back whole, not as its 7-byte prefix;
 *   5. a count outside 1..31 is SS$_IVBUFLEN, the refusal $ENQ makes on the
 *      descriptor.
 * ==========================================================================
 */

/* The measured RMS file-access name for (fid, volume label). `out` is 32. */
static uint32_t rms326_name(char *out, uint16_t fid_num, uint16_t fid_seq,
			    uint8_t fid_rvn, uint8_t fid_nmx,
			    const char *label)
{
	uint32_t i;

	memset(out, 0, 32);
	memcpy(out, "RMS$", 4);
	out[4] = (char)(fid_num & 0xffu);
	out[5] = (char)((fid_num >> 8) & 0xffu);
	out[6] = (char)(fid_seq & 0xffu);
	out[7] = (char)((fid_seq >> 8) & 0xffu);
	out[8] = (char)fid_rvn;
	out[9] = (char)fid_nmx;
	out[10] = (char)0x02;
	for (i = 0u; i < 12u; i++)
		out[11 + i] = (label[i] != '\0') ? label[i] : ' ';
	/* out[23..25] stay NUL -- the three the measured form ends with. */
	return 26u;
}

/* $ENQ a COUNTED name: the length is stated, exactly as $ENQ states the
 * descriptor's dsc$w_length (src/libvms/syssvc/sys_lock.c). */
static uint32_t do_enq_counted(struct vms_proc *proc, const char *resnam,
			       uint32_t resnam_len, uint32_t lkmode,
			       uint32_t flags, uint32_t *lkid_out)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = lkmode;
	a.flags = flags;
	memcpy(a.resnam, resnam, sizeof(a.resnam));
	a.resnam_len = resnam_len;
	vms_ioctl_enq(proc, (unsigned long)(void *)&a);
	if (lkid_out)
		*lkid_out = a.lkid;
	return a.status;
}

static void rms326_counted_binary_names(void)
{
	char n1[32], n2[32];
	struct vms_dlm_proxy_post p1, p2;
	struct vms_getlki_args gk;
	struct vms_proc proc1, proc2;
	uint32_t len, lkid1 = 0, lkid2 = 0, lkid3 = 0, st;

	printf("--- rd vms-326: a counted binary resource name is its own "
	       "resource ---\n");
	len = rms326_name(n1, 0x011cu, 1u, 0u, 0u, "SYSDSK1");
	(void)rms326_name(n2, 0x011cu, 1u, 0u, 0u, "VAX1DATA");
	ct_check(len == 26u && memcmp(n1, n2, 11) == 0 && n1[7] == '\0',
		 "the two names share their first 11 bytes and both carry a "
		 "NUL inside them");

	configure_remote_directory();
	fm_init();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	fm_install();
	proc_init(&proc1);
	proc_init(&proc2);

	/* (1) Two resources, not one. Both are mastered elsewhere here (the
	 * directory is another node), so each $ENQ posts and reports the
	 * request outstanding -- what matters is that there are TWO of them. */
	st = do_enq_counted(&proc1, n1, len, LCK_K_EXMODE, 0, &lkid1);
	ct_check(st == SS__NORMAL && lkid1 != 0,
		 "$ENQ on the SYSDSK1 name is accepted");
	ct_check(fm_wait_posts(1, &p1), "...and posted");
	st = do_enq_counted(&proc2, n2, len, LCK_K_EXMODE, 0, &lkid2);
	ct_check(st == SS__NORMAL && lkid2 != 0 && lkid2 != lkid1,
		 "*** $ENQ on the VAX1DATA name is a SECOND lock, not the same "
		 "resource ***");
	ct_check(fm_wait_posts(2, &p2), "...and posted on its own");

	/* (3) The post carries the COUNT and every byte, read off the RSB. */
	ct_check_eq_u32(p2.resnam_len, 26u,
			"*** the post states the name's 26-byte COUNT ***");
	ct_check(memcmp(p2.resnam, n2, 26) == 0,
		 "*** ...and carries all 26 bytes, NULs included ***");
	ct_check(p1.dir_hash_known && p2.dir_hash_known &&
		 p1.dir_hash != p2.dir_hash,
		 "*** and the two identities have DIFFERENT directory hashes: "
		 "the truncation had one ***");

	/* (4) $GETLKI reads the whole name back. */
	st = do_getlki(&proc1, lkid1, &gk);
	ct_check(st == SS__NORMAL, "$GETLKI on the first lock");
	ct_check_eq_u32(gk.resnam_len, 26u,
			"*** LKI$_RESNAM reads back 26 bytes ***");
	ct_check(memcmp(gk.resnam, n1, 26) == 0,
		 "*** ...byte-for-byte the name that was enqueued ***");

	/* (5) The refusals at the boundary. */
	ct_check_eq_u32(do_enq_counted(&proc1, n1, 32u, LCK_K_EXMODE, 0, NULL),
			SS__IVBUFLEN,
			"a 32-byte count is SS$_IVBUFLEN");
	ct_check_eq_u32(do_enq_counted(&proc1, n1, 99u, LCK_K_EXMODE, 0, NULL),
			SS__IVBUFLEN,
			"...and so is one past the field");

	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();

	/*
	 * (2) THE NON-VACUITY CONTROL, on a node that masters its own
	 * resources: the same 26-byte name from a second process at EX with
	 * NOQUEUE must be REFUSED. If the counted key had made every name
	 * unique -- the opposite error -- this would be granted too.
	 */
	configure_local_only();
	fm_init();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	fm_install();
	proc_init(&proc1);
	proc_init(&proc2);

	st = do_enq_counted(&proc1, n1, len, LCK_K_EXMODE, 0, &lkid1);
	ct_check(st == SS__NORMAL, "the SYSDSK1 name is granted locally at EX");
	st = do_enq_counted(&proc2, n2, len, LCK_K_EXMODE, LCK_M_NOQUEUE,
			    &lkid2);
	ct_check(st == SS__NORMAL && lkid2 != 0,
		 "*** the VAX1DATA name is granted at EX at the same time: two "
		 "files, two resources ***");
	st = do_enq_counted(&proc2, n1, len, LCK_K_EXMODE, LCK_M_NOQUEUE,
			    &lkid3);
	ct_check_eq_u32(st, SS__NOTQUEUED,
			"*** and the SAME name is REFUSED -- the resources do "
			"conflict (the control that makes the two grants above "
			"mean something) ***");

	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

int main(void)
{
	printf("=== test_lock_proxy (FC-P4.4 proxy LKB, real engine, R1 host unit) ===\n");
	proxy_lifecycle();
	proxy_sync_wait();
	proxy_blkast();
	proxy_idempotent_and_lkid_unset();
	master_idempotent_retransmit();
	no_cluster_arm_refuses();
	inbound_redirect();
	e09_master_removes_its_directory_entry();
	e09_no_removal_without_a_remote_entry();
	c55_no_removal_for_an_unregistered_entry();
	rms326_counted_binary_names();
	return ct_summary("test_lock_proxy");
}
