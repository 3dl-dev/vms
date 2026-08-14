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

# Kernel modules (need kernel headers)
make -C src/kernel
make -C src/kernel/vmsfs

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
- **Conventions.** VMS status codes (odd = success) via `ssdef.h`; `src/libvmssys/` is freestanding
  (`-ffreestanding -fno-builtin`, no glibc); VMS naming (`sys$`, `lib$`, `str$`, `mth$`, `ots$`).

## Proposing and landing a change

- **Track work as GitHub Issues; submit changes as pull requests.** You do not need the maintainer's
  personal tooling to contribute.
- **CI must be green.** A PR that leaves CI red has shipped nothing. Gates include `Build & Test`,
  the kernel-executive proof against a real `/dev/vms`, and the VMS-native link / self-host jobs.
- **Tests are mandatory.** No change is complete while a test in any layer it touches is skipped,
  failing, or absent — and don't weaken a test to make a task pass.
- Match the surrounding code's style, naming, and conventions.

## Where the project's truth lives

Read these rather than re-deriving status by hand:

- **Architecture & design** — `docs/architecture.md`
- **Build instructions** — `docs/building.md`
- **Product vision & scope** — `docs/product-vision.md`
- **Roadmap of record** (generated from the work board + git tags) — `docs/release-roadmap-to-1.0.md`
- **Compatibility surface** (source of truth) — `docs/compat/*.yaml`, rendered to
  `docs/compatibility-surface.md`
