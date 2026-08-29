// SPDX-License-Identifier: GPL-2.0
/*
 * vms_lock.c - Lock Manager (Phase 3d)
 *
 * Full VMS lock manager with:
 *   - 6-mode compatibility (NL/CR/CW/PR/PW/EX)
 *   - Wait-for graph deadlock detection
 *   - Lock value blocks (16 bytes per resource)
 *   - Blocking ASTs (notification when your lock blocks others)
 *   - Hierarchical resource trees (parent/child locks)
 *   - Lock conversion
 *
 * Compatibility matrix (1=compatible, 0=incompatible):
 *
 *        NL  CR  CW  PR  PW  EX
 *   NL [  1   1   1   1   1   1 ]
 *   CR [  1   1   1   1   1   0 ]
 *   CW [  1   1   1   0   0   0 ]
 *   PR [  1   1   0   1   0   0 ]
 *   PW [  1   1   0   0   0   0 ]
 *   EX [  1   0   0   0   0   0 ]
 */

/*
 * SUBSTRATE-AGNOSTIC EXECUTIVE CORE (rd vms-84a, Phase G -- the 44 KB payoff and
 * the LAST executive facility promoted onto the shared core). This file lives in
 * src/kernel-core/ and names NO <linux/…> symbol: every host primitive it needs
 * goes through the kernel-backend shim. It is the ONLY facility that uses a
 * red-black tree (the lock-ID database) and a non-RCU intrusive hash (the
 * resource database), so it is where the exec_rbtree seam lands and where
 * exec_hash grows its non-RCU table vocabulary (design record
 * docs/design-netbsd-executive-core.md §3, §8 row G). The Linux vms.ko provides
 * the backend (exec_kbackend_linux.h / exec_list_linux.h / exec_hash_linux.h /
 * exec_rbtree_linux.h); the NetBSD `vms' module will provide its own when locks
 * join its SRCS (rd vms-ff7, the P4-A lock-backend proof).
 */
#include "vms_internal.h"     /* struct vms_lock_entry/resource/proc, the SS$/LCK$
                               * args/status codes, the C-string + fixed-width
                               * vocabulary, and vms_local_csid (extern) */
#include "exec_kbackend.h"    /* exec_lock/trylock/cv/copy/alloc */
#include "exec_list.h"        /* exec_list_* (granted/waiting/proc lists) */
#include "exec_hash.h"        /* exec_hash_* + exec_jhash (resource database) */
#include "exec_rbtree.h"      /* exec_rbtree_* / exec_rb_* (lock-ID database) */

/*
 * Deadlock re-scan interval for a lock blocked in-kernel (sync $ENQW).
 * A cycle is normally caught at enqueue time by the request that closes it;
 * this bounded timer is the safety net that catches a cycle enqueue-time
 * detection missed (check_deadlock uses trylock and may skip a branch),
 * i.e. a deadlock that only becomes observable AFTER the request queued.
 * Analogous to VMS SYSGEN DEADLOCK_WAIT, but far shorter for a local manager.
 */
#define VMS_DEADLOCK_WAIT_MS 500


/* Lock mode compatibility matrix */
static const uint8_t compat[6][6] = {
    /*          NL  CR  CW  PR  PW  EX */
    /* NL */ {  1,  1,  1,  1,  1,  1 },
    /* CR */ {  1,  1,  1,  1,  1,  0 },
    /* CW */ {  1,  1,  1,  0,  0,  0 },
    /* PR */ {  1,  1,  0,  1,  0,  0 },
    /* PW */ {  1,  1,  0,  0,  0,  0 },
    /* EX */ {  1,  0,  0,  0,  0,  0 },
};

/*
 * Global lock state. The former static initializers (RB_ROOT / DEFINE_SPINLOCK /
 * DEFINE_HASHTABLE) become the portable form: EXEC_DEFINE_HASHTABLE lays down the
 * bucket array (statically empty, like DEFINE_HASHTABLE), and the tree root and
 * the two locks are runtime-initialized in vms_lock_init() before any use --
 * exactly as eflag's file-scope lock and exec_list anchors do. On Linux this is
 * behaviour-identical to the static init (vms_lock_init runs at module load,
 * before any ioctl).
 */
exec_rbtree_root_t vms_lock_id_tree;
exec_lock_t vms_lock_id_lock;
uint32_t vms_next_lock_id = 1;

EXEC_DEFINE_HASHTABLE(vms_res_hash, VMS_RES_HASH_BITS);
exec_lock_t vms_res_hash_lock;

/* ================================================================
 * Cross-node REQUESTER-SIDE origin records (DLM epic vms-7fa rung H5, vms-6ca)
 * ================================================================
 *
 * When THIS node issues a cross-node $ENQ over SCS to a REMOTE master, the
 * request is not a local lock (the local lock manager never grants it -- the
 * remote master does). But the requester still needs a REAL, executive-resident
 * record of the outstanding request so its completion is genuine state, not a
 * per-process userspace fake (INV-6): the origin record's granted mode is set
 * ONLY from GRANT/queued-reply messages the master genuinely sent back over the
 * live SCS wire (VMS_DLM_OP_GRANT receive), never by a local grant decision.
 *
 * This is the requester-side mirror of the master's lock block. It lives on its
 * OWN list, keyed by the requester's local lock handle (req_lkid), so it never
 * touches the resource granted/waiting queues the local lock manager scans --
 * the executive cannot auto-grant an origin record, only the wire can complete
 * it. GETLKI falls through to it so the NL->EX status flip driven by the remote
 * master's deferred GRANT is observable on the REQUESTER node.
 */
struct vms_dlm_origin {
    exec_list_node_t list;
    uint32_t         req_lkid;       /* our own lock handle for the request     */
    uint32_t         req_csid;       /* our node's CSID (the requester)          */
    uint32_t         master_lkid;    /* the master's lock handle (0 until known) */
    uint32_t         master_csid;    /* the mastering node's CSID                */
    uint32_t         granted_mode;   /* NL while pending/queued; the granted mode
                                      * once the master's GRANT arrives          */
    uint32_t         requested_mode; /* the mode we asked for                    */
    char             resnam[32];
    /*
     * BLKAST WIRE (rung H6, vms-76d). When this origin record proxies a HOLDER on
     * this node (a GRANT receive established it AT a granted mode WITH a blocking-AST
     * routine), blkastadr/blkastprm remember that routine so a BLKAST the master
     * later sends over SCS fires a REAL user-mode AST on the holder's process.
     * blkast_count counts genuine deliveries (0 until the first BLKAST fires).
     * 0 blkastadr => no blocking AST was registered, so a BLKAST receive is a
     * no-op that declines SS$_UNSUPPORTED rather than fabricating a delivery.
     */
    uint64_t         blkastadr;
    uint64_t         blkastprm;
    uint32_t         blkast_count;
    /*
     * LVB read-crossing (rd vms-eeb, rung H9). When the master's GRANT reply
     * carries its resource value block (the master read res->valblk on the
     * cross-node grant), grant_recv stores those 16 bytes here so GETLKI on this
     * origin record surfaces them to the requester's $ENQ -- the mirror of the
     * write crossing (vms-d81). valblk_valid is set only when a real GRANT
     * delivered a block, so GETLKI reports an unset LVB honestly (zeros) rather
     * than fabricating one. */
    uint8_t          valblk[LCK_VALBLK_SIZE];
    uint32_t         valblk_valid;
};

exec_list_head_t vms_dlm_origin_list;
exec_lock_t vms_dlm_origin_lock;

static int vms_lock_dlm_origin_getlki(uint32_t lkid, uint32_t *granted_mode,
                                      uint32_t *requested_mode, char *resnam,
                                      size_t resnam_len, uint8_t *valblk);

int vms_lock_init(void)
{
    exec_lock_init(&vms_lock_id_lock);
    exec_lock_init(&vms_res_hash_lock);
    exec_lock_init(&vms_dlm_origin_lock);
    exec_rbtree_init(&vms_lock_id_tree);
    exec_hash_init(vms_res_hash);
    exec_list_head_init(&vms_dlm_origin_list);
    return 0;
}

/*
 * lock_entry_free / resource_free - tear down and free one lock entry / resource.
 *
 * A lock entry owns a condition variable (wait_wq, init'd by exec_cv_init in the
 * $ENQ path) and a resource owns a mutex (res->lock, init'd by exec_lock_init in
 * resource_find_or_create). These MUST be paired with exec_cv_destroy /
 * exec_lock_destroy before the object is freed. On Linux exec_cv_destroy /
 * exec_lock_destroy are no-ops (a wait_queue/spinlock owns no external resource),
 * so routing every free through these helpers is behaviour-identical to the bare
 * exec_free(); on NetBSD they resolve to cv_destroy/mutex_destroy, without which
 * the kcondvar/kmutex leaks kernel resources on every free path. Same destroy-
 * then-free pattern as eflag's vms_common_ef_free (vms_eflag.c). Every free path
 * below (creation-failure rollback, DEQ via lock_put, proc/image rundown, module
 * cleanup) MUST go through these.
 */
static void lock_entry_free(struct vms_lock_entry *lock)
{
    exec_cv_destroy(&lock->wait_wq);
    exec_free(lock);
}

static void resource_free(struct vms_lock_resource *res)
{
    exec_lock_destroy(&res->lock);
    exec_free(res);
}

void vms_lock_cleanup(void)
{
    struct vms_lock_resource *res;
    struct vms_lock_entry *lock, *ltmp;
    exec_hash_node_t *tmp;
    int bkt;

    /* Free all resources and their lock entries */
    exec_lock(&vms_res_hash_lock);
    exec_hash_for_each_safe(vms_res_hash, bkt, tmp, res, hash_node) {
        /* Free lock entries on granted list */
        exec_list_for_each_entry_safe(lock, ltmp, &res->granted, res_granted) {
            exec_list_del(&lock->res_granted);
            lock_entry_free(lock);
        }
        /* Free lock entries on waiting list */
        exec_list_for_each_entry_safe(lock, ltmp, &res->waiting, res_waiting) {
            exec_list_del(&lock->res_waiting);
            lock_entry_free(lock);
        }
        exec_hash_del(&res->hash_node);
        resource_free(res);
    }
    exec_unlock(&vms_res_hash_lock);

    /* Free any cross-node requester-side origin records (vms-6ca, H5). */
    {
        struct vms_dlm_origin *org, *otmp;
        exec_lock(&vms_dlm_origin_lock);
        exec_list_for_each_entry_safe(org, otmp, &vms_dlm_origin_list, list) {
            exec_list_del(&org->list);
            exec_free(org);
        }
        exec_unlock(&vms_dlm_origin_lock);
    }

    /* Tear down the runtime-initialized locks (a no-op on Linux; a real
     * mutex_destroy on NetBSD -- paired with the exec_lock_init in
     * vms_lock_init). */
    exec_lock_destroy(&vms_dlm_origin_lock);
    exec_lock_destroy(&vms_res_hash_lock);
    exec_lock_destroy(&vms_lock_id_lock);
}

/* ================================================================
 * Lock ID management (red-black tree)
 * ================================================================ */

static struct vms_lock_entry *lock_find_by_id(uint32_t lkid)
{
    exec_rbtree_node_t *node;

