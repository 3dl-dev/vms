// SPDX-License-Identifier: GPL-2.0
/*
 * vms_module.c - Core VMS kernel module
 *
 * Provides /dev/vms character device with ioctl interface for:
 *   - Access mode enforcement (kernel/exec/super/user)
 *   - 4-level AST delivery
 *   - Kernel event flags
 *   - Lock manager with 6-mode compatibility
 *
 * Processes register via VMS_IOCTL_REGISTER to get a per-process
 * vms_proc structure allocated in kernel memory.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/rbtree.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>     /* thread_group_empty() */
#include <linux/mm.h>               /* vm_area_struct, vm_flags_clear, PAGE_* (vms_lnm_mmap) */
#include <linux/vmalloc.h>          /* remap_vmalloc_range (vms_lnm_mmap, vms-d61) */

#include "vms_internal.h"
#include "vms_bg_core.h"    /* vms_bg_capture_channels -- fork-inherit snapshot (vms-0cd) */

#if defined(OVMX_KTEST_CLUSTER_SEAM)
#include <linux/completion.h>   /* struct completion (post-and-wake proofs) */
#include <linux/delay.h>        /* msleep */
#include "exec_kbackend.h"      /* SS13 exec_l2_* (already real) + SS14..SS18
				  * (FC-P0.2) -- the ONLY substrate surface the
				  * cluster-seam self-test below touches */
#include "vms_cluster.h"        /* struct vms_cluster (FC-P0.16 hammer knob) */
#include "vms_cluster_fork.h"   /* cf_rx_deliver/cf_post/vms_cluster_fork_start
				  * -- the REAL FC-P0.16 rxlock/cv path */
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OVMX Project");
MODULE_DESCRIPTION("VMS subsystem kernel module");
MODULE_VERSION("1.0");

/* ================================================================
 * Global state
 * ================================================================ */

DEFINE_HASHTABLE(vms_proc_hash, VMS_PROC_HASH_BITS);
DEFINE_SPINLOCK(vms_proc_hash_lock);

static struct kmem_cache *vms_proc_cache;

/*
 * vms_local_csid - this node's cluster system ID, consumed by the DLM directory
 * + mastering logic in the shared lock manager (src/kernel-core/vms_lock.c). The
 * variable and its insmod module parameter live HERE, in the Linux module rind,
 * not in the substrate-agnostic core: a module parameter is a host-module-
 * lifecycle facility, and the core facility reaches this value through the extern
 * in vms_internal.h. 0 is reserved for "unmastered", so the default is a non-zero
 * OVMX local placeholder; it is NOT a claim of a VMS-authentic CSID value or
 * layout (CLAUDE.md Rule 8) -- real CSIDs are assigned by the connection manager
 * at cluster join in 0.4.
 */
uint32_t vms_local_csid = 1;
module_param(vms_local_csid, uint, 0444);
MODULE_PARM_DESC(vms_local_csid,
    "OVMX DLM: this node's cluster system ID (local scaffolding; the connection manager assigns the real CSID at cluster join in 0.4)");

/*
 * dlm_member_csids[] / dlm_member_count - the DLM directory membership vector
 * (rd vms-1bba, the "DB" rung). A CONTROLLED, STATIC configuration input: the
 * operator or the 2-node test harness supplies the ordered cluster-member CSID
 * vector at insmod (`dlm_member_csids=1030,1031`), exactly as vms_local_csid is
 * supplied. module_param_array sets dlm_member_count to the number of elements
 * actually given. The shared DLM directory logic (src/kernel-core/vms_lock.c)
 * hashes a resource name across this vector to select the directory node, so
 * every node given the SAME vector independently resolves the SAME directory
 * (and, this rung, master) -- which is what vms-1bba proves.
 *
 * This is DELIBERATELY NOT the live membership feed from the connection manager
 * / SCS rejoin: that is the 0.4 "DC" successor (overlapping vms-2f3). Supplying
 * membership as a load-time input is honest configuration, distinct from
 * fabricating live cluster state (INV-6). Left empty (count 0) it defaults to a
 * cluster-of-one on vms_local_csid, so single-node behaviour is unchanged.
 */
uint32_t dlm_member_csids[VMS_DLM_MAX_MEMBERS];
int      dlm_member_count;
module_param_array(dlm_member_csids, uint, &dlm_member_count, 0444);
MODULE_PARM_DESC(dlm_member_csids,
    "OVMX DLM: static ordered cluster-member CSID vector for the directory proof (controlled DB-rung input, NOT the live 0.4/DC membership feed); same order on every node");

#if defined(OVMX_KTEST_FAULT_INJECT)
/*
 * TEST-ONLY: arm ACP block-I/O fault injection (rd vms-5f82). Compiled in ONLY
 * for the out-of-tree QEMU-test vms.ko (src/kernel/Makefile defines the macro),
 * never in the bootable executive (its distro Kbuild does not). The knob lets a
 * kernel-module test under a real /dev/vms make a genuine ACP block op FAIL --
 * exactly as a bad sector would -- so the executive's REAL per-device error
 * accounting (vms_device.errcnt, which SHOW ERROR / F$GETDVI ERRCNT read) can be
 * exercised end to end. Write "major:minor:count" to arm the next `count` block
 * ops on that backing device to report failure; "M:N:0" disarms. Write-only, and
 * a module-lifecycle facility, so it lives HERE in the Linux rind and forwards
 * to the substrate-agnostic core, exactly as vms_local_csid above does.
 */
static int vms_ktest_bdev_fault_set(const char *val, const struct kernel_param *kp)
{
    unsigned int maj = 0, min = 0, cnt = 0;

    (void)kp;
    if (!val || sscanf(val, "%u:%u:%u", &maj, &min, &cnt) < 3)
        return -EINVAL;
    vmsfs_acp_test_arm_bdev_fault(maj, min, cnt);
    return 0;
}
static const struct kernel_param_ops vms_ktest_bdev_fault_ops = {
    .set = vms_ktest_bdev_fault_set,
};
module_param_cb(vms_ktest_bdev_fault, &vms_ktest_bdev_fault_ops, NULL, 0200);
MODULE_PARM_DESC(vms_ktest_bdev_fault,
    "TEST-ONLY (rd vms-5f82): arm ACP block-I/O fault injection as \"major:minor:count\"; count 0 disarms");
#endif /* OVMX_KTEST_FAULT_INJECT */

#if defined(OVMX_KTEST_CLUSTER_SEAM)
/*
 * TEST-ONLY: the FC-P0.2 substrate contract self-test (rung R3, design
 * docs/design-faithful-cluster-executive.md SS3.9). Compiled in ONLY for the
 * out-of-tree QEMU-test vms.ko (src/kernel/Makefile defines the macro), never
 * the bootable executive (distro Kbuild does not) -- the same posture as
 * OVMX_KTEST_FAULT_INJECT above. Exercises the REAL Linux binding of
 * exec_kbackend.h SS14..SS18 (dev_add_pack/dev_queue_xmit/dev_mc_add/kthread/
 * timer_list/ktime) against a veth pair the harness has already created, using
 * the already-real SS13 exec_l2_* AF_PACKET primitives on the PEER interface
 * as the test's own verification oracle -- no protocol-layer code (vms_pe.c,
 * FC-P0.9) is on this path, only the seam itself. Write-triggered
 * (vms_ktest_cluster_seam_run="ifname_a:ifname_b"), the same
 * arm-then-read-back shape as vms_ktest_bdev_fault above; results are read
 * back as KEY=VAL tokens from vms_ktest_cluster_seam_result.
 */
struct vms_ktest_seam_result {
	int done, open_ok, mc_add_ok, mc_del_ok, close_ok, hwaddr_ok, mtu_ok, link_ok;
	int link_up, rx_ok, tx_ok, kthread_ok, kthread_iters;
	int timer_ok, time_mono_ok, ticks_mono_ok;
	uint8_t mc_mac[6], hwaddr[6];
	uint32_t mtu, rx_len, tx_len;
	char rx_payload[32], tx_payload[32];
};
static struct vms_ktest_seam_result vms_ktest_seam_res;
static DEFINE_MUTEX(vms_ktest_seam_mtx);

struct vms_ktest_seam_rx_ctx {
	struct completion got;
	uint8_t buf[128];
	uint32_t len;
};

/* CONTRACT RULE 1 in miniature: this rx_cb, standing in for the core's own,
 * only copies into a buffer it owns and wakes a waiter -- no protocol code. */
static void vms_ktest_seam_rx_cb(void *ctx, const uint8_t *frame, uint32_t len)
{
	struct vms_ktest_seam_rx_ctx *rc = ctx;
	uint32_t n = len < sizeof(rc->buf) ? len : sizeof(rc->buf);

	memcpy(rc->buf, frame, n);
	rc->len = len;
	complete(&rc->got);
}

struct vms_ktest_seam_kt_ctx {
	atomic_t iters;
	struct completion started;
};

static int vms_ktest_seam_kt_fn(void *arg)
{
	struct vms_ktest_seam_kt_ctx *kc = arg;

	while (!exec_kthread_should_stop(NULL)) {
		if (atomic_inc_return(&kc->iters) == 1)
			complete(&kc->started);
		msleep(20);
	}
	return 0;
}

/* CONTRACT RULE 2 in miniature: this cb only "posts and wakes" -- complete()
 * is this test's stand-in for the real fork queue post+wake (FC-P0.5). */
static void vms_ktest_seam_timer_cb(void *ctx)
{
	complete((struct completion *)ctx);
}

/* Probe the four read-only SS14 accessors on the just-opened port. */
static void vms_ktest_seam_probe_port(struct vms_ktest_seam_result *res,
				       uint8_t hwaddr_out[6])
{
	if (exec_lan_hwaddr(hwaddr_out) == 0) {
		res->hwaddr_ok = 1;
		memcpy(res->hwaddr, hwaddr_out, 6);
	}
	res->mtu_ok = (exec_lan_mtu(&res->mtu) == 0 && res->mtu > 0);
	res->link_ok = (exec_lan_link_up(&res->link_up) == 0);

	res->mc_mac[0] = 0xab; res->mc_mac[1] = 0x00; res->mc_mac[2] = 0x04;
	res->mc_mac[3] = 0x01; res->mc_mac[4] = 0x00; res->mc_mac[5] = 0x2a;
	res->mc_add_ok = (exec_lan_mc_add(res->mc_mac) == 0);
}

/* RX proof: inject a 0x6007 frame from the PEER interface (via the already-
 * real SS13 exec_l2_send) addressed to the port's own hwaddr, and confirm
 * exec_lan_open's rx_cb (registered on THIS interface) saw it -- the veth-
 * pair softirq-delivery proof the design's R3 rung names. */
static void vms_ktest_seam_do_rx(struct vms_ktest_seam_result *res,
				  struct vms_ktest_seam_rx_ctx *rx_ctx,
				  exec_socket_t peer_sock, uint32_t peer_ifindex,
				  const uint8_t hwaddr_a[6])
{
	static const char want[] = "OVMXSEAMRX";
	uint8_t frame[64];

	memset(frame, 0, sizeof(frame));
	memcpy(frame + 0, hwaddr_a, 6);
	frame[12] = 0x60; frame[13] = 0x07;         /* ethertype 0x6007 (SCA) */
	memcpy(frame + 14, want, sizeof(want));
	exec_l2_send(peer_sock, (int)peer_ifindex, 0x6007u, hwaddr_a,
		     frame, 14 + sizeof(want));

	res->rx_ok = wait_for_completion_timeout(&rx_ctx->got, HZ) != 0;
	if (res->rx_ok && rx_ctx->len > 14) {
		uint32_t n = rx_ctx->len - 14;

		if (n > sizeof(res->rx_payload) - 1)
			n = sizeof(res->rx_payload) - 1;
		res->rx_len = rx_ctx->len;
		memcpy(res->rx_payload, rx_ctx->buf + 14, n);
	}
}

/* TX proof: exec_lan_xmit a frame out the port and confirm the PEER
 * interface's (already-real SS13) exec_l2_recv captures it. */
static void vms_ktest_seam_do_tx(struct vms_ktest_seam_result *res,
				  exec_socket_t peer_sock,
				  const uint8_t hwaddr_a[6])
{
	static const char payload[] = "OVMXSEAMTX";
	uint8_t frame[64], rxbuf[128];
	size_t out_len = 0;

	memset(frame, 0xff, 6);                     /* broadcast dst */
	memcpy(frame + 6, hwaddr_a, 6);
	frame[12] = 0x60; frame[13] = 0x07;
	memcpy(frame + 14, payload, sizeof(payload));

	if (exec_lan_xmit(frame, 14 + sizeof(payload)) != 0)
		return;
	if (exec_l2_recv(peer_sock, rxbuf, sizeof(rxbuf), 1000, &out_len) != 0)
		return;
	if (out_len < 14 + sizeof(payload) ||
	    memcmp(rxbuf + 14, payload, sizeof(payload)) != 0)
		return;

	res->tx_ok = 1;
	res->tx_len = (uint32_t)out_len;
	memcpy(res->tx_payload, rxbuf + 14, sizeof(payload));
}

static void vms_ktest_seam_do_kthread(struct vms_ktest_seam_result *res)
{
	struct vms_ktest_seam_kt_ctx kt_ctx;
	exec_kthread_t kt = NULL;

	atomic_set(&kt_ctx.iters, 0);
	init_completion(&kt_ctx.started);
	if (exec_kthread_create(&kt, vms_ktest_seam_kt_fn, &kt_ctx, "vms_seam_kt") != 0)
		return;
	res->kthread_ok = wait_for_completion_timeout(&kt_ctx.started, HZ) != 0;
	exec_kthread_stop(&kt);
	res->kthread_iters = atomic_read(&kt_ctx.iters);
	res->kthread_ok = res->kthread_ok && res->kthread_iters > 0;
}

static void vms_ktest_seam_do_timer(struct vms_ktest_seam_result *res)
{
	struct completion fired;
	exec_timer_t timer;

	init_completion(&fired);
	exec_timer_init(&timer, vms_ktest_seam_timer_cb, &fired);
	exec_timer_arm(&timer, 50);
	res->timer_ok = wait_for_completion_timeout(&fired, HZ) != 0;
	exec_timer_cancel(&timer);
	exec_timer_destroy(&timer);
}

static void vms_ktest_seam_do_time(struct vms_ktest_seam_result *res)
{
	uint64_t t1 = exec_time_now_vms();
	uint64_t k1 = exec_ticks_ms();

	msleep(15);
	res->time_mono_ok = (t1 != 0 && exec_time_now_vms() > t1);
	res->ticks_mono_ok = (exec_ticks_ms() > k1);
}

/*
 * The port and its multicast membership are DELIBERATELY left open on return
 * (no exec_lan_mc_del / exec_lan_close here): the R3 done-condition requires
 * "multicast add visible in `ip maddr`" as an assertion the CALLER makes from
 * userspace, external to this module, AFTER vms_ktest_cluster_seam_run
 * returns -- tearing the join down before the caller can look would make that
 * assertion untestable. vms_ktest_cluster_seam_teardown (below) closes it.
 */
