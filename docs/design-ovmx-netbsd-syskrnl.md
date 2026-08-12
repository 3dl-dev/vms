# Design Record: An "OVMX/NetBSD" SYSKRNL — capturing VAX as a first-class runtime

> **Status:** FEASIBILITY + DESIGN scoping (rd epic `vms-8e8`). Not an
> implementation. The central deliverable was a **decision teed up for the
> operator**: a minimal generalization of the Rule 9 HARD INVARIANT (Phase 0
> below).
>
> **Phase 0 (rd `vms-fff`) — RATIFIED + LANDED.** The Rule 9 generalization to
> "one runtime *model*: the real-host-kernel path" (§5.1) was **ratified by the
> operator on 2026-08-12**. The P0 rule-text + gate edits landed in this same PR:
> `docs/runtime-target.md` reworded to the substrate-neutral form naming both
> sanctioned SYSKRNLs (OVMX/Linux = `vms.ko`, OVMX/NetBSD = the `vms`
> pseudo-device), and `tests/integration/test_runtime_target.sh` taught the new
> grepped anchor — every INV-6 / no-userspace-fallback / no-Docker sentence
> preserved verbatim, gate green. (Rule 9's operative home, `CLAUDE.md`, is not
> tracked in the public repo — it was stripped by #306, which is why the canonical
> public statement is `docs/runtime-target.md`.) P1–P4 remain design-only below.
>
> **Verdict up front: GO, STAGED.** Feasible and strategically aligned, but
> gated on operator ratification of Rule 9 (Phase 0) and staged so the OS port
> is de-risked on a *known* architecture (NetBSD/amd64) **before** the VAX arch
> port. Biggest technical risk: **no QEMU VAX target exists** — CI/boot for
> NetBSD/vax runs on SIMH, not the `qemu-system-*` machinery every existing
> executive test assumes.

---

## 1. Goal and the SYSKRNL dual-identity model

### 1.1 What is being asked

Promote **VAX** from "aspirational" (memory: PLATFORM DIRECTION, `vms-8ce`) to a
**first-class runtime** target. Linux has no VAX port and never will, so the
Linux-kernel/QEMU runtime OVMX ships today cannot reach VAX at all. The proposal
is to add a **second SYSKRNL** — **"OVMX/NetBSD"** — a sibling to today's
**"OVMX/Linux"**, because **NetBSD/vax is a current, actively-maintained port**
(verified below). NetBSD becomes the substrate that lets OVMX run *as a real
runtime* on VAX hardware/emulation — not merely observe it, the way the OpenVMS
VAX labs (lab-1/lab-2) serve as oracles under Rule 8.

This is the difference between an **oracle** (we watch real VMS on a VAX and copy
its wire/behaviour) and a **runtime** (OVMX itself executes on a VAX). Today VAX
is only ever the former. This proposal makes it the latter.

### 1.2 The SYSKRNL dual-identity model (already in the tree)

The identity split is not new; it is the GNU/Linux convention already codified in
`src/libvms/include/ovmx_identity.h` and `docs/architecture.md`:

| Layer | Macro | Today's value | Meaning |
|-------|-------|---------------|---------|
| **Product** | `OVMX_PRODUCT_NAME` | `"OpenVMX"` | What a human logs into; VMS-facing surfaces (banner, SHOW SYSTEM, MONITOR, DCL). Layer 4+. |
| **SYSKRNL** | `OVMX_SYSKRNL_NAME` | `"OVMX/Linux"` | The base layer underneath — kernel, boot, distro tooling. "an OS layered on a KERNEL, not the environment itself." Layers 0–3 + boot. |

`OVMX_SYSKRNL_BANNER` = `"OVMX/Linux -- SYSKRNL (Linux kernel)"`, printed by
`src/ovmx_init/ovmx_init.c` at early boot **before** STARTUP.EXE hands off to the
OpenVMX product identity. It is an intentionally **Linux-facing** surface — the
mirror image of INV-4's "no Linux leak on a VMS-facing surface" rule.

**"OVMX/NetBSD" is exactly this same slot with a different base kernel.** The
product ("OpenVMX", the VMS a user touches) is **unchanged**; what changes is the
SYSKRNL underneath it. Proposed additions:

```c
#define OVMX_SYSKRNL_NAME_LINUX   "OVMX/Linux"
#define OVMX_SYSKRNL_NAME_NETBSD  "OVMX/NetBSD"
/* OVMX_SYSKRNL_NAME resolves to one of the above per build target. */
#define OVMX_SYSKRNL_BANNER_NETBSD OVMX_SYSKRNL_NAME_NETBSD " -- SYSKRNL (NetBSD kernel)"
```

Note the **IRON RULE — NEVER LIE TO THE METAL** (identity.h §3) already covers
this: OVMX reports the arch it actually runs on, and where an arch has no VMS
lineage it does not fabricate one. On real VAX, VMS *did* run — so `F$GETSYI`/
`SYI$_VERSION` compat reporting on OVMX/NetBSD-vax has a genuine VAX VMS lineage
to (honestly) report, unlike OVMX-on-ARM. That is a bonus for authenticity, not a
new problem — but the exact compat token is a purity-pinned, operator-signed value
(INV-5), deferred to the arch-port phase.

---

## 2. Feasibility of a NetBSD executive

The VMS **executive** — locks, event flags, ASTs, access modes, logical names,
mailboxes, process table, device table — is today a **Linux kernel module**
(`src/kernel/vms.ko`, built from `vms_module.c` + the facility `.c` files),
exposing a `/dev/vms` character device via `miscdevice` + an ioctl interface
(`vms_ioctl.h`). Userspace reaches it exclusively through `/dev/vms`
(`src/libvmssys/vms_kif.c`). **INV-6** forbids a per-process userspace fake of any
executive facility: if `/dev/vms` is absent the correct behaviour is to fail
honestly (`SS$_NOSUCHDEV`), never to fake success.

**On NetBSD the executive must ALSO be a real in-kernel facility.** A userland
"executive" on NetBSD would be the exact LARP bug INV-6 exists to kill. So the
NetBSD executive is a **NetBSD pseudo-device driver** (a `cdevsw` character
device named `/dev/vms`) providing the same `/dev/vms` contract.

### 2.1 Loadable module vs. compiled-into-kernel — RESOLVE IN FAVOUR OF STATIC

NetBSD has a first-class kernel-module framework (`module(9)`, `modules(7)`,
`modload(8)`), and the port/feature cross-reference **reports the vax port as
module-capable** (see §7 — but treat this as *verify-in-source*, not settled: VAX
loadable-module support has historically been the thin, under-exercised path, and
a small-model page read is not proof).

**Recommendation: do not bet the port on loadable modules. Compile the executive
pseudo-device statically into a custom NetBSD/vax kernel config.** Rationale:

- OVMX already **owns and builds its kernel image** — the whole runtime is a
  purpose-built appliance kernel + initramfs, not a general-purpose OS a user
  loads modules into. Statically linking a `vms` pseudo-device into a custom
  `GENERIC`-derived config (`pseudo-device vms`) is the appliance-native path and
  sidesteps the vax-LKM uncertainty entirely.
- It matches how the Linux side already leans (PID 1 pins `/dev/vms` for the life
  of the system; the executive is "INTEGRAL", per `vms_kif.c` and Rule 9). A
  facility that must be present at PID 1 for the system to boot has no reason to be
  loadable.
- The Linux `.ko` build stays as-is; this is a NetBSD-side build choice, not a
  regression to the Linux path.

Loadable-module support on NetBSD/amd64 (§7, Phase 2) can still be used during
**development** for fast iteration, then frozen static for the shipped image.

### 2.2 Facility-by-facility map: Linux executive → NetBSD kernel

The executive is largely **self-contained data-structure logic** (hash tables,
rbtrees, per-process control blocks) with a thin dependency on kernel primitives.
That is what makes the port tractable: most of each facility's *logic* is
OS-agnostic C; only the **kernel-primitive substrate** under it changes.

