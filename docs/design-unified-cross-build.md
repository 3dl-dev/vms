# Unified cross-platform build — one CMake target list, every arch

> Status: **DESIGN** (rd `vms-509`, epic `vms-f10` "VAX first-class in the unified
> build + release stream", pillar `vms-ade` toolchain). Operator ruling
> 2026-08-15. This is a design + decomposition, not an implementation.
>
> Downstream design-cascade reviews (CLAUDE.md "Design Change Cascade") are
> enumerated in §7 — this changes build configs and CI, so the cascade fires.

## 1. The problem: the image set is defined twice

Every shipped OVMX userspace image is defined **canonically once** as a CMake
`add_executable` target and its `install(TARGETS ...)` destination:

| Image | Target | CMakeLists |
|-------|--------|-----------|
| `STARTUP.EXE` (PID 1) | `ovmx_init` | `src/ovmx_init/CMakeLists.txt` |
| `PROVISION.EXE` | `ovmx_provision` | `src/ovmx_provision/CMakeLists.txt` |
| `JOB_CONTROL.EXE` | `ovmx_job_control` | `src/ovmx_job_control/CMakeLists.txt` |
| `DCL.EXE` | `vmsdcl` | `src/vmsdcl/CMakeLists.txt` |
| `LOGINOUT.EXE` | `vms_login` | `tools/CMakeLists.txt` |
| `INSTALL.EXE` | `vms_install` | `src/install/CMakeLists.txt` |
| `LINK.EXE` / `LIBRARIAN.EXE` / `OVMXDUMP` | `vmslink` / `vmslibrarian` / `vmsdump` | `src/vmslink/CMakeLists.txt` |
| `PRODUCT.EXE` | `vms_product` | `src/product/CMakeLists.txt` |
| `PARTS.EXE` | `PARTS` | `src/apps/parts/CMakeLists.txt` |
| RTL libraries | `vmssys`, `vmsprocess`, `vms`, `vmslnm`, `vmsfs`, `vmsrms`, `vmsqueue` | `src/*/CMakeLists.txt` |

That target list produces the image set for the native/host arch (x86_64, and
aarch64) through `cmake --build`.

The **same set is then re-enumerated by hand** as ~25 per-image shell scripts in
`tools/cross-vax/build-*.sh`. Each script hard-codes the target's translation
units, its include set, its library dependency order, and its link line — for
example `build-ovmx-init-vax.sh` re-lists `ovmx_init.c ovmx_boot_netbsd.c
sysboot.c`, the `-I` set, the `--start-group ... --end-group` archive cycle, and
`-lpthread -lm -latomic`, all of which already live in
`src/ovmx_init/CMakeLists.txt`.

**That duplication is the drift.** Adding a facility or an image today means
wiring it into BOTH definitions, by hand, separately. The two lists diverge the
moment one is edited without the other, and the divergence is silent until a
per-PR VAX gate reds or — worse — until a release bundle is missing an image.
The operator ruling: kill the second list. (A hand-authored "vax manifest" would
be a *third* list — rejected for the same reason.)

### Why the scripts exist at all (the seam we are closing)

The scripts already drive **CMake for the libraries** — each `build-*.sh`
configures `src/libvmssys`, `src/vmsprocess`, … *standalone* with the toolchain
file and `cmake --build --target <lib>`. What they do **not** do is let the
**top-level** project configure under the cross toolchain and build the image
targets. Instead they hand-compile the image's TUs and hand-link them.

The reason is mechanical, not fundamental: the **top-level** `CMakeLists.txt`
does not yet configure cleanly under a cross toolchain. It pulls in the whole
test tree (`BUILD_TESTS`, un-runnable when `CMAKE_CROSSCOMPILING`), host-only
tooling, `find_library(ssh)`, the `OVMX_IMGACT` arch gate (x86_64/aarch64 only),
and the self-hosting `OVMX_LINK_NATIVE` graph. So the scripts sidestep the
whole-tree configure by driving per-subdir and re-linking the top by hand.

**The unification is: make the top-level project configure + build the shipped
image set under a cross toolchain file, and retire the hand path.**

## 2. The CMake toolchain-file model

### 2.1 The vax toolchain file (`vax--netbsdelf`)

`tools/cross-vax/toolchain-vax-netbsd.cmake` already exists and already carries
the compiler/sysroot/ar/ranlib and `CMAKE_SYSTEM_NAME NetBSD` /
`CMAKE_SYSTEM_PROCESSOR vax`. It is used today only to build libraries
standalone. The design **extends it** so the top-level project, selected with
`-DCMAKE_TOOLCHAIN_FILE`, produces the image set. It must encode three things the
per-image scripts currently encode by hand:

1. **The compiler/sysroot triple** (already present):
   `CMAKE_C_COMPILER = ${PREFIX}/bin/vax--netbsdelf-gcc`, `CMAKE_AR`,
   `CMAKE_RANLIB`, `CMAKE_SYSROOT = ${PREFIX}/sysroot`, the `FIND_ROOT_PATH`
   modes, `CMAKE_CROSSCOMPILING 1`, `CMAKE_C_COMPILER_WORKS 1` (VAX binaries do
   not execute on the amd64 host).

2. **Decision-A linkage (rd `vms-42d`).** This is a **distinct linkage mode**,
   not `OVMX_STATIC` and not the default SHARED: the OVMX RTL is linked as
   **static archives**, but the image is an **ordinary NetBSD ELF32-vax dynamic
   executable** activated by `/usr/libexec/ld.elf_so` (NEEDED = `libc`,
   `libpthread`, `libm`, `libatomic`). Concretely the toolchain file sets, on the
   NetBSD substrate:
   - `OVMX_LIB_TYPE = STATIC` (the RTL libraries become `.a`, matching what the
     scripts build) — **but it does NOT add the global `-static`** that
     `OVMX_STATIC` adds, because the image must stay dynamic for `ld.elf_so`.
   - It does **not** apply the `OVMX_IMGACT` global flags
     (`--dynamic-linker=IMGACT.EXE`, `--hash-style=sysv`,
     `-mtls-dialect=gnu2`) — on this substrate activation is `ld.elf_so`, not
     IMGACT (a **labeled Rule-8 divergence**, ratified in `vms-42d`).
   - It provides the common system link deps once
     (`-lpthread -lm -latomic`) via `CMAKE_C_STANDARD_LIBRARIES` or a substrate
     block, so no per-target repetition is needed.

3. **The static-RTL archive cycle.** The default host build links the RTL as
   SHARED `.so`, so the `libvms ⇄ vmsfs` mutual dependency never needs archive
   grouping. Under Decision-A the RTL is **static**, and the cycle must be
   resolved with `--start-group`/`--end-group` (what the scripts do by hand).
   CMake emits a link group automatically when the mutual dependency is
   **declared both ways** in `target_link_libraries` — so the fix is to declare
   the `libvms ⇄ vmsfs` edge bidirectionally (see §3), not to hand-write the
   flag. If CMake's automatic grouping proves insufficient at our minimum
   version, the fallback is a toolchain-level `--start-group` around the RTL
   set, or a CMake-min bump to 3.24 for `$<LINK_GROUP:RESCAN,...>` — an
   **operator-reserved** version-policy call (§8).

The substrate/arch branches the image targets already key off
(`CMAKE_SYSTEM_NAME STREQUAL "NetBSD"`, `CMAKE_SYSTEM_PROCESSOR MATCHES "vax"`)
are set by this file, so `libvmssys` takes its `netbsd` substrate branch and
`ovmx_init`/`ovmx_provision` pick `ovmx_boot_netbsd.c` — **already true today**,
no new source forks.

### 2.2 How per-arch toolchain files generalize

The toolchain file is **the only thing that varies per arch**; the CMakeLists are
shared. The generalization:

| Arch | Substrate | Toolchain file | Linkage | Notes |
|------|-----------|----------------|---------|-------|
| **x86_64** | linux (raw-freestanding) | none today — native host build; a `toolchain-x86_64-linux.cmake` is optional and only needed for explicit cross | SHARED / `OVMX_IMGACT` / `OVMX_STATIC` per mode | primary arch; the host `cmake --build` is already the "unified build" for it |
| **aarch64** | linux (raw-freestanding) | `toolchain-aarch64-linux.cmake` (cross from x86_64 host) | same modes as x86_64 | needs `-mno-outline-atomics`, TLSDESC default |
| **alpha (axp)** | linux (raw-freestanding) | `toolchain-alpha-linux.cmake` | same modes | requires `src/libvmssys/arch/alpha/{crt0,syscall,sigreturn}.S` (freestanding backend proven under qemu-alpha, vms-054) and an IMGACT alpha backend *only if* `OVMX_IMGACT` is used; the default cross build does not need it. Folds in as a **parallel** rung under `vms-f10` once vax proves the model (§8). |
| **vax** | netbsd (link-libc) | `toolchain-vax-netbsd.cmake` (this design) | **Decision-A**: static RTL, dynamic image, `ld.elf_so` | 32-bit little-endian ILP32 — the new width class the `vms-9dc` audit validates |

The shape is uniform: a toolchain file supplies `CMAKE_SYSTEM_{NAME,PROCESSOR}`,
the compiler/sysroot, and the linkage-mode variables; the tree's existing
`CMAKE_SYSTEM_*`-keyed conditionals do the rest. Alpha is a freestanding-Linux
arch **exactly parallel to x86_64/aarch64** (it is *not* a NetBSD substrate; the
Alpha OpenVMS oracle in lab-Alpha is unrelated to the OVMX alpha target).

## 3. Making the shared targets toolchain-agnostic

Most targets already build under the cross toolchain unchanged (the scripts prove
the sources compile+link for elf32-vax). The concrete gaps to close so the
**top-level** project configures and the image targets build directly:

**A. Cross-configure guards in the top-level `CMakeLists.txt`.** When
`CMAKE_CROSSCOMPILING` (set by the toolchain file), default the un-runnable /
host-only knobs off, and build only the shipped image set:
- `BUILD_TESTS` → OFF (cross tests cannot execute; the freestanding
  `libvmssys` tests and every `add_test` are host-run).
- `OVMX_IMGACT` → OFF (Decision-A uses `ld.elf_so`; the IMGACT subdir also
  fatal-errors on non-x86/arm).
- `OVMX_LINK_NATIVE` → OFF (self-hosting is a host-arch mode — §4).
- `BUILD_TOOLS` stays **ON** (the boot images `ovmx_init`, `ovmx_provision`,
  `ovmx_job_control`, `vms_install`, `vms_product`, and `LOGINOUT` live behind
  it), but the `tools`/`src/*` subdirs it pulls in must each be image-set-scoped
  (below).
- `find_library(ssh)` / `src/vmsssh` already self-skips when libssh is absent
  in the sysroot — correct as-is for cross.

**B. The `ovmx-images` aggregate target — the single authoritative list.**
Introduce one custom aggregate target that `add_dependencies` on exactly the
shipped userspace images + their RTL. This is the replacement for the hand list:
`cmake --build build-vax --target ovmx-images` builds the whole shipped set for
whatever arch the toolchain selects, and **nothing else** (not `vmsscs`'s
AF_PACKET daemon, not host helper tools, not tests). Adding an image = add its
target to this aggregate — the one list that remains. Non-portable targets stay
*out* of the aggregate until individually ported; porting one = adding it to the
aggregate, never a new script.

**C. Two source/config fixes (already proven on `work/vms-installer-utils-vax`,
un-merged — carry them here).**
- `tools/vms_initialize.c`: `<linux/fs.h>`/`BLKGETSIZE64` is Linux-only. Split
  the block-device size query per platform — `BLKGETSIZE64` on `__linux__`,
  `DIOCGDINFO`/`struct disklabel` (`d_secsize * d_secperunit`) on `__NetBSD__`,
  fail-honest `-1` otherwise. Documented public NetBSD `disklabel(9)` API; not a
  VMS format (Rule 8 does not apply); Rule 9 hardware-generic block path.
- `src/vmsfs/ods2/CMakeLists.txt`: add the **standalone-configure guard** that
  `src/vmsfs` and `src/vmsprocess` already use — when configured with no
  enclosing `project()`, declare one and default `VMS_FS_INCLUDE_DIR` to the
  sibling `../include`. `INITIALIZE.EXE` links `ods2` directly for its genuine
  ODS-2 writer, and the top-level cross-configure must resolve the include var.

**D. Per-target link deps stay on the target; genuinely-per-arch bits stay
minimal and explicit.** The only legitimate per-arch conditionals in the tree
are the ones that already exist and key off `CMAKE_SYSTEM_*`:
- `libvmssys` substrate/asm selection (netbsd = no asm, link libc).
- `ovmx_init`/`ovmx_provision` boot-backend object
  (`ovmx_boot_netbsd.c` vs `ovmx_boot_linux.c + opcom_kmsg.c`).
- `imgact` arch dir + the `OVMX_IMGACT`-only global flags.

Everything else (include dirs, `target_link_libraries`, `OUTPUT_NAME`/`SUFFIX`,
`install(TARGETS)`) is **already toolchain-agnostic** and needs no change. The
one addition is declaring the `libvms ⇄ vmsfs` dependency bidirectionally so
CMake groups the static archives on the Decision-A path (§2.1).

## 4. Coexistence with self-hosting (LINK.EXE) — two build modes, do not collapse

There are **two** OVMX build modes and this change touches only the first:

**Mode 1 — host / bootstrap cross-build (this design).** `cmake --build` with a
per-arch toolchain file, using the **host cross-toolchain's** `ld` (NetBSD
`ld.elf_so` activation on vax; `ld`/`ld-musl` or IMGACT on Linux arches). This is
how CI and release cuts *bootstrap* the shipped image set for every arch. It is
`gcc` + `ld` + a sysroot. **This is the path being unified.**

