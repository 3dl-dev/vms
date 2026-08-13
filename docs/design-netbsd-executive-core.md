# Design Record: One shared executive core, two kernel backends

> **Status:** DESIGN DECISION for rd `vms-bea` (epic `vms-8e8`, parent of the
> OVMX/NetBSD SYSKRNL work). This record decides the *abstraction* by which the
> mature Linux VMS executive and a new NetBSD executive run **the same facility
> source** behind a **thin per-substrate kernel-backend shim** — and the SAFE,
> behavior-preserving way to get there without regressing the Linux runtime.
> It is feasibility + design only: **no production code is changed here.** The
> shim header below is a *sketch*.
>
> **Supersedes** the "optional P2.5" hedge in `docs/design-ovmx-netbsd-syskrnl.md`
> §2.2 / §8, which left the shared core as a maybe and blessed a "straight second
> port" as the default. Having *measured* the coupling (below), this record
> reverses that default: the shared core is the recommended path, and a second
> from-scratch NetBSD reimplementation of each facility is rejected as a
> drift-generating duplication of the executive's hardest, authenticity-critical
> logic.
>
> **Clean-room (CLAUDE.md Rule 8):** the shared core is OVMX's own code. The shim
> maps only to **public, documented** Linux and NetBSD kernel APIs (man pages /
> `sys/*` headers: `kmutex(9)`, `cv(9)`, `copyin(9)`/`copyout(9)`, `kmem(9)`,
> `cdevsw(9)`, `module(9)`, `queue(3)`, `rbtree(3)`). No Linux, NetBSD, or
> VSI/HPE source or binary is copied.

---

## 1. The decision, in one paragraph

The VMS executive is **~90% substrate-agnostic C** (VMS identity, event-flag /
lock / AST / mailbox / logical-name / process-table semantics — the
authenticity-critical logic) sitting on a **small, stereotyped surface of host
kernel primitives** (locking, wait/wake, copyin/copyout, kernel alloc, intrusive
containers, and — concentrated in exactly two files — the host-task binding).
The decision: **promote the facility logic and the portable process-control-block
(PCB) state into a substrate-neutral `src/kernel-core/`, and route every host
primitive it touches through one minimal shim header, `exec_kbackend.h`.** The
Linux backend defines that shim as trivial forwarders (`exec_lock` → `spin_lock`)
so the extraction is provably behavior-preserving; the NetBSD backend defines the
same shim against `kmutex(9)`/`cv(9)`/`copyin`/`kmem(9)`. VAX (Phase 3) is then
**not a third executive** — it is the NetBSD backend plus a `libvmssys` VAX arch
backend, inheriting the whole shared core for free.

---

## 2. Evidence: the coupling is thin *and concentrated*

Measured on `origin/main` (`git show origin/main:src/kernel/<f>`), counting the
exact host-primitive call sites per facility:

| Facility (file) | lines | lock (`spin_*`) | copyin/out | wait/wake | kalloc/free | list | hlist / hash | rbtree | rcu | **`current->` / `task_*`** |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| `vms_eflag.c`   |  618 | 41 | 18 | 6 |  6 |  9 | 0 | 0 | 0 | **0** |
| `vms_lock.c`    | 1249 | 69 | 10 | 2 |  4 | 39 | 8 | **10** | 0 | **0** |
| `vms_ast.c`     |  297 | 19 |  5 | 0 |  4 |  6 | 0 | 0 | 0 | **0** |
| `vms_lnm.c`     |  441 |  7 |  5 | 0 | 10 |  0 | 0 | 0 | 0 | **0** |
| `vms_mbx.c`     |  614 | 38 | 10 | 3 | 18 | 21 | 0 | 0 | 0 | **0** |
| `vms_access.c`  |  393 | 18 | 11 | 0 |  0 |  0 | 0 | 0 | 0 | **0** |
| `vms_devtab.c`  | 1142 | 62 | 18 | 0 |  7 | 19 | 0 | 1 (cdev) | 0 | **0** |
| `vms_proctab.c` |  876 | 25 | 11 | 0 |  1 |  0 | 6 | 0 | **6** | **14** |
| `vms_module.c`  | 1031 | 22 |  3 | 0 |  2 |  6 | 14 | 0 | **18** | **24** |
| `vms_internal.h`|  900 | — | — | — | — | — | — | — | — | (shared struct defs) |

Three facts drive the whole design:

1. **The host-*task* binding is not diffused — it lives in exactly two files.**
   Every facility handler takes `struct vms_proc *proc`, but the facilities
   (`eflag`, `lock`, `ast`, `lnm`, `mbx`, `access`, `devtab`) touch **zero**
   `current->` / `task_struct` / `struct pid` state. All 38 host-task references
   are in `vms_proctab.c` (find/register/reap/UIC-derivation) and `vms_module.c`
   (ioctl entry resolving `current->tgid` → PCB). **The proc-model abstraction is
   therefore a localized job on two files, not a tax on all nine.** This is the
   single most important finding: it means the *first* facility to extract (event
   flags) needs no proc-model abstraction at all.

2. **The primitive vocabulary is tiny and stereotyped.** Confirming the eflag
   ratio the epic cited: across the facilities it is the same ~6 primitives every
   time — a plain spinlock (**zero** `irqsave` variants anywhere: all
   `spin_lock()`/`spin_unlock()`, which map cleanly to adaptive `kmutex(9)` at
   `IPL_NONE`), `copy_{to,from}_user`, a `wait_event_interruptible` sleep with a
   `wake_up`, `kzalloc`/`kfree`, and intrusive lists. Each has a direct NetBSD
   analogue. The logic *between* those calls is pure VMS semantics.

