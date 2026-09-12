// SPDX-License-Identifier: GPL-2.0
/*
 * test_lock_quorum_hang.c - THE QUORUM HANG, on the real engine and a real
 * CLUB (FC-P8.1, rd vms-b6d; test-ladder rung R1).
 *
 * WHAT IS REAL HERE. Everything that decides anything:
 *
 *   - src/kernel-core/vms_lock.c, the REAL lock engine, unmodified and
 *     compiled exactly as test_lock_host.c compiles it (FC-P4.9 host backend).
 *   - src/kernel-core/vms_cnxman_quorum.c + vms_cnxman_csb.c, the REAL quorum
 *     arithmetic over a REAL struct vms_club whose CSBs are built through the
 *     connection manager's own helpers -- votes learned into CSBs, never a
 *     hand-set quorum figure (INV-6).
 *   - the REAL gate between them: the engine asks `struct vms_quorum_ops.hang`
 *     (vms_dlm_quorum.h) and this file's binding answers with
 *     cnxman_quorum_hang_active(&g_cl) -- the same one line the production
 *     binding in vms_dlm_scs.c's dlm_arm_quorum_hang() is. Nothing about the
 *     hang is faked: to make the engine stall, this test has to make a CLUB
 *     genuinely lose its votes.
 *
 * WHAT IT PROVES (the item's host bar, in order):
 *   1. A node WITH quorum grants normally -- no spurious stall.
 *   2. A member in the join-transient honest-zero window does NOT stall (the
 *      gate: quorum_lost is set, and it is not a hang).
 *   3. A clustered $ENQ STALLS in an armed quorum loss: queued, no grant, and
 *      NO error status -- not even for LCK$M_NOQUEUE.
 *   4. Releases still work during the hang, and a release does NOT grant the
 *      stalled request.
 *   5. A down-convert proceeds during the hang; an up-convert stalls.
 *   6. The SAME request GRANTS when quorum returns -- including a synchronous
 *      $ENQW that sat in the hang past the deadlock-rescan interval without
 *      being handed a deadlock (a hang is not a cycle).
 *
 * ORACLE (clean-room, rule 8, page cites only): *VAXcluster Principles*
 * (Davis 1993) p. 7-4 -- activity proceeds while available votes >= QUORUM,
 * otherwise the system blocks activity and waits for quorum to be regained;
 * pp. 7-10/7-11 -- CEVOTES cannot decrease by itself, which is why a two-vote
 * cluster that loses one vote hangs rather than re-deriving a quorum of one.
 */

#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h -> lock_host_internal.h */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h -> exec_kbackend_host.h */
#include "vms_dlm_quorum.h"   /* the gate the engine consults */

/* The connection manager's own headers, host-mode (OVMX_CLUSTER_HOST). */
#include "vms_cluster.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_quorum.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* This node's CSID, as the engine reads it (see test_lock_host.c). */
uint32_t vms_local_csid = 1;

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

/* ================================================================
 * The CLUB: one node, one peer, real votes learned into real CSBs.
 * ================================================================ */

static struct vms_cluster g_cl;
static struct vms_csb    *g_peer;

/* The production binding, one line of it: the engine asks, the connection
 * manager answers from its own club. */
static int test_quorum_hang(void *ctx)
{
	return cnxman_quorum_hang_active((const struct vms_cluster *)ctx);
}

/* Recompute + latch, exactly as cnxman_quorum_apply() does on the fork
 * thread. Returns the enforceable answer afterwards. */
static int club_recompute(void)
{
	cnxman_quorum_recompute(&g_cl.club);
	cnxman_quorum_arm_update(&g_cl);
	return cnxman_quorum_hang_active(&g_cl);
}

/*
 * A two-node cluster this node is a committed member of: local VOTES=1,
 * EXPECTED_VOTES=2, and a peer advertising 1 vote -- so CEVOTES=2, QUORUM=2,
 * and both votes present. The peer's votes are LEARNED through
 * cnxman_csb_set_params, the same call the op-01 PARAMS receive path makes.
 */
