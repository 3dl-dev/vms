/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_kbackend_linux.h - the LINUX realization of the OVMX executive
 * kernel-backend shim (rd vms-adb, epic vms-8e8).
 *
 * DO NOT include this directly -- include "exec_kbackend.h", which selects
 * this file on a Linux kernel build. See that header for the op contract,
 * especially the wait/wake (cv) contract.
 *
 * Every op here is a TRIVIAL FORWARDER to the exact Linux primitive the
 * executive already uses, so a facility converted onto the shim compiles to
 * byte-identical behaviour:
 *
 *   exec_lock_t          == spinlock_t
 *   exec_cv_t            == wait_queue_head_t
 *   exec_lock/unlock     -> spin_lock / spin_unlock
 *   exec_cv_wait         -> a faithful open-coding of wait_event_interruptible's
 *                           per-iteration body (prepare_to_wait / unlock /
 *                           schedule / lock / finish_wait)
 *   exec_cv_broadcast    -> wake_up_interruptible  (wake ALL, as the executive
 *                           does today via wake_up_interruptible)
 *   exec_copyin/out      -> copy_from_user / copy_to_user, normalized to
 *                           0 / EXEC_EFAULT
 *   exec_zalloc_atomic   -> kzalloc(n, GFP_ATOMIC)
 *   exec_zalloc          -> kzalloc(n, GFP_KERNEL)
 *   exec_alloc / exec_free -> kmalloc(n, GFP_KERNEL) / kfree
 *
 * Clean-room (CLAUDE.md Rule 8): these forwarders call only public, documented
 * Linux kernel APIs. No code is copied from the Linux source.
 */

#ifndef OVMX_EXEC_KBACKEND_LINUX_H
#define OVMX_EXEC_KBACKEND_LINUX_H

#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/compiler.h>   /* __user */
#include <linux/gfp.h>
#include <linux/jiffies.h>    /* msecs_to_jiffies (exec_cv_wait_timeout) */
/* Phase F (host-task / RCU-lite / sleepable mutex) backing headers. */
#include <linux/pid.h>            /* struct pid, pid_task, PIDTYPE_PID */
#include <linux/capability.h>     /* capable, CAP_SYS_ADMIN */
#include <linux/mutex.h>          /* struct mutex, DEFINE_MUTEX, mutex_* */
#include <linux/rcupdate.h>       /* rcu_read_lock/unlock, call_rcu, rcu_head */
#include <linux/sched/cputime.h>  /* task cputime fields */
#include <linux/sched/mm.h>       /* get_task_mm, mmput */
#include <linux/mm.h>             /* get_mm_rss */
#include <linux/timekeeping.h>    /* ktime_get_boottime_ns, ktime_get_real_ts64 */
/* vms-31b (exec_current_uid/gid + block-device resolution) backing headers. */
#include <linux/cred.h>           /* current_uid, current_gid */
#include <linux/uidgid.h>         /* from_kuid, from_kgid, init_user_ns */
#include <linux/blkdev.h>         /* lookup_bdev, bdev_open_by_dev/bdev_file_open_by_dev */
#include <linux/kdev_t.h>         /* MAJOR / MINOR / MKDEV */
#include <linux/bio.h>            /* bio_init / __bio_add_page / submit_bio_wait (vms-127) */
#include <linux/version.h>        /* LINUX_VERSION_CODE / KERNEL_VERSION (bdev-open guard) */
/* FC-P0.1 (the cluster seam, SS15/SS16/SS18) backing headers: the TYPES the core
 * embeds by value plus the console primitive. The rx/xmit headers
 * (<linux/netdevice.h>, <linux/if_ether.h>) arrive with the real binding in
 * FC-P0.2 -- they are not needed to fix a size. */
#include <linux/kthread.h>        /* struct task_struct (exec_kthread_t) */
#include <linux/timer.h>          /* struct timer_list (exec_timer_t) */
#include <linux/printk.h>         /* printk / KERN_ERR (exec_console_printf) */
/* vms-9d2 (primary Ethernet net device -> ETH0:) backing headers. */
#include <linux/netdevice.h>      /* struct net_device, for_each_netdev, netif_carrier_ok */
#include <linux/rtnetlink.h>      /* rtnl_lock / rtnl_unlock */
#include <net/net_namespace.h>    /* init_net */
#include <linux/if_arp.h>         /* ARPHRD_ETHER */
/* vms-d61 (seqlock barriers + userspace-publishable arena) backing headers. */
#include <linux/vmalloc.h>        /* vmalloc_user / vfree (exec_arena_*) */
#include <asm/barrier.h>          /* smp_wmb / smp_rmb (exec_membar_*) */
/* vms-9951 (host TCP client socket -> BGn:) backing headers. */
#include <linux/net.h>            /* struct socket, sock_create_kern, kernel_* */
#include <linux/in.h>             /* struct sockaddr_in, IPPROTO_TCP/IP */
/* vms-7eb (host AF_PACKET raw datalink socket -> L2) backing headers. */
#include <linux/if_packet.h>      /* struct sockaddr_ll, AF_PACKET/SOCK_RAW */
#include <linux/if_ether.h>       /* ETH_ALEN */
#include <linux/socket.h>         /* AF_INET, SOL_SOCKET, SHUT_RDWR */
#include <linux/kref.h>           /* kref (the exec_socket_t refcount) */
#include <net/sock.h>             /* sock_release, sock_setsockopt, sock_flag, sock_error, KERNEL_SOCKPTR */
#include <linux/tcp.h>            /* tcp_sk() */
#include <net/tcp.h>              /* TCP_NAGLE_OFF */
#include <net/inet_sock.h>        /* inet_sk() */

/* ---- primitive types ---- */
typedef spinlock_t        exec_lock_t;
typedef wait_queue_head_t exec_cv_t;

/* ---- 1. locking ---- */
static inline void exec_lock_init(exec_lock_t *l)    { spin_lock_init(l); }
static inline void exec_lock(exec_lock_t *l)         { spin_lock(l); }
static inline void exec_unlock(exec_lock_t *l)       { spin_unlock(l); }
static inline void exec_lock_destroy(exec_lock_t *l) { (void)l; /* no-op on Linux */ }
/* exec_trylock: acquire without blocking. Nonzero iff acquired (Phase G; the
 * lock manager's deadlock walker takes other processes' lock-list locks with a
 * trylock to avoid an ABBA inversion). Linux: spin_trylock. */
static inline int exec_trylock(exec_lock_t *l)       { return spin_trylock(l); }

/* ---- 1b. receive-level lock (FC-P0.16, exec_kbackend.h SS1b / design SS3.2.3
 * RULING) ----
 * On Linux, exec_rxlock_t is the SAME spinlock_t as exec_lock_t, but acquired
 * with _irqsave/_irqrestore rather than the plain form: disabling the local
 * CPU's interrupts for the critical section is what makes it safe against a
 * same-CPU softirq (the cluster rx path) taking the identical lock -- the
 * plain exec_lock_t hazard SS3.2.3 records. EXEC_LAN_RX_IPL is a no-op on
 * Linux (irqsave already covers every level a softirq can run at); it exists
 * so the ONE cf_bind.c call site is substrate-symmetric.
 */
typedef spinlock_t    exec_rxlock_t;
typedef unsigned long exec_rxflags_t;
#define EXEC_LAN_RX_IPL 0   /* no-op: irqsave covers all Linux receive levels */

static inline void exec_rxlock_init(exec_rxlock_t *l)    { spin_lock_init(l); }
static inline void exec_rxlock_destroy(exec_rxlock_t *l) { (void)l; /* no-op on Linux */ }

static inline void exec_rxlock_acquire(exec_rxlock_t *l, exec_rxflags_t *flags)
{
	spin_lock_irqsave(l, *flags);
}

static inline void exec_rxlock_release(exec_rxlock_t *l, exec_rxflags_t *flags)
{
	spin_unlock_irqrestore(l, *flags);
}