    exec_lock(&vms_lock_id_lock);
    node = exec_rbtree_first(&vms_lock_id_tree);
    while (node) {
        struct vms_lock_entry *entry = exec_rb_entry(node, struct vms_lock_entry, rb_node);
        if (lkid < entry->lkid)
            node = exec_rb_left(node);
        else if (lkid > entry->lkid)
            node = exec_rb_right(node);
        else {
            entry->refcount++;
            exec_unlock(&vms_lock_id_lock);
            return entry;
        }
    }
    exec_unlock(&vms_lock_id_lock);
    return NULL;
}

static void lock_put(struct vms_lock_entry *entry)
{
    exec_lock(&vms_lock_id_lock);
    entry->refcount--;
    if (entry->refcount <= 0) {
        /* Entry was removed from tree and last reference dropped */
        exec_unlock(&vms_lock_id_lock);
        lock_entry_free(entry);
        return;
    }
    exec_unlock(&vms_lock_id_lock);
}

static void lock_insert_id(struct vms_lock_entry *entry)
{
    exec_rbtree_node_t **p, *parent = NULL;

    exec_lock(&vms_lock_id_lock);
    entry->lkid = vms_next_lock_id++;

    p = exec_rbtree_root_link(&vms_lock_id_tree);
    while (*p) {
        struct vms_lock_entry *e = exec_rb_entry(*p, struct vms_lock_entry, rb_node);
        parent = *p;
        if (entry->lkid < e->lkid)
            p = exec_rb_left_link(*p);
        else
            p = exec_rb_right_link(*p);
    }
    exec_rb_link_node(&entry->rb_node, parent, p);
    exec_rb_insert_color(&entry->rb_node, &vms_lock_id_tree);
    exec_unlock(&vms_lock_id_lock);
}

static void lock_remove_id(struct vms_lock_entry *entry)
{
    exec_lock(&vms_lock_id_lock);
    exec_rb_erase(&entry->rb_node, &vms_lock_id_tree);
    exec_unlock(&vms_lock_id_lock);
}

/* ================================================================
 * Resource management (hash table by name)
 * ================================================================ */

static uint32_t resource_hash_key(const char *name)
{
    return exec_jhash(name, strnlen(name, 32), 0);
}

static struct vms_lock_resource *resource_find(const char *name)
{
    struct vms_lock_resource *res;
    uint32_t key = resource_hash_key(name);

    exec_hash_for_each_possible(vms_res_hash, res, hash_node, key) {
        if (strncmp(res->name, name, 32) == 0)
            return res;
    }
    return NULL;
}

static struct vms_lock_resource *resource_find_or_create(const char *name)
{
    struct vms_lock_resource *res, *new_res;
    uint32_t key;

    exec_lock(&vms_res_hash_lock);
    res = resource_find(name);
    if (res) {
        res->refcount++;
        exec_unlock(&vms_res_hash_lock);
        return res;
    }
    exec_unlock(&vms_res_hash_lock);

    /* Allocate outside spinlock so we can use a may-sleep allocation */
    new_res = exec_zalloc(sizeof(struct vms_lock_resource));
    if (!new_res)
        return NULL;

    /* Re-check under lock — another thread may have created it */
    exec_lock(&vms_res_hash_lock);
    res = resource_find(name);
    if (res) {
        res->refcount++;
        exec_unlock(&vms_res_hash_lock);
        /* Bare exec_free (NOT resource_free): new_res->lock is not init'd until
         * below, so there is no exec_lock_destroy to pair on this race path. */
        exec_free(new_res);
        return res;
    }

    strscpy(new_res->name, name, sizeof(new_res->name));
    exec_list_head_init(&new_res->granted);
    exec_list_head_init(&new_res->waiting);
    memset(new_res->valblk, 0, LCK_VALBLK_SIZE);
    exec_lock_init(&new_res->lock);
    new_res->refcount = 1;
    new_res->parent = NULL;

    key = resource_hash_key(name);
    exec_hash_add(vms_res_hash, &new_res->hash_node, key);
    exec_unlock(&vms_res_hash_lock);

    return new_res;
}

static void resource_release(struct vms_lock_resource *res)
{
    int i, has_valblk = 0;

    exec_lock(&vms_res_hash_lock);
    res->refcount--;
    if (res->refcount <= 0 && exec_list_empty(&res->granted) && exec_list_empty(&res->waiting)) {
        /* Preserve resource if it has a non-zero value block */
        for (i = 0; i < LCK_VALBLK_SIZE; i++) {
            if (res->valblk[i]) { has_valblk = 1; break; }
        }
        if (!has_valblk) {
            exec_hash_del(&res->hash_node);
            resource_free(res);
        }
    }
    exec_unlock(&vms_res_hash_lock);
}

/* ================================================================
 * DLM resource directory + mastering (vms-ci.5 DB) -- LOCAL scaffolding
 *
 * On OpenVMS the lock database is distributed: every resource is MASTERED on
 * one node, and to find the master a node hashes the resource name to a
 * DIRECTORY node, which holds the name->master mapping. An enqueue routes to
 * the master; a directory/master on another node is reached over SCS. VMS's
 * directory lookup is the documented 3-case algorithm (IDSM lock-management
 * chapter, "directory lookups" -- mined transcript ch6-part02, pp. 6-18..6-35;
 * summarized in docs/design-cluster-node.md §5):
 *
 *   (1) this node is the directory AND masters the resource -> grant locally;
 *   (2) this node is the directory, another node masters it -> route to master;
 *   (3) this node is not the directory -> inquire of the directory node, which
 *       returns the master (or assigns the requester as master on first use).
 *
 * This is the LOCAL scaffolding for that model. Membership is a stub-of-one
 * (a "cluster of one"), so the directory vector has a single entry -- this
 * node -- and every name resolves to the local CSID: case (1) always, self is
 * both directory and master, and the grant runs through the existing single-
 * node lock manager below unchanged. The multi-node membership view and the
 * remote paths (case (2)/(3) forwarding over the VMS$VAXcluster VC, and
 * dynamic remastering on state transitions) are 0.4 (vms-ci.5 DC/DD): the
 * enqueue path returns SS$_UNSUPPORTED for a non-local directory or master
 * rather than fabricating a remote answer (INV-6 spirit).
 * ================================================================ */

/*
 * This node's CSID (vms_local_csid). 0 is reserved for "unmastered"
 * (struct vms_lock_resource.master_csid), so the default is a non-zero OVMX
 * local placeholder. The VARIABLE and its insmod module parameter live in the
 * Linux module glue (src/kernel/vms_module.c) -- module parameters are a
 * host-module-lifecycle concern, not portable executive logic, so they stay in
 * the per-substrate rind (design record §4). This core facility reads it through
 * the extern declaration in vms_internal.h; the connection manager (0.4) or a
 * test sets the real CSID at insmod time. It is NOT a claim of a VMS-authentic
 * CSID value or layout (CLAUDE.md Rule 8) -- real CSIDs are assigned by the CM
 * at join.
 */

/*
 * Number of directory-participating nodes in the membership view. Read from the
 * CONTROLLED, STATIC membership vector supplied at load time (dlm_member_csids /
 * dlm_member_count, vms-1bba "DB" rung -- an insmod module_param_array on Linux,
 * a load-time symbol on NetBSD; see vms_internal.h). Empty (count 0, the
 * default) falls back to a cluster-of-one, so an unconfigured node behaves
 * exactly as the prior stub-of-one did -- single-node grants unchanged.
 *
 * This is NOT the live membership feed from the connection manager / quorum
 * (src/vmsscs/scs_quorum.c): that dynamic feed is the 0.4 "DC" successor (and
 * overlaps vms-2f3's rejoin). A static, harness/operator-supplied vector is an
 * honest controlled input for the directory proof, never fabricated live state.
 */
/*
 * Runtime departure set (rd vms-2bf, DLM rung H10a). The configured membership
 * vector (dlm_member_csids) is a STATIC insmod param a graceful departure cannot
 * mutate; instead the executive marks a member departed HERE, via
 * VMS_IOCTL_DLM_MEMBER_DEPART, and the LIVE membership the directory hashes over
 * is the configured set MINUS the departed. dlm_member_departed[i] == 1 means
 * the i-th configured member left the cluster. Departure is MONOTONIC in this
 * rung (a member departs and does not rejoin -- rejoin is 0.4/vms-2f3), so the
 * flag only ever goes 0 -> 1; readers need no lock (a concurrent enqueue sees
 * either the pre- or post-departure membership, both consistent, self-healing on
 * the next call). The ioctl mutates it under vms_res_hash_lock together with the
 * directory-cache invalidation it drives. INV-6: set only from a REAL departure
 * the connection manager (scsd) reported, never fabricated.
 */
static uint8_t dlm_member_departed[VMS_DLM_MAX_MEMBERS];

static unsigned int dlm_membership_count(void)
{
    unsigned int i, n = 0;

    if (dlm_member_count <= 0)
        return 1u;   /* cluster-of-one fallback (unconfigured) */
    for (i = 0; i < (unsigned int)dlm_member_count && i < VMS_DLM_MAX_MEMBERS; i++)
        if (!dlm_member_departed[i])
            n++;
    return n > 0 ? n : 1u;
}

/*
 * The CSID of the idx-th LIVE member of the directory vector (departed members
 * skipped). Reads the configured static vector minus the runtime departure set;
 * falls back to the local CSID when unconfigured or out of range (cluster-of-
 * one). Every node given the SAME ordered vector AND the same departures maps
 * idx -> CSID identically, which is what keeps directory/master resolution in
 * agreement across nodes as membership shrinks.
 */
static uint32_t dlm_member_csid(unsigned int idx)
{
    unsigned int i, live = 0;

    if (dlm_member_count > 0) {
        for (i = 0; i < (unsigned int)dlm_member_count && i < VMS_DLM_MAX_MEMBERS; i++) {
            if (dlm_member_departed[i])
                continue;
            if (live == idx)
                return dlm_member_csids[i];
            live++;
        }
    }
    return vms_local_csid;
}

/*
 * dlm_directory_csid - which node is the DIRECTORY for a resource name.
 *
 * The documented algorithm hashes the resource name and selects, modulo the
 * number of directory-participating nodes, an entry of the cluster directory
 * vector that names the directory node (IDSM lock-management, "directory
 * lookups", ch6-part02 pp. 6-18..6-35). The STRUCTURE -- hash(name) -> index
 * -> directory node -- is that algorithm; the SPECIFIC hash (exec_jhash, the
 * same one resource_hash_key uses) is an OVMX design choice, because public docs
 * do not publish VMS's directory hash function (CLAUDE.md Rule 8).
 *
 * Stub-of-one membership makes this resolve to the local CSID for every name;
 * the hash + modulo + vector indexing are nonetheless real so that growing the
 * membership in 0.4 is a change to dlm_membership_count()/dlm_member_csid(),
 * not to the lookup path.
 */
static uint32_t dlm_directory_csid(const char *name)
{
    unsigned int n = dlm_membership_count();
    unsigned int idx = n ? (exec_jhash(name, strnlen(name, 32), 0) % n) : 0;

    return dlm_member_csid(idx);
}

/*
 * dlm_resolve_master - resolve the directory + master for a resource, mastering
 * it locally on first use.
 *
 * Called with res->lock held. Resolves res->dir_csid (once, lazily) via the
 * directory hash; if this node is the directory and the resource is not yet
 * mastered, masters it here (mastered on first use, case (1)). Returns
 * SS$_NORMAL when the resource is directoried AND mastered locally -- the only
 * case the single-node lock manager can serve. Returns SS$_UNSUPPORTED when
 * the directory or the master is a REMOTE node: forwarding that request over
 * the VMS$VAXcluster VC (DC) and remastering (DD) are 0.4. Never fabricates a
 * remote grant.
 */