static void club_form_two_node(void)
{
	struct vms_csb *local;

	memset(&g_cl, 0, sizeof(g_cl));
	g_cl.params.scssystemid = 0x0000040001FFull;
	memcpy(g_cl.params.scsnode, "LOCAL", 5);
	g_cl.params.scsnode_len = 5;
	g_cl.params.votes = 1;
	g_cl.params.expected_votes = 2;
	cnxman_club_init(&g_cl);

	local = cnxman_club_local(&g_cl.club);
	cnxman_csb_set_flags(local, (uint16_t)(VMS_CSB_F_SELECTED |
					       VMS_CSB_F_MEMBER));

	g_peer = cnxman_club_alloc_csb(&g_cl.club, 0x2000ull, 1);
	cnxman_csb_set_params(g_peer, 1, 2, 0);
	cnxman_csb_set_flags(g_peer, (uint16_t)(VMS_CSB_F_SELECTED |
						VMS_CSB_F_MEMBER));
	g_peer->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;

	g_cl.state = VMS_CLUSTER_MEMBER;
	(void)club_recompute();
}

/* The peer's circuit dies: its votes are no longer AVAILABLE (p. 7-5). */
static int club_lose_peer(void)
{
	g_peer->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	return club_recompute();
}

static int club_regain_peer(void)
{
	g_peer->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
	return club_recompute();
}

/* ================================================================
 * Engine harness (same shape as test_lock_host.c)
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

	memset(&d, 0, sizeof(d));
	d.lkid = lkid;
	vms_ioctl_deq(proc, (unsigned long)(void *)&d);
	return d.status;
}

static uint32_t do_convert(struct vms_proc *proc, uint32_t lkid, uint32_t mode)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkid = lkid;
	a.lkmode = mode;
	vms_ioctl_convert(proc, (unsigned long)(void *)&a);
	return a.status;
}

/* The STATE DELTA this whole item is measured by, read back from the engine's
 * own lock block rather than inferred from a return code. */
static void lki(struct vms_proc *proc, uint32_t lkid, uint32_t *granted,
		uint32_t *requested)
{
	struct vms_getlki_args g;

	memset(&g, 0, sizeof(g));
	g.lkid = lkid;
	vms_ioctl_getlki(proc, (unsigned long)(void *)&g);
	*granted = g.granted_mode;
	*requested = g.requested_mode;
}

static int is_queued(struct vms_proc *proc, uint32_t lkid, uint32_t want_mode)
{
	uint32_t granted = 0, requested = 0;

	lki(proc, lkid, &granted, &requested);
	return granted == LCK_K_NLMODE && requested == want_mode;
}

static int is_granted(struct vms_proc *proc, uint32_t lkid, uint32_t want_mode)
{
	uint32_t granted = 0, requested = 0;

	lki(proc, lkid, &granted, &requested);
	(void)requested;
	return granted == want_mode;
}

static void gate_install(void)
{
	struct vms_quorum_ops qops;

	qops.hang = test_quorum_hang;
	qops.ctx  = &g_cl;
	vms_lock_set_quorum_ops(&qops);
}

/* ================================================================
 * 1-4. Grant, gate, stall, release
 * ================================================================ */

