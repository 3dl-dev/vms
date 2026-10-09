// SPDX-License-Identifier: GPL-2.0
/*
 * test_lock_host.c - R1 host-unit proof that src/kernel-core/vms_lock.c (the
 * REAL lock engine, unmodified) compiles, links, and runs on a plain host
 * compiler against the FC-P4.9 host backend (rd FC-P4.9,
 * docs/plan-faithful-cluster-executive.md P4; design sec 3.9's test-ladder
 * rung 1).
 *
 * PORTED SEMANTIC (done-condition: "existing lock unit semantics reproduced
 * on one host test"): the same three assertions tests/qemu/test_syssvc_lock.c
 * makes over the real /dev/vms ioctl surface, minus the fork()/pipe()
 * cross-process plumbing that program needs ONLY because it drives a running
 * kernel module from two Linux processes. Here, "two processes" are simply
 * two `struct vms_proc` instances in one host binary -- vms_lock.c's own
 * cross-process behaviour is entirely a matter of which `struct vms_proc *`
 * a call names, so this is a faithful, more direct exercise of the SAME
 * engine code, calling its real entry points (vms_ioctl_enq/vms_ioctl_deq)
 * exactly as the kernel ioctl dispatcher does:
 *
 *   1. proc_a's $ENQ EX is granted with a real, nonzero lock ID.
 *   2. proc_b's $ENQ EX+NOQUEUE, and CR+NOQUEUE, are BOTH denied
 *      (SS$_NOTQUEUED) while proc_a holds EX (EX and CR are each
 *      incompatible with a granted EX -- the compat[] matrix).
 *   3. proc_a's $DEQ releases; proc_b's *synchronous* ($ENQW-equivalent,
 *      LCK_M_SYNC) EX request -- already blocked in-kernel via a REAL
 *      pthread_cond_timedwait on vms_lock.c's enq_wait_sync -- wakes and is
 *      granted once proc_a releases, proving the cv contract this host
 *      backend implements (exec_cv_wait_timeout / exec_cv_broadcast) is
 *      lost-wakeup-free end to end, not just type-correct.
 *
 * A second routine (lock_stress) drives several hundred $ENQ/$DEQ cycles
 * across many distinctly-named resources -- exercising the hand-rolled
 * exec_rbtree_host.h (the lock-ID database) and exec_hash_host.h (the
 * resource database) under real churn, not just a single insert/erase pair.
 */

/* pthread_timedjoin_np (rd vms-f87: the deadline that turns a kernel spin into
 * a named failed assertion) is a GNU extension, so this has to come first. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h -> lock_host_internal.h */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h -> exec_kbackend_host.h */
#include "vms_dlm_master.h"   /* the MASTER-side door the DLM's wire arm uses */

#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ================================================================
 * Real, non-fabricated globals vms_lock.c reads (see lock_host_internal.h's
 * comment on these externs): a genuine one-node "cluster" -- CSID 1 is the
 * only member, and it is this node. No cross-node behaviour is exercised by
 * this host test (that is FC-P5's mastering/directory work); INV-6: nothing
 * here claims a member that is not configured.
 * ================================================================ */
uint32_t vms_local_csid = 1;

/*
 * vms_ast_notify_arrival - link-time stub (see lock_host_internal.h's
 * comment on this prototype). Every $ENQ this test issues either supplies no
 * astadr/blkastadr or sets LCK_M_SYNC (which queue_completion_ast's own guard
 * skips unconditionally), so vms_lock.c never actually calls this at
 * runtime; it exists only so the object built from vms_lock.c links.
 */
void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

/* ---- test-harness process setup (mirrors what vms_proctab.c would do on
 * process registration in the real kernel -- out of scope for a lock-
 * manager-only host build, so this test does it directly). ---- */
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
		       uint32_t lkmode, uint32_t flags, uint32_t *lkid_out)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = lkmode;
	a.flags = flags;
	strscpy(a.resnam, resnam, sizeof(a.resnam));
	vms_ioctl_enq(proc, (unsigned long)(void *)&a);
	if (lkid_out)
		*lkid_out = a.lkid;
	return a.status;
}

static uint32_t do_deq(struct vms_proc *proc, uint32_t lkid)
{
	struct vms_deq_args d;

	(void)proc;
	memset(&d, 0, sizeof(d));
	d.lkid = lkid;
	vms_ioctl_deq(proc, (unsigned long)(void *)&d);
	return d.status;
}

/* ---- the blocking ($ENQW-equivalent) request runs on a real pthread, so
 * the wait is genuinely concurrent with the releasing DEQ below -- the same
 * shape test_syssvc_lock.c's forked child exercises, minus the IPC. ---- */
struct sync_enq_result {
	struct vms_proc *proc;
	const char       *resnam;
	uint32_t          status;
	uint32_t          lkid;
};

static void *sync_enq_thread(void *arg)
{
	struct sync_enq_result *r = arg;

	r->status = do_enq(r->proc, r->resnam, LCK_K_EXMODE, LCK_M_SYNC, &r->lkid);
	return NULL;
}

static void lock_basic(void)
{
	struct vms_proc proc_a, proc_b;
	uint32_t lkid_a = 0, status;
	pthread_t th;
	struct sync_enq_result sr;

	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}

	proc_init(&proc_a);
	proc_init(&proc_b);

	/* 1. proc_a: EX granted, real lock ID. */
	status = do_enq(&proc_a, "LOCK_HOST_TEST1", LCK_K_EXMODE, 0, &lkid_a);
	ct_check(status == SS__NORMAL && lkid_a != 0,
		 "proc_a: EX granted, real lock ID (vms_ioctl_enq, real engine)");

	/* 2a. proc_b: EX+NOQUEUE denied while proc_a holds EX. */
	status = do_enq(&proc_b, "LOCK_HOST_TEST1", LCK_K_EXMODE, LCK_M_NOQUEUE, NULL);
	ct_check(status == SS__NOTQUEUED,
		 "proc_b: EX+NOQUEUE denied while proc_a holds EX");

	/* 2b. proc_b: CR+NOQUEUE also denied (CR incompatible with granted EX). */
	status = do_enq(&proc_b, "LOCK_HOST_TEST1", LCK_K_CRMODE, LCK_M_NOQUEUE, NULL);
	ct_check(status == SS__NOTQUEUED,
		 "proc_b: CR+NOQUEUE denied while proc_a holds EX");

	/* 3. proc_b issues a SYNCHRONOUS EX request -- it blocks for real,
	 * in vms_lock.c's enq_wait_sync, on this host backend's
	 * exec_cv_wait_timeout. Start it on its own thread. */
	sr.proc = &proc_b;
	sr.resnam = "LOCK_HOST_TEST1";
	sr.status = 0;
	sr.lkid = 0;
	if (pthread_create(&th, NULL, sync_enq_thread, &sr) != 0) {
		ct_check(0, "pthread_create for the synchronous $ENQW-equivalent");
		return;
	}

	/* Give the blocking request time to actually reach res->waiting and
	 * enter its cv wait before proc_a releases -- otherwise this proves
	 * nothing about the wait path (it would just be an ordinary grant on
	 * an already-free resource). 20ms is generous next to the
	 * microsecond-scale lock/hash/list ops on either side. */
	usleep(20 * 1000);

	/* Release proc_a's EX -- this is the real wakeup: try_grant_waiters
	 * (called from vms_deq_core) sets grant_state and
	 * exec_cv_broadcasts proc_b's wait_wq under res->lock. */
	status = do_deq(&proc_a, lkid_a);
	ct_check(status == SS__NORMAL, "proc_a: $DEQ released EX (real engine)");

	pthread_join(th, NULL);
	ct_check(sr.status == SS__NORMAL && sr.lkid != 0,
		 "proc_b: synchronous EX granted after proc_a's $DEQ "
		 "(real pthread_cond wait/wake through vms_lock.c's enq_wait_sync)");

	if (sr.status == SS__NORMAL) {
		status = do_deq(&proc_b, sr.lkid);
		ct_check(status == SS__NORMAL, "proc_b: $DEQ released its EX");
	}

	vms_lock_cleanup();
}