static uint32_t dlm_resolve_master(struct vms_lock_resource *res)
{
    if (res->dir_csid == 0)
        res->dir_csid = dlm_directory_csid(res->name);

    if (res->dir_csid != vms_local_csid)
        return SS__UNSUPPORTED;      /* case (3): remote directory -- 0.4 */

    /* We are the directory. Master on first use (case (1)). */
    if (res->master_csid == 0)
        res->master_csid = vms_local_csid;

    if (res->master_csid != vms_local_csid)
        return SS__UNSUPPORTED;      /* case (2): remote master -- 0.4 */

    return SS__NORMAL;
}

/* ================================================================
 * Lock compatibility checking
 * ================================================================ */

/*
 * Check if a lock at mode 'requested' is compatible with all
 * currently granted locks on the resource (excluding 'exclude').
 */
static int lock_compatible(struct vms_lock_resource *res,
                           uint32_t requested,
                           struct vms_lock_entry *exclude)
{
    struct vms_lock_entry *granted;

    exec_list_for_each_entry(granted, &res->granted, res_granted) {
        if (granted == exclude)
            continue;
        if (!compat[requested][granted->granted_mode])
            return 0;
    }
    return 1;
}

/* ================================================================
 * Deadlock detection (wait-for graph iterative BFS)
 * ================================================================ */

#define MAX_DEADLOCK_DEPTH 16

/*
 * Check if granting 'lock' would create a deadlock.
 *
 * Walk the wait-for graph iteratively using a fixed-size stack to
 * avoid recursive spinlock acquisition (the old recursive version
 * would exec_lock(&proc->lock_list_lock) while already holding
 * another proc's lock_list_lock, risking ABBA deadlock).
 *
 * Strategy: use exec_trylock on proc->lock_list_lock. If we can't get it,
 * conservatively assume potential deadlock at that branch (safe
 * because false positives just cause SS$_DEADLOCK, which the caller
 * retries or reports).
 */
static int check_deadlock(struct vms_lock_entry *lock,
                           int depth __attribute__((unused)))
{
    struct vms_proc *origin_proc = lock->proc;
    struct vms_lock_entry *stack[MAX_DEADLOCK_DEPTH];
    int sp = 0;

    stack[sp++] = lock;

    while (sp > 0) {
        struct vms_lock_entry *cur = stack[--sp];
        struct vms_lock_resource *res = cur->resource;
        struct vms_lock_entry *granted;

        /*
         * For each granted lock on cur's resource that blocks cur,
         * check if the blocking process is also waiting somewhere.
         * Note: res->lock is already held by the caller (vms_ioctl_enq
         * or vms_ioctl_convert) for the initial resource. For other
         * resources in the chain, we only read the granted list under
         * trylock to avoid lock-order inversions.
         */
        exec_list_for_each_entry(granted, &res->granted, res_granted) {
            if (compat[cur->requested_mode][granted->granted_mode])
                continue;  /* This one doesn't block us */

            /* Direct cycle: blocker is the original requester */
            if (granted->proc == origin_proc)
                return 1;  /* Deadlock! */

            /* Check if the blocking process has any waiting locks */
            if (!exec_trylock(&granted->proc->lock_list_lock))
                continue;  /* Can't get lock — skip this branch */

            {
                struct vms_lock_entry *their_lock;
                exec_list_for_each_entry(their_lock, &granted->proc->locks, proc_list) {
                    if (their_lock->waiting && sp < MAX_DEADLOCK_DEPTH) {
                        if (their_lock->proc == origin_proc) {
                            exec_unlock(&granted->proc->lock_list_lock);
                            return 1;  /* Deadlock! */
                        }
                        stack[sp++] = their_lock;
                    }
                }
            }
            exec_unlock(&granted->proc->lock_list_lock);
        }
    }

    return 0;
}

/* ================================================================
 * Grant waiting locks after a dequeue or conversion
 * ================================================================ */

/*
 * Queue a completion AST for a waiter that was just granted. This is the
 * async ($ENQ, no LCK_M_SYNC) path: the caller returned immediately with the
 * request queued, and asked to be notified via astadr when the lock is
 * finally granted. Delivered through the process's user-mode AST queue, which
 * userspace drains via VMS_IOCTL_DELIVERAST.
 *
 * Skipped for a sync waiter (LCK_M_SYNC): that caller is blocked in-kernel in
 * enq_wait_sync() and is woken directly, so there is nobody to drain an AST.
 * Skipped when no astadr was supplied.
 *
 * Called with res->lock held (from try_grant_waiters); allocates non-sleeping
 * and nests ast_state->lock inside res->lock (same order as
 * notify_blocking_asts, so no new lock-order inversion).
 */
static void queue_completion_ast(struct vms_lock_entry *lock)
{
    struct vms_ast_entry *ast;
    struct vms_ast_state *ast_state;

    if (!lock->astadr || (lock->flags & LCK_M_SYNC))
        return;

    ast = exec_zalloc_atomic(sizeof(*ast));
    if (!ast)
        return;

    ast->astadr = lock->astadr;
    ast->astprm = lock->astprm;
    ast->acmode = PSL_C_USER;

    ast_state = &lock->proc->ast[PSL_C_USER];
    exec_lock(&ast_state->lock);
    if (ast_state->count < VMS_AST_MAX_PER_MODE) {
        exec_list_add_tail(&ast->list, &ast_state->pending);
        ast_state->count++;
        exec_unlock(&ast_state->lock);
        /* Async delivery (vms-feb): wake the lock's process if it is
         * hibernating so its $HIBER drains and runs this completion AST. After
         * ast_state->lock is dropped (hiber_lock must nest OUTSIDE it); res->lock
         * is still held, hiber_lock under it is a fresh edge. */
        vms_ast_notify_arrival(lock->proc);
    } else {
        exec_free(ast);
        exec_unlock(&ast_state->lock);
    }
}

static void try_grant_waiters(struct vms_lock_resource *res)
{
    struct vms_lock_entry *waiter, *tmp;

    exec_list_for_each_entry_safe(waiter, tmp, &res->waiting, res_waiting) {
        if (lock_compatible(res, waiter->requested_mode, NULL)) {
            /* Grant it */
            exec_list_del(&waiter->res_waiting);
            waiter->granted_mode = waiter->requested_mode;
            waiter->waiting = 0;
            exec_list_add_tail(&waiter->res_granted, &res->granted);

            /* Copy resource value block if requested */
            if (waiter->flags & LCK_M_VALBLK)
                memcpy(waiter->valblk, res->valblk, LCK_VALBLK_SIZE);

            /*
             * Signal completion. Async waiters get a completion AST;
             * sync ($ENQW) waiters blocked in enq_wait_sync() are woken.
             * Both are cheap and safe under res->lock.
             *
             * The grant_state write and the wake both happen under res->lock --
             * the SAME lock the sync waiter holds across exec_cv_wait_timeout --
             * so the cv-idiom in enq_wait_sync is lost-wakeup-free (a waiter is
             * already enqueued on wait_wq before it drops res->lock, and this
             * waker cannot set grant_state or wake until it holds res->lock).
             */
            queue_completion_ast(waiter);
            waiter->grant_state = SS__NORMAL;
            exec_cv_broadcast(&waiter->wait_wq);
        } else {
            /* FIFO: stop at first non-grantable waiter
             * (VMS actually checks all waiters, but FIFO is simpler
             *  and prevents starvation) */
            break;
        }
    }
}

/*
 * Send blocking AST notifications to granted lock holders when
 * a new request is blocked.
 */
static void notify_blocking_asts(struct vms_lock_resource *res,
                                 struct vms_lock_entry *blocked)
{
    struct vms_lock_entry *granted;

    exec_list_for_each_entry(granted, &res->granted, res_granted) {
        if (!compat[blocked->requested_mode][granted->granted_mode] &&
            granted->blkastadr) {
            /*
             * Queue a blocking AST for the granted lock's process.
             * The blocking AST tells the process "someone is waiting
             * for your resource -- consider releasing or downgrading."
             */
            struct vms_ast_entry *ast;
            struct vms_ast_state *ast_state;

            ast = exec_zalloc_atomic(sizeof(*ast));
            if (!ast)
                continue;

            ast->astadr = granted->blkastadr;
            ast->astprm = granted->lkid;
            ast->acmode = PSL_C_USER;

            ast_state = &granted->proc->ast[PSL_C_USER];
            exec_lock(&ast_state->lock);
            if (ast_state->count < VMS_AST_MAX_PER_MODE) {
                exec_list_add_tail(&ast->list, &ast_state->pending);
                ast_state->count++;
                exec_unlock(&ast_state->lock);
                /* Async delivery (vms-feb): wake the granted holder if it is
                 * hibernating so its $HIBER drains and runs this blocking AST. */
                vms_ast_notify_arrival(granted->proc);
            } else {
                exec_free(ast);
                exec_unlock(&ast_state->lock);
            }
        }
    }
}

/* ================================================================
 * Process lock cleanup
 * ================================================================ */

/*
 * Tear down one lock and free it. Caller holds proc->lock_list_lock and has
 * already unlinked (or is about to unlink) the entry from proc->locks via the
 * exec_list_for_each_entry_safe cursor. Shared verbatim by full process teardown
 * (vms_proc_release_locks) and image rundown (vms_proc_rundown_locks) so the
 * two cannot drift.
 */
static void lock_teardown_locked(struct vms_lock_entry *lock)
{
    struct vms_lock_resource *res = lock->resource;

    /* Remove from resource lists */
    exec_lock(&res->lock);
    if (lock->waiting)
        exec_list_del(&lock->res_waiting);
    else
        exec_list_del(&lock->res_granted);

    /* Write back value block if held */
    if ((lock->flags & LCK_M_VALBLK) && !lock->waiting)
        memcpy(res->valblk, lock->valblk, LCK_VALBLK_SIZE);

    try_grant_waiters(res);
    exec_unlock(&res->lock);

    /* Remove from process list and ID tree */
    exec_list_del(&lock->proc_list);
    lock_remove_id(lock);

    resource_release(res);
    lock_entry_free(lock);
}

void vms_proc_release_locks(struct vms_proc *proc)
{
    struct vms_lock_entry *lock, *tmp;

    exec_lock(&proc->lock_list_lock);
    exec_list_for_each_entry_safe(lock, tmp, &proc->locks, proc_list)
        lock_teardown_locked(lock);
    proc->lock_count = 0;
    exec_unlock(&proc->lock_list_lock);
}

/*
 * vms_proc_rundown_locks - image rundown's lock release (vms-68f.v).
 *
 * Dequeue only the locks this process enqueued at access mode >= min_acmode
 * (image rundown passes PSL_C_USER, so only USER-mode locks an activated image
 * held), leaving inner-mode (process-permanent) locks granted. A skipped lock
 * keeps its lkid valid; a released one's lkid becomes SS$_IVLOCKID on the next
 * $DEQ, which is exactly the property test_kmod_rundown.c asserts. Grounding:
 * docs/design-image-rundown-resource-classes.md ($DEQ acmode semantics).
 */