static void quorum_hang_stalls_the_grant(void)
{
	struct vms_proc pa;
	uint32_t held = 0, stalled = 0, noqueue = 0, before_hang = 0;

	printf("[hang] a clustered $ENQ stalls on quorum loss and grants on regain\n");

	vms_lock_init();
	proc_init(&pa);
	club_form_two_node();
	gate_install();

	/* --- 1. WITH QUORUM: nothing stalls. --- */
	ct_check(!cnxman_quorum_hang_active(&g_cl), "formed CN=2: no hang");
	ct_check(do_enq(&pa, "Q_HELD", LCK_K_EXMODE, 0, &held) == SS__NORMAL &&
		 held != 0 && is_granted(&pa, held, LCK_K_EXMODE),
		 "with quorum: $ENQ EX is GRANTED immediately (no spurious stall)");

	/* --- 2. THE GATE: the honest-zero window is not a hang. A node that is
	 * not (yet) a committed member computes quorum_lost=1 over the set it
	 * has learned so far; the engine must keep granting. --- */
	g_cl.state = VMS_CLUSTER_OFF;
	g_peer->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	cnxman_quorum_recompute(&g_cl.club);
	ct_check(g_cl.club.quorum_lost != 0,
		 "join-transient: the raw quorum_lost flag IS set");
	ct_check(!cnxman_quorum_hang_active(&g_cl),
		 "join-transient: and it is NOT a hang");
	ct_check(do_enq(&pa, "Q_TRANSIENT", LCK_K_EXMODE, 0, &before_hang) ==
			 SS__NORMAL &&
		 is_granted(&pa, before_hang, LCK_K_EXMODE),
		 "join-transient: $ENQ still GRANTS -- the gate is what keeps a "
		 "join from freezing");
	(void)do_deq(&pa, before_hang);
	g_cl.state = VMS_CLUSTER_MEMBER;
	(void)club_regain_peer();

	/* --- 3. THE HANG. The peer's votes go; CEVOTES stays 2 (p. 7-10), so
	 * one present vote is a real loss. --- */
	ct_check(club_lose_peer() != 0,
		 "peer evacuated: the CLUB reports an ARMED quorum loss");

	ct_check(do_enq(&pa, "Q_STALL", LCK_K_EXMODE, 0, &stalled) == SS__NORMAL,
		 "in the hang: $ENQ returns SS$_NORMAL -- a stall is not an error");
	ct_check(stalled != 0, "in the hang: ... with a real lock id");
	ct_check(is_queued(&pa, stalled, LCK_K_EXMODE),
		 "in the hang: ... and the lock is QUEUED, not granted "
		 "(granted NL, requested EX) -- on a resource with NO holder");

	/* NOQUEUE does not become an error either: it speaks about holders. */
	ct_check(do_enq(&pa, "Q_NOQUEUE", LCK_K_EXMODE, LCK_M_NOQUEUE, &noqueue) ==
			 SS__NORMAL &&
		 noqueue != 0 && is_queued(&pa, noqueue, LCK_K_EXMODE),
		 "in the hang: LCK$M_NOQUEUE is QUEUED too, never SS$_NOTQUEUED");

	/* --- 4. RELEASES STILL WORK, and do not grant the stalled request. --- */
	ct_check(do_deq(&pa, held) == SS__NORMAL,
		 "in the hang: $DEQ of a lock held since before the loss SUCCEEDS");
	ct_check(is_queued(&pa, stalled, LCK_K_EXMODE),
		 "in the hang: ... and that release did NOT grant the stalled "
		 "request (no $DEQ may end a quorum hang)");

	/* --- 6a. RESUME: the same requests grant, untouched, in FIFO order. --- */
	ct_check(club_regain_peer() == 0, "peer back: the CLUB reports quorum");
	vms_lock_quorum_resume();
	ct_check(is_granted(&pa, stalled, LCK_K_EXMODE),
		 "on regain: the STALLED request is now GRANTED at EX -- the same "
		 "request, completed, never re-made");
	ct_check(is_granted(&pa, noqueue, LCK_K_EXMODE),
		 "on regain: the NOQUEUE request is GRANTED too -- the request a "
		 "naive implementation would have failed with SS$_NOTQUEUED gets "
		 "the lock it asked for");

	vms_lock_set_quorum_ops(NULL);
	vms_lock_cleanup();
}

/* ================================================================
 * 5. Conversions: down proceeds, up stalls
 * ================================================================ */

static void quorum_hang_stalls_up_conversion_only(void)
{
	struct vms_proc pa;
	uint32_t lk = 0;

	printf("[hang] conversions: a down-convert gives strength back, an "
	       "up-convert asks for it\n");

	vms_lock_init();
	proc_init(&pa);
	club_form_two_node();
	gate_install();

	ct_check(do_enq(&pa, "Q_CONV", LCK_K_PRMODE, 0, &lk) == SS__NORMAL &&
		 is_granted(&pa, lk, LCK_K_PRMODE),
		 "with quorum: PR granted");

	ct_check(club_lose_peer() != 0, "peer evacuated: armed quorum loss");

	ct_check(do_convert(&pa, lk, LCK_K_NLMODE) == SS__NORMAL &&
		 is_granted(&pa, lk, LCK_K_NLMODE),
		 "in the hang: the DOWN-convert PR->NL completes (a release of "
		 "strength is always allowed)");

	ct_check(do_convert(&pa, lk, LCK_K_EXMODE) == SS__NORMAL &&
		 is_queued(&pa, lk, LCK_K_EXMODE),
		 "in the hang: the UP-convert NL->EX is QUEUED, not granted and "
		 "not refused");

	ct_check(club_regain_peer() == 0, "peer back: quorum");
	vms_lock_quorum_resume();
	ct_check(is_granted(&pa, lk, LCK_K_EXMODE),
		 "on regain: the up-convert completes at EX");

	vms_lock_set_quorum_ops(NULL);
	vms_lock_cleanup();
}