**Mode 2 — self-hosting (`OVMX_LINK_NATIVE`, pillar `vms-ade`, north-star
`vms-678`).** The OVMX-native `LINK.EXE` + `.vms$sv` symbol-vector + `IMGACT.EXE`
activation graph (`src/vmslink/build_link_native.sh`, `mk_*_shr.sh`,
`run_dcl_native.sh`) that weans OVMX off the Unix linker/loader. It is **host-arch
only** (x86_64/aarch64 — RISC/x86 ELF relocs; `docs/design-link-native-toolchain.md`)
and is the "build OVMX inside OVMX" endgame.

**The invariant this design must not violate:** the unified cross-build is Mode 1
only. It does **not** replace, route through, or regress Mode 2.
`OVMX_LINK_NATIVE` stays a host-arch mode, orthogonal to the cross toolchain, and
cross builds never set it (§3-A forces it OFF under `CMAKE_CROSSCOMPILING`).
Decision-A's `ld.elf_so` activation on vax is a **separately-ratified, labeled**
Rule-8 divergence (`vms-42d`) — it does not weaken the self-hosting goal, which
remains native-activation on the host arches. Re-ratify only if 1.0 tightens to
native-activation-everywhere.

## 5. What gets retired, what stays script-based

### Retired — the per-image userspace `build-*.sh` (become CMake targets)