static int vms_ktest_cluster_seam_run(const char *ifname_a, const char *ifname_b)
{
	struct vms_ktest_seam_result res;
	struct vms_ktest_seam_rx_ctx rx_ctx;
	exec_socket_t peer_sock = NULL;
	uint32_t peer_ifindex = 0;
	uint8_t hwaddr_a[6] = {0};

	memset(&res, 0, sizeof(res));
	init_completion(&rx_ctx.got);
	rx_ctx.len = 0;

	if (exec_lan_open(ifname_a, 0x6007u, vms_ktest_seam_rx_cb, &rx_ctx) != 0) {
		res.open_ok = 0;
		goto record;    /* honest: nothing else is meaningful with no port */
	}
	res.open_ok = 1;
	vms_ktest_seam_probe_port(&res, hwaddr_a);

	if (res.hwaddr_ok &&
	    exec_l2_open(ifname_b, 0x6007u, &peer_ifindex, &peer_sock) == 0) {
		vms_ktest_seam_do_rx(&res, &rx_ctx, peer_sock, peer_ifindex, hwaddr_a);
		vms_ktest_seam_do_tx(&res, peer_sock, hwaddr_a);
		exec_socket_release(peer_sock);
	}

	vms_ktest_seam_do_kthread(&res);
	vms_ktest_seam_do_timer(&res);
	vms_ktest_seam_do_time(&res);

record:
	res.done = 1;
	mutex_lock(&vms_ktest_seam_mtx);
	vms_ktest_seam_res = res;
	mutex_unlock(&vms_ktest_seam_mtx);
	return 0;
}

/* Written second, after the caller has inspected the live multicast join
 * (/proc/net/dev_mcast == what `ip maddr` reads) and the open port: tears
 * the SS14 state down and records mc_del_ok/close in the result. */
static int vms_ktest_cluster_seam_teardown(void)
{
	struct vms_ktest_seam_result res;

	mutex_lock(&vms_ktest_seam_mtx);
	res = vms_ktest_seam_res;
	mutex_unlock(&vms_ktest_seam_mtx);

	if (res.open_ok) {
		res.mc_del_ok = (exec_lan_mc_del(res.mc_mac) == 0);
		exec_lan_close();
		res.close_ok = 1;
	}

	mutex_lock(&vms_ktest_seam_mtx);
	vms_ktest_seam_res = res;
	mutex_unlock(&vms_ktest_seam_mtx);
	return 0;
}

static int vms_ktest_cluster_seam_set(const char *val, const struct kernel_param *kp)
{
	char ifname_a[IFNAMSIZ] = {0};
	char ifname_b[IFNAMSIZ] = {0};
	const char *sep;
	size_t blen;

	(void)kp;
	if (!val)
		return -EINVAL;
	sep = strchr(val, ':');
	if (!sep || (size_t)(sep - val) >= sizeof(ifname_a))
		return -EINVAL;
	memcpy(ifname_a, val, sep - val);

	blen = strlen(sep + 1);
	if (blen && (sep + 1)[blen - 1] == '\n')
		blen--;                          /* strip a sysfs-write newline */
	if (blen >= sizeof(ifname_b))
		return -EINVAL;
	memcpy(ifname_b, sep + 1, blen);

	return vms_ktest_cluster_seam_run(ifname_a, ifname_b);
}
static const struct kernel_param_ops vms_ktest_cluster_seam_run_ops = {
	.set = vms_ktest_cluster_seam_set,
};
module_param_cb(vms_ktest_cluster_seam_run, &vms_ktest_cluster_seam_run_ops, NULL, 0200);
MODULE_PARM_DESC(vms_ktest_cluster_seam_run,
    "TEST-ONLY (rd FC-P0.2): run the SS14..SS18 substrate contract self-test against a veth pair \"ifname_a:ifname_b\"; leaves the port + multicast join OPEN for inspection until _teardown is written");

static int vms_ktest_cluster_seam_teardown_set(const char *val, const struct kernel_param *kp)
{
	(void)val;
	(void)kp;
	return vms_ktest_cluster_seam_teardown();
}
static const struct kernel_param_ops vms_ktest_cluster_seam_teardown_ops = {
	.set = vms_ktest_cluster_seam_teardown_set,
};
module_param_cb(vms_ktest_cluster_seam_teardown, &vms_ktest_cluster_seam_teardown_ops, NULL, 0200);
MODULE_PARM_DESC(vms_ktest_cluster_seam_teardown,
    "TEST-ONLY (rd FC-P0.2): close the port opened by vms_ktest_cluster_seam_run (any value written triggers it)");

static int vms_ktest_cluster_seam_result_get(char *buf, const struct kernel_param *kp)
{
	struct vms_ktest_seam_result res;

	(void)kp;
	mutex_lock(&vms_ktest_seam_mtx);
	res = vms_ktest_seam_res;
	mutex_unlock(&vms_ktest_seam_mtx);

	return scnprintf(buf, PAGE_SIZE,
	    "DONE=%d OPEN=%d MC_ADD=%d MC_DEL=%d CLOSE=%d MC_MAC=%02x:%02x:%02x:%02x:%02x:%02x "
	    "HWADDR=%d HWADDR_VAL=%02x:%02x:%02x:%02x:%02x:%02x MTU=%d MTU_VAL=%u "
	    "LINK=%d LINK_UP=%d RX=%d RX_LEN=%u RX_PAYLOAD=%s "
	    "TX=%d TX_LEN=%u TX_PAYLOAD=%s KTHREAD=%d KTHREAD_ITERS=%d "
	    "TIMER=%d TIME_MONO=%d TICKS_MONO=%d\n",
	    res.done, res.open_ok, res.mc_add_ok, res.mc_del_ok, res.close_ok,
	    res.mc_mac[0], res.mc_mac[1], res.mc_mac[2],
	    res.mc_mac[3], res.mc_mac[4], res.mc_mac[5],
	    res.hwaddr_ok, res.hwaddr[0], res.hwaddr[1], res.hwaddr[2],
	    res.hwaddr[3], res.hwaddr[4], res.hwaddr[5],
	    res.mtu_ok, res.mtu, res.link_ok, res.link_up,
	    res.rx_ok, res.rx_len, res.rx_payload,
	    res.tx_ok, res.tx_len, res.tx_payload,
	    res.kthread_ok, res.kthread_iters,
	    res.timer_ok, res.time_mono_ok, res.ticks_mono_ok);
}
static const struct kernel_param_ops vms_ktest_cluster_seam_result_ops = {
	.get = vms_ktest_cluster_seam_result_get,
};
module_param_cb(vms_ktest_cluster_seam_result, &vms_ktest_cluster_seam_result_ops, NULL, 0444);
MODULE_PARM_DESC(vms_ktest_cluster_seam_result,
    "TEST-ONLY (rd FC-P0.2): read the last vms_ktest_cluster_seam_run result as KEY=VAL tokens");

/*
 * TEST-ONLY: the FC-P0.16 receive-level lock conformance self-test (rung R3,
 * design docs/design-faithful-cluster-executive.md SS3.2.3 RULING, CONTRACT
 * RULE 14.1). Compiled in under the SAME OVMX_KTEST_CLUSTER_SEAM gate as the
 * FC-P0.2 self-test above (out-of-tree QEMU-test vms.ko only, never the
 * bootable executive).
 *
 * WHAT THIS PROVES. It stands up the REAL cluster fork context
 * (vms_cluster_fork_start, the actual exec_rxlock_t + exec_cv_wait_rx path
 * from vms_cluster_fork_bind.c -- FC-P0.16's landing) and hammers its ONE
 * shared object, the fork queue, from BOTH sides CONTRACT RULE 14.1
 * distinguishes:
 *
 *   - RECEIVE LEVEL: a real 0x6007 frame flood over a veth pair, delivered to
 *     exec_lan_open's rx_cb in Linux's receive softirq -- vms_ktest_hammer_
 *     rx_bridge calls cf_rx_deliver() exactly as CONTRACT RULE 14.1(a) permits
 *     (copy/enqueue/wake, nothing else), copying the FC-P0.2 self-test's own
 *     already-proven veth-loopback mechanism.
 *   - PROCESS CONTEXT: a poster kthread calling cf_post() in a tight loop --
 *     CONTRACT RULE 14.1's "process-context posters ... rxlock briefly (post +
 *     signal)" row.
 *
 * SAME-CPU BY CONSTRUCTION. The flood and poster kthreads are explicitly
 * kthread_bind()'d to CPU 0; the harness this runs under (tests/qemu/
 * run_tests.sh) boots with -smp 1, so CPU 0 is the ONLY vCPU and every
 * receive-softirq delivery, every fork-thread dispatch and every poster
 * iteration genuinely share one core -- exactly the hazard design SS3.2.3
 * records (a process-context holder of the OLD exec_lock_t preempted by a
 * same-CPU softirq taking the same lock deadlocks solid; the fix is
 * spin_lock_irqsave, which this test exercises for real, not a mock).
 *
 * TWO-TIER LOCKUP DETECTION (INV-6: never a fabricated pass). After the flood
 * and poster stop, a BOUNDED poll (<=2s) waits for cf_stats' dispatched
 * counters to catch up with what was actually enqueued/posted -- the "every
 * posted item is observed within one scheduling latency" assertion, made
 * concrete. That bound cannot itself hang on a lost wakeup. The GENUINE
 * lockup detector is the vms_cluster_fork_stop() join immediately after: if
 * the rxlock fix ever regresses, the fork thread is stuck spinning against a
 * same-CPU holder and that call never returns -- the whole sysfs write hangs
 * honestly, caught by run_tests.sh's own wall timeout, rather than this code
 * reporting a pass it cannot back.
 *
 * DEFAULT DURATION vs THE FULL 60s. run_tests.sh shares ONE ~600s wall across
 * ~77 suites in a single QEMU boot (see that script's own header); a genuine
 * 60s hammer wired into the default per-PR battery would cost 10% of that
 * wall on every PR. vms_ktest_cluster_fork_hammer_run's THIRD field is a
 * duration in milliseconds (default 3000 if omitted, capped at 65000) so the
 * SAME real mechanism -- same code, same lock, same kthreads -- runs a short
 * proof by default and can be driven for the full 60s from a dedicated,
 * non-default invocation (tests/qemu/run_cluster_fork_hammer_60s.sh).
 */
struct vms_ktest_hammer_result {
	int done;
	int fork_start_ok;
	int open_ok;
	int l2_open_ok;
	unsigned int duration_ms;
	unsigned int elapsed_ms;
	unsigned int frames_sent;
	unsigned int posts_accepted;
	unsigned int posts_dropped_nobuf;
	int drain_ok;
	unsigned int drain_ms;
	unsigned long long st_rx_enqueued;
	unsigned long long st_rx_dropped_nobuf;
	unsigned long long st_rx_dispatched;
	unsigned long long st_work_posted;
	unsigned long long st_work_dropped_nobuf;
	unsigned long long st_work_dispatched;
	unsigned long long st_waits;

	/*
	 * The SERVED-I/O WORKER leg (FC-P6.6). This is the R3/R4 substrate
	 * proof that a BLOCKING call on the worker does not stall the fork
	 * thread: the io handler below really msleep()s -- the shape of
	 * exec_blockdev_read_block on a served volume -- while a real
	 * exec_timer cadence keeps firing into the fork thread.
	 */
	int                worker_start_ok;
	unsigned int       io_submitted;
	unsigned int       io_refused;
	unsigned int       io_handler_calls;
	unsigned int       io_done_seen;
	unsigned int       io_tag_mismatches;
	unsigned int       cadence_ticks;         /* expiries the fork thread ran */
	unsigned int       cadence_ticks_during_io;
	unsigned int       cadence_max_gap_ms;    /* the JITTER measurement      */
	unsigned int       work_during_io;        /* THE PROPERTY, robustly      */
	unsigned long long st_io_posted;
	unsigned long long st_io_completed;
	unsigned long long st_io_abandoned;
};
static struct vms_ktest_hammer_result vms_ktest_hammer_res;
static DEFINE_MUTEX(vms_ktest_hammer_mtx);

/* CONTRACT RULE 14.1(a) in miniature: the REAL receive-level entry point,
 * called from Linux's rx softirq. Copy/enqueue/wake and nothing else -- the
 * exact repertoire cf_rx_deliver() itself enforces. */
static void vms_ktest_hammer_rx_bridge(void *ctx, const uint8_t *frame, uint32_t len)
{
	(void)cf_rx_deliver((struct vms_cluster_fork *)ctx, frame, len);
}

/* The fork thread's own handlers -- run under the fork mutex, one at a time,
 * exactly like every other dispatched event. A bare counter, nothing more:
 * this test's assertions are on cf_stats, not on handler-side state. */
static void vms_ktest_hammer_rx_handler(void *ctx, const uint8_t *frame, uint32_t len)
{
	(void)frame; (void)len;
	atomic_inc((atomic_t *)ctx);
}

/*
 * THE SERVED-I/O WORKER LEG (FC-P6.6, design §3.2.6's E42 corollary).
 *
 * `io_busy` marks the window in which a worker callback is inside a BLOCKING
 * sleep -- exactly where exec_blockdev_read_block sits when a served READ is
 * in flight. The cadence handler below records how many of its expiries the
 * FORK THREAD ran during that window, and the largest gap between two of them.
 * Before FC-P6.6 that work ran ON the fork thread, so the count could only have
 * been zero and the gap could only have been the disk's own latency.
 */
struct vms_ktest_hammer_io_ctx {
	struct vms_cluster_fork *fork;
	unsigned long            deadline;    /* jiffies */
	atomic_t                 busy;        /* a callback is blocking now    */
	atomic_t                 calls;
	unsigned int             submitted;   /* this one thread only */
	unsigned int             refused;
	unsigned int             block_ms;
};

/* One cadence identity, a stand-in for the HELLO beat: a REAL exec_timer armed
 * through cf_timer_arm, re-armed by its own expiry handler in the fork thread,
 * exactly as vms_pe.c's cadence does. */
#define VMS_KTEST_HAMMER_CADENCE_WHICH 7u
#define VMS_KTEST_HAMMER_CADENCE_MS    10u

struct vms_ktest_hammer_cadence {
	struct vms_cluster_fork      *fork;
	struct vms_ktest_hammer_io_ctx *io;
	unsigned long                 last_jiffies;
	unsigned int                  ticks;
	unsigned int                  ticks_during_io;
	unsigned int                  max_gap_ms;
};

/* WORKER CONTEXT: it sleeps, which is the whole point. Returns the request's
 * own tag so the completion's two halves can be checked against each other. */
static uint32_t vms_ktest_hammer_io_handler(void *ctx, const struct cf_io *io)
{
	struct vms_ktest_hammer_io_ctx *ic = ctx;

	atomic_inc(&ic->busy);
	msleep(ic->block_ms);
	atomic_inc(&ic->calls);
	atomic_dec(&ic->busy);
	return io->tag;
}