/* ---- 2. wait / wake (cv-shaped; see exec_kbackend.h) ----
 *
 * exec_cv_wait is a faithful open-coding of what wait_event_interruptible
 * does on each loop iteration, so that the caller's
 *
 *     for (;;) { if (PRED) break; if (exec_cv_wait(cv,lk)) {...} }
 *
 * is behaviour-identical to wait_event_interruptible(*cv, PRED) on Linux:
 *
 *   - `lk` is held on entry. prepare_to_wait() enqueues us on `cv` and sets
 *     TASK_INTERRUPTIBLE WHILE `lk` IS STILL HELD -- so a waker holding the
 *     same `lk` cannot slip a signal past us before we are on the queue.
 *   - if a signal is already pending we do NOT sleep (matches wait_event's
 *     `if (___wait_is_interruptible && __int) break;` before schedule()).
 *   - otherwise we drop `lk`, schedule(), and re-acquire `lk`.
 *   - finish_wait() dequeues and restores TASK_RUNNING.
 *   - returns nonzero iff a signal is pending. The CALLER re-tests the
 *     predicate first (predicate has priority over the interrupt), exactly
 *     as wait_event checks `condition` before the interruptible test.
 */
static inline void exec_cv_init(exec_cv_t *cv) { init_waitqueue_head(cv); }

static inline int exec_cv_wait(exec_cv_t *cv, exec_lock_t *lk)
{
	DEFINE_WAIT(__w);
	int intr;

	prepare_to_wait(cv, &__w, TASK_INTERRUPTIBLE);
	if (signal_pending(current)) {
		intr = 1;                 /* do not sleep with a signal pending */
	} else {
		spin_unlock(lk);
		schedule();
		spin_lock(lk);
		intr = signal_pending(current);
	}
	finish_wait(cv, &__w);
	return intr;
}

/* exec_cv_wait_timeout (Phase G) -- exec_cv_wait with a bounded sleep, the
 * faithful one-iteration expansion of wait_event_interruptible_TIMEOUT. Same cv
 * contract (caller holds `lk`, loops re-testing the predicate); additionally,
 * `*timed_out` is set nonzero iff the full `ms` elapsed with no wake (so the
 * caller can run a bounded re-scan on a real timeout, as enq_wait_sync does for
 * deadlock detection). Returns nonzero iff interrupted, exactly like
 * exec_cv_wait. Each call re-arms the full `ms`; that matches the executive's
 * former outer wait_event_interruptible_timeout re-arm, and is sound because the
 * sole waker (try_grant_waiters) always mutates the predicate before signalling,
 * so a wake with the predicate still false is not a normal event. */
static inline int exec_cv_wait_timeout(exec_cv_t *cv, exec_lock_t *lk,
				       unsigned int ms, int *timed_out)
{
	DEFINE_WAIT(__w);
	long ret;
	int intr;

	*timed_out = 0;
	prepare_to_wait(cv, &__w, TASK_INTERRUPTIBLE);
	if (signal_pending(current)) {
		intr = 1;                 /* do not sleep with a signal pending */
	} else {
		spin_unlock(lk);
		ret = schedule_timeout(msecs_to_jiffies(ms));
		spin_lock(lk);
		if (ret == 0)
			*timed_out = 1;   /* the full timeout elapsed, no wake */
		intr = signal_pending(current);
	}
	finish_wait(cv, &__w);
	return intr;
}

/* wake ONE / wake ALL. The executive's event flags wake ALL waiters
 * (wake_up_interruptible), so facilities use exec_cv_broadcast; exec_cv_signal
 * is provided for completeness of the contract. */
static inline void exec_cv_signal(exec_cv_t *cv)    { wake_up_interruptible_nr(cv, 1); }
static inline void exec_cv_broadcast(exec_cv_t *cv) { wake_up_interruptible(cv); }
static inline void exec_cv_destroy(exec_cv_t *cv)   { (void)cv; /* no-op on Linux */ }

/*
 * exec_cv_wait_rx (FC-P0.16, exec_kbackend.h SS1b) -- the SS2 cv contract with
 * an exec_rxlock_t as the interlock, THREAD CONTEXT ONLY (the fork thread is
 * the sole caller: cf_bind.c's cfb_wait).
 *
 * TASK_INTERRUPTIBLE, NOT TASK_UNINTERRUPTIBLE -- a DELIBERATE, MEASURED
 * DEVIATION from design SS3.2.3's literal Linux pseudocode ("prepare_to_wait
 * (TASK_UNINTERRUPTIBLE)"), found by the FC-P0.16 R3 same-CPU hammer, not by
 * inspection: exec_cv_signal/exec_cv_broadcast are UNCHANGED per the ruling
 * (still wake_up_interruptible{,_nr} above), and Linux's wake_up_interruptible
 * only wakes waiters in TASK_INTERRUPTIBLE state -- try_to_wake_up's `p->state
 * & state` test is false against a TASK_UNINTERRUPTIBLE sleeper, so a
 * TASK_UNINTERRUPTIBLE exec_cv_wait_rx paired with the ruling's own "unchanged"
 * broadcast is a LOST WAKEUP by construction: the fork thread would sleep
 * forever on the first wait, exactly what the hammer's DRAIN_OK/WORK_DISPATCHED
 * assertions caught (dispatched stayed 0 while enqueued/posted grew). Contract
 * symmetry is preserved on the RETURN side: this still always returns 0 (no
 * signal semantics), because the fork kthread never calls allow_signal(), so
 * signal_pending(current) is never true for it -- TASK_INTERRUPTIBLE is
 * observationally identical to TASK_UNINTERRUPTIBLE for THIS specific thread,
 * while actually pairing with the wake primitive the ruling keeps unchanged.
 * prepare_to_wait enqueues on `cv` WHILE `l` is still held (irqsave, so no
 * receive-level interrupt can slip in), so a waker sharing `l` cannot lose
 * the wakeup -- the same lost-wakeup-free argument as exec_cv_wait, with the
 * rxlock's saved irq flags restored on the re-acquire.
 */
static inline int exec_cv_wait_rx(exec_cv_t *cv, exec_rxlock_t *l,
				   exec_rxflags_t *flags)
{
	DEFINE_WAIT(__w);

	prepare_to_wait(cv, &__w, TASK_INTERRUPTIBLE);
	spin_unlock_irqrestore(l, *flags);
	schedule();
	spin_lock_irqsave(l, *flags);
	finish_wait(cv, &__w);
	return 0;
}

/* ---- 3. user <-> kernel copy (normalized 0 / EXEC_EFAULT) ----
 * The __user annotation is re-applied here so a portable facility can pass a
 * plain pointer (the design record drops __user in the shared core). */
static inline int exec_copyin(void *kdst, const void *usrc, size_t n)
{
	return copy_from_user(kdst, (const void __user *)usrc, n) ? EXEC_EFAULT : 0;
}

static inline int exec_copyout(void *udst, const void *ksrc, size_t n)
{
	return copy_to_user((void __user *)udst, ksrc, n) ? EXEC_EFAULT : 0;
}

/* ---- 4. kernel memory ---- */
static inline void *exec_zalloc(size_t n)        { return kzalloc(n, GFP_KERNEL); }
static inline void *exec_zalloc_atomic(size_t n) { return kzalloc(n, GFP_ATOMIC); }
static inline void *exec_alloc(size_t n)         { return kmalloc(n, GFP_KERNEL); }
static inline void  exec_free(void *p)           { kfree(p); }

/* ---- 5. host-task binding (Phase F; see exec_kbackend.h) ----
 * The opaque handle is the thread group's struct pid (task_tgid); the pinned
 * handle is a reference-counted struct task_struct. Both forward to exactly the
 * primitives vms_proctab.c used before this seam existed, so the module is
 * behaviour-identical. */
typedef struct pid          exec_task_ref_t;   /* PCB pid_ref: the tgid's pid */
typedef struct task_struct  exec_task_pin_t;   /* a pinned (referenced) task */

static inline int exec_current_is_privileged(void) { return capable(CAP_SYS_ADMIN); }

/* exec_current_uid/gid (vms-31b): the REAL uid/gid of `current`, mapped into the
 * initial user namespace -- exactly the reads the device table's caller_uic()
 * did before this seam existed, so the module is behaviour-identical. */
static inline uint32_t exec_current_uid(void)
{
	return (uint32_t)from_kuid(&init_user_ns, current_uid());
}
static inline uint32_t exec_current_gid(void)
{
	return (uint32_t)from_kgid(&init_user_ns, current_gid());
}