| Facility (file) | Linux primitives used | NetBSD equivalent | Effort |
|---|---|---|---|
| Core / device (`vms_module.c`) | `miscdevice`, `file_operations.unlocked_ioctl`, `DEFINE_HASHTABLE`, `spinlock_t`, `kmem_cache` | `cdevsw` char device, `d_ioctl`, own hash (or `hashinit(9)`), `kmutex(9)`, `pool(9)` | **Medium** — mechanical primitive swap; the shape is identical |
| Process table (`vms_proctab.c`) | keyed on `current->tgid`; `struct pid`/`task_tgid` | `curproc->p_pid` (NetBSD process id); PCB keyed on proc | **Medium** — the tgid-vs-tid lesson (vms-9fc) re-applies: key on the **process**, not the LWP/thread |
| Locks (`vms_lock.c`) | rbtree, spinlock, wait/wake | `rb_tree(3)`/own tree, `kmutex(9)`, `cv(9)` condvars | **Medium** |
| Event flags (`vms_eflag.c`) | per-PCB bitmasks, wake | same data, `cv(9)` for waiters | **Low/Medium** |
| ASTs (`vms_ast.c`) | 4-level queue per PCB; delivered via ioctl poll (`DELIVERAST`/`-EAGAIN`) | same queue; delivery model unchanged (userspace polls) | **Low/Medium** — delivery is already poll-based, not signal-based, so no NetBSD signal-machinery dependency |
| Access modes / privs (`vms_access.c`) | `capable()`, `cred`, `uidgid` | `kauth(9)` credentials, `kauth_cred_geteuid` | **Medium** — credential model differs most here; map `CAP_SYS_ADMIN` → a `kauth` authorization |
| Logical names (`vms_lnm.c`) | hash tables in kernel memory | own hash + `pool(9)` | **Low** — pure data structure |
| Mailboxes (`vms_mbx.c`) | kernel buffers + wait queues | buffers + `cv(9)` | **Low/Medium** |
| Device table (`vms_devtab.c`) | static table built at module load | same, built at driver attach | **Low** |
| P0/P1 space (`vms_p0.c`, `vms_p1.c`) | per-process address-region bookkeeping | same bookkeeping; VM interaction minimal | **Low/Medium** |

**No facility requires a NetBSD-only kernel service that does not exist.** The
substrate primitives (mutexes, condvars, pools, char-device switch, credential
auth, hash/tree helpers) all have direct NetBSD analogues. The dominant cost is
**re-expressing the same logic against a second kernel API**, not inventing new
behaviour. A shared-core refactor (facility logic in OS-agnostic `.c`, a thin
`vms_kprim_{linux,netbsd}.c` shim for the ~dozen primitives) is worth evaluating
during Phase 2 but is **not** a precondition — a straight second port is
acceptable and lower-risk to start.

**Verdict for §2: feasible.** No facility is blocked; the executive's logic is
portable and its kernel-primitive dependencies all have NetBSD equivalents.

---

## 3. The `vms_kif` transport abstraction

`vms_kif.c` issues Linux ioctls on `/dev/vms` via `vms_sys_ioctl(fd, req, arg)`,
where `req` is an `_IOWR`-encoded number from `vms_ioctl.h`. Two portability seams
exist between Linux and NetBSD:

1. **`_IO*` request encoding differs.** `vms_ioctl.h` currently *hard-codes* the
   Linux encoding for the non-`__KERNEL__` (userspace) case:
   `_IOR/_IOW/_IOWR = (dir<<30) | (size<<16) | (type<<8) | nr`. NetBSD's
   `sys/ioccom.h` uses a **different bit layout** (`IOCPARM` mask/shift, direction
   bits `IOC_OUT`/`IOC_IN` at bits 30/31 but with a different param-length field
   width — 13 bits vs Linux's 14, and `IOC_VOID` semantics differ). If both sides
   of a NetBSD build use NetBSD's own `sys/ioccom.h`, the numbers agree with the
   NetBSD kernel driver automatically — the danger is *only* the currently
   hard-coded Linux fallback macros.

2. **`ioctl(2)` request type.** Linux: `unsigned long request`. NetBSD:
   `unsigned long` also in practice, but the freestanding `vms_sys_ioctl` syscall
   number and calling convention are NetBSD-specific (§4).

**Proposed abstraction (keeps `/dev/vms` as the contract, no wire/ABI change on
Linux):**

- Split `vms_ioctl.h` into (a) a **substrate-neutral payload header** — the
  `struct vms_*_args` layouts and the `VMS_IOC_*` *logical* command list
  (magic + ordinal + direction + payload type), and (b) a **per-substrate
  request-encoding header** that turns each logical command into the concrete
  `_IO*` number using *that platform's* `_IO*` macros. On Linux, include
  `<asm/ioctl.h>`-equivalent (or keep the existing hand-rolled Linux macros); on
  NetBSD, include `<sys/ioccom.h>`. **Delete the hard-coded Linux-only fallback**
  and always derive from the platform's own macros so kernel and userspace can
  never disagree.
- Introduce a thin `vms_kif_transport.h` seam so `vms_kif.c` calls
  `vms_kif_ioctl(fd, VMS_CMD_REGISTER, &args)` and the seam maps the logical
  command to the platform request number. `vms_kif.c`'s VMS-status logic
  (`kif_bind`, `vms_kif_kerr_to_ss`, the registration protocol) is **entirely
  substrate-agnostic already** and does not change.
- The errno→SS$ mapping (`vms_kif_kerr_to_ss`) is a *closed set both sides of
  `/dev/vms` produce*. NetBSD's errno numbers for the same conditions
  (EFAULT/ENOMEM/ENOTTY/ESRCH/EAGAIN) are the standard BSD values and map to the
  same SS$ codes; confirm the numeric values match what the NetBSD driver returns
  and keep the mapping oracle-pinned.

**Phase 1 proves this seam on Linux alone with zero behaviour change** — the
strongest possible de-risk: if the refactored Linux build is bit-identical in
behaviour and green on CI, the seam is correct before NetBSD ever enters.

---

## 4. VAX as a new arch + width class

VAX is **32-bit, little-endian** (verified §7). Every existing OVMX runtime target
is **64-bit** (x86_64, aarch64; Alpha is the 64-bit oracle). **VAX is therefore a
new *width class*, not just a new arch** — a larger blast radius than the Alpha
port, which stayed 64-bit.

The Alpha port (on the stale `vms-054-alpha-port` branch) is the **precedent and
template** for "add a new arch to freestanding libvmssys": it added
`src/libvmssys/arch/alpha/{crt0,syscall,sigreturn}.S` and normalized Alpha's
out-of-band syscall-error convention into the negative-errno contract the C layer
expects. VAX repeats that shape, plus width work.

### 4.1 Freestanding libvmssys VAX backend — **link NetBSD libc, do NOT go raw-freestanding**

The Linux/Alpha backends are **raw-freestanding**: hand-written `crt0.S`/
`syscall.S` issuing raw kernel traps, no libc. **For NetBSD/vax, recommend
linking against NetBSD libc instead of raw syscalls.** Reasons:

- NetBSD is a **complete OS that provides libc and a stable syscall ABI via
  libc** — unlike the Linux-appliance path where OVMX deliberately avoids glibc.
  NetBSD's syscall trap ABI is *not* a stable public contract the way Linux's is;
  NetBSD explicitly expects programs to go through libc. Hand-rolling raw VAX
  syscall stubs fights the platform.
- VAX has **no tcc / no OVMX-native toolchain backend** (§4.3), so the freestanding
  purity that justifies raw syscalls on the self-hosting path does not apply here
  anyway — this is a GCC-cross build regardless.
- `libvmssys`'s value on NetBSD/vax is the **VMS API surface and the `/dev/vms`
  kif transport**, not glibc-avoidance. Let NetBSD libc provide crt0/TLS/syscall
  plumbing and keep `vms_kif.c` + the VMS RTL on top.

Concretely: a `netbsd-vax` build variant of libvmssys where `vms_runtime_init.c`'s
hand-rolled auxv/TLS setup (currently `#if defined(__x86_64__)/__aarch64__`) is
**not compiled** — NetBSD's csu/libc does that — and `vms_syscall.h` wrappers
resolve to NetBSD libc calls. This is a meaningful structural fork of the
libvmssys build model and must be scoped as such.

> **Open decision for the port phase (tee up, don't pre-decide):** raw-freestanding
> VAX (max purity, fights NetBSD, large asm effort incl. VAX calling standard) vs.
> link-libc (recommended, pragmatic, smaller). Recommendation = link libc; flagged
> for confirmation when the phase opens.

### 4.2 32-bit / width assumptions — audit blast radius

`vms_runtime_init.c` hard-codes `struct elf64_phdr` and 64-bit TLS variants. RMS,
descriptors, and kernel structs must be audited for 64-bit assumptions:

- **Descriptors** (`descrip.h/.c`): VMS descriptors are natively 32-bit-friendly
  (VAX is their origin), but any OVMX code assuming 64-bit pointers inside a
  descriptor payload breaks.
- **RMS** (FAB/RAB/NAM/XAB): audit for `uint64_t` where VMS uses longword.
- **Kernel `struct vms_*_args`** in `vms_ioctl.h`: any pointer-width or `long`
  field changes size on ILP32 — the ioctl payload structs must be **fixed-width
  (`uint32_t`/`uint64_t`), never `long`/pointer**, or the 32-bit userspace and the
  (32-bit) VAX kernel agree but a cross-checked 64-bit build diverges. Audit now.
- **ELF class:** VAX ELF is `ELFCLASS32`; the IMGACT / image-activation backend and
  `elf64_phdr` parsing need an `elf32` path. (On the link-libc path, NetBSD's
  activator/ld.elf_so handles image loading, shrinking this — but OVMX's own
  IMGACT/symbol-vector activation model, if used on VAX, needs the 32-bit ELF
  variant.)
- **VAX floating point is NOT IEEE-754** (F/D/G/H formats). GCC/vax defaults to VAX
  float. This affects `MTH$`/`OTS$` float RTL, not the executive or kif — but flag
  it: any float constant round-trip or IEEE assumption in the RTL is a latent bug
  on VAX. Out of scope for the boot/executive milestone; note for the RTL phase.

### 4.3 Toolchain — GCC cross is the only realistic path

- **tcc / the OVMX-native toolchain does NOT target VAX** (self-hosting north star
  is x86_64/Alpha-class; VAX codegen is not on the roadmap). The self-hosting
  story does not extend to VAX and should not block this.
- **GCC still has a maintained VAX backend** (verified §7) and the
  `compiler-explorer/gcc-cross-builder` project ships a **`build-vax-netbsd.sh`**
  recipe — a `vax--netbsd` GCC cross toolchain is a known, buildable artifact.
  **This is the realistic and recommended path.** Containerize it (Rule: all deps
  containerized) as a `Dockerfile`-built cross toolchain, exactly like the existing
  build tooling.

### 4.4 CI matrix

Add a `netbsd-vax` (and, per staging, a `netbsd-amd64`) column. **The hard part is
not compilation — it is execution** (§6): there is no `qemu-system-vax`.

---

## 5. Rule 9 reconciliation — THE decision gate (operator-reserved)

Rule 9 and `docs/runtime-target.md` are written **Linux-specifically**: *"OVMX has
exactly one runtime: the real-kernel / QEMU path, where `vms.ko` provides the VMS
executive and userspace reaches it through `/dev/vms`."* A second SYSKRNL appears
to violate "exactly one runtime" — **but it does not violate the invariant's
intent.** The intent is not "Linux specifically"; it is:

> **The executive is ALWAYS a real host-kernel facility, reached through
> `/dev/vms`. Userspace never fakes it. Docker is never a runtime.**

NetBSD-with-a-real-`vms` pseudo-device satisfies every word of that intent. What
Rule 9 actually forbids — a userland fake (INV-6), and Docker-as-runtime — remains
forbidden, unchanged, on **both** substrates.

### 5.1 Proposed minimal generalization (diff-in-spirit)

**`docs/runtime-target.md` — "The rule":**

> ~~One runtime target: the kernel/QEMU path.~~ → **One runtime *model*: the
> real-host-kernel path.** OVMX runs on a real OS kernel that provides the VMS
> **executive** as an in-kernel facility reached through `/dev/vms`. There are two
> sanctioned SYSKRNLs: **OVMX/Linux** (executive = `vms.ko`, x86_64/aarch64) and
> **OVMX/NetBSD** (executive = the `vms` pseudo-device, initially VAX). Both
> expose the identical `/dev/vms` contract. **Docker is never a runtime on either.
> A userspace fake of any executive facility (INV-6) is forbidden on either.**

**`CLAUDE.md` Project-Specific Rule 9:** same generalization — "one runtime = the
kernel/QEMU path with `vms.ko`" becomes "one runtime **model** = a real host kernel
providing the executive via `/dev/vms`, realized as `vms.ko` on Linux or the `vms`
pseudo-device on NetBSD." Keep every INV-6 / no-userspace-fallback / no-Docker
sentence verbatim.

**The standing gate `tests/integration/test_runtime_target.sh`:** it greps
`docs/runtime-target.md` for the rule text and enforces the mechanical parts (no
recreated root `Dockerfile`/`docker-compose.yml`, no allowlist growth). It **must
learn the generalization**: (a) update the grepped rule-text anchors so the gate
does not fail on the reworded doc, and (b) it must **not** start treating the
NetBSD build files as a forbidden runtime Dockerfile. It should continue to forbid
a *glibc product container* runtime while permitting the NetBSD SYSKRNL build. This
is a small, surgical gate edit — scope it inside Phase 0.

### 5.2 This ratification is operator-reserved

Per `~/.claude/CLAUDE.md` Authority and CLAUDE.md Rule (weakening/altering a stated
principle is operator-reserved), **the Rule 9 generalization is a call only the
operator makes, and it must land before any NetBSD implementation.** Phase 0 exists
to obtain it. Everything downstream is blocked on it.

---

## 6. Test / CI / oracle strategy

### 6.1 The execution problem: no `qemu-system-vax`

**Verified (§7): QEMU has no VAX system target and never has.** Every existing
executive test (`tests/qemu/`) boots a Linux kernel under `qemu-system-x86_64/-aarch64`
and exercises `/dev/vms`. That machinery **does not extend to VAX.** The VAX
emulator of record is **SIMH** (`simh` VAX / MicroVAX 3900), the same emulator the
OpenVMS VAX labs use; NetBSD/vax is routinely installed and booted under SIMH.

Consequences:
- **NetBSD/vax boot + executive tests run under SIMH, not QEMU.** A new
  `tests/simh/` (or `tests/netbsd-vax/`) harness boots a NetBSD/vax SIMH image with
  the executive compiled in, then runs the QEMU-test analogue against the real
  `/dev/vms` inside it. SIMH is scriptable (console over telnet/pty), containerizable,
  and already understood by the lab tooling — but it is **slower** than QEMU and a
  new harness surface.
- **De-risk with NetBSD/amd64 first (Phase 2):** NetBSD/amd64 **does** run under
  `qemu-system-x86_64`. Proving the NetBSD executive driver + `/dev/vms` + the kif
  transport on NetBSD/amd64 under QEMU exercises the entire **OS port** on fast,
  familiar infrastructure, leaving **only the arch/width delta** for the SIMH/VAX
  phase. This is the single most valuable sequencing decision in the plan (see §7
  verdict).

### 6.2 Downstream payoff (NOT scope)

A real VAX-architecture OVMX node could eventually speak the **actual VAXcluster
wire** as a genuine 32-bit VAX participant — directly serving the cluster-interop
objective (`vms-ci`), which today depends on VAX *oracles* rather than a VAX
*peer*. **This is downstream payoff, explicitly out of scope here.** Do not let it
expand the milestone; the milestone ends at "OVMX/NetBSD-vax boots and its
executive test is green."

---

## 7. Verified facts vs. assumptions

**Verified via web search (Aug 2026):**
- **NetBSD/vax is a current, actively-maintained port** — NetBSD 10 shipped vax
  updates (rasops smg(4), gpx(4) framebuffer drivers); development continues.
  [[netbsd-changes-10]] [[netbsd-vax]]
- **VAX is 32-bit, little-endian.** [[vax-wikipedia]]
- **QEMU has no VAX system target** — QEMU's system targets are alpha/arm/hppa/
  i386/m68k/mips/ppc/riscv/s390x/sh4/sparc/xtensa/… ; **VAX is absent.** SIMH is the
  VAX emulator of record. [[qemu-targets]]
- **GCC still ships a maintained VAX backend**, and a `vax--netbsd` cross toolchain
  is a known buildable artifact (`compiler-explorer/gcc-cross-builder`
  `build-vax-netbsd.sh`; GCC VAX-Options doc live). [[gcc-vax]] [[gcc-cross-vax]]

**Reported but treat as verify-in-source (do NOT bet the port on it):**
- NetBSD's port/feature cross-reference **reports vax as module-capable ("Y")**.
  Given VAX LKM support has historically been thin and this was a single
  small-model page read, **§2.1 recommends compiling the executive statically into
  a custom kernel regardless**, which moots the question.

**Assumed (standard platform knowledge, not separately verified):**
- NetBSD kernel primitives (`cdevsw`, `kmutex(9)`, `cv(9)`, `pool(9)`, `kauth(9)`,
  `hashinit(9)`/`rb_tree(3)`) exist and map to the Linux primitives in §2.2.
- NetBSD's `sys/ioccom.h` `_IO*` bit layout differs from Linux's (13- vs 14-bit
  param length field) — the §3 seam must derive from each platform's own macros.
- VAX floating point is non-IEEE (F/D/G/H) — well-known; affects RTL only.

Sources:
- [NetBSD changes 9→10](https://www.netbsd.org/changes/changes-10.0.html)
- [NetBSD/vax port page](https://www.netbsd.org/ports/vax/index.html)
- [VAX (Wikipedia)](https://en.wikipedia.org/wiki/VAX)
- [QEMU System Emulator Targets](https://qemu-project.gitlab.io/qemu/system/targets.html)
- [GCC VAX Options](https://gcc.gnu.org/onlinedocs/gcc/VAX-Options.html)
- [gcc-cross-builder build-vax-netbsd.sh](https://github.com/compiler-explorer/gcc-cross-builder/blob/main/build/build-vax-netbsd.sh)

---

## 8. Verdict and phased plan

### Verdict: **GO, STAGED.**

Feasible on every axis examined: the executive's logic ports (§2), the kif
transport abstracts cleanly and can be proven on Linux first (§3), the arch/width
work has a precedent (Alpha) and a real toolchain (GCC cross, §4), and Rule 9
generalizes without weakening its intent (§5). It directly advances the standing
objective (VAX commoditization; downstream, VAX cluster-interop). It is **staged**
because two risks demand de-risking *before* the expensive VAX arch work:

1. **The Rule 9 call is operator-reserved and must land first** (Phase 0).
2. **The OS port and the arch port are independent risks; separate them.** Prove
   the NetBSD executive on NetBSD/amd64 under QEMU (Phase 2) before touching VAX,
   so a Phase-3/4 failure is unambiguously an *arch/width* problem, not an
   OS-port problem.

**Single biggest technical risk: no `qemu-system-vax`.** All existing executive-test
infrastructure assumes QEMU; NetBSD/vax runs on SIMH. The Phase-2-before-VAX staging
is precisely the mitigation — it moves all OS-port risk onto QEMU and leaves SIMH to
carry only the (already-de-risked) arch delta.

### Proposed rd decomposition (epic `vms-8e8`) — conductor files these

Each is outcome-scoped. Wire `--parent-id vms-8e8` and the dependency chain
Phase 0 → 1 → 2 → 3 → 4 (Phase 1 may run parallel to Phase 0's operator wait).

| # | Proposed title | One-line outcome | Domain |
|---|---|---|---|
| **P0** | Rule 9 generalized to "one runtime *model*: real host kernel via /dev/vms" — operator-ratified | `docs/runtime-target.md` + CLAUDE.md Rule 9 + `test_runtime_target.sh` state the substrate-neutral rule (Linux `vms.ko` OR NetBSD `vms` pseudo-device); INV-6 + no-Docker preserved; gate green; **operator sign-off recorded**. | TechWriter (drafts) → **operator gate** |
| **P1** | `vms_kif` transport seam extracted on Linux with zero behaviour change | `vms_ioctl.h` split into substrate-neutral payloads + per-platform `_IO*` encoding; `vms_kif.c` calls a logical-command seam; Linux build bit-behaviour-identical and full CI green. Proves the seam before NetBSD exists. | Systems |
| **P2** | NetBSD/amd64 SYSKRNL proof — real `vms` pseudo-device, `/dev/vms`, executive test green under QEMU | NetBSD/amd64 boots under `qemu-system-x86_64` with the executive compiled into a custom kernel; a `tests/qemu`-analogue exercises ≥1 facility (locks) against the real `/dev/vms`; INV-6 honest-fail verified when absent. De-risks the OS port on a known arch. | Systems + QA |
| **P3** | libvmssys VAX (netbsd-vax) backend builds under a containerized GCC cross toolchain | `vax--netbsd` GCC cross image builds libvmssys for VAX (link-libc model); 32-bit/ILP32 width audit of `vms_ioctl.h` structs, descriptors, RMS, ELF32 activation complete and fixed-width-clean; unit build green in CI. | Systems |
| **P4** | OVMX/NetBSD-vax boots under SIMH and its executive test is green | A SIMH NetBSD/vax image with the executive compiled in boots to the OVMX/NetBSD SYSKRNL banner + product handoff; `tests/simh` harness runs the executive facility test against the real in-kernel `/dev/vms` on VAX; green in CI. Milestone: VAX is a first-class runtime. | Systems + QA |

> Optional P2.5 (evaluate during P2, not pre-committed): refactor facility logic
> into OS-agnostic core + `vms_kprim_{linux,netbsd}` shim, if the straight second
> port shows enough duplication to justify it. Not a precondition for P2.

**Downstream (separate epic, NOT in vms-8e8):** VAX RTL float (F/D/G/H) correctness;
OVMX/NetBSD-vax as a real VAXcluster wire participant (`vms-ci`).
