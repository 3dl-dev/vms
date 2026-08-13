# OVMX Self-Host Spine #5/#6 — MMK builds a real component (vms-fe4 / vms-d1b)

> Status (spine #5, vms-fe4): plan + toolchain-determinism proven on the host.
> Status (spine #6, vms-d1b): **MMK-driven EXECUTION now GREEN in QEMU for the
> COMPILE stage — the shipped MMK.EXE drives the static TCC.EXE over its
> persistent mailbox DCL, against a real `/dev/vms`, to compile the real
> src/libvmssys runtime TU `vms_string.c` to a valid ELF object, byte-identical
> across two in-guest builds, zero bash** (`tests/qemu/test_syssvc_mmk_build.c`,
> wired into the standing `kernel-executive` CI barrier). The full
> compile→archive→**LINK**-to-image chain in-guest is the remaining residual
> (see "What is NOT yet driven in-guest" below); **BUILD.COM stays until that
> lands** (Rule 6/7 — no red gate shipped). Builds on spine #4 (vms-b23, MMK's
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

## What is NOT yet driven in-guest (the remaining residual)

The **full** `OVMXRT.MMS` chain (four TCC compiles, a LIBRARIAN archive, a LINK
to a runnable image) is **not** driven in QEMU yet. Two concrete blockers, each
tracked, keep it out of scope for spine #6:

- **`vms_math.c` is not tinycc-compilable on x86_64** — its SSE `"x"`-constraint
  inline asm and `__builtin_fabs` are rejected by tinycc (asserted, not assumed,
  by `run_tcc_static_component.sh`'s step 4). So the 3-TU library cannot be
  compiled by TCC-in-guest on the x86_64 QEMU path; the compile proof uses the
  two string/format TUs that DO compile.
- **The LINK-to-image step needs more staging than the fork+execve compile** —
  `LNK --executable --use SYS$LIBRARY:DECC$SHR.EXE …` requires the six OVMX
  shareables staged at `SYS$LIBRARY`, **logical-name resolution inside LINK.EXE**
  for `SYS$LIBRARY:`, and **IMGACT activation** of the produced image to validate
  it. None of that is staged in `tests/qemu/Dockerfile` today.

Closing these (a wider TU set / an alternate math TU, plus staging the shareables
and driving LINK+activate in-guest) is the natural spine #7.

## Retiring BUILD.COM

`distro/.../BUILD.COM` (the shell-free **DCL** build driver) stays as the
S3.2/S4 self-host artifact **until the MMK-driven LINK-to-image path is green in
QEMU** (the residual above); spine #6 proved the COMPILE stage only. `OVMXRT.MMS`
is its MMK-native successor for the build description. BUILD.COM is **not** deleted
in this session — deletion follows the full compile→archive→link execution proof
(Rule 6/7 — no red gate shipped).