static inline int exec_task_alive(exec_task_ref_t *ref)
{
	struct task_struct *task;

	if (!ref)
		return 0;
	rcu_read_lock();
	task = pid_task(ref, PIDTYPE_PID);
	rcu_read_unlock();
	return task != NULL;
}

static inline exec_task_pin_t *exec_task_pin(exec_task_ref_t *ref)
{
	struct task_struct *task;

	if (!ref)
		return NULL;
	rcu_read_lock();
	task = pid_task(ref, PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	return task;
}

static inline void exec_task_unpin(exec_task_pin_t *pin) { put_task_struct(pin); }

/*
 * exec_task_read_acct - the exact reads vms_proctab.c's fill_proc_acct did,
 * moved behind the seam so the facility keeps only the VMS unit/field mapping.
 * cpu_ns = utime+stime (ns); page_faults = min+maj faults; create_wall_ns is
 * the boot-clock start converted to a Unix-epoch wall time via the current
 * boot/real pair; rss via get_task_mm/get_mm_rss (get_task_mm may sleep, so
 * this whole op may sleep and must run with no lock held). NUL of a kernel
 * thread's mm leaves has_rss 0, as before.
 */
static inline void exec_task_read_acct(exec_task_pin_t *pin,
				       struct exec_proc_acct *out)
{
	struct task_struct *task = pin;
	u64 created_boot_ns, now_boot_ns, wall_now_ns;
	struct timespec64 now_wall;
	struct mm_struct *mm;

	out->cpu_ns      = task->utime + task->stime;
	out->has_cpu     = 1;                          /* sourced from the task (vms-f62) */
	out->page_faults = (u64)task->min_flt + (u64)task->maj_flt;
	out->has_faults  = 1;                          /* sourced from the task (vms-f62) */

	created_boot_ns = task->start_boottime;
	now_boot_ns     = ktime_get_boottime_ns();
	ktime_get_real_ts64(&now_wall);
	wall_now_ns     = (u64)now_wall.tv_sec * NSEC_PER_SEC + now_wall.tv_nsec;
	out->create_wall_ns = (now_boot_ns >= created_boot_ns)
				? wall_now_ns - (now_boot_ns - created_boot_ns)
				: wall_now_ns;
	out->has_create = 1;                           /* sourced from the task (vms-f62) */

	out->rss_pages = 0;
	out->has_rss   = 0;
	mm = get_task_mm(task);
	if (mm) {
		out->rss_pages = (u64)get_mm_rss(mm);
		out->has_rss   = 1;
		mmput(mm);
	}
}

/* ---- 6. RCU-lite deferred reclaim (Phase F; see exec_kbackend.h) ---- */
typedef struct rcu_head exec_rcu_head_t;

static inline void exec_rcu_read_lock(void)   { rcu_read_lock(); }
static inline void exec_rcu_read_unlock(void) { rcu_read_unlock(); }
static inline void exec_free_deferred(exec_rcu_head_t *h,
				      void (*fn)(exec_rcu_head_t *))
{
	call_rcu(h, fn);
}

/* ---- 7. sleepable mutex (Phase F; see exec_kbackend.h) ---- */
typedef struct mutex exec_mutex_t;

#define EXEC_DEFINE_MUTEX(name) DEFINE_MUTEX(name)
static inline void exec_mutex_init(exec_mutex_t *m)    { mutex_init(m); }
static inline void exec_mutex_lock(exec_mutex_t *m)    { mutex_lock(m); }
static inline void exec_mutex_unlock(exec_mutex_t *m)  { mutex_unlock(m); }
static inline void exec_mutex_destroy(exec_mutex_t *m) { mutex_destroy(m); }

/* ---- 8. block-device resolution (vms-31b; see exec_kbackend.h) ----
 * Trivial forwarders to the exact block-layer primitives the device table used
 * before this seam existed: lookup_bdev() resolves a /dev path to a dev_t
 * without opening the device, and MAJOR()/MINOR() split it -- so the converted
 * vms_devtab.c compiles to byte-identical behaviour. */
typedef dev_t exec_dev_t;

static inline int exec_blockdev_lookup(const char *path, exec_dev_t *out)
{
	return lookup_bdev(path, out) ? -1 : 0;
}
static inline unsigned int exec_blockdev_major(exec_dev_t dev) { return MAJOR(dev); }
static inline unsigned int exec_blockdev_minor(exec_dev_t dev) { return MINOR(dev); }

/*
 * READ one 512-byte logical block off a backing block device, for the Files-11
 * ODS-2 ACP $MOUNT (vms-127; see exec_kbackend.h). Opens the device READ-ONLY
 * and NON-EXCLUSIVELY (holder == NULL), then reads the one block with a
 * SYNCHRONOUS bio (submit_bio_wait) rather than the buffer cache: __bread /
 * sb_bread assume a mounted-filesystem context (a superblock with a set block
 * size), which the ACP does not have -- it reads a RAW disk unit before any
 * volume is mounted. submit_bio_wait is the direct, self-contained block read
 * (bi_sector is in 512-byte units, matching ODS-2 LBNs) and does not depend on
 * the bdev's buffer-cache block-size state. All PUBLIC, documented block-layer
 * APIs -- no Linux source is copied (Rule 8).
 *
 * VERSION-GUARDED across the bdev-open API split so the SAME header compiles on
 * BOTH kernels this module targets: the QEMU-test build (Ubuntu 6.8, out-of-tree)
 * and the shipped bootable build (linux-6.12 LTS, distro/Dockerfile.bootable).
 * 6.9 replaced bdev_open_by_dev()/struct bdev_handle with
 * bdev_file_open_by_dev()/struct file (+ file_bdev/fput). This inline is only
 * CALLED under -DOVMX_ODS2_KERNEL (the out-of-tree vms.ko), but it still has to
 * COMPILE everywhere the header is included -- an unconditional reference to a
 * symbol the bootable 6.12 kernel lacks would break the bootable modpost even
 * unused (the #623 class of breakage), so the guard is mandatory, not cosmetic.
 */
/*
 * exec_bdev_get_cached / exec_bdev_cache_release_all (rd vms-648) -- the
 * BACKING-DEVICE HANDLE CACHE the two block primitives below draw on. DEFINED in
 * src/kernel/vms_module.c (Linux module core, linked into both the out-of-tree
 * QEMU-test vms.ko and the in-tree bootable vms.ko), DECLARED here because the
 * inline primitives call it.
 *
 * WHY (the vms-648 flake): the ODS-2 ACP reaches the raw disk one 512-byte LBN
 * per call, and opening+closing the block device on EVERY block
 * (bdev_file_open_by_dev + fput) is pathologically slow -- a PRODUCT INSTALL
 * writes thousands of blocks through the ACP, and on a slow single-CPU TCG CI
 * runner the per-block open/close pushed the R1 release-install e2e's install
 * step past its timeout (the install "hung" at PCSI Configuring). The NetBSD twin
 * (vms_blockdev_netbsd.c) already caches its backing vnode and reads/writes off
 * it; this is the missing Linux equivalent. exec_bdev_get_cached opens each
 * backing device ONCE (READ|WRITE, non-exclusive) and hands back the cached
 * struct block_device for reuse; the handle is released at module exit
 * (exec_bdev_cache_release_all from vms_exit), the OS-lifetime analogue of
 * NetBSD's release-at-detach. Only the OPEN is amortized -- every block is still
 * a real synchronous bio to a real device (INV-6 / Rule 8, public block-layer
 * APIs only). A miss the cache cannot hold (table full, or the open fails)
 * returns NULL, and the primitives fall back to the original open-per-call path,
 * so correctness never depends on the cache.
 */
struct block_device *exec_bdev_get_cached(dev_t devt);
void exec_bdev_cache_release_all(void);

static inline int exec_blockdev_read_block(unsigned int major, unsigned int minor,
					   uint64_t lbn, void *buf, size_t buflen)
{
	dev_t devt = MKDEV(major, minor);
	struct block_device *bdev;
	struct bio bio;
	struct bio_vec bvec;
	struct page *page;
	int ret = -1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	struct file *bf;
#else
	struct bdev_handle *bh_open;
#endif

	if (!buf || buflen < 512)
		return -1;

	/* Fast path (vms-648): reuse the cached, already-open backing handle. */
	bdev = exec_bdev_get_cached(devt);
	if (bdev) {
		page = alloc_page(GFP_KERNEL);
		if (page) {
			bio_init(&bio, bdev, &bvec, 1, REQ_OP_READ);
			bio.bi_iter.bi_sector = (sector_t)lbn;   /* 512-byte units == ODS-2 LBN */
			__bio_add_page(&bio, page, 512, 0);
			if (submit_bio_wait(&bio) == 0) {
				memcpy(buf, page_address(page), 512);
				ret = 0;
			}
			bio_uninit(&bio);
			__free_page(page);
		}
		return ret;
	}

	/* Fallback: the cache could not hold this device -- open per call. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	bf = bdev_file_open_by_dev(devt, BLK_OPEN_READ, NULL, NULL);
	if (IS_ERR(bf))
		return -1;
	bdev = file_bdev(bf);
#else
	bh_open = bdev_open_by_dev(devt, BLK_OPEN_READ, NULL, NULL);
	if (IS_ERR(bh_open))
		return -1;
	bdev = bh_open->bdev;
#endif

	page = alloc_page(GFP_KERNEL);
	if (page) {
		bio_init(&bio, bdev, &bvec, 1, REQ_OP_READ);
		bio.bi_iter.bi_sector = (sector_t)lbn;   /* 512-byte units == ODS-2 LBN */
		__bio_add_page(&bio, page, 512, 0);
		if (submit_bio_wait(&bio) == 0) {
			memcpy(buf, page_address(page), 512);
			ret = 0;
		}
		bio_uninit(&bio);
		__free_page(page);
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	fput(bf);
#else
	bdev_release(bh_open);
#endif
	return ret;
}

/*
 * WRITE one 512-byte logical block to a backing block device, the write twin of
 * exec_blockdev_read_block above -- for the Files-11 ODS-2 ACP IO$_WRITEVBLK /
 * implicit-extend path (vms-c60; see exec_kbackend.h). Opens the device
 * READ-WRITE and NON-EXCLUSIVELY (holder == NULL), then commits the one block
 * with a SYNCHRONOUS write bio (submit_bio_wait, REQ_OP_WRITE): the ACP writes a
 * RAW disk unit (there is no mounted super_block behind an ACP volume, so no
 * page cache / sb_getblk), exactly as its reads do. bi_sector is in 512-byte
 * units, matching ODS-2 LBNs. All PUBLIC, documented block-layer APIs -- no
 * Linux source is copied (Rule 8).
 *
 * VERSION-GUARDED across the bdev-open API split for the SAME reason the read
 * twin is (6.9's bdev_file_open_by_dev / file_bdev / fput vs the older
 * bdev_open_by_dev / bdev_handle / bdev_release), so the one header compiles on
 * both the QEMU-test (6.8) and shipped bootable (6.12) kernels; called only
 * under -DOVMX_ODS2_KERNEL but it must COMPILE everywhere the header is
 * included (the #623 dangling-symbol class).
 */
static inline int exec_blockdev_write_block(unsigned int major, unsigned int minor,
					    uint64_t lbn, const void *buf, size_t buflen)
{
	dev_t devt = MKDEV(major, minor);
	struct block_device *bdev;
	struct bio bio;
	struct bio_vec bvec;
	struct page *page;
	int ret = -1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	struct file *bf;
#else
	struct bdev_handle *bh_open;
#endif

	if (!buf || buflen < 512)
		return -1;

	/* Fast path (vms-648): reuse the cached, already-open backing handle. */
	bdev = exec_bdev_get_cached(devt);
	if (bdev) {
		page = alloc_page(GFP_KERNEL);
		if (page) {
			memcpy(page_address(page), buf, 512);
			bio_init(&bio, bdev, &bvec, 1, REQ_OP_WRITE);
			bio.bi_iter.bi_sector = (sector_t)lbn;   /* 512-byte units == ODS-2 LBN */
			__bio_add_page(&bio, page, 512, 0);
			if (submit_bio_wait(&bio) == 0)
				ret = 0;
			bio_uninit(&bio);
			__free_page(page);
		}
		return ret;
	}

	/* Fallback: the cache could not hold this device -- open per call. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	bf = bdev_file_open_by_dev(devt, BLK_OPEN_WRITE, NULL, NULL);
	if (IS_ERR(bf))
		return -1;
	bdev = file_bdev(bf);
#else
	bh_open = bdev_open_by_dev(devt, BLK_OPEN_WRITE, NULL, NULL);
	if (IS_ERR(bh_open))
		return -1;
	bdev = bh_open->bdev;
#endif

	page = alloc_page(GFP_KERNEL);
	if (page) {
		memcpy(page_address(page), buf, 512);
		bio_init(&bio, bdev, &bvec, 1, REQ_OP_WRITE);
		bio.bi_iter.bi_sector = (sector_t)lbn;   /* 512-byte units == ODS-2 LBN */
		__bio_add_page(&bio, page, 512, 0);
		if (submit_bio_wait(&bio) == 0)
			ret = 0;
		bio_uninit(&bio);
		__free_page(page);
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	fput(bf);
#else
	bdev_release(bh_open);
#endif
	return ret;
}

/* ---- 11. primary Ethernet net device (vms-9d2; see exec_kbackend.h) ----
 * NIC-agnostic: walk the host's net devices through the GENERIC netdev API and
 * return the first non-loopback Ethernet controller, naming no driver. This is
 * the exact primitive set the block seam uses, one layer over: for_each_netdev
 * enumerates init_net's devices under rtnl_lock (a mutex -- safe from the
 * module-init process context the device table probes from), IFF_LOOPBACK and
 * ARPHRD_ETHER classify without touching any driver-private state, and
 * netif_carrier_ok reads the generic link state. No code is copied from the
 * Linux source (Rule 8); these are all public, documented kernel APIs. */
static inline int exec_netdev_primary(char *name, unsigned int namesz, int *link_up)
{
	struct net_device *dev;
	int found = -1;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if (dev->flags & IFF_LOOPBACK)
			continue;
		if (dev->type != ARPHRD_ETHER)
			continue;
		if (name && namesz) {
			/* Bounded, self-contained copy (no strscpy dependency in this
			 * header): dev->name is a NUL-terminated IFNAMSIZ string. */
			unsigned int i;
			for (i = 0; i + 1 < namesz && dev->name[i]; i++)
				name[i] = dev->name[i];
			name[i] = '\0';
		}
		if (link_up)
			*link_up = netif_carrier_ok(dev) ? 1 : 0;
		found = 0;
		break;
	}
	rtnl_unlock();
	return found;
}