Library builds: `build-libvmssys-vax.sh`, `build-vmsprocess-vax.sh`,
`build-libvms-vax.sh`, `build-vmslnm-vax.sh`, `build-vmsfs-vax.sh`,
`build-vmsfs-core-vax.sh`, `build-vmsrms-vax.sh`.

Image builds: `build-ovmx-init-vax.sh`, `build-provision-vax.sh`,
`build-boot-images-vax.sh`, `build-job-control-vax.sh`, `build-loginout-vax.sh`,
`build-librarian-vax.sh`, `build-vmsdcl-vax.sh`.

Facility images + their negctl "teeth" tools: `build-access-vax.sh`,
`build-eflag-vax.sh`, `build-mbx-vax.sh`, `build-proctab-vax.sh`, and the
`build-{access,eflag,mbx,proctab}-tool-vax.sh` diagnostic/negctl builders,
`build-activation-vax.sh`, `build-devvms-vax.sh`.

The Decision-A **activation contract** those scripts assert (ELF32 · Digital VAX
· dynamically linked · `PT_INTERP == /usr/libexec/ld.elf_so` · NEEDED ⊆
{libc,pthread,m,atomic} · **no** `.vms$sv`/`.vms$imp` · does **not** request
IMGACT.EXE) is **not lost** — it is re-homed as a single post-build verification
(a `ctest` check or one `verify-vax-images.sh`) that iterates the CMake-produced
image files (keyed off the target/install list, **not** re-enumerating sources).
Likewise the negctl "compiler-rejects-bad-code" teeth become CMake/ctest
build-negative checks.