/* ---- stress: many resources, many ENQ/DEQ cycles -- exercises the
 * hand-rolled rbtree (lock-ID database) and hash (resource database) under
 * real churn, not just one insert/erase pair. ---- */
#define STRESS_RESOURCES 16
#define STRESS_ITERS     64

static void lock_stress(void)
{
	struct vms_proc proc;
	char resnam[32];
	int i, iter, ok = 1;

	if (vms_lock_init() != 0) {
		ct_check(0, "lock_stress: vms_lock_init");
		return;
	}
	proc_init(&proc);

	for (iter = 0; iter < STRESS_ITERS && ok; iter++) {
		uint32_t lkids[STRESS_RESOURCES];

		for (i = 0; i < STRESS_RESOURCES; i++) {
			uint32_t status;

			snprintf(resnam, sizeof(resnam), "STRESS_RES_%d", i);
			status = do_enq(&proc, resnam, LCK_K_EXMODE, 0, &lkids[i]);
			if (status != SS__NORMAL || lkids[i] == 0) {
				ok = 0;
				break;
			}
		}
		/* Release in reverse order, so the rbtree/hash see a mixed
		 * insert/erase pattern rather than a strict LIFO/FIFO one. */
		for (i = STRESS_RESOURCES - 1; ok && i >= 0; i--) {
			if (do_deq(&proc, lkids[i]) != SS__NORMAL) {
				ok = 0;
				break;
			}
		}
		if (ok && proc.lock_count != 0)
			ok = 0;
	}

	ct_check(ok, "lock_stress: 64 iterations x 16 resources, real "
		     "$ENQ/$DEQ, rbtree+hash intact (lock_count back to 0 "
		     "every iteration)");

	vms_lock_cleanup();
}

/* ================================================================
 * rd vms-c27 CONDITION 1 -- "the delivery proc is the OWNER, not the MODE
 * SOURCE". The cross-node DLM receive path serves a peer's $ENQ on the
 * DELIVERY PROC (the process that issued VMS_IOCTL_CLUSTER_START). If the
 * resulting master-side LKB took that process's current_mode, a local image
 * rundown on THIS node would release a lock ANOTHER NODE still holds -- the
 * master would silently drop a grant it had already acknowledged on the wire.
 *
 * These are the TEETH of that binding, driven through the real engine:
 *   - the delivery proc is put at PSL_C_USER (the worst case: exactly the mode
 *     an inherited acmode would have picked up),
 *   - it holds one genuinely LOCAL USER-mode lock AND one lock created for a
 *     REMOTE requester (vms_lock_dlm_xnode_dispatch, req_csid set),
 *   - image rundown runs on it (PSL_C_USER, what vms_access.c passes),
 *   - the LOCAL lock MUST be gone (proving rundown really ran and really does
 *     release USER-mode locks on this very process -- the discriminator),
 *   - the REMOTE-held lock MUST survive, still held for its peer's CSID, and
 *     the peer's own cross-node $DEQ must still release it.
 * ================================================================ */
#define C27_REMOTE_CSID 0x00020005u
#define C27_REMOTE_LKID 0x0000beefu

/* The granted mode of a lock, straight off $GETLKI. */
static uint32_t lki_mode(uint32_t lkid)
{
	struct vms_getlki_args a;

	memset(&a, 0, sizeof(a));
	a.lkid = lkid;
	vms_ioctl_getlki(NULL, (unsigned long)(void *)&a);
	return a.status == SS__NORMAL ? a.granted_mode : 0xffffffffu;
}

static uint32_t do_resmaster(struct vms_proc *proc, const char *resnam,
			     struct vms_resmaster_args *rm)
{
	memset(rm, 0, sizeof(*rm));
	strscpy(rm->resnam, resnam, sizeof(rm->resnam));
	vms_ioctl_get_resmaster(proc, (unsigned long)(void *)rm);
	return rm->status;
}

static uint32_t xnode_enq(struct vms_proc *delivery, const char *resnam,
			  uint32_t *master_lkid_out)
{
	struct vms_dlm_xnode_args req;
	uint32_t st;

	memset(&req, 0, sizeof(req));
	req.op = VMS_DLM_OP_ENQ;
	req.lkmode = LCK_K_EXMODE;
	req.req_csid = C27_REMOTE_CSID;
	req.req_lkid = C27_REMOTE_LKID;
	strscpy(req.resnam, resnam, sizeof(req.resnam));
	st = vms_lock_dlm_xnode_dispatch(delivery, &req);
	*master_lkid_out = req.master_lkid;
	return st;
}

static uint32_t xnode_deq(struct vms_proc *delivery, const char *resnam,
			  uint32_t master_lkid)
{
	struct vms_dlm_xnode_args req;

	memset(&req, 0, sizeof(req));
	req.op = VMS_DLM_OP_DEQ;
	req.req_csid = C27_REMOTE_CSID;
	req.req_lkid = C27_REMOTE_LKID;
	req.master_lkid = master_lkid;
	strscpy(req.resnam, resnam, sizeof(req.resnam));
	return vms_lock_dlm_xnode_dispatch(delivery, &req);
}

