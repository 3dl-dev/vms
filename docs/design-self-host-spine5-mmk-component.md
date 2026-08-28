# OVMX Self-Host Spine #5/#6/#7 — MMK builds a real component (vms-fe4 / vms-d1b / vms-6be / vms-725)

> Status (spine #5, vms-fe4): plan + toolchain-determinism proven on the host.
> Status (spine #6, vms-d1b): **MMK-driven EXECUTION green in QEMU for the
> COMPILE stage** — the shipped MMK.EXE drives the static TCC.EXE over its
> persistent mailbox DCL, against a real `/dev/vms`, to compile a real
> src/libvmssys runtime TU to a valid ELF object, byte-identical.
> Status (spine #7 archive, vms-6be): the **ARCHIVE** stage joined it — MMK
> drives TCC.EXE + LIBRARIAN.EXE to compile runtime TUs and `/CREATE`
> `OVMXRT.OLB`, byte-identical.
> Status (spine #7 LINK→activate, vms-725): **THE WHOLE CHAIN now runs in-guest.**
> The same MMK.EXE drives compile → archive → **LINK** over its mailbox DCL —
> TCC.EXE compiles the runtime TU + the driver `OVMXRTRUN.C`, LIBRARIAN.EXE archives
> `OVMXRT.OLB`, LINK.EXE `--executable --use DECC$SHR.EXE` links the runnable image
> `OVMXRT.EXE` — and the harness **ACTIVATES** it through IMGACT (PT_INTERP) to its
> oracle **exit 216**; the compile+archive outputs are byte-identical across two
> in-guest drives (LINK+activate rides one drive, its output byte-identity
> host-proven — a 120s-VM-budget choice), zero bash
> (`tests/qemu/test_syssvc_mmk_build.c`, wired into the standing
> `kernel-executive` CI barrier). This is self-hosting's final MMK-driven rung:
> **MMK builds a real OVMX component to a running image entirely inside OVMX.**
> **BUILD.COM has since been retired** (vms-e49) — the S4 self-host *fixpoint*
> (a multi-TU LINK.EXE gen2==gen3 build, a 1.0 gate) it was load-bearing for is
> now driven by the MMK `LINKSH.MMS`; see "Retiring BUILD.COM" below. Builds on spine #4 (vms-b23, MMK's
> mailbox-driven DCL drive) and spine #3 (vms-ca9, LIBRARIAN/.OLB). Reads
> `docs/design-self-host-mmk-spine.md` (the anchor) and
> `docs/design-mmk-exec-drive-ovmx.md` first.

## Goal (from the item)

MMK.EXE builds a **real multi-translation-unit OVMX component** end to end inside
OVMX — compile each TU via TCC.EXE, archive via LIBRARIAN.EXE, link via LINK.EXE
— **driven by MMK over DCL (zero bash in the build path)**, with **byte-identical
output across two independent builds**, retiring BUILD.COM for the MMK path.

## The component

The **OVMX freestanding runtime** — the real `src/libvmssys` TUs `vms_string.c`,
`vms_snprintf.c`, `vms_math.c` (CLAUDE.md Rule 3: `-ffreestanding -fno-builtin`,
no glibc) plus a small driver. Chosen because it is a genuine, load-bearing OVMX
component that is multi-TU, self-contained (headers = its own + `<stdint.h>`),
and TCC-compilable. Its natural build product is an object **library** (`.OLB`)
plus a linked image, so it exercises **all three** toolchain stages (compile →
archive → link) — the full self-host chain.

Committed build description: **`tests/toolchain/component/OVMXRT.MMS`** (the MMK
descrip.mms) + **`OVMXRTDRV.C`** (the driver). MMS/MMK syntax (macros `$(NAME)`,
automatic macros `$(MMS$TARGET)`/`$(MMS$SOURCE)`, `target : deps` + TAB actions)
is grounded in the MadGoat MMK manual (`tests/corpus/tier3-mmk/mmk_doc.sdml`) and
the public HP/VSI MMS Reference Manual (clean-room, Rule 8).

## What is PROVEN this session (host, no /dev/vms)

Two host ctests under `tests/toolchain/` (both green):

1. **`toolchain-mmk-component-plan`** (`run_mmk_component_plan.sh`) — the shipped
   MMK.EXE reads `OVMXRT.MMS` through OVMX RMS, expands its MMS macros, builds
   the dependency graph, and emits the **full multi-TU build plan** in `/NOACTION`
   mode: the **four TCC compiles**, then the **LIBRARIAN archive**, then the
   **LINK**, in **dependency order** (all compiles < archive < link), and
   **byte-identical across two runs**. This is spine #4's single-TU parse proof
   scaled to a real multi-TU + library component. Zero bash — MMK drives.

2. **`toolchain-mmk-component-build`** (`run_mmk_component_build.sh`) — the
   OVMX-native **LIBRARIAN.EXE** and **LINK.EXE** build the component's `.OLB`
   and image **BYTE-IDENTICALLY across two independent builds** (`cmp` clean on
   both) from the **real component objects**, with **selective member pull**
   (2 of 3 members — SNPRINTF+STRING, not MATH — proving symbol-driven
   extraction), the image validated by OVMXDUMP. LIBRARIAN zeroes the `ar`
   mtime/uid/gid fields (the classic archive reproducibility trap). Combined with
   **TCC.EXE's compile determinism** (already proven by
   `src/imgact/test/run_tcc_selfhost.sh`, gen-2 == gen-3 objects byte-identical),
   the whole **TCC → LIBRARIAN → LINK** chain for this component is reproducible
   — the byte-identical-twice bar for the build **output**.

## MMK-driven EXECUTION in QEMU — the COMPILE stage, now GREEN (spine #6, vms-d1b)

MMK's real drive (`sp_open`/`sp_send`/`sp_receive` in
`tests/corpus/tier3-mmk/ovmx/ovmx_mmk_sp.c`) executes commands by spawning a
**persistent DCL over VMS mailboxes** and therefore **requires a real executive
(`/dev/vms`) — QEMU** (with no `/dev/vms` it honestly fails `SS$_NOSUCHDEV`,
Rule 9 / INV-6). Spine #4's capstone `tests/qemu/test_syssvc_mmk_drive.c` proved
MMK drives a **DCL-computed** action (`6*7 → 42`) that way; spine #6 turns the
driven action into a **real TCC compile of a real component TU**. The suite that
proves it is **`tests/qemu/test_syssvc_mmk_build.c`** (extends the spine #4 drive
test). What it establishes, all against a real `/dev/vms`:

1. **Static TCC.EXE, built and staged.** `tests/toolchain/mk_tcc_static.sh`
   builds the vendored tinycc as a **plain static (musl)** binary — NOT the
   IMGACT-packaged image `src/vmslink/mk_tcc.sh` produces for the in-OVMX
   self-host fixpoint. DCL activates a foreign command by `fork()`+`execve()` of
   the image path (`dcl_cmd_process.c` `dcl_exec_foreign_command` →
   `dcl_activate_image`: a plain static image is not in-process-eligible, so
   `imgact_activate` returns `SS$_UNSUPPORTED` and DCL forks it), exactly as it
   activates any real utility and exactly how the staged `MMK.EXE`/`DCL.EXE` run
   — **no IMGACT/shareable staging on this path**. The Dockerfile stages it at
   `SYS$SYSTEM:TCC.EXE`, tinycc's own headers at `SYS$SYSTEM:[.include]`, and —
   because tinycc ships no `<stdint.h>` and the initramfs has no `/usr/include`
   — musl's `stdint.h` + its two-file `bits/` closure beside them.
2. **Foreign-command definition, self-contained in the descrip.mms.** No
   `.FIRST`/login mechanism is needed: MMK streams **each TAB-indented action
   line** into the spawned DCL as its own command record, so the action list is
   simply `TCC :== "$/vms/…/TCC.EXE"` then `TCC …`. The image path is **quoted**
   so DCL's `:==` assignment preserves its case (`dcl_exec.c` `exec_assign`
   upcases an unquoted `:==` value, which would turn `/vms` into `/VMS` and miss
   the case-sensitive Linux mount). The foreign-command **tail** is delivered raw
   (`cmd->raw_tail`), so tcc's case-sensitive flags (`-x c -c …`) survive — the
   same whole-line-raw delivery native `LINK` relies on.
