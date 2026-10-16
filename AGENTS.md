# AGENTS.md — OpenVMX

Guidance for anyone contributing to OpenVMX — human or AI coding agent, whatever tool you use.
This is the single, tool-agnostic source for how to build, test, and land a change here. Claude
Code, Cursor, Zed, OpenCode, Aider and others all read this file; there is no separate per-tool
instruction set to hunt for.

## What OpenVMX is

An independent, open-source re-implementation of a DEC-style operating environment — the **DCL**
command language, **Record Management Services (RMS)**, the VMS **system services**, and a kernel
**executive** — with its own userland (no libc, no ld.so) running above the Linux or NetBSD kernel.
Built clean-room from public documentation and observed behavior. Start with
`docs/product-vision.md` and `docs/architecture.md`.

## Build and test

Build/test tooling is containerized where noted. Don't install project dependencies on a shared
host.

```bash
# Development build — shared libraries, tests, and tools
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DBUILD_TOOLS=ON
cmake --build build -j$(nproc)

# Run the test suite
cd build && ctest --output-on-failure

# Static build (musl) — used for the bootable initramfs
cmake -B build-static -DCMAKE_C_COMPILER=musl-gcc -DOVMX_STATIC=ON -DBUILD_TOOLS=ON
cmake --build build-static -j$(nproc)

# Kernel module (needs kernel headers). vms.ko is the VMS executive; it carries
# the Files-11 ODS-2 ACP (the legacy vmsfs.ko VFS driver was retired, vms-165).
make -C src/kernel

# QEMU kernel-module tests
tests/qemu/run_tests.sh
```

The **bootable QEMU VM** is the project's one runtime (see below). Its build-and-boot steps live in
`docs/building.md`.

## The one runtime (hard invariant)

OpenVMX has exactly **one** runtime: the real-kernel / QEMU path, where `vms.ko` provides the VMS
**executive** and userspace reaches it through `/dev/vms`. Docker and podman are **build/test
tooling only** — never an OVMX runtime and never a supported way to "run OVMX." When the executive
is absent, a service must **fail honestly** (e.g. `SS$_NOSUCHDEV`) — it must never fake per-process
success.

## Invariants you must not break

These keep OpenVMX legally clean, authentic, and coherent. A change that violates one will not be
accepted.

- **Clean-room reverse engineering.** Every VMS format and protocol is derived ONLY from public
  OpenVMS documentation and the observed behavior of real systems. **Never** disassemble, decompile,
  or copy VSI/HPE source or binaries, and never paste leaked VMS source. This is what makes the
  interop legally protected — it is not optional.
- **One runtime** (above): never add a userspace fake for an executive facility.
- **No facades.** A service that cannot do the real thing returns an honest VMS error — never a
  plausible success that shares no real state. Facade-risk is a tracked bug class, not a shortcut.
- **Single-source records.** The roadmap, the release history, and the compatibility surface each
  have exactly ONE authoritative source; every other view is generated. Never hand-edit a generated
  file, and never add a second hand-maintained copy of the same facts — fix the source or the
  generator and regenerate.
- **VMS compatibility first.** When VMS behavior and implementation ease conflict, match VMS — or
  hide the difference honestly. See `docs/architecture.md`.
- **Third-party software is a faithfulness ladder, not a vendor-and-adapt shortcut.** Do not vendor a
  Linux program and minimally-adapt it to OVMX's surface. GCC, OpenSSH, and much else were *already
  ported to VMS.* Build OVMX's VMS-compatibility surface (DECC$SHR/CRTL, RMS, the VMS socket API,
  LINK.EXE/object/symbol-vector formats, TCP/IP services) **up the ladder until the existing VMS port
  builds against it, unchanged.** A vendored-and-hacked Linux artifact is at best a *labelled
  temporary bootstrap*; the deliverable is the VMS port compiling on OVMX. (tcc bootstraps the
  userland today; binutils vendoring was ripped under this rule.)
- **Conventions.** VMS status codes (odd = success) via `ssdef.h`; `src/libvmssys/` is freestanding
  (`-ffreestanding -fno-builtin`, no glibc); VMS naming (`sys$`, `lib$`, `str$`, `mth$`, `ots$`).

