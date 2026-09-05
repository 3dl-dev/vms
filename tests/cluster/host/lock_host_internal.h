/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lock_host_internal.h - the HOST stand-in for src/kernel/vms_internal.h,
 * scoped to exactly what src/kernel-core/vms_lock.c needs to compile (rd
 * FC-P4.9). Reached ONLY through the frozen quoted #include "vms_internal.h"
 * in vms_lock.c itself, redirected here by
 * tests/cluster/host/lock_shim/vms_internal.h (a 3-line forwarder placed
 * FIRST on the host build's include path -- see that file and
 * exec_kbackend_host.h's header comment for the mechanism).
 *
 * WHY A NEW FILE, NOT THE REAL vms_internal.h. The real src/kernel/
 * vms_internal.h is Linux-kernel-module glue for EVERY facility (process
 * table, event flags, ASTs, image activation, devices, ...), 1400+ lines,
 * built on raw <linux/list.h>/<linux/rbtree.h>/<linux/wait.h>/<linux/
 * spinlock.h>/<linux/hashtable.h> types -- struct vms_lock_entry and struct
 * vms_lock_resource in that file still use those CONCRETE Linux types
 * directly (e.g. `struct list_head res_granted;`, not `exec_list_node_t`),
 * which compiles on Linux only because exec_list_node_t etc. are Linux-
 * backend TYPEDEFS of those same concrete types. None of that compiles or
 * links on a plain host compiler. This file defines the SAME struct/macro
 * VOCABULARY vms_lock.c consumes (struct vms_proc/vms_lock_entry/
 * vms_lock_resource/vms_ast_state/vms_ast_entry, the SS__* status codes,
 * VMS_RES_HASH_BITS, VMS_AST_MAX_PER_MODE, the vms_local_csid/
 * vms_local_csid extern, and vms_ast_notify_arrival's prototype) using the
 * PORTABLE exec_* container/lock/cv types instead -- exactly the shape
 * design sec 3.9 describes vms_lock.c's struct layer eventually taking once
 * it is fully promoted into src/kernel-core/. Every field name, order, and
 * width matches src/kernel/vms_internal.h's own definitions (verified by
 * inspection against that file); only the FIELD TYPE differs (portable
 * exec_* vs raw Linux). This is OVMX's own internal struct layout (not a VMS
 * wire artifact), so Rule 8's clean-room bar does not apply to copying field
 * shapes from OVMX's own header -- it applies to the WIRE spec, which this
 * file never touches.
 *
 * The genuinely PORTABLE pieces (the ioctl argument structs -- struct
 * vms_enq_args, vms_deq_args, vms_dlm_xnode_args, vms_cluster_member, and
 * so on -- and the LCK_ / VMS_DLM_ / PSL_C_ constants) already live in the
 * real, substrate-neutral src/kernel/vms_ioctl.h (gated on __KERNEL__, with
 * a plain stdint.h fallback, already used from plain userspace by every
 * tests/qemu/test_syssvc_ program), so this file includes THAT header
 * unmodified rather than duplicating it.
 */

#ifndef OVMX_LOCK_HOST_INTERNAL_H
#define OVMX_LOCK_HOST_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

/* The container/lock/cv seam. vms_lock.c includes these itself too (all
 * header-guarded, so re-inclusion there is a no-op), but this file uses their
 * types in the struct definitions below and so needs them FIRST -- exactly
 * as the real vms_internal.h independently pulls <linux/list.h> etc. rather
 * than relying on include order from its caller. */
#include "exec_kbackend.h"
#include "exec_list.h"
#include "exec_hash.h"
#include "exec_rbtree.h"

/* The portable ioctl arg structs and the LCK_ / VMS_DLM_ / PSL_C_ constants
 * (real, unmodified src/kernel/vms_ioctl.h -- see file header). */
#include "vms_ioctl.h"