static void remote_lkb_is_outside_image_rundown(void)
{
	struct vms_proc delivery;
	struct vms_resmaster_args rm;
	uint32_t local_lkid = 0, master_lkid = 0, status;

	if (vms_lock_init() != 0) {
		ct_check(0, "vms-c27 cond.1: vms_lock_init");
		return;
	}
	proc_init(&delivery);

	/* The delivery proc is running an image at USER mode when the peer's
	 * request arrives. Nothing about that may reach the remote LKB. */
	delivery.current_mode = PSL_C_USER;

	status = do_enq(&delivery, "C27_LOCAL_RES", LCK_K_EXMODE, 0, &local_lkid);
	ct_check(status == SS__NORMAL && local_lkid != 0,
		 "vms-c27 cond.1: delivery proc holds a LOCAL USER-mode lock");

	status = xnode_enq(&delivery, "C27_REMOTE_RES", &master_lkid);
	ct_check(status == SS__NORMAL && master_lkid != 0,
		 "vms-c27 cond.1: cross-node $ENQ granted on the delivery proc "
		 "(real vms_lock_dlm_xnode_dispatch)");

	status = do_resmaster(&delivery, "C27_REMOTE_RES", &rm);
	ct_check(status == SS__NORMAL && rm.found &&
		 rm.remote_holder_csid == C27_REMOTE_CSID,
		 "vms-c27 cond.2: the master's lock record names the REMOTE "
		 "requester's CSID (GET_RESMASTER readback, not a fabrication)");

	/* Image rundown on the delivery proc -- exactly what vms_access.c does
	 * when an image on this process runs down. */
	vms_proc_rundown_locks(&delivery, PSL_C_USER);

	/* Discriminator: rundown really ran, and really does release the
	 * USER-mode locks of THIS process. */
	ct_check(do_deq(&delivery, local_lkid) == SS__IVLOCKID,
		 "vms-c27 cond.1 DISCRIMINATOR: image rundown DID release the "
		 "delivery proc's own USER-mode lock");

	/* Teeth: the lock held for a peer is NOT in that scope. */
	status = do_resmaster(&delivery, "C27_REMOTE_RES", &rm);
	ct_check(status == SS__NORMAL && rm.found && rm.n_granted == 1 &&
		 rm.remote_holder_csid == C27_REMOTE_CSID,
		 "vms-c27 cond.1 TEETH: the REMOTE-held LKB SURVIVED image "
		 "rundown, still granted and still held for the peer's CSID");

	/* And it is still a live lock, releasable only by its real owner's
	 * cross-node $DEQ (vms-4d3 will add the per-CSID departure path). */
	ct_check(xnode_deq(&delivery, "C27_REMOTE_RES", master_lkid) == SS__NORMAL,
		 "vms-c27 cond.1: the peer's own cross-node $DEQ releases it");

	status = do_resmaster(&delivery, "C27_REMOTE_RES", &rm);
	ct_check(status == SS__NORMAL && rm.n_granted == 0,
		 "vms-c27 cond.1: no grant remains after the peer's $DEQ");

	vms_lock_cleanup();
}

/* ================================================================
 * THE MASTER-SIDE DOOR (vms_dlm_master.h) -- what the DLM's wire arm actually
 * calls, driven against the REAL engine.
 *
 * The arm itself (src/kernel-core/vms_dlm_scs.c) is not host-linkable -- it
 * names exec_kbackend.h and the fork API, the same reason vms_cnxman.c is not,
 * and its wiring is proven by tests/cluster/host/test_dlm_scs_arm.c. What IS
 * host-testable, and is the half that decides what goes on the wire, is this
 * door: given a peer's request, what does the engine really do, and is what the
 * door reports READ OUT OF THE LOCK THAT RESULTED?
 * ================================================================ */
#define MD_PEER_A   0x00010002u
#define MD_PEER_B   0x00010003u
#define MD_LKID_A   0x0000a1a1u
#define MD_LKID_B   0x0000b2b2u
#define MD_PEER_GROUP 1u          /* the requester's UIC group, off its frame */
#define MD_PEER_MODE  3u          /* ... and its access mode (user)           */

static void md_fill(struct vms_dlm_master_request *r, uint32_t op,
		    uint32_t csid, uint32_t lkid, uint32_t lkmode,
		    uint32_t flags, const char *resnam)
{
	memset(r, 0, sizeof(*r));
	r->op = op;
	r->req_csid = csid;
	r->req_lkid = lkid;
	r->lkmode = lkmode;
	r->flags = flags;
	strscpy(r->resnam, resnam, sizeof(r->resnam));
	/*
	 * ...and WHICH resource of that name (rd vms-b5b0): the identity the
	 * requester's own frame carried at body[44:46]/body[46], which the wire
	 * arm reads through the codec and this door refuses to serve without.
	 * MD_PEER_GROUP/MD_PEER_MODE stand for one real VMS requester's domain --
	 * a group-qualified user-mode name, the commonest shape on a real wire.
	 */
	r->res_group = MD_PEER_GROUP;
	r->res_mode = MD_PEER_MODE;
	r->res_ident_valid = 1u;
}

/* CONDITION 4 (rd vms-c27): no delivery proc, no service -- and, decisively,
 * NO LOCK. A master-side LKB must be owned by a real process; refusing is the
 * honest floor, and a refusal that had quietly created lock state anyway would
 * be the worst of both. */
static void master_door_refuses_without_a_delivery_proc(void)
{
	struct vms_dlm_master_request r;
	struct vms_dlm_master_result res;
	struct vms_resmaster_args rm;
	struct vms_proc probe;

	if (vms_lock_init() != 0) {
		ct_check(0, "master door: vms_lock_init");
		return;
	}
	proc_init(&probe);
	vms_lock_dlm_set_delivery_proc(NULL);

	ct_check(vms_lock_dlm_have_delivery_proc() == 0,
		 "vms-c27 cond.4: no delivery proc is registered");

	md_fill(&r, VMS_DLM_MREQ_ENQ, MD_PEER_A, MD_LKID_A, LCK_K_EXMODE, 0,
		"MD_NOPROC_RES");
	ct_check(vms_lock_dlm_master_serve(&r, &res) == SS__NORMAL &&
		 res.outcome == (uint8_t)VMS_DLM_MASTER_REFUSED,
		 "vms-c27 cond.4: a peer's $ENQ is REFUSED with no delivery proc");

	ct_check(do_resmaster(&probe, "MD_NOPROC_RES", &rm) == SS__NORMAL &&
		 rm.found == 0u,
		 "vms-c27 cond.4: ... and NO lock state was created for it");

	vms_lock_cleanup();
}