/* ---- 9. store/load memory barriers (vms-d61; see exec_kbackend.h) ----
 * Trivial forwarders to the exact barriers the logical-name seqlock used
 * before this seam existed (smp_wmb between the generation bumps and the entry
 * stores), so the converted vms_lnm.c compiles to byte-identical behaviour. */
static inline void exec_membar_producer(void) { smp_wmb(); }
static inline void exec_membar_consumer(void) { smp_rmb(); }

/* ---- 10. userspace-publishable arena (vms-d61; see exec_kbackend.h) ----
 * The allocation half of the arena seam. vmalloc_user() is the exact call the
 * logical-name arena used before this seam existed: page-aligned, zeroed, and
 * the one allocation the mmap glue's remap_vmalloc_range accepts -- so the
 * converted vms_lnm.c is behaviour-identical. The mmap-time mapping itself is
 * NOT here: it stays as raw Linux glue in vms_module.c's vms_lnm_mmap, which
 * reads the base back through the facility's accessor. */
typedef void *exec_arena_t;
static inline void *exec_arena_alloc(size_t n) { return vmalloc_user(n); }
static inline void  exec_arena_free(void *arena) { vfree(arena); }

/* ================================================================
 * §12  Host TCP client socket (vms-9951; BGn: INET facility).
 *
 * exec_socket_t is a REFERENCE-COUNTED holder over the host in-kernel socket:
 * the channel holds one reference, the Linux readiness poll fd (a Linux rind,
 * src/kernel/vms_bg_pollfd.c) holds a second via exec_socket_get, so the socket
 * outlives a poll fd still open when the channel is $DASSGN'd. The IP stack is
 * Linux's (sock_create_kern into init_net); OVMX never reimplements transport.
 * Addresses cross the seam as raw network-order fields (no struct sockaddr_in in
 * shared core -- the backend builds the sockaddr).
 * ================================================================ */