3. **First-ever TCC.EXE run inside QEMU** — the spawned DCL forks the static
   TCC.EXE, which compiles the staged `VMS_STRING.C` freestanding to an `.OBJ`
   through plain musl file I/O against the SYSDISK work directory.
4. **Independent oracle + determinism** — the parent (which never runs a
   compiler) asserts the produced object is a valid ELF relocatable carrying the
   real runtime symbol `vms_strlen`, and is **byte-identical across two
   independent MMK-driven in-guest builds** (`cmp`). Host determinism is
   separately proven by `tests/toolchain/run_tcc_static_component.sh` (the same
   `mk_tcc_static.sh` TCC.EXE compiles `vms_string`/`vms_snprintf`/the driver to
   byte-identical objects) and `src/imgact/test/run_tcc_selfhost.sh`
   (gen2==gen3).

The suite honest-skips (77) with no `/dev/vms` (the negative-control contract),
and is anchored by the `mmk-drive-command-not-sent` per-facility control
(`facility_defects.sh`), whose `sp_send`-drops-the-command mutation reddens it
for the same root as the spine #4 drive test. **The CI gate is the standing
`kernel-executive` barrier**: it builds `tests/qemu/Dockerfile` from the checked-
out tree (a clean context) and runs the whole QEMU harness, of which this suite
is one — so the MMK-driven native build is exercised from a clean checkout with
zero bash in the build path on every run, no new job required.