/* FORK THREAD: the cadence expiry, which re-arms itself and measures its own
 * gap. A stall behind a served disk shows up here and nowhere else. */
static void vms_ktest_hammer_cadence_tick(struct vms_ktest_hammer_cadence *cd)
{
	unsigned long now = jiffies;

	if (cd->ticks) {
		unsigned int gap = jiffies_to_msecs(now - cd->last_jiffies);

		if (gap > cd->max_gap_ms)
			cd->max_gap_ms = gap;
	}
	cd->last_jiffies = now;
	cd->ticks++;
	if (atomic_read(&cd->io->busy))
		cd->ticks_during_io++;
	(void)cf_timer_arm(cd->fork, CF_OWNER_PE,
			   VMS_KTEST_HAMMER_CADENCE_WHICH, 0u,
			   VMS_KTEST_HAMMER_CADENCE_MS);
}

/* PROCESS CONTEXT: the submitter, standing in for the fork thread's own
 * srv_op_io_submit. */
static int vms_ktest_hammer_io_fn(void *arg)
{
	struct vms_ktest_hammer_io_ctx *ic = arg;
	struct cf_io io;
	uint32_t seq = 0;

	memset(&io, 0, sizeof(io));
	io.owner = CF_OWNER_PE;

	while (!kthread_should_stop() && time_before(jiffies, ic->deadline)) {
		cf_status_t st;

		seq++;
		io.tag = seq;
		st = cf_io_post(ic->fork, &io);
		if (st == CF_OK)
			ic->submitted++;
		else
			ic->refused++;
		msleep(1);   /* the worker is the bottleneck, not this loop */
	}
	return 0;
}

/*
 * The fork thread's work handler. Three kinds arrive: the hammer's own posts,
 * the cadence expiry (CF_WORK_TIMER) and the SERVED-I/O WORKER's completions
 * (CF_WORK_IO_DONE). All three run here, one at a time, under the fork mutex --
 * which is exactly the claim FC-P6.6 makes: the protocol stays serialised and
 * only the DISK WAIT moved.
 */
struct vms_ktest_hammer_work_ctx {
	atomic_t                        delivered;
	struct vms_ktest_hammer_cadence cadence;
	unsigned int                    io_done_seen;
	unsigned int                    io_tag_mismatches;
	/*
	 * THE ROBUST FORM OF THE MEASUREMENT: fork-thread dispatches that
	 * happened while a worker callback was BLOCKED. The cadence below is
	 * the same property expressed as a timer, but a timer expiry needs a
	 * free work item and this test deliberately floods the work pool, so a
	 * starved cadence is expected here and is NOT what the assertion rests
	 * on (vms_cluster_fork.c: "The tick is LOST -- counted, never faked").
	 */
	unsigned int                    work_during_io;
};

static void vms_ktest_hammer_work_handler(void *ctx, const struct cf_work *w)
{
	struct vms_ktest_hammer_work_ctx *wc = ctx;

	atomic_inc(&wc->delivered);
	if (atomic_read(&wc->cadence.io->busy))
		wc->work_during_io++;
	if (w->kind == CF_WORK_IO_DONE) {
		/* The handler returned the tag as its status, so a completion
		 * whose halves disagree means one of them was rewritten. */
		if (w->arg0 != w->arg1)
			wc->io_tag_mismatches++;
		wc->io_done_seen++;
	} else if (w->kind == CF_WORK_TIMER &&
		   w->arg0 == VMS_KTEST_HAMMER_CADENCE_WHICH) {
		vms_ktest_hammer_cadence_tick(&wc->cadence);
	}
}

struct vms_ktest_hammer_flood_ctx {
	exec_socket_t peer_sock;
	uint32_t      peer_ifindex;
	uint8_t       hwaddr_a[6];
	unsigned long deadline;   /* jiffies */
	unsigned int  sent;       /* touched only by this one thread */
};

/* The RECEIVE-LEVEL producer: floods real 0x6007 frames at the port's own
 * hwaddr over the veth pair. Every successful send drives a real softirq
 * delivery into vms_ktest_hammer_rx_bridge on the SAME (only) CPU this
 * kthread is bound to (CONTRACT RULE 1 / 14.1(a) territory) -- process
 * context is never touched by this thread. */
static int vms_ktest_hammer_flood_fn(void *arg)
{
	struct vms_ktest_hammer_flood_ctx *fc = arg;
	uint8_t frame[32];
	uint32_t seq = 0;

	memset(frame, 0, sizeof(frame));
	memcpy(frame + 0, fc->hwaddr_a, 6);
	frame[12] = 0x60; frame[13] = 0x07;      /* ethertype 0x6007 (SCA) */

	while (!kthread_should_stop() && time_before(jiffies, fc->deadline)) {
		seq++;
		frame[14] = (uint8_t)(seq);
		frame[15] = (uint8_t)(seq >> 8);
		frame[16] = (uint8_t)(seq >> 16);
		frame[17] = (uint8_t)(seq >> 24);
		if (exec_l2_send(fc->peer_sock, (int)fc->peer_ifindex, 0x6007u,
				  fc->hwaddr_a, frame, sizeof(frame)) >= 0)
			fc->sent++;
		if ((seq & 0x3fu) == 0)
			cond_resched();
	}
	return 0;
}

struct vms_ktest_hammer_post_ctx {
	struct vms_cluster_fork *fork;
	unsigned long            deadline;   /* jiffies */
	unsigned int             accepted;      /* touched only by this thread */
	unsigned int             dropped_nobuf; /* touched only by this thread */
};

/* The PROCESS-CONTEXT producer: cf_post() in a tight loop -- CONTRACT RULE
 * 14.1's "process-context posters ... rxlock briefly (post + signal)" row,
 * exercised for real against the same rxlock the flood above hammers from
 * receive level, on the same CPU. */
static int vms_ktest_hammer_post_fn(void *arg)
{
	struct vms_ktest_hammer_post_ctx *pc = arg;
	struct cf_work w;
	uint32_t seq = 0;

	memset(&w, 0, sizeof(w));
	w.owner = CF_OWNER_PE;
	w.kind  = 1;      /* a private "hammer post" kind; never CF_WORK_TIMER */

	while (!kthread_should_stop() && time_before(jiffies, pc->deadline)) {
		cf_status_t st;

		seq++;
		w.arg0 = seq;
		st = cf_post(pc->fork, &w);
		if (st == CF_OK)
			pc->accepted++;
		else if (st == CF_E_NOBUF)
			pc->dropped_nobuf++;
		if ((seq & 0x3fu) == 0)
			cond_resched();
	}
	return 0;
}

/*
 * Spawn one CPU-0-bound producer, holding a task reference for the caller.
 * Returns NULL on failure, which every caller treats as "that producer did not
 * run" rather than as a reason to abandon the test.
 */
static struct task_struct *vms_ktest_hammer_spawn(int (*fn)(void *), void *arg,
						  const char *name)
{
	struct task_struct *t = kthread_create(fn, arg, "%s", name);

	if (IS_ERR(t))
		return NULL;
	get_task_struct(t);      /* released by vms_ktest_hammer_reap */
	kthread_bind(t, 0);
	wake_up_process(t);
	return t;
}

/* Join one producer and release the reference its handle held. Safe on NULL
 * and on a thread that has already returned -- which is the ordinary case
 * here, since every producer exits on its own deadline. */
static void vms_ktest_hammer_reap(struct task_struct *t)
{
	if (!t)
		return;
	kthread_stop(t);
	put_task_struct(t);
}

static int vms_ktest_cluster_fork_hammer_run(const char *ifname_a,
					      const char *ifname_b,
					      unsigned int duration_ms)
{
	struct vms_ktest_hammer_result res;
	struct vms_cluster *cl;
	struct vms_ktest_hammer_flood_ctx fc;
	struct vms_ktest_hammer_post_ctx pc;
	struct vms_ktest_hammer_io_ctx ic;
	struct vms_ktest_hammer_work_ctx wc;
	struct task_struct *flood_t = NULL, *post_t = NULL, *io_t = NULL;
	atomic_t rx_delivered_ctr;
	unsigned long start_jiffies = 0, deadline = 0;
	unsigned long drain_start = 0;
	int rc;

	memset(&res, 0, sizeof(res));
	res.duration_ms = duration_ms;
	atomic_set(&rx_delivered_ctr, 0);
	memset(&fc, 0, sizeof(fc));
	memset(&pc, 0, sizeof(pc));
	memset(&ic, 0, sizeof(ic));
	memset(&wc, 0, sizeof(wc));
	atomic_set(&wc.delivered, 0);
	atomic_set(&ic.busy, 0);
	atomic_set(&ic.calls, 0);
	ic.block_ms = 5;   /* the shape of a served disk read */

	cl = kzalloc(sizeof(*cl), GFP_KERNEL);
	if (!cl) {
		res.done = 1;
		goto record;
	}

	/* This is vms_cluster_fork_bind.c's REAL start path: exec_rxlock_init,
	 * exec_cv_init, the real kthread. No fake ops, no shortcut. */
	rc = vms_cluster_fork_start(cl, NULL);
	res.fork_start_ok = (rc == SS__NORMAL);   /* SS$_ convention: odd == success */
	if (!res.fork_start_ok) {
		/* Honest end of the road (Rule 9): no thread, no test. */
		kfree(cl);
		res.done = 1;
		goto record;
	}

	cf_set_rx_handler(cl->fork, vms_ktest_hammer_rx_handler, &rx_delivered_ctr);
	wc.cadence.fork = cl->fork;
	wc.cadence.io = &ic;
	(void)cf_set_work_handler(cl->fork, CF_OWNER_PE,
				   vms_ktest_hammer_work_handler, &wc);

	/*
	 * The SERVED-I/O WORKER (FC-P6.6): the REAL second kthread, started by
	 * the REAL binding, with a callback that really blocks. Not available
	 * on a substrate without a §15 binding, which is an honest skip of this
	 * leg rather than a fabricated pass (Rule 9).
	 */
	(void)cf_set_io_handler(cl->fork, CF_OWNER_PE,
				vms_ktest_hammer_io_handler, &ic);
	res.worker_start_ok =
		(vms_cluster_fork_worker_start(cl) == SS__NORMAL);

	rc = exec_lan_open(ifname_a, 0x6007u, vms_ktest_hammer_rx_bridge, cl->fork);
	res.open_ok = (rc == 0);
	if (!res.open_ok)
		goto teardown_fork;

	rc = exec_lan_hwaddr(fc.hwaddr_a);
	if (rc != 0)
		goto teardown_lan;

	rc = exec_l2_open(ifname_b, 0x6007u, &fc.peer_ifindex, &fc.peer_sock);
	res.l2_open_ok = (rc == 0);
	if (!res.l2_open_ok)
		goto teardown_lan;

	start_jiffies = jiffies;
	deadline = start_jiffies + msecs_to_jiffies(duration_ms);
	fc.deadline = deadline;
	pc.fork = cl->fork;
	pc.deadline = deadline;
	ic.fork = cl->fork;
	ic.deadline = deadline;

	/* Arm the cadence BEFORE the producers start, from process context with
	 * the fork thread already running -- the same order vms_pe.c uses. */
	(void)cf_timer_arm(cl->fork, CF_OWNER_PE,
			   VMS_KTEST_HAMMER_CADENCE_WHICH, 0u,
			   VMS_KTEST_HAMMER_CADENCE_MS);

	/*
	 * Both bound to CPU 0: the harness's only vCPU. See the file-header
	 * comment above for why that makes "same-CPU" true by construction.
	 *
	 * EACH HANDLE HOLDS A TASK REFERENCE (vms_ktest_hammer_spawn). These
	 * producer bodies RETURN when their own deadline passes and are then
	 * kthread_stop()'d to be joined -- and on Linux a kthread that has
	 * returned is self-reaped, so without a held reference that join reads
	 * a freed task_struct. The FC-P6.6 QEMU run found exactly that
	 * (kthread_stop+0x48, refcount already 0); exec_kbackend_linux.h's
	 * SS15 binding carries the same fix for the executive's own threads.
	 */
	flood_t = vms_ktest_hammer_spawn(vms_ktest_hammer_flood_fn, &fc,
					 "vms_hammer_flood");
	post_t = vms_ktest_hammer_spawn(vms_ktest_hammer_post_fn, &pc,
					"vms_hammer_post");
	if (res.worker_start_ok)
		io_t = vms_ktest_hammer_spawn(vms_ktest_hammer_io_fn, &ic,
					      "vms_hammer_io");

	/* Let both threads run out their OWN deadline check before signaling a
	 * stop: kthread_stop() does not just join, it ACTIVELY requests an
	 * exit (sets the should_stop flag and wakes the target), so calling it
	 * immediately here would cut the hammer off after a few scheduler
	 * ticks instead of running it for duration_ms. */
	while (time_before(jiffies, deadline))
		msleep(10);

	/* Now the join: each thread has already (or is about to) notice its
	 * own deadline and return on its own; kthread_stop() here is the
	 * synchronization primitive that waits for that exit and reaps it. */
	vms_ktest_hammer_reap(flood_t);
	vms_ktest_hammer_reap(post_t);
	vms_ktest_hammer_reap(io_t);

	/* Stop the cadence before the drain poll: it re-arms itself, so a live
	 * cadence would keep the work queue non-empty forever and the bounded
	 * drain below would be measuring the wrong thing. */
	cf_timer_cancel(cl->fork, CF_OWNER_PE, VMS_KTEST_HAMMER_CADENCE_WHICH,
			0u);

	res.elapsed_ms = jiffies_to_msecs(jiffies - start_jiffies);
	res.frames_sent = fc.sent;
	res.posts_accepted = pc.accepted;
	res.posts_dropped_nobuf = pc.dropped_nobuf;
	res.io_submitted = ic.submitted;
	res.io_refused = ic.refused;

	/*
	 * The SERVED-I/O WORKER goes down FIRST and is JOINED: it is the only
	 * context that could still be inside a callback, and the readback below
	 * must be of a settled queue. Every OTHER teardown here already had
	 * that property; the worker is the new thing that does not, so it is
	 * made to.
	 */
	vms_cluster_fork_worker_stop(cl);
	/* Read AFTER the join, or the last callback to finish is missed. */
	res.io_handler_calls = (unsigned int)atomic_read(&ic.calls);

	/* The bounded drain proof -- see the file-header comment's "TWO-TIER
	 * LOCKUP DETECTION". */
	{
		unsigned long drain_deadline;
		struct cf_stats st;

		drain_start = jiffies;
		drain_deadline = drain_start + msecs_to_jiffies(2000);
		for (;;) {
			cf_stats_get(cl->fork, &st);
			if (st.rx_dispatched >= st.rx_enqueued &&
			    st.work_dispatched >= st.work_posted)
				break;
			if (!time_before(jiffies, drain_deadline))
				break;
			msleep(5);
		}
		res.drain_ok = (st.rx_dispatched >= st.rx_enqueued &&
				 st.work_dispatched >= st.work_posted);
		res.drain_ms = jiffies_to_msecs(jiffies - drain_start);
		res.st_rx_enqueued = st.rx_enqueued;
		res.st_rx_dropped_nobuf = st.rx_dropped_nobuf;
		res.st_rx_dispatched = st.rx_dispatched;
		res.st_work_posted = st.work_posted;
		res.st_work_dropped_nobuf = st.work_dropped_nobuf;
		res.st_work_dispatched = st.work_dispatched;
		res.st_waits = st.waits;
		res.st_io_posted = st.io_posted;
		res.st_io_completed = st.io_completed;
		res.st_io_abandoned = st.io_abandoned;
	}

	res.io_done_seen = wc.io_done_seen;
	res.io_tag_mismatches = wc.io_tag_mismatches;
	res.cadence_ticks = wc.cadence.ticks;
	res.cadence_ticks_during_io = wc.cadence.ticks_during_io;
	res.cadence_max_gap_ms = wc.cadence.max_gap_ms;
	res.work_during_io = wc.work_during_io;

	exec_socket_release(fc.peer_sock);
teardown_lan:
	exec_lan_close();
teardown_fork:
	/* THE genuine lockup detector: see the file-header comment. A real
	 * rxlock regression makes this call, not the bounded poll above,
	 * hang -- honestly, under run_tests.sh's own wall timeout. */
	vms_cluster_fork_stop(cl);
	kfree(cl);
	res.done = 1;

record:
	mutex_lock(&vms_ktest_hammer_mtx);
	vms_ktest_hammer_res = res;
	mutex_unlock(&vms_ktest_hammer_mtx);
	return 0;
}