struct exec_socket_holder {
	struct socket *sock;
	struct kref kref;
	int promisc_ifindex;   /* vms-a84d: nonzero if exec_l2_open put this ifindex
				* into promiscuous mode (so a directed cluster HELLO to
				* the aa:00:04:00:<sysid> logical addr reaches the NIC
				* whose real MAC differs); decremented on free. 0 for
				* TCP/ICMP holders. */
};
typedef struct exec_socket_holder *exec_socket_t;

static inline void exec_socket_holder_free(struct kref *kref)
{
	struct exec_socket_holder *h =
		container_of(kref, struct exec_socket_holder, kref);
	if (h->promisc_ifindex) {
		struct net_device *dev =
			dev_get_by_index(&init_net, h->promisc_ifindex);
		if (dev) {
			rtnl_lock();
			dev_set_promiscuity(dev, -1);
			rtnl_unlock();
			dev_put(dev);
		}
	}
	if (h->sock)
		sock_release(h->sock);
	kfree(h);
}

static inline int exec_socket_create(exec_socket_t *out)
{
	struct exec_socket_holder *h;
	struct socket *sock;
	int rc;

	*out = NULL;
	rc = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (rc)
		return rc;
	h = kmalloc(sizeof(*h), GFP_KERNEL);
	if (!h) {
		sock_release(sock);
		return -ENOMEM;
	}
	h->sock = sock;
	h->promisc_ifindex = 0;     /* vms-a84d: TCP/ICMP holders never go promisc */
	kref_init(&h->kref);        /* the channel's reference */
	*out = h;
	return 0;
}

/* Raw ICMP socket for PING (vms-80b): identical holder discipline to
 * exec_socket_create, only the type/protocol differ (SOCK_RAW/IPPROTO_ICMP).
 * The caller builds the ICMP echo-request datagram and its checksum itself; a
 * connected raw socket then sends/receives it through the shared
 * exec_socket_send / exec_socket_recv. A kernel socket bypasses the CAP_NET_RAW
 * check a userspace raw socket would face. */
static inline int exec_socket_create_icmp(exec_socket_t *out)
{
	struct exec_socket_holder *h;
	struct socket *sock;
	int rc;

	*out = NULL;
	rc = sock_create_kern(&init_net, AF_INET, SOCK_RAW, IPPROTO_ICMP, &sock);
	if (rc)
		return rc;
	h = kmalloc(sizeof(*h), GFP_KERNEL);
	if (!h) {
		sock_release(sock);
		return -ENOMEM;
	}
	h->sock = sock;
	h->promisc_ifindex = 0;     /* vms-a84d: TCP/ICMP holders never go promisc */
	kref_init(&h->kref);        /* the channel's reference */
	*out = h;
	return 0;
}

static inline void exec_socket_get(exec_socket_t s) { kref_get(&s->kref); }
static inline void exec_socket_release(exec_socket_t s)
{
	kref_put(&s->kref, exec_socket_holder_free);
}

static inline int exec_socket_connect(exec_socket_t s, uint16_t family,
				      uint16_t port_be, uint32_t addr_be)
{
	struct sockaddr_in sa;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = family;
	sa.sin_port = port_be;              /* network byte order, straight through */
	sa.sin_addr.s_addr = addr_be;
	return kernel_connect(s->sock, (struct sockaddr *)&sa, sizeof(sa), 0);
}

static inline long exec_socket_send(exec_socket_t s, const void *buf, size_t len)
{
	struct msghdr msg;
	struct kvec vec;

	memset(&msg, 0, sizeof(msg));
	vec.iov_base = (void *)buf;
	vec.iov_len = len;
	return kernel_sendmsg(s->sock, &msg, &vec, 1, len);
}

static inline long exec_socket_recv(exec_socket_t s, void *buf, size_t len)
{
	struct msghdr msg;
	struct kvec vec;

	memset(&msg, 0, sizeof(msg));
	vec.iov_base = buf;
	vec.iov_len = len;
	return kernel_recvmsg(s->sock, &msg, &vec, 1, len, 0);
}

static inline int exec_socket_shutdown(exec_socket_t s)
{
	return kernel_sock_shutdown(s->sock, SHUT_RDWR);
}

static inline int exec_socket_getname(exec_socket_t s, int peer, uint16_t *family,
				      uint16_t *port_be, uint32_t *addr_be)
{
	struct sockaddr_storage ss;
	struct sockaddr_in *sin;
	int rc;

	memset(&ss, 0, sizeof(ss));
	rc = peer ? kernel_getpeername(s->sock, (struct sockaddr *)&ss)
		  : kernel_getsockname(s->sock, (struct sockaddr *)&ss);
	if (rc < 0)
		return rc;
	if (ss.ss_family != AF_INET)
		return -EAFNOSUPPORT;       /* IPv6 not carried by this tuple yet */
	sin = (struct sockaddr_in *)&ss;
	*family = sin->sin_family;
	*port_be = sin->sin_port;
	*addr_be = sin->sin_addr.s_addr;
	return 0;
}

static inline int exec_socket_setopt_int(exec_socket_t s, int level, int name, int val)
{
	struct socket *sock = s->sock;

	/* SOL_SOCKET options must go through sock_setsockopt directly (->setsockopt
	 * reaches only the protocol/IP handlers); every other level rides ->ops. */
	if (level == SOL_SOCKET)
		return sock_setsockopt(sock, level, name, KERNEL_SOCKPTR(&val), sizeof(val));
	if (sock->ops && sock->ops->setsockopt)
		return sock->ops->setsockopt(sock, level, name, KERNEL_SOCKPTR(&val), sizeof(val));
	return -ENOPROTOOPT;
}

static inline int exec_socket_getopt_int(exec_socket_t s, int level, int name, int *out)
{
	struct sock *sk = s->sock->sk;

	/* ->getsockopt takes user pointers (unusable from kernel context); read the
	 * live socket state directly for the integer whitelist OpenSSH probes. Each
	 * value is the socket's genuine current state; anything else is a HONEST
	 * -ENOPROTOOPT (the caller maps it to SS$_BADPARAM), never a faked value. */
	if (!sk)
		return -EINVAL;
	if (level == SOL_SOCKET && name == SO_KEEPALIVE) {
		*out = sock_flag(sk, SOCK_KEEPOPEN) ? 1 : 0; return 0;
	}
	if (level == SOL_SOCKET && name == SO_REUSEADDR) {
		*out = sk->sk_reuse ? 1 : 0; return 0;
	}
	if (level == SOL_SOCKET && name == SO_ERROR) {
		*out = -sock_error(sk); return 0;   /* pending error, cleared as getsockopt does */
	}
	if (level == IPPROTO_TCP && name == TCP_NODELAY) {
		*out = (tcp_sk(sk)->nonagle & TCP_NAGLE_OFF) ? 1 : 0; return 0;
	}
	if (level == IPPROTO_IP && name == IP_TOS) {
		*out = inet_sk(sk)->tos; return 0;
	}
	return -ENOPROTOOPT;
}

/* ---- server path (vms-698): bind / listen / accept ------------------------ */

static inline int exec_socket_bind(exec_socket_t s, uint16_t family,
				   uint16_t port_be, uint32_t addr_be)
{
	struct sockaddr_in sa;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = family;
	sa.sin_port = port_be;              /* network byte order, straight through */
	sa.sin_addr.s_addr = addr_be;
	return kernel_bind(s->sock, (struct sockaddr *)&sa, sizeof(sa));
}

static inline int exec_socket_listen(exec_socket_t s, int backlog)
{
	return kernel_listen(s->sock, backlog);
}

/* Accept one inbound connection, minting a NEW reference-counted holder for the
 * accepted socket (ref count 1 -- the accepting channel's reference), returned
 * via *out. Blocking (MAY SLEEP), exactly the $QIOW shape. The local/peer address
 * of *out is read afterwards through exec_socket_getname, not returned here. */
