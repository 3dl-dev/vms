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
 * HOW "REMOTE" IS ARRANGED WITHOUT FABRICATING A CLUSTER. The membership vector
 * is a real, controlled executive input (dlm_member_csids/dlm_member_count, the
 * same one the module takes at load). This test configures a genuine and
 * entirely legitimate VMS configuration: ONE directory-participating member,
 * CSID 2, which is NOT this node (CSID 1) -- a satellite with LOCKDIRWT 0, the
 * configuration design D-DLM-1 names. Every root name then resolves to a
 * directory node that is not us, deterministically, WITHOUT depending on the
 * value of any hash (which FC-P4.3 is still to settle, Rule 8).
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
uint32_t dlm_member_csids[VMS_DLM_MAX_MEMBERS] = { 1 };
int      dlm_member_count = 1;

#define CSID_LOCAL     1u    /* this node                                    */
#define CSID_DIRECTORY 2u    /* the only directory-participating member       */
#define CSID_MASTER    3u    /* the node the directory names as tree master   */

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

static void fm_install(void)
{
	struct vms_dlm_requester_ops ops;

	ops.post = fm_post;
	ops.ctx = &fm;
	vms_lock_dlm_set_requester_ops(&ops);
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
	dlm_member_csids[0] = CSID_DIRECTORY;
	dlm_member_count = 1;
}

static void configure_local_only(void)
{
	vms_local_csid = CSID_LOCAL;
	dlm_member_csids[0] = CSID_LOCAL;
	dlm_member_count = 1;
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
	vms_lock_dlm_set_requester_ops(NULL);
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

int main(void)
{
	printf("=== test_lock_proxy (FC-P4.4 proxy LKB, real engine, R1 host unit) ===\n");
	proxy_lifecycle();
	proxy_sync_wait();
	proxy_blkast();
	proxy_idempotent_and_lkid_unset();
	master_idempotent_retransmit();
	no_cluster_arm_refuses();
	return ct_summary("test_lock_proxy");
}
