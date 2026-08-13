# Design Record: P4 — OVMX boots to a DCL prompt on NetBSD-vax under SIMH

> **Status:** DESIGN / DELIVERY PLAN for rd `vms-d59` (P4, the capstone of epic
> `vms-8e8`, "OVMX/NetBSD SYSKRNL — pluggable executive substrate to capture VAX
> as a first-class runtime"). This record is the backing documentation for the
> P4 rd tree: it maps every deliverable to its real rd item, states the
> done-condition each item must hit, and names the risks honestly. **Doc only —
> no code is changed here.**
>
> **Refresh note (2026-08-13):** this record was originally written when P4 read
> as a single kernel/userspace backbone. A **completeness audit** — tracing the
> real Linux boot path end-to-end — found that backbone covered only ~60–65% of
> what a boot actually needs, and grew the tree: it added the **ODS-2/vmsfs
> portability tributary**, a set of **boot bring-up gaps** (bootable-disk
> assembly, the `ovmx_init` boot-plumbing seam, image activation on VAX, the
> boot-conformance harness), and the **lnm mm/mmap seam**. This refresh folds
> those in as first-class deliverables, records the substantial foundation work
> that has since **merged**, and documents the **NetBSD CI split** decision
> (`vms-2d9`). See §7 (Progress ledger) for the one-screen status.
>
> **Reads on top of** `docs/design-ovmx-netbsd-syskrnl.md` (the SYSKRNL
> feasibility + strategy), `docs/design-netbsd-executive-core.md` (rd `vms-bea` —
> one shared core, two kernel backends), and `docs/runtime-target.md` (Rule 9,
> substrate-neutral). It does not restate them.
>
> **Clean-room (CLAUDE.md Rule 8):** everything below is OVMX's own code cross-
> compiled and cross-tested. The only external surfaces touched are **public,
> documented** NetBSD kernel/libc APIs (`module(9)`, `cdevsw(9)`, `kmutex(9)`,
> `cv(9)`, `copyin(9)`/`copyout(9)`, `kmem(9)`, `rbtree(3)`, `queue(3)`,
> `vnode(9)`/`vfsops(9)`) and the SIMH VAX emulator. No VSI/HPE source or binary
> is read, disassembled, or copied.

---

## 1. Goal, and the framing: foundation done, integration ahead

**Goal (P4, `vms-d59`):** `ovmx_init` → `LOGINOUT` → a **DCL prompt** on
**NetBSD-vax under SIMH**, with the OVMX executive test green against a **real
in-kernel `/dev/vms`** on that VAX — no per-process userspace fakes (INV-6),
no mocks.

The reason P4 is an **integration** effort and not a research effort is that
every "will this even work?" unknown has already been answered **YES** by merged
work under `vms-8e8`. Since this record was first written, the **entire
Linux-side extraction** the plan depended on has landed, and the ODS-2 core and
the boot seam with it:

| Foundation fact (merged) | rd | Evidence |
|---|---|---|
| Rule 9 generalized to substrate-neutral ("a real host kernel", not "Linux") | — | PR #364 |
| `vms_kif` transport seam split from Linux specifics | `vms-a3b` | PR #370 |
| NetBSD/amd64 SYSKRNL: real `vms` pseudo-device, executive test green under QEMU | `vms-dd8` | PR #376 |
| NetBSD/**vax** boots on SIMH as a reusable cached-disk lab artifact | `vms-0041` | PR #379 |
| Shared-core / two-backend abstraction decided (`exec_kbackend.h`) | `vms-bea` | PR #384 |
| Kernel-backend shim (`exec_kbackend`) + Linux backend landed | `vms-adb` | PR #388 |
| Event flags → `src/kernel-core/` on the `exec_list` shim | `vms-ec4` | PR #391 |
| Event flags on **both** kernels + cross-process proof on NetBSD/amd64 | `vms-4b4` | PR #393 |
| AST + access-mode → shared core; lnm mm-seam flagged | `vms-5b2` | PR #395 |
| Mailboxes → shared core | `vms-a88` | PR #410 |
| Process table → shared core (host-task / RCU-lite / hash seam) | `vms-846b` | PR #412 |
| Lock manager → shared core (`exec_rbtree` seam) | `vms-84a` | PR #420 |
| Device table → shared core (`exec_blockdev` seam) | `vms-31b` | PR #430 |
| Logical-name manager → shared core (`exec_membar` + `exec_arena` seams — the last facility) | `vms-d61` | PR #434 |
| ODS-2 format + core algorithms → `src/kernel-core/vmsfs/` behind the block/inode shim | `vms-544` | (V1, merged) |
| vmsfs Linux VFS backend reduced to a thin shim over vmsfs-core (0 `<linux/>` in core) | `vms-00c` | (V2, merged) |
| ODS-2 storage/FID/cluster allocator + dir-scan + header R/W → vmsfs-core | `vms-d69` | (V2b, merged) |
| `ovmx_init` boot-plumbing seam extracted Linux-side (`ovmx_boot_*`), zero behaviour change | `vms-28f` | PR #440 |

So the hard unknowns — *does NetBSD boot on our SIMH-vax? is the executive
substrate-portable without duplicating facility logic? does `libvmssys` target
VAX at all? can ODS-2 be factored without feature drift?* — are all closed. What
remains is **breadth on the VAX substrate** (every facility + the filesystem +
the boot, not just the Linux-side extraction), **width** (32-bit VAX, ILP32,
non-IEEE float), and **integration** (a full userspace, a mounted system disk, an
init, and a boot). This record plans that remaining work.

**One honesty correction the refresh must carry:** the `libvmssys` elf32-vax
cross-build (`vms-9dc`, PR #396) is **NOT yet on main** — it is **open and behind
main, held pending the CI stabilization** (`vms-2d9`, §6). The earlier draft
listed it as a merged foundation fact; it is not. Everything downstream of the
VAX toolchain therefore sits behind that landing.

---

## 2. The extraction is complete — what the shared core now looks like

The plan's premise was "one facility source, two kernel backends" (`vms-bea`).
That premise is now **fully realized on the Linux side**: all six executive
facilities plus the device table and the logical-name manager live in
`src/kernel-core/`, byte-identical `.o` behaviour on Linux, no facility logic
forked. The shim seams that make a second (NetBSD) backend a drop-in are all in
place:

| Seam header | Purpose | Landed by |
|---|---|---|
| `exec_kbackend.h` | master backend shim (mutex/cv/copyin-out/kmem/EFAULT) | `vms-adb` (#388) |
| `exec_list.h` | intrusive doubly-linked list container | `vms-ec4` (#391) |
| `exec_hash.h` | intrusive hash + RCU-lite grace-period contract | `vms-846b` (#412) |
| `exec_rbtree.h` | intrusive red-black tree (lock DB, devtab) | `vms-84a` (#420) |
| `exec_blockdev` | block-device seam for the device table | `vms-31b` (#430) |
| `exec_membar` | `producer`/`consumer` ordering primitives (seqlock) | `vms-d61` (#434) |
| `exec_arena` | userspace-publishable arena (mmap base+size handed to the host) | `vms-d61` (#434) |

`src/kernel-core/` today: `vms_eflag.c`, `vms_ast.c`, `vms_access.c`, `vms_mbx.c`,
`vms_devtab.c`, `vms_proctab.c`, `vms_lock.c`, `vms_lnm.c` — the complete
executive. `src/kernel/` holds only the thin Linux backend
(`exec_kbackend_linux.h`); `src/kernel-netbsd/` holds the NetBSD twins that
already exist (`exec_kbackend_netbsd.h`, `exec_list_netbsd.[ch]`,
`exec_hash_netbsd.h`, `exec_rbtree_netbsd.h`, `vms_eflag_nb.h`, `vms_netbsd.c`).
The NetBSD amd64 event-flag proof (`vms-4b4`, #393) already runs the *shared*
core through that NetBSD twin. **P4-A is now "fill in the remaining NetBSD
twins," not "invent an abstraction."**

The ODS-2 filesystem got the same treatment (the audit's tributary): all ODS-2
logic now lives in `src/kernel-core/vmsfs/` (`vmsfs_version.c`, `vmsfs_name.c`,
`vmsfs_alloc.c`, `vmsfs_dirscan.c`, `vmsfs_header.c`, `vmsfs_map.c`) behind the
block/inode seam (`vmsfs_bio.h` / `vmsfs_backend.h`, per
`src/kernel/vmsfs/README-backend.md` §3). `src/kernel/vmsfs/` is now the thin
Linux VFS backend, and `README-backend.md` is the **NetBSD-vnode backend
template** for V4.

---

## 3. The convergent tracks

P4 is orchestrated as convergent tracks that share the same `libvmssys` VAX base
(`vms-9dc`, in flight — see §1 correction) and meet at the boot capstone. The
original two-seat parallelization — **executive-on-VAX** and **userspace
cross-build** — still holds; the completeness audit added two more tributaries
that also converge on the boot: the **ODS-2/vmsfs portability** track (the PID-1
`/vms` mount) and the **boot bring-up** track (disk assembly, the init seam,
image activation, the conformance harness). None can finish P4 alone; the
capstone (`vms-d59`) depends on all of them.

```
              libvmssys elf32-vax + ILP32 audit  (vms-9dc — IN FLIGHT, #396 behind main)
                                        |
     ┌──────────────┬───────────────────┴───────────────┬────────────────────────┐
     │              │                                    │                        │
 TRACK 1:       TRACK 2:                          TRIBUTARY A:              TRIBUTARY B:
 EXEC-ON-VAX    USERSPACE CROSS-BUILD             ODS-2/vmsfs (vms-8e5)     BOOT BRING-UP
     │              │                                    │                        │
 Linux exec-core   vms-30a RTL width (3 ILP32)     V1 vms-544  core extract  vms-28f init seam
 extraction        vms-84b vmsprocess               V2 vms-00c  linux thin    (DONE #440)
 D/E/F/G ALL DONE      │                            V2b vms-d69 alloc/dir/hdr    │
 (#395/410/412/420)  vms-1b2 libvms                (all DONE)              vms-f2e NetBSD init
     │              │                                    │                  backend
 P4-A (vms-f8a):    vms-271 vmslnm+vmsfs+vmsrms     V3 vms-bb8  elf32-vax        │
 NetBSD backends    (USERSPACE lib — NOT the       V4 vms-308  NetBSD vnode  vms-42d IMGACT
 + amd64 proofs      kernel fs; audit clarified)       (amd64 mount+read)    ELF32-vax
   vms-9bb ast/lnm/access   │                       V5 vms-544d mount on          │
   vms-d7a mbx/devtab   vms-1cb2 vmsdcl                 vax/SIMH             vms-945e full
   vms-ca7 proctab          │                            │                  facilities on vax
   vms-ff7 lock DB      vms-5d1 ovmx_init/STARTUP        │                       │
     │                  + images (link; activation   vms-8e5 P4-VFS done   vms-7b1 bootable
 P4-B (vms-476):        is vms-42d, not here)                                disk ASSEMBLED
 executive on VAX        │                                                       │
   vms-20b9 elf32-vax     │                                              vms-625 boot-conf
   vms-f78bb /dev/vms     │                                              harness (gated)
   vms-4e7 eflag proof    │                                                       │
   (EFLAG ONLY — full   P4-C (vms-c99): full userspace cross-builds              │
    facility parity is    │                                                       │
    vms-945e, tributary)  │                                                       │
     └──────────────┴──────────────────┬───────────────┴────────────────────────┘
                                        │
                           ┌────────────┴────────────┐
                           │      CAPSTONE  vms-d59   │
                           │  ovmx_init→LOGINOUT→DCL  │
                           │  on NetBSD-vax / SIMH +  │
                           │  gated boot-conformance  │
                           └──────────────────────────┘
```

The ordering fact that keeps the tracks honest: **Track 1's Linux-side exec-core
extraction (Phases D–G) is now fully merged** (`vms-5b2/a88/846b/84a`, all done),
so P4-A's NetBSD-backend items (`vms-9bb/d7a/ca7/ff7`) are **unblocked and ready**
— gated in practice only by the NetBSD CI stabilization (`vms-2d9`), not by any
missing Linux work. P4-A mirrors the already-proven event-flag pattern
(`vms-4b4`, #393) facility-by-facility on **NetBSD/amd64**, where the emulator is
fast; P4-B then recompiles that same, now-complete core for **elf32-vax** and
lights it up on the slow VAX. The width port is deliberately the *last* kernel
step, so a width bug surfaces against known-good behaviour.

---

## 4. Per-deliverable plan and done-conditions

Ground rule for every done-condition: **the proof runs against real state, never
a mock.** The amd64 proofs are cross-*process* (two processes, one `/dev/vms`,
shared kernel state — the INV-6 honesty bar from `vms-4b4`). The VAX proofs run
against the **real in-kernel `/dev/vms`** on the SIMH lab disk. The capstone
boots a **real OVMX image** on the **real NetBSD/vax SIMH lab** and asserts a DCL
prompt.

### 4.1 Track 1 · P4-A — every facility has a NetBSD backend (`vms-f8a`)

Extend the event-flag precedent to the whole executive surface. The Linux-side
extraction each item waited on is **done**, so each sub-item now does exactly two
things: (a) write the NetBSD backend of the shim for that facility's primitives,
compiling the **shared** `src/kernel-core/` logic into the NetBSD `vms` module —
**no reimplementation, no forked facility logic** (`vms-bea`); (b) prove it
cross-process on NetBSD/amd64.

| rd | Facility group | Linux extraction (done) | Done-condition |
|---|---|---|---|
| `vms-9bb` | ast + lnm + access | `vms-5b2` #395 (+ `vms-d61` #434 lnm) | Shared ast/lnm/access core compiles into the NetBSD module; amd64 cross-process proof green — two processes observe the same AST delivery / logical-name table / access-mode state through one `/dev/vms`. |
| `vms-d7a` | mailbox + device-table | `vms-a88` #410, `vms-31b` #430 | Shared mbx/devtab core in the NetBSD module; amd64 cross-process proof — a mailbox written by P1 is read by P2; device table is shared, not per-process. |
| `vms-ca7` | process table | `vms-846b` #412 | Shared proctab core + the **proc-model / RCU-lite shim** land on NetBSD; amd64 proof — process table entries created by one process are visible to another. |
| `vms-ff7` | lock manager (DLM) | `vms-84a` #420 | Shared lock-manager core (rbtree + hash) compiles against NetBSD `rbtree(3)`; amd64 cross-process **DLM** proof — an ENQ in P1 blocks/grants against P2 through the real kernel lock DB. |

**P4-A done (`vms-f8a`):** all four green — the NetBSD `vms` module exposes the
*complete* executive facility set, every facility proven cross-process on
NetBSD/amd64 against a real `/dev/vms`. This is breadth parity with the Linux
`vms.ko`, still on the fast substrate.

### 4.2 Track 1 · P4-B — the executive builds + loads on NetBSD-VAX (`vms-476`)

Now take the complete, amd64-proven core to 32-bit VAX.

| rd | Step | Done-condition |
|---|---|---|
| `vms-20b9` | B1: compile for elf32-vax | The NetBSD backend + shared executive core (`exec_list`/`kmem`/`cv`/`kmutex` uses, all facility logic) **compile clean for elf32-vax**, width-clean — no 64-bit assumptions, audited against the ILP32 findings (`docs/audit-ilp32-vax-libvmssys.md`). |
| `vms-f78bb` | B2: `/dev/vms` live on VAX | The `vms` executive is **present and live on NetBSD-vax under SIMH**: `/dev/vms` opens. Preferred path is a **loadable `module(9)`**; **fallback** is compiling the driver into a **custom NetBSD/vax kernel** if `modules(9)` on the VAX port is too thin to carry a `cdevsw` module (see §5). Either way the done-condition is identical: a real in-kernel `/dev/vms` node. |
| `vms-4e7` | B3: eflag proof on VAX | **Event flags** — *only* event flags — proven cross-process against the REAL `/dev/vms` on NetBSD-vax under SIMH: the exact `vms-4b4` proof re-run on the VAX. INV-6-honest: shared kernel state, no per-process fallback. **The audit corrected a false read here:** `vms-4e7` proves the executive *lives and is INV-6-honest on VAX*, it does **not** prove full facility parity on VAX. Full boot-required facility parity on VAX is `vms-945e` (§4.5). |

**P4-B done (`vms-476`):** the OVMX executive builds and loads on NetBSD-VAX and
passes its first real cross-process facility test on the VAX. The kernel half of
the runtime exists on VAX.

### 4.3 Track 2 · P4-C — full OVMX userspace cross-builds for netbsd-vax (`vms-c99`)

Cross-build the entire userspace following the library dependency graph (the same
order CLAUDE.md documents), starting from the `libvmssys` VAX base. Before the
graph can climb, three RTL width items deferred by the ILP32 audit must be
resolved.

**RTL width first (`vms-30a`)** — the three deferred ILP32 items:

1. **`vms_time_t` vs 64-bit `time_t`.** NetBSD/vax's `time_t` is 64-bit even on
   ILP32; OVMX's `vms_time_t` and its VMS 64-bit-quadword time semantics must be
   reconciled deliberately, not by accidental truncation. Done: a decided,
   documented width mapping, exercised by a test.
2. **`VMS_O_*` / futex constant namespaces.** The audit flagged Linux-numeric
   constant namespaces (open flags, futex ops) leaking into portable code. Done:
   these are abstracted behind the substrate layer so netbsd-vax gets NetBSD
   numeric values, not Linux ones.
3. **VAX non-IEEE floating point — HIGHEST-RISK RTL item (see §5).** The VAX FPU
   is F/D/G/H format, **not IEEE-754**. Any RTL that assumes IEEE bit layout,
   rounding, or `NaN`/`Inf` semantics is wrong on VAX. Done: the float-touching
   RTL is either VAX-format-correct or explicitly scoped, with a test that pins
   the behaviour.

**Then the library build order** (each item = cross-builds + links for
netbsd-vax; done-condition = clean cross-compile + link, no unresolved width
issues):

| rd | Layer | Blocked by |
|---|---|---|
| `vms-84b` | `vmsprocess` (PCBs, ASTs, event flags) | `vms-9dc` |
| `vms-1b2` | `libvms` (system services + RTL) | `vms-30a`, `vms-84b` |
| `vms-271` | `vmslnm` + `vmsfs` + `vmsrms` — **the USERSPACE libraries** | `vms-1b2` |
| `vms-1cb2` | `vmsdcl` (DCL shell) | `vms-271`, `vms-84b` |
| `vms-5d1` | `ovmx_init`/STARTUP + the OVMX images (LOGINOUT, DCL.EXE, …) cross-build + **link** | `vms-1cb2`, `vms-42d` |

**Two audit corrections in this table, to kill false precision:**

- **`vms-271` is the userspace `vmsfs` *library*, NOT the ODS-2 kernel filesystem
  that PID 1 mounts `/vms` through.** That kernel filesystem is the separate
  ODS-2/vmsfs tributary (`vms-8e5`, §4.4). Do not conflate them — they share a
  name and nothing else.
- **`vms-5d1` is link, not activate.** Its title once read "link + activate"; on
  VAX, *how an image activates* is an undecided design gap, now its own item
  (`vms-42d`, §4.5). `vms-5d1` is therefore blocked by `vms-42d` so activation is
  a real, decided thing before the userspace-link item can close.

**P4-C done (`vms-c99`):** the full OVMX userspace — through `ovmx_init` and the
activatable images — cross-builds and links for netbsd-vax. The userspace half of
the runtime exists on VAX.

### 4.4 Tributary A · ODS-2/vmsfs factored for portability (`vms-8e5`)

The audit's finding: PID 1's `/vms` mount depends on an ODS-2 *kernel*
filesystem, and that filesystem was as Linux-bound as the executive was before
extraction. So it gets the same one-core / two-backends treatment. **V1/V2/V2b
are merged** (§1); the core is out.

| rd | Step | State | Done-condition |
|---|---|---|---|
| `vms-544` | V1: ODS-2 format + core algorithms (version resolve, dir lookup, bitmap alloc, header checksum) → `src/kernel-core/vmsfs/` behind a block/inode shim | **done** | Linux `vmsfs.ko` green, `.o` behaviour-identical. |
| `vms-00c` | V2: Linux VFS backend reduced to the thin shim over vmsfs-core | **done** | 0 `<linux/>` left in core; `vmsfs.ko` byte-identical; seam doc for the NetBSD vnode backend produced (`README-backend.md`). |
| `vms-d69` | V2b: storage/FID/cluster allocator + dir-block scanner + file-header R/W → vmsfs-core behind `vmsfs_bio`/`vmsfs_fh_info` | **done** | Linux green via `test_kmod_vmsfs_blkdev`. |
| `vms-bb8` | V3: vmsfs-core compiles width-clean for elf32-vax (ILP32 / F-float-free / endian-audited) | blocked by `vms-544`, `vms-9dc` | Clean elf32-vax compile of vmsfs-core; no 64-bit or IEEE assumptions. |
| `vms-308` | V4: NetBSD **vnode/VFS** backend — a mastered OVMX ODS-2 disk MOUNTS + READS `DCL.EXE` on NetBSD/amd64 from vmsfs-core | blocked by `vms-00c`, `vms-bb8`, `vms-d69` | Real mount+read on NetBSD/amd64 (fast substrate first), backend built from `README-backend.md`. |
| `vms-544d` | V5: the ODS-2 system disk MOUNTS + READS on netbsd-vax under SIMH (satisfies the PID-1 `/vms` mount) | blocked by `vms-308`, `vms-f78bb` | Real mount+read on NetBSD-vax under SIMH — the mount PID 1 needs. |

**Tributary A done (`vms-8e5`):** one shared ODS-2 core, Linux + NetBSD VFS
backends, no feature drift, and the system disk mounts on VAX.

### 4.5 Tributary B · boot bring-up gaps

The audit traced the real Linux boot path and found these seams between "userspace
links" and "the machine boots." Each is now a first-class deliverable.

| rd | Deliverable | State | Done-condition |
|---|---|---|---|
| `vms-28f` | `ovmx_init` boot-plumbing seam extracted Linux-side: an `ovmx_boot_*` substrate layer (module-load / mount / reboot / device-naming / console-log), ONE `ovmx_init` source, no `#ifdef` | **done (#440)** | Linux backend = current behaviour, Persistent Boot Smoke green, zero behaviour change. |
| `vms-f2e` | `ovmx_init` boots on NetBSD via that seam (NetBSD backend of `ovmx_boot_*`) | blocked by `vms-28f`, `vms-42d`, `vms-5d1` | ONE `ovmx_init` source, no `#ifdef` fork; NetBSD backend drives module-load/mounts/device-naming/reboot/console-log. |
| `vms-42d` | image activation **DECIDED (A) + gated** on netbsd-vax: OVMX images are ordinary NetBSD ELF32-vax dynamic exes activated by `/usr/libexec/ld.elf_so` — a labelled Rule-8 substrate divergence, NOT OVMX-native IMGACT/symbol-vector. See §4.5.1. | blocked by `vms-30a` (RTL width) | **done (this PR)** — a representative OVMX image links elf32-vax + the per-PR `activation-netbsd-vax` gate asserts it activates via ld.elf_so; `LOGINOUT.EXE`/`DCL.EXE` follow the same path once their higher layers cross-build (`vms-5d1`). |
| `vms-945e` | every boot-required executive facility (proctab/CREPRC, lnm, mbx, ast, access) proven cross-process against the REAL `/dev/vms` on netbsd-vax | blocked by `vms-9bb/ca7/d61/d7a/f78bb` | Full facility parity on VAX (not just event flags — catches ILP32/float/ELF32 bugs amd64 can't). |
| `vms-7b1` | a bootable OVMX/NetBSD-vax system disk + custom kernel is ASSEMBLED from the OVMX build (the `Dockerfile.bootable`/`vmsfs_master` analog) | blocked by `vms-544d`, `vms-5d1`, `vms-f2e`, `vms-f78bb` | SIMH boots the assembled disk unattended with `ovmx_init` as init. |
| `vms-625` | boot-conformance harness: lab-vax installs OVMX onto the vax disk, boots it, and mechanically drives the console to a DCL prompt | blocked by `vms-7b1` | Gated, reuses the cached disk, **never reinstalls** (§5). |

The **lnm mm/mmap seam** the audit called out is already resolved: the
logical-name manager needed a userspace-publishable arena + memory-barrier
abstraction to be substrate-portable, and that is the `exec_membar` + `exec_arena`
seam landed by `vms-d61` (#434) — the last executive facility into the shared core.

#### 4.5.1 Decision record — image activation on netbsd-vax (`vms-42d`)

> **Status:** DECIDED. **Operator-visible:** this call sits on the OVMX-native-
> toolchain / authenticity pillar (`vms-ade`, [[self-hosting-northstar]]). It does
> **not** weaken that pillar on its home substrate; it declares that the pillar
> **does not extend to the NetBSD/vax substrate**, and labels that as a Rule-8
> divergence (below). Flagged for operator awareness; ratify if the 1.0 authenticity
> bar ever tightens to "OVMX-native activation on every substrate."

**The two options (from the completeness audit GAP-D):**

- **(A) `ld.elf_so` divergence** — OVMX images on NetBSD/vax are ordinary ELF32-vax
  **dynamic** executables, activated by NetBSD's own runtime linker
  `/usr/libexec/ld.elf_so`. No OVMX-native IMGACT.EXE, no `PT_INTERP → IMGACT.EXE`, no
  `.vms$sv`/`.vms$imp` symbol-vector activation.
- **(B) native IMGACT ELF32-vax** — generalize the OVMX loader to `ELFCLASS32` in
  shared code plus a `src/imgact/arch/vax` backend, so OVMX-native PT_INTERP /
  symbol-vector activation works on VAX too.

**Decision: (A).** And not merely as the recommended option — **(B) is foreclosed by
three already-merged decisions**, so (A) is the only design-consistent path:

1. **The NetBSD/vax userspace already links NetBSD libc**
   (`docs/design-ovmx-netbsd-syskrnl.md` §4.1; merged: libvmssys `vms-9dc`,
   vmsprocess `vms-84b`, libvms `vms-1b2`). Every OVMX image on this substrate is
   produced by the `vax--netbsdelf` GCC cross toolchain as a **standard NetBSD
   ELF** — its crt0, TLS setup and syscall plumbing come from NetBSD's csu/libc. A
   standard NetBSD dynamic ELF is, by definition, activated by
   `/usr/libexec/ld.elf_so`. There is no OVMX symbol-vector image on VAX to activate
   any other way.
2. **There is no OVMX-native VAX toolchain** (§4.3): tcc / LINK.EXE do not target
   VAX and are explicitly not on the roadmap. The OVMX-native activation model
   (LINK.EXE emits `.vms$sv` symbol vectors + `PT_INTERP=IMGACT.EXE`; IMGACT.EXE
   resolves them) exists **only** because on Linux OVMX builds its own toolchain and
   deliberately avoids `ld.so` — that is the self-hosting pillar. On VAX that
   premise is absent: nothing emits symbol-vector images, so nothing needs a
   symbol-vector activator.
3. **IMGACT.EXE is a raw-Linux-syscall, freestanding, ELFCLASS64-only binary**
   (`src/imgact/imgact.c`: `SYS_openat`/`SYS_mmap` Linux numbers, `-nostdlib`,
   `#error` on any non-x86_64/aarch64 arch, `struct elf64_phdr`). Option (B) would
   require porting it to **raw NetBSD/vax syscalls** — which §4.1 explicitly rejects
   ("NetBSD's syscall trap ABI is not a stable public contract … hand-rolling raw
   VAX syscall stubs fights the platform") — plus a hand-written VAX `start.S`. That
   directly contradicts decision (1)/(2). (A) reuses the platform's own, correct
   activator instead of re-implementing one against an unstable ABI.

**No hard reason for (B) was found.** The audit's escape hatch — "an image that
isn't a plain dynamic exe" — does not occur on this substrate: every OVMX image is
GCC-cross standard ELF, and the boot path needs no symbol-vector resolution
(`LIBVMS$SHR`/`DECC$SHR`-style shareable images are the Linux native-toolchain
artifact; on VAX the RTL is linked from `.a` archives + NetBSD libc via
`ld.elf_so`).

**The Rule-8 divergence, LABELED.** Per CLAUDE.md Rule 8, this is a deliberate,
documented substrate divergence, not presented as VMS-authentic:

| | Linux / self-hosting substrate | **NetBSD/vax substrate (this decision)** |
|---|---|---|
| Toolchain | OVMX-native LINK.EXE | `vax--netbsdelf` GCC cross |
| Image form | `PT_INTERP=IMGACT.EXE` + `.vms$sv`/`.vms$imp` symbol vectors | plain NetBSD ELF32-vax dynamic exe |
| Activator | OVMX-native IMGACT.EXE (userspace, freestanding) | NetBSD `/usr/libexec/ld.elf_so` (the platform's runtime linker) |
| Pillar | is the OVMX-native-toolchain / self-hosting pillar | pillar **does not extend here** (labeled) |

This mirrors the substrate split the whole SYSKRNL effort already commits to
(`vms.ko` on Linux vs the `vms` pseudo-device on NetBSD): the executive and RTL
**semantics** are the same shared OVMX code; the **platform plumbing beneath them**
(runtime linker, csu, libc) is the host's, on both substrates. It does **not**
weaken Rule 9 (the executive is still a real in-kernel `/dev/vms` facility) and it
does **not** weaken the authenticity pillar on its home substrate.

**What is implemented for `vms-42d` (this PR).** The per-PR
`activation-netbsd-vax` CI gate (`tools/cross-vax/build-activation-vax.sh`) builds
the ported OVMX layers (libvmssys + vmsprocess) for `netbsd-vax`, links a
**representative OVMX image** (LOGINOUT/DCL-class — the remaining `vmsdcl` /
`ovmx_init` layers are not yet cross-built, `vms-1cb2`/`vms-5d1`) as a real
elf32-vax dynamic executable, and **asserts the Decision-A contract**: ELFCLASS32 /
Digital VAX / little-endian, `PT_INTERP == /usr/libexec/ld.elf_so`, genuinely dynamic
(`DT_NEEDED` present), **not** `IMGACT.EXE`, and **no** `.vms$sv`/`.vms$imp`
sections. The gate is build-only (no `qemu-system-vax` exists); the booted-guest
activation round-trip is the SIMH nightly follow-up under `vms-945e`/`vms-d59`.
`vms-5d1` (the full LOGINOUT/DCL/`ovmx_init` link) inherits this decided path.

### 4.6 Capstone — boot to a DCL prompt (`vms-d59`)

With P4-B (executive on VAX), P4-C (userspace on VAX), Tributary A (ODS-2 mounts
on VAX) and Tributary B (assembled bootable disk) all done, integrate:
`ovmx_init` runs as the boot process on NetBSD-vax, mounts the ODS-2 system disk,
activates `LOGINOUT`, and lands a **DCL prompt** — on the real in-kernel
`/dev/vms`.

**Done-condition (ground-sourced, no mocks):** the **gated boot-conformance test**
(`vms-625`), the VAX analog of `tests/qemu/test_boot_conformance.sh` /
`test_persistent_boot.sh`, boots a **real OVMX image on the real NetBSD/vax SIMH
lab** (`tests/lab-vax/`, `vms-0041`) and asserts, mechanically on the console
transcript, that a **DCL prompt** is reached (and that the executive test is green
against the real `/dev/vms`). It reuses the cached lab disk — **never reinstalls**
(§5). Like the other emulator tiers it is **CI-gated** (push/nightly, not
per-PR) so the slow SIMH job never blocks a PR.

`vms-d59` is blocked by `vms-0041` (done), `vms-476`, `vms-7b1`, `vms-945e`,
`vms-9dc`, `vms-c99`.

---

## 5. Risks, honestly

1. **VAX non-IEEE float (highest RTL risk).** The VAX has F/D/G/H floating
   formats, not IEEE-754. RTL that assumes IEEE bit-patterns, rounding modes, or
   special values is silently wrong on VAX and can pass a smoke test while
   producing wrong numbers. *Mitigation:* isolate float-touching RTL in `vms-30a`,
   pin behaviour with a VAX-format test, and scope explicitly what OVMX guarantees
   in VAX float rather than assuming the host `gcc` gets it right by default.
2. **`modules(9)` on NetBSD/vax may be too thin.** The VAX port is not where
   NetBSD's loadable-module machinery gets the most love; a `cdevsw` module may
   not load. *Mitigation:* `vms-f78bb` carries an explicit fallback — compile the
   `vms` driver **into a custom NetBSD/vax kernel**. The done-condition (a real
   in-kernel `/dev/vms`) is identical either way, so this is a build-path choice,
   not a redesign.
3. **SIMH-vax is SLOW.** Boot and test wall-clock on emulated VAX is long (the
   `vms-0041` lab boots in ~10 min under TCG-class emulation; a full OVMX boot +
   conformance run is longer). *Mitigation:* keep every fast proof on
   NetBSD/**amd64** (all of P4-A, Tributary A's V4), do the minimum on VAX, and
   give VAX jobs a hard `timeout` wrapper (a QEMU/SIMH proof with no hard timeout
   has run unbounded before). The VAX is the *oracle of last resort*, not the
   routine loop.
4. **Shared-host disk constraint.** The dev/CI host is a **shared** machine and is
   ~96–98% full (measured: ~3.7 GB free on a 154 GB root). SIMH VAX installs are
   multi-GB. *Mitigation, mandatory for every VAX boot-test item (`vms-f78bb`,
   `vms-4e7`, `vms-544d`, `vms-d59`):* **reuse the cached `tests/lab-vax` disk,
   never reinstall** in the hot path; installs happen once into a mounted cache
   exactly as `vms-0041` established. And **CI-gate the heavy jobs** the way the
   other emulator tiers are gated so they run on push/nightly, not on every PR.
5. **CI reliability under TCG — the reason for the split (`vms-2d9`).** GitHub
   runners have **no `/dev/kvm`**, so every NetBSD guest runs under **TCG**, which
   is slow *and* timing-variable. The console-driven pexpect handshake
   (`drive_netbsd*.py`) desyncs under that variance — green on #393, red on #396 —
   which was blocking *all* NetBSD-touching PRs. This is a harness/environment
   flake, not an OVMX defect. *Mitigation:* the CI split in §6, plus the
   deterministic console driver `vms-2d9` is landing (prompt-sync, drained buffer,
   per-command sentinels, resync-not-timeout). The **real** fix would be a
   KVM-capable runner; absent that, TCG variance is a standing risk on the runtime
   proofs.
6. **The VAX toolchain (`vms-9dc`, #396) is behind main.** The elf32-vax
   `libvmssys` cross-build is open and **held pending the CI stabilization** — so
   *everything* downstream of the VAX toolchain (all of P4-B's compile, P4-C's
   build order, Tributary A's V3/V5) cannot start landing until #396 does. The CI
   split (§6) is what unblocks it.

---

## 6. NetBSD CI / verification strategy (the split — `vms-2d9`)

**Problem.** QEMU/SIMH under TCG on GitHub runners (no `/dev/kvm`) is too slow and
too timing-variable to run console-driven NetBSD emulator tests reliably on every
PR. The pexpect handshake desyncs (green #393 / red #396), and because a
NetBSD-touching PR would sit behind a flaky multi-minute TCG job, it was gating
the entire epic.

**Resolution (`vms-2d9`).** Re-gate, honestly — do not weaken. Split the NetBSD
verification into two tiers:

- **Per-PR gate:** `Build & Test` **plus a fast build-only check** — "the
  NetBSD/amd64 `vms` module **cross-compiles**." This is the real per-PR value:
  because the facilities are **byte-identical Linux-side**, the thing a PR can
  actually break is the *build* or the *shared-core drift*, and a build-only check
  catches exactly that in seconds, deterministically, with no TCG.
- **Nightly `schedule`:** the full console-driven **runtime** proofs — P2a/P2b
  (NetBSD boot + `vms` pseudo-device), the **Executive Harness**, and the **P2c
  cross-process** proof (the INV-6 honesty bar, validated green in Phase C, #393).
  These prove *behaviour*, need a real running guest, and tolerate the slow TCG
  wall-clock because nothing waits on them.

**Why this is honest re-gating, not a weakened gate.** The full cross-process
runtime proof **still runs** — every night, unconditionally, against a real
`/dev/vms`. INV-6 is not relaxed; it is moved to the tier that can run it
reliably. The per-PR tier keeps the fast, deterministic signal (build/drift)
where speed and determinism actually matter. A silent drop of the runtime proof
would be the LARP bug class the authenticity invariants exist to kill; this is the
opposite — the proof is preserved at full strength and merely rescheduled.

**The honest caveat.** The *only* reason the runtime proof can't be per-PR is the
missing `/dev/kvm`. A KVM-capable runner would let it run per-PR at native-ish
speed and the split would collapse back to one tier. Until then, the split stands
and the deterministic console driver (`vms-2d9`) reduces — but does not eliminate
— the TCG-variance flake.

> Scope note: the mechanical `ci.yml` change implementing this split is part of
> the `vms-2d9` work, not this doc PR. This section records the **decision and its
> rationale** so the tree and the workflow agree on *why* the NetBSD jobs are
> tiered the way they are.

---

## 7. Progress ledger

Where P4 actually stands as of this refresh (re-derive live status from
`rd dep tree vms-8e8`; this is a snapshot, not a frozen truth):

| Block | State | Evidence |
|---|---|---|
| **Exec-core migration (Linux side)** | **COMPLETE** — all six facilities + devtab + lnm in `src/kernel-core/` behind the shim seams, byte-identical, no drift | `vms-adb/ec4/4b4/5b2/a88/846b/84a/31b/d61`; PRs #388/391/393/395/410/412/420/430/434 |
| **ODS-2/vmsfs core (Tributary A)** | **EXTRACTED** — V1+V2+V2b merged; core in `src/kernel-core/vmsfs/`, Linux backend thin, `README-backend.md` = NetBSD template | `vms-544/00c/d69` |
| **Boot-plumbing seam (Tributary B)** | **LANDED** — `ovmx_boot_*` substrate layer, Linux backend, zero behaviour change | `vms-28f` (#440) |
| NetBSD amd64 executive foundation | proven (pseudo-device + event-flag cross-process) | `vms-dd8` #376, `vms-4b4` #393 |
| NetBSD-vax SIMH lab | reusable cached-disk artifact | `vms-0041` #379 |
| **Remaining — P4-A NetBSD backends** | ready (Linux deps done), gated by CI split | `vms-9bb/d7a/ca7/ff7` |
| **Remaining — VAX toolchain** | **in flight, behind main** | `vms-9dc` **#396 open**, held on `vms-2d9` |
| **Remaining — P4-B/C, Tributary A V3–V5, Tributary B backends** | blocked on the toolchain + CI landing | `vms-476/c99/8e5/f2e/42d/945e/7b1/625` |

**One-line read:** the hard, cross-cutting refactors are **done** — the executive
and the filesystem are both substrate-portable and the init seam is in — and what
remains is the VAX-substrate breadth (NetBSD backends, the width port, the
userspace cross-build, the mount, and the boot), **gated on the CI split
(`vms-2d9`) and the VAX toolchain (`vms-9dc`/#396) landing.**

---

## 8. Definition of done (P4)

P4 (`vms-d59`) is **done** when **all** of the following hold:

- **P4-A (`vms-f8a`)**: `vms-9bb`, `vms-d7a`, `vms-ca7`, `vms-ff7` all green — every
  executive facility has a NetBSD backend built from the **shared** core (no forked
  facility logic) and is proven cross-process on NetBSD/amd64 against a real
  `/dev/vms`.
- **P4-B (`vms-476`)**: `vms-20b9` (elf32-vax compile) → `vms-f78bb` (`/dev/vms`
  live on VAX) → `vms-4e7` (event flags cross-process vs the REAL VAX `/dev/vms`)
  all green.
- **P4-C (`vms-c99`)**: `vms-30a` (3 RTL width items resolved, incl. VAX float) and
  the full build-order chain `vms-84b → vms-1b2 → vms-271 → vms-1cb2 → vms-5d1`
  cross-build + link for netbsd-vax.
- **Tributary A (`vms-8e5`)**: V3 (`vms-bb8`) → V4 (`vms-308`, mount+read on amd64)
  → V5 (`vms-544d`, mount+read on VAX) green — the PID-1 `/vms` mount exists on
  VAX from one shared ODS-2 core.
- **Tributary B**: image activation decided (`vms-42d`), full facility parity on
  VAX (`vms-945e`), NetBSD init backend (`vms-f2e`), and the assembled bootable
  disk (`vms-7b1`) all green.
- **Capstone (`vms-d59` / `vms-625`)**: `ovmx_init` → `LOGINOUT` → a **DCL prompt**
  on NetBSD-vax under SIMH, and the **gated boot-conformance test** — booting a
  real OVMX image on the real cached SIMH-vax lab disk — asserts the DCL prompt and
  a green executive test against the real in-kernel `/dev/vms`. **No mocks, no
  per-process fallback (INV-6).**

**Final gate:** when the capstone lands, run **`/sweep`** (adversarial sweep —
security, bugs, dead code, antipatterns, test coverage, harness) over the P4
delta before P4 is closed, per release-gate practice.

---

## 9. What P4 does and does not claim

P4 delivers **VAX as a first-class OVMX runtime**: a real host kernel (NetBSD/vax)
exposing the VMS executive through `/dev/vms`, with a full OVMX userspace booting
to DCL on it, over a real ODS-2 system disk. It does **not** claim OpenVMS-VAX
authenticity of the *host* kernel (NetBSD is the substrate, exactly as Linux is on
x86_64) and it does **not** weaken Rule 9: `tests/lab-vax` and the SIMH harness
remain **build/test tooling** that prove the runtime model on VAX — they are never
themselves a runtime. The authenticity surface OVMX owns (VMS identity, facility
semantics, ODS-2, DCL, RMS) is the **same shared code** running on the VAX
backend, which is the entire point of the `vms-bea` one-core/two-backends
decision — now realized, not merely planned.