## What spine #7 drove in-guest — the WHOLE chain (vms-6be archive + vms-725 LINK→activate)

**Spine #7 drove compile → archive → LINK → activate in-guest, end to end.**
`test_syssvc_mmk_build.c` streams a descrip.mms over MMK's mailbox DCL that:

1. compiles the runtime TU `VMS_STRING.C` (+ the driver `OVMXRTRUN.C`) with
   TCC.EXE;
2. `/CREATE`s `OVMXRT.OLB` from the runtime object with LIBRARIAN.EXE;
3. `LINK --executable --use <abs>/DECC$SHR.EXE`s `OVMXRTRUN.OBJ` + `OVMXRT.OLB`
   into the runnable image `OVMXRT.EXE` (pulling `VMS_STRING` from the library —
   the driver references `vms_strlen`);

then the harness **activates** `OVMXRT.EXE` (`fork+exec` → the kernel loads its
`PT_INTERP=/vms/.../IMGACT.EXE`, which maps `DECC$SHR.EXE` from `SYS$LIBRARY` and
binds the one cross-image import) and asserts it **exits 216**
(`vms_strlen("OVMXRT")·36`).

**The whole-VM 120s budget shaped the suite (two hard lessons).** The
kernel-executive gate runs ~76 suites in ONE QEMU VM under `run_tests.sh`'s fixed
`timeout 120`, and the set is already near that edge on CI's (slower) runner. Two
attempts overran it:
- Linking + activating on *both* MMK drives, each compiling **two** runtime TUs,
  was too heavy: the 40s per-drive bound elapsed after the archive but before the
  LINK (killing MMK mid-drive), and the two full drives blew the whole-VM budget.
- Even after moving the LINK to one drive, compiling **two** runtime TUs per drive
  still tipped CI: `mmk_build` itself passed, but the VM hit 120s during the very
  next suite (`mmk_drive`) and the qemu timeout reddened the run.

So the suite is deliberately LIGHTER than the vms-6be compile+archive suite:
**each drive compiles ONE runtime TU** (`vms_string`), and the **LINK+activate runs
on drive #2 only** (drive #1 is the lighter compile+archive drive). The
**compile+archive OUTPUTS are asserted byte-identical across the two drives**
(determinism). The LINK OUTPUT's byte-identity, and LIBRARIAN archiving MULTIPLE
members, are proven **on the host** (`run_mmk_component_build.sh` links + archives
byte-identically); in-guest the suite proves the compile→archive→LINK→activate
chain end to end. The per-drive bound was raised 40s→60s.

