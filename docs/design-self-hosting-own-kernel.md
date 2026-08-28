# Design Record: The Ouroboros — OVMX builds the kernel it ships, from inside OVMX

> **Status:** MAP / DELIVERY-PLAN for rd `vms-df7` ("OVMX self-hosts its OWN
> kernel — close the distribution loop"). **Post-1.0.** This is a road-map, not
> an implementation: it stages the endgame so the 1.0 self-hosting spine
> sequences *toward* it and nothing is lost. **Doc only — no code changed here.**
>
> **Grounds on** `docs/design-self-host-mmk-spine.md` (the RTL-gap verdict),
> `docs/design-self-host-spine5-mmk-component.md` (MMK drives compile→archive→
> LINK→activate in-guest), the self-hosting ladder (memory [[self-hosting-northstar]],
> rd `vms-116`/`vms-678`), `distro/Dockerfile.bootable` (the current kernel
> cross-build), `docs/design-ovmx-netbsd-syskrnl.md` + `docs/design-p4-netbsd-vax-boot.md`
> (the second substrate), and `docs/runtime-target.md` (Rule 9, substrate-neutral).
> It does not restate them.
>
> **Clean-room (CLAUDE.md Rule 8):** GCC, GNU binutils, the Linux kernel, and
> NetBSD are all free software OVMX has the right to build and run. Nothing below
> reads, disassembles, or copies VSI/HPE source or binaries. Object/image formats
> that go through OVMX's own LINK.EXE/symbol-vector path remain OVMX-labeled
> design choices (Rule 8); ELF and a.out for the kernel targets are the public,
> documented native formats of the substrate toolchains, produced by GNU
> binutils, **not** by LINK.EXE.

---

## 1. The framing: OVMX is a distribution, and a mature distribution builds its own kernel

OVMX is not "an OS" in the monolithic sense; it is a **distribution** — a VMS
userland + personality (system services, RTL, DCL, RMS, the toolchain, the boot
orchestration) layered over a **swappable kernel substrate** (a "SYSKRNL", per
`src/libvms/include/ovmx_identity.h` and `docs/design-ovmx-netbsd-syskrnl.md`).
This is the GNU/Linux convention made explicit: **OVMX/Linux** and **OVMX/NetBSD**
are two distributions of the same product over two kernels, exactly as
Debian/Linux and Debian/kFreeBSD were two ports of one Debian.

A distribution matures in two distinct senses that this document is careful to
keep apart:

- **Vendor-independence — ALREADY MET.** Every ingredient of the kernel today is
  free software the project may build and ship without asking anyone: GCC,
  binutils, the Linux kernel (pinned from `cdn.kernel.org` source, SHA-256
  verified — `distro/Dockerfile.bootable` `kernel-build` stage, rd `vms-448`),
  NetBSD/vax. There is no VSI/HPE dependency in the kernel path. **This is done.**

- **Closed-loop self-reproduction — THIS DOCUMENT.** A mature distribution builds
  its own kernel *package* **from inside itself**: Debian builds `linux-image` on
  Debian, using Debian's own gcc/make/dpkg. OVMX does not yet do this. Today OVMX
  self-hosts its **userland** (§2) but **cross-builds its kernel in an outside
  container** — `distro/Dockerfile.bootable` runs Ubuntu's gcc + Linux kbuild.
  That is the "**Linux-on-Minix**" stage: the system can reproduce everything
  above the kernel line, but the kernel itself is still made by a foreign
  toolchain. **Closing that loop — the ouroboros — is `vms-df7`.**

The distinction matters because it bounds the claim. Closing the loop buys **no
new vendor-independence** (we already depend on no one). What it buys is
**architectural completeness and honesty**: the distribution can regenerate *all*
of itself, including its own substrate, with no non-OVMX toolchain anywhere in the
path. It is the terminal milestone of the self-hosting program
([[self-hosting-northstar]]), not a separate track.

**Scope note (do not re-freeze):** 1.0 ships an externally-built kernel and that
is **fine** — the memory record is explicit that "kernel is cross-built for 1.0"
is the *bootstrap crutch*, not a permanent boundary. This map exists so that the
crutch is removed deliberately, post-1.0, rather than forgotten.

---

## 2. Current state — what self-hosts today, grounded

> **LADDER RECONCILE (operator 2026-08-22, Rule 1 / `vms-ports-build-ladder`):**
> this doc's framing already holds — **tcc is an explicitly-labeled temporary
> bootstrap** (the S2 beachhead), **not the faithful endgame.** The faithful
> self-host destination is the *existing VMS toolchain ports* building OVMX on
> OVMX over a genuine VMS-compat surface: the `alpha-dec-vms` OpenVMS **GCC** port
> on OVMX (`vms-da0` → ladder target `vms-fd1`) and **MMK/LIBRARIAN** (`vms-59a`),
> not a bespoke OVMX slop surface only our forks target. tcc retires once the VMS
> GCC port builds on OVMX. "tcc self-hosts OVMX; GCC lets OVMX self-host its
> substrate" (§ below) is the same statement.

### 2.1 The userland self-host ladder (S2→S4, ACHIEVED)

The toolchain already reproduces itself inside OVMX, driven by DCL against a real
`/dev/vms`, zero bash in the build path:

| Rung | What runs inside OVMX | Evidence (origin/main) |
|---|---|---|
| **S2** | **TCC.EXE self-hosts** — tinycc, IMGACT-activated, compiles itself gen2==gen3 byte-identical | rd `vms-4ba`; `src/imgact/test/run_tcc_selfhost.sh` |
| **S3.1** | **LINK.EXE is an OVMX-native image** — reads `.o` via RMS, writes a runnable `.EXE`, resolves `.vms$sv` symbol vectors (no `ld`, no `ld.so`, no DT_NEEDED/DT_HASH) | rd `vms-247`; `src/vmslink/link.c`; the `link-native` stage gate in `distro/Dockerfile.bootable` |
| **S3.2** | **BUILD.COM** — `@SYS$SYSTEM:BUILD` compiles→links→runs from a DCL session, zero bash | rd `vms-251` |
| **S4** | **Fixpoint** — OVMX builds LINK.EXE from within via multi-TU BUILD.COM, gen2==gen3 byte-identical, CI job "LINK.EXE self-host fixpoint (S4)" | rd `vms-255` (`156065c`) |
| **MMK** | **MMK.EXE drives the whole chain in-guest** — compile (TCC.EXE) → archive (LIBRARIAN.EXE, `.OLB`=`ar` container) → LINK.EXE → **IMGACT activation** of a real OVMX component, over MMK's mailbox-DCL, byte-identical | rd `vms-6be`/`vms-725`; `tests/qemu/test_syssvc_mmk_build.c` (in the `kernel-executive` CI barrier) |

**Images that run inside OVMX today** (IMGACT-activated, PT_INTERP, against
`/dev/vms`): `TCC.EXE`, `LINK.EXE`, `LIBRARIAN.EXE`, `MMK.EXE`, `DCL.EXE`,
`LOGINOUT.EXE`, `IMGACT.EXE` itself (freestanding, no PT_INTERP), the SYSLIB
shareables (`DECC$SHR.EXE` + the six `LIBVMS*$SHR.EXE`), plus demo `PARTS.EXE`.
The C library the built images bind is `DECC$SHR.EXE` = whole-archived musl
`libc.a` + `libgcc.a` (`src/vmslink/mk_decc_shr.sh`).

**1.0 self-host gate** (rd `vms-678`, "Build-native" bar): no bash/host tool in
the 1.0 *build path* for everything **above the kernel line**. `BUILD.COM` was
retired in favour of the MMK descrip.mms driver for the full tree (rd `vms-89d`,
`vms-e49`); S5
(agent-in-OVMX, network-native, runtime-native) is explicitly **post-1.0**.

### 2.2 The kernel is cross-built OUTSIDE OVMX (the open loop)

`distro/Dockerfile.bootable` builds the shipped kernel in a **foreign** toolchain
— none of it runs inside OVMX:

- **`kernel-build` stage** (`FROM ubuntu:24.04`): `apt install gcc make bc bison
  flex libssl-dev libelf-dev xz-utils cpio kmod`; fetch pinned `linux-6.12.103`
  source (SHA-256 verified, rd `vms-448` "owns-kernel" `vms-19e`); overlay the
  OVMX modules in-tree at `drivers/ovmx/{vms,vmsfs}/` (rd `vms-934`); `make
  ARCH=x86_64 defconfig` + `merge_config.sh` OVMX fragment + `olddefconfig`; `make
  bzImage modules`; sign modules with the OVMX signing key (build secret, not committed; rd `vms-ff5`, `vms-secret-signing-key`).
- The kernel is pinned at **6.12 LTS** because `vmsfs.ko` targets the pre-6.14 VFS
  mount surface (`vms.ko` builds on 6.18; `vmsfs.ko` does not) — a real porting
  constraint that also lives on the outside toolchain today.

So the line is exactly: **everything that becomes a `.EXE`/`.OLB`/shareable
self-hosts; the `vmlinuz` + `vms.ko`/`vmsfs.ko` do not.** The ouroboros closes
when that second clause is built inside OVMX too.

---

## 3. The ouroboros compiler: a multi-target GCC VMS port, as OVMX-native images

**This is the crux, and it is MANDATORY — not an optional accelerator.** The
reasons are concrete, not aesthetic:

1. **Only GCC spans the whole OVMX target matrix.** OVMX targets x86_64-linux +
   aarch64-linux (OVMX/Linux) **and** vax-netbsd + alpha-* (OVMX/NetBSD +
   Alpha-first-class, memory PLATFORM DIRECTION `vms-8ce`). **LLVM/Clang has no
   VAX back end and dropped its Alpha back end**; it can co-star on modern
   targets but can **never** close the two legacy ones. rustc cannot build the
   Linux kernel at all. **GCC is the only compiler that reaches every OVMX
   substrate *and* is the compiler the Linux and NetBSD kernels are written for.**

2. **tcc cannot build a kernel.** tinycc is the self-host **beachhead** (S2) —
   deliberately minimal, GAS-only integrated assembler, no optimizer, no VAX/Alpha
   back ends, does not implement the GNU C dialect the kernel relies on
   (`-fno-strict-aliasing` semantics, named-address-space, deep inline-asm,
   `asm goto`, section attributes, `__builtin_*` breadth). It compiles OVMX's own
   freestanding TUs; it will never compile `linux-6.12` or a NetBSD kernel. The
   compat register already records this: tcc is "NOT a perf compiler … rustc/Rust
   later" (`docs/compat/facilities/compilers.yaml`).

3. **GCC's VMS lineage is proven-to-run-on-VMS.** GCC has a documented history of
   running natively on VAX-VMS and Alpha-VMS and under GNV; it is the compiler
   that has, historically, actually executed on an OpenVMS host. That lineage is
   evidence the port is *possible as an OVMX-native image*, not a leap.

**Therefore the ouroboros compiler = GCC (+ GNU binutils: `as`, `ld`, `ar`, the
BFD object machinery) built and run as OVMX-native images**, host-personality =
VMS/OVMX, configured as a **cross/multi-target** compiler whose *targets* are the
substrate triples (`x86_64-linux`, `aarch64-linux`, `vax--netbsdelf`,
`alpha-*`). It emits **native ELF/a.out via binutils**, entirely bypassing OVMX's
LINK.EXE/symbol-vector path (which is for OVMX's own `.EXE` images, not for kernel
objects — see §4.3).

This does **not** retire tcc. tcc/LINK.EXE remain the beachhead that bootstraps
the environment and builds OVMX's *own* userland images; GCC is added **above**
them as the production compiler that can build the kernel (and, downstream, the
Fortran/COBOL/etc. corpus — a large independent payoff, §7). The relationship is:
**tcc self-hosts OVMX; GCC lets OVMX self-host its substrate.**

---

## 4. The gap: what a kernel build needs that userland self-hosting does not provide

Userland self-hosting proves OVMX can compile→archive→link→activate **its own
image format**. A kernel build needs four things that path does not supply.

### 4.1 A production, multi-target C compiler + assembler (GCC + GNU `as`)

Covered in §3. Concretely, as OVMX-native images: `GCC.EXE` (the driver `gcc`),
`CC1.EXE` (the C compiler proper), `AS.EXE` / `GAS` (GNU assembler — the kernel is
full of `.S` files and inline asm tcc's assembler cannot handle), and `CPP`
(preprocessor). Each must run IMGACT-activated against `/dev/vms`, reading source
via RMS and writing objects to the SYSDISK, exactly as TCC.EXE does today — but
GCC is ~100× the surface. This is the **single largest lift in the program**
(§6.1).

### 4.2 GNU binutils as OVMX images (`as`, `ld`, `ar`, `objcopy`, `nm`, BFD)

The kernel is linked by **`ld`**, not OVMX's LINK.EXE, and its build invokes
`ar`/`objcopy`/`nm`/`objtool`. OVMX's LINK.EXE consumes `ar` archives already
(`link.c` reads `!<arch>` whole-archive, rd `vms-004`) but **emits OVMX `.EXE`
images with `.vms$sv` symbol vectors** — a kernel needs a genuine ELF `vmlinux`
with a real `.dynamic`/relocation structure that only `ld` + BFD produce. So
**binutils must be ported as its own OVMX-native images**, not faked through
LINK.EXE. binutils is a much smaller, more portable lift than GCC (it is
plain C, autotools, well-travelled to exotic hosts), and it is a **prerequisite of
GCC** (GCC's build wants a working `as`/`ld` for the target).

### 4.3 A POSIX build-substrate sufficient for the kernel build system

This is the second crux (§6.2), and the two substrates diverge sharply here:

**Linux kbuild** is a `make` + shell + host-tool machine. Building `linux-6.12`
requires, *on the build host*:
- **GNU make** (kbuild is deeply GNU-make-specific: `$(eval)`, `$(call)`,
  order-only prereqs, `.SECONDEXPANSION`, recursive submake).
- **A POSIX shell + coreutils** — thousands of `$(shell …)` escapes, `scripts/*.sh`,
  `sed`/`awk`/`grep`/`cat`/`printf`/`cmp`/`ln`/`mkdir`.
- **Kbuild's own host programs, compiled during the build**: `fixdep`, `modpost`,
  `genksyms`, `kallsyms`, the `kconfig/conf` family, `sorttable`, `objtool`,
  plus **`bc`, `flex`, `bison`, `perl`** (bounds/timeconst, kconfig lexer/parser,
  some generators), **`openssl`** (module signing / cert), **`libelf`**
  (objtool/modpost). These are compiled **by the host compiler for the host** and
  then *run* during the kernel build — so they must run **as OVMX images too**.

That inventory is a **GNV-scale POSIX layer** ("GNU's Not VMS" — the historical
POSIX-on-OpenVMS environment). The open question (§6.2) is whether OVMX must port
GNV wholesale, or whether a **leaner POSIX shim** suffices: OVMX already has a
DCL, RMS file I/O, and a musl `libc.a` inside `DECC$SHR.EXE`. The minimal set is
"GNU make + a POSIX `sh` + the coreutils kbuild actually shells out to + the
host-tool binaries built from the kernel's own `scripts/` + bc/flex/bison/perl/
openssl/libelf as OVMX images." **This is plausibly smaller than full GNV but is
still the second-biggest item in the program, and it is where the estimate is
softest.**

**NetBSD `build.sh` is dramatically cleaner** and is why the NetBSD loop is the
**easier substrate to close** (§5.2). `build.sh` is explicitly designed for a
**host-agnostic cross-build**: it first bootstraps its *own* toolchain (a known
GCC + binutils + the `nbmake`/host tools it needs) into a `tools/` directory from
in-tree source, then cross-builds the entire system with that. It does **not**
assume a rich ambient GNU environment the way kbuild does — it brings its own.
The POSIX surface it needs from the host is far smaller (a C compiler to bootstrap
`nbmake` + host tools, a `sh`, basic utilities). Closing the NetBSD loop is
therefore mostly "get GCC+binutils running as OVMX images and feed `build.sh`,"
whereas the Linux loop additionally demands the GNV-scale POSIX layer.

### 4.4 Target object/image formats that bypass OVMX LINK.EXE

The kernel targets are **ELF** (x86_64/aarch64 Linux `vmlinuz`, elf32-vax /
NetBSD) and, on some NetBSD arches, **a.out** — produced by **GNU `ld`/BFD**, not
LINK.EXE. This is a clean separation, not a conflict: OVMX's `.vms$sv` symbol
vector is OVMX's *own* image format (Rule 8, OVMX-labeled); the kernel gets the
substrate's *authentic* native format from binutils. Nothing about the ouroboros
asks LINK.EXE to emit a `vmlinux`. The in-tree module overlay (`drivers/ovmx/`,
rd `vms-934`) already builds `vms.ko`/`vmsfs.ko` as ordinary in-tree ELF modules —
that mechanism is unchanged; only the *toolchain running it* moves inside OVMX.

---

## 5. The two ouroboros loops (staged outcomes + DAG)

Each stage is a **verifiable end-state**, not a layer. Dependencies are drawn as a
DAG. "Could-start-now" = independently useful yeoman's work that does not wait for
1.0. "Post-1.0" = should not compete with the 1.0 spine.

### 5.1 Shared foundation (feeds BOTH loops)

```
[F1] binutils as OVMX images ────────────┐
     as/ld/ar/objcopy/nm/BFD, IMGACT-     │
     activated, emit native ELF/a.out     │
     done: `as`+`ld` build a trivial ELF  │
     exe inside OVMX, runs on the target  │
                                          v
[F2] GCC multi-target as OVMX images ─────┐   (depends: F1)
     gcc/cc1/cpp, host=OVMX, TARGET set   │
     {x86_64-linux, aarch64-linux,        │
      vax-netbsdelf, alpha-*}             │
     done: GCC.EXE compiles a multi-TU    │
     C program to a target ELF .o inside  │
     OVMX, byte-reproducible, and the     │
     object links+runs on the target      │
                                          │
[F3] GCC self-host fixpoint (optional,    │   (depends: F2)
     post-1.0 nicety): GCC-in-OVMX builds │
     GCC-in-OVMX, gen2==gen3              │
                                          │
                        ┌─────────────────┴─────────────────┐
                        v                                    v
                 LINUX LOOP (§5.1a)                   NETBSD LOOP (§5.2)
```

**Foundation is the common trunk.** F1→F2 is the compiler ouroboros itself and
is shared. F3 (GCC building GCC inside OVMX) is a *purity nicety*, not required to
build a kernel — flag it optional/post-everything.

#### 5.1a Linux loop

```
[F2] GCC+binutils images
        │
        v
[L1] POSIX/kbuild host layer as OVMX images ──────────┐  (depends: F2)
     GNU make + POSIX sh + the coreutils kbuild        │
     shells out to + kernel host-tools (fixdep/        │
     modpost/genksyms/kallsyms/kconfig) + bc/flex/     │
     bison/perl/openssl/libelf as OVMX images          │
     done: `make defconfig` + `make scripts` completes │
     inside OVMX against a real linux-6.12 tree         │
        │                                               │
        v                                               │
[L2] Build the Linux kernel INSIDE OVMX ───────────────┤  (depends: L1, F2)
     `make ARCH=<a> bzImage modules` runs entirely      │
     on OVMX images; overlay vms.ko/vmsfs.ko in-tree    │
     (reuse drivers/ovmx, vms-934) — no foreign         │
     container                                          │
     done: a bzImage + signed in-tree vms.ko/vmsfs.ko    │
     produced with ZERO non-OVMX toolchain in the path   │
        │                                               │
        v                                               │
[L3] Boot OVMX on the OVMX-built kernel ────────────────┘
     done: the vmlinuz built by L2 boots OVMX to a DCL
     prompt in QEMU; executive test green on /dev/vms.
     LOOP CLOSED (Linux).
        │
        v
[L4] Reproducibility + gate parity (post): the OVMX-built
     kernel is byte-reproducible and swappable for the
     Dockerfile.bootable output; cut-release can use
     either. done: cmp-clean vs a foreign-built kernel
     of the same commit (or documented, understood diff).
```

#### 5.2 NetBSD/VAX loop (the EASIER substrate to close)

```
[F2] GCC+binutils images  (TARGET incl. vax-netbsdelf, alpha-*)
        │
        v
[N1] NetBSD build.sh host layer as OVMX images ────────┐  (depends: F2)
     far leaner than L1: build.sh bootstraps its own    │
     tools/; OVMX supplies sh + a C compiler + basic    │
     utils. done: `build.sh tools` completes inside      │
     OVMX                                                │
        │                                               │
        v                                               │
[N2] Build the NetBSD/VAX kernel INSIDE OVMX ───────────┤  (depends: N1, F2)
     `build.sh kernel=GENERIC` (the MODULAR variant P4   │
     already needs, per design-p4) cross-built on OVMX   │
     images. done: a /netbsd for vax produced with ZERO  │
     non-OVMX toolchain                                  │
        │                                               │
        v                                               │
[N3] Boot OVMX/NetBSD on the OVMX-built kernel ─────────┘
     done: the /netbsd built by N2 boots OVMX/NetBSD to a
     DCL prompt on SIMH-vax; /dev/vms executive test green.
     LOOP CLOSED (NetBSD/VAX).
```

**Why NetBSD is easier to close but harder to reach:** the *loop mechanics* are
simpler (build.sh vs kbuild+GNV), so **N1 is much smaller than L1**. But the
**target** is exotic — elf32-vax, ILP32, non-IEEE VAX float, SIMH-only (no
qemu-system-vax), and the P4 arc (rd `vms-8e8`) must first make OVMX *boot* on
NetBSD/vax at all (that is 1.0-adjacent work already in flight, `docs/design-p4-netbsd-vax-boot.md`).
So sequence-wise: F1/F2 are shared; then **do the NetBSD loop's N1 as the
proof-of-concept** (cleanest host layer) while the Linux loop's L1 (the GNV lift)
runs as the larger parallel effort.

---

## 6. Hard parts, unknowns, and risks — stated honestly

### 6.1 The GCC-VMS-port lift (LONG POLE #1)

- **Magnitude.** GCC is ~an order of magnitude more code and build complexity
  than tcc. Its build is a **three-stage bootstrap** (stage1 by host cc → stage2
  by stage1 → stage3 by stage2, compare stage2==stage3) that itself wants a
  working `make`, `sh`, and target `as`/`ld` — so GCC's *own* build already needs
  a chunk of the POSIX layer (§6.2). Porting GCC to *run as an OVMX image* means:
  its configure/build must complete against OVMX's musl `DECC$SHR.EXE` + RMS file
  I/O; its process model (GCC `gcc` driver forks `cc1`/`as`/`ld` as subprocesses)
  must map onto OVMX image activation (IMGACT/DCL foreign-command spawn, the same
  path MMK uses to drive TCC.EXE today).
- **Known unknowns:** (a) does GCC's reliance on `fork`+`exec` of sub-tools map
  cleanly onto OVMX's activation model, or does the `gcc` driver need patching to
  activate images the OVMX way? (b) GCC's libiberty/host portability layer assumes
  a fairly complete POSIX host — how much of that is satisfied by musl-in-DECC$SHR
  vs. needs shimming? (c) memory/PIC/TLS model interactions between GCC's own
  image and IMGACT. (d) reproducibility: GCC bootstrap is sensitive to host
  determinism — the byte-identical bar (proven achievable for tcc/LINK.EXE) must
  be re-established for GCC.
- **De-risking:** stage this as **F2a: GCC cross-compiler built by the *foreign*
  toolchain but hosted-for-OVMX** (i.e. a `--host=<ovmx>` cross first, proving the
  OVMX host libc/RMS/activation surface is sufficient) **before F2b: GCC built
  *inside* OVMX**. The historical VAX-VMS/Alpha-VMS/GNV GCC lineage is the
  evidence this terminates.

### 6.2 The kbuild/GNV POSIX layer (LONG POLE #2, softest estimate)

- **The real question is scope:** full GNV port vs. a lean POSIX shim. The honest
  answer is *unknown until measured* — it requires an **empirical host-tool
  inventory**: instrument a real `linux-6.12` `make defconfig && make bzImage
  modules` and capture **every** binary it `exec`s and every shell builtin it
  relies on. That inventory (a could-start-now yeoman item, §7) converts this from
  hand-waving to a concrete list of OVMX images to produce.
- **Known hard sub-parts:** GNU make itself as an OVMX image (kbuild uses advanced
  GNU-make features, so BSD make / a minimal make will **not** substitute); a
  POSIX `sh` (not DCL — kbuild scripts are `sh`, so OVMX needs a real `sh` image,
  likely a ported `dash`/`ash` over musl); `perl` (needed by a few generators — a
  real risk, perl is itself a large port; mitigations: newer kernels have reduced
  perl use, and some scripts have non-perl fallbacks — **verify which the pinned
  6.12 actually needs**); `flex`/`bison` (kconfig); `openssl` (module signing — or
  reuse the sign-file path already in the Dockerfile; key is a build secret, not committed); `libelf`
  (objtool/modpost — a hard dependency on modern x86). Each is a distinct OVMX
  image port.
- **Risk:** this could balloon into "port half of a GNU userland." The lean-shim
  hypothesis (only the tools kbuild *actually* execs, over musl+RMS) is the
  mitigation, but it is a hypothesis until the §7 inventory lands. **This is the
  item most likely to be under-estimated.**

### 6.3 Second-order risks

- **Determinism regressions.** cut-release currently cmps four artifacts
  byte-for-byte across two `--no-cache` cuts (rd `vms-d73`), and the Dockerfile
  pins `SOURCE_DATE_EPOCH`/`KBUILD_BUILD_*`. An OVMX-built kernel must re-establish
  that bar — GCC bootstrap + kbuild both have well-known nondeterminism sources
  (build paths, timestamps, `__DATE__`, hash-order). Non-trivial but solved-in-
  principle (the foreign build already solved it).
- **Boot-strap circularity.** To *build* the kernel inside OVMX you must first
  *boot* OVMX — on a kernel. That is fine (Debian builds linux-image on a
  running Debian): the OUTER OVMX runs on a previously-built (eventually
  OVMX-built) kernel, and produces the NEXT kernel. The first OVMX-built kernel is
  necessarily produced on an instance booted from a foreign-built one; the loop
  closes on the *second* generation. State this explicitly so no one mistakes it
  for a paradox.
- **vmsfs.ko VFS-version coupling.** The 6.12-LTS pin exists because `vmsfs.ko`
  targets the pre-6.14 mount API. Moving the build inside OVMX does not fix that;
  it inherits it. The kernel-version pin and the vmsfs mount-API port (tracked
  separately) are orthogonal to the ouroboros — note it so it is not conflated.
- **Alpha.** Alpha is first-class (memory `vms-8ce`) but has **no OVMX kernel
  substrate yet** (Linux/Alpha exists upstream; whether OVMX targets Linux/Alpha
  or waits is an open call). GCC's Alpha back end is in-scope for F2 (it is why
  GCC, not clang); the Alpha *kernel* loop is a **future** extension of §5, not
  planned here beyond "GCC must retain the alpha target."

---

## 7. Sequencing vs the 1.0 spine, and the yeoman's work that starts now

### 7.1 Strict ordering vs 1.0

- **1.0 does NOT depend on any of this.** 1.0 ships the foreign-built kernel
  (`Dockerfile.bootable`) and the Build-native userland gate (rd `vms-678`/`vms-89d`).
  The ouroboros is **post-1.0** and must not steal seats from the 1.0 spine.
- **But the 1.0 spine is the prerequisite substrate for the ouroboros.** Every
  ouroboros stage runs *on the self-hosting machinery 1.0 builds*: IMGACT
  activation, DCL foreign-command spawn, MMK drive, RMS file I/O, `DECC$SHR.EXE`.
  So finishing the 1.0 spine (S4 fixpoint ✓, MMK-drives-the-tree `vms-89d`, RTL
  breadth `vms-801`) is what *unblocks* F1/F2. **Nothing here competes with 1.0;
  it queues behind it.**

### 7.2 Could-start-now yeoman's work (independently useful before the loop closes)

These are worth doing early because each pays off on its own, regardless of when
the loop closes:

1. **Kbuild host-tool inventory (§6.2).** Instrument a real `linux-6.12` build,
   capture every exec'd binary + shell feature. Pure investigation, no OVMX
   dependency, and it is the input that de-risks the single softest estimate.
   **Highest-value early item.**
2. **binutils-as-OVMX-image spike (F1).** binutils is the smaller, more portable
   half of the compiler port and is a prerequisite of GCC; standing up
   `as`/`ld`/`ar` as OVMX images is useful the moment it exists (it strengthens the
   toolchain pillar `vms-ade`) and de-risks F2.
3. **GCC-hosted-for-OVMX cross (F2a).** A `--host=<ovmx>` GCC cross-compiler,
   built by the foreign toolchain, proves the OVMX host surface (musl/RMS/
   activation) is sufficient for GCC **without** first solving the in-OVMX build.
   This is the key GCC de-risk and doubles as **the production compiler that
   unlocks the Fortran/COBOL/BASIC corpus** (compat register `vms-082`) — a large
   independent payoff the whole "run real VMS software" goal (R2) leans on.
4. **NetBSD build.sh host-layer scoping (N1).** Because build.sh is self-
   contained, scoping *exactly* what OVMX must supply it is a small, bounded
   investigation that identifies the cleanest first loop to actually close.

### 7.3 Recommended global sequence

```
1.0 spine (vms-678/vms-89d/vms-801)         ← prerequisite, already prioritized
        │
        ├── (could-start-now, parallel-safe) kbuild inventory · binutils spike ·
        │    GCC-hosted-for-OVMX cross · build.sh scoping
        v
F1 binutils images → F2 GCC images  (shared trunk; F2a cross de-risks F2b)
        │
        ├── NETBSD loop first (N1 lean → N2 → N3): cleanest host layer, closes a
        │   real loop earliest — proves the ouroboros end-to-end on the easier
        │   substrate.  (Gated also on the P4 boot arc vms-8e8.)
        │
        └── LINUX loop (L1 GNV lift → L2 → L3 → L4): larger POSIX-layer effort,
            runs as the major parallel push once F2 exists.
```

Rationale for **NetBSD-first to *close* the loop, Linux-first to *reach* it**: the
NetBSD host layer (N1) is far smaller, so the *first* fully-closed ouroboros is
cheapest on NetBSD — but NetBSD's target (vax) is only reachable once the P4 boot
arc lands. In practice F1/F2 are shared and unavoidable; the choice of which loop
to *finish* first is "whichever of {NetBSD boot arc done, GNV layer built} clears
first." Keep both alive; do not serialize them behind one another beyond the shared
F1/F2 trunk.

---

## 8. What "done" means for the whole program

The ouroboros is closed when, for **at least one** substrate, this holds with **no
non-OVMX toolchain anywhere in the path**:

> An OVMX instance, booted on a kernel, uses OVMX-native GCC + binutils + build
> system (all IMGACT-activated images running against `/dev/vms`) to compile and
> link the **next** kernel — including `vms.ko`/`vmsfs.ko` in-tree — and that
> kernel boots OVMX to a DCL prompt with the executive test green.

Closing it on **both** substrates (Linux + NetBSD/VAX) is the terminal state of
the distribution: OVMX/Linux and OVMX/NetBSD each regenerate themselves whole,
kernel included. That is the ouroboros — the self-hosting north star's true
summit, and the point at which "OVMX is a distribution" is a complete, not
partial, claim.
