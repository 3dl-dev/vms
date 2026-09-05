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
#include "exec_hash.h"        /* exec_hash_* (the resource database)          */
#include "exec_rbtree.h"      /* exec_rbtree_* / exec_rb_* (lock-ID database) */
#include "vms_dlm_proxy.h"    /* the PROXY-LKB requester seam (FC-P4.4) */

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
 * The PROXY LKB: the requester-side image of a lock mastered elsewhere
 * (FC-P4.4; design SS3.4 + hard call 7; Davis p. 6-52's *process copy*)
 * ================================================================
 *
 * When THIS node issues a $ENQ for a resource whose tree is mastered on ANOTHER
 * node, the request is not something the local lock manager may grant -- the
 * master grants it. But the requester still needs a REAL, executive-resident
 * lock block, so that its completion is genuine executive state and not a
 * per-process userspace fake (INV-6), and so that $GETLKI, $DEQ, convert,
 * blocking-AST delivery and the lock value block all have ONE object to operate
 * on. That object is a proxy LKB: an ordinary struct vms_lock_entry with
 * proxy == 1, carrying master_csid and (once the master answers) master_lkid.
 *
 * It REPLACES the vms_dlm_origin side list, which duplicated LKB fields on a
 * second keyspace: a proxy LKB's own lock id IS the handle this node puts on
 * the wire as req_lkid, so the lock-ID database is the (req_csid, req_lkid)
 * index -- no second table, and a duplicate reply can only ever find the ONE
 * lock it names (the idempotency D-DLM-5 asks for).
 *
 * WHY IT IS ON A THIRD RESOURCE QUEUE. A proxy hangs off res->proxies, never
 * off res->granted or res->waiting. try_grant_waiters() and lock_compatible()
 * walk only the latter two, so the executive is structurally incapable of
 * granting -- or of counting as a blocker -- a lock the cluster masters
 * elsewhere. Only a message from the master (vms_lock_dlm_xnode_grant_recv)
 * moves a proxy's granted mode. This is the same guarantee the origin list gave
 * by living outside the queues, kept by construction rather than by a flag test
 * inside every scan.
 *
 * WHERE IT DIVERGES FROM VMS. VMS keeps process-copy LKBs on the RSB's own
 * granted/waiting queues and tells the three kinds apart by a field (p. 6-52).
 * OVMX splits the queue instead, because in OVMX those queues ARE the local
 * granting algorithm's input. The behaviour is identical -- a tree is mastered
 * on exactly one node, so a node never runs local granting on a tree it proxies
 * -- and the split makes the fabrication unrepresentable rather than merely
 * absent.
 */

/*
 * The requester ops (vms_dlm_proxy.h): how the engine reaches the wire. NULL
 * until the cluster's DLM arm registers them at start, and NULL again at stop,
 * so an OVMX node with no cluster is purely local and REFUSES a remote-mastered
 * resource honestly (SS$_UNSUPPORTED) instead of serving it from thin air.
 * Written only through vms_lock_dlm_set_requester_ops under this lock.
 */
static struct vms_dlm_requester_ops vms_dlm_req_ops;
static exec_lock_t vms_dlm_req_ops_lock;

/* ================================================================
 * Cluster membership does NOT live here (FC-P3.9)
 * ================================================================
 *
 * rd vms-551 put a module-global `vms_cluster_members[]` block in this file
 * that a USERSPACE daemon populated through VMS_IOCTL_CLUSTER_MEMBER_SET/
 * _CLEAR while SHOW CLUSTER read it back -- an executive-shaped mirror of a
 * userspace fact. The operator's 2026-09-02 reset retired that daemon, and
 * FC-P3.9 deleted the block with it: membership is the CONNECTION MANAGER's
 * CLUB and CSB table (src/kernel-core/vms_cnxman.c), and
 * VMS_IOCTL_CLUSTER_MEMBER_GET now projects THAT
 * (vms_ioctl_cluster_member_get, src/kernel-core/vms_devtab.c). There is no
 * setter any more, on any path, because there is nothing outside the
 * executive entitled to assert who the cluster is.
 */

int vms_lock_init(void)
{
    exec_lock_init(&vms_lock_id_lock);
    exec_lock_init(&vms_res_hash_lock);
    exec_lock_init(&vms_dlm_req_ops_lock);
    exec_rbtree_init(&vms_lock_id_tree);
    exec_hash_init(vms_res_hash);
    return 0;
}

/*
 * vms_lock_dlm_set_requester_ops - install (or remove, with NULL) the ops the
 * engine posts proxy requests through (vms_dlm_proxy.h). Called by the DLM's
 * wire arm at cluster start/stop, never by a test that wants to pretend a
 * cluster is there: with no ops the engine is honestly local-only.
 */
void vms_lock_dlm_set_requester_ops(const struct vms_dlm_requester_ops *ops)
{
    exec_lock(&vms_dlm_req_ops_lock);
    if (ops)
        vms_dlm_req_ops = *ops;
    else
        memset(&vms_dlm_req_ops, 0, sizeof(vms_dlm_req_ops));
    exec_unlock(&vms_dlm_req_ops_lock);
}

/* A snapshot of the installed ops, taken under the ops lock so a concurrent
 * cluster stop cannot tear the function pointer out from under a poster. */
static struct vms_dlm_requester_ops dlm_req_ops_get(void)
{
    struct vms_dlm_requester_ops o;

    exec_lock(&vms_dlm_req_ops_lock);
    o = vms_dlm_req_ops;
    exec_unlock(&vms_dlm_req_ops_lock);
    return o;
}

/*
 * dlm_proxy_fill_post - build one outbound request FROM THE LOCK BLOCK.
 *
 * This is the INV-6 chokepoint for the requester side. Every field is read out
 * of the LKB and its RSB here, at post time. Nothing is copied from an inbound
 * frame, nothing comes from a template, and there is no parameter by which a
 * caller could supply a value the executive does not hold. Caller holds
 * res->lock.
 */
/* The seam states the value-block length itself so vms_dlm_proxy.h needs no
 * substrate struct twin (FC-P4.6). This is the TU that sees both spellings, so
 * this is where a drift becomes a build failure instead of a wire bug. */
_Static_assert(VMS_DLM_VALBLK_LEN == LCK_VALBLK_SIZE,
               "vms_dlm_proxy.h VMS_DLM_VALBLK_LEN must equal LCK_VALBLK_SIZE");

static void dlm_proxy_fill_post(const struct vms_lock_entry *lock,
                                const struct vms_lock_resource *res,
                                uint32_t op, uint32_t dst_csid,
                                struct vms_dlm_proxy_post *p)
{
    memset(p, 0, sizeof(*p));
    p->op          = op;
    p->dst_csid    = dst_csid;
    p->req_csid    = vms_local_csid;      /* this node, from the executive */
    p->req_lkid    = lock->lkid;          /* the proxy's own handle */
    p->master_csid = lock->master_csid;   /* 0 until the cluster told us */
    p->master_lkid = lock->master_lkid;   /* 0 until the master named it */
    p->lkmode      = lock->requested_mode;
    p->flags       = lock->flags;
    strscpy(p->resnam, res->name, sizeof(p->resnam));
    memcpy(p->valblk, lock->valblk, LCK_VALBLK_SIZE);
    /*
     * The root name's DIRECTORY HASH, off the resource block (FC-P4.6). Never
     * computed here or anywhere else: this is the value some system in the
     * cluster put on the wire for this exact name, recorded by
     * vms_lock_dlm_learn_dir_hash(). The wire arm needs it to address a
     * directory lookup at all, and this is the only non-deriving source.
     */
    p->dir_hash       = res->hash16;
    p->dir_hash_known = res->hash_known ? 1u : 0u;
    /*
     * Which of the two things this transmission IS. `dst_csid` is the master
     * when the cluster has named one (the RSB or the LKB carries it) and the
     * directory node otherwise -- and the wire arm must not have to guess,
     * because a directory node that is also the master (p. 6-31 outcome 1)
     * makes the two CSIDs equal.
     */
    p->to_directory = (lock->master_csid == 0u && res->master_csid == 0u) ? 1u : 0u;
}

/*
 * dlm_proxy_post - hand one request to the cluster's DLM arm.
 *
 * Refuses to post without a lock id (VMS_DLM_LKID_UNSET, the codec's rule
 * enforced at the engine too: a placeholder lock id bugchecked a real VAX with
 * INVLOCKID). Refuses honestly when no ops are installed -- a node with no
 * cluster does not get to pretend it asked one. `post` is contractually
 * non-blocking and must not re-enter the lock manager.
 */
static uint32_t dlm_proxy_post(const struct vms_dlm_proxy_post *p)
{
    struct vms_dlm_requester_ops ops = dlm_req_ops_get();

    if (p->req_lkid == 0 || p->dst_csid == 0)
        return SS__BADPARAM;
    if (ops.post == NULL)
        return SS__UNSUPPORTED;
    return ops.post(ops.ctx, p);
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
        /* Free the PROXY LKBs for resources this node does not master
         * (FC-P4.4) -- the third queue, same discipline as the other two. */
        exec_list_for_each_entry_safe(lock, ltmp, &res->proxies, res_proxy) {
            exec_list_del(&lock->res_proxy);
            lock_entry_free(lock);
        }
        exec_hash_del(&res->hash_node);
        resource_free(res);
    }
    exec_unlock(&vms_res_hash_lock);

    /* Tear down the runtime-initialized locks (a no-op on Linux; a real
     * mutex_destroy on NetBSD -- paired with the exec_lock_init in
     * vms_lock_init). */
    exec_lock_destroy(&vms_dlm_req_ops_lock);
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

/*
 * lock_insert_id_at - insert a lock at a lock id the CLUSTER named, not one this
 * node minted (FC-P4.4).
 *
 * Used on exactly one path: a message from a master naming a req_lkid for which
 * this node holds no proxy LKB (vms_lock_dlm_xnode_grant_recv's reconstruct
 * case). req_lkid is by definition a lock id of THIS node, so it belongs in the
 * same lock-ID database as every other lock -- there is no second keyspace.
 *
 * Returns 0 on success, or -1 if that id is already taken (the caller then
 * refuses the message rather than minting a second lock under one handle).
 * On success vms_next_lock_id is advanced past `lkid`, so a later local $ENQ
 * can never collide with it.
 */
static int lock_insert_id_at(struct vms_lock_entry *entry, uint32_t lkid)
{
    exec_rbtree_node_t **p, *parent = NULL;

    if (lkid == 0)
        return -1;

    exec_lock(&vms_lock_id_lock);
    p = exec_rbtree_root_link(&vms_lock_id_tree);
    while (*p) {
        struct vms_lock_entry *e = exec_rb_entry(*p, struct vms_lock_entry, rb_node);
        parent = *p;
        if (lkid < e->lkid)
            p = exec_rb_left_link(*p);
        else if (lkid > e->lkid)
            p = exec_rb_right_link(*p);
        else {
            exec_unlock(&vms_lock_id_lock);
            return -1;      /* already taken -- never two locks on one handle */
        }
    }
    entry->lkid = lkid;
    if (lkid >= vms_next_lock_id)
        vms_next_lock_id = lkid + 1;
    exec_rb_link_node(&entry->rb_node, parent, p);
    exec_rb_insert_color(&entry->rb_node, &vms_lock_id_tree);
    exec_unlock(&vms_lock_id_lock);
    return 0;
}

static void lock_remove_id(struct vms_lock_entry *entry)
{
    exec_lock(&vms_lock_id_lock);
    exec_rb_erase(&entry->rb_node, &vms_lock_id_tree);
    exec_unlock(&vms_lock_id_lock);
}

/*
 * lock_unlink_from_res - take a lock off whichever of the resource's three
 * queues it is on. Caller holds res->lock. A proxy LKB is on res->proxies and
 * nowhere else (FC-P4.4), so its `waiting` flag says "pending at the master",
 * not "on the local waiting queue" -- test proxy FIRST.
 */
static void lock_unlink_from_res(struct vms_lock_entry *lock)
{
    if (lock->proxy)
        exec_list_del(&lock->res_proxy);
    else if (lock->waiting)
        exec_list_del(&lock->res_waiting);
    else
        exec_list_del(&lock->res_granted);
}

/* ================================================================
 * Resource management (hash table by name)
 * ================================================================ */

/*
 * resource_hash_key - the LOCAL BUCKET KEY for the resource database.
 *
 * This is a private index into this node's own hash table and NOTHING ELSE. It
 * is never sent, never compared against another node's value, and never used
 * to choose a directory node -- VMS's directory hash is a different quantity
 * that OVMX takes off the wire and never computes (vms_dlm_ldwv.h, Davis
 * p. 6-50). The seam's shared exec_jhash() was deleted with FC-P4.3 precisely
 * so there is no general-purpose "the hash function" left in the executive for
 * a future directory path to reach for by accident; what remains is these six
 * lines, inside the one function that names the bucket array.
 *
 * FNV-1a over the name's significant bytes: public-domain, unrelated to
 * anything VMS does, and its VALUE is unobservable outside this file (a lookup
 * confirms the name with strncmp), which is what makes it safe to be OVMX's
 * own choice.
 */
static uint32_t resource_hash_key(const char *name)
{
    uint32_t h = 2166136261u;   /* FNV-1a offset basis */
    size_t i, n = strnlen(name, 32);

    for (i = 0; i < n; i++) {
        h ^= (uint32_t)(unsigned char)name[i];
        h *= 16777619u;         /* FNV-1a prime */
    }
    return h;
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
    exec_list_head_init(&new_res->proxies);   /* FC-P4.4 */
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
    /* A resource with a PROXY LKB on it is still in use by this node even
     * though it holds no local grant (FC-P4.4): the proxy is the requester-side
     * image of a lock the cluster holds for us, and freeing the RSB under it
     * would orphan the master's handle. */
    if (res->refcount <= 0 && exec_list_empty(&res->granted) &&
        exec_list_empty(&res->waiting) && exec_list_empty(&res->proxies)) {
        /* Preserve resource if it has a non-zero value block */
        for (i = 0; i < LCK_VALBLK_SIZE; i++) {
            if (res->valblk[i]) { has_valblk = 1; break; }
        }
        /*
         * ...and preserve it if the CLUSTER has told us this name's directory
         * hash (FC-P4.3). The value cannot be recomputed -- it is only ever
         * read off the wire (Davis p. 6-50) -- so discarding the block that
         * holds it would guarantee an SS$_UNSUPPORTED the next time this node
         * touches the name, on exactly the shared resources the cluster talks
         * about most. This is the resource database keeping what the cluster
         * told it, which is what VMS's own Resource Hash Table does (p. 6-49).
         */
        if (!has_valblk && !res->hash_known) {
            exec_hash_del(&res->hash_node);
            resource_free(res);
        }
    }
    exec_unlock(&vms_res_hash_lock);
}

/* ================================================================
 * DLM resource directory + mastering -- the DIRECTORY RESOLVER (FC-P4.3)
 *
 * On OpenVMS the lock database is distributed: every resource tree is MASTERED
 * on one node, and to find the master a node routes a lookup to the tree's
 * DIRECTORY NODE, which holds the name->master mapping. The three outcomes are
 * the published ones (Davis p. 6-31; docs/research-dlm-directory-algorithm.md):
 *
 *   (1) the directory node is also the master -> it resolves the request;
 *   (2) it knows the master -> it answers with the master's CSID;
 *   (3) it knows no master -> it tells the requester to become the master.
 *       And if THIS node is the directory, absence of the root in its own
 *       database means nobody masters it and it assumes mastery (p. 6-31).
 *
 * WHICH NODE IS THE DIRECTORY is `ldwv[hash16 mod n]` -- the Lock Directory
 * Weight Vector the connection manager rebuilds at every state transition
 * (vms_dlm_ldwv.h), indexed by the resource name's 16-bit hash. The engine
 * reaches the vector through the injected `dir_resolve` op (vms_dlm_proxy.h),
 * so the lock manager holds no copy of the cluster's membership and there is
 * no second place for it to drift.
 *
 * THE HASH IS NEVER COMPUTED HERE OR ANYWHERE ELSE.
 *
 * The predecessor of this code was `exec_jhash(name) % n` over a static insmod
 * vector -- an OVMX hash standing in for VMS's, which is a different function.
 * Pointing it at a real cluster caused a reformation (commit 90b3bbbd), and the
 * same class of error with a placeholder hash of 0 produced the campaign's
 * 35-per-second grant storm: a lookup carrying the wrong hash makes the
 * directory node scan the wrong chain, miss the name, and take outcome (3) --
 * silently installing the SENDER as master of a resource somebody else already
 * masters (memory cluster-promotion-gap; research note SS3).
 *
 * So the hash is taken off the wire and only off the wire (p. 6-50: every
 * directory lookup carries the sender's own 16-bit hash, and the directory node
 * uses the received value). `res->hash16`/`res->hash_known` are written by
 * vms_lock_dlm_learn_dir_hash() from a parsed cat-0x02 frame, and a resource
 * with no learned hash is NOT looked up: the enqueue returns SS$_UNSUPPORTED,
 * an honest refusal that costs this node locking on root names it is the first
 * in the cluster to touch (its own private volumes and files) and costs it
 * nothing else -- membership, directory duty, mastering and every shared name
 * are unaffected (design SS3.6, rung A'; the residual's resolution is rung B/C).
 * ================================================================ */

/*
 * This node's CSID (vms_local_csid). 0 is reserved for "unmastered"
 * (struct vms_lock_resource.master_csid), so the default is a non-zero OVMX
 * local placeholder. The VARIABLE and its insmod module parameter live in the
 * per-substrate rind (src/kernel/vms_module.c, src/kernel-netbsd/vms_netbsd.c)
 * -- module parameters are a host-module-lifecycle concern, not portable
 * executive logic (design record SS4). This core facility reads it through the
 * extern declaration in vms_internal.h. It is NOT a claim of a VMS-authentic
 * CSID value or layout (CLAUDE.md Rule 8) -- real CSIDs are assigned by the
 * connection manager at join, and re-pointing this at the CLUB's LEARNED local
 * CSID is FC-P4.8's glue.
 */

/*
 * How many learned directory hashes disagreed with a value already held for
 * the same resource name. See vms_lock_dlm_learn_dir_hash(): the first value
 * stands and the disagreement is counted, because a rising count falsifies
 * either the body[10:12] offset (INFERRED until FC-P4.2) or the "one hash per
 * name, cluster-wide" property the whole scheme rests on. Written under
 * vms_res_hash_lock.
 */
static uint32_t vms_dlm_dir_hash_conflicts;

uint32_t vms_lock_dlm_dir_hash_conflicts(void)
{
    uint32_t n;

    exec_lock(&vms_res_hash_lock);
    n = vms_dlm_dir_hash_conflicts;
    exec_unlock(&vms_res_hash_lock);
    return n;
}

/*
 * dir_hash_store - record one wire-learned hash on a resource. Caller holds
 * res->lock. Returns SS$_NORMAL when the value is now held, SS$_BADPARAM when a
 * DIFFERENT value was already learned for this name (the caller counts it).
 */
static uint32_t dir_hash_store(struct vms_lock_resource *res, uint16_t hash16)
{
    if (res->hash_known) {
        if (res->hash16 == hash16)
            return SS__NORMAL;
        /* The HELD value stands -- routing must not churn on a disagreement.
         * SS$_BADPARAM says "that is not the value this executive holds for
         * that name"; the caller's own counter is what makes it evidence. */
        return SS__BADPARAM;
    }
    res->hash16 = hash16;
    res->hash_known = 1;
    /* A hash learned against the OLD vector says nothing about the new one;
     * the generation check in dir_resolve() re-runs the lookup anyway, but
     * clearing here keeps "cached" and "learned" from ever being confused. */
    res->dir_valid = 0;
    return SS__NORMAL;
}

uint32_t vms_lock_dlm_learn_dir_hash(const char *resnam, uint16_t hash16)
{
    struct vms_lock_resource *res;
    uint32_t st;

    if (resnam == NULL || resnam[0] == '\0')
        return SS__BADPARAM;

    res = resource_find_or_create(resnam);
    if (res == NULL)
        return SS__INSFMEM;

    exec_lock(&res->lock);
    st = dir_hash_store(res, hash16);
    exec_unlock(&res->lock);

    if (st != SS__NORMAL) {
        exec_lock(&vms_res_hash_lock);
        vms_dlm_dir_hash_conflicts++;
        exec_unlock(&vms_res_hash_lock);
    }

    /* Drop the reference resource_find_or_create took. A resource carrying a
     * learned hash survives the release (resource_release's preservation rule):
     * throwing the value away here would guarantee a refusal the next time this
     * node touches the name. */
    resource_release(res);
    return st;
}

/*
 * dir_resolve - which node is the DIRECTORY for this resource's root name.
 *
 * THE Rule-8 BOUNDARY, in one function. It computes nothing: it hands the
 * cluster's own hash for this name to the cluster's own directory vector.
 * Caller holds res->lock.
 *
 *   no resolver installed  -> *out_csid = 0 (this node), SS$_NORMAL. A node
 *                             with no cluster stack is alone: it is trivially
 *                             the directory and the master for everything, and
 *                             single-node locking is untouched.
 *   no wire-learned hash   -> SS$_UNSUPPORTED. Never a computed hash, never a
 *                             probe, never a placeholder (the whole point).
 *   vector unusable        -> SS$_UNSUPPORTED (mid-transition, or the vector
 *                             was refused; vms_dlm_ldwv.h SS3).
 *   otherwise              -> the vector's answer; 0 means THIS node.
 *
 * The answer is cached in the RSB together with the vector GENERATION it came
 * from, so it is re-resolved the instant the vector changes -- which is what
 * "every rsb->dir_csid is invalidated on a vector change" means here, enforced
 * by construction rather than by remembering to walk the database (Davis
 * p. 6-33; vms_dlm_proxy.h `dir_generation`).
 */
/*
 * Is this node in a cluster at all, as far as the DIRECTORY is concerned? With
 * no resolver installed there is no vector and no other member: this node is
 * the directory and the master for every name, which is a real answer and not
 * a computed one.
 */
static int dlm_directory_installed(void)
{
    struct vms_dlm_requester_ops ops = dlm_req_ops_get();

    return ops.dir_resolve != NULL;
}

static uint32_t dir_resolve(struct vms_lock_resource *res, uint32_t *out_csid)
{
    struct vms_dlm_requester_ops ops = dlm_req_ops_get();
    uint32_t gen, csid = 0;

    *out_csid = 0;
    if (ops.dir_resolve == NULL)
        return SS__NORMAL;                 /* cluster of one: nothing to resolve */

    if (!res->hash_known)
        return SS__UNSUPPORTED;            /* INV-6: wire-learned or nothing */

    gen = (ops.dir_generation != NULL) ? ops.dir_generation(ops.ctx) : 0u;
    if (res->dir_valid && res->dir_gen == gen) {
        *out_csid = res->dir_csid;
        return SS__NORMAL;
    }

    if (ops.dir_resolve(ops.ctx, res->hash16, &csid) != SS__NORMAL)
        return SS__UNSUPPORTED;

    res->dir_csid = csid;                  /* 0 == this node (p. 6-32) */
    res->dir_gen = gen;
    res->dir_valid = 1;
    *out_csid = csid;
    return SS__NORMAL;
}

/* Where a $ENQ for this resource must be served (FC-P4.4). */
enum dlm_route {
    DLM_ROUTE_LOCAL  = 0,   /* this node masters it: the local engine serves */
    DLM_ROUTE_REMOTE = 1    /* mastered elsewhere: a PROXY LKB + a posted request */
};

/*
 * dlm_resolve_master - resolve the directory + master for a resource, mastering
 * it locally on first use.
 *
 * Called with res->lock held. The published three-outcome model (Davis
 * pp. 6-31/6-32; IDSM lock management):
 *
 *   known master, and it is us      -> LOCAL   (nothing to resolve, p. 6-32:
 *                                      one lookup per tree while we hold a lock)
 *   known master, and it is not us  -> REMOTE, addressed to THE MASTER
 *   unknown master, we are the      -> LOCAL, and we master it on first use
 *     directory node                  ("simply assumes mastery", p. 6-31)
 *   unknown master, someone else is -> REMOTE, addressed to THE DIRECTORY NODE,
 *     the directory node               which answers with the master, or with
 *                                      "you master it" (p. 6-31 outcomes a/b/c)
 *   unknown master, and the directory  -> REFUSED, SS$_UNSUPPORTED. In a
 *     cannot be resolved                 cluster that means this node has no
 *                                        wire-learned hash for this root name
 *                                        (FC-P4.3: never computed, never
 *                                        guessed) or the vector is not
 *                                        authoritative right now (a transition
 *                                        is in flight). An honest refusal; the
 *                                        caller returns it to $ENQ unchanged.
 *
 * `inbound` is nonzero when this is a request that ARRIVED from another node
 * rather than a local $ENQ; see the comment at the refusal below for the one
 * row it changes.
 *
 * *dst_csid receives the node the request must be addressed to on a REMOTE
 * route. Never a guess: it is either a master CSID the cluster told us
 * (grant_recv stores it) or the directory CSID the cluster's own weight vector
 * named for the cluster's own hash of this name.
 *
 * ROOT NAMES ONLY, and the engine does not yet know the difference. Only a
 * tree's ROOT is looked up: the whole tree is mastered where its root is, and
 * the master's CSID propagates down every branch (p. 6-31/6-32). The RSB model
 * here carries no parent link (the tree lives on the LKBs' parent_id), so every
 * resource is treated as a root. That over-counts lookups for a sub-resource;
 * it never mis-routes one, because a sub-resource's own hash resolves through
 * the same vector. Binding a child RSB to its root's master belongs with the
 * tree/rebuild work (FC-P4.6/FC-P5.5) and is recorded here so it is not lost.
 */
/* Route to a master this node already knows, or say there is none yet.
 * Returns 1 when the answer is settled (the two "known master" rows above). */
static int dlm_route_known_master(struct vms_lock_resource *res,
                                  enum dlm_route *route, uint32_t *dst_csid)
{
    if (res->master_csid == 0)
        return 0;
    if (res->master_csid == vms_local_csid)
        return 1;                            /* LOCAL: we master it */
    *route = DLM_ROUTE_REMOTE;               /* straight to the known master */
    *dst_csid = res->master_csid;
    return 1;
}

static uint32_t dlm_resolve_master(struct vms_lock_resource *res, int inbound,
                                   enum dlm_route *route, uint32_t *dst_csid)
{
    uint32_t dir = 0, st;

    *route = DLM_ROUTE_LOCAL;
    *dst_csid = 0;

    if (dlm_route_known_master(res, route, dst_csid))
        return SS__NORMAL;

    st = dir_resolve(res, &dir);
    if (st != SS__NORMAL) {
        /*
         * WHY AN INBOUND REQUEST DOES NOT INHERIT THIS REFUSAL. A cross-node
         * request arrived HERE because the SENDER resolved the directory and
         * the cluster addressed it at this node -- that routing decision is a
         * fact this node RECEIVED, and re-deriving it is not required to honour
         * it. Refusing because we cannot independently reproduce a decision the
         * cluster already made would break locking on exactly the shared names
         * the cluster is talking to us about, and it would refuse a resource we
         * are in fact about to master (p. 6-31 outcome 3, "the requester
         * becomes the master"). The REQUESTER side is where the refusal has to
         * bite -- it is the side that would otherwise put a guessed hash on the
         * wire -- and it does, below.
         */
        if (!inbound)
            return st;
    } else if (dir != 0 && dir != vms_local_csid) {
        *route = DLM_ROUTE_REMOTE;           /* the lookup goes to the directory */
        *dst_csid = dir;
        return SS__NORMAL;
    }

    /* We are the directory and nobody masters it yet: master it here
     * ("simply assumes mastery", p. 6-31). `dir == 0` is the vector's own way
     * of saying "your entry" (p. 6-32); `dir == vms_local_csid` is the same
     * fact reached through a resolver that names us explicitly. */
    res->master_csid = vms_local_csid;
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

    /* No astadr, a sync waiter (woken directly instead), or a lock owned by no
     * local process -- a reconstructed PROXY LKB, which is the NODE's record of
     * a cluster-held lock and has no process to deliver to (FC-P4.4). */
    if (!lock->astadr || (lock->flags & LCK_M_SYNC) || !lock->proc)
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
 * Process lock cleanup -- and the RUNDOWN RELEASE the master must hear about
 * ================================================================
 *
 * E6, CLOSED BY FC-P4.6. A proxy LKB released through $DEQ tells the master
 * (deq_proxy_release, which posts from process context outside every lock). A
 * proxy torn down by PROCESS RUNDOWN used to tell nobody, because all three
 * teardown call sites hold proc->lock_list_lock -- a spinlock on the Linux
 * substrate -- and `post` is the cluster's implementation, not this file's, so
 * calling it there would push arbitrary code into atomic context.
 *
 * The fix is not to make `post` atomic-safe (that would constrain the wire arm
 * forever); it is to POST FROM A CONTEXT THAT MAY BLOCK. Teardown COLLECTS the
 * release -- filled by dlm_proxy_fill_post from the real LKB, at the moment the
 * LKB still exists, so every field is genuine executive state (INV-6) -- into a
 * small fixed batch on the caller's stack. The sweep then DROPS
 * proc->lock_list_lock and posts the batch, and repeats until no matching lock
 * is left. A full batch simply ends that pass, so nothing is ever dropped for
 * want of room and the bound on stack use is a constant.
 *
 * A failed post is reported by the requester path, not swallowed here: the
 * master may still believe it holds the lock, and reconciling that is the
 * departure/rebuild machinery's job (FC-P5.5), exactly as $DEQ already
 * documents.
 */

/* How many rundown releases one pass collects. An OVMX design value: small
 * enough that the batch is a few hundred bytes on a VAX kernel stack, and the
 * sweep loops, so it bounds stack use without bounding correctness. */
#define LOCK_RELEASE_BATCH 4u

struct dlm_release_batch {
    uint32_t                  n;
    struct vms_dlm_proxy_post p[LOCK_RELEASE_BATCH];
};

/* Post every collected release. Called with NO lock held (see above). */
static void dlm_release_batch_post(struct dlm_release_batch *b)
{
    uint32_t i;

    for (i = 0; i < b->n; i++)
        (void)dlm_proxy_post(&b->p[i]);
    b->n = 0;
}

/*
 * Collect the master release for a PROXY LKB about to be torn down. Caller
 * holds res->lock, so the fields are read out of the live LKB. Returns 0 when
 * there was no room -- and then the caller must NOT tear the lock down in this
 * pass, so the release is deferred, never lost.
 */
static int dlm_release_collect_locked(struct dlm_release_batch *b,
                                      struct vms_lock_entry *lock,
                                      struct vms_lock_resource *res)
{
    uint32_t dst;

    if (b == NULL)
        return 1;                 /* caller does not want releases collected */
    if (b->n >= LOCK_RELEASE_BATCH)
        return 0;

    dst = lock->master_csid ? lock->master_csid : res->master_csid;
    dlm_proxy_fill_post(lock, res, VMS_DLM_POST_DEQ, dst, &b->p[b->n]);
    b->n++;
    return 1;
}

/*
 * Tear down one lock and free it. Caller holds proc->lock_list_lock and has
 * already unlinked (or is about to unlink) the entry from proc->locks via the
 * exec_list_for_each_entry_safe cursor. Shared verbatim by full process teardown
 * (vms_proc_release_locks) and image rundown (vms_proc_rundown_locks) so the
 * two cannot drift.
 *
 * `b` collects the master release for a proxy LKB (see above); pass NULL to
 * tear down without one. Returns 0 -- WITHOUT tearing anything down -- when the
 * batch is full, which is how the sweep knows to drain it and come back.
 */
static int lock_teardown_locked(struct vms_lock_entry *lock,
                                struct dlm_release_batch *b)
{
    struct vms_lock_resource *res = lock->resource;

    exec_lock(&res->lock);
    /* The release is read off the LKB while the LKB is still real. */
    if (lock->proxy && !dlm_release_collect_locked(b, lock, res)) {
        exec_unlock(&res->lock);
        return 0;
    }
    lock_unlink_from_res(lock);

    /* Write back value block if held. NOT for a proxy: the master owns that
     * resource's value block, and writing ours into the local RSB would invent
     * a value no node ever agreed on (FC-P4.4). */
    if (!lock->proxy && (lock->flags & LCK_M_VALBLK) && !lock->waiting)
        memcpy(res->valblk, lock->valblk, LCK_VALBLK_SIZE);

    try_grant_waiters(res);
    exec_unlock(&res->lock);

    /* Remove from process list and ID tree */
    exec_list_del(&lock->proc_list);
    lock_remove_id(lock);

    resource_release(res);
    lock_entry_free(lock);
    return 1;
}

/*
 * ONE SWEEP, described by a filter, run in passes so the master releases it
 * collects are posted from a context that may block (E6, see above).
 *
 * `min_acmode`/`use_acmode` skip inner-mode (process-permanent) locks;
 * `parent_lkid` selects only sublocks of one parent. A pass ends when the
 * batch fills, and `*more` says whether to come back.
 */
struct lock_sweep {
    uint8_t  min_acmode;
    uint8_t  use_acmode;
    uint32_t parent_lkid;   /* 0 == every lock */
};

static int lock_sweep_matches(const struct vms_lock_entry *lock,
                              const struct lock_sweep *s)
{
    if (s->use_acmode && lock->acmode < s->min_acmode)
        return 0;
    if (s->parent_lkid != 0 && lock->parent_id != s->parent_lkid)
        return 0;
    return 1;
}

/* Returns nonzero when matching locks may remain (the batch filled). */
static int lock_sweep_pass(struct vms_proc *proc, const struct lock_sweep *s,
                           struct dlm_release_batch *b)
{
    struct vms_lock_entry *lock, *tmp;
    int more = 0;

    exec_lock(&proc->lock_list_lock);
    exec_list_for_each_entry_safe(lock, tmp, &proc->locks, proc_list) {
        if (!lock_sweep_matches(lock, s))
            continue;
        if (!lock_teardown_locked(lock, b)) {
            more = 1;    /* batch full: drain it and come back for this one */
            break;
        }
        if (proc->lock_count > 0)
            proc->lock_count--;
    }
    exec_unlock(&proc->lock_list_lock);
    return more;
}

static void lock_sweep_run(struct vms_proc *proc, const struct lock_sweep *s)
{
    struct dlm_release_batch b;
    int more;

    b.n = 0;
    do {
        more = lock_sweep_pass(proc, s, &b);
        /* OUTSIDE proc->lock_list_lock: this is the blockable context E6 asks
         * the release to be posted from. */
        dlm_release_batch_post(&b);
    } while (more);
}

void vms_proc_release_locks(struct vms_proc *proc)
{
    struct lock_sweep s;

    memset(&s, 0, sizeof(s));
    lock_sweep_run(proc, &s);

    exec_lock(&proc->lock_list_lock);
    proc->lock_count = 0;
    exec_unlock(&proc->lock_list_lock);
}

/*
 * release_child_locks - the parent-child AUTO-RELEASE CASCADE (vms-0dd / vms-489):
 * releasing a lock releases its child SUBLOCKS. RMS record locks carry their file-
 * access lock as parent (parent_id), so a $CLOSE that $DEQs the file lock releases
 * every record lock still held under it -- WITHOUT this, a record lock outlives its
 * file (the userspace RAB that held the lkid was re-initialized on reopen and lost
 * it), and the leaked lock then blocks the file's own re-read (the real correctness
 * bug test_syssvc_dcl_acp's copy-reopen-reread exercises). A child belongs to the
 * SAME process as its parent (RMS holds file + record locks in one process), so the
 * sweep is over proc->locks, mirroring vms_proc_release_locks' safe iteration. One
 * level is enough for record-under-file, but children are released depth-first so a
 * deeper tree unwinds too. Every release is a REAL teardown, never a fabricated
 * clear (INV-6).
 */
static void release_child_locks(struct vms_proc *proc, uint32_t parent_lkid)
{
    struct lock_sweep s;

    if (parent_lkid == 0)
        return;

    memset(&s, 0, sizeof(s));
    s.parent_lkid = parent_lkid;
    lock_sweep_run(proc, &s);
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
    struct lock_sweep s;

    memset(&s, 0, sizeof(s));
    s.min_acmode = min_acmode;
    s.use_acmode = 1;
    lock_sweep_run(proc, &s);
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
 *   anything else- a terminal status another executive path wrote into
 *                  grant_state. Today that is a PROXY LKB whose master left the
 *                  cluster (FC-P4.4, vms_ioctl_dlm_member_depart): the request
 *                  cannot be answered by a node that is gone, so the wait ends
 *                  honestly instead of hanging. Callers unwind on any non-NORMAL.
 *
 * A PROXY LKB waits here too, and the wait is the same wait -- but the wake can
 * only come from the MASTER (vms_lock_dlm_xnode_grant_recv), never from a local
 * grant, and the bounded deadlock re-scan is SKIPPED for it: the local
 * wait-for graph does not contain the cluster's holders, so running it on a
 * proxy would invent a cycle. Distributed deadlock search is its own mechanism
 * (rung H11 / FC-P5.6).
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
        /* Predicate has priority (cv contract). Any nonzero grant_state is a
         * terminal answer: SS$_NORMAL granted, SS$_DEADLOCK a cycle, anything
         * else a status another executive path completed this request with. */
        if (lock->grant_state != 0) {
            status = lock->grant_state;
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
        if (timed_out && !lock->proxy && lock->waiting &&
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
 * PROXY LKB lifecycle (FC-P4.4)
 *
 * Six short functions, one job each: find, fill a post from real state, post,
 * allocate, unwind, and the $ENQ entry that ties them together. Nothing here
 * decides a lock's fate -- the MASTER does that, and its answer arrives at
 * vms_lock_dlm_xnode_grant_recv.
 * ================================================================ */

/*
 * dlm_proxy_find - the proxy LKB a cluster message names, by (req_csid,
 * req_lkid). req_lkid is a lock id of THIS node, so the lock-ID database IS the
 * index: one lookup, and a duplicate message can only find the one lock it
 * names. Returns NULL (holding no reference) when the id names no lock or names
 * something that is not a proxy -- a peer must never be able to reach this
 * node's master-copy or local locks through a requester-side handle.
 *
 * Takes a reference via lock_find_by_id; the caller drops it with lock_put.
 */
static struct vms_lock_entry *dlm_proxy_find(uint32_t req_lkid)
{
    struct vms_lock_entry *lock = lock_find_by_id(req_lkid);

    if (lock == NULL)
        return NULL;
    if (!lock->proxy) {
        lock_put(lock);
        return NULL;
    }
    return lock;
}

/*
 * dlm_proxy_alloc - a proxy LKB for `res`, owned by `proc` (NULL when the
 * cluster, not a local process, is what put it here). Granted mode NL: only the
 * master can raise it. `lkid_from_wire` is 0 for a request this node
 * originates (the id is minted here) or the id a master's message named.
 * Returns NULL if the allocation failed or the named id is already taken.
 */
static struct vms_lock_entry *dlm_proxy_alloc(struct vms_proc *proc,
                                              struct vms_lock_resource *res,
                                              const struct vms_enq_args *a,
                                              uint32_t lkid_from_wire)
{
    struct vms_lock_entry *lock = exec_zalloc(sizeof(struct vms_lock_entry));

    if (!lock)
        return NULL;

    lock->refcount       = 1;
    lock->proxy          = 1;
    lock->granted_mode   = LCK_K_NLMODE;   /* only the MASTER raises this */
    lock->requested_mode = a->lkmode;
    lock->flags          = a->flags;
    lock->astadr         = a->astadr;
    lock->astprm         = a->astprm;
    lock->blkastadr      = a->blkastadr;
    lock->resource       = res;
    lock->proc           = proc;
    lock->waiting        = 1;              /* pending AT THE MASTER */
    lock->grant_state    = 0;
    lock->req_csid       = vms_local_csid; /* we are the requester */
    lock->parent_id      = a->parid;
    exec_cv_init(&lock->wait_wq);
    if (a->flags & LCK_M_VALBLK)
        memcpy(lock->valblk, a->valblk, LCK_VALBLK_SIZE);

    if (lkid_from_wire != 0) {
        if (lock_insert_id_at(lock, lkid_from_wire) != 0) {
            lock_entry_free(lock);
            return NULL;
        }
    } else {
        lock_insert_id(lock);
    }
    return lock;
}

/* Link a fresh proxy onto its resource's proxy queue and, if a local process
 * owns it, onto that process's lock list. */
static void dlm_proxy_link(struct vms_proc *proc, struct vms_lock_resource *res,
                           struct vms_lock_entry *lock)
{
    exec_lock(&res->lock);
    exec_list_add_tail(&lock->res_proxy, &res->proxies);
    exec_unlock(&res->lock);

    if (proc) {
        exec_lock(&proc->lock_list_lock);
        exec_list_add_tail(&lock->proc_list, &proc->locks);
        proc->lock_count++;
        exec_unlock(&proc->lock_list_lock);
    }
}

/* Undo dlm_proxy_link + dlm_proxy_alloc. Drops the resource reference the
 * caller took, so the caller must not release it again. */
static void dlm_proxy_unwind(struct vms_proc *proc, struct vms_lock_resource *res,
                             struct vms_lock_entry *lock)
{
    if (proc) {
        exec_lock(&proc->lock_list_lock);
        exec_list_del(&lock->proc_list);
        if (proc->lock_count > 0)
            proc->lock_count--;
        exec_unlock(&proc->lock_list_lock);
    }
    exec_lock(&res->lock);
    exec_list_del(&lock->res_proxy);
    exec_unlock(&res->lock);
    lock_remove_id(lock);
    resource_release(res);
    lock_entry_free(lock);
}

/*
 * enq_proxy_request - the $ENQ path for a resource this node does NOT master
 * (FC-P4.4; design SS3.6 D-DLM-4, "outbound requests originate from a real proxy
 * LKB in waiting").
 *
 * Create the proxy, post the request to `dst_csid`, then let the caller SLEEP ON
 * THE LKB's condition variable exactly as a local $ENQW sleeps -- the wake comes
 * from the master's grant. An async $ENQ returns with the request outstanding
 * and gets its completion AST when the grant lands.
 *
 * `res` carries one reference from resource_find_or_create; it is handed to the
 * proxy on success and released on every failure path.
 */
static void enq_proxy_request(struct vms_proc *proc, struct vms_enq_args *a,
                              struct vms_lock_resource *res, uint32_t dst_csid)
{
    struct vms_dlm_proxy_post post;
    struct vms_lock_entry *lock;
    uint32_t st;

    /* No cluster arm installed: this node cannot ask, so it says so. It must
     * NOT fall back to granting locally -- that is the fabrication INV-6 and
     * Rule 9 forbid, and it is what makes two masters for one tree. */
    if (dlm_req_ops_get().post == NULL) {
        resource_release(res);
        a->status = SS__UNSUPPORTED;
        return;
    }

    lock = dlm_proxy_alloc(proc, res, a, 0);
    if (!lock) {
        resource_release(res);
        a->status = SS__INSFMEM;
        return;
    }
    dlm_proxy_link(proc, res, lock);

    exec_lock(&res->lock);
    dlm_proxy_fill_post(lock, res, VMS_DLM_POST_ENQ, dst_csid, &post);
    exec_unlock(&res->lock);

    st = dlm_proxy_post(&post);
    if (st != SS__NORMAL) {
        dlm_proxy_unwind(proc, res, lock);
        a->lkid = 0;
        a->status = st;                 /* honest: the request never left */
        return;
    }

    if (a->flags & LCK_M_SYNC) {
        st = enq_wait_sync(res, lock);  /* the master's grant wakes us */
        if (st != SS__NORMAL) {
            dlm_proxy_unwind(proc, res, lock);
            a->lkid = 0;
            a->status = st;
            return;
        }
        a->lkid = lock->lkid;
        a->lk_status = lock->granted_mode;
        if (a->flags & LCK_M_VALBLK)
            memcpy(a->valblk, lock->valblk, LCK_VALBLK_SIZE);
        a->status = SS__NORMAL;
        return;
    }

    /* Async: the request is outstanding at the master. Report the handle and
     * the mode ASKED for -- granted_mode stays NL until the master answers. */
    a->lkid = lock->lkid;
    a->lk_status = lock->requested_mode;
    a->status = SS__NORMAL;
}

/*
 * convert_proxy_request - $ENQ/LCK$M_CONVERT on a PROXY LKB (FC-P4.4).
 *
 * The lock is granted at the master, so the conversion is a request TO the
 * master carrying the new mode, built from this lock block. The caller waits on
 * the same LKB (sync) or is told the request is outstanding (async). A failed
 * post restores the lock to the mode it is still genuinely granted at -- VMS
 * semantics: a failed conversion never loses the held lock.
 *
 * Caller holds a lock_find_by_id reference on `lock` and drops it afterwards.
 */
static uint32_t convert_proxy_request(struct vms_proc *proc,
                                      struct vms_enq_args *a,
                                      struct vms_lock_entry *lock)
{
    struct vms_lock_resource *res = lock->resource;
    struct vms_dlm_proxy_post post;
    uint32_t held_mode, dst, st;

    (void)proc;

    exec_lock(&res->lock);
    held_mode = lock->granted_mode;
    if (a->blkastadr)
        lock->blkastadr = a->blkastadr;
    if (a->flags & LCK_M_VALBLK)
        memcpy(lock->valblk, a->valblk, LCK_VALBLK_SIZE);
    lock->requested_mode = a->lkmode;
    lock->flags = a->flags;
    lock->waiting = 1;            /* pending AT THE MASTER, again */
    lock->grant_state = 0;
    dst = lock->master_csid ? lock->master_csid : res->master_csid;
    dlm_proxy_fill_post(lock, res, VMS_DLM_POST_CONVERT, dst, &post);
    exec_unlock(&res->lock);

    st = dlm_proxy_post(&post);
    if (st != SS__NORMAL) {
        exec_lock(&res->lock);
        lock->requested_mode = held_mode;   /* still held at the old mode */
        lock->waiting = 0;
        exec_unlock(&res->lock);
        return st;
    }

    if (a->flags & LCK_M_SYNC) {
        st = enq_wait_sync(res, lock);
        if (st != SS__NORMAL) {
            exec_lock(&res->lock);
            lock->requested_mode = lock->granted_mode;
            lock->waiting = 0;
            exec_unlock(&res->lock);
            return st;
        }
        a->lk_status = lock->granted_mode;
        if (a->flags & LCK_M_VALBLK)
            memcpy(a->valblk, lock->valblk, LCK_VALBLK_SIZE);
        return SS__NORMAL;
    }

    a->lk_status = lock->requested_mode;
    return SS__NORMAL;
}

/* ================================================================
 * What the DLM's WIRE ARM may ask this engine for (FC-P4.6)
 * ================================================================
 *
 * Three entry points, declared in vms_dlm_proxy.h with the reasoning. All
 * three take res->lock briefly and none of them sleeps, waits or allocates,
 * so the cluster fork thread may call them (design SS3.2.6, E42/E45).
 */

/*
 * vms_lock_dlm_proxy_refill_post - re-read a proxy LKB into a fresh post.
 *
 * THE ANTI-LARP PRIMITIVE. Every transmission the requester FSM makes -- first
 * send, retransmit, retry at a new target, and above all the COMPLETION that
 * names the master's handle -- is built from a call to this, so no wire field
 * is ever a value remembered off an earlier frame. It is the same
 * dlm_proxy_fill_post() the original post used: one filler, one set of fields,
 * one moment of truth.
 */
uint32_t vms_lock_dlm_proxy_refill_post(uint32_t req_lkid, uint32_t op,
                                        uint32_t dst_csid,
                                        struct vms_dlm_proxy_post *out)
{
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;

    if (out == NULL || req_lkid == VMS_DLM_LKID_UNSET)
        return SS__BADPARAM;

    lock = dlm_proxy_find(req_lkid);
    if (lock == NULL)
        return SS__IVLOCKID;   /* no proxy of ours by that handle -- honest */

    res = lock->resource;
    exec_lock(&res->lock);
    dlm_proxy_fill_post(lock, res, op, dst_csid, out);
    exec_unlock(&res->lock);
    lock_put(lock);
    return SS__NORMAL;
}

/*
 * vms_lock_dlm_proxy_fail - end a PENDING proxy's wait with a real status.
 *
 * The mechanism the departure path already uses (dlm_proxies_master_departed):
 * grant_state under res->lock, then broadcast, so a $ENQW asleep in
 * enq_wait_sync wakes and returns `status`. A GRANTED proxy is left alone -- a
 * real lock is not something a timeout may take away.
 */
uint32_t vms_lock_dlm_proxy_fail(uint32_t req_lkid, uint32_t status)
{
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;

    if (req_lkid == VMS_DLM_LKID_UNSET || status == SS__NORMAL)
        return SS__BADPARAM;   /* "failed with success" is not a status */

    lock = dlm_proxy_find(req_lkid);
    if (lock == NULL)
        return SS__IVLOCKID;

    res = lock->resource;
    exec_lock(&res->lock);
    if (lock->waiting && lock->grant_state == 0) {
        lock->grant_state = status;
        exec_cv_broadcast(&lock->wait_wq);
    }
    exec_unlock(&res->lock);
    lock_put(lock);
    return SS__NORMAL;
}

/*
 * dlm_promote_proxy_locked - turn a proxy LKB into a real local lock.
 * Caller holds res->lock and has already recorded this node as the master.
 *
 * The proxy comes off res->proxies and goes onto res->waiting, which is the
 * ONE act that makes it visible to the local granting algorithm (the split
 * queue is why the executive is structurally unable to grant a lock the
 * cluster masters elsewhere -- see the PROXY LKB comment at the top of this
 * file). try_grant_waiters() then decides it exactly as it decides every local
 * $ENQ: granted if the local queues allow, genuinely queued if they do not.
 */
static void dlm_promote_proxy_locked(struct vms_lock_resource *res,
                                     struct vms_lock_entry *lock)
{
    exec_list_del(&lock->res_proxy);
    lock->proxy       = 0;
    lock->master_csid = vms_local_csid;   /* we master it now, for real */
    lock->master_lkid = VMS_DLM_LKID_UNSET; /* no remote handle exists */
    lock->waiting     = 1;
    lock->grant_state = 0;
    exec_list_add_tail(&lock->res_waiting, &res->waiting);
    try_grant_waiters(res);
}

/*
 * vms_lock_dlm_assume_mastery - Davis p. 6-31 OUTCOME 3, from the wire.
 *
 * The directory node answered "no master -- you master it". Record the mastery
 * and promote the outstanding proxy, so the requester's own $ENQW completes
 * from a genuine local grant rather than from a message this node invented.
 */
uint32_t vms_lock_dlm_assume_mastery(const char *resnam, uint32_t req_lkid)
{
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;
    uint32_t st = SS__NORMAL;

    if (resnam == NULL || resnam[0] == '\0' || req_lkid == VMS_DLM_LKID_UNSET)
        return SS__BADPARAM;

    lock = dlm_proxy_find(req_lkid);
    if (lock == NULL)
        return SS__IVLOCKID;

    res = lock->resource;
    exec_lock(&res->lock);
    /* The answer must be about the lock it names. Acting on a directory reply
     * for one resource against a proxy on another would master the wrong
     * tree -- the shape of error that produces two masters. */
    if (strncmp(res->name, resnam, sizeof(res->name)) != 0) {
        st = SS__BADPARAM;
    } else {
        res->master_csid = vms_local_csid;
        res->dir_valid = 0;          /* the routing question is settled now */
        dlm_promote_proxy_locked(res, lock);
    }
    exec_unlock(&res->lock);
    lock_put(lock);
    return st;
}

/* The two engine receive paths the kernel-core-typed doors below translate
 * onto. Defined further down beside the rest of the cross-node receive
 * handlers; declared here because the doors are part of the FC-P4.6 seam
 * block and belong beside the other three. */
static uint32_t vms_lock_dlm_xnode_grant_recv(struct vms_dlm_xnode_args *req);
static uint32_t vms_lock_dlm_xnode_blkast_recv(struct vms_proc *proc,
                                               struct vms_dlm_xnode_args *req);

/*
 * vms_lock_dlm_record_master - Davis p. 6-31 OUTCOME 2, from the wire.
 *
 * The directory node named the master. Store it on the proxy LKB and on the
 * RSB, and settle the routing question -- see vms_dlm_proxy.h for why the wire
 * arm must put the answer HERE and then re-read it, rather than remembering it.
 * Grants nothing and wakes nobody: a routing fact is not a completion.
 */
uint32_t vms_lock_dlm_record_master(const char *resnam, uint32_t req_lkid,
                                    uint32_t master_csid)
{
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;
    uint32_t st = SS__NORMAL;

    if (resnam == NULL || resnam[0] == '\0' ||
        req_lkid == VMS_DLM_LKID_UNSET || master_csid == 0)
        return SS__BADPARAM;   /* 0 IS "unmastered" here, so it is no answer */

    lock = dlm_proxy_find(req_lkid);
    if (lock == NULL)
        return SS__IVLOCKID;

    res = lock->resource;
    exec_lock(&res->lock);
    if (strncmp(res->name, resnam, sizeof(res->name)) != 0) {
        st = SS__BADPARAM;     /* an answer about some other lock */
    } else {
        lock->master_csid = master_csid;
        res->master_csid  = master_csid;
        res->dir_valid    = 0; /* the lookup is answered; nothing cached */
    }
    exec_unlock(&res->lock);
    lock_put(lock);
    return st;
}

/*
 * vms_lock_dlm_proxy_grant_recv - the kernel-core-typed door onto the engine's
 * requester-side grant receive (vms_dlm_proxy.h).
 *
 * One translation and one honesty rule, and nothing else: this function holds
 * no policy that vms_lock_dlm_xnode_grant_recv does not already hold.
 *
 * THE HONESTY RULE IS `valblk_present`. The engine's grant receive copies the
 * grant's value block onto the proxy whenever the granted mode is real. No
 * cat-0x02 LVB field is grounded, so a wire arm has nothing to put there -- and
 * a zero-filled block copied onto a proxy that holds a real value would destroy
 * data and call it a grant. When the wire carried none, the proxy's OWN current
 * bytes are what goes into the args, so the engine's copy is a no-op and the
 * value block is left exactly as it was.
 */
static void dlm_grant_valblk_from_proxy(uint32_t req_lkid, uint8_t *out)
{
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;

    memset(out, 0, LCK_VALBLK_SIZE);
    lock = dlm_proxy_find(req_lkid);
    if (lock == NULL)
        return;
    res = lock->resource;
    exec_lock(&res->lock);
    memcpy(out, lock->valblk, LCK_VALBLK_SIZE);
    exec_unlock(&res->lock);
    lock_put(lock);
}

uint32_t vms_lock_dlm_proxy_grant_recv(const struct vms_dlm_proxy_grant *g)
{
    struct vms_dlm_xnode_args args;

    if (g == NULL)
        return SS__BADPARAM;

    memset(&args, 0, sizeof(args));
    args.op          = VMS_DLM_OP_GRANT;
    args.req_lkid    = g->req_lkid;
    args.master_lkid = g->master_lkid;
    args.master_csid = g->master_csid;
    args.req_csid    = vms_local_csid;
    args.lkmode      = g->granted_mode;
    if (g->valblk_present)
        memcpy(args.valblk, g->valblk, LCK_VALBLK_SIZE);
    else
        dlm_grant_valblk_from_proxy(g->req_lkid, args.valblk);

    return vms_lock_dlm_xnode_grant_recv(&args);
}

/*
 * vms_lock_dlm_proxy_blkast_recv - the kernel-core-typed door onto the engine's
 * holder-side blocking-AST delivery (vms_dlm_proxy.h).
 *
 * `proc` is NULL because the cluster's receive context is not a process; the
 * engine then delivers to the process that OWNS the proxy, and honestly
 * declines (SS$_UNSUPPORTED) when none does.
 */
uint32_t vms_lock_dlm_proxy_blkast_recv(uint32_t req_lkid)
{
    struct vms_dlm_xnode_args args;

    memset(&args, 0, sizeof(args));
    args.op       = VMS_DLM_OP_BLKAST;
    args.req_lkid = req_lkid;
    args.req_csid = vms_local_csid;

    return vms_lock_dlm_xnode_blkast_recv(NULL, &args);
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
     * DLM directory lookup + master resolution BEFORE anything is granted.
     * In a cluster of one -- and for every tree this node masters -- the route
     * is LOCAL and the single-node lock manager below runs unchanged.
     *
     * A REMOTE route means the cluster masters this tree (or its directory node
     * does) and the request must GO THERE: a proxy LKB is created, the request
     * is posted, and the caller sleeps on that LKB (FC-P4.4). A cross-node
     * inbound request (xn set) never takes this path -- it arrived AT the
     * master, which is this node by definition of having been sent here.
     */
    {
        enum dlm_route route;
        uint32_t dst_csid, dlm_st;

        exec_lock(&res->lock);
        dlm_st = dlm_resolve_master(res, xn ? 1 : 0, &route, &dst_csid);
        exec_unlock(&res->lock);

        if (dlm_st != SS__NORMAL) {
            resource_release(res);
            args.status = dlm_st;
            goto out;
        }
        if (route == DLM_ROUTE_REMOTE && !xn) {
            enq_proxy_request(proc, &args, res, dst_csid);
            goto out;      /* enq_proxy_request owns `res` from here */
        }
        if (route == DLM_ROUTE_REMOTE) {
            /* An inbound cross-node request for a tree this node does not
             * master is DECLINED (D-DLM-4), never served: two masters for one
             * tree is how a real cluster breaks. */
            resource_release(res);
            args.status = SS__UNSUPPORTED;
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
     * The PARENT lock (vms-0dd). 0 for a root lock -- every existing $ENQ leaves
     * args.parid 0 (the memset default), so parentless locks are unchanged. RMS
     * record locks pass the file-access lock's lkid so a record lock is
     * getlki-visible UNDER its file lock. Stored only; the parent-child
     * auto-release cascade is a follow-on (vms-489).
     */
    lock->parent_id = args.parid;
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
/*
 * deq_proxy_release - the $DEQ half of a PROXY LKB (FC-P4.4).
 *
 * The lock lives at the MASTER, so the release is a message to the master built
 * from this very lock block -- including the value block, when the releaser
 * wrote one (LCK_M_VALBLK): the LVB write crossing travels with the release, as
 * it does on the cross-node $DEQ the master already implements.
 *
 * The local record is torn down either way. If the post failed, the caller gets
 * that status (the master may still believe it holds the lock; the cluster's
 * departure/rebuild machinery is what reconciles that) -- an honest report, not
 * a silent SS$_NORMAL.
 */
static uint32_t deq_proxy_release(struct vms_lock_entry *lock,
                                  struct vms_lock_resource *res)
{
    struct vms_dlm_proxy_post post;
    uint32_t dst;

    exec_lock(&res->lock);
    dst = lock->master_csid ? lock->master_csid : res->master_csid;
    dlm_proxy_fill_post(lock, res, VMS_DLM_POST_DEQ, dst, &post);
    exec_unlock(&res->lock);

    return dlm_proxy_post(&post);
}

static long vms_deq_core(struct vms_proc *proc, struct vms_deq_args *io)
{
    struct vms_deq_args args = *io;
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;
    uint32_t proxy_st = SS__NORMAL;

    lock = lock_find_by_id(args.lkid);
    if (!lock || lock->proc != proc) {
        if (lock)
            lock_put(lock);
        args.status = SS__IVLOCKID;
        goto out;
    }

    res = lock->resource;

    /* A PROXY LKB releases at the MASTER first; only then is the local record
     * torn down. Its value block belongs to the master's RSB, never to ours,
     * so nothing is written back here. */
    if (lock->proxy)
        proxy_st = deq_proxy_release(lock, res);

    /* Remove from resource */
    exec_lock(&res->lock);

    /* Write back value block from lock to resource */
    if (!lock->proxy && (lock->flags & LCK_M_VALBLK) && !lock->waiting)
        memcpy(res->valblk, lock->valblk, LCK_VALBLK_SIZE);

    lock_unlink_from_res(lock);

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

    /* Parent-child cascade (vms-0dd/vms-489): releasing this lock releases any
     * child sublocks held under it (RMS record locks under a file-access lock),
     * so a $CLOSE that $DEQs the file lock does not leak its record locks. Runs
     * after this lock is torn down and holds no other lock, so it nests nothing;
     * a leaf lock (no children) makes the sweep a cheap no-op. */
    release_child_locks(proc, args.lkid);

    args.status = proxy_st;   /* SS$_NORMAL for a local lock; for a proxy, what
                               * the post to the master actually returned */

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
 * vms_lock_acp_vol_standing - take the STANDING per-volume lock a faithful MOUNT
 * holds for the WHOLE mount life (vms-25e). Unlike vms_lock_acp_vol_ex (the
 * transient EX synchronization lock a single on-disk-structure write takes and
 * immediately drops), this is a NULL-mode (NL) lock held from $MOUNT to
 * $DISMOUNT. Its purpose is cluster registration, not serialization: it names the
 * volume in the DLM so the connection manager can register "this node has this
 * volume mounted" to the coordinator during a directory rebuild -- the standing
 * F11B$v<label> lock a real VMS node holds and re-registers on join.
 *
 * NL asserts PRESENCE, not exclusion: it takes a different resource (F11B$v) than
 * the XQP's per-write sync lock (F11B$s), holds nothing exclusive, and so can
 * never block the XQP, another writer, or another cluster node -- deadlock-free
 * and cluster-safe by construction. Released by vms_lock_acp_vol_release at
 * $DISMOUNT (or on the holder's rundown). Returns the executive status; on
 * SS$_NORMAL *lkid_out is the standing lock's handle for later registration.
 */
uint32_t vms_lock_acp_vol_standing(struct vms_proc *proc, const char *resnam,
                                   uint32_t *lkid_out)
{
    struct vms_enq_args a;

    if (lkid_out)
        *lkid_out = 0;
    memset(&a, 0, sizeof(a));
    a.lkmode = LCK_K_NLMODE;    /* NL: a presence marker, holds nothing exclusive */
    a.flags  = LCK_M_SYNC;      /* $ENQW; an NL request grants immediately */
    strscpy(a.resnam, resnam, sizeof(a.resnam));

    vms_enq_core_ex(proc, &a, NULL);
    if (a.status == SS__NORMAL && lkid_out)
        *lkid_out = a.lkid;
    return a.status;
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

    /* A PROXY LKB converts AT THE MASTER (FC-P4.4): the new mode is a request
     * over the wire, and the caller waits on the same LKB it already holds --
     * the local compatibility matrix has no say over a lock the cluster owns. */
    if (lock->proxy) {
        args.status = convert_proxy_request(proc, &args, lock);
        lock_put(lock);
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

    /*
     * ONE OBJECT (FC-P4.4). A cross-node request this node issued is a PROXY
     * LKB in the same lock-ID database as every other lock, so there is no
     * fall-through table any more: this lookup finds it, and the fields below
     * report the master's genuine answer -- the NL->EX flip driven by the remote
     * master is observable here exactly as a local grant is. An unset value
     * block reads as zeros (the LKB is zero-allocated and only a real grant
     * writes it), never a stale or fabricated one.
     */
    lock = lock_find_by_id(args.lkid);
    if (!lock) {
        args.status = SS__IVLOCKID;
        goto out;
    }

    args.granted_mode = lock->granted_mode;
    args.requested_mode = lock->waiting ? lock->requested_mode : lock->granted_mode;
    args.parent_id = lock->parent_id; /* the lock's PARENT lkid (vms-0dd), 0 if root */

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
 * vms_ioctl_dlm_member_depart - a member of this cluster has left.
 *
 * WHAT IT DOES NOW (FC-P4.3). The membership this node's directory resolves
 * over is the connection manager's CLUB, reached through the injected
 * `dir_resolve`/`dir_generation` ops -- so a departure does not need to be
 * mirrored into the lock engine at all: the transition that removes the member
 * invalidates the Lock Directory Weight Vector at Phase 1 and refills it at
 * Phase 2 (Davis p. 6-33), the generation changes, and every cached
 * res->dir_csid is re-resolved by construction. The static insmod membership
 * vector this ioctl used to mutate (dlm_member_csids / dlm_member_departed) is
 * GONE with it.
 *
 * What is still this facility's own business is the LOCK STATE the departure
 * orphans, and that is what remains here: a resource MASTERED on the departed
 * node loses its master (it re-masters on first use), cached directory answers
 * are dropped eagerly rather than waiting for the next generation check, and a
 * proxy LKB still pending at the departed master is ended honestly rather than
 * left asleep forever. Reconstructing the locks themselves is the rebuild
 * (FC-P5.5), not this.
 *
 * `members_live` is reported as 0: the number of live members is the connection
 * manager's fact, not this engine's, and answering with a count the lock
 * manager no longer holds would be exactly the mirrored-membership fabrication
 * FC-P3.9 deleted. `found` reports whether the departed CSID actually mastered
 * anything here -- a real observation, not a lookup in a configured list.
 */

/*
 * dlm_proxies_master_departed - end the waits that the departed node was the
 * only possible answer to (FC-P4.4). Caller holds res->lock.
 *
 * A proxy LKB PENDING at a master that has left the cluster can never be
 * granted by that master. Rather than leave the requester asleep forever, the
 * executive completes the request with SS$_UNSUPPORTED -- honest: this node
 * cannot serve it, and re-driving it at the tree's new master is the rebuild's
 * job (FC-P5.5), which does not exist yet. A proxy already GRANTED is left
 * alone: it is a real lock the survivors must describe to the new master
 * (Davis p. 6-53), not something to throw away here.
 */
static void dlm_proxies_master_departed(struct vms_lock_resource *res,
                                        uint32_t departed_csid)
{
    struct vms_lock_entry *p;

    if (departed_csid == 0)
        return;
    exec_list_for_each_entry(p, &res->proxies, res_proxy) {
        if (p->master_csid != departed_csid)
            continue;
        if (!p->waiting || p->grant_state != 0)
            continue;                  /* granted: preserved for the rebuild */
        p->grant_state = SS__UNSUPPORTED;
        exec_cv_broadcast(&p->wait_wq);
    }
}

long vms_ioctl_dlm_member_depart(struct vms_proc *proc, unsigned long arg)
{
    struct vms_dlm_depart_args args;
    struct vms_lock_resource *res;
    int bkt;

    (void)proc;
    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&vms_res_hash_lock);

    /* Drop every cached directory answer: the vector the answers came from is
     * being rebuilt around this departure (p. 6-33). The generation check in
     * dir_resolve() would catch them anyway; clearing here makes the discard
     * immediate and keeps this path meaningful for a node whose connection
     * manager has not yet run the transition. A resource mastered ON the
     * departed node loses its master (re-master on first use); one mastered on
     * a survivor keeps it. */
    exec_hash_for_each(vms_res_hash, bkt, res, hash_node) {
        exec_lock(&res->lock);
        res->dir_valid = 0;
        res->dir_csid = 0;
        if (args.departed_csid != 0 && res->master_csid == args.departed_csid) {
            res->master_csid = 0;
            args.found = 1;
        }
        dlm_proxies_master_departed(res, args.departed_csid);
        exec_unlock(&res->lock);
    }

    args.members_live = 0;   /* the CLUB's fact, not this engine's */
    exec_unlock(&vms_res_hash_lock);

    args.status = SS__NORMAL;
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_dlm_get_granted - $DLM granted-lock readback (rd vms-dca9, H10b).
 * Report the FIRST remote-held granted lock on a resource (req_csid != 0) -- its
 * holder CSID, the holder's OWN lock handle (req_lkid) and the granted mode -- so
 * a test can VALUE-VERIFY a rebuilt cross-node lock EQUALS the one the holder
 * really held pre-departure, not merely that n_granted rose. INV-6: reports the
 * REAL res->granted state; found=0 (fields 0) when no remote-held lock exists.
 */
long vms_ioctl_dlm_get_granted(struct vms_proc *proc, unsigned long arg)
{
    struct vms_dlm_granted_args args;
    struct vms_lock_resource *res;

    (void)proc;
    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    args.resnam[sizeof(args.resnam) - 1] = '\0';
    if (args.resnam[0] == '\0') {
        args.status = SS__BADPARAM;
        goto out;
    }

    exec_lock(&vms_res_hash_lock);
    res = resource_find(args.resnam);
    if (res) {
        struct vms_lock_entry *granted;
        uint32_t n = 0;

        exec_lock(&res->lock);
        exec_list_for_each_entry(granted, &res->granted, res_granted) {
            n++;
            /* First REMOTE-held granted lock (held FOR a peer, req_csid != 0) --
             * the rebuilt cross-node lock the H10b harness value-verifies. */
            if (granted->req_csid != 0 && args.found == 0) {
                args.found = 1;
                args.holder_csid = granted->req_csid;
                args.holder_req_lkid = granted->req_lkid;
                args.granted_mode = granted->granted_mode;
            }
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
 * vms_ioctl_dlm_enum_waits - $DLM pending-wait enumeration (rd vms-ec75, H11).
 * The HOME authority for distributed deadlock search: enumerate THIS node's
 * outstanding cross-node requests that are still PENDING (a PROXY LKB with
 * granted_mode == NL). Each names a resource this node waits on (resnam), the
 * node mastering it (master_csid), and this node's own requester-side handle for
 * the wait (req_lkid) -- one outgoing wait-for edge for the edge-chase.
 * INV-6: a READ of the REAL proxy LKBs the master's replies completed (FC-P4.4;
 * formerly the vms_dlm_origin list); count 0 when nothing is pending, never a
 * fabricated edge. Surfaces EXISTING executive state -- no wait-for graph is
 * stored or guessed.
 */
long vms_ioctl_dlm_enum_waits(struct vms_proc *proc, unsigned long arg)
{
    struct vms_dlm_enum_waits_args args;
    struct vms_lock_resource *res;
    int bkt;

    (void)proc;
    memset(&args, 0, sizeof(args));

    exec_lock(&vms_res_hash_lock);
    exec_hash_for_each(vms_res_hash, bkt, res, hash_node) {
        struct vms_lock_entry *p;

        exec_lock(&res->lock);
        exec_list_for_each_entry(p, &res->proxies, res_proxy) {
            if (p->granted_mode != LCK_K_NLMODE)
                continue;             /* pending (NL) waits only -- a granted
                                       * proxy is a HOLD, not a wait-for edge */
            args.total++;
            if (args.count < VMS_DLM_ENUM_WAITS_MAX) {
                struct vms_dlm_wait_ent *e = &args.ent[args.count];
                strscpy(e->resnam, res->name, sizeof(e->resnam));
                e->master_csid = p->master_csid;
                e->req_lkid = p->lkid;
                e->req_csid = p->req_csid;
                e->granted_mode = p->granted_mode;
                args.count++;
            }
        }
        exec_unlock(&res->lock);
    }
    exec_unlock(&vms_res_hash_lock);

    args.status = SS__NORMAL;
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_lock_dlm_xnode_dlksrch - the distributed deadlock search VICTIM leg (rd
 * vms-ec75, DLM rung H11). The SEARCH legs of the edge-chase are pure reads
 * orchestrated in scsd over the two readback ioctls (DLM_ENUM_WAITS home authority
 * + DLM_GET_GRANTED master authority); only the VICTIM leg mutates executive state,
 * so only it is dispatched here.
 *
 * When a probe closes a cycle, the detecting node names the GLOBAL-min victim
 * (req->req_csid, req->req_lkid) and sends this to the node that MASTERS the
 * victim's queued request. That master aborts it: find the queued cross-node waiter
 * (a res->waiting entry with req_csid == victim_csid && req_lkid == victim_lkid),
 * remove it, and complete it with SS$_DEADLOCK exactly as the local detector's
 * unwind does. IDEMPOTENT (design §3): a second VICTIM naming an already-aborted
 * request finds no waiter and is a no-op (status SS$_NORMAL, queued 0), so a
 * concurrent A- and B-initiated search that agree on the victim abort it once.
 *
 * INV-6: aborts a REAL queued waiter read off res->waiting; when none matches it
 * fabricates nothing (queued 0). Returns via req->status: SS$_DEADLOCK when a
 * waiter was aborted this call, SS$_NORMAL when none matched (idempotent no-op).
 */
static uint32_t vms_lock_dlm_xnode_dlksrch(struct vms_dlm_xnode_args *req)
{
    struct vms_lock_resource *res;
    struct vms_lock_entry *victim = NULL;
    struct vms_lock_resource *victim_res = NULL;
    uint32_t victim_csid = req->req_csid;
    uint32_t victim_lkid = req->req_lkid;
    int bkt;

    req->queued = 0;

    /* Only the VICTIM leg reaches the executive; a SEARCH leg dispatched here is a
     * scsd bug (SEARCH is read-only orchestration), refused honestly. */
    if (req->flags != VMS_DLM_DLK_VICTIM)
        return SS__BADPARAM;
    if (victim_lkid == 0)
        return SS__BADPARAM;

    /* Scan every resource's waiting queue for the named cross-node waiter. The
     * victim's request carries req_csid == victim_csid (its home node), so a match
     * is (req_csid,req_lkid)-exact -- a REAL queued lock, not a guess. */
    exec_lock(&vms_res_hash_lock);
    exec_hash_for_each(vms_res_hash, bkt, res, hash_node) {
        struct vms_lock_entry *w;
        exec_lock(&res->lock);
        exec_list_for_each_entry(w, &res->waiting, res_waiting) {
            if (w->waiting && w->req_csid == victim_csid &&
                w->req_lkid == victim_lkid) {
                /* Remove the victim from the waiting queue under res->lock. A
                 * waiter blocks nobody (only granted holders do), so its removal
                 * grants no one here: the cycle breaks when the victim's PROCESS
                 * releases the lock it HELD (application back-off, the VMS
                 * contract), not by this abort. goto exits BOTH the bucket and the
                 * chain loop of the nested exec_hash_for_each cleanly. */
                victim = w;
                victim_res = res;
                exec_list_del(&victim->res_waiting);
                victim->waiting = 0;
                exec_unlock(&res->lock);
                goto found;
            }
        }
        exec_unlock(&res->lock);
    }
found:
    exec_unlock(&vms_res_hash_lock);

    if (!victim)
        return SS__NORMAL;    /* idempotent: nothing to abort (already gone) */

    /* Detach the aborted request from its owning proc and free it -- the same
     * unwind the local detector runs when check_deadlock trips (vms_enq_core_ex). */
    if (victim->proc) {
        exec_lock(&victim->proc->lock_list_lock);
        exec_list_del(&victim->proc_list);
        victim->proc->lock_count--;
        exec_unlock(&victim->proc->lock_list_lock);
    }
    lock_remove_id(victim);
    resource_release(victim_res);
    lock_entry_free(victim);

    req->queued = 1;          /* aborted this call (distinct from the idempotent no-op) */
    return SS__DEADLOCK;
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
     * dir_csid is the DIRECTORY NODE for this name, and it is now reported only
     * when the executive genuinely holds the two things it takes (FC-P4.3): the
     * cluster's own 16-bit hash for the name, learned from the wire, and an
     * authoritative Lock Directory Weight Vector. It is filled in the found
     * branch below from the resource's real resolution.
     *
     * 0 therefore means "not resolved" -- no cluster, no wire-learned hash, or
     * a vector under rebuild -- exactly as master_csid 0 means "unmastered".
     * The predecessor reported a value for EVERY name, because it computed one
     * (exec_jhash % a static vector); a value computed from an OVMX hash is not
     * the cluster's directory node, and reporting it as one is what a readback
     * must never do (INV-6).
     *
     * The one name-independent case is a node with NO cluster: there is no
     * other member, so this node is the directory for everything, and that is
     * reportable whether or not a resource block exists.
     */
    if (!dlm_directory_installed())
        args.dir_csid = vms_local_csid;

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

        uint32_t dir = 0;

        exec_lock(&res->lock);
        args.found = 1;
        if (dir_resolve(res, &dir) == SS__NORMAL)
            args.dir_csid = (dir != 0) ? dir : vms_local_csid;
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
    if (lock->proxy || lock->req_csid == 0 || lock->req_csid != req->req_csid) {
        /* Not a cross-node lock, not held for the releasing node, or a PROXY
         * LKB -- this node's own requester-side image of a lock some OTHER node
         * masters, which no peer may release through the master path (FC-P4.4). */
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
    lock_unlink_from_res(lock);

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
 * dlm_proxy_reconstruct - create the proxy LKB a master's message names when
 * this node holds none (FC-P4.4).
 *
 * req_lkid is, by the protocol's own definition, a lock id of THIS node, so it
 * is inserted into the lock-ID database AT that id -- one keyspace, no side
 * table. The lock is owned by no local process (proc NULL): it is the NODE's
 * record of a lock the cluster holds for it, so no process rundown may take it
 * away. Every field comes from the master's message.
 *
 * Returns the new proxy (no extra reference; it lives on res->proxies) or NULL
 * when the id is already in use or the resource/lock cannot be made.
 */
static struct vms_lock_entry *dlm_proxy_reconstruct(struct vms_dlm_xnode_args *req)
{
    struct vms_lock_resource *res;
    struct vms_lock_entry *lock;
    struct vms_enq_args a;

    res = resource_find_or_create(req->resnam);   /* +1 ref, held by the proxy */
    if (!res)
        return NULL;

    memset(&a, 0, sizeof(a));
    a.lkmode = (req->lkmode > LCK_K_NLMODE) ? req->lkmode : LCK_K_NLMODE;

    lock = dlm_proxy_alloc(NULL, res, &a, req->req_lkid);
    if (!lock) {
        resource_release(res);
        return NULL;
    }
    /* The identity the MASTER addressed this request to. It is the CSID the
     * cluster knows us by, supplied by the cluster itself -- which is what a
     * later wait-for enumeration (DLM_ENUM_WAITS) must report, not a
     * locally-assumed one. Falls back to the executive's own CSID when the
     * message did not name it. */
    if (req->req_csid != 0)
        lock->req_csid = req->req_csid;
    dlm_proxy_link(NULL, res, lock);
    return lock;
}

/*
 * vms_lock_dlm_xnode_grant_recv - the REQUESTER-SIDE GRANT RECEIVE (vms-6ca, DLM
 * epic vms-7fa rung H5; retargeted onto the PROXY LKB by FC-P4.4).
 *
 * A GRANT / queued-reply message the MASTER sent back in answer to a cross-node
 * $ENQ THIS node issued lands here and completes the PROXY LKB that represents
 * the request -- so the request's status is genuine executive state, not a
 * per-process userspace flag (INV-6). The proxy's granted mode is set ONLY from
 * what the master genuinely sent:
 *   - a queued-reply carries lkmode == NL  -> the proxy stays PENDING
 *     (granted_mode NL): the requester genuinely sees "blocked", from the
 *     master's real waiting-queue decision transported over the wire.
 *   - a deferred GRANT carries lkmode == the granted mode (e.g. EX) and
 *     status SS$_NORMAL -> the proxy flips NL -> EX and any $ENQW asleep on it
 *     WAKES. THIS is the status flip observed on the REQUESTER node, driven by
 *     the master's real release.
 *
 * IDEMPOTENT BY CONSTRUCTION (D-DLM-5). The (req_csid, req_lkid) key IS the
 * lock-ID database: a retransmitted GRANT finds the SAME proxy and re-records
 * the same values -- it can never mint a second lock under one handle. And it
 * can never turn a lock this node MASTERS into a proxy: an id naming a
 * non-proxy lock is refused SS$_IVLOCKID.
 *
 * VMS_DLM_LKID_UNSET (the fc8540ae cluster-crasher): a GRANT that claims a
 * granted mode while carrying NO master handle is REFUSED. Recording a
 * placeholder handle is how a completion later put a fabricated lock id on the
 * wire, and a real VAX bugchecked (INVLOCKID) over exactly that.
 */
static uint32_t vms_lock_dlm_xnode_grant_recv(struct vms_dlm_xnode_args *req)
{
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;
    int created = 0;

    if (req->req_lkid == 0)
        return SS__BADPARAM;   /* a GRANT must name the requester's own handle */
    if (req->lkmode > LCK_K_NLMODE && req->master_lkid == VMS_DLM_LKID_UNSET)
        return SS__BADPARAM;   /* a grant with no master handle is not a grant */

    lock = lock_find_by_id(req->req_lkid);
    if (lock != NULL && !lock->proxy) {
        lock_put(lock);
        return SS__IVLOCKID;   /* names one of OUR locks, not one of our proxies */
    }
    if (lock == NULL) {
        if (req->resnam[0] == '\0')
            return SS__BADPARAM;
        lock = dlm_proxy_reconstruct(req);
        if (lock == NULL)
            return SS__INSFMEM;
        created = 1;
    }
    res = lock->resource;

    exec_lock(&res->lock);

    /* Record what the MASTER genuinely reported: its handle for the lock, its
     * CSID, and the mode it says the request is granted at. */
    if (req->master_lkid != VMS_DLM_LKID_UNSET)
        lock->master_lkid = req->master_lkid;
    if (req->master_csid != 0) {
        lock->master_csid = req->master_csid;
        /*
         * The cluster has told us who masters this tree. Storing it on the RSB
         * is what sends the NEXT $ENQ straight to the master instead of back
         * through the directory node (Davis p. 6-32: one lookup per tree while
         * this node holds a lock in it). It is a value the CLUSTER returned --
         * never one this node computed (Rule 8).
         */
        if (res->master_csid != vms_local_csid)
            res->master_csid = req->master_csid;
    }
    if (req->lkmode > LCK_K_NLMODE)
        lock->requested_mode = req->lkmode;
    lock->granted_mode = req->lkmode;   /* NL on a queued-reply; EX on a grant */

    /*
     * LVB read crossing (rd vms-eeb, rung H9). A real GRANT (a non-NL granted
     * mode) carries the master's resource value block; store it ON THE PROXY so
     * GETLKI(req_lkid) surfaces it to the requester's $ENQ caller -- the mirror
     * of the write crossing (vms-d81). A queued-reply (NL) delivers no LVB and
     * writes none, so an ungranted proxy reports zeros honestly rather than a
     * stale block. The bytes are the master's real res->valblk transported over
     * SCS, never fabricated (INV-6).
     */
    if (req->lkmode > LCK_K_NLMODE)
        memcpy(lock->valblk, req->valblk, LCK_VALBLK_SIZE);

    /*
     * BLKAST WIRE (rung H6, vms-76d). When this GRANT establishes the proxy as a
     * HOLDER (a real granted mode) AND the holder registered a blocking-AST
     * routine, remember it so a BLKAST the master later sends can fire a genuine
     * user-mode AST. Recorded ONLY from what the holder supplied on its own $ENQ
     * (req->blkastadr) -- never fabricated. A queued-reply (NL), or a GRANT with
     * no blkastadr, leaves the proxy with no blocking AST, so a later BLKAST
     * honestly declines rather than inventing a delivery (INV-6).
     */
    if (req->blkastadr != 0 && req->lkmode > LCK_K_NLMODE) {
        lock->blkastadr = req->blkastadr;
        lock->blkastprm = req->blkastprm;
    }

    /*
     * WAKE THE REQUESTER. A granted proxy is no longer pending, so a $ENQW
     * asleep in enq_wait_sync on this very LKB is woken here -- under res->lock,
     * the same discipline try_grant_waiters uses, which is what makes the wait
     * lost-wakeup-free. A queued-reply leaves it pending and wakes nobody.
     */
    if (req->lkmode > LCK_K_NLMODE) {
        lock->waiting = 0;
        lock->grant_state = SS__NORMAL;
        queue_completion_ast(lock);      /* async $ENQ: the completion AST */
        exec_cv_broadcast(&lock->wait_wq);
    }
    exec_unlock(&res->lock);

    if (!created)
        lock_put(lock);   /* drop the lock_find_by_id reference */
    return SS__NORMAL;
}

/*
 * vms_lock_dlm_xnode_blkast_recv - the HOLDER-SIDE BLOCKING-AST DELIVERY (rung H6,
 * vms-76d; retargeted onto the PROXY LKB by FC-P4.4). The symmetric mirror of
 * vms_lock_dlm_xnode_grant_recv: a BLKAST the MASTER sent -- because a conflicting
 * request queued behind this node's granted lock -- lands here and FIRES the
 * holder's blocking AST for real.
 *
 * The object is the PROXY LKB the GRANT receive established (named by the
 * holder's own lock id, which the BLKAST frame carries as req_lkid). If it
 * exists and the holder registered a blocking-AST routine on its own $ENQ, a
 * REAL user-mode AST is queued -- the SAME mechanism notify_blocking_asts and
 * queue_completion_ast use -- so the holder genuinely receives the blocking AST
 * (drainable via VMS_IOCTL_DELIVERAST), not a log line. It goes to the process
 * that OWNS the proxy when one does; a proxy the cluster reconstructed has no
 * owner, so it goes to the delivering context, as before.
 *
 * Returns SS$_NORMAL and sets req->blkast_delivered=1 when an AST was genuinely
 * queued. Returns SS$_UNSUPPORTED (never faked) when the handle names no proxy of
 * ours, or that proxy carries no blocking-AST routine -- honest, per INV-6.
 */
static uint32_t vms_lock_dlm_xnode_blkast_recv(struct vms_proc *proc,
                                               struct vms_dlm_xnode_args *req)
{
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;
    struct vms_ast_entry *ast;
    struct vms_ast_state *ast_state;
    struct vms_proc *target;
    uint64_t blkastadr, blkastprm;

    if (req->req_lkid == 0)
        return SS__BADPARAM;   /* a BLKAST must name the holder's own handle */
    /*
     * `proc` MAY be NULL (FC-P4.6). The ioctl path always has one, but the
     * cluster's own receive context is not a process -- so the delivery target
     * is resolved below from the PROXY's owner, and a proxy with no owner is an
     * honest SS$_UNSUPPORTED rather than a BADPARAM about a caller that did
     * nothing wrong.
     */

    lock = dlm_proxy_find(req->req_lkid);
    if (lock == NULL)
        return SS__UNSUPPORTED;   /* no proxy of ours by that handle -- honest */

    res = lock->resource;
    exec_lock(&res->lock);
    blkastadr = lock->blkastadr;
    blkastprm = lock->blkastprm;
    target = lock->proc ? lock->proc : proc;
    if (blkastadr != 0 && target != NULL)
        lock->blkast_count++;     /* a REAL delivery, counted */
    exec_unlock(&res->lock);
    lock_put(lock);

    if (blkastadr == 0)
        return SS__UNSUPPORTED;   /* the holder registered none -- never faked */
    if (target == NULL)
        return SS__UNSUPPORTED;   /* nobody to deliver to -- never faked */

    /* Queue a REAL user-mode blocking AST to the holder. Mirrors
     * notify_blocking_asts: astprm is the holder's own request handle (VMS delivers
     * the lock id as the blocking AST parameter unless the holder supplied one). */
    ast = exec_zalloc_atomic(sizeof(*ast));
    if (ast == NULL)
        return SS__INSFMEM;
    ast->astadr = blkastadr;
    ast->astprm = blkastprm ? blkastprm : (uint64_t)req->req_lkid;
    ast->acmode = PSL_C_USER;

    ast_state = &target->ast[PSL_C_USER];
    exec_lock(&ast_state->lock);
    if (ast_state->count < VMS_AST_MAX_PER_MODE) {
        exec_list_add_tail(&ast->list, &ast_state->pending);
        ast_state->count++;
        exec_unlock(&ast_state->lock);
        vms_ast_notify_arrival(target);
    } else {
        exec_free(ast);
        exec_unlock(&ast_state->lock);
        return SS__INSFMEM;   /* queue full -- honest failure, no faked delivery */
    }

    req->blkast_delivered = 1;
    return SS__NORMAL;
}

/*
 * vms_lock_dlm_xnode_rebuild - remaster lock reconstruction (rd vms-dca9, rung
 * H10b). A surviving holder (req->req_csid) re-registers its cross-node lock on
 * THIS node -- the NEW master, after the old master gracefully departed and the
 * directory re-resolved the resource here. RECONSTRUCT the holder's EXACT prior
 * grant DIRECTLY into res->granted -- like vms_lock_dlm_xnode_deq manipulates the
 * queues directly, NOT through the enqueue/grant core: re-running the core would
 * re-derive a grant against this node's (empty) queue and lock_compatible rather
 * than preserve the holder's REAL pre-departure lock. The mode (req->lkmode), the
 * holder's identity (req->req_csid) and its own handle (req->req_lkid) are the
 * REAL values from the holder's own proxy LKB (it read them via GETLKI before
 * pushing this REBUILD), transported over SCS -- never a fabricated or defaulted
 * lock (INV-6). This node becomes the resource's master. Returns the new master's
 * handle for the rebuilt lock in req->master_lkid.
 */
static uint32_t vms_lock_dlm_xnode_rebuild(struct vms_proc *proc,
                                           struct vms_dlm_xnode_args *req)
{
    struct vms_lock_resource *res;
    struct vms_lock_entry *lock;

    /* A rebuild MUST name a real remote holder + its own handle + a grant mode;
     * a zero/NL rebuild would fabricate a lock, which INV-6 forbids. */
    if (req->req_csid == 0 || req->req_lkid == 0 || req->lkmode == LCK_K_NLMODE)
        return SS__BADPARAM;

    res = resource_find_or_create(req->resnam);   /* +1 refcount, held by lock */
    if (!res)
        return SS__INSFMEM;

    lock = exec_zalloc(sizeof(struct vms_lock_entry));
    if (!lock) {
        resource_release(res);
        return SS__INSFMEM;
    }

    lock->refcount = 1;
    lock->granted_mode = req->lkmode;      /* the mode the holder REALLY held */
    lock->requested_mode = req->lkmode;
    lock->resource = res;
    lock->proc = proc;                     /* the delivering (scsd) proc */
    lock->waiting = 0;
    exec_cv_init(&lock->wait_wq);
    lock->req_csid = req->req_csid;        /* held FOR this remote holder */
    lock->req_lkid = req->req_lkid;        /* the holder's OWN handle */
    lock_insert_id(lock);                  /* a fresh local lkid on the new master */

    exec_lock(&res->lock);
    res->master_csid = vms_local_csid;     /* this node is now the master */
    exec_list_add_tail(&lock->res_granted, &res->granted);
    exec_unlock(&res->lock);

    req->master_lkid = lock->lkid;
    return SS__NORMAL;
}

/*
 * vms_lock_dlm_xnode_enq_idempotent - RETRANSMIT IDEMPOTENCY at the master
 * (D-DLM-5; the anti-storm property, salvaged from feat/coord-rebuild-completion
 * and extended to the waiting queue by FC-P4.4).
 *
 * A cluster member that has not seen a STABLE granted lock re-sends the same
 * request. Without this, every re-send ran the full enqueue core and minted a
 * FRESH master lock record, so the handle the master returned changed on each
 * reply (0x328 -> 0x329 -> 0x32a ...) and the requester could never correlate a
 * held lock: it re-requested forever. That is the measured ~35/sec storm on
 * LNM$CWLOGICALS and F11B$aSYSDSK1. A real VMS master is idempotent to a
 * retransmit: the SAME (req_csid, req_lkid) on the SAME resource gets the SAME
 * master handle every time, and the same answer.
 *
 * Both queues are matched, because both states are re-sent:
 *   GRANTED at a mode that covers the request -> the same handle, SS$_NORMAL.
 *   still WAITING at the same requested mode  -> the same handle, and the
 *      caller returns VMS_DLM_STS_QUEUED again. Minting a second waiter would
 *      double-queue one request, which is how a queue grows without bound.
 * An UP-CONVERSION (the existing grant is weaker than the request) is NOT a
 * retransmit -- it falls through to the core, which must process it for real.
 *
 * Returns 1 when an existing lock answers the retransmit (with *queued telling
 * the caller which status to return), 0 to run the core. Cross-node only (both
 * ids non-zero); a local $ENQ is never deduped here. INV-6: the handle returned
 * names a REAL lock record on this master's own queue -- never a fabricated or
 * defaulted grant. A PROXY LKB can never match: proxies are on neither queue.
 */
static int vms_lock_dlm_xnode_enq_idempotent(struct vms_dlm_xnode_args *req,
                                             int *queued)
{
    struct vms_lock_resource *res;
    struct vms_lock_entry *lock;
    int handled = 0;

    *queued = 0;
    if (req->req_csid == 0 || req->req_lkid == 0)
        return 0;   /* a local ENQ or an unidentified requester: never dedup */

    res = resource_find_or_create(req->resnam);
    if (!res)
        return 0;   /* let the core report SS$_INSFMEM honestly */

    exec_lock(&res->lock);
    exec_list_for_each_entry(lock, &res->granted, res_granted) {
        if (lock->req_csid == req->req_csid &&
            lock->req_lkid == req->req_lkid &&
            lock->granted_mode >= req->lkmode) {
            req->master_lkid = lock->lkid;          /* the SAME stable handle */
            req->master_csid = vms_local_csid;
            memcpy(req->valblk, res->valblk, LCK_VALBLK_SIZE);
            handled = 1;
            break;
        }
    }
    if (!handled) {
        exec_list_for_each_entry(lock, &res->waiting, res_waiting) {
            if (lock->req_csid == req->req_csid &&
                lock->req_lkid == req->req_lkid &&
                lock->requested_mode == req->lkmode) {
                req->master_lkid = lock->lkid;      /* the SAME stable handle */
                req->master_csid = vms_local_csid;
                handled = 1;
                *queued = 1;
                break;
            }
        }
    }
    exec_unlock(&res->lock);

    resource_release(res);
    return handled;
}

/*
 * vms_lock_dlm_xnode_dispatch - the cross-node DLM RECEIVE handler
 * (vms-94c transport; DLM epic vms-7fa).
 *
 * A DLM request that arrived over SCS from a REMOTE node (decoded by
 * a peer, marshalled through VMS_IOCTL_DLM_XNODE) is dispatched
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
        int dup_queued = 0;

        /* A request that names a resource must actually name one. */
        if (req->resnam[0] == '\0')
            return SS__BADPARAM;

        /*
         * RETRANSMIT FIRST (D-DLM-5). A re-send of a request this master is
         * already serving gets the SAME handle and the SAME answer -- it never
         * mints a second lock. This is what stops the re-request storm.
         */
        if (vms_lock_dlm_xnode_enq_idempotent(req, &dup_queued)) {
            req->queued = dup_queued ? 1u : 0u;
            return dup_queued ? (uint32_t)VMS_DLM_STS_QUEUED : SS__NORMAL;
        }

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
    case VMS_DLM_OP_REBUILD:
        /* REMASTER LOCK REBUILD (rd vms-dca9, rung H10b). A surviving holder
         * re-registers its cross-node lock on THIS node -- the NEW master after
         * the old master gracefully departed and the directory re-resolved here.
         * Reconstruct the holder's REAL pre-departure grant DIRECTLY into
         * res->granted (never re-derive it through the enqueue/grant core). */
        if (req->resnam[0] == '\0')
            return SS__BADPARAM;
        return vms_lock_dlm_xnode_rebuild(proc, req);
    case VMS_DLM_OP_DLKSRCH:
        /* DISTRIBUTED DEADLOCK SEARCH, VICTIM leg (rd vms-ec75, rung H11). A probe
         * that closed a cross-node deadlock cycle named the global-min victim; this
         * node MASTERS the victim's queued request and aborts it, completing that
         * $ENQ with SS$_DEADLOCK. The SEARCH legs never reach here -- they are
         * read-only orchestration in scsd over DLM_ENUM_WAITS + DLM_GET_GRANTED.
         * Idempotent (a second VICTIM finds no waiter -> no-op). INV-6: aborts a
         * REAL queued waiter read off res->waiting, never a fabricated cycle. */
        return vms_lock_dlm_xnode_dlksrch(req);
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