Key choices:

- **`vms_math.c` is excluded** (SSE `"x"` inline asm not tcc-compilable on
  x86_64); `vms_string.c` is the runtime TU the suite drives.
- **Only `DECC$SHR.EXE` is `--use`d, not the six shareables.** The component is
  freestanding: its *sole* external symbol is `vms_strlen` (defined in the
  `.OLB`), so the executable's only cross-image import is crt0/exit from
  DECC$SHR. That collapses the "six shareables" the residual once anticipated to
  **one** — DECC$SHR is `mk_decc_shr.sh`'s whole-archived musl `libc.a` +
  `libgcc.a`, which **builds clean in the ubuntu+`musl-gcc` Dockerfile** (the
  alpine-only assumption was unfounded; the producer graph links there too).
- **Absolute `--use` path, not `SYS$LIBRARY:`.** The drive passes the absolute
  `DECC$SHR.EXE` path (as the compile/archive steps pass absolute TCC/LIBRARIAN
  paths and `mk_dcl.sh` passes absolute shareable paths); the `$` in `DECC$SHR` is
  an ordinary VMS filename character in the raw-delivered DCL tail, not an
  apostrophe substitution. `SYS$LIBRARY:` logical-name resolution inside LINK.EXE
  remains an unneeded authenticity nicety.
- **`LNK`, not `LINK`** — `LINK` IS the built-in DCL verb; `LNK` (not a prefix of
  it) falls through to the foreign-command symbol and forks the staged static
  LINK.EXE. The same trap as `LIBRARIAN` vs the `LIBRARY` built-in (vms-6be).
- **A multi-member archive + `vms_snprintf` are host-proven, not driven here.**
  Keeping the in-guest suite to one runtime TU is the budget choice above;
  `run_mmk_component_build.sh` archives three members byte-identically on the
  host. Separately, TCC compiles `vms_snprintf`'s varargs to tinycc's `__va_arg`
  helper, which DECC$SHR does not export — so exercising `vms_snprintf` in a
  *runnable* image would first need `__va_arg` appended to DECC$SHR (a follow-up
  nicety), independent of the budget.

Staged into `tests/qemu/Dockerfile`: the static `vmslink` LINK.EXE, `IMGACT.EXE`
(`make -C src/imgact ARCH=x86_64`), and `DECC$SHR.EXE` (`mk_decc_shr.sh`).

## Retiring BUILD.COM — DONE (vms-e49)

`distro/.../BUILD.COM` (the shell-free **DCL** build driver) has been **retired.**
The block was the S4 self-host FIXPOINT — a 1.0 gate (vms-62b): BUILD.COM was
load-bearing for `src/imgact/test/run_link_selfhost_native.sh` (CI job
`link-selfhost-native`), which drove it as a **multi-TU** build to rebuild
`LINK.EXE` from within OVMX and prove `gen2 == gen3` byte-stable, and for
`run_build_com_native.sh` (S3.2, single-TU compile→link→run).

That block cleared once the multi-TU LINK.EXE self-host fixpoint was **ported from
BUILD.COM to an MMK descrip.mms** (`tests/toolchain/component/LINKSH.MMS`): the CI
job `mmk-link-selfhost-fixpoint` (`run_mmk_link_selfhost_build.sh`) now proves
`gen2 == gen3` on the real OVMX LINK.EXE, the `toolchain-mmk-link-selfhost-plan`
ctest proves MMK resolves that plan, and the in-guest QEMU suite
`test_syssvc_mmk_build.c` proves the shipped **MMK.EXE** drives TCC→LIBRARIAN→LINK
and **activates** the produced image — superseding the single-TU RUN proof
`run_build_com_native.sh` carried. With those green, vms-e49 deleted `BUILD.COM`,
`run_build_com_native.sh`, `run_link_selfhost_native.sh`, and their two CI jobs.
`OVMXRT.MMS`/`LINKSH.MMS` are the MMK-native successors for the *build
description*; MMK is now the sole self-host build driver.