/* "digits" only -- no libc/kstrtox dependency, matching this file's existing
 * minimal hand-rolled parsing style (vms_ktest_cluster_seam_set above). */
static unsigned int vms_ktest_hammer_parse_uint(const char *s)
{
	unsigned int v = 0;

	while (*s >= '0' && *s <= '9') {
		v = v * 10u + (unsigned int)(*s - '0');
		s++;
	}
	return v;
}

static int vms_ktest_cluster_fork_hammer_set(const char *val, const struct kernel_param *kp)
{
	char ifname_a[IFNAMSIZ] = {0};
	char ifname_b[IFNAMSIZ] = {0};
	unsigned int duration_ms = 3000;   /* default: fits run_tests.sh's shared wall */
	const char *sep1, *sep2;
	size_t blen;

	(void)kp;
	if (!val)
		return -EINVAL;
	sep1 = strchr(val, ':');
	if (!sep1 || (size_t)(sep1 - val) >= sizeof(ifname_a))
		return -EINVAL;
	memcpy(ifname_a, val, sep1 - val);

	sep2 = strchr(sep1 + 1, ':');
	if (sep2) {
		if ((size_t)(sep2 - (sep1 + 1)) >= sizeof(ifname_b))
			return -EINVAL;
		memcpy(ifname_b, sep1 + 1, sep2 - (sep1 + 1));
		duration_ms = vms_ktest_hammer_parse_uint(sep2 + 1);
		if (duration_ms == 0)
			duration_ms = 3000;
	} else {
		blen = strlen(sep1 + 1);
		if (blen && (sep1 + 1)[blen - 1] == '\n')
			blen--;                          /* strip a sysfs-write newline */
		if (blen >= sizeof(ifname_b))
			return -EINVAL;
		memcpy(ifname_b, sep1 + 1, blen);
	}

	if (duration_ms > 65000u)
		duration_ms = 65000u;   /* never let a bad value hang the harness forever */

	return vms_ktest_cluster_fork_hammer_run(ifname_a, ifname_b, duration_ms);
}
static const struct kernel_param_ops vms_ktest_cluster_fork_hammer_run_ops = {
	.set = vms_ktest_cluster_fork_hammer_set,
};
module_param_cb(vms_ktest_cluster_fork_hammer_run, &vms_ktest_cluster_fork_hammer_run_ops, NULL, 0200);
MODULE_PARM_DESC(vms_ktest_cluster_fork_hammer_run,
    "TEST-ONLY (rd FC-P0.16): run the receive-level lock conformance same-CPU hammer as \"ifname_a:ifname_b[:duration_ms]\" (default 3000, cap 65000)");

static int vms_ktest_cluster_fork_hammer_result_get(char *buf, const struct kernel_param *kp)
{
	struct vms_ktest_hammer_result res;

	(void)kp;
	mutex_lock(&vms_ktest_hammer_mtx);
	res = vms_ktest_hammer_res;
	mutex_unlock(&vms_ktest_hammer_mtx);

	return scnprintf(buf, PAGE_SIZE,
	    "DONE=%d FORK_START=%d OPEN=%d L2_OPEN=%d DURATION_MS=%u ELAPSED_MS=%u "
	    "FRAMES_SENT=%u POSTS_ACCEPTED=%u POSTS_DROPPED_NOBUF=%u DRAIN_OK=%d DRAIN_MS=%u "
	    "RX_ENQUEUED=%llu RX_DROPPED_NOBUF=%llu RX_DISPATCHED=%llu "
	    "WORK_POSTED=%llu WORK_DROPPED_NOBUF=%llu WORK_DISPATCHED=%llu WAITS=%llu "
	    "WORKER_START=%d IO_SUBMITTED=%u IO_REFUSED=%u IO_HANDLER_CALLS=%u "
	    "IO_DONE_SEEN=%u IO_TAG_MISMATCHES=%u CADENCE_TICKS=%u "
	    "CADENCE_TICKS_DURING_IO=%u CADENCE_MAX_GAP_MS=%u WORK_DURING_IO=%u "
	    "IO_POSTED=%llu IO_COMPLETED=%llu IO_ABANDONED=%llu\n",
	    res.done, res.fork_start_ok, res.open_ok, res.l2_open_ok,
	    res.duration_ms, res.elapsed_ms,
	    res.frames_sent, res.posts_accepted, res.posts_dropped_nobuf,
	    res.drain_ok, res.drain_ms,
	    res.st_rx_enqueued, res.st_rx_dropped_nobuf, res.st_rx_dispatched,
	    res.st_work_posted, res.st_work_dropped_nobuf, res.st_work_dispatched,
	    res.st_waits,
	    res.worker_start_ok, res.io_submitted, res.io_refused,
	    res.io_handler_calls, res.io_done_seen, res.io_tag_mismatches,
	    res.cadence_ticks, res.cadence_ticks_during_io,
	    res.cadence_max_gap_ms, res.work_during_io,
	    res.st_io_posted, res.st_io_completed, res.st_io_abandoned);
}
static const struct kernel_param_ops vms_ktest_cluster_fork_hammer_result_ops = {
	.get = vms_ktest_cluster_fork_hammer_result_get,
};
module_param_cb(vms_ktest_cluster_fork_hammer_result, &vms_ktest_cluster_fork_hammer_result_ops, NULL, 0444);
MODULE_PARM_DESC(vms_ktest_cluster_fork_hammer_result,
    "TEST-ONLY (rd FC-P0.16): read the last vms_ktest_cluster_fork_hammer_run result as KEY=VAL tokens");
#endif /* OVMX_KTEST_CLUSTER_SEAM */

/* ================================================================
 * Process management
 * ================================================================ */

struct vms_proc *vms_proc_find(pid_t pid)
{
    struct vms_proc *proc;

    rcu_read_lock();
    hash_for_each_possible_rcu(vms_proc_hash, proc, hash_node, pid) {
        if (proc->linux_pid == pid) {
            rcu_read_unlock();
            return proc;
        }
    }
    rcu_read_unlock();
    return NULL;
}

/*
 * vms_proc_find_or_err - this TASK'S process entry.
 *
 * KEYED ON THE THREAD GROUP, NOT THE THREAD (vms-9fc round 2).
 *
 * On OpenVMS a process has exactly ONE PCB and its kernel threads SHARE
 * it -- that shared residency is the entire meaning of a process-wide
 * event flag cluster, a process logical name table, a process name and a
 * process's lock ids. Keying this table on current->pid (the Linux TID)
 * minted one VMS process per THREAD: two threads of one image disagreed
 * about their own identity, could not see each other's event flags, and
 * could not release each other's locks. That is Rule 11's facade shape
 * inverted -- per-thread state pretending to be per-process -- and it is
 * not something VMS can be in.
 *
 * Linux's process-wide identifier is the thread-group id: current->tgid,
 * which is what getpid(2) returns, while current->pid is what gettid(2)
 * returns. So tgid is the key, and task_tgid() is the pinned identity.
 */
struct vms_proc *vms_proc_find_or_err(void)
{
    struct vms_proc *proc = vms_proc_find(current->tgid);

    /*
     * A table entry now outlives the /dev/vms channel (vms-8019), so an
     * entry keyed by this pid number is not automatically OURS: if the
     * pid has been recycled, the entry belongs to a process that no
     * longer exists, and handing it over would hand over its
     * privileges with it. Match on the pinned struct pid, which is
     * unique per process instance, rather than on the reusable number.
     */
    if (proc && proc->pid_ref != task_tgid(current))
        return NULL;

    return proc;
}

/*
 * vms_pid_counter - the executive's process-ID generator (vms-2b8).
 *
 * OVMX DESIGN CHOICE, labelled in vms_ioctl.h: OpenVMS builds a process
 * ID from a PCB-vector index plus a sequence number, and no public
 * document publishes that layout byte for byte, so OVMX does not
 * imitate it. What OVMX reproduces is the property that matters to
 * every caller -- the ID is assigned by the executive, is unique among
 * live processes, and is not handed straight back to the next process
 * when one exits.
 *
 * The base is deliberately above any value a Linux pid can take under
 * the kernel's own PID_MAX_LIMIT (2^22), so a VMS process ID is never
 * mistakable for the Linux pid it used to be copied from.
 */
#define VMS_PID_BASE    0x10000000u
static atomic_t vms_pid_counter = ATOMIC_INIT(0);

/*
 * assign_vms_pid - hand out an unused VMS process ID.
 *
 * MUST be called with vms_proc_hash_lock held, so that the uniqueness
 * scan and the insertion that follows it cannot be separated: two
 * concurrent registrations that each found "no clash" and then both
 * inserted would recreate the very collision this exists to prevent.
 *
 * A duplicate is possible only after 2^32 registrations have wrapped
 * the counter onto a still-live process, so the retry loop is a
 * correctness backstop rather than a hot path -- but it is not
 * optional: "unlikely" is not "unique", and a single collision lets
 * $GETJPI resolve one process's identity to another's row.
 */