void vms_proc_rundown_locks(struct vms_proc *proc, uint8_t min_acmode)
{
    struct vms_lock_entry *lock, *tmp;

    exec_lock(&proc->lock_list_lock);
    exec_list_for_each_entry_safe(lock, tmp, &proc->locks, proc_list) {
        if (lock->acmode < min_acmode)
            continue;   /* inner-mode: process-permanent, survives rundown */
        lock_teardown_locked(lock);
        if (proc->lock_count > 0)
            proc->lock_count--;
    }
    exec_unlock(&proc->lock_list_lock);
}

/* ================================================================
 * Synchronous ($ENQW) in-kernel blocking
 * ================================================================ */

/*
 * enq_wait_sync - Block the caller until its queued lock is granted, or a
 * deadlock is detected.
 *
 * Preconditions: `lock` is on res->waiting (lock->waiting == 1), res->lock is
 * NOT held, and lock->grant_state was reset to 0 before it was queued. The
 * lock entry stays alive for the duration of the wait: it is owned by the
 * calling process (only that process can DEQ it) and, for CONVERT, an extra
 * find-reference is held by the caller.
 *
 * Returns:
 *   SS__NORMAL   - granted. try_grant_waiters moved the lock to res->granted,
 *                  set granted_mode, and set grant_state. Entry left intact.
 *   SS__DEADLOCK - a cycle was detected on a bounded re-scan. The entry has
 *                  been removed from res->waiting and lock->waiting cleared;
 *                  the caller decides how to unwind (free for a fresh ENQ,
 *                  restore granted mode for a CONVERT).
 *
 * WAIT MODEL. This is the executive's synchronous wait, expressed in the shim's
 * cv-idiom (design record §3, the wait/wake seam): the waiter holds res->lock --
 * the SAME lock that guards the predicate (lock->grant_state) and that every
 * waker (try_grant_waiters) holds while it sets grant_state and wakes -- and
 * loops re-testing the predicate. exec_cv_wait_timeout atomically drops res->lock,
 * sleeps up to VMS_DEADLOCK_WAIT_MS, and re-acquires res->lock before returning,
 * so the predicate is always re-tested under the lock and no wakeup is lost.
 * The wait is interruptible so a signal (e.g. an AST-delivery RT signal) can
 * wake us; the interrupt is ignored (we simply re-arm) because a VMS wait ends
 * only when its condition is satisfied. Only a genuine timeout (timed_out)
 * triggers a deadlock re-scan for this still-waiting request. On Linux this is
 * behaviour-identical to the former lock-free
 * wait_event_interruptible_timeout(wq, grant_state != 0, to) loop: the only
 * waker sets grant_state before waking, so the held-lock predicate test and the
 * lock-free READ_ONCE test observe the same transitions.
 */
static int enq_wait_sync(struct vms_lock_resource *res,
                         struct vms_lock_entry *lock)
{
    int status;
    int timed_out;

    exec_lock(&res->lock);
    for (;;) {
        /* Predicate has priority (cv contract). */
        if (lock->grant_state == SS__NORMAL) {
            status = SS__NORMAL;
            break;
        }
        if (lock->grant_state == SS__DEADLOCK) {
            status = SS__DEADLOCK;
            break;
        }
        /* Granted between a wakeup and re-acquiring res->lock: the entry left
         * res->waiting but grant_state was not the value tested above. */
        if (!lock->waiting) {
            lock->grant_state = SS__NORMAL;
            status = SS__NORMAL;
            break;
        }

        /* Sleep until woken (grant/signal) or the timeout elapses. res->lock is
         * dropped across the sleep and held again on return. */
        exec_cv_wait_timeout(&lock->wait_wq, &res->lock,
                             VMS_DEADLOCK_WAIT_MS, &timed_out);

        /*
         * Woke without a grant recorded. On a genuine timeout re-run deadlock
         * detection for this still-waiting request; otherwise (signal wake or a
         * grant that raced in) fall through and re-test the predicate at the top.
         */
        if (timed_out && lock->waiting &&
            lock->grant_state == 0 && check_deadlock(lock, 0)) {
            exec_list_del(&lock->res_waiting);
            lock->waiting = 0;
            lock->grant_state = SS__DEADLOCK;
            status = SS__DEADLOCK;
            break;
        }
        /* Signal wake or benign timeout: keep waiting. */
    }
    exec_unlock(&res->lock);
    return status;
}

/* ================================================================
 * ioctl handlers
 * ================================================================ */

/*
 * vms_ioctl_enq - Enqueue lock request ($ENQ equivalent)
 *
 * Requests a lock on a named resource. If compatible with all
 * granted locks, the lock is granted immediately. Otherwise,
 * if LCK_M_NOQUEUE is set, fails with SS$_NOTQUEUED. Otherwise,
 * the request is queued (caller blocks in kernel until granted).
 */
/*
 * vms_enq_core - the $ENQ logic, operating on an in-memory args struct (no
 * copyin/copyout). Reached two ways: (a) the vms_ioctl_enq wrapper marshals a
 * userspace $ENQ; (b) an in-kernel executive facility running in the caller's
 * ioctl context enqueues DIRECTLY (no /dev/vms round-trip) -- the Files-11 ACP's
 * per-volume synchronization lock (vms-233, vms_lock_acp_vol_ex below). The body
 * is unchanged from the original ioctl handler; only the copy in/out moved to
 * the callers, so every `goto out` writes the result back through *io.
 */
/*
 * Cross-node ($ENQ-over-SCS) enqueue outputs (DLM epic vms-7fa rung 3, vms-904c).
 * When the caller passes a non-NULL pointer, vms_enq_core_ex is in CROSS-NODE mode:
 *   - LOCAL wait-for-graph deadlock detection is SKIPPED. Two locks held for two
 *     different cluster identities are two different owners even though they share
 *     the local delivery proc, so the single-node detector (which keys on `proc`)
 *     would false-positive; cross-node/distributed deadlock detection is a later
 *     rung (vms-ec75) and is honestly out of scope here, not faked.
 *   - a QUEUED (blocked) request reports queued=1 and, if a cross-node holder
 *     blocks it, the identity that must receive a blocking AST (blocking_csid) and
 *     that holder's master lock handle (blocking_master_lkid).
 */
struct dlm_xnode_enq_out {
    uint32_t req_lkid;              /* IN: the remote requester's own lock handle,
                                    * stored on the master lock so a deferred
                                    * GRANT can name the original request (H5). */
    int      queued;
    uint32_t blocking_csid;
    uint32_t blocking_master_lkid;
    uint32_t blocking_req_lkid;    /* OUT: the blocking holder's REQUESTER-side lock
                                    * handle -- the value a BLKAST names so the
                                    * holder node finds its ORIGIN record (H6). */
};

static long vms_enq_core_ex(struct vms_proc *proc, struct vms_enq_args *io,
                            struct dlm_xnode_enq_out *xn)
{
    struct vms_enq_args args = *io;
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;

    if (args.lkmode > LCK_K_EXMODE) {
        args.status = SS__BADPARAM;
        goto out;
    }

    /* Ensure null-terminated resource name */
    args.resnam[31] = '\0';
    if (args.resnam[0] == '\0') {
        args.status = SS__BADPARAM;
        goto out;
    }

    /* Find or create the resource */
    res = resource_find_or_create(args.resnam);
    if (!res) {
        args.status = SS__INSFMEM;
        goto out;
    }

    /*
     * DLM directory lookup + master resolution (vms-ci.5 DB) BEFORE anything
     * is granted. For the local scaffolding this masters the resource on this
     * node -- self is both the directory (name hashes to the local CSID) and
     * the master -- and falls through to the single-node lock manager below
     * unchanged. A resource whose directory or master resolves to a REMOTE
     * node would have to be forwarded over the VMS$VAXcluster VC (DC) or driven
     * through remastering (DD), which are 0.4: fail honestly with
     * SS$_UNSUPPORTED rather than fabricate a remote grant, releasing the
     * resource block we may have just created for it. In a cluster of one this
     * branch is unreachable (the directory hash always yields the local CSID),
     * which is exactly why it is an honest deferral and not fake remote state.
     */
    {
        uint32_t dlm_st;

        exec_lock(&res->lock);
        dlm_st = dlm_resolve_master(res);
        exec_unlock(&res->lock);

        if (dlm_st != SS__NORMAL) {
            resource_release(res);
            args.status = dlm_st;
            goto out;
        }
    }

    /* Allocate lock entry */
    lock = exec_zalloc(sizeof(struct vms_lock_entry));
    if (!lock) {
        resource_release(res);
        args.status = SS__INSFMEM;
        goto out;
    }

    lock->refcount = 1;
    lock->granted_mode = LCK_K_NLMODE;
    lock->requested_mode = args.lkmode;
    lock->flags = args.flags;
    lock->astadr = args.astadr;
    lock->astprm = args.astprm;
    lock->blkastadr = args.blkastadr;
    lock->resource = res;
    lock->proc = proc;
    lock->waiting = 0;
    exec_cv_init(&lock->wait_wq);
    lock->grant_state = 0;
    /*
     * The cluster identity this lock is held FOR. 0 for an ordinary $ENQ (a
     * local process on this node owns the lock -- args.owner_csid is 0, the
     * memset default every userspace caller leaves in place). Non-zero only on
     * the cross-node DLM grant path (vms_lock_dlm_xnode_dispatch, vms-e8f1),
     * which sets owner_csid to the REMOTE requester's CSID so the master's lock
     * record carries the identity it is held on behalf of. Read back through
     * GET_RESMASTER.remote_holder_csid -- a genuine held-lock proof, not a
     * fabricated status (INV-6).
     */
    lock->req_csid = args.owner_csid;
    /*
     * The remote requester's OWN lock handle (vms-6ca, H5). Non-zero only on the
     * cross-node path (xn set), so that when this granted lock is later released
     * and its release grants a queued cross-node waiter, the master can name the
     * waiter's original request (req_lkid) in the deferred GRANT it wires back.
     */
    lock->req_lkid = xn ? xn->req_lkid : 0;

    /*
     * Record the access mode $ENQ was issued from, so image rundown can tell
     * an image-scoped (USER) lock from a process-permanent one (vms-68f.v).
     * VMS $ENQ takes an acmode maximized against the caller's mode; OVMX
     * records the caller's current mode. Note this is the ACCESS mode
     * (0-3), NOT the lock mode in requested_mode/granted_mode (NL..EX, 0-5).
     * See docs/design-image-rundown-resource-classes.md.
     */
    exec_lock(&proc->mode_lock);
    lock->acmode = proc->current_mode;
    exec_unlock(&proc->mode_lock);

    if (args.flags & LCK_M_VALBLK)
        memcpy(lock->valblk, args.valblk, LCK_VALBLK_SIZE);

    /* Assign lock ID */
    lock_insert_id(lock);

    /* Add to process lock list */
    exec_lock(&proc->lock_list_lock);
    exec_list_add_tail(&lock->proc_list, &proc->locks);
    proc->lock_count++;
    exec_unlock(&proc->lock_list_lock);