### Stays script/make-based — kernel modules (out of scope, correctly)

The kernel-module build is **not** a CMake ELF image and stays make/script-based,
exactly as `src/kernel` does on Linux (standalone Makefiles / kbuild):
- `build-vms-module-vax.sh` — `vms.kmod.o`, the executive module (a relocatable
  object built against kernel headers).
- `build-vmsfs-mount-vax.sh` — `vmsfs.kmod` loadable module + its static mount
  helper (`MODULE()` metadata).
- `build-vax-modular-kernel.sh` — the custom MODULAR NetBSD/vax kernel (R3/R4
  runtime parity, ~40-min build, gated on the boot capstone `vms-d59`).

The `tools/cross-vax/Dockerfile` (the `ovmx-cross-vax` toolchain image providing
`vax--netbsdelf-gcc` + sysroot) **stays** — it is the environment the toolchain
file points at. `toolchain-vax-netbsd.cmake` stays (extended). This is
build/test tooling, fully within CLAUDE.md Rule 9.

## 6. CI collapse

Today's per-PR VAX gates (`netbsd-vax-vms-crosscompile`,
`netbsd-vax-vmsfs-crosscompile`, `vmsdcl-netbsd-vax`, `ovmx-init-netbsd-vax`,
`ovmx-boot-images-netbsd-vax`, `librarian-netbsd-vax`, …) each invoke a
`build-*.sh`. They collapse to **one** job:

```
configure:  cmake -S . -B build-vax -DCMAKE_TOOLCHAIN_FILE=tools/cross-vax/toolchain-vax-netbsd.cmake
build:      cmake --build build-vax --target ovmx-images -j
verify:     verify Decision-A activation contract over the produced images
```

Plus the **unchanged** kernel-module jobs (`netbsd-vax-devvms`,
`netbsd-vax-vmsfs` mount, `netbsd-vax-boot` nightly) — those keep their scripts.
A broken VAX build still reds the PR; it is now one build, not N.

## 7. Design-cascade downstream reviews (fire on merge of each rung)

This modifies build configuration and CI, so the CLAUDE.md cascade applies:

1. **API / compatibility check (Systems).** Does forcing `OVMX_LIB_TYPE=STATIC`
   on the NetBSD substrate, and the bidirectional `libvms⇄vmsfs` edge, change any
   **host** build (SHARED / IMGACT / STATIC) or any shipped-image ABI? Go/no-go +
   breaking-change list. The host default build and the self-host graph must be
   byte-unchanged.
2. **Test-coverage check (QA).** The Decision-A activation assertions must
   survive the migration intact (no loss of the ELF32/interp/no-symbol-vector
   checks); the QEMU and self-host jobs stay green by SHA; the new single vax job
   must fail on a broken TU exactly as the negctl teeth did.
3. **Docs update (TechWriter).** `docs/building.md`,
   `docs/design-vax-mainstream-release.md` (R1/R2 reference the `build-*.sh`),
   `docs/design-ovmx-netbsd-syskrnl.md`, and the coexistence note in
   `docs/design-link-native-toolchain.md`.

## 8. Migration / decomposition plan

Outcome-scoped rungs (each a verifiable end state), sequenced. **Build the
toolchain path ALONGSIDE the scripts — both green — and delete the scripts only
in Rung E once the CMake path is proven equal. Never a big-bang swap:** these are
green, load-bearing gates (a broken vax job blocks every merge).