/* ================================================================
 * 6b. The SYNCHRONOUS waiter: it waits, it is not deadlocked, it wakes
 * ================================================================ */

struct sync_result {
	struct vms_proc *proc;
	uint32_t         status;
	uint32_t         lkid;
	int              done;
};

static void *sync_enq_thread(void *arg)
{
	struct sync_result *r = (struct sync_result *)arg;
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = LCK_K_EXMODE;
	a.flags = LCK_M_SYNC;
	strscpy(a.resnam, "Q_SYNC", sizeof(a.resnam));
	vms_ioctl_enq(r->proc, (unsigned long)(void *)&a);
	r->status = a.status;
	r->lkid = a.lkid;
	r->done = 1;
	return NULL;
}

static void quorum_hang_holds_a_sync_waiter_without_deadlocking_it(void)
{
	struct vms_proc pa;
	struct sync_result r;
	pthread_t th;
	int i;

	printf("[hang] a synchronous $ENQW waits out the hang (a hang is not a "
	       "deadlock) and wakes on the regain\n");

	vms_lock_init();
	proc_init(&pa);
	club_form_two_node();
	gate_install();
	ct_check(club_lose_peer() != 0, "peer evacuated: armed quorum loss");

	memset(&r, 0, sizeof(r));
	r.proc = &pa;
	ct_check(pthread_create(&th, NULL, sync_enq_thread, &r) == 0,
		 "sync $ENQW issued on its own thread");

	/*
	 * Past VMS_DEADLOCK_WAIT_MS (500 ms), which is when the engine re-runs
	 * deadlock detection for a still-waiting request. A quorum-stalled
	 * request must be skipped by that scan: there is no cycle, and
	 * answering SS$_DEADLOCK would be the error return a quorum hang does
	 * not have.
	 */
	for (i = 0; i < 80 && !r.done; i++)
		usleep(10000);              /* 800 ms ceiling */
	ct_check(!r.done,
		 "in the hang: the $ENQW is STILL waiting after the deadlock "
		 "re-scan interval -- not granted, and not SS$_DEADLOCK");

	ct_check(club_regain_peer() == 0, "peer back: quorum");
	vms_lock_quorum_resume();

	for (i = 0; i < 200 && !r.done; i++)
		usleep(10000);
	ct_check(r.done && r.status == SS__NORMAL && r.lkid != 0,
		 "on regain: the $ENQW woke and was GRANTED (SS$_NORMAL)");
	ct_check(r.done && is_granted(&pa, r.lkid, LCK_K_EXMODE),
		 "on regain: ... at the mode it asked for");

	pthread_join(th, NULL);
	vms_lock_set_quorum_ops(NULL);
	vms_lock_cleanup();
}

/* ================================================================
 * NEGCTL: with no gate installed -- a node with no cluster -- a vote-less,
 * quorum-lost CLUB cannot stall anything, because nobody is asking it.
 * ================================================================ */

static void no_cluster_never_stalls(void)
{
	struct vms_proc pa;
	uint32_t lk = 0;

	printf("[hang] negctl: a node with no cluster locks freely\n");

	vms_lock_init();
	proc_init(&pa);
	club_form_two_node();
	ct_check(club_lose_peer() != 0,
		 "the CLUB is in an armed quorum loss ...");
	vms_lock_set_quorum_ops(NULL);   /* ... but no cluster is wired to the
					  * engine (vms_dlm_scs_stop's state) */

	ct_check(do_enq(&pa, "Q_LOCAL", LCK_K_EXMODE, 0, &lk) == SS__NORMAL &&
		 is_granted(&pa, lk, LCK_K_EXMODE),
		 "no gate installed: $ENQ grants -- an ungated engine has no "
		 "quorum to lose");

	vms_lock_cleanup();
}

int main(void)
{
	printf("=== test_lock_quorum_hang (FC-P8.1, the real engine + a real "
	       "CLUB) ===\n");
	quorum_hang_stalls_the_grant();
	quorum_hang_stalls_up_conversion_only();
	quorum_hang_holds_a_sync_waiter_without_deadlocking_it();
	no_cluster_never_stalls();
	return ct_summary("test_lock_quorum_hang");
}