static uint32_t assign_vms_pid(void)
{
    struct vms_proc *cur;
    uint32_t candidate;
    int bkt, attempts;

    for (attempts = 0; attempts < 1024; attempts++) {
        bool taken = false;

        candidate = VMS_PID_BASE +
                    (uint32_t)atomic_inc_return(&vms_pid_counter);
        if (candidate == 0)
            continue;   /* 0 is "no process"; never hand it out */

        hash_for_each(vms_proc_hash, bkt, cur, hash_node) {
            if (cur->vms_pid == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return candidate;
    }
    return 0;   /* table is pathologically full: refuse to register */
}

/*
 * vms_proc_continue_identity - stamp this task's registered VMS parent's
 * identity onto a not-yet-inserted child PCB (vms-4d7, Option B).
 *
 * This is what makes OVMX's fork-per-image invisible to VMS. On OpenVMS an
 * activated image runs IN the current process; OVMX runs it in a fresh
 * forked+exec'd process, so the executive is told (via
 * VMS_IOCTL_REGISTER_CONTINUE, selected by kif_bind when DCL activated the
 * image) to CONTINUE the parent rather than mint a new PCB.
 *
 * THE IDENTITY IS READ FROM THE PARENT'S PCB, NOT FROM THE CALLER. The
 * caller declares only the RELATIONSHIP ("continue my parent"); which
 * process is the parent comes from current->real_parent, which a process
 * cannot forge, and the UIC/username/privileges come from that parent's
 * executive row. This is the same discipline as UIC derivation: a derived
 * fact, never an asserted one -- so it is not the self-declared-identity
 * facade VMS_IOCTL_REGISTER's args were stripped to remove.
 *
 * SECURITY: the parent's CURRENT masks are copied. A parent that
 * setident'd DOWN to an unprivileged identity has reduced masks, so
 * continuing it cannot bring a privilege back -- the reduction now
 * survives image activation, where under the derive-from-capable() path it
 * did not.
 *
 * LOCKING: hash_lock covers uic/username/prcnam/terminal; mode_lock covers
 * the privilege masks. Taken hash-outer then mode-inner, the same order as
 * vms_ioctl_setident(), so an identity is never torn. proc is not yet in
 * the hash, so its own fields need no lock.
 *
 * Returns TRUE when a registered VMS parent was found and its identity was
 * copied onto `proc`, FALSE when the task has no registered VMS parent -- in
 * which case the caller derives an identity the ordinary way. Independently,
 * *shared_vms_pid_out is set to the parent's VMS PID when share_pid is true and
 * a parent was found (the child SHARES it -- image activation, _CONTINUE), and
 * left 0 otherwise (the caller then mints a FRESH VMS PID -- a genuinely new
 * subprocess that inherits identity but NOT the PID, _SUBPROCESS/vms-19e9). The
 * "inherited" answer is returned SEPARATELY from the shared PID precisely
 * because the subprocess case inherits identity yet shares no PID: a 0 shared
 * PID no longer means "did not inherit."
 */
static bool vms_proc_continue_identity(struct vms_proc *proc, bool share_pid,
                                       uint32_t *shared_vms_pid_out)
{
    struct task_struct *rp;
    struct pid *parent_pid = NULL;
    pid_t parent_tgid = 0;
    struct vms_proc *parent;
    uint32_t shared_vms_pid = 0;
    bool inherited = false;

    rcu_read_lock();
    rp = rcu_dereference(current->real_parent);
    if (rp) {
        parent_pid  = task_tgid(rp);
        parent_tgid = task_tgid_nr(rp);
    }
    if (!parent_pid) {
        rcu_read_unlock();
        if (shared_vms_pid_out)
            *shared_vms_pid_out = 0;
        return false;
    }

    spin_lock(&vms_proc_hash_lock);
    hash_for_each_possible(vms_proc_hash, parent, hash_node, parent_tgid) {
        if (parent->linux_pid != parent_tgid)
            continue;
        /*
         * Recycle-safe: an entry keyed by this pid NUMBER is ours to
         * continue only if it belongs to this same parent INSTANCE. The
         * pinned struct pid is unique per instance; the number is reused.
         * (Same check as vms_proc_find_or_err.)
         */
        if (parent->pid_ref != parent_pid)
            continue;

        proc->uic = parent->uic;
        memcpy(proc->username, parent->username, sizeof(proc->username));
        memcpy(proc->prcnam,   parent->prcnam,   sizeof(proc->prcnam));
        memcpy(proc->terminal, parent->terminal, sizeof(proc->terminal));

        /*
         * CLI invocation context (vms-f60d): an image inherits the invoking
         * CLI's command line and cliflag from the CLI's PCB, the same way it
         * inherits identity. This is what lets DCL set the context once
         * (VMS_IOCTL_SETCLI) and every image it activates read its OWN
         * invoking command line back (VMS_IOCTL_GETCLI) from the executive,
         * never a Linux env-var shim (INV-6). The image's own completion
         * $STATUS is NOT inherited -- each image records its own via
         * VMS_IOCTL_SETEXIT -- so exit_status/has_exit_status are left as the
         * kmem_cache_zalloc zero.
         */
        proc->cli_present = parent->cli_present;
        proc->cli_length  = parent->cli_length;
        memcpy(proc->cli_command, parent->cli_command, sizeof(proc->cli_command));

        spin_lock(&parent->mode_lock);
        proc->perm_privs = parent->perm_privs;
        proc->cur_privs  = parent->cur_privs;
        spin_unlock(&parent->mode_lock);

        /*
         * SHARE the parent's VMS PID only for an image-activation CONTINUE
         * (share_pid). A subprocess (_SUBPROCESS, vms-19e9) inherits the
         * identity above but leaves shared_vms_pid 0, so the caller mints it a
         * FRESH, distinct VMS PID -- a genuinely new VMS process, as $CREPRC
         * creates.
         */
        if (share_pid)
            shared_vms_pid = parent->vms_pid;
        inherited = true;
        break;
    }
    spin_unlock(&vms_proc_hash_lock);
    rcu_read_unlock();

    if (shared_vms_pid_out)
        *shared_vms_pid_out = shared_vms_pid;
    return inherited;
}

/*
 * vms_proc_parent_job_id - the JOB this registration belongs to (vms-aba,
 * LNM$JOB residency), inherited from the DIRECT parent's PCB if it already
 * has one.
 *
 * A VMS job is a top-level process (an interactive login, a detached
 * process) plus every subprocess it creates -- SPAWN's child stays in the
 * parent's job (documented VMS behaviour: DCL Dictionary, SPAWN; System
 * Services Reference, $CREPRC's JOBCTL parameter). This mirrors that
 * inheritance rule directly against the task hierarchy: a task whose real
 * parent is ALREADY a registered VMS process inherits that parent's
 * job_id, whether this registration is an image-activation CONTINUE of the
 * very same VMS process (vms_proc_continue_identity(), just above) or a
 * genuinely new PCB such as a SPAWNed subprocess -- either way the child
 * is part of the parent's job. Returns 0 when the real parent has no
 * registered PCB, which the caller reads as "this task is a job root."
 *
 * Same read-the-parent's-row-never-the-caller's-word discipline as
 * vms_proc_continue_identity(): a process cannot forge which job it
 * belongs to any more than it can forge its own UIC.
 */
static uint32_t vms_proc_parent_job_id(void)
{
    struct task_struct *rp;
    struct pid *parent_pid = NULL;
    pid_t parent_tgid = 0;
    struct vms_proc *parent;
    uint32_t job_id = 0;

    rcu_read_lock();
    rp = rcu_dereference(current->real_parent);
    if (rp) {
        parent_pid  = task_tgid(rp);
        parent_tgid = task_tgid_nr(rp);
    }
    if (!parent_pid) {
        rcu_read_unlock();
        return 0;
    }

    spin_lock(&vms_proc_hash_lock);
    hash_for_each_possible(vms_proc_hash, parent, hash_node, parent_tgid) {
        if (parent->linux_pid != parent_tgid)
            continue;
        /* Recycle-safe: see vms_proc_continue_identity(). */
        if (parent->pid_ref != parent_pid)
            continue;

        job_id = parent->job_id;
        break;
    }
    spin_unlock(&vms_proc_hash_lock);
    rcu_read_unlock();

    return job_id;
}

/*
 * vms_proc_inherit_channels - copy the registering task's VMS parent's open
 * BGn: channels into this fresh child PCB (vms-3bf, executive fork/exec
 * inheritance of BG channels).
 *
 * INHERIT THE CHANNEL, NOT THE IDENTITY. This is deliberately separate from
 * vms_proc_continue_identity(): the child keeps its OWN PCB and its own
 * identity (UIC, user name, privilege masks, VMS PID) -- the per-task fresh
 * accounting that vms-8019/2b8/4d7 rests on is untouched. What the child gains
 * is only the ability to operate the parent's BG channels BY NUMBER, exactly as
 * a Linux child inherits the parent's open fds across fork() without inheriting
 * the parent's credentials. vms_bg_inherit() copies channel-table entries and
 * refcount-shares the host sockets; it writes none of the identity fields.
 *
 * Same read-the-parent's-row discipline as the two helpers above: the child
 * declares nothing; which process is the parent comes from current->real_parent
 * (unforgeable), which survives both fork() and execve(), so the exec'd image's
 * first registration still finds the channel-owning parent. A task with no
 * registered VMS parent -- or a parent that holds no BG channels -- inherits
 * nothing, and a later $QIO on a channel it never received fails honestly with
 * SS$_IVCHAN (INV-6: no fabricated inheritance).
 *
 * LOCKING: vms_proc_hash_lock is held across the copy so the parent PCB cannot
 * be freed under us; vms_bg_inherit() takes the parent's chan_lock nested
 * inside it (hash-outer, chan-inner; BG ioctls never take hash_lock, so no
 * inverse order exists). child is not yet in the hash, so it needs no lock.
 */
/*
 * vms_proc_capture_channels_for_task - for the fork-inherit rind (vms-0cd). If
 * `parent_task`'s thread group has a registered PCB, capture a kref'd snapshot of
 * its BG channels onto `out` (a caller-owned empty list). Returns nonzero iff
 * anything was captured. Holds vms_proc_hash_lock so the parent PCB cannot be freed
 * mid-capture -- safe to call from the atomic sched_process_fork tracepoint. Same
 * hash-outer / chan-inner order as vms_proc_inherit_channels below.
 */
int vms_proc_capture_channels_for_task(struct task_struct *parent_task,
                                       struct list_head *out, uint32_t *out_next_chan)
{
    struct vms_proc *parent;
    pid_t ptgid = task_tgid_nr(parent_task);
    struct pid *ppid = task_tgid(parent_task);

    spin_lock(&vms_proc_hash_lock);
    hash_for_each_possible(vms_proc_hash, parent, hash_node, ptgid) {
        if (parent->linux_pid != ptgid)
            continue;
        if (parent->pid_ref != ppid)     /* recycle-safe: same INSTANCE only */
            continue;
        (void)vms_bg_capture_channels(parent, out, out_next_chan);
        break;
    }
    spin_unlock(&vms_proc_hash_lock);
    return !list_empty(out);
}

static void vms_proc_inherit_channels(struct vms_proc *child)
{
    struct task_struct *rp;
    struct pid *parent_pid = NULL;
    pid_t parent_tgid = 0;
    struct vms_proc *parent;

    /*
     * vms-0cd: prefer the FORK-TIME snapshot, captured before an accept->fork->close
     * forking server (sshd, inetd) could close the listener's copy of the accepted
     * connection. Falls through to the #815 real_parent-at-registration snapshot
     * below when there is no fork record (the parent-stays-open case, which both
     * paths handle -- the fork record just wins the race when the parent closes
     * early).
     */
    if (vms_bg_forkinherit_consume(child))
        return;

    rcu_read_lock();
    rp = rcu_dereference(current->real_parent);
    if (rp) {
        parent_pid  = task_tgid(rp);
        parent_tgid = task_tgid_nr(rp);
    }
    if (!parent_pid) {
        rcu_read_unlock();
        return;
    }

    spin_lock(&vms_proc_hash_lock);
    hash_for_each_possible(vms_proc_hash, parent, hash_node, parent_tgid) {
        if (parent->linux_pid != parent_tgid)
            continue;
        /* Recycle-safe: continue only the same parent INSTANCE (pinned struct
         * pid), never a reused pid number. Same check as the helpers above. */
        if (parent->pid_ref != parent_pid)
            continue;
        vms_bg_inherit(child, parent);
        break;
    }
    spin_unlock(&vms_proc_hash_lock);
    rcu_read_unlock();
}

struct vms_proc *vms_proc_register(pid_t pid, bool inherit_identity,
                                   bool share_pid)
{
    struct vms_proc *existing, *proc;
    uint32_t shared_vms_pid = 0;
    bool inherited = false;
    uint32_t parent_job_id;
    int i;

    /*
     * Read before the PCB exists and before any lock this function takes
     * below -- vms_proc_parent_job_id() takes and releases
     * vms_proc_hash_lock itself, the same pattern vms_proc_continue_identity()
     * uses just below.
     */
    parent_job_id = vms_proc_parent_job_id();

    proc = kmem_cache_zalloc(vms_proc_cache, GFP_KERNEL);
    if (!proc)
        return ERR_PTR(-ENOMEM);

    proc->linux_pid = pid;
    proc->current_mode = PSL_C_USER;    /* start in user mode */

    /*
     * Executive-owned identity (vms-8019).
     *
     * The process starts unnamed; $SETPRN (VMS_IOCTL_SETPRN) names it.
     * The UIC is derived here from the task's own credentials -- it is
     * deliberately NOT taken from the register arguments, because a
     * process that can declare its own UIC can forge the group that
     * scopes process-name uniqueness and lookup. UIC [group,member] is
     * packed as (group << 16) | member, the packing sys$getjpi's
     * JPI$_UIC item returns, with OVMX's [gid,uid] mapping.
     */
    proc->prcnam[0] = '\0';
    proc->uic = (((uint32_t)from_kgid(&init_user_ns, current_gid()) & 0xFFFFu) << 16) |
                ((uint32_t)from_kuid(&init_user_ns, current_uid()) & 0xFFFFu);

    /*
     * No user name yet (vms-2b8). A registered process is not an
     * AUTHENTICATED process: registration proves only that a task
     * exists. The name arrives with VMS_IOCTL_SETIDENT, after something
     * that holds privilege has checked SYSUAF -- which is why the
     * executive can report it as an identity rather than a claim.
     */
    memset(proc->username, 0, sizeof(proc->username));

    /*
     * Pin the backing PROCESS's pid so the entry's liveness can be tested
     * without racing pid reuse. task_tgid(), not task_pid(): the entry
     * belongs to the whole thread group and must outlive any one thread
     * in it (see vms_proc_find_or_err). Released in vms_proc_free().
     */
    proc->pid_ref = get_pid(task_tgid(current));

    /*
     * THE AUTHORIZED MASK IS DERIVED, NOT REQUESTED (vms-2b8).
     *
     * This used to clamp a mask the PROCESS supplied. Clamping an
     * attacker-supplied value is still trusting an attacker-supplied
     * value in the unclamped case, and the unclamped case was "the
     * process is CAP_SYS_ADMIN" -- so every privileged process got
     * exactly the privileges it asked for, which is the honor system
     * this item exists to remove. src/ovmx_init/ovmx_init.c asked for
     * 0xFFFFFFFFFFFFFFFF and got it.
     *
     * Now nothing is supplied. capable(CAP_SYS_ADMIN) is a REAL kernel
     * credential read from the task -- a process cannot grant itself
     * CAP_SYS_ADMIN, exactly as it cannot grant itself the uid and gid
     * the UIC above is derived from. That is the whole difference
     * between a derived fact and an asserted one.
     *
     * The privileged case gets the enforced set rather than all 64 bits
     * (CLAUDE.md Rule 10, and this item's constraint): the executive
     * hands out only privileges it will actually refuse an operation
     * over. Privileges beyond that arrive from SYSUAF through
     * VMS_IOCTL_SETIDENT, where they are stored and reported because
     * VMS reports them -- but they are never conjured at registration.
     */
    /*
     * INHERIT THE PARENT'S IDENTITY, OR DERIVE (vms-4d7 Option B; vms-19e9).
     *
     * When this registration inherits an already-registered VMS parent's
     * identity, the identity above is REPLACED by the parent's -- UIC, user
     * name, process name, terminal and both privilege masks. Two shapes:
     *   - IMAGE ACTIVATION (VMS_IOCTL_REGISTER_CONTINUE): share_pid true, so
     *     the child ALSO shares the parent's VMS PID -- DCL and the image it
     *     activated are one VMS process.
     *   - SUBPROCESS (VMS_IOCTL_REGISTER_SUBPROCESS, SPAWN/$CREPRC): share_pid
     *     false, so the child inherits the identity but is minted a FRESH,
     *     distinct VMS PID below -- a genuinely new VMS process.
     * See vms_proc_continue_identity(); it reads the parent's row, never the
     * caller's word, and copies the parent's CURRENT (possibly reduced) masks
     * so a setident-down cannot be undone by a fork. `inherited` (not a zero
     * shared PID) is what says the identity came from the parent: a subprocess
     * inherits identity yet shares no PID.
     */
    if (inherit_identity)
        inherited = vms_proc_continue_identity(proc, share_pid,
                                               &shared_vms_pid);

    if (!inherited) {
        proc->perm_privs = capable(CAP_SYS_ADMIN)
                         ? (VMS_PRV_M_ENFORCED | VMS_DEFAULT_PRIVS)
                         : VMS_DEFAULT_PRIVS;
        proc->cur_privs = proc->perm_privs;
    }
    /* image_active/pre_image_mode (vms-68f.iii): kmem_cache_zalloc() above
     * already zeroed both, so a fresh process starts with no controlled
     * descent open, exactly what VMS_IOCTL_IMAGE_RUNDOWN's guard needs. */
    spin_lock_init(&proc->mode_lock);

    /* Initialize AST queues */
    for (i = 0; i < 4; i++) {
        INIT_LIST_HEAD(&proc->ast[i].pending);
        proc->ast[i].count = 0;
        proc->ast[i].enabled = 1;  /* enabled by default */
        spin_lock_init(&proc->ast[i].lock);
    }

    /* Hibernate/wake + async AST-delivery wakeup (vms-feb). wake_pending was
     * zeroed by kmem_cache_zalloc() above; the wait queue and its paired lock
     * need explicit init. */
    init_waitqueue_head(&proc->hiber_wq);
    spin_lock_init(&proc->hiber_lock);

    /* Initialize event flags */
    proc->ef.local[0] = 0;
    proc->ef.local[1] = 0;
    proc->ef.common[0] = NULL;
    proc->ef.common[1] = NULL;
    init_waitqueue_head(&proc->ef.waitq);
    spin_lock_init(&proc->ef.lock);

    /* Initialize lock list */
    INIT_LIST_HEAD(&proc->locks);
    proc->lock_count = 0;
    spin_lock_init(&proc->lock_list_lock);

    /* Initialize the I/O channel list (device table, vms-d0b) */
    INIT_LIST_HEAD(&proc->channels);
    proc->next_chan = 0;
    spin_lock_init(&proc->chan_lock);

    /* Mailbox channels (vms-d44) -- a separate list, same chan_lock and
     * next_chan counter as the device channels above (vms_mbx.h). */
    INIT_LIST_HEAD(&proc->mbx_channels);

    /* INET pseudo-device channels (BGn:, vms-527) -- likewise a separate list
     * on the same chan_lock and next_chan counter (vms_bg.h). */
    INIT_LIST_HEAD(&proc->bg_channels);

    /* Files-11 (ODS-2) ACP file-class channels (vms-149) -- likewise a separate
     * list on the same chan_lock and next_chan counter (vms_acp.h). */
    INIT_LIST_HEAD(&proc->file_channels);

    /* L2 (raw datalink) socket handles (vms-7eb, auth slice of vms-1e4) -- a
     * separate list on its OWN dedicated lock, NOT the chan_lock/next_chan
     * space above (an L2 handle is not a $ASSIGN channel; see vms_l2.h). */
    INIT_LIST_HEAD(&proc->l2_channels);
    spin_lock_init(&proc->l2_lock);

    /* P0 program region (vms-68f.i): unmapped until VMS_IOCTL_P0_MAP
     * records an extent. kmem_cache_zalloc() above already zeroed
     * p0_base/p0_limit; only the lock needs initializing. */
    spin_lock_init(&proc->p0_lock);

    /* P1 control region (vms-68f.ii): unregistered until VMS_IOCTL_P1_MAP
     * records an extent. Separate lock from p0_lock -- see the p1_lock
     * comment in vms_internal.h for why that separation is the mechanism
     * behind "P0 deleted on rundown, P1 survives", not decoration.
     * kmem_cache_zalloc() above already zeroed p1_base/p1_limit. */
    spin_lock_init(&proc->p1_lock);

    /*
     * EXECUTIVE FORK/EXEC INHERITANCE OF BG CHANNELS (vms-3bf). Now that the
     * child's channel lists and chan_lock are initialised (empty), copy in the
     * parent's open BGn: channels -- by number, host socket refcount-shared --
     * so a forked (and fork+exec'd) child can operate a connection its parent
     * accepted, the fd-inheritance analogue a stock forking server (sshd's
     * privsep sshd-session) needs over BGn:. Done here, before the child is
     * published in the hash, so no reader observes a half-filled channel list;
     * identity is NOT inherited -- the child keeps its own PCB (see
     * vms_proc_inherit_channels). Unrelated tasks inherit nothing -> honest
     * SS$_IVCHAN downstream, never a fabricated channel (INV-6).
     */
    vms_proc_inherit_channels(proc);

    /* Atomically check-and-insert under spinlock to avoid TOCTOU race */
    spin_lock(&vms_proc_hash_lock);
    hash_for_each_possible_rcu(vms_proc_hash, existing, hash_node, pid) {
        if (existing->linux_pid == pid) {
            spin_unlock(&vms_proc_hash_lock);
            /* Release any BG channels vms_proc_inherit_channels() copied in,
             * dropping their shared host-socket references, before discarding
             * this never-published PCB -- otherwise the inherited socket refs
             * leak (vms-3bf). */
            vms_bg_release_all(proc);
            put_pid(proc->pid_ref);
            kmem_cache_free(vms_proc_cache, proc);
            return ERR_PTR(-EEXIST);
        }
    }
    /*
     * THE VMS PROCESS ID IS ASSIGNED HERE, UNDER THE SAME LOCK AS THE
     * INSERTION (vms-2b8 round 3). It used to be copied from the
     * register arguments -- i.e. chosen by the process -- with no
     * uniqueness check at all, so two processes could share one VMS PID
     * and $GETJPI by that PID returned whichever the hash walk reached
     * first. Choosing it here, inside the critical section, is what
     * makes "unique among live processes" true rather than likely.
     */
    /*
     * A CONTINUED image activation SHARES its parent's VMS PID rather than
     * minting a new one (vms-4d7): the two Linux tasks are one VMS process,
     * so $GETJPI must resolve either to the same identity. This is not the
     * duplicate-PID defect vms-2b8 round 3 removed -- that was two DIFFERENT
     * identities under one PID; here both rows carry the SAME identity by
     * construction. assign_vms_pid()'s uniqueness scan still protects new
     * processes: it walks the live table, so it will never hand a fresh
     * process a PID a live continued child is already sharing.
     */
    proc->vms_pid = shared_vms_pid ? shared_vms_pid : assign_vms_pid();
    if (proc->vms_pid == 0) {
        spin_unlock(&vms_proc_hash_lock);
        /* Same inherited-channel unwind as the -EEXIST path above (vms-3bf). */
        vms_bg_release_all(proc);
        put_pid(proc->pid_ref);
        kmem_cache_free(vms_proc_cache, proc);
        return ERR_PTR(-ENOSPC);
    }
    /*
     * JOB DERIVATION (vms-aba). Inherit the parent's job if it has one;
     * otherwise this registration is a job root and its job IS its own
     * freshly assigned vms_pid -- exactly as a new VMS job's ID is the PID
     * of the process that started it. Finalised here, inside the same
     * critical section as vms_pid, and before hash_add_rcu() publishes the
     * entry, so no reader can observe a zero/unset job_id.
     */
    proc->job_id = parent_job_id ? parent_job_id : proc->vms_pid;
    hash_add_rcu(vms_proc_hash, &proc->hash_node, pid);
    spin_unlock(&vms_proc_hash_lock);

    /*
     * ROUTINE PER-PROCESS TRACE -- pr_debug, NOT pr_info (vms-2213). Every
     * process registration emitted this at pr_info, so a single interactive
     * login (LOGINOUT -> DCL -> each spawned image) sprayed several
     * "vms: registered process ..." lines onto the operator console during
     * the login flow -- kernel-driver chatter real VMS never shows there.
     * This line is routine per-object diagnostic trace, so it belongs at
     * debug level: it stays available through the kernel log (dmesg with
     * dynamic-debug enabled for this module) but does not reach the console.
     * Genuine warnings/errors elsewhere in this module keep pr_warn/pr_err.
     */
    pr_debug("vms: registered process pid=%d vms_pid=0x%08x uic=[%o,%o] job=0x%08x privs=0x%llx (%s)\n",
             pid, proc->vms_pid, proc->uic >> 16, proc->uic & 0xFFFFu,
             proc->job_id, proc->perm_privs,
             shared_vms_pid ? "continued" : (inherited ? "subprocess" : "derived"));

    return proc;
}

/*
 * vms_proc_free_claimed - tear down an entry already removed from the hash.
 *
 * The caller must have unlinked proc under vms_proc_hash_lock; that
 * removal IS the ownership claim, so exactly one caller reaches here
 * per entry. Callers that still need to claim go through
 * vms_proc_free().
 */
void vms_proc_free_claimed(struct vms_proc *proc)
{
    int i;
    struct vms_ast_entry *ast, *tmp;

    /* Free AST queues */
    for (i = 0; i < 4; i++) {
        spin_lock(&proc->ast[i].lock);
        list_for_each_entry_safe(ast, tmp, &proc->ast[i].pending, list) {
            list_del(&ast->list);
            kfree(ast);
        }
        spin_unlock(&proc->ast[i].lock);
    }

    /* Release all locks */
    vms_proc_release_locks(proc);

    /* Release common event flag associations */
    vms_proc_release_common_ef(proc);

    /*
     * Give back every I/O channel. The devices themselves belong to
     * the executive and outlive the process; what dies here is this
     * process's claim on them, which is what releases device
     * ownership when the last channel goes.
     */
    vms_proc_release_channels(proc);

    /*
     * Give back every INET pseudo-device channel (BGn:, vms-527) and release
     * its host socket. Called here rather than from vms_proc_release_channels
     * (kernel-core) because the BG driver is host-socket glue that the
     * substrate-agnostic core must not name -- see vms_bg.c's header.
     */
    vms_bg_release_all(proc);

    /* Give back every L2 handle (and its host socket) too (vms-7eb) -- exactly
     * as the BG channels above, on its own dedicated lock. */
    vms_l2_release_all(proc);

    /* Give back this process's Files-11 ACP file-class channels too (vms-149),
     * dropping each mounted volume's assigned-channel refcount -- exactly as
     * the mailbox and BG channels above. */
    vms_acp_release_all(proc);

    /* Clear this process's $SETCLUEVT registration, if any (FC-P3.8) -- a
     * no-op when the cluster stack never started or this process never
     * registered; `proc` is passed as `void *` so vms_internal.h's own
     * declaration needs no vms_cluster.h include. */
    vms_cnxman_proc_gone(vms_cluster_node(), (void *)proc);

    /* Drop the pinned pid reference taken at registration */
    if (proc->pid_ref) {
        put_pid(proc->pid_ref);
        proc->pid_ref = NULL;
    }

    /* RCU-deferred free — proc may still be accessed by RCU readers */
    kfree_rcu(proc, rcu);
}

void vms_proc_free(struct vms_proc *proc)
{
    /*
     * Claim the entry: an entry can now be freed from three places
     * (channel release of an exiting task, the lazy reaper, and module
     * unload), so removal from the hash is what decides ownership.
     * hash_del_rcu() leaves the node unhashed, so a second claimant
     * bails out here instead of double-freeing.
     */
    spin_lock(&vms_proc_hash_lock);
    if (hlist_unhashed(&proc->hash_node)) {
        spin_unlock(&vms_proc_hash_lock);
        return;
    }
    hash_del_rcu(&proc->hash_node);
    /*
     * /NOWAIT spawn completion on ABNORMAL subprocess deletion (vms-2a4). This
     * is the claim point a SIGKILLed/crashed subprocess reaches FIRST on Linux:
     * do_exit closes its /dev/vms channel -> vms_dev_release -> here, before the
     * lazy reaper ever runs. If the dying process never recorded an exit but a
     * parent armed a completion on it, fire it now under the SAME hash_lock that
     * unlinked it (one-shot; a no-op if already delivered). Without this the
     * common Linux SIGKILL case would drop the notification and hang the parent.
     */
    vms_proc_deliver_abnormal_completion(proc);
    spin_unlock(&vms_proc_hash_lock);

    vms_proc_free_claimed(proc);
}

/* ================================================================
 * ioctl dispatch
 * ================================================================ */

static long vms_ioctl_register(unsigned long arg, bool inherit_identity,
                               bool share_pid)
{
    struct vms_register_args args;
    struct vms_proc *proc;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    /*
     * Clear out entries whose process is gone before claiming a slot,
     * so a recycled pid never collides with a dead predecessor.
     */
    vms_proc_reap_dead();

    /*
     * NOTHING FROM args IS READ. The struct is output-only: the privilege
     * mask went in the first round of vms-2b8 and the VMS process ID went
     * in the third. A registration that takes no input from the process
     * cannot be steered by one.
     *
     * ADOPT, do not recreate, and do not report an error (vms-9fc).
     *
     * This used to answer 0x1C for a task that already had an entry, which
     * made registration a once-per-image operation. It is not: the executive
     * entry belongs to the PROCESS and survives execve(), so the very next
     * image activated in the same process would be told "already registered"
     * and, having nothing else to do with that, would carry on unregistered.
     *
     * ORACLE PIN (reference lab node VAX1, OpenVMS VAX V7.3, 2026-07-30):
     * activating an image inside an existing process does not recreate the
     * process and is not an error. SHOW PROCESS/ACCOUNTING before and after
     * two further image activations reports
     *     Process ID: 2020021D   Process name: "SYSTEM"   Images activated: 19
     *     Process ID: 2020021D   Process name: "SYSTEM"   Images activated: 21
     * -- same PCB, same identity, same connect time, images activated simply
     * counts up. So the VMS-faithful answer to "register a process that
     * already exists" is to hand back the process that already exists --
     * INCLUDING the VMS process ID it was already assigned. Minting a second
     * ID for the same process would make one process answer to two, which is
     * the collision defect vms-2b8 round 3 removed, arriving from the other
     * direction.
     */
    proc = vms_proc_find_or_err();
    if (proc) {
        args.vms_pid = proc->vms_pid;
        args.status = 0x00000001;  /* SS$_NORMAL */
        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;
        return 0;
    }

    /* current->tgid, not current->pid: one PCB per process, shared by
     * every thread in it (see vms_proc_find_or_err). */
    proc = vms_proc_register(current->tgid, inherit_identity, share_pid);
    if (IS_ERR(proc)) {
        if (PTR_ERR(proc) != -EEXIST)
            return PTR_ERR(proc);   /* -ENOMEM -> SS$_INSFMEM at the boundary */

        /*
         * Lost the insert race, or the hash holds an entry for this pid
         * number that is NOT ours (a recycled pid the reaper missed).
         * Re-resolve through the pid-identity check: adopting on the
         * strength of a matching pid NUMBER would hand this task another
         * process's entry, and its privileges with it.
         */
        proc = vms_proc_find_or_err();
        if (!proc)
            return -ESRCH;
    }

    args.vms_pid = proc->vms_pid;
    args.status = 0x00000001;  /* SS$_NORMAL */
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;

    return 0;
}

static long vms_dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct vms_proc *proc;

    /* REGISTER doesn't require an existing proc. REGISTER_CONTINUE is the
     * image-activation variant (vms-4d7): it continues the caller's already-
     * registered VMS parent (identity AND shared PID). REGISTER_SUBPROCESS
     * (vms-19e9) is the SPAWN/$CREPRC variant: it inherits the parent's
     * identity but mints a FRESH VMS PID -- a genuinely new VMS process. */
    if (cmd == VMS_IOCTL_REGISTER)
        return vms_ioctl_register(arg, false, false);
    if (cmd == VMS_IOCTL_REGISTER_CONTINUE)
        return vms_ioctl_register(arg, true, true);
    if (cmd == VMS_IOCTL_REGISTER_SUBPROCESS)
        return vms_ioctl_register(arg, true, false);

    /* All other ioctls require a registered process */
    proc = vms_proc_find_or_err();
    if (!proc)
        return -ESRCH;

    switch (cmd) {
    /* Access mode (3a) */
    case VMS_IOCTL_SETMODE:
        return vms_ioctl_setmode(proc, arg);
    case VMS_IOCTL_GETMODE:
        return vms_ioctl_getmode(proc, arg);
    case VMS_IOCTL_ENTER_IMAGE:
        return vms_ioctl_enter_image(proc, arg);
    case VMS_IOCTL_IMAGE_RUNDOWN:
        return vms_ioctl_image_rundown(proc, arg);
    case VMS_IOCTL_SETPRV:
        return vms_ioctl_setprv(proc, arg);
    case VMS_IOCTL_CHKPRIV:
        return vms_ioctl_chkpriv(proc, arg);

    /* AST delivery (3b) */
    case VMS_IOCTL_DCLAST:
        return vms_ioctl_dclast(proc, arg);
    case VMS_IOCTL_SETAST:
        return vms_ioctl_setast(proc, arg);
    case VMS_IOCTL_DELIVERAST:
        return vms_ioctl_deliverast(proc, arg);

    /* Event flags (3c) */
    case VMS_IOCTL_SETEF:
        return vms_ioctl_setef(proc, arg);
    case VMS_IOCTL_CLREF:
        return vms_ioctl_clref(proc, arg);
    case VMS_IOCTL_WAITFR:
        return vms_ioctl_waitfr(proc, arg);
    case VMS_IOCTL_WFLOR:
        return vms_ioctl_wflor(proc, arg);
    case VMS_IOCTL_WFLAND:
        return vms_ioctl_wfland(proc, arg);
    case VMS_IOCTL_READEF:
        return vms_ioctl_readef(proc, arg);
    case VMS_IOCTL_ASCEFC:
        return vms_ioctl_ascefc(proc, arg);
    case VMS_IOCTL_DACEFC:
        return vms_ioctl_dacefc(proc, arg);
    case VMS_IOCTL_DLCEFC:
        return vms_ioctl_dlcefc(proc, arg);

    /* Lock manager (3d) */
    case VMS_IOCTL_ENQ:
        return vms_ioctl_enq(proc, arg);
    case VMS_IOCTL_DEQ:
        return vms_ioctl_deq(proc, arg);
    case VMS_IOCTL_CONVERT:
        return vms_ioctl_convert(proc, arg);
    case VMS_IOCTL_GETLKI:
        return vms_ioctl_getlki(proc, arg);
    /* DLM resource-directory + mastering readback (vms-ci.5 DB) */
    case VMS_IOCTL_GET_RESMASTER:
        return vms_ioctl_get_resmaster(proc, arg);
    /* DLM graceful member departure -> shrink live membership + re-resolve the
     * directory over the survivors (vms-2bf, DLM rung H10a). */
    case VMS_IOCTL_DLM_MEMBER_DEPART:
        return vms_ioctl_dlm_member_depart(proc, arg);
    /* DLM granted-lock readback -> value-verify a rebuilt cross-node lock
     * (vms-dca9, DLM rung H10b). */
    case VMS_IOCTL_DLM_GET_GRANTED:
        return vms_ioctl_dlm_get_granted(proc, arg);
    /* DLM pending-wait enumeration -> HOME authority for distributed deadlock
     * search (rd vms-ec75, DLM rung H11). */
    case VMS_IOCTL_DLM_ENUM_WAITS:
        return vms_ioctl_dlm_enum_waits(proc, arg);
    case VMS_IOCTL_DLM_ENUM_STANDING:
        return vms_ioctl_dlm_enum_standing(proc, arg);
    /* SHOW CLUSTER's read (vms-551), projecting the connection manager's own
     * CLUB/CSB table since FC-P3.9. There is no SET/CLEAR: the userspace
     * daemon that populated a mirror through them is retired, and so are
     * they. */
    case VMS_IOCTL_CLUSTER_MEMBER_GET:
        return vms_ioctl_cluster_member_get(proc, arg);
    /* The port's SDA SHOW PORT-equivalent diagnostics read (FC-P0.9,
     * vms_pe.c), against the one per-node vms_cluster_node(). */
    case VMS_IOCTL_CLUSTER_DIAG_PORT:
        return vms_ioctl_cluster_diag_port(proc, arg);
    /* SCS's SDA SHOW CONNECTIONS-equivalent diagnostics read (FC-P2.4,
     * vms_scs.c): the SCS-wide view, and one CDT row per call. */
    case VMS_IOCTL_CLUSTER_DIAG_CONN:
        return vms_ioctl_cluster_diag_conn(proc, arg);
    /* The connection manager's own SDA-equivalent CLUB/CSB read (FC-P3.8,
     * vms_cnxman.c), and $SETCLUEVT's executive-side registration. */
    case VMS_IOCTL_CLUSTER_DIAG_CSB:
        return vms_ioctl_cluster_diag_csb(proc, arg);
    case VMS_IOCTL_CLUSTER_SETCLUEVT:
        return vms_ioctl_cluster_setcluevt(proc, arg);
    /* $GETSYI's cluster item codes, from the CLUB (FC-P3.9). */
    case VMS_IOCTL_CLUSTER_GETSYI:
        return vms_ioctl_cluster_getsyi(proc, arg);
    /* STARTUP.EXE's own case of SYSBOOT (FC-P0.10): load the cluster SYSGEN
     * parameters + CLUSTER_AUTHORIZE into vms_cluster_node()->params, once,
     * before VMS_IOCTL_CLUSTER_START (FC-P0.11). */
    case VMS_IOCTL_SYSGEN_LOAD:
        return vms_ioctl_sysgen_load(proc, arg);
    /* STARTUP.EXE's boot path (FC-P0.11; join semantics FC-P3.9): the fork
     * thread, then vms_pe_start(), vms_scs_start() and vms_cnxman_start() --
     * against the same vms_cluster_node(), gated on VAXCLUSTER at both the
     * ovmx_init.c caller and here (each layer's own check). */
    case VMS_IOCTL_CLUSTER_START:
        return vms_ioctl_cluster_start(proc, arg);
    /* DLM cross-node lock-request dispatch (vms-94c, DLM epic vms-7fa rung 1):
     * a decoded remote DLM message reaches the cross-node handler, which
     * returns SS$_UNSUPPORTED (rung 1 transport; no fake grant). */
    case VMS_IOCTL_DLM_XNODE:
        return vms_ioctl_dlm_xnode(proc, arg);

    /* Device table (executive-resident I/O database) */
    case VMS_IOCTL_ASSIGN:
        return vms_ioctl_assign(proc, arg);
    case VMS_IOCTL_DASSGN:
        return vms_ioctl_dassgn(proc, arg);
    case VMS_IOCTL_GETDVI:
        return vms_ioctl_getdvi(proc, arg);
    case VMS_IOCTL_DEVSCAN:
        return vms_ioctl_devscan(proc, arg);
    case VMS_IOCTL_TTSETMODE:
        return vms_ioctl_ttsetmode(proc, arg);
    case VMS_IOCTL_ALLOC:
        return vms_ioctl_alloc(proc, arg);
    case VMS_IOCTL_DALLOC:
        return vms_ioctl_dalloc(proc, arg);
    case VMS_IOCTL_GETVOL:
        return vms_ioctl_acp_getvol(proc, arg);
    case VMS_IOCTL_DISK_RESOLVE:
        return vms_ioctl_disk_resolve(proc, arg);
    case VMS_IOCTL_SETTERM:
        return vms_ioctl_setterm(proc, arg);

    /* Process table (executive-resident PCB directory) */
    case VMS_IOCTL_SETPRN:
        return vms_ioctl_setprn(proc, arg);
    case VMS_IOCTL_GETJPI:
        return vms_ioctl_getjpi(proc, arg);
    case VMS_IOCTL_PROCSCAN:
        return vms_ioctl_procscan(proc, arg);
    case VMS_IOCTL_SETIDENT:
        return vms_ioctl_setident(proc, arg);
    case VMS_IOCTL_ESTABLISH_SYSTEM:
        return vms_ioctl_establish_system(proc, arg);

    /* Hibernate / wake, executive-resident + AST-interruptible (vms-feb) */
    case VMS_IOCTL_HIBER:
        return vms_ioctl_hiber(proc, arg);
    case VMS_IOCTL_WAKE:
        return vms_ioctl_wake(proc, arg);

    /* $EXIT/$STATUS + CLI invocation context (vms-f60d) -- the executive
     * half of IMGACT's VMS-standard image return path (ovmx_activation.h) */
    case VMS_IOCTL_SETEXIT:
        return vms_ioctl_setexit(proc, arg);
    case VMS_IOCTL_GETEXIT:
        return vms_ioctl_getexit(proc, arg);
    case VMS_IOCTL_SETCLI:
        return vms_ioctl_setcli(proc, arg);
    case VMS_IOCTL_GETCLI:
        return vms_ioctl_getcli(proc, arg);

    /* /NOWAIT subprocess-exit completion arm (vms-e9a B1, LIB$SPAWN efn/astadr) */
    case VMS_IOCTL_SPAWN_NOTIFY:
        return vms_ioctl_spawn_notify(proc, arg);

    /* Logical name tables (executive-resident LNM$SYSTEM, vms-d37) */
    case VMS_IOCTL_LNM_DEFINE:
        return vms_ioctl_lnm_define(proc, arg);
    case VMS_IOCTL_LNM_DELETE:
        return vms_ioctl_lnm_delete(proc, arg);
    case VMS_IOCTL_LNM_GETSCOPE:
        return vms_ioctl_lnm_getscope(proc, arg);

    /* Mailboxes (executive-resident MBAn:, vms-d44) */
    case VMS_IOCTL_MBX_CREATE:
        return vms_ioctl_mbx_create(proc, arg);
    case VMS_IOCTL_MBX_ASSIGN:
        return vms_ioctl_mbx_assign(proc, arg);
    case VMS_IOCTL_MBX_WRITE:
        return vms_ioctl_mbx_write(proc, arg);
    case VMS_IOCTL_MBX_READ:
        return vms_ioctl_mbx_read(proc, arg);
    case VMS_IOCTL_MBX_DELMBX:
        return vms_ioctl_mbx_delmbx(proc, arg);

    /* Files-11 (ODS-2) ACP: mount table + file-class channel (vms-149,
     * epic vms-208). The ACP-QIO file operations get the remaining band
     * slots in a later rung; this rung is mount/dismount + $ASSIGN. */
    case VMS_IOCTL_ACP_MOUNT:
        return vms_ioctl_acp_mount(proc, arg);
    case VMS_IOCTL_ACP_DMOUNT:
        return vms_ioctl_acp_dmount(proc, arg);
    case VMS_IOCTL_ACP_ASSIGN:
        return vms_ioctl_acp_assign(proc, arg);
    case VMS_IOCTL_ACP_ACCESS:
        return vms_ioctl_acp_access(proc, arg);
    case VMS_IOCTL_ACP_DEACCESS:
        return vms_ioctl_acp_deaccess(proc, arg);
    case VMS_IOCTL_ACP_READVBLK:
        return vms_ioctl_acp_readvb(proc, arg);
    case VMS_IOCTL_ACP_WRITEVBLK:
        return vms_ioctl_acp_writevb(proc, arg);
    case VMS_IOCTL_ACP_ACPCONTROL:
        return vms_ioctl_acp_acpcontrol(proc, arg);
    case VMS_IOCTL_ACP_FILEOP:
        return vms_ioctl_acp_fileop(proc, arg);
    case VMS_IOCTL_MBX_SET_WRTATTN:
        return vms_ioctl_mbx_set_wrtattn(proc, arg);

    /* INET pseudo-device (executive-resident BGn:, vms-527). BGn: is a
     * kernel-mode driver over the host in-kernel socket API (src/kernel/
     * vms_bg.c); $ASSIGN creates a unit+channel, IO$_SETMODE the socket, and
     * IO$_ACCESS/WRITEVBLK/READVBLK/DEACCESS map to connect/send/recv/shutdown. */
    case VMS_IOCTL_BG_CREATE:
        return vms_ioctl_bg_create(proc, arg);
    case VMS_IOCTL_BG_SETMODE:
        return vms_ioctl_bg_setmode(proc, arg);
    case VMS_IOCTL_BG_SETMODE_ICMP:
        return vms_ioctl_bg_setmode_icmp(proc, arg);
    case VMS_IOCTL_BG_CONNECT:
        return vms_ioctl_bg_connect(proc, arg);
    case VMS_IOCTL_BG_SEND:
        return vms_ioctl_bg_send(proc, arg);
    case VMS_IOCTL_BG_RECV:
        return vms_ioctl_bg_recv(proc, arg);
    case VMS_IOCTL_BG_DEACCESS:
        return vms_ioctl_bg_deaccess(proc, arg);
    case VMS_IOCTL_BG_DASSGN:
        return vms_ioctl_bg_dassgn(proc, arg);
    case VMS_IOCTL_BG_POLLFD:
        return vms_ioctl_bg_pollfd(proc, arg);
    case VMS_IOCTL_BG_GETNAME:
        return vms_ioctl_bg_getname(proc, arg);
    case VMS_IOCTL_BG_SOCKOPT:
        return vms_ioctl_bg_sockopt(proc, arg);
    case VMS_IOCTL_BG_BIND:
        return vms_ioctl_bg_bind(proc, arg);
    case VMS_IOCTL_BG_LISTEN:
        return vms_ioctl_bg_listen(proc, arg);
    case VMS_IOCTL_BG_ACCEPT:
        return vms_ioctl_bg_accept(proc, arg);
    case VMS_IOCTL_BG_MATERIALIZE_FD:
        return vms_ioctl_bg_materialize_fd(proc, arg);

    /* L2 (raw datalink) socket surface (vms-7eb, auth slice of vms-1e4): a
     * kernel-owned AF_PACKET socket for the SCS cluster wire, gated on the
     * VMS PHY_IO privilege at OPEN (src/kernel-core/vms_l2.c). */
    case VMS_IOCTL_L2_OPEN:
        return vms_ioctl_l2_open(proc, arg);
    case VMS_IOCTL_L2_SEND:
        return vms_ioctl_l2_send(proc, arg);
    case VMS_IOCTL_L2_RECV:
        return vms_ioctl_l2_recv(proc, arg);
    case VMS_IOCTL_L2_CLOSE:
        return vms_ioctl_l2_close(proc, arg);

    /* P0 program region (vms-68f.i, in-process image activation foundation) */
    case VMS_IOCTL_P0_MAP:
        return vms_ioctl_p0_map(proc, arg);
    case VMS_IOCTL_P0_UNMAP:
        return vms_ioctl_p0_unmap(proc, arg);

    /* P1 control region (vms-68f.ii, same design, increment (ii)) */
    case VMS_IOCTL_P1_MAP:
        return vms_ioctl_p1_map(proc, arg);

    default:
        return -ENOTTY;
    }
}

static int vms_dev_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static int vms_dev_release(struct inode *inode, struct file *filp)
{
    struct vms_proc *proc;

    /*
     * The executive PCB belongs to the PROCESS, not to the channel
     * (vms-8019). Closing /dev/vms -- including the implicit close of
     * an inherited descriptor at execve() time -- must not delete the
     * process from the executive's process table, or the process name
     * would not survive image activation and would once again be
     * something only the current image can see.
     *
     * So the entry is destroyed here only when the task that owns it is
     * actually going away. Entries whose task exits without ever
     * reaching this path (a forked child sharing the parent's struct
     * file, for instance) are reclaimed by vms_proc_reap_dead().
     *
     * AND ONLY WHEN THE WHOLE THREAD GROUP IS GOING AWAY (vms-2b8). The
     * entry is keyed by tgid and shared by every thread, so an exiting
     * worker thread that happens to hold a channel must not delete the
     * PCB out from under the threads still running: on VMS a thread
     * terminating does not delete the process. thread_group_empty() is
     * true only for the last thread standing, which is the point at
     * which the VMS process really is ending.
     */
    if (!(current->flags & PF_EXITING) || !thread_group_empty(current))
        return 0;

    proc = vms_proc_find_or_err();
    if (proc)
        vms_proc_free(proc);

    return 0;
}

/*
 * vms_lnm_mmap - hand userspace a READ-ONLY view of the executive-resident
 * logical-name arena (vms-d37; the CHAR-DEVICE half of the arena seam, vms-d61).
 *
 * The arena itself is owned, allocated and written by the substrate-agnostic
 * facility (src/kernel-core/vms_lnm.c) through the exec_arena seam
 * (exec_kbackend.h §10). The MMAP-TIME MAPPING here -- Linux mm machinery:
 * remap_vmalloc_range and clearing VM_MAYWRITE -- is host char-device glue and
 * stays in the Linux module rind, NOT in the portable facility (design record
 * docs/design-netbsd-executive-core.md §2, the host-mm coupling stays in the
 * rind exactly like registration does). This handler asks the facility for the
 * arena base+size (vms_lnm_arena_base/_size) and does the Linux mapping itself;
 * the facility never names struct vm_area_struct, remap_vmalloc_range, VM_* or
 * PAGE_*.
 *
 * The mapping is read-only and VM_MAYWRITE is cleared so mprotect() cannot
 * turn it writable afterwards -- this is the direct analogue of VMS
 * protecting system space by processor access mode (design §2.4). The MMU,
 * not a convention, is what stops a process corrupting the system logical
 * name table.
 */
static int vms_lnm_mmap(struct file *filp, struct vm_area_struct *vma)
{
    void *base = vms_lnm_arena_base();
    unsigned long size = vma->vm_end - vma->vm_start;
    int ret;

    (void)filp;

    if (!base)
        return -ENODEV;

    /* Only the arena, only from its start. */
    if (vma->vm_pgoff != (VMS_LNM_MMAP_OFFSET >> PAGE_SHIFT))
        return -EINVAL;
    if (size > PAGE_ALIGN(vms_lnm_arena_size()))
        return -EINVAL;

    /* Reject any write intent, now and forever. */
    if (vma->vm_flags & VM_WRITE)
        return -EACCES;
    vm_flags_clear(vma, VM_MAYWRITE);

    ret = remap_vmalloc_range(vma, base, 0);
    if (ret < 0)
        return ret;

    return 0;
}

static const struct file_operations vms_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = vms_dev_ioctl,
    .mmap           = vms_lnm_mmap,      /* read-only logical-name arena (vms-d37) */
    .open           = vms_dev_open,
    .release        = vms_dev_release,
};