    /* Try to grant */
    exec_lock(&res->lock);
    if (lock_compatible(res, args.lkmode, NULL)) {
        /* Granted immediately */
        if (xn)
            xn->queued = 0;
        lock->granted_mode = args.lkmode;
        exec_list_add_tail(&lock->res_granted, &res->granted);

        if (args.flags & LCK_M_VALBLK) {
            /* If user provided a value block, write it to resource.
             * Otherwise, read resource value block into lock. */
            int i, has_val = 0;
            for (i = 0; i < LCK_VALBLK_SIZE; i++) {
                if (lock->valblk[i]) { has_val = 1; break; }
            }
            if (has_val)
                memcpy(res->valblk, lock->valblk, LCK_VALBLK_SIZE);
            else
                memcpy(lock->valblk, res->valblk, LCK_VALBLK_SIZE);
        }

        exec_unlock(&res->lock);

        args.lkid = lock->lkid;
        args.lk_status = lock->granted_mode;
        if (args.flags & LCK_M_VALBLK)
            memcpy(args.valblk, lock->valblk, LCK_VALBLK_SIZE);
        args.status = SS__NORMAL;
    } else {
        /* Not compatible */
        if (args.flags & LCK_M_NOQUEUE) {
            exec_unlock(&res->lock);

            /* Clean up */
            exec_lock(&proc->lock_list_lock);
            exec_list_del(&lock->proc_list);
            proc->lock_count--;
            exec_unlock(&proc->lock_list_lock);
            lock_remove_id(lock);
            resource_release(res);
            lock_entry_free(lock);

            args.lkid = 0;
            args.status = SS__NOTQUEUED;
        } else {
            /* Queue the request */
            lock->waiting = 1;
            lock->grant_state = 0;
            exec_list_add_tail(&lock->res_waiting, &res->waiting);

            /*
             * Check for deadlock. Skipped in cross-node mode (xn set): the local
             * wait-for-graph detector keys on `proc`, but every cross-node lock
             * shares the delivery proc while representing a DIFFERENT cluster
             * owner, so it would false-positive; distributed deadlock detection is
             * a later rung (vms-ec75), honestly out of scope here.
             */
            if (!xn && check_deadlock(lock, 0)) {
                exec_list_del(&lock->res_waiting);
                exec_unlock(&res->lock);

                exec_lock(&proc->lock_list_lock);
                exec_list_del(&lock->proc_list);
                proc->lock_count--;
                exec_unlock(&proc->lock_list_lock);
                lock_remove_id(lock);
                resource_release(res);
                lock_entry_free(lock);

                args.lkid = 0;
                args.status = SS__DEADLOCK;
            } else {
                /* Notify blocking AST holders (LOCAL holders -- a real user-mode
                 * blocking AST). A cross-node holder has no local blkastadr, so it
                 * is handled by the directive below instead. */
                notify_blocking_asts(res, lock);

                /*
                 * Cross-node BLKAST directive (vms-904c). Still under res->lock:
                 * find a currently-granted holder that (a) blocks this request and
                 * (b) is held for a REMOTE cluster identity (req_csid != 0). That
                 * holder must be sent a blocking AST over SCS -- which is the
                 * daemon's transport job -- so the master hands its identity and
                 * lock handle back to the caller. First blocking remote holder
                 * only (the foundational rung; several blocked holders => several
                 * BLKASTs is a refinement). This is the executive genuinely FIRING
                 * the blocking AST decision, not a fabricated notification.
                 */
                if (xn) {
                    struct vms_lock_entry *g;
                    xn->queued = 1;
                    exec_list_for_each_entry(g, &res->granted, res_granted) {
                        if (!compat[lock->requested_mode][g->granted_mode] &&
                            g->req_csid != 0) {
                            xn->blocking_csid = g->req_csid;
                            xn->blocking_master_lkid = g->lkid;
                            /* H6 (vms-76d): the holder's OWN (requester-side) lock
                             * handle, so the daemon can address the BLKAST to the
                             * holder node's ORIGIN record (keyed by req_lkid). */
                            xn->blocking_req_lkid = g->req_lkid;
                            break;
                        }
                    }
                }
                exec_unlock(&res->lock);

                if (args.flags & LCK_M_SYNC) {
                    /*
                     * Synchronous $ENQW: block in-kernel until the request
                     * is granted (by another process's DEQ/CONVERT, via
                     * try_grant_waiters) or a deadlock is detected. This
                     * replaces the old userspace GETLKI poll loop.
                     */
                    if (enq_wait_sync(res, lock) == SS__DEADLOCK) {
                        /* Never granted: unwind the fresh request. */
                        exec_lock(&proc->lock_list_lock);
                        exec_list_del(&lock->proc_list);
                        proc->lock_count--;
                        exec_unlock(&proc->lock_list_lock);
                        lock_remove_id(lock);
                        resource_release(res);
                        lock_entry_free(lock);

                        args.lkid = 0;
                        args.status = SS__DEADLOCK;
                        goto out;
                    }

                    /* Granted at the requested mode. */
                    args.lkid = lock->lkid;
                    args.lk_status = lock->granted_mode;
                    if (args.flags & LCK_M_VALBLK)
                        memcpy(args.valblk, lock->valblk, LCK_VALBLK_SIZE);
                    args.status = SS__NORMAL;
                } else {
                    /*
                     * Async $ENQ: return the lock ID with the request
                     * queued (granted mode still NL). A completion AST is
                     * delivered later, on grant, if astadr was supplied.
                     */
                    args.lkid = lock->lkid;
                    args.lk_status = lock->requested_mode;
                    args.status = SS__NORMAL;
                }
            }
        }
    }

out:
    *io = args;
    return 0;
}