/* The four outcomes the arm turns into wire shapes, each from a real lock. */
static void master_door_reports_what_the_engine_did(void)
{
	struct vms_dlm_master_request r;
	struct vms_dlm_master_result granted, denied, queued, released;
	struct vms_proc delivery;
	struct vms_resmaster_args rm;

	if (vms_lock_init() != 0) {
		ct_check(0, "master door: vms_lock_init");
		return;
	}
	proc_init(&delivery);
	delivery.current_mode = PSL_C_USER;
	vms_lock_dlm_set_delivery_proc(&delivery);

	/* GRANTED. */
	md_fill(&r, VMS_DLM_MREQ_ENQ, MD_PEER_A, MD_LKID_A, LCK_K_EXMODE, 0,
		"MD_RES");
	ct_check(vms_lock_dlm_master_serve(&r, &granted) == SS__NORMAL &&
		 granted.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED &&
		 granted.master_lkid != 0u,
		 "master door: a compatible cross-node $ENQ is GRANTED with a "
		 "real master handle");
	ct_check(granted.req_lkid == MD_LKID_A,
		 "master door: the requester handle the grant reply carries is "
		 "READ BACK off the LKB the engine stamped (INV-6), not echoed");
	ct_check(granted.granted_mode == (uint8_t)LCK_K_EXMODE,
		 "master door: the granted MODE is read off that LKB too");
	ct_check(do_resmaster(&delivery, "MD_RES", &rm) == SS__NORMAL &&
		 rm.remote_holder_csid == MD_PEER_A,
		 "master door: the lock database names the REMOTE holder's CSID");

	/* DENIED -- a second peer, NOQUEUE, incompatible. */
	md_fill(&r, VMS_DLM_MREQ_ENQ, MD_PEER_B, MD_LKID_B, LCK_K_EXMODE,
		LCK_M_NOQUEUE, "MD_RES");
	ct_check(vms_lock_dlm_master_serve(&r, &denied) == SS__NORMAL &&
		 denied.outcome == (uint8_t)VMS_DLM_MASTER_DENIED &&
		 denied.master_lkid == 0u,
		 "master door: NOQUEUE + incompatible is DENIED, and names no "
		 "lock handle -- because this node holds no lock for it");

	/* QUEUED -- the same request without NOQUEUE is a REAL lock on a REAL
	 * waiting queue, and it names the holder that blocks it. */
	md_fill(&r, VMS_DLM_MREQ_ENQ, MD_PEER_B, MD_LKID_B, LCK_K_EXMODE, 0,
		"MD_RES");
	ct_check(vms_lock_dlm_master_serve(&r, &queued) == SS__NORMAL &&
		 queued.outcome == (uint8_t)VMS_DLM_MASTER_QUEUED &&
		 queued.master_lkid != 0u,
		 "master door: an incompatible cross-node $ENQ is QUEUED on the "
		 "master's real waiting queue");
	ct_check(queued.blocking_csid == MD_PEER_A &&
		 queued.blocking_master_lkid == granted.master_lkid &&
		 queued.blocking_req_lkid == MD_LKID_A,
		 "master door: ... and names the REMOTE holder that blocks it "
		 "(the BLKAST target), read off the blocking LKB");

	/* CROSS-NODE AUTHORIZATION IS BY CLUSTER IDENTITY. Peer B may not
	 * release a lock the master holds for peer A. */
	md_fill(&r, VMS_DLM_MREQ_DEQ, MD_PEER_B, MD_LKID_B, 0, 0, "MD_RES");
	r.master_lkid = granted.master_lkid;
	ct_check(vms_lock_dlm_master_serve(&r, &released) == SS__NORMAL &&
		 released.outcome == (uint8_t)VMS_DLM_MASTER_REFUSED,
		 "master door: a peer may NOT release a lock held for another "
		 "node's CSID");

	/* RELEASED, by its real owner -- and the release FLIPS the queued
	 * waiter, which is the deferred grant the master would owe it. */
	md_fill(&r, VMS_DLM_MREQ_DEQ, MD_PEER_A, MD_LKID_A, 0, 0, "MD_RES");
	r.master_lkid = granted.master_lkid;
	ct_check(vms_lock_dlm_master_serve(&r, &released) == SS__NORMAL &&
		 released.outcome == (uint8_t)VMS_DLM_MASTER_RELEASED,
		 "master door: the holder's own release is RELEASED");
	ct_check(released.deferred_grant == 1u &&
		 released.deferred_csid == MD_PEER_B &&
		 released.deferred_req_lkid == MD_LKID_B &&
		 released.deferred_master_lkid == queued.master_lkid &&
		 released.deferred_mode == (uint8_t)LCK_K_EXMODE,
		 "master door: ... and it FLIPPED the queued cross-node waiter "
		 "to granted, naming it for the deferred GRANT");

	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_cleanup();
}

/* ================================================================
 * vms-04f (DLM rung H11): a concurrent BOTH-INITIATE deadlock search aborts
 * the victim EXACTLY ONCE.
 *
 * When a cross-node deadlock cycle is detected, the detecting node names the
 * global-min victim (req_csid, req_lkid) and sends the VICTIM leg to the node
 * that MASTERS the victim's queued request. In a concurrent both-initiate
 * race the reference lab could not stage, node A and node B independently
 * close the SAME cycle and BOTH send a VICTIM leg naming the SAME victim. The
 * mastering node must abort that queued request ONCE (one SS$_DEADLOCK) and
 * treat the second VICTIM as an idempotent no-op (SS$_NORMAL) -- never a
 * double-abort, never two SS$_DEADLOCKs, never a fabricated abort of a lock
 * that is already gone. Driven through the REAL engine
 * (vms_lock_dlm_xnode_dispatch) against a REAL queued cross-node waiter (a
 * conflicting inbound $ENQ), not a stub.
 * ================================================================ */
static uint32_t xnode_victim(struct vms_proc *delivery, uint32_t victim_csid,
			     uint32_t victim_lkid, uint32_t *queued_out)
{
	struct vms_dlm_xnode_args req;
	uint32_t st;

	memset(&req, 0, sizeof(req));
	req.op = VMS_DLM_OP_DLKSRCH;
	req.flags = VMS_DLM_DLK_VICTIM;
	req.req_csid = victim_csid;
	req.req_lkid = victim_lkid;
	st = vms_lock_dlm_xnode_dispatch(delivery, &req);
	*queued_out = req.queued;
	return st;
}