3. **Two primitives are the real portability tax, and they are isolated.**
   `vms_lock.c` is the only facility using an **rbtree (10)** and a **hashtable
   (8)** — the lock-ID tree and resource hash. **RCU (24+6)** appears only in
   `vms_module.c`/`vms_proctab.c`, for lockless PCB-hash liveness lookup, and RCU
   has *no* NetBSD equivalent (the analogue is `pserialize(9)`/`psref(9)` or a
   refcount under the hash lock). Both taxes sit in the facilities we extract
   **last**, so they never block the proof-of-pattern.

### 2.1 The PCB split: portable state vs. host binding

`struct vms_proc` (in `vms_internal.h`) is where the two worlds meet. Classifying
its members:

**Portable executive PCB state (→ shared core, unchanged semantics):**
`vms_pid`, `prcnam`, `uic`, `job_id`, `username`, `terminal`, `current_mode`,
`cur_privs`/`perm_privs`, `image_active`/`pre_image_mode`, `ast[4]`, `ef`,
`locks`/`lock_count`, `channels`/`mbx_channels`/`next_chan`,
`p0_base/limit`, `p1_base/limit`. This is *the whole point of the executive* —
the identity and facility state every other process can see. None of it is
Linux-shaped except the embedded primitive *types* (see below).

**Host-OS process binding (→ behind the shim, 4 fields):**
`linux_pid` (a `pid_t` = host tgid, the hash **key**), `pid_ref`
(`struct pid *`, the liveness handle from `get_pid(task_tgid())`), and the
credential-derived provenance (`uic`/`username` are *portable data* but the
*derivation* — `from_kuid(current_uid())`, `current->real_parent` ancestry for
`job_id` — is host-bound). The `rcu_head` used to free-defer the PCB is also
host machinery.

**Embedded primitive types (→ shim typedefs):** the struct literally embeds
`spinlock_t` (×14), `wait_queue_head_t` (×3), `struct list_head` (×16),
`struct hlist_node` (×2), `struct rb_node`/`rb_root`, `struct rcu_head`. **These
are why the shared *header* — not any single `.c` — is the true porting gate:**
nothing compiles on NetBSD until these become `exec_lock_t` / `exec_cv_t` /
`exec_list_node` etc.

**Where the line goes (recommendation):** keep `struct vms_proc` a **shared-core
type** whose members are shim-typed, and reduce its host binding to a single
opaque handle:

```c
/* in the shared core */
struct exec_task_ref;                 /* opaque; defined per-substrate */
struct vms_proc {
    struct exec_hash_node hash_node;  /* shim container */
    exec_pid_t            host_pid;   /* hash key: host process id */
    struct exec_task_ref *task;       /* liveness handle (get/alive/put via shim) */
    uint32_t vms_pid;                 /* ... all portable state as today ... */
    exec_lock_t mode_lock;
    struct exec_rcu_head rcu;
    /* ef/ast/locks/channels: portable, using shim container + lock types */
};
```

Everything that reads `proc->host_pid` for *identity* is portable; everything
that turns a *live host task* into a `host_pid` or tests liveness goes through
`exec_current_*` / `exec_task_*` (below), and those calls exist only in the
proctab facility and the device glue.

---

## 3. The kernel-backend shim — `exec_kbackend.h` (sketch)

Design rule for the whole shim: **shape it like the Linux API** so the Linux
backend is a set of trivial forwarders and the refactor is behavior-preserving —
*except* the wait/wake path, which is shaped like the more-restrictive `cv(9)`
(caller holds the guarding lock), because Linux can always emulate condvar
semantics but NetBSD cannot cheaply emulate Linux's lock-free `wait_event`. Keep
it **minimal**: only what the facilities actually call.

```c
/* exec_kbackend.h — the ONLY header a shared-core facility includes for host
 * primitives. Selected at build time; each backend provides the impl. */
#ifndef EXEC_KBACKEND_H
#define EXEC_KBACKEND_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- opaque primitive types (concrete per substrate) ---- */
typedef struct exec_lock  exec_lock_t;   /* Linux: spinlock_t   NetBSD: kmutex_t */
typedef struct exec_cv    exec_cv_t;     /* Linux: wait_queue_head_t  NetBSD: kcondvar_t */
typedef long              exec_pid_t;    /* host process id (tgid / p_pid) */

/* ---- 1. locking ---- */
void exec_lock_init(exec_lock_t *);
void exec_lock(exec_lock_t *);
void exec_unlock(exec_lock_t *);
void exec_lock_destroy(exec_lock_t *);   /* no-op on Linux; mutex_destroy on NetBSD */

/* ---- 2. wait / wake (cv-shaped: caller holds `lk`) ----
 * Idiom:  exec_lock(&x->lk);
 *         while (!COND) if (exec_cv_wait(&x->cv, &x->lk)) { ... interrupted ... }
 *         exec_unlock(&x->lk);
 * Returns 0 normally, nonzero if interrupted (maps SS$_ABORT/-ERESTARTSYS). */
void exec_cv_init(exec_cv_t *);
int  exec_cv_wait(exec_cv_t *, exec_lock_t *);   /* atomically drops+reacquires lk */
void exec_cv_signal(exec_cv_t *);                /* wake one */
void exec_cv_broadcast(exec_cv_t *);             /* wake all */
void exec_cv_destroy(exec_cv_t *);

/* ---- 3. user <-> kernel copy (normalized: 0 = ok, EXEC_EFAULT = fault) ---- */
#define EXEC_EFAULT 14
int  exec_copyin (void *kdst, const void *usrc, size_t n);
int  exec_copyout(void *udst, const void *ksrc, size_t n);

/* ---- 4. kernel memory (size tracked so NetBSD kmem_free has it) ---- */
void *exec_zalloc(size_t n);   /* zeroed; may sleep */
void *exec_alloc (size_t n);
void  exec_free  (void *p);    /* size recovered from a small header/pool */

/* ---- 5. host-task binding (called ONLY from proctab + device glue) ---- */
exec_pid_t            exec_current_pid(void);          /* Linux current->tgid / NetBSD curproc->p_pid */
exec_pid_t            exec_current_parent_pid(void);   /* for job_id ancestry */
uint32_t              exec_current_uic(void);          /* derived from host creds */
bool                  exec_current_is_privileged(void);/* CAP_SYS_ADMIN / kauth authorization */
struct exec_task_ref *exec_task_pin(exec_pid_t);       /* get_pid(task_tgid()) / proc reference */
bool                  exec_task_alive(struct exec_task_ref *);
void                  exec_task_unpin(struct exec_task_ref *);

/* ---- 6. deferred free (RCU-lite; NetBSD = pserialize or refcount) ---- */
struct exec_rcu_head;
void exec_rcu_read_lock(void);
void exec_rcu_read_unlock(void);
void exec_free_deferred(struct exec_rcu_head *, void (*fn)(struct exec_rcu_head *));

/* ---- 7. intrusive containers: list, hash, rbtree ----
 * Linux-API-shaped; Linux backend forwards to <linux/list.h> etc., NetBSD
 * backend ships an OVMX implementation with the same signatures. (Separate
 * headers exec_list.h / exec_hash.h / exec_rbtree.h, included by the core.) */

#endif /* EXEC_KBACKEND_H */
```

