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

#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h -> lock_host_internal.h */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h -> exec_kbackend_host.h */

#include <pthread.h>
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
uint32_t dlm_member_csids[VMS_DLM_MAX_MEMBERS] = { 1 };
int      dlm_member_count = 1;

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

int main(void)
{
	printf("=== test_lock_host (vms_lock.c, the real engine, R1 host unit) ===\n");
	lock_basic();
	lock_stress();
	return ct_summary("test_lock_host");
}