/*
 * THE EXECUTIVE ENTRY POINT IS NOT PRIVILEGE-GATED (vms-2b8).
 *
 * miscdevice with no .mode creates the node 0600 root:root, which meant NO
 * UNPRIVILEGED PROCESS COULD REACH THE EXECUTIVE AT ALL. That is not a
 * conservative default here, it is a different system: with a 0600 door,
 * every reachable caller is root, multi-user OVMX cannot exist, and the
 * per-service privilege checks this module enforces are unreachable in the
 * product even though they are demonstrable in a test.
 *
 * VMS SEMANTICS THIS MATCHES (CLAUDE.md Rule 10, answer 1). On OpenVMS the
 * system-service entry sequence -- the change-mode-to-kernel/exec instruction
 * that the $-service jackets execute -- is an UNPRIVILEGED instruction
 * available to every process at every access mode. There is no permission on
 * "may I call the executive". Access control lives INSIDE each service, which
 * validates the caller's privilege mask and returns SS$_NOPRIV. That is
 * exactly the shape this module already has (vms_ioctl_setident,
 * vms_ioctl_setprv, vms_access_check), so the door must be open for the
 * checks behind it to be the thing that decides.
 *
 * THE ANSWER TO THE QUESTION THIS COMMENT WAS ASKED (round 3): YES,
 * unprivileged processes SHOULD be able to open /dev/vms, and therefore the
 * per-service checks are load-bearing security, not defence in depth. That
 * has a cost paid in this same change: when the door was opened in round 2,
 * vms_ioctl_getjpi() and vms_ioctl_procscan() had NO caller check, so an
 * unprivileged process could read the user name, UIC and privilege mask of
 * every process on the system -- the premise that "access control lives
 * inside each service" was false of the two services the open door newly
 * exposed. vms_proc_may_read() is that missing check, and its rule is
 * measured on the oracle rather than assumed (see vms_ioctl.h).
 *
 * OVMX DESIGN CHOICE, labelled as such (CLAUDE.md Rule 8): /dev/vms is an
 * OVMX construct with no VMS counterpart, so no public OpenVMS document
 * publishes a mode for it. 0666 is chosen as the closest Linux expression of
 * "every process may enter the executive"; it is not presented as
 * VMS-authentic.
 *
 * KNOWN CONSEQUENCE, deliberately not handled here and reported to the
 * security review (vms-cb5): an unprivileged process may now consume
 * executive memory -- process entries, locks, event flags, queued ASTs --
 * with no bound. VMS bounds exactly this with per-process quotas (BYTLM,
 * ENQLM, ASTLM) charged from SYSUAF. OVMX has no quota system yet, so this
 * enlarges a local denial-of-service surface. Adding an arbitrary in-module
 * cap would be the illegal third answer (Rule 10): VMS's answer is quotas,
 * and quotas are the item to write, not a limit invented here.
 */