| Rung | Outcome | Depends on | Parallel? |
|------|---------|-----------|-----------|
| **A. Toolchain file + first image via `cmake --build`** | `toolchain-vax-netbsd.cmake` encodes Decision-A linkage (static RTL, dynamic image, no `-static`, no IMGACT flags, system link deps); top-level gains the `CMAKE_CROSSCOMPILING` guards (§3-A); `cmake -S . -B build-vax --toolchain … && cmake --build --target ovmx_init` produces a `STARTUP.EXE` that passes the Decision-A ELF assertions. One `add_executable` builds for vax through the top-level project. | — | — |
| **B. Shared targets toolchain-agnostic + the `ovmx-images` aggregate** | Carry the `vms_initialize.c` include split and the ods2 standalone guard (§3-C); declare the `libvms⇄vmsfs` edge bidirectionally; define `ovmx-images`; `cmake --build --target ovmx-images` under the vax toolchain produces the **full shipped userspace image + RTL set**. | A | — |
| **C. Migrate the boot image set** | The 5 boot images + `LIBRARIAN.EXE` build via `ovmx-images`; the Decision-A activation contract is re-homed as one post-build verify keyed off the target list. `build-boot-images-vax.sh` + its 5 per-image children + `build-librarian-vax.sh` are **superseded** (still present, both paths green). | B | ∥ with D |
| **D. Migrate the facility + test-tool builds** | The access/eflag/mbx/proctab facility images build via `ovmx-images`; the negctl "teeth" become CMake/ctest build-negative checks. Those `build-*.sh` superseded. | B | ∥ with C |
| **E. Retire scripts + reroute CI** | Delete the retired userspace `build-*.sh`; the per-PR VAX gates collapse to the single configure+build `ovmx-images` + verify job (§6). Kernel-module jobs untouched. | C, D | — |
| **F. `cut-release-vax.sh` derives its artifact set from the CMake target list** | R2 co-release stops calling N `build-*.sh`; it configures+builds `ovmx-images` for vax and copies the `install(TARGETS)` outputs into the bundle (artifact manifest = CMake install manifest, not a hand list). Kernel-module artifacts (`vms.kmod.o`, `vmsfs.kmod`) still come from their make path and are appended. Adding an image auto-appears in the vax release bundle. | E (or B for early spike) | — |

**Sequence:** A → B → {C ∥ D} → E → F. C and D are the parallelizable middle.
`cut-release-vax.sh` (rung F) is what makes the co-release invariant
(`docs/design-vax-mainstream-release.md` R2) inherit the single list — it is the
payoff that closes the loop from "one `add_executable`" to "in the release
bundle."

**Alpha (axp)** folds in *after* vax proves the model, as a **parallel arc under
`vms-f10`**: `toolchain-alpha-linux.cmake` + `src/libvmssys/arch/alpha/*.S`
following this exact template. It is not a rung of vms-509 (which is the vax
proving ground) — flagged as its own decomposition (§ operator decision 3).

## 9. Risks and operator-reserved decisions

**Risks (honest):**
- **This retires a working, green build path and rewires the gates that block
  every merge.** The CI reroute (Rung E) is the sharp edge: a regression in the
  single vax job reds all PRs. Mitigation is the alongside-then-delete sequencing
  above — the scripts stay until the CMake path is proven byte-equal.
- **Whole-tree cross-configure exposes latent Linux-only assumptions** in targets
  not previously cross-built. Mitigation: the `ovmx-images` aggregate scopes the
  vax build to a vetted allowlist; non-portable targets stay out until ported.
- **Static-RTL archive cycle** (`libvms⇄vmsfs`): if CMake's automatic
  `--start-group` at our min version proves insufficient, we need a manual link
  group or a CMake-min bump (decision 2).
- **Self-hosting invariant**: the one thing that must not regress. §4 is the
  guard; the API/test cascade (§7) must confirm the host + self-host builds are
  byte-unchanged.

**Operator-reserved decisions:**
1. **Freeze new `build-*.sh`?** The ruling calls this a foundation that "should
   precede more per-arch feature hand-wiring." Recommendation (proceed absent an
   answer): freeze *new* per-image scripts immediately — any new image lands as an
   `add_executable` in `ovmx-images` only; in-flight per-arch work continues but
   adds to CMake, not scripts.
2. **CMake minimum version.** Keep 3.16 with a hand-wired toolchain-level
   `--start-group`, or bump to 3.24 for `$<LINK_GROUP>`. Recommendation: keep
   3.16 + hand-wired group (reversible, no builder-version churn).
3. **Alpha timing.** Fold alpha in now (parallel) or after vax lands.
   Recommendation: after — vax is the proving ground; alpha is its own `vms-f10`
   decomposition on the same template.
4. **Does this pause per-arch feature work until it lands?** Recommendation: no
   hard pause, but couple it with decision 1 (new work adds to CMake, not
   scripts) so the drift stops accumulating immediately even before the scripts
   are deleted.
