# GCC-port host-surface gate — PROVEN (F2a rung-3, vms-e52)

> **Status:** GREEN, 2026-09-11. rung-3 (final) of vms-078 (F2a). Demonstrates the
> rung-2 verdict: the OVMX host CRTL/`DECC$SHR` **link+symbol surface suffices** to
> host the alpha-dec-vms OpenVMS GCC port — it builds, RUNS, and compiles multi-TU C
> to a correct EM_ALPHA target object. Reproducible gate:
> `tools/cross-alpha-vms/f2a-host-surface-gate.sh` (run on the k3s-worker).

## What was proven (and the exact claim scope)

A GCC configured **host = x86_64-alpine-linux-musl** (musl 1.2.5 — the same CRTL
symbol set OVMX's `DECC$SHR` wraps), **target = alpha-dec-vms**, built from the port
recipe (`build-toolchain.sh`, `--without-headers --enable-languages=c`, GCC-14.2.0),
was **actually run** and used to compile a minimal multi-TU C program to real target
objects. Evidence (k3s-worker, 2026-09-11):

```
$ alpha-dec-vms-gcc -dumpmachine
alpha-dec-vms
# the driver is a MUSL host binary — it RUNS on the host, not a link-only check:
$ file .../alpha-dec-vms-gcc
ELF 64-bit LSB executable, x86-64, interpreter /lib/ld-musl-x86_64.so.1, ...
# multi-TU compile (a.c references add() defined in b.c) — the compiler RAN:
OK compiled a.obj
OK compiled b.obj
# the produced target object is a real EVAX/Alpha object:
$ alpha-dec-vms-objdump -f a.obj
a.obj:  file format vms-alpha
architecture: alpha, flags 0x00000010: HAS_SYMS
```

**CLAIM (link/symbol-level, per INV-6):** *the OVMX host CRTL/`DECC$SHR` link+symbol
surface suffices to host the port GCC, which builds, RUNS, and compiles multi-TU C →
a correct EM_ALPHA target object.* Building host=musl proves the port GCC links
against, and runs over, the **same musl CRTL symbol set** OVMX's `DECC$SHR` provides
(rung-2: only 11 minor CRTL gaps, all alias/stub/optional).

**What this does NOT claim (deferred, downstream):** it does **not** prove GCC runs
correctly on OVMX with **executive-backed CRTL** — raw-musl-on-Linux runtime behavior
≠ musl-as-`DECC$SHR`-over-the-OVMX-executive runtime (VMS file/RMS semantics, the
`DECC$` veneer, executive-backed I/O). "essentially suffices" is a **link/symbol**
verdict. Running the port GCC under an OVMX x86_64 substrate (the `DECC$SHR`-over-
executive path) is a stronger, downstream rung — **not** required to green F2a, and
OOM/alpha-runtime-adjacent.

## Anti-LARP note

This gate deliberately does NOT accept "the host=ovmx GCC linked clean but didn't
execute." The port GCC is a **musl binary that runs on the worker Linux** (verified
`file` → `ld-musl-x86_64` interpreter), so the compile is a **real compiler run**,
and the produced object is **inspected** (`objdump -f` → `file format vms-alpha,
architecture: alpha`), not assumed. Non-alpha, OOM-safe (the k3s-worker), per the
placement discipline; the workshop (11GB, OOM-thrashing) was never used for the build.

## Reproduce

`tools/cross-alpha-vms/f2a-host-surface-gate.sh` — run from the repo root on the
k3s-worker (e.g. `tools/k3s/run-on-rail.sh --keep --dind main "bash tools/cross-alpha-vms/f2a-host-surface-gate.sh"`).
It builds the port GCC on Alpine (musl host, GMP/MPFR/MPC from Alpine's musl `-dev`
packages — the rung-2 host-provided-libs scenario), then runs the compile + inspects
the target object. ~60–90 min on a CPU-throttled pod (GCC-14.2 `all-gcc`); heavy, so
it belongs on the worker as a manual/nightly gate, not a per-PR leg.

## F2a complete

rung-1 (demand: 523 symbols) + rung-2 (gap map: CRTL surface essentially suffices, 11
minor gaps + deps) + rung-3 (this: build+run+compile→EM_ALPHA) together de-risk the
GCC-on-OVMX forcing function at the **host link+symbol surface** level. The remaining
GCC-on-OVMX work (executive-backed CRTL runtime, the alpha-dec-vms codegen crash
vms-032, the produced GCC running on OVMX-alpha) is downstream and OOM/alpha-gated.