static void dlksrch_both_initiate_aborts_once(void)
{
	struct vms_proc delivery;
	struct vms_dlm_xnode_args enq;
	uint32_t local_lkid = 0, queued = 0, st;

	printf("-- vms-04f (H11): concurrent both-initiate deadlock aborts the "
	       "victim EXACTLY once (one SS$_DEADLOCK, then an idempotent no-op)\n");

	if (vms_lock_init() != 0) {
		ct_check(0, "vms-04f: vms_lock_init");
		return;
	}
	proc_init(&delivery);

	/* This node masters DLK04FRES and holds it EX locally. */
	st = do_enq(&delivery, "DLK04FRES", LCK_K_EXMODE, 0, &local_lkid);
	ct_check(st == SS__NORMAL && local_lkid != 0,
		 "a local EX grant -- this node masters and holds the resource");

	/* A REMOTE node's conflicting EX $ENQ arrives: it cannot be granted (EX
	 * held locally), so the master QUEUES it -- a real cross-node waiter on
	 * res->waiting, held FOR the remote CSID. This is the victim-to-be. */
	memset(&enq, 0, sizeof(enq));
	enq.op = VMS_DLM_OP_ENQ;
	enq.lkmode = LCK_K_EXMODE;
	enq.req_csid = C27_REMOTE_CSID;
	enq.req_lkid = C27_REMOTE_LKID;
	strscpy(enq.resnam, "DLK04FRES", sizeof(enq.resnam));
	st = vms_lock_dlm_xnode_dispatch(&delivery, &enq);
	ct_check(st == (uint32_t)VMS_DLM_STS_QUEUED && enq.queued == 1u,
		 "the conflicting cross-node $ENQ QUEUES -- a real waiter parked "
		 "for the remote CSID (the deadlock victim-to-be)");

	/* FIRST VICTIM leg (node A's search closed the cycle and named this
	 * victim): the master aborts the queued waiter this call. */
	st = xnode_victim(&delivery, C27_REMOTE_CSID, C27_REMOTE_LKID, &queued);
	ct_check(st == SS__DEADLOCK && queued == 1u,
		 "FIRST VICTIM: the queued waiter is aborted this call "
		 "(SS$_DEADLOCK, queued=1) -- a REAL waiter removed, not a stub");

	/* SECOND VICTIM leg (the CONCURRENT search node B initiated agrees on the
	 * same victim and sends it too): the waiter is already gone, so this is an
	 * IDEMPOTENT no-op -- NOT a second SS$_DEADLOCK, NOT a fabricated abort.
	 * This is the whole H11 both-initiate invariant: aborted EXACTLY once. */
	queued = 0xffu;
	st = xnode_victim(&delivery, C27_REMOTE_CSID, C27_REMOTE_LKID, &queued);
	ct_check(st == SS__NORMAL && queued == 0u,
		 "SECOND (concurrent B-initiated) VICTIM: idempotent no-op "
		 "(SS$_NORMAL, queued=0) -- the victim was aborted EXACTLY once");
}

/* ================================================================
 * THE RESOURCE NAMESPACE IS QUALIFIED (rd vms-b5b0)
 *
 * $ENQ: two requests name the same resource only if they agree on the name,
 * the parent, the access mode AND the UIC group -- unless LCK$M_SYSTEM makes
 * the name system-wide, which puts it in group 0. Before this item the engine
 * keyed on the name alone, so two UIC groups shared one resource: a
 * fabrication in the harmless direction locally (one lock too few) and the
 * reason a resource block could not state its own wire identity at all.
 * ================================================================ */
static void the_namespace_is_qualified(void)
{
	struct vms_proc g1, g2;
	uint32_t lkid1 = 0, lkid2 = 0;
	struct vms_resmaster_args rm;

	if (vms_lock_init() != 0) {
		ct_check(0, "namespace: vms_lock_init");
		return;
	}
	printf("-- the resource namespace is qualified by UIC group and mode\n");
	proc_init(&g1);
	proc_init(&g2);
	g1.uic = (11u << 16) | 4u;        /* [11,4] */
	g2.uic = (22u << 16) | 4u;        /* [22,4] */

	/* TWO GROUPS, ONE NAME: two resources, so BOTH get EX. */
	ct_check(do_enq(&g1, "QUALNAME", LCK_K_EXMODE, 0, &lkid1) == SS__NORMAL,
		 "group 11 takes EX on QUALNAME");
	ct_check(do_enq(&g2, "QUALNAME", LCK_K_EXMODE, LCK_M_NOQUEUE,
			&lkid2) == SS__NORMAL,
		 "group 22 takes EX on the SAME NAME -- a different resource, "
		 "so it is not blocked (the VMS namespace, $ENQ)");

	/* ONE GROUP, ONE NAME, TWO ACCESS MODES: also two resources. */
	g1.current_mode = PSL_C_SUPER;
	ct_check(do_enq(&g1, "QUALNAME", LCK_K_EXMODE, LCK_M_NOQUEUE,
			NULL) == SS__NORMAL,
		 "the same process at a different ACCESS MODE names a "
		 "different resource too");
	g1.current_mode = PSL_C_KERNEL;

	/* LCK$M_SYSTEM puts both groups in group 0: ONE resource, so the
	 * second request IS blocked. */
	g1.cur_privs = VMS_PRV_M_SYSLCK;
	g2.cur_privs = VMS_PRV_M_SYSLCK;
	ct_check(do_enq(&g1, "SYSQUAL", LCK_K_EXMODE, LCK_M_SYSTEM,
			NULL) == SS__NORMAL,
		 "group 11 takes EX on a SYSTEM-WIDE name");
	ct_check(do_enq(&g2, "SYSQUAL", LCK_K_EXMODE,
			LCK_M_SYSTEM | LCK_M_NOQUEUE, NULL) == SS__NOTQUEUED,
		 "group 22's LCK$M_SYSTEM request on that name is BLOCKED -- "
		 "a system-wide name is one resource, in group 0");

	/* And the blocks really exist, one per domain. */
	ct_check(do_resmaster(&g1, "QUALNAME", &rm) == SS__NORMAL &&
		 rm.found == 1u,
		 "a resource block of that name exists (the readback reports "
		 "the first domain's, which is all a name-only readback can)");

	vms_lock_cleanup();
}

/* A master-side request that does not say WHICH resource of that name it means
 * is refused, and creates nothing (rd vms-b5b0). The wire arm reads the
 * identity off the frame; a caller that holds none has no honest default. */