long vms_ioctl_enq(struct vms_proc *proc, unsigned long arg)
{
    struct vms_enq_args args;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;
    vms_enq_core_ex(proc, &args, NULL);   /* local $ENQ: full deadlock detection */
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_deq - Dequeue (release) a lock ($DEQ equivalent)
 */
static long vms_deq_core(struct vms_proc *proc, struct vms_deq_args *io)
{
    struct vms_deq_args args = *io;
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;

    lock = lock_find_by_id(args.lkid);
    if (!lock || lock->proc != proc) {
        if (lock)
            lock_put(lock);
        args.status = SS__IVLOCKID;
        goto out;
    }

    res = lock->resource;

    /* Remove from resource */
    exec_lock(&res->lock);

    /* Write back value block from lock to resource */
    if ((lock->flags & LCK_M_VALBLK) && !lock->waiting)
        memcpy(res->valblk, lock->valblk, LCK_VALBLK_SIZE);

    if (lock->waiting)
        exec_list_del(&lock->res_waiting);
    else
        exec_list_del(&lock->res_granted);

    /* Try to grant waiters now that this lock is released */
    try_grant_waiters(res);
    exec_unlock(&res->lock);

    /* Remove from process list */
    exec_lock(&proc->lock_list_lock);
    exec_list_del(&lock->proc_list);
    proc->lock_count--;
    exec_unlock(&proc->lock_list_lock);

    /* Remove from ID tree and free */
    lock_remove_id(lock);
    resource_release(res);
    lock_put(lock);  /* drop lock_find_by_id reference */
    lock_put(lock);  /* drop "exists in system" reference — triggers free */

    args.status = SS__NORMAL;

out:
    *io = args;
    return 0;
}

long vms_ioctl_deq(struct vms_proc *proc, unsigned long arg)
{
    struct vms_deq_args args;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;
    vms_deq_core(proc, &args);
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * ================================================================
 * In-kernel volume-synchronization lock for the Files-11 ACP (vms-233)
 * ================================================================
 *
 * The ODS-2 XQP serializes on-disk-structure updates through the distributed
 * lock manager's per-volume "volume synchronization" lock, NOT a userspace
 * flock (design-files11-acp-executive.md §4.7; OpenVMS I&DS Manual, XQP
 * execution model). The ACP runs in the caller's ioctl/process context, so it
 * enqueues DIRECTLY here -- no /dev/vms round-trip -- taking an EX-mode lock on
 * a per-volume resource across each allocate/read-modify-write/flush span.
 *
 * DEADLOCK-FREE BY CONSTRUCTION: an ACP write holds no other DLM lock while it
 * requests the volume lock and takes exactly one such lock at a time, so no
 * cross-resource wait cycle can form; the only wait is for another writer's
 * bounded critical section to end. Cluster/MSCP-ready by construction: the
 * resource name is the cluster-wide sync point, so a volume served to a second
 * node serializes through the same resource.
 */
uint32_t vms_lock_acp_vol_ex(struct vms_proc *proc, const char *resnam,
                             uint32_t *lkid_out)
{
    struct vms_enq_args a;

    if (lkid_out)
        *lkid_out = 0;
    memset(&a, 0, sizeof(a));
    a.lkmode = LCK_K_EXMODE;
    a.flags  = LCK_M_SYNC;      /* $ENQW: block in-kernel until granted */
    strscpy(a.resnam, resnam, sizeof(a.resnam));

    vms_enq_core_ex(proc, &a, NULL);   /* ACP volume lock: local, full detection */
    if (a.status == SS__NORMAL && lkid_out)
        *lkid_out = a.lkid;
    return a.status;
}

/* Release the volume lock taken by vms_lock_acp_vol_ex. A zero lkid (the lock
 * was never taken -- acquire failed, or the negctl dropped the enqueue) is a
 * no-op, so the ACP write path can release unconditionally at its single exit. */
uint32_t vms_lock_acp_vol_release(struct vms_proc *proc, uint32_t lkid)
{
    struct vms_deq_args d;

    if (lkid == 0)
        return SS__NORMAL;
    memset(&d, 0, sizeof(d));
    d.lkid = lkid;
    vms_deq_core(proc, &d);
    return d.status;
}

/*
 * vms_ioctl_convert - Convert lock mode ($ENQ with LCK$M_CONVERT)
 *
 * Changes the mode of an existing granted lock. If the new mode
 * is compatible, conversion happens immediately. Otherwise, the
 * lock is queued at the new mode (old mode remains until granted).
 */
long vms_ioctl_convert(struct vms_proc *proc, unsigned long arg)
{
    struct vms_enq_args args;
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    if (args.lkmode > LCK_K_EXMODE) {
        args.status = SS__BADPARAM;
        goto out;
    }

    lock = lock_find_by_id(args.lkid);
    if (!lock || lock->proc != proc) {
        if (lock)
            lock_put(lock);
        args.status = SS__IVLOCKID;
        goto out;
    }

    if (lock->waiting) {
        lock_put(lock);
        args.status = SS__CANCELGRANT;
        goto out;
    }

    res = lock->resource;

    exec_lock(&res->lock);

    /* Update blocking AST address if provided */
    if (args.blkastadr)
        lock->blkastadr = args.blkastadr;

    /* Write value block before conversion if requested */
    if (args.flags & LCK_M_VALBLK)
        memcpy(res->valblk, args.valblk, LCK_VALBLK_SIZE);

    /* Check compatibility (exclude self) */
    if (lock_compatible(res, args.lkmode, lock)) {
        /* Immediate conversion */
        lock->granted_mode = args.lkmode;

        if (args.flags & LCK_M_VALBLK)
            memcpy(lock->valblk, res->valblk, LCK_VALBLK_SIZE);

        exec_unlock(&res->lock);

        args.lk_status = lock->granted_mode;
        if (args.flags & LCK_M_VALBLK)
            memcpy(args.valblk, lock->valblk, LCK_VALBLK_SIZE);
        args.status = SS__NORMAL;
    } else {
        if (args.flags & LCK_M_NOQUEUE) {
            exec_unlock(&res->lock);
            args.status = SS__NOTQUEUED;
        } else {
            /* Move to waiting list, keep granted mode until converted */
            lock->requested_mode = args.lkmode;
            lock->waiting = 1;
            lock->grant_state = 0;
            exec_list_del(&lock->res_granted);
            exec_list_add_tail(&lock->res_waiting, &res->waiting);

            /* Check deadlock */
            if (check_deadlock(lock, 0)) {
                /* Undo: move back to granted */
                exec_list_del(&lock->res_waiting);
                lock->waiting = 0;
                exec_list_add_tail(&lock->res_granted, &res->granted);
                exec_unlock(&res->lock);
                args.status = SS__DEADLOCK;
            } else {
                notify_blocking_asts(res, lock);
                exec_unlock(&res->lock);

                if (args.flags & LCK_M_SYNC) {
                    /* Synchronous convert: block until granted at the new
                     * mode, or a deadlock is detected. On deadlock the lock
                     * retains its original granted mode (VMS semantics --
                     * a failed convert does not lose the held lock). */
                    if (enq_wait_sync(res, lock) == SS__DEADLOCK) {
                        exec_lock(&res->lock);
                        lock->requested_mode = lock->granted_mode;
                        lock->waiting = 0;
                        exec_list_add_tail(&lock->res_granted, &res->granted);
                        exec_unlock(&res->lock);
                        args.status = SS__DEADLOCK;
                    } else {
                        args.lk_status = lock->granted_mode;
                        if (args.flags & LCK_M_VALBLK)
                            memcpy(args.valblk, lock->valblk, LCK_VALBLK_SIZE);
                        args.status = SS__NORMAL;
                    }
                } else {
                    args.lk_status = lock->requested_mode;
                    args.status = SS__NORMAL;
                }
            }
        }
    }

    lock_put(lock);  /* drop lock_find_by_id reference */

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_getlki - Get lock information ($GETLKI equivalent)
 */
long vms_ioctl_getlki(struct vms_proc *proc, unsigned long arg)
{
    struct vms_getlki_args args;
    struct vms_lock_entry *lock;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    lock = lock_find_by_id(args.lkid);
    if (!lock) {
        /*
         * Fall through to the requester-side cross-node ORIGIN records (vms-6ca,
         * H5): a cross-node request THIS node issued is not a local lock (the
         * remote master holds it), so it has no entry in the lock-ID tree, but
         * its origin record carries the status the master's GRANT completed. This
         * makes the NL->EX flip driven by the remote master observable here.
         */
        uint32_t org_granted = 0, org_requested = 0;
        if (vms_lock_dlm_origin_getlki(args.lkid, &org_granted, &org_requested,
                                       args.resnam, sizeof(args.resnam),
                                       args.valblk)) {
            args.granted_mode = org_granted;
            args.requested_mode = org_requested;
            args.parent_id = 0;
            /* args.valblk was filled by origin_getlki: the master's LVB the
             * GRANT delivered (rd vms-eeb), or zeros if none -- the read
             * crossing surfacing the master's value block to this requester. */
            args.status = SS__NORMAL;
            goto out;
        }
        args.status = SS__IVLOCKID;
        goto out;
    }

    args.granted_mode = lock->granted_mode;
    args.requested_mode = lock->waiting ? lock->requested_mode : lock->granted_mode;
    args.parent_id = 0; /* TODO: parent lock support */

    if (lock->resource) {
        strscpy(args.resnam, lock->resource->name, sizeof(args.resnam));
        memcpy(args.valblk, lock->valblk, LCK_VALBLK_SIZE);
    } else {
        memset(args.resnam, 0, sizeof(args.resnam));
        memset(args.valblk, 0, LCK_VALBLK_SIZE);
    }

    args.status = SS__NORMAL;
    lock_put(lock);  /* drop lock_find_by_id reference */

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_get_resmaster - read a resource's DLM directory + mastering state
 * (vms-ci.5 DB). READ-ONLY: it reports which node is the directory for the
 * name, which node masters the resource (0 = not yet mastered), whether this
 * node is the master, and how many locks are granted. It does NOT create or
 * master a resource -- an unknown name returns found=0, master_csid=0 -- so a
 * test can call it before and after an $ENQ to prove the local-master path
 * actually mastered the resource, instead of asserting a hand-set structure.
 */
/*
 * vms_ioctl_dlm_member_depart - $DLM graceful member departure (rd vms-2bf,
 * DLM rung H10a). scsd calls this when it observes a graceful cluster departure
 * (SCS_MEMBER_OP_DEPART). Mark departed_csid gone from the LIVE directory
 * membership, then invalidate every resource's cached directory (res->dir_csid)
 * -- and a master that resolved to the departed node (res->master_csid ==
 * departed_csid) -- so the next resolution re-runs over the shrunk set:
 * dlm_directory_csid re-hashes % the smaller live count, and a departed master's
 * resources remaster to a survivor. A lock's STATE is NOT reconstructed here --
 * that (collecting survivors' origin records + rebuilding res->granted) is the
 * H10b rung (vms-dca9). Returns members_live (post-shrink directory count) and
 * found (1 iff departed_csid was a configured member). INV-6: reflects a REAL
 * departure the connection manager reported; nothing fabricated.
 */
long vms_ioctl_dlm_member_depart(struct vms_proc *proc, unsigned long arg)
{
    struct vms_dlm_depart_args args;
    struct vms_lock_resource *res;
    unsigned int i;
    int bkt;

    (void)proc;
    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&vms_res_hash_lock);

    /* Mark the departed CSID gone from the LIVE membership (the configured set
     * minus the runtime departure flags). CSID 0 is reserved ("unmastered") and
     * is never a member, so it never matches. */
    if (dlm_member_count > 0 && args.departed_csid != 0) {
        for (i = 0; i < (unsigned int)dlm_member_count && i < VMS_DLM_MAX_MEMBERS; i++) {
            if (dlm_member_csids[i] == args.departed_csid) {
                dlm_member_departed[i] = 1;
                args.found = 1;
            }
        }
    }

    /* Re-resolve the directory: the membership modulus changed, so every cached
     * res->dir_csid is stale -- clear it so dlm_directory_csid recomputes over
     * the shrunk set on next use. A resource mastered ON the departed node loses
     * its master (re-master on first use); one mastered on a survivor keeps it. */
    exec_hash_for_each(vms_res_hash, bkt, res, hash_node) {
        exec_lock(&res->lock);
        res->dir_csid = 0;
        if (res->master_csid == args.departed_csid)
            res->master_csid = 0;
        exec_unlock(&res->lock);
    }

    args.members_live = dlm_membership_count();
    exec_unlock(&vms_res_hash_lock);

    args.status = SS__NORMAL;
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

long vms_ioctl_get_resmaster(struct vms_proc *proc, unsigned long arg)
{
    struct vms_resmaster_args args;
    struct vms_lock_resource *res;

    (void)proc;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    args.resnam[31] = '\0';
    if (args.resnam[0] == '\0') {
        args.status = SS__BADPARAM;
        goto out;
    }

    args.local_csid = vms_local_csid;
    /*
     * The directory node for this name is a property of the name and the
     * membership, independent of whether the resource currently exists -- so
     * report it even when found=0. This is the same hash the enqueue path
     * masters through.
     */
    args.dir_csid = dlm_directory_csid(args.resnam);

    /*
     * Master reporting is AUTHENTIC, not directory-deterministic (vms-1bba). The
     * DIRECTORY node for a name is a deterministic property of the name + the
     * membership vector (dir_csid above -- identical on every node given the same
     * vector). The MASTER is NOT: on VMS a resource is UNMASTERED until its first
     * $ENQ (master_csid 0, is_local_master 0), then mastered ON FIRST USE -- and
     * the master need not be the directory node. So report the REAL mastering
     * state: 0/unmastered until a lock block exists here, the genuine
     * res->master_csid once it does (set in the found branch below; master_csid
     * and is_local_master default to 0 from the memset at entry). This preserves
     * the authentic "unmastered until locked" invariant (test_kmod_resdir).
     *
     * The cross-node CONSISTENCY this DB rung proves does NOT live in a
     * synthesized master field: it lives in dir_csid (every node computes the
     * SAME directory for a name) and in the enqueue split (a node grants/masters
     * a resource iff it is the directory -- else SS$_UNSUPPORTED). Reporting
     * dir_csid AS the master for an unmastered resource would conflate "who the
     * directory says is responsible" with "who actually masters it yet", and
     * fabricate a master where VMS has none.
     */

    /* Read the resource block, if one exists, without creating it. */
    exec_lock(&vms_res_hash_lock);
    res = resource_find(args.resnam);
    if (res) {
        struct vms_lock_entry *granted;
        uint32_t n = 0;

        exec_lock(&res->lock);
        args.found = 1;
        args.master_csid = res->master_csid;
        args.is_local_master =
            (res->master_csid != 0 && res->master_csid == vms_local_csid) ? 1 : 0;
        exec_list_for_each_entry(granted, &res->granted, res_granted) {
            n++;
            /*
             * Report the identity a REMOTE-held grant is held for (the first
             * one, if several). A local grant carries req_csid==0 and is
             * skipped, so remote_holder_csid stays 0 when every holder is
             * local. This is the held-lock proof the cross-node grant
             * (vms-e8f1) is verified by: after a peer's $ENQ, the master's DB
             * genuinely shows a lock held FOR that peer's CSID.
             */
            if (granted->req_csid != 0 && args.remote_holder_csid == 0)
                args.remote_holder_csid = granted->req_csid;
        }
        args.n_granted = n;
        exec_unlock(&res->lock);
    }
    exec_unlock(&vms_res_hash_lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_lock_dlm_xnode_deq - the cross-node DLM RELEASE ($DEQ) on the master
 * (DLM epic vms-7fa rung 3, vms-904c).
 *
 * A peer's $DEQ arrived over SCS: release the master's lock record identified by
 * req->master_lkid, held FOR the releasing node (req->req_csid). Cross-node
 * AUTHORIZATION is by cluster identity, NOT local proc: a node may release only a
 * lock the master holds for THAT node's CSID -- a local lock (req_csid == 0) or a
 * lock held for a different CSID is refused SS$_IVLOCKID. Releasing runs
 * try_grant_waiters, so a request queued behind this holder (the contention case)
 * GRANTS now -- the block-then-grant flip, driven by a real release, never faked.
 */
static uint32_t vms_lock_dlm_xnode_deq(struct vms_dlm_xnode_args *req)
{
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;

    lock = lock_find_by_id(req->master_lkid);   /* takes a reference */
    if (!lock)
        return SS__IVLOCKID;
    if (lock->req_csid == 0 || lock->req_csid != req->req_csid) {
        /* Not a cross-node lock, or not held for the releasing node. */
        lock_put(lock);
        return SS__IVLOCKID;
    }

    res = lock->resource;

    exec_lock(&res->lock);
    /*
     * LVB replication, cross-node WRITE (rd vms-d81, rung 6). A remote holder
     * that wrote the value block on convert/$DEQ (LCK_M_VALBLK) sends the new
     * 16 bytes on the $DEQ frame; scsd marshals them into req->valblk. When the
     * releaser set the flag and is not a waiter, replicate that WIRE value into
     * the MASTER resource so a subsequent $ENQ on this (the mastering) node
     * reads the updated LVB -- "node A writes the LVB, node B reads it". This is
     * req->valblk, NOT lock->valblk: the master lock's stored block is stale;
     * the fresh value lives on the $DEQ request. Mirrors the single-node
     * writeback in vms_deq_core, keyed on the releaser's write intent. (The
     * symmetric case -- a client READING the master's LVB back over the wire on
     * a cross-node $ENQ -- is a later rung: it needs the GRANT reply to carry
     * the LVB and the requester's origin record to surface it.)
     */
    if ((req->flags & LCK_M_VALBLK) && !lock->waiting)
        memcpy(res->valblk, req->valblk, LCK_VALBLK_SIZE);
    if (lock->waiting)
        exec_list_del(&lock->res_waiting);
    else
        exec_list_del(&lock->res_granted);

    /*
     * Deferred-GRANT discovery (vms-6ca, DLM epic vms-7fa rung H5). Snapshot the
     * FIRST cross-node waiter (held FOR a remote CSID) BEFORE the release, so
     * after try_grant_waiters we can tell whether THIS release flipped it from
     * queued to granted -- the block-then-grant transition the master must now
     * WIRE back to that requester as a deferred GRANT. We report the flipped
     * waiter's identity (its requester CSID + req_lkid + master lock handle +
     * granted mode) through the fields that are 0 on a DEQ, so the daemon that
     * delivered this $DEQ can transport the GRANT without a second round-trip.
     * First waiter only -- the same "first blocking holder" scope the contention
     * rung uses (vms-904c); several flipped waiters is a refinement. INV-6: this
     * REPORTS a grant the executive genuinely made; it fabricates nothing.
     */
    uint32_t pend_lkid = 0, pend_csid = 0, pend_req_lkid = 0;
    {
        struct vms_lock_entry *w;
        exec_list_for_each_entry(w, &res->waiting, res_waiting) {
            if (w->req_csid != 0) {
                pend_lkid = w->lkid;
                pend_csid = w->req_csid;
                pend_req_lkid = w->req_lkid;
                break;
            }
        }
    }

    /* The release grants any waiter now compatible -- the blocked cross-node
     * request flips from queued (grant_state 0) to granted (grant_state
     * SS$_NORMAL, granted_mode = requested). */
    try_grant_waiters(res);

    /* Did the snapshotted cross-node waiter just get granted? If so, name it in
     * the deferred-GRANT outputs (reusing the fields that are otherwise 0 on a
     * DEQ). We are still under res->lock, and the granted list is walked here. */
    req->queued = 0;
    req->blocking_csid = 0;
    req->blocking_master_lkid = 0;
    if (pend_lkid != 0) {
        struct vms_lock_entry *g;
        exec_list_for_each_entry(g, &res->granted, res_granted) {
            if (g->lkid == pend_lkid && !g->waiting &&
                g->grant_state == SS__NORMAL) {
                /* DEFERRED GRANT: this queued cross-node request is now granted. */
                req->queued = 1;                       /* "a waiter flipped"      */
                req->blocking_csid = pend_csid;        /* -> deferred-grant CSID  */
                req->blocking_master_lkid = g->lkid;   /* -> its master handle    */
                req->req_lkid = pend_req_lkid;         /* -> requester's handle   */
                req->lkmode = g->granted_mode;         /* -> the mode granted     */
                break;
            }
        }
        (void)pend_req_lkid;
    }
    exec_unlock(&res->lock);

    exec_lock(&lock->proc->lock_list_lock);
    exec_list_del(&lock->proc_list);
    if (lock->proc->lock_count > 0)
        lock->proc->lock_count--;
    exec_unlock(&lock->proc->lock_list_lock);

    lock_remove_id(lock);
    resource_release(res);
    lock_put(lock);  /* drop lock_find_by_id reference */
    lock_put(lock);  /* drop "exists in system" reference -- triggers free */
    return SS__NORMAL;
}

/*
 * vms_lock_dlm_xnode_grant_recv - the REQUESTER-SIDE GRANT RECEIVE (vms-6ca, DLM
 * epic vms-7fa rung H5). Previously SS$_UNSUPPORTED.
 *
 * A GRANT / queued-reply message the MASTER sent back over SCS in answer to a
 * cross-node $ENQ THIS node issued lands here. It completes the requester's
 * origin record -- the executive-resident proxy of the outstanding request --
 * so the request's status is genuine executive state, not a per-process
 * userspace flag (INV-6). The record's granted mode is set ONLY from what the
 * master genuinely sent:
 *   - a queued-reply carries lkmode == NL  -> the origin record stays pending
 *     (granted_mode NL): the requester genuinely sees "blocked", from the
 *     master's real waiting-queue decision transported over the wire.
 *   - a deferred GRANT carries lkmode == the granted mode (e.g. EX) and
 *     status SS$_NORMAL -> the origin record flips NL -> EX. THIS is the status
 *     flip observed on the REQUESTER node, driven by the master's real release.
 *
 * find-or-create keyed by the requester's own lock handle (req_lkid). The record
 * never touches the resource granted/waiting queues (the local lock manager
 * cannot auto-grant it); only this wire path completes it. GETLKI(req_lkid)
 * reads it back, so the flip is independently observable.
 *
 * Returns SS$_NORMAL when the receive was accepted (the record now reflects the
 * master's status), or SS$_INSFMEM if the record could not be allocated. It
 * grants nothing itself -- it records the master's genuine decision.
 */
static uint32_t vms_lock_dlm_xnode_grant_recv(struct vms_dlm_xnode_args *req)
{
    struct vms_dlm_origin *org = NULL, *cur;

    if (req->req_lkid == 0)
        return SS__BADPARAM;   /* a GRANT must name the requester's own handle */

    exec_lock(&vms_dlm_origin_lock);
    exec_list_for_each_entry(cur, &vms_dlm_origin_list, list) {
        if (cur->req_lkid == req->req_lkid) {
            org = cur;
            break;
        }
    }
    if (org == NULL) {
        org = exec_zalloc(sizeof(*org));
        if (org == NULL) {
            exec_unlock(&vms_dlm_origin_lock);
            return SS__INSFMEM;
        }
        org->req_lkid = req->req_lkid;
        org->req_csid = req->req_csid;
        org->requested_mode = LCK_K_EXMODE; /* refined below from the reply mode */
        org->granted_mode = LCK_K_NLMODE;   /* pending until a real grant arrives */
        memcpy(org->resnam, req->resnam, sizeof(org->resnam));
        org->resnam[sizeof(org->resnam) - 1] = '\0';
        exec_list_add_tail(&org->list, &vms_dlm_origin_list);
    }

    /* Record what the MASTER genuinely reported. The master's lock handle and
     * CSID come from the reply; the granted mode is whatever the master said the
     * request is granted at (NL == still pending/queued, non-NL == granted). */
    if (req->master_lkid != 0)
        org->master_lkid = req->master_lkid;
    if (req->master_csid != 0)
        org->master_csid = req->master_csid;
    if (req->lkmode > LCK_K_NLMODE)
        org->requested_mode = req->lkmode;
    org->granted_mode = req->lkmode;   /* NL on a queued-reply; EX on a grant */

    /*
     * LVB read crossing (rd vms-eeb, rung H9). A real GRANT (a non-NL granted
     * mode) carries the master's resource value block in the reply; store it on
     * the origin record so GETLKI(req_lkid) surfaces it to the requester's $ENQ
     * caller -- the mirror of the write crossing (vms-d81). A queued-reply (NL)
     * delivers no LVB, so valblk_valid stays clear and GETLKI reports zeros
     * honestly rather than a stale block. The bytes are the master's real
     * res->valblk transported over SCS, never fabricated (INV-6).
     */
    if (req->lkmode > LCK_K_NLMODE) {
        memcpy(org->valblk, req->valblk, LCK_VALBLK_SIZE);
        org->valblk_valid = 1;
    }

    /*
     * BLKAST WIRE (rung H6, vms-76d). When this GRANT establishes the record as a
     * HOLDER proxy (a real granted mode) AND the holder registered a blocking-AST
     * routine, remember it so a BLKAST the master later sends over SCS can fire a
     * genuine user-mode AST. Recorded ONLY from what the holder supplied on its own
     * $ENQ (req->blkastadr) -- never fabricated. A queued-reply (NL) or a GRANT with
     * no blkastadr leaves the record with no blocking AST, so a later BLKAST honestly
     * declines rather than inventing a delivery (INV-6).
     */
    if (req->blkastadr != 0 && req->lkmode > LCK_K_NLMODE) {
        org->blkastadr = req->blkastadr;
        org->blkastprm = req->blkastprm;
    }

    exec_unlock(&vms_dlm_origin_lock);
    return SS__NORMAL;
}

/*
 * vms_lock_dlm_xnode_blkast_recv - the HOLDER-SIDE BLOCKING-AST DELIVERY (rung H6,
 * vms-76d). The symmetric mirror of vms_lock_dlm_xnode_grant_recv: a BLKAST the
 * MASTER sent over SCS -- because a conflicting request queued behind this node's
 * granted lock -- lands here and FIRES the holder's blocking AST for real.
 *
 * The record is the holder-side ORIGIN proxy the GRANT receive established (keyed
 * by the holder's own req_lkid, which the BLKAST frame carries). If it exists and
 * the holder registered a blocking-AST routine (org->blkastadr, from its own $ENQ),
 * a REAL user-mode AST is queued to the delivering process's USER-mode AST queue --
 * the SAME mechanism notify_blocking_asts/queue_completion_ast use -- so the holder
 * genuinely receives the blocking AST (drainable via VMS_IOCTL_DELIVERAST), not a
 * log line. `proc` is the holder node's registered delivery context (the daemon that
 * both established the record and dispatches this BLKAST -- one process), so no stale
 * proc pointer is stored on the record.
 *
 * Returns SS$_NORMAL and sets req->blkast_delivered=1 when an AST was genuinely
 * queued. Returns SS$_UNSUPPORTED (never faked) when there is no holder record for
 * the named handle or it carries no blocking-AST routine -- honest, per INV-6.
 */
static uint32_t vms_lock_dlm_xnode_blkast_recv(struct vms_proc *proc,
                                               struct vms_dlm_xnode_args *req)
{
    struct vms_dlm_origin *org = NULL, *cur;
    struct vms_ast_entry *ast;
    struct vms_ast_state *ast_state;
    uint64_t blkastadr = 0, blkastprm = 0;

    if (proc == NULL)
        return SS__BADPARAM;
    if (req->req_lkid == 0)
        return SS__BADPARAM;   /* a BLKAST must name the holder's own handle */

    exec_lock(&vms_dlm_origin_lock);
    exec_list_for_each_entry(cur, &vms_dlm_origin_list, list) {
        if (cur->req_lkid == req->req_lkid && cur->blkastadr != 0) {
            org = cur;
            blkastadr = cur->blkastadr;
            blkastprm = cur->blkastprm;
            break;
        }
    }
    if (org == NULL) {
        exec_unlock(&vms_dlm_origin_lock);
        return SS__UNSUPPORTED;   /* no holder record / no blocking AST -- honest */
    }
    org->blkast_count++;
    exec_unlock(&vms_dlm_origin_lock);

    /* Queue a REAL user-mode blocking AST to the holder's delivery process. Mirrors
     * notify_blocking_asts: astprm is the holder's own request handle (VMS delivers
     * the lock id as the blocking AST parameter unless the holder supplied one). */
    ast = exec_zalloc_atomic(sizeof(*ast));
    if (ast == NULL)
        return SS__INSFMEM;
    ast->astadr = blkastadr;
    ast->astprm = blkastprm ? blkastprm : (uint64_t)req->req_lkid;
    ast->acmode = PSL_C_USER;

    ast_state = &proc->ast[PSL_C_USER];
    exec_lock(&ast_state->lock);
    if (ast_state->count < VMS_AST_MAX_PER_MODE) {
        exec_list_add_tail(&ast->list, &ast_state->pending);
        ast_state->count++;
        exec_unlock(&ast_state->lock);
        vms_ast_notify_arrival(proc);
    } else {
        exec_free(ast);
        exec_unlock(&ast_state->lock);
        return SS__INSFMEM;   /* queue full -- honest failure, no faked delivery */
    }

    req->blkast_delivered = 1;
    return SS__NORMAL;
}

/*
 * vms_lock_dlm_origin_getlki - GETLKI fall-through for a requester-side origin
 * record (vms-6ca, H5). Returns 1 and fills the getlki fields if an origin
 * record with lkid == req_lkid exists; 0 otherwise. Lets GETLKI observe the
 * cross-node request's status flip on the REQUESTER node.
 */
static int vms_lock_dlm_origin_getlki(uint32_t lkid, uint32_t *granted_mode,
                                      uint32_t *requested_mode, char *resnam,
                                      size_t resnam_len, uint8_t *valblk)
{
    struct vms_dlm_origin *cur;
    int found = 0;

    if (lkid == 0)
        return 0;
    exec_lock(&vms_dlm_origin_lock);
    exec_list_for_each_entry(cur, &vms_dlm_origin_list, list) {
        if (cur->req_lkid == lkid) {
            if (granted_mode)
                *granted_mode = cur->granted_mode;
            if (requested_mode)
                *requested_mode = cur->requested_mode;
            if (resnam && resnam_len)
                strscpy(resnam, cur->resnam, resnam_len);
            /*
             * LVB read crossing (rd vms-eeb): surface the value block the
             * master's GRANT delivered (valblk_valid), else zeros -- an
             * unset LVB is reported honestly, never a stale/fabricated block.
             */
            if (valblk) {
                if (cur->valblk_valid)
                    memcpy(valblk, cur->valblk, LCK_VALBLK_SIZE);
                else
                    memset(valblk, 0, LCK_VALBLK_SIZE);
            }
            found = 1;
            break;
        }
    }
    exec_unlock(&vms_dlm_origin_lock);
    return found;
}

/*
 * vms_lock_dlm_xnode_dispatch - the cross-node DLM RECEIVE handler
 * (vms-94c transport; DLM epic vms-7fa).
 *
 * A DLM request that arrived over SCS from a REMOTE node (decoded by
 * src/vmsscs/scs_dlm.c, marshalled through VMS_IOCTL_DLM_XNODE) is dispatched
 * HERE -- the point at which the kernel lock manager acts on a peer's behalf, as
 * the resource's master (membership is a stub-of-one, so this node is directory +
 * master for the name and grants/queues locally; that is the mastering node's
 * REAL lock state, never a fabrication).
 *
 * RUNG 2 -- THE FOUNDATION GRANT (vms-e8f1): a COMPATIBLE cross-node $ENQ grants
 * (SS$_NORMAL), the lock held FOR the remote requester's CSID (owner_csid), so
 * GET_RESMASTER reports remote_holder_csid == the peer.
 *
 * RUNG 3 -- CROSS-NODE CONTENTION / BLOCK-THEN-GRANT + BLKAST (vms-904c). The ENQ
 * scope-fence is LIFTED on exactly three things, implemented for real:
 *   - An INCOMPATIBLE cross-node $ENQ (no NOQUEUE) now QUEUES on the master's real
 *     waiting queue instead of declining. The dispatch returns VMS_DLM_STS_QUEUED
 *     (0, "no completion posted") -- NOT SS$_NORMAL, NOT SS$_NOTQUEUED -- with
 *     req->queued=1 and req->master_lkid the queued lock's handle. It NEVER blocks
 *     the delivery thread (async queue, no LCK_M_SYNC). A wire NOQUEUE still
 *     declines SS$_NOTQUEUED (honest).
 *   - The master FIRES the blocking-AST decision: when the queued request blocks a
 *     REMOTE holder, req->blocking_csid / req->blocking_master_lkid name the holder
 *     that must receive a BLKAST over SCS (the daemon transports it).
 *   - VMS_DLM_OP_DEQ is IMPLEMENTED (vms_lock_dlm_xnode_deq): a peer's release
 *     runs try_grant_waiters, so the blocked request GRANTS -- the block-then-grant
 *     flip, driven by a real $DEQ.
 *
 * RUNG H6 -- THE BLKAST WIRE (vms-76d). VMS_DLM_OP_GRANT (requester-side GRANT
 * receive, H5) and VMS_DLM_OP_BLKAST (holder-side blocking-AST delivery, H6) are
 * both IMPLEMENTED RECEIVE ops now: a GRANT completes the requester's origin record,
 * and a BLKAST fires the holder's blocking AST for real (a genuine user-mode AST on
 * the holder's process). The ENQ path additionally reports blocking_req_lkid -- the
 * blocking holder's requester-side handle -- so the daemon can address the BLKAST to
 * the holder node's origin record. INV-6: a BLKAST with no matching holder record or
 * no registered blocking-AST routine declines SS$_UNSUPPORTED, never a faked AST.
 *
 * STILL FENCED HONESTLY (INV-6 -- SS$_UNSUPPORTED, never faked):
 *   - LVB replication (vms-d81), resource-directory consistency (vms-1bba),
 *     remastering (vms-6ee), distributed deadlock detection (vms-ec75).
 *
 * The request is VALIDATED so a malformed message is rejected (SS$_BADPARAM)
 * rather than silently dropped -- the same discipline vms_enq_core applies.
 */
uint32_t vms_lock_dlm_xnode_dispatch(struct vms_proc *proc,
                                     struct vms_dlm_xnode_args *req)
{
    if (!req)
        return SS__BADPARAM;
    if (!proc)
        return SS__BADPARAM;
    if (req->lkmode > LCK_K_EXMODE)
        return SS__BADPARAM;
    req->resnam[sizeof(req->resnam) - 1] = '\0';

    /* Clean contention outputs on every path; the ENQ branch fills them. */
    req->queued = 0;
    req->blocking_csid = 0;
    req->blocking_master_lkid = 0;
    req->blocking_req_lkid = 0;   /* H6: the BLKAST target handle (ENQ fills it) */
    req->blkast_delivered = 0;    /* H6: 1 only when a BLKAST receive fires an AST */

    switch (req->op) {
    case VMS_DLM_OP_ENQ: {
        struct vms_enq_args a;
        struct dlm_xnode_enq_out xn;

        /* A request that names a resource must actually name one. */
        if (req->resnam[0] == '\0')
            return SS__BADPARAM;

        /*
         * Marshal the decoded cross-node $ENQ into the single-node lock manager
         * on this (the mastering) node, in CROSS-NODE mode (xn): the lock is held
         * FOR the remote requester's cluster identity (owner_csid), local deadlock
         * detection is skipped (distributed, rung 7), and a blocked request is
         * QUEUED -- not declined. NOQUEUE is honored from the wire flags; so is
         * VALBLK (the lock value block read crossing, rd vms-eeb -- see below);
         * LCK_M_SYNC is not (it must never block the delivery thread).
         */
        memset(&a, 0, sizeof(a));
        a.lkmode = req->lkmode;
        a.flags = req->flags & (LCK_M_NOQUEUE | LCK_M_VALBLK);
        memcpy(a.resnam, req->resnam, sizeof(a.resnam));
        a.resnam[sizeof(a.resnam) - 1] = '\0';
        a.owner_csid = req->req_csid;   /* held FOR the remote requester */
        /*
         * LVB READ crossing (rd vms-eeb, rung H9). Carry the requester's inbound
         * value block into the single-node core: with LCK_M_VALBLK set and a zero
         * inbound block, vms_enq_core_ex's grant path READS the master's current
         * res->valblk back into a.valblk. That read-back is copied into the reply
         * below, so the GRANT the master sends over SCS carries its LVB to the
         * requester -- "node B reads the master's updated LVB on a cross-node
         * $ENQ", the mirror of vms-d81's write crossing.
         */
        memcpy(a.valblk, req->valblk, sizeof(a.valblk));

        memset(&xn, 0, sizeof(xn));
        xn.req_lkid = req->req_lkid;    /* stamp the master lock with the
                                         * requester's own handle (H5) */
        vms_enq_core_ex(proc, &a, &xn);

        /* Hand the master's lock id back (the GRANT reply's master_lkid) and the
         * contention outputs. On a NOQUEUE decline a.lkid is 0. */
        req->master_lkid = a.lkid;
        /* Return the master's current value block to the requester (rd vms-eeb).
         * The GRANT reply builder in scsd copies this into the wire frame. */
        memcpy(req->valblk, a.valblk, sizeof(req->valblk));
        req->master_csid = vms_local_csid; /* this node mastered the request */
        req->queued = xn.queued ? 1u : 0u;
        req->blocking_csid = xn.blocking_csid;
        req->blocking_master_lkid = xn.blocking_master_lkid;
        req->blocking_req_lkid = xn.blocking_req_lkid; /* H6 BLKAST target handle */

        /* A queued request is a REAL lock on the waiting queue, not a grant. */
        if (xn.queued)
            return VMS_DLM_STS_QUEUED;
        return a.status;
    }
    case VMS_DLM_OP_DEQ:
        /* A request that names a resource must actually name one. */
        if (req->resnam[0] == '\0')
            return SS__BADPARAM;
        return vms_lock_dlm_xnode_deq(req);
    case VMS_DLM_OP_GRANT:
        /* REQUESTER-SIDE GRANT RECEIVE (vms-6ca, H5). A GRANT / queued-reply the
         * MASTER sent back for a cross-node $ENQ THIS node issued completes the
         * requester's origin record -- genuine executive state, the status flip
         * observed on the requester node. Was SS$_UNSUPPORTED. */
        return vms_lock_dlm_xnode_grant_recv(req);
    case VMS_DLM_OP_BLKAST:
        /* HOLDER-SIDE BLKAST RECEIVE (rung H6, vms-76d). A BLKAST the master sent
         * over SCS -- because a conflicting request queued behind this node's
         * granted lock -- FIRES the holder's blocking AST for real: a genuine
         * user-mode AST queued to the holder's process (drainable via
         * VMS_IOCTL_DELIVERAST), the symmetric mirror of the requester-side GRANT
         * RECEIVE. Was SS$_UNSUPPORTED (the wire was deferred at H5). Still
         * SS$_UNSUPPORTED, never faked, when there is no holder record for the
         * named handle or it registered no blocking-AST routine (INV-6). */
        return vms_lock_dlm_xnode_blkast_recv(proc, req);
    default:
        return SS__BADPARAM;
    }
}

/*
 * vms_ioctl_dlm_xnode - VMS_IOCTL_DLM_XNODE marshalling wrapper (vms-94c).
 * Copies the decoded DLM request in, runs the cross-node handler, copies the
 * status back. Requires no registered process (like GET_RESMASTER): the daemon
 * that issues it is delivering a peer's request, not enqueuing its own.
 */
long vms_ioctl_dlm_xnode(struct vms_proc *proc, unsigned long arg)
{
    struct vms_dlm_xnode_args args;

    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    args.status = vms_lock_dlm_xnode_dispatch(proc, &args);

    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