static inline int exec_socket_accept(exec_socket_t s, exec_socket_t *out)
{
	struct exec_socket_holder *h;
	struct socket *newsock = NULL;
	int rc;

	*out = NULL;
	rc = kernel_accept(s->sock, &newsock, 0);
	if (rc)
		return rc;
	h = kmalloc(sizeof(*h), GFP_KERNEL);
	if (!h) {
		sock_release(newsock);
		return -ENOMEM;
	}
	h->sock = newsock;
	kref_init(&h->kref);            /* the accepting channel's reference */
	*out = h;
	return 0;
}

/* Linux-ONLY accessor for the readiness poll-fd rind (src/kernel/vms_bg_pollfd.c):
 * the raw host socket for ->ops->poll delegation. NOT part of the substrate-
 * neutral seam -- no NetBSD counterpart (NetBSD uses kqueue; see vms-024). */
static inline struct socket *exec_socket_raw(exec_socket_t s) { return s->sock; }

/* ================================================================
 * SS13  Host AF_PACKET raw datalink socket (vms-7eb, auth slice of vms-1e4;
 * BGn:'s sibling one layer down -- the SCS cluster wire, ethertype 0x6007).
 *
 * Reuses exec_socket_t / struct exec_socket_holder (SS12 above) unchanged:
 * same kref discipline, same holder shape. Only the socket DOMAIN differs
 * (AF_PACKET/SOCK_RAW instead of AF_INET/SOCK_STREAM etc.), so a channel with an L2 socket
 * and a channel with a TCP/ICMP socket are interchangeable at the seam's type
 * level -- only the facility that opened it (vms_l2.c vs vms_bg.c) knows
 * which. A kernel socket bypasses the CAP_NET_RAW check a userspace raw
 * socket would face, exactly the exec_socket_create_icmp precedent (SS12).
 * ================================================================ */

/*
 * The executive OWNS every datalink it opens, so it brings the interface
 * administratively up itself rather than trusting something else already
 * did (vms-7eb) -- exactly as PEDRIVER brings up the LAN adapter when the
 * cluster starts (SS14 exec_lan_open, below, is the second caller). Without
 * this, bind()/dev_add_pack() succeeds on an administratively-down NIC but
 * the first send silently drops at the (still noop) qdisc instead of
 * reaching the wire -- dev_queue_xmit's NET_XMIT_* soft codes are all >= 0,
 * so a caller checking only "< 0 is failure" (both SS13 and SS14's xmit
 * primitives do, correctly, since a queued/congested send is not a hard
 * failure) never sees that drop. Idempotent (a no-op when already up);
 * needs the RTNL lock, as any flag change from kernel context does. Returns
 * 0 on success, a negative errno otherwise.
 */
static inline int exec_netdev_ensure_up(struct net_device *dev)
{
	int rc = 0;

	if (dev->flags & IFF_UP)
		return 0;
	rtnl_lock();
	rc = dev_change_flags(dev, dev->flags | IFF_UP, NULL);
	rtnl_unlock();
	return rc;
}

/* Open a kernel AF_PACKET/SOCK_RAW socket bound to `ifname`/`ethertype` (host
 * order; converted to network order here for both the socket's own protocol
 * and the sockaddr_ll bind -- struct sock's sk_protocol field mirrors AF_PACKET
 * convention: a raw packet socket dispatches by network-order ethertype).
 * *out gets a fresh holder (ref count 1, the caller's channel reference);
 * *out_ifindex gets the resolved interface index (dev_get_by_name), which the
 * caller needs again for exec_l2_send's destination sockaddr_ll. Returns 0 on
 * success, a negative errno otherwise (-ENODEV: no such interface). MAY SLEEP
 * (dev_get_by_name, kernel_bind). */
static inline int exec_l2_open(const char *ifname, uint16_t ethertype,
				uint32_t *out_ifindex, exec_socket_t *out)
{
	struct exec_socket_holder *h;
	struct socket *sock;
	struct net_device *dev;
	struct sockaddr_ll sll;
	int rc;

	*out = NULL;
	rc = sock_create_kern(&init_net, AF_PACKET, SOCK_RAW, htons(ethertype), &sock);
	if (rc)
		return rc;

	dev = dev_get_by_name(&init_net, ifname);
	if (!dev) {
		sock_release(sock);
		return -ENODEV;
	}

	rc = exec_netdev_ensure_up(dev);
	if (rc) {
		dev_put(dev);
		sock_release(sock);
		return rc;
	}

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ethertype);
	sll.sll_ifindex = dev->ifindex;

	rc = kernel_bind(sock, (struct sockaddr *)&sll, sizeof(sll));
	if (rc) {
		dev_put(dev);
		sock_release(sock);
		return rc;
	}

	h = kmalloc(sizeof(*h), GFP_KERNEL);
	if (!h) {
		dev_put(dev);
		sock_release(sock);
		return -ENOMEM;
	}
	h->sock = sock;
	h->promisc_ifindex = 0;
	kref_init(&h->kref);            /* the caller's channel reference */

	/* vms-a84d: put the NIC in PROMISCUOUS mode so a directed cluster HELLO
	 * addressed to OUR logical addr aa:00:04:00:<LE16(SCSSYSTEMID)> reaches us.
	 * The guest NIC's real MAC differs from that SCA address, so without
	 * promisc the hardware filter silently drops the peer member's directed
	 * solicit -- and then the VC never forms (start_acked stays 0, the
	 * NEW->MEMBER join sequencer never leaves JS_IDLE). Ref-counted;
	 * exec_socket_holder_free decrements it on close. */
	rtnl_lock();
	rc = dev_set_promiscuity(dev, 1);
	rtnl_unlock();
	if (rc) {
		kfree(h);
		dev_put(dev);
		sock_release(sock);
		return rc;
	}
	h->promisc_ifindex = dev->ifindex;

	*out = h;
	if (out_ifindex)
		*out_ifindex = (uint32_t)dev->ifindex;
	dev_put(dev);
	return 0;
}

/* Report `ifname`'s hardware (MAC) address. Independent of any open socket --
 * a plain interface-table lookup, exactly like exec_netdev_primary's classify
 * step above -- so a caller can re-query it without reopening. 0 (+ *mac) on
 * success, -ENODEV if no such interface. */
static inline int exec_l2_hwaddr(const char *ifname, uint8_t mac[6])
{
	struct net_device *dev;

	dev = dev_get_by_name(&init_net, ifname);
	if (!dev)
		return -ENODEV;
	memcpy(mac, dev->dev_addr, ETH_ALEN);
	dev_put(dev);
	return 0;
}

/* Send one frame's payload out `s` to `dst_mac` on `ifindex`, tagged with
 * `ethertype` (host order). Every send names its destination (sendto-style,
 * via msg_name) -- an L2 socket carries no connect step.
 *
 * SOCK_RAW (unlike SOCK_DGRAM) does NOT synthesize a link-layer header from
 * sll_addr/sll_protocol on send -- the caller's buffer IS the wire frame, so
 * this function builds the 14-byte Ethernet header (dst_mac, this interface's
 * OWN hardware address as src, ethertype) itself and sends it as a SEPARATE
 * leading kvec ahead of the caller's payload -- one kernel_sendmsg call, no
 * extra copy of the (possibly large) payload. sll_addr/sll_halen/sll_protocol
 * are still filled (some drivers' xmit path reads them; harmless either way).
 * Returns the PAYLOAD byte count sent (i.e. `len`'s counterpart, not
 * including the 14-byte header this function added), or negative on error.
 * MAY SLEEP (dev_get_by_index). Linux: kernel_sendmsg with a sockaddr_ll
 * msg_name, the datalink twin of exec_socket_send. */