static void master_door_refuses_an_unstated_identity(void)
{
	struct vms_dlm_master_request r;
	struct vms_dlm_master_result res;
	struct vms_resmaster_args rm;
	struct vms_proc delivery, probe;

	if (vms_lock_init() != 0) {
		ct_check(0, "master door: vms_lock_init");
		return;
	}
	printf("-- NEGATIVE: a cross-node $ENQ with no stated resource identity\n");
	proc_init(&delivery);
	proc_init(&probe);
	delivery.current_mode = PSL_C_USER;
	vms_lock_dlm_set_delivery_proc(&delivery);

	md_fill(&r, VMS_DLM_MREQ_ENQ, MD_PEER_A, MD_LKID_A, LCK_K_EXMODE, 0,
		"MD_NOIDENT");
	r.res_ident_valid = 0u;          /* the codec would not give us one */
	ct_check(vms_lock_dlm_master_serve(&r, &res) == SS__BADPARAM,
		 "a peer's $ENQ with no resource identity is REFUSED");
	ct_check(do_resmaster(&probe, "MD_NOIDENT", &rm) == SS__NORMAL &&
		 rm.found == 0u,
		 "... and NO lock state was created for it (INV-6)");

	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_cleanup();
}

/* ==========================================================================
 * THE ev6 LAB QUEUE SHAPE (rd vms-b5b0 follow-on, 2026-10-09)
 *
 * The configuration a booted OVMX master hung in: an OVMX process holds NL on a
 * resource it masters, a VAX holds EX, the LOCAL lock converts NL->EX and
 * queues, a second VAX takes NL and converts NL->EX behind it, and then the EX
 * holder DEQs over the wire. The lab saw an RCU self-detected stall on the fork
 * thread at the moment that DEQ arrived.
 *
 * WHAT THIS TEST ESTABLISHES, and it is a NEGATIVE as much as a positive: the
 * ENGINE serves that exact sequence through its own master door WITHOUT
 * spinning, and with the right answers at every step. So the stall is NOT in
 * the engine's convert/queue/release path for this shape -- which is worth
 * pinning down, because it is where one would look first. (The suite's 30 s
 * ctest timeout is the detector: if this ever hangs, the engine HAS acquired
 * the fault.)
 * ========================================================================== */
static void the_ev6_queue_shape_does_not_spin(void)
{
	struct vms_dlm_master_request r;
	struct vms_dlm_master_result out;
	struct vms_proc delivery, app;
	struct vms_enq_args cvt;
	uint32_t local_lkid = 0, vax1_master = 0;

	if (vms_lock_init() != 0) {
		ct_check(0, "ev6 shape: vms_lock_init");
		return;
	}
	printf("-- the ev6 lab queue shape: local NL converting + remote NL "
	       "converting, and the remote EX holder DEQs\n");
	proc_init(&delivery);
	proc_init(&app);
	delivery.current_mode = PSL_C_USER;
	app.current_mode = MD_PEER_MODE;
	app.uic = (MD_PEER_GROUP << 16);
	vms_lock_dlm_set_delivery_proc(&delivery);

	/* 1. The OVMX process takes NL, so this node masters the resource. */
	ct_check(do_enq(&app, "EVAC$WORKLOAD", LCK_K_NLMODE, 0,
			&local_lkid) == SS__NORMAL && local_lkid != 0u,
		 "an OVMX process takes NL and this node masters the resource");

	/* 2. VAX1 asks EX. NL conflicts with nothing, so it is granted. */
	md_fill(&r, VMS_DLM_MREQ_ENQ, MD_PEER_A, MD_LKID_A, LCK_K_EXMODE, 0,
		"EVAC$WORKLOAD");
	ct_check(vms_lock_dlm_master_serve(&r, &out) == SS__NORMAL &&
		 out.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED,
		 "VAX1's EX is GRANTED (NL conflicts with nothing)");
	vax1_master = out.master_lkid;

	/* 3. The local lock converts NL->EX. Incompatible with VAX1's EX, so it
	 *    QUEUES -- async, so the status is "accepted" and the mode stays NL
	 *    until a grant. */
	memset(&cvt, 0, sizeof(cvt));
	cvt.lkid = local_lkid;
	cvt.lkmode = LCK_K_EXMODE;
	vms_ioctl_convert(&app, (unsigned long)(void *)&cvt);
	ct_check_eq_u32(cvt.status, SS__NORMAL,
			"the local NL->EX convert is accepted");
	ct_check_eq_u32(cvt.lk_status, LCK_K_EXMODE,
			"  ... as a QUEUED request at the new mode (the async "
			"form: lk_status is what was ASKED for)");

	/* 4. VAX2 takes NL behind the EX holder -- compatible, granted. */
	md_fill(&r, VMS_DLM_MREQ_ENQ, MD_PEER_B, MD_LKID_B, LCK_K_NLMODE, 0,
		"EVAC$WORKLOAD");
	ct_check(vms_lock_dlm_master_serve(&r, &out) == SS__NORMAL &&
		 out.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED,
		 "VAX2's NL is GRANTED");

	/* 5. VAX2 converts NL->EX: queued behind the local convert. */
	md_fill(&r, VMS_DLM_MREQ_CONVERT, MD_PEER_B, MD_LKID_B, LCK_K_EXMODE,
		0, "EVAC$WORKLOAD");
	ct_check(vms_lock_dlm_master_serve(&r, &out) == SS__NORMAL &&
		 out.outcome == (uint8_t)VMS_DLM_MASTER_QUEUED,
		 "VAX2's NL->EX convert is QUEUED behind it");

	/* 6. THE MOMENT THE LAB HUNG IN: the remote EX holder releases. */
	md_fill(&r, VMS_DLM_MREQ_DEQ, MD_PEER_A, MD_LKID_A, 0, 0,
		"EVAC$WORKLOAD");
	r.master_lkid = vax1_master;
	ct_check(vms_lock_dlm_master_serve(&r, &out) == SS__NORMAL &&
		 out.outcome == (uint8_t)VMS_DLM_MASTER_RELEASED,
		 "*** VAX1's cross-node $DEQ is served and RELEASES -- the "
		 "engine does not spin on this queue shape ***");

	/* And it granted the right one: FIFO gives the LOCAL convert the EX, so
	 * VAX2's convert is still waiting and no deferred grant is owed to it. */
	ct_check_eq_u32(lki_mode(local_lkid), LCK_K_EXMODE,
			"the LOCAL convert is the one the release granted "
			"(FIFO), now held at EX");
	ct_check_eq_u32((unsigned long)out.deferred_grant, 0u,
			"and NO deferred grant is reported for VAX2, whose "
			"convert is still genuinely queued behind it");

	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_cleanup();
}