/* ================================================================
 * strscpy - Linux kernel string helper vms_lock.c calls (resource_find_or_
 * create, vms_lock_acp_vol_ex, vms_ioctl_getlki). Not in host libc; a tiny
 * bounded-copy stand-in. vms_lock.c never uses the return value, so this
 * need not reproduce Linux's ssize_t truncation-signalling contract exactly
 * -- it only needs to do what every call site actually relies on: copy at
 * most `size`-1 bytes and NUL-terminate.
 * ================================================================ */
static inline void strscpy(char *dst, const char *src, size_t size)
{
	size_t n;

	if (size == 0)
		return;
	n = strnlen(src, size - 1);
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/* ================================================================
 * SS$ status codes vms_lock.c uses (real values, copied from
 * src/kernel/vms_internal.h -- see that file's own ssdef.h cross-references).
 * ================================================================ */
#define SS__NORMAL      0x00000001
#define SS__BADPARAM    0x00000014
#define SS__INSFMEM     292          /* SS$_INSFMEM */
#define SS__NOTQUEUED   2488         /* SS$_NOTQUEUED */
#define SS__DEADLOCK    3594         /* SS$_DEADLOCK */
#define SS__IVLOCKID    8484         /* SS$_IVLOCKID */
#define SS__CANCELGRANT 8508         /* SS$_CVTUNGRANT */
#define SS__UNSUPPORTED 2296         /* SS$_UNSUPPORTED */

/* ================================================================
 * Lock-manager sizing constants (real values, copied from
 * src/kernel/vms_internal.h).
 * ================================================================ */
#define VMS_RES_HASH_BITS    10
#define VMS_AST_MAX_PER_MODE 64

/* ================================================================
 * AST plumbing (struct shapes mirror src/kernel/vms_internal.h's
 * vms_ast_entry/vms_ast_state field-for-field, portable types).
 * ================================================================ */
struct vms_ast_entry {
	exec_list_node_t list;
	uint64_t         astadr;
	uint64_t         astprm;
	uint8_t          acmode;
};

struct vms_ast_state {
	exec_list_head_t pending;
	int              count;
	int              enabled;
	exec_lock_t      lock;
};

/* ================================================================
 * struct vms_proc - scoped to the fields vms_lock.c touches:
 * ast[4] (indexed PSL_C_KERNEL..PSL_C_USER), current_mode/mode_lock (lock
 * conversion's acmode stamp), and the per-process lock list (locks/
 * lock_count/lock_list_lock). Anything vms_proc carries for OTHER
 * facilities (identity, job, terminal, ...) is out of scope for a lock-
 * manager-only host build -- consistent with exec_kbackend_host.h's own
 * "only what the caller needs" discipline.
 * ================================================================ */
struct vms_proc {
	struct vms_ast_state ast[4];
	uint8_t              current_mode;
	exec_lock_t          mode_lock;

	exec_list_head_t     locks;
	int                  lock_count;
	exec_lock_t          lock_list_lock;
};

/* ================================================================
 * struct vms_lock_entry / struct vms_lock_resource - field-for-field
 * mirrors of src/kernel/vms_internal.h's definitions (see that file's
 * "Lock entry" / "Lock resource" comments for the per-field rationale this
 * file does not repeat), with exec_* portable types in place of the raw
 * Linux ones.
 * ================================================================ */
struct vms_lock_entry {
	exec_list_node_t          proc_list;
	exec_list_node_t          res_granted;
	exec_list_node_t          res_waiting;
	exec_list_node_t          res_proxy;      /* FC-P4.4 proxy queue link */
	exec_rbtree_node_t        rb_node;
	uint32_t                  lkid;
	uint32_t                  granted_mode;
	uint32_t                  requested_mode;
	uint32_t                  flags;
	uint64_t                  astadr;
	uint64_t                  astprm;
	uint64_t                  blkastadr;
	uint64_t                  blkastprm;      /* FC-P4.4 */
	uint8_t                   valblk[LCK_VALBLK_SIZE];
	struct vms_lock_resource *resource;
	struct vms_proc          *proc;
	int                       waiting;
	int                       refcount;
	exec_cv_t                 wait_wq;
	int                       grant_state;
	uint8_t                   acmode;
	uint32_t                  req_csid;
	uint32_t                  req_lkid;
	uint32_t                  parent_id;
	/* PROXY LKB (FC-P4.4) -- see src/kernel/vms_internal.h for the per-field
	 * rationale; this file mirrors field presence/type, not byte layout. */
	uint8_t                   proxy;
	uint32_t                  master_csid;
	uint32_t                  master_lkid;
	uint32_t                  blkast_count;
};

struct vms_lock_resource {
	exec_hash_node_t          hash_node;
	char                      name[32];
	exec_list_head_t          granted;
	exec_list_head_t          waiting;
	exec_list_head_t          proxies;        /* FC-P4.4 proxy queue */
	uint8_t                   valblk[LCK_VALBLK_SIZE];
	exec_lock_t               lock;
	int                       refcount;
	struct vms_lock_resource *parent;

	/*
	 * THE DIRECTORY (FC-P4.3, src/kernel-core/vms_dlm_ldwv.h).
	 *
	 * hash16 is the resource name's 16-bit directory hash AS THE CLUSTER
	 * PUTS IT ON THE WIRE (Davis p. 6-50). It is LEARNED -- by
	 * vms_lock_dlm_learn_dir_hash() from a parsed cat-0x02 frame -- and
	 * never computed: the hash function is not published at the bit level,
	 * and a wrong value makes the directory node install the sender as
	 * master of somebody else's resource. hash_known 0 means "no lookup may
	 * be sent for this name", not "hash 0".
	 *
	 * dir_csid is the directory node the weight vector named for that hash;
	 * 0 there means THIS node (p. 6-32). It is meaningful only while
	 * dir_valid is set AND dir_gen still equals the vector's generation --
	 * which is how a cached resolution is discarded the moment the vector
	 * changes at a state transition (p. 6-33).
	 *
	 * master_csid is the node that masters the resource; 0 = unmastered.
	 */
	uint16_t            hash16;
	uint8_t             hash_known;
	uint8_t             dir_valid;
	uint32_t            dir_gen;
	uint32_t                  dir_csid;
	uint32_t                  master_csid;
};

/* ================================================================
 * This node's CSID, the one cluster global vms_lock.c reads (its real
 * definition lives in the test harness -- a host test fixes it to a real,
 * deliberately-chosen node id). The static membership vector
 * (dlm_member_csids / dlm_member_count) is GONE with FC-P4.3: the
 * membership a directory resolves over reaches the engine through the
 * injected dir_resolve/dir_generation ops (vms_dlm_proxy.h), so a test
 * that wants a cluster installs those ops rather than declaring one.
 * ================================================================ */
extern uint32_t vms_local_csid;

/* ================================================================
 * vms_ast_notify_arrival - defined in src/kernel-core/vms_ast.c (the AST
 * subsystem), not part of this host build. vms_lock.c calls it only on the
 * completion-AST / blocking-AST delivery paths (queue_completion_ast /
 * notify_blocking_asts), both gated on a nonzero astadr/blkastadr this
 * item's host test never sets (see test_lock_host.c) -- so the symbol is
 * referenced by the compiled object but never actually invoked at runtime.
 * A link-time stub lives in test_lock_host.c, clearly labelled there.
 * ================================================================ */
struct vms_proc;
void vms_ast_notify_arrival(struct vms_proc *proc);

/* ================================================================
 * vms_lock.c's own public entry points (real declarations for callers in a
 * SEPARATE translation unit -- test_lock_host.c -- mirroring the real
 * vms_internal.h's role of declaring these for every other module that
 * calls into the lock manager).
 * ================================================================ */
int  vms_lock_init(void);
void vms_lock_cleanup(void);
long vms_ioctl_enq(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_deq(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_convert(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getlki(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dlm_enum_waits(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dlm_member_depart(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_get_resmaster(struct vms_proc *proc, unsigned long arg);
uint32_t vms_lock_dlm_xnode_dispatch(struct vms_proc *proc,
                                     struct vms_dlm_xnode_args *req);

#endif /* OVMX_LOCK_HOST_INTERNAL_H */