static struct miscdevice vms_misc = {
    .minor  = MISC_DYNAMIC_MINOR,
    .name   = "vms",
    .fops   = &vms_fops,
    .mode   = 0666,
};

/* ================================================================
 * Module init / exit
 * ================================================================ */

static int __init vms_init(void)
{
    int ret;

    pr_info("vms: initializing VMS kernel module\n");

    /* Create slab cache for process structs */
    vms_proc_cache = kmem_cache_create("vms_proc",
                                        sizeof(struct vms_proc),
                                        0, SLAB_HWCACHE_ALIGN, NULL);
    if (!vms_proc_cache) {
        pr_err("vms: failed to create process slab cache\n");
        return -ENOMEM;
    }

    /* Initialize subsystems */
    hash_init(vms_proc_hash);
    ret = vms_lock_init();
    if (ret) {
        pr_err("vms: failed to initialize lock manager: %d\n", ret);
        kmem_cache_destroy(vms_proc_cache);
        return ret;
    }
    vms_eflag_init();

    /*
     * Bring up the device table before /dev/vms exists, so that the
     * console terminal is in the executive's I/O database before any
     * process can possibly ask about it -- a device is never something
     * a process introduces.
     */
    ret = vms_devtab_init();
    if (ret) {
        pr_err("vms: failed to initialize device table: %d\n", ret);
        vms_lock_cleanup();
        vms_eflag_cleanup();
        kmem_cache_destroy(vms_proc_cache);
        return ret;
    }

    /*
     * Announce the SYSTEM identity constant before /dev/vms exists, for the
     * same reason the device table just did (vms-a17e): VMS_SYSTEM_UIC and
     * VMS_PRV_M_SYSTEM_ALL (vms_internal.h) are facts the executive owns
     * from module load, not values any process registers or supplies.
     * vms_ioctl_establish_system() stamps them onto a process later, on
     * request (PROVISION.EXE, at boot) -- this line is the proof that what
     * gets stamped was already true before that process, or SYSUAF, existed
     * to the executive at all.
     */
    pr_info("vms: system identity constant SYSTEM [%o,%o] privileges=ALL established by the executive\n",
            VMS_SYSTEM_UIC >> 16, VMS_SYSTEM_UIC & 0xFFFFu);

    /*
     * Bring up the logical-name arena before /dev/vms exists, for the same
     * reason as the device table: the executive-resident tables are a
     * property of the node, present before any process can ask (vms-d37).
     */
    ret = vms_lnm_init();
    if (ret) {
        pr_err("vms: failed to initialize logical-name arena: %d\n", ret);
        vms_devtab_cleanup();
        vms_lock_cleanup();
        vms_eflag_cleanup();
        kmem_cache_destroy(vms_proc_cache);
        return ret;
    }

    /* Mailbox table (vms-d44) -- starts empty, nothing that can fail here
     * (see vms_mbx.c's vms_mbx_init()). */
    vms_mbx_init();

    /* Files-11 (ODS-2) ACP mounted-volume table (vms-149) -- starts empty,
     * nothing that can fail here (see vmsfs_acp.c's vms_acp_init()). */
    vms_acp_init();

    /* Register /dev/vms */
    ret = misc_register(&vms_misc);
    if (ret) {
        pr_err("vms: failed to register /dev/vms: %d\n", ret);
        vms_acp_cleanup();
        vms_mbx_cleanup();
        vms_lnm_cleanup();
        vms_devtab_cleanup();
        vms_lock_cleanup();
        vms_eflag_cleanup();
        kmem_cache_destroy(vms_proc_cache);
        return ret;
    }

    pr_info("vms: /dev/vms registered successfully\n");

    /* vms-0cd: eager fork-time BG channel inheritance (sched_process_fork hook), so
     * an accept->fork->close forking server (sshd, inetd) hands its accepted
     * connection to the forked child even though it closes its own copy right after
     * the fork. Non-fatal on failure: the #815 real_parent-at-registration path
     * still handles the parent-stays-open case; only the close-early race stays
     * unfixed, and it fails HONEST (SS$_IVCHAN), never fabricated. */
    if (vms_bg_forkinherit_init())
        pr_warn("vms: fork-time BG channel inheritance unavailable; accept->fork->close daemons fall back to #815 (honest)\n");

    return 0;
}