/*
 * A SIGNAL TO A PROCESS BLOCKED IN $ENQW ENDS THE IOCTL -- IT DOES NOT SPIN A
 * CPU (rd vms-f87, lab run ci6-evac-11).
 *
 * WHAT HAPPENED. An OVMX process converted NL->EX on a resource this node
 * mastered whose blocker was a remote VAX EX holder. The Linux backend does not
 * sleep while a signal is pending (it would be woken again at once), and
 * enq_wait_sync DROPPED that return: it re-tested a predicate that was still
 * false and called straight back in. The result was a tight loop taking and
 * dropping res->lock, a CPU that never left the kernel, and `rcu: INFO:
 * self-detected stall on CPU 0 (9931 ticks this GP)` growing to 98,763 ticks
 * while the fork thread on the other CPU served the cluster normally. Any
 * signal did it -- including the STOP sent to recover the process.
 *
 * WHAT THIS TEST DOES. The host backend has no signals, so it has an INTERRUPT
 * SEAM (exec_kbackend_host.h): `exec_host_interrupt_waits` makes the next N
 * waits report INTERRUPTED without sleeping, which is exactly the Linux
 * behaviour being modelled. With the fix, the ioctl returns -ERESTARTSYS with
 * NO status written, so userspace re-enters the wait (libvmssys'
 * KIF_WAIT_CALL) -- what VMS does when an AST interrupts a wait.
 *
 * AND THE TEST BOUNDS THE SPIN ITSELF rather than relying on a harness
 * timeout: each wait runs on its own thread and is JOINED WITH A DEADLINE, so
 * the regression shows up as a named FAILED ASSERTION in a few seconds. A test
 * for an infinite loop that hangs to prove it is a test no mutation gate can
 * measure.
 *
 * Both halves are asserted: the ENQW and the CONVERT, because they are two
 * separate waits in two separate ioctls and only one of them was in the lab's
 * stack.
 */
struct intr_call {
	struct vms_proc    *proc;
	struct vms_enq_args args;
	long                rc;
	int                 convert;
	int                 done;
};

static void *intr_call_thread(void *arg)
{
	struct intr_call *c = arg;

	c->rc = c->convert
		? vms_ioctl_convert(c->proc, (unsigned long)(void *)&c->args)
		: vms_ioctl_enq(c->proc, (unsigned long)(void *)&c->args);
	c->done = 1;
	return NULL;
}

/*
 * Run one ioctl on a thread and join it with a DEADLINE. Returns 1 when it came
 * back on its own -- 0 means it is still in the kernel, which IS the
 * regression.
 *
 * ON A MISS THE TEST STILL TERMINATES, deliberately: `unblock_lkid` (the
 * holder's lock) is released so the waiting thread completes and can be joined.
 * A test for a wait that never ends must not itself never end -- leaving a
 * detached thread inside the engine makes everything after it meaningless, and
 * a suite that proves its point by hanging is a suite no mutation gate can
 * measure.
 */
static int run_with_deadline(struct intr_call *c, unsigned int secs,
			     struct vms_proc *holder, uint32_t unblock_lkid)
{
	pthread_t th;
	struct timespec ts;

	c->done = 0;
	c->rc = 0;
	if (pthread_create(&th, NULL, intr_call_thread, c) != 0)
		return 0;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += (time_t)secs;
	if (pthread_timedjoin_np(th, NULL, &ts) == 0)
		return 1;

	/* It did not come back. Let it finish so this suite can. */
	if (holder != NULL && unblock_lkid != 0u)
		(void)do_deq(holder, unblock_lkid);
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += (time_t)secs;
	if (pthread_timedjoin_np(th, NULL, &ts) != 0)
		(void)pthread_detach(th);   /* nothing left to try */
	return 0;
}

static void a_signal_ends_the_wait_instead_of_spinning(void)
{
	struct vms_proc holder, waiter;
	struct intr_call c;
	struct vms_resmaster_args rm;
	uint32_t holder_lkid = 0, waiter_lkid = 0;

	if (vms_lock_init() != 0) {
		ct_check(0, "signal/wait: vms_lock_init");
		return;
	}
	printf("-- rd vms-f87: a signal ENDS a blocked $ENQW; it does not spin "
	       "a CPU --\n");
	proc_init(&holder);
	proc_init(&waiter);

	ct_check(do_enq(&holder, "F87_WAIT", LCK_K_EXMODE, 0,
			&holder_lkid) == SS__NORMAL && holder_lkid != 0u,
		 "a holder takes EX, so the next request must queue");

	/* --- the $ENQW --- */
	memset(&c, 0, sizeof(c));
	c.proc = &waiter;
	c.args.lkmode = LCK_K_EXMODE;
	c.args.flags = LCK_M_SYNC;
	strscpy(c.args.resnam, "F87_WAIT", sizeof(c.args.resnam));
	exec_host_interrupt_waits = 1u;     /* a signal is pending */
	ct_check(run_with_deadline(&c, 5u, &holder, holder_lkid) == 1,
		 "*** the interrupted $ENQW COMES BACK (it does not spin in "
		 "the kernel: the lab's CPU 0 never did) ***");
	ct_check_eq_u32(exec_host_interrupt_waits, 0u,
			"the wait really was entered and interrupted once");
	ct_check_eq_u32((unsigned long)(-c.rc), (unsigned long)ERESTARTSYS,
			"*** and returns -ERESTARTSYS ***");
	ct_check_eq_u32(c.args.status, 0u,
			"*** writing NO status: $ENQW has no 'your wait was "
			"interrupted' condition value, so userspace re-enters "
			"the wait and no caller can observe this ***");

	/* The request is STILL QUEUED, which is what makes re-entering correct:
	 * the lock the caller asked for has not been lost or granted. */
	ct_check(do_resmaster(&waiter, "F87_WAIT", &rm) == SS__NORMAL &&
		 rm.found == 1u && rm.n_granted == 1u,
		 "the resource still has exactly the holder's grant -- the "
		 "interrupted request neither vanished nor was granted");

	/* --- the same for a CONVERT, the path the lab was actually in --- */
	ct_check(do_enq(&waiter, "F87_CVT", LCK_K_NLMODE, 0,
			&waiter_lkid) == SS__NORMAL && waiter_lkid != 0u,
		 "the waiter takes NL on a second resource");
	ct_check(do_enq(&holder, "F87_CVT", LCK_K_EXMODE, 0, NULL) ==
		 SS__NORMAL,
		 "and the holder takes EX on it (NL conflicts with nothing)");

	memset(&c, 0, sizeof(c));
	c.proc = &waiter;
	c.convert = 1;
	c.args.lkid = waiter_lkid;
	c.args.lkmode = LCK_K_EXMODE;
	c.args.flags = LCK_M_SYNC | LCK_M_CONVERT;
	exec_host_interrupt_waits = 1u;
	ct_check(run_with_deadline(&c, 5u, &holder, 0u) == 1,
		 "*** the interrupted $ENQW CONVERT comes back too -- the "
		 "exact ioctl the lab's stuck CPU was in ***");
	ct_check_eq_u32((unsigned long)(-c.rc), (unsigned long)ERESTARTSYS,
			"  with -ERESTARTSYS");
	ct_check_eq_u32(c.args.status, 0u, "  and no status written");
	ct_check_eq_u32(lki_mode(waiter_lkid), LCK_K_NLMODE,
			"*** and the lock is STILL HELD AT ITS OLD MODE: a "
			"failed convert never loses the lock (VMS semantics) "
			"***");

	/*
	 * AND AN UNINTERRUPTED WAIT STILL WAITS AND STILL COMPLETES. On its own
	 * resource and its own pair of processes, so nothing above can colour
	 * it: the fix must change what a SIGNAL does and nothing else.
	 */
	exec_host_interrupt_waits = 0u;
	{
		struct vms_proc h2, w2;
		pthread_t th;
		uint32_t h2_lkid = 0;

		proc_init(&h2);
		proc_init(&w2);
		ct_check(do_enq(&h2, "F87_OK", LCK_K_EXMODE, 0, &h2_lkid) ==
			 SS__NORMAL && h2_lkid != 0u,
			 "a fresh holder takes EX on a fresh resource");

		memset(&c, 0, sizeof(c));
		c.proc = &w2;
		c.args.lkmode = LCK_K_EXMODE;
		c.args.flags = LCK_M_SYNC;
		strscpy(c.args.resnam, "F87_OK", sizeof(c.args.resnam));
		c.done = 0;
		ct_check(pthread_create(&th, NULL, intr_call_thread, &c) == 0,
			 "a $ENQW blocks for real, with nothing interrupting "
			 "it");
		usleep(50000);
		ct_check_eq_u32((unsigned long)c.done, 0u,
				"*** it is STILL waiting 50 ms later: the fix "
				"did not turn every wait into an immediate "
				"return ***");
		ct_check(do_deq(&h2, h2_lkid) == SS__NORMAL,
			 "the holder releases");
		ct_check(pthread_join(th, NULL) == 0, "the waiter returns");
		ct_check_eq_u32(c.args.status, SS__NORMAL,
				"*** GRANTED, through the same wait: the only "
				"thing that changed is what a SIGNAL does ***");
		ct_check_eq_u32(c.rc, 0u, "  and the ioctl itself succeeded");
	}

	vms_lock_cleanup();
}

