# OVMX Self-Host Spine #5 — MMK builds a real multi-TU component (vms-fe4)

> Status: **PARTIAL — plan + toolchain-determinism proven on the host; MMK-driven
> EXECUTION in QEMU is the precise residual gap, handed to spine #6 (vms-d1b, the
> CI gate).** Builds on spine #4 (vms-b23, MMK's mailbox-driven DCL drive) and
> spine #3 (vms-ca9, LIBRARIAN/.OLB). Reads
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

## The residual gap (spine #6, vms-d1b): MMK-driven EXECUTION in QEMU

MMK's real drive (`sp_open`/`sp_send`/`sp_receive` in
`tests/corpus/tier3-mmk/ovmx/ovmx_mmk_sp.c`) executes commands by spawning a
**persistent DCL over VMS mailboxes** and therefore **requires a real executive
(`/dev/vms`) — QEMU** (with no `/dev/vms` it honestly fails `SS$_NOSUCHDEV`,
Rule 9 / INV-6). Spine #4's capstone `tests/qemu/test_syssvc_mmk_drive.c` already
proves MMK drives a **DCL-computed** action (`6*7 → 42`) that way. Turning that
into a **real TCC/LIBRARIAN/LINK build inside QEMU** needs the following, each a
real integration point (not proven here):

1. **Stage the toolchain into the QEMU initramfs** (`tests/qemu/Dockerfile`).
   Today only `MMK.EXE` + `DCL.EXE` are staged at SYS$SYSTEM. Add **static-musl**
   `TCC.EXE`, `LINK.EXE`, `LIBRARIAN.EXE` (DCL activates foreign commands by
   `fork`+`execve` of the image path — `dcl_cmd_process.c` `dcl_exec_foreign_command`
   → `dcl_activate_image` — so plain static binaries suffice, exactly like the
   staged `MMK.EXE`/`DCL.EXE`; **no IMGACT/shareable staging required** on this
   path). `LINK.EXE`/`LIBRARIAN.EXE` are existing CMake targets (`vmslink`,
   `vmslibrarian`); **TCC.EXE needs a static-musl build target** (only `mk_tcc.sh`
   exists today, which builds the IMGACT-packaged image).
2. **Foreign-command definitions in the spawned DCL.** The descrip.mms actions
   say `TCC`/`LIBR`/`LNK`; the persistent DCL MMK spawns must have them defined
   (`TCC :== $SYS$SYSTEM:TCC.EXE`, …). Candidate mechanisms: an MMK `.FIRST`
   target action, MMK's ini-setup command, or a SYS$LOGIN the spawned DCL runs.
   **`.FIRST`/login-in-spawned-DCL support is unverified** in the OVMX MMK/DCL
   path and must be settled.
3. **Component headers staged** (`OVMX$INCLUDE:` → the `src/libvmssys` headers)
   and reachable by TCC's `-I` inside QEMU.
4. **First-ever TCC.EXE run inside QEMU** — TCC has only ever run on the host
   native/binfmt path (`run_tcc_selfhost.sh` et al). RMS temp-file handling and
   `.OBJ` write through OVMX RMS under a real `/dev/vms` are unexercised.
5. **Byte-identical assertion in QEMU** — MMK drives the build **twice**, `cmp`
   the two images. The determinism knobs are already in the tree (proven above);
   this asserts they hold end-to-end through the mailbox drive.

The QEMU suite that closes this — `tests/qemu/test_syssvc_mmk_build.c`, extending
`test_syssvc_mmk_drive.c` so the descrip.mms action is a real `TCC`/`LNK` build —
plus its negctl anchor in `facility_defects.sh` + floor bump, is **spine #6
(vms-d1b, the CI gate)**. This session deliberately does **not** ship an
unproven/red QEMU suite (Rule 6/7); it lands the host-provable halves and this
precise gap spec.

## Retiring BUILD.COM

`distro/.../BUILD.COM` (the shell-free **DCL** build driver) stays as the
S3.2/S4 self-host artifact until the MMK-driven QEMU path (spine #6) is green;
`OVMXRT.MMS` is its MMK-native successor for the build description. BUILD.COM is
not deleted in this session — deletion follows the spine #6 execution proof.