static inline long exec_l2_send(exec_socket_t s, int ifindex, uint16_t ethertype,
				 const uint8_t dst_mac[6], const void *frame, size_t len)
{
	struct sockaddr_ll sll;
	struct msghdr msg;
	struct kvec vec;
	long n;

	/* vms-a84d: `frame` is ALREADY a complete ethernet frame. scsd.c's
	 * send_frame_raw() hands the datalink a fully-built frame (dst@0, src@6,
	 * ethertype@12, then the SCA payload), exactly as the AF_PACKET probe
	 * transmits it. A SOCK_RAW socket sends the caller's link-layer header
	 * VERBATIM -- we must NOT prepend our own. The earlier version built and
	 * prepended a second [dst|dev_addr|ethertype] header, double-encapsulating
	 * every frame with a 14-byte duplicate eth header (byte-identical to the
	 * real one) that a peer's NISCA/PEDRIVER input validator silently drops --
	 * which is why a real VAX never solicited a booted OVMX node. The dst and
	 * ethertype are already in `frame`; sll only names the egress interface. */
	(void)dst_mac;
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_ifindex = ifindex;
	sll.sll_protocol = htons(ethertype);

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &sll;
	msg.msg_namelen = sizeof(sll);
	vec.iov_base = (void *)frame;
	vec.iov_len = len;
	n = kernel_sendmsg(s->sock, &msg, &vec, 1, len);
	return n;   /* full frame bytes sent, or negative errno */
}

/* Receive one frame off `s` into `buf` (up to `buf_len`), honoring
 * `timeout_ms` (0 = block indefinitely). Sets sk_rcvtimeo directly on the raw
 * socket before the call -- the same field-level socket-state access already
 * established for the whitelisted getopt reads above (SO_KEEPALIVE/
 * TCP_NODELAY/...), not a new pattern. Returns 0 (+ *out_len) on success, a
 * negative errno on error or timeout (-EAGAIN). MAY SLEEP. */
static inline int exec_l2_recv(exec_socket_t s, void *buf, size_t buf_len,
				uint32_t timeout_ms, size_t *out_len)
{
	struct msghdr msg;
	struct kvec vec;
	long n;

	if (s->sock->sk)
		s->sock->sk->sk_rcvtimeo = timeout_ms
			? msecs_to_jiffies(timeout_ms) : MAX_SCHEDULE_TIMEOUT;

	memset(&msg, 0, sizeof(msg));
	vec.iov_base = buf;
	vec.iov_len = buf_len;
	n = kernel_recvmsg(s->sock, &msg, &vec, 1, buf_len, 0);
	if (n < 0)
		return (int)n;
	*out_len = (size_t)n;
	return 0;
}

/* exec_l2_close is deliberately NOT a distinct primitive: an L2 handle's
 * socket is released exactly like a TCP/ICMP one -- exec_socket_release
 * (SS12) drops the reference and, on the last drop, sock_release()s it. */

/* ================================================================
 * SS14..SS18  The cluster seam (FC-P0.1 contract; FC-P0.2 the REAL Linux
 * binding).
 *
 * FC-P0.1 froze the contract (exec_kbackend.h SS14..SS18, including CONTRACT
 * RULES 1 and 2). FC-P0.2 lands the real primitives here: dev_add_pack /
 * dev_queue_xmit / dev_mc_add / dev_mc_del / kthread_run / timer_list /
 * ktime_get_* / printk, proved by the rung-3 substrate contract test
 * (tests/qemu/test_kmod_cluster_seam.c, driven through the TEST-ONLY
 * OVMX_KTEST_CLUSTER_SEAM knob in src/kernel/vms_module.c): a veth pair
 * delivers a 0x6007 frame to rx_cb in softirq and exec_lan_xmit is seen on
 * the peer, multicast add is visible in `ip maddr` (== /proc/net/dev_mcast),
 * timer post-and-wake, kthread start/stop, exec_time monotone.
 *
 * The TYPES were already real as of FC-P0.1 (struct timer_list, struct
 * task_struct *): they fixed the sizes the core embeds by value, so this is a
 * body-only change, as designed.
 * ================================================================ */

typedef struct task_struct *exec_kthread_t;

typedef struct exec_timer {
	struct timer_list tl;      /* the real Linux timer this binding arms */
	void (*cb)(void *);
	void *ctx;
} exec_timer_t;

/*
 * SS14 LAN port. "One node has exactly one cluster port... these ops
 * therefore name no handle: the binding owns at most one open port"
 * (exec_kbackend.h SS14) -- so this backend keeps the open port's state in
 * one file-scope struct, exactly the "no handle crosses the seam" contract.
 * Only vms_pe.c calls these in the shipped executive; the TEST-ONLY knob in
 * vms_module.c (OVMX_KTEST_CLUSTER_SEAM) is the only other caller, and never
 * both at once.
 */
struct vms_lan_port {
	struct net_device *dev;    /* dev_get_by_name'd ref, held while open */
	struct packet_type pt;     /* registered via dev_add_pack */
	exec_lan_rx_cb_t   rx_cb;
	void              *rx_ctx;
};
static struct vms_lan_port vms_lan_port;

/*
 * dev_add_pack's func trampoline -- CONTRACT RULE 1 territory: this runs in
 * Linux's receive softirq (the packet_type dispatch out of
 * __netif_receive_skb_core). It does the MINIMUM needed to hand the core a
 * flat (frame, len) view of the wire frame before calling rx_cb, which is
 * where rule 1 (copy/enqueue/wake only, no protocol) actually binds:
 *
 *   - skb_linearize: a paged skb cannot be handed to rx_cb as one pointer;
 *     linearizing uses GFP_ATOMIC internally (__pskb_pull_tail), so it does
 *     not sleep and is safe here. A failure (OOM) drops the frame -- the
 *     same "pool empty" drop-and-count posture rule 1 already prescribes for
 *     the core's own queue, applied one step earlier.
 *   - the driver's eth_type_trans() already reset skb->mac_header to the
 *     frame's start and pulled skb->data past the 14-byte Ethernet header
 *     before this callback ever runs, so skb_mac_header(skb) is byte 0 (the
 *     destination MAC) and (skb->data - skb_mac_header(skb)) + skb->len is
 *     the WHOLE frame length -- exactly the "byte 0 = dest MAC" shape SS14
 *     promises the core, with no substrate offset base leaking through.
 */
static int vms_lan_rx_thunk(struct sk_buff *skb, struct net_device *dev,
			     struct packet_type *pt, struct net_device *orig_dev)
{
	struct vms_lan_port *port = container_of(pt, struct vms_lan_port, pt);
	const uint8_t *frame;
	uint32_t frame_len;

	(void)dev;
	(void)orig_dev;

	if (skb_linearize(skb))
		goto drop;   /* OOM: cannot present one contiguous frame -- drop */

	frame = skb_mac_header(skb);
	frame_len = skb->len + (uint32_t)(skb->data - frame);
	if (port->rx_cb)
		port->rx_cb(port->rx_ctx, frame, frame_len);
drop:
	kfree_skb(skb);
	return 0;
}

static inline int exec_lan_open(const char *ifname, uint16_t ethertype,
				exec_lan_rx_cb_t rx_cb, void *ctx)
{
	struct net_device *dev;

	if (vms_lan_port.dev)
		return (int)EXEC_SS_NOSUCHDEV;   /* "at most one open port" */

	dev = dev_get_by_name(&init_net, ifname);
	if (!dev)
		return (int)EXEC_SS_NOSUCHDEV;   /* honest: no such interface */

	/*
	 * vms-fc-e51: PEDRIVER OWNS this datalink exactly like SS13's
	 * exec_l2_open does, and needs the SAME bring-up (see
	 * exec_netdev_ensure_up's own comment, above SS13). Without it the
	 * port comes up (dev_add_pack succeeds on an administratively-down
	 * NIC) and CLUSTER_START reports SS$_NORMAL, but every HELLO
	 * dev_queue_xmit later posts is silently dropped by the still-noop
	 * qdisc (NET_XMIT_CN, >= 0 -- "success" by exec_lan_xmit's own "< 0
	 * is failure" rule) and nothing ever reaches the wire. Measured: a
	 * booted node's PEA0: reporting up while 3 tcpdump windows (20/40/
	 * 240s) on the peer's LAN saw zero frames from this node's MAC. A
	 * bring-up that itself fails is honestly refused -- no PEA0: for an
	 * interface this node cannot actually use (INV-6).
	 */
	if (exec_netdev_ensure_up(dev)) {
		dev_put(dev);
		return (int)EXEC_SS_NOSUCHDEV;
	}

	vms_lan_port.dev = dev;                  /* reference held until close */
	vms_lan_port.rx_cb = rx_cb;
	vms_lan_port.rx_ctx = ctx;
	vms_lan_port.pt.type = htons(ethertype);
	vms_lan_port.pt.dev = dev;
	vms_lan_port.pt.func = vms_lan_rx_thunk;
	dev_add_pack(&vms_lan_port.pt);
	return 0;
}