/* ev11 (rd vms-ci.6, 2026-10-09 11:12Z): the lab's OVMX node spun CPU 0 with
 * res->lock held (an RCU stall) the moment a local CONVERT queued behind a
 * remote EX. check_deadlock() walked the blocker's owner -- the cluster
 * delivery process, which owns EVERY remote system's lock -- found one of its
 * OTHER locks waiting behind ANOTHER of its locks (two VAXes contending for
 * one resource), and re-pushed that pair forever. alarm() turns a regression
 * back into a hang into a failed test instead of a wedged ctest. */


static void the_ev11_remote_contention_does_not_spin_the_deadlock_search(void)
{
	struct vms_dlm_master_request r;
	struct vms_dlm_master_result out;
	struct vms_proc delivery, app;
	struct vms_enq_args cvt;
	uint32_t local_lkid = 0;

	if (vms_lock_init() != 0) {
		ct_check(0, "ev11: vms_lock_init");
		return;
	}
	printf("-- ev11: a local CONVERT behind a remote EX while two remote "
	       "systems contend for another resource\n");
	proc_init(&delivery);
	proc_init(&app);
	delivery.current_mode = PSL_C_USER;
	app.current_mode = MD_PEER_MODE;
	app.uic = (MD_PEER_GROUP << 16);
	vms_lock_dlm_set_delivery_proc(&delivery);

	/* Two remote systems contend on OTHER$NAME: VAX1 EX granted, VAX2 EX
	 * queued -- both owned here by the delivery process. */
	ct_check(do_enq(&app, "OTHER$NAME", LCK_K_NLMODE, 0, &local_lkid) ==
		 SS__NORMAL, "this node masters OTHER$NAME");
	md_fill(&r, VMS_DLM_MREQ_ENQ, MD_PEER_A, MD_LKID_A, LCK_K_EXMODE, 0,
		"OTHER$NAME");
	ct_check(vms_lock_dlm_master_serve(&r, &out) == SS__NORMAL &&
		 out.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED,
		 "VAX1 holds EX on OTHER$NAME");
	md_fill(&r, VMS_DLM_MREQ_ENQ, MD_PEER_B, MD_LKID_B + 1u, LCK_K_EXMODE, 0,
		"OTHER$NAME");
	ct_check(vms_lock_dlm_master_serve(&r, &out) == SS__NORMAL &&
		 out.outcome == (uint8_t)VMS_DLM_MASTER_QUEUED,
		 "VAX2's EX on OTHER$NAME queues behind VAX1's");

	/* The workload resource: local NL, VAX1 EX, then the local CONVERT. */
	ct_check(do_enq(&app, "EVAC$WORKLOAD", LCK_K_NLMODE, 0, &local_lkid) ==
		 SS__NORMAL, "an OVMX process takes NL on EVAC$WORKLOAD");
	md_fill(&r, VMS_DLM_MREQ_ENQ, MD_PEER_A, MD_LKID_A + 2u, LCK_K_EXMODE, 0,
		"EVAC$WORKLOAD");
	ct_check(vms_lock_dlm_master_serve(&r, &out) == SS__NORMAL &&
		 out.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED,
		 "VAX1's EX on EVAC$WORKLOAD is granted");

	memset(&cvt, 0, sizeof(cvt));
	cvt.lkid = local_lkid;
	cvt.lkmode = LCK_K_EXMODE;
	alarm(10);
	vms_ioctl_convert(&app, (unsigned long)(void *)&cvt);
	alarm(0);
	ct_check_eq_u32(cvt.status, SS__NORMAL,
			"*** the local NL->EX convert QUEUES and returns -- the "
			"deadlock search does not spin through the delivery "
			"process ***");
	ct_check_eq_u32(vms_lock_deadlock_budget_hits(), 0u,
			"*** and it never needed the step budget: a remote "
			"holder's other waits are not this request's wait-for "
			"edges ***");

	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_cleanup();
}

int main(void)
{
	printf("=== test_lock_host (vms_lock.c, the real engine, R1 host unit) ===\n");
	lock_basic();
	lock_stress();
	remote_lkb_is_outside_image_rundown();
	master_door_refuses_without_a_delivery_proc();
	master_door_refuses_an_unstated_identity();
	master_door_reports_what_the_engine_did();
	the_namespace_is_qualified();
	dlksrch_both_initiate_aborts_once();
	the_ev6_queue_shape_does_not_spin();
	a_signal_ends_the_wait_instead_of_spinning();
	the_ev11_remote_contention_does_not_spin_the_deadlock_search();
	return ct_summary("test_lock_host");
}