## Proposing and landing a change

- **Track work as GitHub Issues; submit changes as pull requests.** You do not need the maintainer's
  personal tooling to contribute.
- **Merge through the merge queue.** Main is gated by GitHub's merge queue: `gh pr merge <pr>
  --squash --auto` (or "Merge when ready" in the PR) puts a PR in the queue, which gates main + the
  PR, one group at a time, exactly as it will land, and merges it only when every gating workflow's
  `Merge gate: <workflow>` check is green -- the heavy boot/e2e/negative-control jobs included, which
  do not run on `pull_request` but do run in the queue. A PR is therefore fresh by construction and
  cannot be starved by other merges. Each gating workflow ends in a `gate` job that needs every other
  job (`tools/ci/check_merge_gates.py` fails a job left out of it).
- **Never queue a PR stacked on another unmerged PR.** The queue can reorder them and squash the
  base into the stacked PR's commit. Queue the base; once it lands, rebase the stacked PR onto main
  (`git rebase --onto origin/main <old-base>`), let its checks run, and queue it then.
- **Append-only lists are directories, so concurrent PRs never conflict.** A new LIBVMSSYS$SHR
  universal is a file `src/vmslink/libvmssys_shr.d/<NAME>` holding its class -- never a line appended
  to `libvmssys_shr.vec`, which is the RELEASED vector and changes only in a release cut
  (`tools/vmslink/vec_release_cut.sh`; the freeze gate refuses anything else). VMS-ordinal vectors
  (`src/vmslink/vms_vectors/`) stay pinned to VMS's offsets and take no fragments. A known-diff
  entry is a file `docs/oracle/*/known-diff.d/<a>-<b>`.
- **No mapped reds.** The queue merges only on green. A red on main is fixed first -- by the PR
  that fixes it, which goes through the queue like any other -- never mapped around.
- **`tools/ci/safe_merge.sh <pr>` is the emergency bypass only** (the queue down, or main red with
  its fix itself blocked). It refuses unless the PR is fresh against main, its own checks are green,
  (with `--require-heavy`, also that every heavy workflow it skipped passed in a dispatch on its head
  SHA, `tools/ci/merge_heavy_gates.py`; off by default now that the queue is the gate); `--allow 'WORKFLOW::JOB::FIXED-BY'` maps a red main
  carries to the PR fixing it and is refused when that red skipped steps main has not passed. It
  merges pinned to the head it checked and posts its evidence as the merge comment. Never call
  `gh pr merge` without `--auto` (the queue) outside this script.
- **CI must be green.** A PR that leaves CI red has shipped nothing. Gates include `Build & Test`,
  the kernel-executive proof against a real `/dev/vms`, and the VMS-native link / self-host jobs.
- **Tests are mandatory.** No change is complete while a test in any layer it touches is skipped,
  failing, or absent — and don't weaken a test to make a task pass.
- Match the surrounding code's style, naming, and conventions.
- A facility negative control is one file, `tests/qemu/defects.d/<name>.defect` (its metadata,
  `defect_meta`, and its mutation, `defect_apply`); `tests/qemu/facility_defects.sh` lists the
  directory, so two PRs adding controls never conflict.

## Lab consoles

Talk to a lab emulator console (the real-VAX/Alpha nodes in the `ovmx-lab` namespace) only through
`tests/lab/tools/lab-console.sh send|ctrl|read <pod> <node> ...`, one command per call: `send` one
line, `ctrl` one control character (`E` for the SIMH `sim>` prompt), `read` the console tail. Lines
travel base64-encoded, so DCL's `$` and `"` survive. Don't write to a console FIFO or `kubectl exec`
into a lab pod by hand. The lab layout is in `tests/lab/README.md`.

## Where the project's truth lives

Read these rather than re-deriving status by hand:

- **Architecture & design** — `docs/architecture.md`
- **Build instructions** — `docs/building.md`
- **Product vision & scope** — `docs/product-vision.md`
- **Roadmap of record** (generated from the work board + git tags) — `docs/release-roadmap-to-1.0.md`
- **Compatibility surface** (source of truth) — `docs/compat/*.yaml`, rendered to
  `docs/compatibility-surface.md`
