# musl arch overlay: `alpha-dec-vms` (OVMX port, RUNG 1)

This directory is OVMX's from-scratch musl **architecture port** for the
**`alpha-dec-vms`** (OpenVMS/Alpha, VMS ABI) target. musl upstream has **no**
Alpha port at all; everything here is clean-room (Rule 8): the Alpha ISA/ABI
facts come from the *Alpha Architecture Handbook*, the *OpenVMS Calling
Standard*, the public Alpha/Linux UAPI headers, and musl's own generic
patterns — never from VSI/HPE source or binaries.

It is checked in (not vendored): `build-musl.sh` **fetches** musl (pinned +
checksum-verified), overlays this tree onto it, and builds `lib/libc.a` with the
`tools/cross-alpha-vms` cross toolchain. Build/oracle tooling, Rule-9-clean —
nothing here runs inside the OVMX guest.

## What RUNG 1 delivers (vms-960)

`libc.a` **builds** as a real archive of `alpha-dec-vms` EVAX objects, where:

- the **portable C** members (string/mem/malloc/stdio-formatting/…) are genuine
  `alpha-dec-vms` objects (`strlen`, `malloc`, `memcpy`, `vsnprintf` are real
  text symbols — asserted by `build-musl.sh`), and
- the **syscall-dependent** members compile against an **honest stub** syscall
  layer that returns `-ENOSYS` — never faked success (INV-6).

This is the substrate the Alpha DECC$SHR C run-time is built on. It is **not** a
fully-working libc: the syscall backend, threads, dynamic linker, and fenv are
deliberately left as documented gaps for later rungs.

## Layout (mirrors the musl source tree)

```
arch/alpha-dec-vms/            arch overlay (top files + bits/)
crt/alpha-dec-vms/             crti.s / crtn.s
src/setjmp/alpha-dec-vms/      setjmp.s / longjmp.s
src/internal/vms_alpha_syscall.c   the honest -ENOSYS syscall stub
build-musl.sh                  fetch + overlay + build + verify (runs in-container)
```

`../run_musl_alpha_build.sh` drives the whole thing (build the toolchain image,
then run `build-musl.sh` inside it). CI runs the same path (`musl-alpha-vms`).

## What is REAL vs. STUBBED at RUNG 1

| Piece | Status |
|---|---|
| `atomic_arch.h` (Alpha LDx_L/STx_C + `mb` barriers) | **REAL** — correctness-critical. Alpha's memory model is the weakest of any musl target; every RMW is bracketed with full `mb` via `a_pre_llsc`/`a_post_llsc`. |
| `setjmp.s` / `longjmp.s` (s0-s6, ra, gp, sp, $f2-$f9) | **REAL** register save/restore per the Alpha calling standard. |
| `crt_arch.h` (`_start`) / `crti.s` / `crtn.s` | **REAL** minimal OSF/Alpha-style entry — only for musl-linked programs; the GCC port images use their own crt0 (this exists so the static-link set is complete). |
| Portable C (string, malloc, printf, …) | **REAL** genuine EVAX objects. |
| `syscall_arch.h` + `vms_alpha_syscall.c` | **HONEST STUB** — every syscall returns `-ENOSYS`. Real backend = the OVMX Alpha executive, **GAP3 / vms-8954**. |
| `bits/fenv.h` + fenv ops | **no-op** (honest): only `FE_TONEAREST` exposed; real Alpha FPCR control is a later rung. |
| threads (`clone`/`__unmapself`/`syscall_cp`) | **absent** (honest gap): no arch asm shipped, so thread creation is unresolved until the executive backend lands. |
| dynamic linker (ldso) | **not built** — static `libc.a` only. |
| syscall numbers (`bits/syscall.h.in`) | **placeholder** (generic Linux numbering) — the stub ignores the number; real Alpha numbering is GAP3. |
| `bits/fcntl.h`, `mman.h`, `signal.h` values | Alpha/Linux ABI (public), documented placeholder pending GAP3 reconciliation with the executive. They reach no kernel while the syscall layer is stubbed. |

## The load-bearing honesty gate

`build-musl.sh` runs a **preflight** that probes the *real* cross compiler and
**refuses to build** unless `alpha-dec-vms` is LP64 little-endian
(`sizeof(long)==sizeof(void*)==8`, `int==4`) with `long double==8` (IEEE
binary64). 64-bit pointers require `-mpointer-size=64` (alpha-dec-vms defaults
to 32-bit VMS short pointers), which the recipe passes on every compile. If the
model diverges (e.g. 32-bit `long`), the build stops rather than emit a subtly
broken libc — that is a real finding to escalate, not something to paper over.