/* ---- ODS-2 ACP backing-device handle cache (rd vms-648) ------------------
 * See exec_kbackend_linux.h for the full rationale. The Files-11 ODS-2 ACP
 * reaches the raw disk one 512-byte LBN per call; opening+closing the block
 * device on EVERY block (bdev_file_open_by_dev + fput) is pathologically slow
 * and pushed the R1 release-install e2e's install step past its timeout on a
 * slow single-CPU TCG runner (the install "hung" at PCSI Configuring). Cache
 * each backing device's open handle (READ|WRITE, non-exclusive), keyed by
 * dev_t, and reuse it -- the Linux analogue of the NetBSD twin's cached backing
 * vnode (vms_blockdev_netbsd.c). Slots are only ADDED during the OS's life and
 * released together at module exit (no runtime eviction), so a struct
 * block_device* handed back stays valid for the module's life -- no
 * use-after-free even though the bio runs outside this lock. A full table or a
 * failed open returns NULL and the caller falls back to open-per-call, so
 * correctness never depends on the cache. Only the OPEN is amortized: every
 * block is still a real synchronous bio to a real device (INV-6 / Rule 8).
 */
#define OVMX_BDEV_CACHE_SLOTS 8
struct ovmx_bdev_cslot {
    dev_t devt;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
    struct file *bf;
#else
    struct bdev_handle *bh;
#endif
    struct block_device *bdev;
    bool valid;
};
static struct ovmx_bdev_cslot ovmx_bdev_cache[OVMX_BDEV_CACHE_SLOTS];
static DEFINE_MUTEX(ovmx_bdev_cache_lock);

struct block_device *exec_bdev_get_cached(dev_t devt)
{
    struct block_device *bdev = NULL;
    int i, freeslot = -1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
    struct file *bf;
#else
    struct bdev_handle *bh;
#endif

    mutex_lock(&ovmx_bdev_cache_lock);
    for (i = 0; i < OVMX_BDEV_CACHE_SLOTS; i++) {
        if (ovmx_bdev_cache[i].valid) {
            if (ovmx_bdev_cache[i].devt == devt) {
                bdev = ovmx_bdev_cache[i].bdev;
                mutex_unlock(&ovmx_bdev_cache_lock);
                return bdev;
            }
        } else if (freeslot < 0) {
            freeslot = i;
        }
    }
    if (freeslot < 0) {
        /* Full -- caller falls back to open-per-call (rare: the ACP touches at
         * most SYS$DISK + the install target). */
        mutex_unlock(&ovmx_bdev_cache_lock);
        return NULL;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
    bf = bdev_file_open_by_dev(devt, BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
    if (IS_ERR(bf)) {
        mutex_unlock(&ovmx_bdev_cache_lock);
        return NULL;
    }
    bdev = file_bdev(bf);
    ovmx_bdev_cache[freeslot].bf = bf;
#else
    bh = bdev_open_by_dev(devt, BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
    if (IS_ERR(bh)) {
        mutex_unlock(&ovmx_bdev_cache_lock);
        return NULL;
    }
    bdev = bh->bdev;
    ovmx_bdev_cache[freeslot].bh = bh;
#endif
    ovmx_bdev_cache[freeslot].devt = devt;
    ovmx_bdev_cache[freeslot].bdev = bdev;
    ovmx_bdev_cache[freeslot].valid = true;
    mutex_unlock(&ovmx_bdev_cache_lock);
    return bdev;
}

void exec_bdev_cache_release_all(void)
{
    int i;

    mutex_lock(&ovmx_bdev_cache_lock);
    for (i = 0; i < OVMX_BDEV_CACHE_SLOTS; i++) {
        if (!ovmx_bdev_cache[i].valid)
            continue;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
        fput(ovmx_bdev_cache[i].bf);
        ovmx_bdev_cache[i].bf = NULL;
#else
        bdev_release(ovmx_bdev_cache[i].bh);
        ovmx_bdev_cache[i].bh = NULL;
#endif
        ovmx_bdev_cache[i].bdev = NULL;
        ovmx_bdev_cache[i].devt = 0;
        ovmx_bdev_cache[i].valid = false;
    }
    mutex_unlock(&ovmx_bdev_cache_lock);
}

static void __exit vms_exit(void)
{
    struct vms_proc *proc;
    struct hlist_node *tmp;
    int bkt;

    pr_info("vms: unloading VMS kernel module\n");

    /* Unregister device */
    misc_deregister(&vms_misc);

    /* vms-0cd: stop capturing forks and drain any un-consumed fork-inherit records
     * (dropping the socket refs they hold) BEFORE the PCBs they snapshot are freed. */
    vms_bg_forkinherit_exit();

    /* Free all process state (vms_proc_free handles sub-objects) */
    hash_for_each_safe(vms_proc_hash, bkt, tmp, proc, hash_node) {
        vms_proc_free(proc);
    }
    /* Wait for RCU callbacks to complete before destroying the cache */
    rcu_barrier();

    /* Cleanup subsystems */
    vms_lock_cleanup();
    vms_eflag_cleanup();
    vms_devtab_cleanup();
    vms_lnm_cleanup();
    vms_mbx_cleanup();
    vms_acp_cleanup();
    exec_bdev_cache_release_all();   /* rd vms-648: close cached ACP backing handles */

    kmem_cache_destroy(vms_proc_cache);

    pr_info("vms: module unloaded\n");
}

module_init(vms_init);
module_exit(vms_exit);