**Two-column mapping (every entry):**

| Shim entry | Linux backend (`exec_kbackend_linux.h`) | NetBSD backend (`exec_kbackend_netbsd.{h,c}`) |
|---|---|---|
| `exec_lock_t` | `spinlock_t` | `kmutex_t` |
| `exec_lock_init` | `spin_lock_init` | `mutex_init(&m, MUTEX_DEFAULT, IPL_NONE)` |
| `exec_lock` / `exec_unlock` | `spin_lock` / `spin_unlock` | `mutex_enter` / `mutex_exit` |
| `exec_lock_destroy` | *(empty)* | `mutex_destroy` |
| `exec_cv_t` | `wait_queue_head_t` | `kcondvar_t` |
| `exec_cv_init` | `init_waitqueue_head` | `cv_init(&cv, "vmsexec")` |
| `exec_cv_wait(cv,lk)` | `prepare_to_wait`+`spin_unlock`+`schedule`+`spin_lock`+`finish_wait` (faithful expansion of `wait_event_interruptible`) | `cv_wait_sig(&cv, &m)` (returns `ERESTART`/`EINTR` on signal) |
| `exec_cv_signal` / `exec_cv_broadcast` | `wake_up` (nr=1) / `wake_up` | `cv_signal` / `cv_broadcast` |
| `exec_copyin` | `copy_from_user` → `?EXEC_EFAULT:0` | `copyin` (already `0`/`EFAULT`) |
| `exec_copyout` | `copy_to_user` → `?EXEC_EFAULT:0` | `copyout` |
| `exec_zalloc` | `kzalloc(n, GFP_KERNEL)` | `kmem_zalloc(n, KM_SLEEP)` (+size header) |
| `exec_alloc` / `exec_free` | `kmalloc` / `kfree` | `kmem_alloc` / `kmem_free(p, n)` |
| `exec_current_pid` | `current->tgid` | `curproc->p_pid` |
| `exec_current_parent_pid` | `task_tgid_nr(current->real_parent)` | `curproc->p_pptr->p_pid` |
| `exec_current_uic` | `from_kuid(&init_user_ns, current_uid())` | `kauth_cred_geteuid(kauth_cred_get())` |
| `exec_current_is_privileged` | `capable(CAP_SYS_ADMIN)` | `kauth_authorize_generic(..., KAUTH_GENERIC_ISSUSER)` |
| `exec_task_pin` | `get_pid(task_tgid(...))` | `proc_find(pid)` + hold reference |
| `exec_task_alive` | `pid_alive` / `pid_task != NULL` | `proc_find(pid) != NULL` (same instance) |
| `exec_task_unpin` | `put_pid` | release reference |
| `exec_free_deferred` | `call_rcu` / `kfree_rcu` | `pserialize` + `kmem_free`, or refcount-drop |
| `exec_list_*` | `<linux/list.h>` macros | OVMX list (BSD `queue(3)`-backed or own) |
| `exec_hash_*` | `<linux/hashtable.h>` | OVMX hash (or `hashinit(9)`) |
| `exec_rbtree_*` | `<linux/rbtree.h>` | `rbtree(3)` (`rb.h`) or OVMX tree |
| char-dev + `/dev/vms` + module lifecycle | `miscdevice`/`file_operations`/`module_init` | `cdevsw`/`d_ioctl`/`MODULE(9)` — **not shared; per-substrate glue** (§4) |

Note the **asymmetries the shim must absorb**, so a facility author never sees
them: (a) `copy_*_user` returns *bytes-not-copied* while `copyin/copyout` return
`0`/`EFAULT` — normalize to `0`/`EXEC_EFAULT`; (b) `kmem_free` needs the size —
`exec_free` recovers it from a tiny allocation header or a `pool(9)`; (c) RCU has
no NetBSD twin — `exec_free_deferred` is the seam, and if `pserialize` proves
awkward the fallback is refcount-under-hash-lock (the proc hash already carries a
`refcount`).

---

## 4. The shared / per-substrate boundary + directory layout