static inline void exec_lan_close(void)
{
	if (!vms_lan_port.dev)
		return;                           /* nothing open; a no-op close */
	dev_remove_pack(&vms_lan_port.pt);
	dev_put(vms_lan_port.dev);
	memset(&vms_lan_port, 0, sizeof(vms_lan_port));
}

/*
 * Transmit ONE complete Ethernet frame (source MAC already set by the caller
 * per SS14). alloc_skb + skb_put_data copies `frame` into a fresh skb headed
 * by the device's real link-layer reserve; skb_reset_mac_header + reading
 * h_proto back out of the frame we just wrote mirrors the driver-side setup
 * eth_type_trans does on receive, so tc/qdisc classification sees a
 * correctly-tagged skb. dev_queue_xmit's NET_XMIT_* soft codes (queued,
 * congested) are all >= 0 -- only a negative return is a hard failure. */
static inline int exec_lan_xmit(const uint8_t *frame, uint32_t len)
{
	struct sk_buff *skb;

	if (!vms_lan_port.dev)
		return (int)EXEC_SS_NOSUCHDEV;

	skb = alloc_skb(len + LL_RESERVED_SPACE(vms_lan_port.dev), GFP_KERNEL);
	if (!skb)
		return (int)EXEC_SS_NOSUCHDEV;

	skb_reserve(skb, LL_RESERVED_SPACE(vms_lan_port.dev));
	skb_put_data(skb, frame, len);
	skb->dev = vms_lan_port.dev;
	skb_reset_mac_header(skb);
	skb->protocol = eth_hdr(skb)->h_proto;

	if (dev_queue_xmit(skb) < 0)
		return (int)EXEC_SS_NOSUCHDEV;
	return 0;
}

static inline int exec_lan_mc_add(const uint8_t mac[6])
{
	if (!vms_lan_port.dev)
		return (int)EXEC_SS_NOSUCHDEV;
	return dev_mc_add(vms_lan_port.dev, mac) ? (int)EXEC_SS_NOSUCHDEV : 0;
}

static inline int exec_lan_mc_del(const uint8_t mac[6])
{
	if (!vms_lan_port.dev)
		return (int)EXEC_SS_NOSUCHDEV;
	return dev_mc_del(vms_lan_port.dev, mac) ? (int)EXEC_SS_NOSUCHDEV : 0;
}

static inline int exec_lan_hwaddr(uint8_t out[6])
{
	if (!vms_lan_port.dev)
		return (int)EXEC_SS_NOSUCHDEV;   /* untouched: never a fabricated MAC */
	memcpy(out, vms_lan_port.dev->dev_addr, ETH_ALEN);
	return 0;
}

static inline int exec_lan_mtu(uint32_t *out)
{
	if (!vms_lan_port.dev)
		return (int)EXEC_SS_NOSUCHDEV;
	*out = (uint32_t)vms_lan_port.dev->mtu;
	return 0;
}

static inline int exec_lan_link_up(int *out)
{
	if (!vms_lan_port.dev)
		return (int)EXEC_SS_NOSUCHDEV;   /* "port not open" is not "link down" */
	*out = netif_carrier_ok(vms_lan_port.dev) ? 1 : 0;
	return 0;
}

/* ---- SS15 fork context: kthread_run / kthread_stop (joins) /
 * kthread_should_stop. Linux's kthread_should_stop() reads `current` and
 * ignores its argument -- the handle parameter exists for the NetBSD twin,
 * which has no such implicit read (exec_kbackend.h SS15). ----
 *
 * THE HANDLE OWNS A TASK REFERENCE, AND IT MUST (found by the FC-P6.6 QEMU
 * hammer run: `BUG: kernel NULL pointer dereference ... kthread_stop+0x48`,
 * a task_struct whose usage refcount was already 0).
 *
 * Every thread body in this stack RETURNS ON ITS OWN when its work is done --
 * cf_run() returns once a stop has been requested AND the queues are drained,
 * cf_io_run() likewise -- and only THEN does the stopper call
 * exec_kthread_stop() to join it. On Linux a kthread that returns is
 * self-reaped (its parent kthreadd ignores SIGCHLD, so exit_notify() autoreaps
 * it), which drops the last reference and frees the task_struct AND the
 * `struct kthread` behind it. kthread_stop() then reads a freed task's
 * worker_private and dies -- and kthread_stop(9)'s own contract says so:
 * "If threadfn() may call kthread_exit() itself, the caller must ensure
 * task_struct can't go away."
 *
 * So the HANDLE holds a reference for its whole life. The NetBSD twin already
 * has this guarantee by construction (KTHREAD_MUSTJOIN keeps the lwp until
 * kthread_join), so this makes the two substrates' handles mean the same
 * thing, which is the entire point of the seam.
 */

static inline int exec_kthread_create(exec_kthread_t *t, int (*fn)(void *),
				      void *arg, const char *name)
{
	struct task_struct *task = kthread_run(fn, arg, "%s", name);

	if (IS_ERR(task)) {
		*t = NULL;
		return (int)EXEC_SS_NOSUCHDEV;
	}
	get_task_struct(task);   /* released by exec_kthread_stop */
	*t = task;
	return 0;
}

static inline void exec_kthread_stop(exec_kthread_t *t)
{
	if (t && *t) {
		struct task_struct *task = *t;

		/* Cleared FIRST so a second _stop on the same handle is the
		 * documented no-op rather than a second put of one reference. */
		*t = NULL;
		kthread_stop(task);
		put_task_struct(task);
	}
}

static inline int exec_kthread_should_stop(exec_kthread_t *t)
{
	(void)t;                         /* Linux reads `current`, not the handle */
	return kthread_should_stop();
}

/* ---- SS16 timers: timer_list + CONTRACT RULE 2 (a Linux timer callback runs
 * in softirq with a single `struct timer_list *` argument -- the trampoline
 * recovers {cb, ctx} via container_of and forwards to the core's cb, which
 * must itself honour rule 2: post and wake, nothing else). ---- */

static void vms_exec_timer_thunk(struct timer_list *tl)
{
	struct exec_timer *t = container_of(tl, struct exec_timer, tl);

	if (t->cb)
		t->cb(t->ctx);
}

static inline void exec_timer_init(exec_timer_t *t, void (*cb)(void *), void *ctx)
{
	t->cb = cb;
	t->ctx = ctx;
	timer_setup(&t->tl, vms_exec_timer_thunk, 0);
}

static inline void exec_timer_arm(exec_timer_t *t, uint32_t ms)
{
	mod_timer(&t->tl, jiffies + msecs_to_jiffies(ms));
}

static inline void exec_timer_cancel(exec_timer_t *t)
{
	del_timer_sync(&t->tl);
}

static inline void exec_timer_destroy(exec_timer_t *t)
{
	(void)t;                         /* Linux has no callout_destroy twin */
}

/* ---- SS17 time: VMS absolute time is 100ns ticks since 00:00 17-NOV-1858,
 * i.e. Unix epoch (1970) + 3,506,716,800 wall seconds; exec_ticks_ms is a
 * monotonic (never wall-clock) millisecond counter for deadline math. ---- */

#define VMS_EXEC_EPOCH_OFFSET_100NS (3506716800ULL * 10000000ULL)

static inline uint64_t exec_time_now_vms(void)
{
	return (uint64_t)(ktime_get_real_ns() / 100) + VMS_EXEC_EPOCH_OFFSET_100NS;
}

static inline uint64_t exec_ticks_ms(void)
{
	return (uint64_t)(ktime_get_ns() / 1000000ULL);
}

/* A macro, not a function: it forwards the caller's format string straight to
 * printk so the compiler's -Wformat checks the call site, and KERN_ERR is the
 * level the QEMU console actually shows. Already the real binding as of
 * FC-P0.1; unchanged here, listed for SS18 completeness. */
#define exec_console_printf(fmt, ...) printk(KERN_ERR fmt, ##__VA_ARGS__)

#endif /* OVMX_EXEC_KBACKEND_LINUX_H */
