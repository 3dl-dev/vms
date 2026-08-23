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

## RUNG 1 status: arch overlay COMPLETE; full build BLOCKED on a cross-toolchain gap

Two things were discovered empirically by building against the real
`alpha-dec-vms` cross toolchain (`build-musl.sh` runs both checks):

1. **`alpha-dec-vms` is LLP64, not LP64.** The task assumed LP64 (base on
   aarch64). The real compiler reports `int=4, long=4, long long=8, pointer=8`
   (with `-mpointer-size=64`), little-endian, `long double=8` (IEEE binary64),
   `wchar_t=unsigned int`. `long` is 32-bit by the OpenVMS ABI and cannot be
   widened without breaking VMS compatibility. **The overlay was adapted to
   LLP64** (`alltypes.h.in` decouples `_Addr/_Int64/_Reg` from the 32-bit
   `long`; `bits/setjmp.h` uses 64-bit elements; `atomic_arch.h`'s pointer
   ll/sc uses a 64-bit tie). musl configures and the C compile begins cleanly.

2. **The `alpha-dec-vms` GCC (`cc1` 14.2 + binutils 2.43 GAS) cannot assemble
   its own output for standard C-library function names.** The backend gives
   recognized C-RTL names the VMS `decc$` prefix and then emits asm its own GAS
   rejects, in two ways:
   - **Defining** such a function: `.ent decc$strlen` / label `strlen..en` /
     `.pdesc decc$strlen..en,null` — the `.pdesc` names `decc$strlen..en` but
     the entry label is `strlen..en` (mismatch) →
     `Error: redefined symbol cannot be used on reloc`. This fails even a
     pure-leaf `strlen`.
   - **Calling** such a function: `.linkage decc$t<callee>` per call site →
     `Fatal error: cannot generate BFD_RELOC_16 relocation`.

   Measured on musl 1.2.5: **702 of 1353 objects (~52%) fail**, and **all four**
   rung-1 targets (`strlen`, `malloc`, `memcpy`, `vsnprintf`) are among them —
   because they are exactly the standard names the backend mangles. A function
   with a non-CRTL name (e.g. `myfunc`) assembles cleanly. There are also 327
   `visibility attribute not supported` warnings (ELF `.weak`/`.hidden` /
   `weak_alias` unsupported on EVAX). **This is a cc1/binutils (GCC-lane) bug,
   not a musl-overlay bug** — it sabotages the very act of defining/calling libc
   functions, so no libc builds until it is fixed. Out of scope for this arch
   port; route: conductor 3-way, shared toolchain.

So a genuine, usable `libc.a` is **not** achievable with the current toolchain.
`build-musl.sh` produces a clearly-labeled **PARTIAL** `libc.a` from the objects
that do assemble (708 non-CRTL-named members), enumerates the failing set, and
**exits non-zero** because the four rung-1 target functions
(`strlen`/`malloc`/`memcpy`/`vsnprintf`) are absent — an honest red, never a
faked green. The `musl-alpha-vms` CI job reflects that state.

The syscall backend, threads, dynamic linker, and fenv are separately left as
documented gaps for later rungs (see the table below).

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
**refuses to build** unless the measured model matches what the overlay assumes:
`alpha-dec-vms` LLP64 little-endian (`int=4, long=4, long long=8, pointer=8`)
with `long double=8` (IEEE binary64). 64-bit pointers require `-mpointer-size=64`
(alpha-dec-vms defaults to 32-bit VMS short pointers), which the recipe passes on
every compile. If the model diverges, the build stops rather than emit a subtly
broken libc — a real finding to escalate, not something to paper over. This gate
is exactly what caught the LP64→LLP64 correction.