**Substrate-agnostic executive core** (`src/kernel-core/`):
- Every facility's *logic*: `vms_eflag.c`, `vms_lock.c`, `vms_ast.c`,
  `vms_lnm.c`, `vms_mbx.c`, `vms_access.c`, `vms_devtab.c`, and the
  proc-table + ioctl-dispatch logic split out of `vms_module.c` (call it
  `vms_proc.c` + `vms_dispatch.c`).
- The shared structs: `vms_internal.h` (shim-typed), the substrate-neutral
  payload half of `vms_ioctl.h` (the `struct vms_*_args` layouts — already the
  userspace seam's contract).
- The shim contract: `exec_kbackend.h`, `exec_list.h`, `exec_hash.h`,
  `exec_rbtree.h`.

**Per-substrate glue — Linux** (`src/kernel/`, keeps its Kbuild `Makefile`):
- `exec_kbackend_linux.h` (the forwarders), the Linux device rind
  (`miscdevice`, `file_operations`, `.unlocked_ioctl` → `vms_dispatch`,
  `.open`/`.release`, `module_init`/`module_exit`, `MODULE_LICENSE`) — i.e. the
  ~200 lines of `vms_module.c` that are genuinely Linux — plus the per-platform
  `_IO*` request-encoding half of `vms_ioctl.h`. Builds core `.c` + this glue
  into `vms.ko`.

**Per-substrate glue — NetBSD** (`src/kernel-netbsd/`, `bsd.kmodule.mk`):
- `exec_kbackend_netbsd.{h,c}` (kmutex/cv/copyin/kmem impls + the OVMX
  list/hash/rbtree implementations + RCU-lite), the `cdevsw`/`d_ioctl` →
  `vms_dispatch` rind that `vms_netbsd.c` already scaffolds, `MODULE(9)`, and the
  NetBSD `_IO*` encoding. Builds the *same* core `.c` + this glue into the `vms`
  pseudo-device module.

The boundary mirrors the already-proven userspace seam
(`kif_transport_{linux,netbsd}.c` behind `kif_transport.h`) one layer down: this
is the **kernel** analog of that seam. `src/kernel-core/vms_dispatch.c` is the
kernel twin of `vms_kif.c` — substrate-agnostic, unchanged per platform.

---

## 5. Build feasibility — YES, with four bounded caveats

Can the **same** core `.c` compile under **both** Linux Kbuild and NetBSD
`bsd.kmodule.mk`? **Yes**, provided:

1. **Freestanding, no libc, header hygiene.** The core may include *only*
   `exec_kbackend.h` + the OVMX container headers + fixed-width `<stdint.h>`.
   Today each facility includes `<linux/*.h>` directly (e.g. `vms_lock.c` pulls
   `rbtree.h`, `hashtable.h`, `jhash.h`, `wait.h`). Every such include must move
   *behind the shim*. This is mechanical but must be complete — a stray
   `<linux/…>` in a core file breaks the NetBSD build. Enforce with a grep gate.
2. **Intrusive containers can't be macro-bridged host-to-host.** Linux
   `list_for_each_entry` and NetBSD `TAILQ_FOREACH` have incompatible shapes;
   you cannot `#define` one to the other cleanly. Resolution: the core uses one
   **OVMX-shipped, Linux-API-shaped** container API; the Linux backend `#define`s
   it straight through to `<linux/list.h>` (zero cost, preserves byte-identical
   Linux behavior), and the NetBSD backend *implements* it (~150–250 lines of
   freestanding list/hash/rbtree — OVMX's own code, clean-room). This is the one
   place we ship real code rather than a typedef, and it is why "share the core"
   is not free — but it is written once.
3. **RCU has no NetBSD equivalent** (§2, §3). Contained to proctab + device glue,
   behind `exec_free_deferred` + `exec_rcu_read_{lock,unlock}`; fallback is
   refcount-under-hash-lock. Not a core-wide `#if`.
4. **Type portability.** The core already uses `uint32_t`/`uint64_t`/`uint8_t`
   throughout (good). Residual Linux spellings — `u32`, bare `pid_t`, `__user` —
   must be normalized (`exec_pid_t`, drop `__user` in the core since copy goes
   through the shim). **Endianness / width for VAX (Phase 3) is deferred but
   pre-audited:** the payload structs are already fixed-width; the ILP32 + VAX
   byte-order audit is P3's job (`vms-9dc`) and touches the `_IO*` encoding and
   descriptor/RMS layouts, *not* the executive facility logic.

**Net:** no facility forces an `#if` at the core level. The only conditional
compilation lives *inside* the backend headers/`.c`, which is exactly where it
belongs. `#if __linux__` in a core facility file would be a design failure and
should be a review gate.

---

## 6. The SAFE refactor sequence (the critical part)

The Linux executive is mature and authenticity-critical: the **Kernel Executive
QEMU gate (`vms.ko` on `/dev/vms`)**, **INV-6** (no per-process userspace fake),
and the authenticity program all depend on its exact behavior. The refactor MUST
be a mechanical, **behavior-preserving extraction**, facility-by-facility, with
**Linux Debug ctest + the Kernel Executive QEMU gate + `test_runtime_target.sh`
GREEN after every step**. Never a big-bang rewrite.

**Step 0 — Introduce the shim in place, Linux stays byte-identical.**
Add `exec_kbackend.h` + `exec_kbackend_linux.h` (pure forwarders:
`#define exec_lock spin_lock`, `typedef spinlock_t exec_lock`, …) and the
container headers (Linux = straight-through). Convert `vms_internal.h`'s embedded
types to `exec_*`. **No files move; still one `vms.ko`.** Because every forwarder
preprocesses to the original token, the compiled module is behavior-identical.
*Gate: full CI green.* This is the "type-vocabulary" step and the true porting
gate — do it first, alone.

**Step 1 — Extract ONE facility: event flags (the proof-of-pattern).**
`vms_eflag.c` is the right first target: it touches **only** lists (no
hlist/rbtree/rcu) and **zero** host-task state, so no proc-model abstraction is
needed yet. The work is (a) mechanical renames — `spin_lock`→`exec_lock`,
`copy_*_user`→`exec_copyin/out`, `kzalloc`→`exec_zalloc`, `list_*`→`exec_list_*`
— **plus** (b) the one non-`sed` change: convert its 5–6
`wait_event_interruptible(wq, COND)` sites from Linux's lock-free idiom to the
cv-idiom (`while(!COND) exec_cv_wait(&cv,&lk)` under `exec_lock`). On Linux this
still expands to the same wait/wake behavior. *Gate: Linux Debug ctest + Kernel
Executive QEMU gate + `test_runtime_target.sh` green — proving zero regression.*

**Step 2 — Prove ONE core compiles into BOTH kernels (this is re-scoped P2c).**
Move shim-typed `vms_internal.h` + `vms_eflag.c` into `src/kernel-core/`; point
Linux Kbuild at the new location (still green). Add `exec_kbackend_netbsd.{h,c}`
(kmutex/cv/copyin/kmem + the OVMX list impl) and compile **the same
`vms_eflag.c`** into the NetBSD `vms` module. Run a **two-process shared-state
proof on NetBSD/amd64 under QEMU** (a common event-flag cluster set by one
process, waited on and seen by another — the INV-6-honest proof that the state is
executive-resident, not per-process faked). *Gate: Linux stays green AND the same
source is a live executive facility on NetBSD.* **This is the whole thesis
demonstrated on the smallest real facility.**

**Steps 3…N — Extract the rest in ascending coupling order** (each step: same
Linux triple-gate green + compiles into NetBSD):
1. `vms_ast.c`, `vms_access.c` — lists/copy/alloc only; near-pure `sed`. (access
   turned out to need **no** privilege shim — it gates on the executive's own
   `proc->cur_privs`, never `capable()`.) **`vms_lnm.c` was reclassified OUT of
   this batch during Phase D (rd vms-5b2):** it is mm-coupled, not lists/copy —
   `vmalloc_user`/`remap_vmalloc_range`/`vm_flags_clear`/`struct vm_area_struct`
   share its arena to userspace and `smp_wmb()` orders its seqlock, a
   memory-mapping + barrier seam the `exec_*` shim does not cover. It waits on
   that seam (a later phase), not this near-pure batch.
2. `vms_mbx.c`, `vms_devtab.c` — lists + more wait/wake (mbx) and the device
   table; still no proc binding.
3. `vms_proctab.c` — **first proc-model + RCU-lite consumer.** Introduce
   `exec_current_pid/parent_pid/uic`, `exec_task_pin/alive/unpin`,
   `exec_free_deferred`. This is where the "heavier than it looks" work
   concentrates — but it is *one* facility, and its NetBSD `curproc`/`proc_find`
   mapping is well-trodden.
4. Split `vms_module.c` → `vms_dispatch.c` (core) + Linux device rind; NetBSD
   `cdevsw` rind calls the same `vms_dispatch`.
5. **`vms_lock.c` LAST** — the 44KB payoff and the biggest tax (rbtree + hash +
   the deadlock cycle-detector). By now the shim is proven on eight facilities;
   locks bring in `exec_rbtree.h` + `exec_hash.h` on NetBSD, the only remaining
   container work.

Ordering rationale: value-per-risk. Prove the pattern on the smallest clean
facility (eflag), bank the shared-core existence proof (P2c), then descend the
coupling gradient, leaving the highest-tax / highest-value facility (locks) for
when the machinery is fully trusted.

---

## 7. Honest cost/benefit + recommendation

**Recommendation: a pragmatic hybrid — full shared core for the facilities and
the wire/struct headers; per-platform glue for the device/module rind.**

- **Share (high payoff):** `vms_lock.c` (44 KB), `vms_module.c` logic (40 KB),
  `vms_proctab.c` (34 KB), `vms_mbx.c` / `vms_eflag.c` (19 KB each),
  `vms_devtab.c`, `vms_ast.c`, `vms_lnm.c`, `vms_access.c`, and the shared
  headers. This is ~230 KB of authenticity-critical logic that must **never**
  drift into two divergent copies. A second from-scratch port would re-derive the
  lock deadlock-detector, the PCB-identity rules (the whole `tgid`-not-`tid`
  lesson, `vms-9fc`), the ownership/allocation semantics — and every future oracle
  pin would have to be applied twice. That is the failure mode this record exists
  to prevent.
- **Do NOT force-abstract the thin glue:** the char-device/module rind
  (`misc_register` vs `cdevsw`/`devsw_attach`, `module_init` vs `MODULE(9)`, the
  `_IO*` encoding) is genuinely different, genuinely thin, and *already* lives
  per-platform in `vms_module.c` / `vms_netbsd.c`. Sharing it would mean an
  awkward `#if` rind for no payoff. Keep it two small files.

**Be honest about where step 1 is heavier than the epic's "~90% portable, swap 5
primitives" framing suggests:**
1. **The header is the gate, not the `.c`.** No single facility compiles on
   NetBSD until `vms_internal.h`'s embedded primitive *types* are shim-typed
   (Step 0). That is real up-front work before any facility "moves."
2. **The wait/wake path is not a rename.** Converting `wait_event_interruptible`
   (Linux's lock-free idiom) to the cv-idiom (held-lock loop) is code motion at
   every wait site, and getting it subtly wrong is a lost-wakeup race. Bounded
   (eflag has ~6 sites; the whole executive has ~11 wait sites total) but real.
3. **RCU and the two containers (rbtree/hash)** are genuine new NetBSD code, not
   typedefs — deferred to proctab and locks, but they don't vanish.

The good news the measurement *adds* to the epic's optimism: the **proc-model
abstraction does not burden the first facilities** — it is concentrated in
`vms_proctab.c` + device glue, so eflag (Step 1) and the next five facilities
carry no host-task coupling at all. The scary-sounding "every handler takes
`struct vms_proc *`" is defused: they take it, but they only read its *portable*
fields.

---

## 8. Revised decomposition (proposed rd items — conductor files these)

Re-scope the existing epic items and insert the shared-core spine. Wire
`--parent-id vms-8e8`; dependency chain **A → B → C(=re-scoped `vms-4b4`) → rest
→ locks**, with `vms-9dc` (VAX) inheriting the pattern.

| # | Proposed title | One-line outcome | Domain | rd |
|---|---|---|---|---|
| **A** | Kernel-backend shim landed on Linux with zero behaviour change | `exec_kbackend.h` + Linux forwarders + `exec_list/hash/rbtree.h` added; `vms_internal.h` primitive types shim-typed; `vms.ko` behaviour-identical; full CI + Kernel-Executive QEMU gate + `test_runtime_target.sh` green. | Systems | new |
| **B** | Event flags extracted to `src/kernel-core/`, Linux green | `vms_eflag.c` uses only `exec_*` (renames + wait→cv-idiom); moved to `src/kernel-core/`; Linux Debug ctest + QEMU exec gate + runtime-target gate green — proves behaviour-preserving extraction. | Systems | new |
| **C** | *(re-scope `vms-4b4`)* Event flags proven on the shared-core pattern — one source, both kernels | The **same** `vms_eflag.c` compiles into Linux `vms.ko` **and** the NetBSD `vms` module; two-process shared-state (common EF cluster) proof green on NetBSD/amd64 under QEMU; INV-6 honest-fail when `/dev/vms` absent; Linux gate stays green. | Systems + QA | `vms-4b4` |
| **D** | Low-coupling facilities extracted to shared core (ast + access done; **lnm deferred**) | `vms_ast.c` + `vms_access.c` on `exec_*`, moved to `src/kernel-core/`, Linux gate green + `.o` behaviour-identical (only the shim's `exec_copyin/out` 0/EXEC_EFAULT normalization differs from the raw `copy_*_user` test). `vms_access.c` needed **no** privilege shim after all — it gates on the executive's own `proc->cur_privs` mask, never `capable()`. **`vms_lnm.c` NOT extracted (rd vms-5b2):** it is not low-coupling — its arena is shared to userspace by `vmalloc_user`/`remap_vmalloc_range`/`vm_flags_clear`/`struct vm_area_struct` (a memory-mapping seam) and its seqlock uses `smp_wmb()` barriers, none of which the `exec_*` shim covers. Extracting it would drag a whole mm/mmap + barrier seam forward; deferred to a later mm-seam phase. `exec_list_first_entry` added to the shim (ast's DELIVERAST needed it). | Systems | new |
| **E** | Mailbox + device table extracted to shared core | `vms_mbx.c`/`vms_devtab.c` on `exec_*`; both kernels; Linux gate green; mbx wait/wake proven on NetBSD. | Systems | new |
| **F** | Process table + dispatch extracted; proc-model + RCU-lite shim landed | `exec_current_*`/`exec_task_*`/`exec_free_deferred` defined both backends; `vms_proctab.c` + `vms_dispatch.c` (split from `vms_module.c`) shared; NetBSD `cdevsw` rind calls `vms_dispatch`; PCB identity + liveness green on both. | Systems | new |
| **G** | Lock manager extracted to shared core (the 44 KB payoff) | `vms_lock.c` on `exec_*` incl. `exec_rbtree`/`exec_hash`; NetBSD container impls landed; ENQ/DEQ + deadlock detect green on both kernels; Linux authenticity gate unchanged. | Systems | new |
| **H** | *(inherits — `vms-9dc`)* libvmssys VAX backend + ILP32/endian width audit | VAX = **NetBSD backend (already built, G) + `libvmssys` VAX arch backend**, *not* a third executive. `vax--netbsd` GCC cross builds; ILP32 + VAX byte-order audit of `_IO*` encoding, descriptors, RMS, ELF32 activation — executive facility logic untouched (already width-clean). | Systems | `vms-9dc` |

`vms-9dc` (H) explicitly inherits the shared core: once G lands, VAX brings up
**zero** new executive facility code — only the NetBSD backend's arch bits and
the libvmssys VAX syscall backend. That is the entire point of this record.

---

## 9. Summary answers

- **Boundary:** shared substrate-agnostic executive core (all facility logic +
  portable PCB state + shared structs) in `src/kernel-core/`, over one minimal
  shim `exec_kbackend.h`; per-platform glue = the char-device/module rind only,
  in `src/kernel/` (Linux) and `src/kernel-netbsd/` (NetBSD). The host-task
  binding is concentrated in `vms_proctab.c` + device glue, so it is a localized
  shim, not a cross-cutting one.
- **Biggest Linux-refactor risk:** the wait/wake conversion —
  `wait_event_interruptible` (lock-free) → cv-idiom (held-lock loop) — which can
  introduce a lost-wakeup race the QEMU gate may not deterministically catch.
  Mitigate by keeping the Linux `exec_cv_wait` a faithful expansion of what
  `wait_event_interruptible` already compiles to, converting one facility at a
  time, and adding a two-thread wait/wake stress to the executive test.
- **Same-core-both-kernels build:** **YES, with caveats** — freestanding header
  hygiene (no `<linux/…>` in core), OVMX-shipped Linux-API-shaped containers
  (macro-forwarded on Linux, implemented on NetBSD), an RCU-lite seam, and type
  normalization. No core-level `#if`.
- **Phase list:** A shim(Linux, no-op) → B eflag-extract(Linux green) →
  **C = re-scoped `vms-4b4`** (one source, both kernels, NetBSD shared-state
  proof) → D ast/lnm/access → E mbx/devtab → F proctab+dispatch (proc-model+RCU
  shim) → **G locks last (44 KB payoff)** → **H = `vms-9dc`** VAX inherits the
  pattern for free.

## 10. Phase C landing (`vms-4b4`) — what shipped, and four glue decisions

Phase C compiles the SAME `src/kernel-core/vms_eflag.c` into the NetBSD `vms`
pseudo-device that the Linux `vms.ko` builds, and proves it holds real
cross-process state on NetBSD 10.1/amd64 (tests/netbsd P2c). The facility source
is unchanged in substance; the new code is entirely per-substrate glue in
`src/kernel-netbsd/`: the NetBSD backends (`exec_kbackend_netbsd.h`,
`exec_list_netbsd.{h,c}`), the NetBSD struct twin (`vms_internal.h`), and the
`cdevsw`/`d_ioctl` dispatch. Four decisions worth recording:

1. **The copy seam = `_IOWR`; the cdevsw framework owns the user boundary.**
   The facility owns its copy (it calls `exec_copyin` at entry and `exec_copyout`
   at exit on the `arg` it is handed). On Linux `arg` is the raw user pointer and
   those are `copy_*_user`. On NetBSD the event-flag ioctls are **`_IOWR`** (like
   PING and like `src/kernel/vms_ioctl.h`), so the generic cdevsw path copies the
   caller's argument into a kernel buffer BEFORE the driver runs and copies the
   answer back out AFTER — and the driver hands that kernel buffer straight to the
   shared facility. On the NetBSD backend `exec_copyin`/`exec_copyout` are
   therefore **in-kernel copies (`memcpy`)** between the framework buffer and the
   facility's locals; the ONE real user boundary crossing is the framework's, at
   the syscall edge. This is the honest, idiomatic NetBSD integration and is the
   one place the NetBSD backend's copy op differs in mechanism from Linux's
   (`memcpy` vs `copy_*_user`) — a deliberate, documented deviation from §3's
   provisional "`exec_copyin`→`copyin`" note. The considered alternative — encode
   the ioctls `IOC_VOID` to force the raw user pointer through so `exec_copyin`
   could be a literal `copyin` — fights the cdevsw ABI (relying on the exact
   IOC_VOID pointer-passing convention) and buys nothing, since the data still
   crosses the boundary exactly once. Because the encoding is `_IOWR` with the
   same structs and NR bytes as `vms_ioctl.h`, the request NUMBERS are now
   **identical across substrates**. No data is fabricated (a real copy of real
   caller data occurs; a bad user address is rejected by the framework's copyin
   before the facility runs), so this is not the INV-6 silent-fallback class.

2. **One lifecycle addition to the shared core, no-op on Linux.** The facility
   `exec_cv_init`/`exec_lock_init`s each common cluster's `waitq`/`lock` at
   creation but the cluster-free sites called bare `exec_free()` — fine on Linux
   (a wait_queue/spinlock owns no external resource, so `exec_*_destroy` are
   no-ops) but a resource leak / DIAGNOSTIC trip on NetBSD (a live
   `kcondvar`/`kmutex` must be `cv_destroy`/`mutex_destroy`d before free). Phase C
   routes all five cluster-free paths through one `vms_common_ef_free()` helper
   that destroys then frees, and adds the paired `exec_lock_destroy` for the
   list-guard lock in `vms_eflag_cleanup`. This is the intended use of the shim's
   `exec_*_destroy` ops (§3 declares them). **Verified inert on Linux:** the
   compiled `vms_eflag.o` is disassembly-identical (0 instruction diff) to
   origin/main — the helper inlines and the no-op destroys vanish.

3. **Per-pid proc table as glue (a stand-in for Phase F's `vms_proctab`).** The
   facility needs a stable `struct vms_proc` (for the per-process ASCEFC
   associations that point at the shared clusters). A NetBSD `cdevsw` has no
   per-open private data and its `d_close` fires only on the LAST system-wide
   close, so the glue keeps a small module-lifetime table keyed by the calling
   lwp's pid (find-or-create in `d_ioctl`, freed en masse at unload). The shared
   COMMON-cluster state a PERMANENT cluster holds outlives any one proc — which is
   exactly what makes a flag set by an already-exited process visible to a later
   one. Phase F replaces this with the real shared `vms_proctab` + host-task
   binding.

4. **The proof exercises association AND the cv wait/wake, not just a word.** The
   P2c test is VMS-faithful: each op `$ASCEFC`s the well-known PERMANENT common
   cluster first (the facility rejects an unassociated common EFN with
   `SS$_UNASEFC`, never a per-process fake — INV-6). Beyond the A-sets/B-reads
   visibility proof, a waiter BLOCKS in-kernel in `exec_cv_wait` on flag 66 and is
   WOKEN by a DIFFERENT process's `$SETEF` (`exec_cv_broadcast`) — the
   lost-wakeup-free cv contract, held across a process boundary on NetBSD `cv(9)`,
   on the cluster's shared kernel `kcondvar`+`kmutex`.

## 11. Phase F landing (`vms-846b`) — process table + the host-task / RCU-lite / hash seam

Phase F promotes `vms_proctab.c` (the executive-resident process database —
`$SETPRN`, `$GETJPI`, `$PROCSCAN`, `$SETIDENT`, `establish_system`) to
`src/kernel-core/`, where it names **no** `<linux/…>` symbol. It is the first
core facility to bind the **host task**, so it is where the design's three
remaining exec-core seams land. As §2 measured, that binding is concentrated
here and in `vms_module.c` — the seven earlier facilities carried none — so this
is a localized job, not a cross-cutting one. Extraction is behaviour-preserving:
the pure identity/authorisation logic (`vms_proc_may_read`, `proc_fill_info`,
`find_by_name`/`find_by_vms_pid`, `establish_system`) is **`.o`
disassembly-identical** to `origin/main`; the only deltas are the three allowed
seam sites (copy-normalisation, the accounting restructure, the liveness/pin
folding), proven behaviour-identical by the Kernel Executive QEMU suite.

**The four shim families landed (`exec_kbackend.h` §5–§7 + `exec_hash.h`),
Linux backend real, NetBSD backend the contract's real mapping:**

1. **Host-task credential + liveness/accounting.** `exec_current_is_privileged()`
   (Linux `capable(CAP_SYS_ADMIN)`; NetBSD kauth "is-superuser") — proctab's
   *only* `current->` read is this one privilege gate. The UIC/username
   *derivation* from `current`'s credentials (`from_kuid`/`current->real_parent`)
   is **not** here — it lives in `vms_module.c`'s registration and lands with the
   `vms_module.c → vms_dispatch.c` split (§8 row F, second half). The PCB's
   liveness handle `pid_ref` becomes the opaque `exec_task_ref_t`, driven by
   `exec_task_alive` / `exec_task_pin` / `exec_task_read_acct` / `exec_task_unpin`
   (Linux: `pid_task`/`get_task_struct`/the `fill_proc_acct` reads/`put_task_struct`;
   NetBSD: `proc_find(9)` + a documented accounting stub). `exec_task_read_acct`
   returns a substrate-neutral `struct exec_proc_acct`; the VMS unit/field
   conversions stay in the portable facility. **Scope note:** this is a host-task
   *property* read (scalar RSS/cputime), **not** the userspace-arena *mapping*
   seam (`vmalloc_user`/`remap_vmalloc_range`) that `vms_lnm.c` (`vms-d61`) waits
   on — a different, later seam.

2. **RCU-lite (`exec_rcu_read_lock/unlock`, `exec_free_deferred`,
   `exec_rcu_head_t`).** RCU has no NetBSD twin (§2/§3/§5 caveat 3). The
   **read-side/grace-period contract:** the process hash has lockless readers
   (`vms_module.c`'s `vms_proc_find` walks it under an RCU read section, no table
   lock), so an unlinked PCB must survive a grace period. Unlink uses
   `exec_hash_del_rcu` (a node a reader is *already* traversing walks off cleanly;
   a reader starting *after* the unlink cannot reach it); reclaim uses
   `exec_free_deferred` (Linux `call_rcu`; NetBSD immediate, as it has no lockless
   readers). The two are **one idiom** — `exec_hash_del_rcu` then a synchronous
   free is a use-after-free. Proctab's own read-side RCU (the `pid_task` guards)
   is folded *inside* `exec_task_*`, so the facility makes no bare RCU call; the
   deferred-free op is landed and documented but adopted by `vms_module.c`'s
   `kfree_rcu` in that file's later split.

3. **Sleepable mutex (`exec_mutex_t`, `EXEC_DEFINE_MUTEX`, `exec_mutex_*`).**
   Distinct from `exec_lock_t`: proctab's reap serialiser is held across a
   per-victim teardown that may sleep. Linux `struct mutex`; NetBSD adaptive
   `kmutex(9)` at `IPL_NONE`.

4. **Intrusive hash (`exec_hash.h`), the third container seam beside
   `exec_list.h`.** Minimal — only what proctab calls: `exec_hash_node_t`,
   `exec_hash_for_each[_safe]`, `exec_hash_del_rcu`. The table itself
   (`DECLARE/DEFINE_HASHTABLE`, `hash_init`, `hash_add_rcu`, possible-key lookup)
   stays raw in the Linux `vms_internal.h` + `vms_module.c` glue, which the core
   never spells. Linux backend macro-forwards to `<linux/hashtable.h>`; the
   NetBSD backend is the contract + real-mapping sketch (an OVMX/`hashinit(9)`
   intrusive hash), not yet compiled because proctab is not yet in the NetBSD
   `vms` module's SRCS — exactly the state `exec_list.h` was in before Phase C.

**Deliberately flagged residue (a clean partial, per the phase rule).**
`vms_module.c` stays in `src/kernel/` as Linux glue: registration (host-credential
identity derivation), the `struct pid`/`kfree_rcu` free path, the hash
definition, and ioctl dispatch are module-lifecycle host binding, extracted with
the `vms_dispatch.c` split (§8 row F, second half) — not forced into Phase F.
The NetBSD-side proof (proctab compiled into the `vms` module + a two-process
PCB-identity/liveness test, the analogue of Phase C's event-flag proof) is
`vms-9dc`'s job; Phase F fixes the seam boundary and the Linux backend so that
proof brings up **zero** new executive facility code.

**Negctl anchors** repointed `kernel/vms_proctab.c → kernel-core/vms_proctab.c`
(all seven `targets` lines); the five proctab defect seds still hit exactly their
intended line in the moved file, and the coverage meta-check's failure set is
byte-identical to `origin/main` (proctab named, no new gap). The two integration
source-scan gates (`test_system_identity_no_sysuaf{,_negctl}.sh`) that hardcode
the path are repointed and green.
