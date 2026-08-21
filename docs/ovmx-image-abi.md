# OVMX Image ABI: `-fPIC`, not `-fPIE` (vms-608, epic vms-da0 F2b)

> Status: **RULING** (conductor, vms-608). Companion to
> `docs/design-link-native-toolchain.md` (the LINK.EXE/IMGACT toolchain design)
> — this doc states the compile-time ABI contract that toolchain assumes, not
> the toolchain itself.

## The rule

**Every OVMX image (executable or shareable) is compiled `-fPIC`.**
`-fPIE` (gcc's own default for a plain `cc foo.c -o foo`) is **unsupported** for
any translation unit that references a cross-image `=DATA` symbol (a global
variable defined in one image and used from another — e.g. `stdout`, defined in
`DECC$SHR.EXE`, read from every consumer image).

This is not a per-program workaround. OVMX is a **shared-image system**: images
resolve most externals (libc/RTL entry points, other images' globals) through
another image's `.vms$sv` symbol vector at activation, not by static linking.
That is architecturally the same shape as a Linux process loading `.so`
libraries — and on Linux, code that will be loaded alongside other shared
objects and reference their data is compiled `-fPIC`, precisely so the compiler
emits **GOT-indirect** data references instead of direct ones. OVMX's producer
graph (`DECC$SHR`, `LIBVMS$SHR`, `LIBVMSPROCESS$SHR`, `LIBVMSFS$SHR`,
`LIBVMSLNM$SHR`, `LIBVMSRMS$SHR`, `DCL.EXE`, `TCC.EXE`, `LINK.EXE`, `AS.EXE`, …)
was already built this way — every `mk_*.sh` recipe in `src/vmslink/` hardcodes
`-fPIC` in its `CFLAGS` default. `CPPTEST.EXE` (`src/vmslink/mk_cpptest_ovmx.sh`)
was the sole holdout, compiled with gcc's own `-fPIE` default; vms-608 found and
fixed that gap.

## Why `-fPIE` breaks on OVMX (the vms-608 finding)

`-fPIE` still assumes a **copy relocation**: at a site that reads an external
global directly (`mov sym(%rip), %reg`), gcc emits a single `R_X86_64_PC32`
displacement against the symbol, trusting the eventual linker/loader (`ld` +
`ld.so`) to place a *local copy* of that symbol's data in the PIE's own image
and have `ld.so` fill it at load time — the "copy relocation" mechanism.
`-fPIC` never assumes that: it always emits `R_X86_64_REX_GOTPCRELX` (or plain
`R_X86_64_GOTPCREL`) against a GOT cell, and loads through the cell.

OVMX's `LINK.EXE` implements the GOT-indirect half of this (§7.4 of the
toolchain design doc: cross-image import binding via `.vms$imp` + import-GOT
cells) but **has no copy-relocation equivalent**. Its import-collection pass
(`link.c`, promoting relocations to cross-image imports) only recognizes
`is_call` (`PLT32`/`CALL26`) and `is_gotr` (GOT-family) relocation classes. A
direct `R_X86_64_PC32` against a symbol that turns out to be `UND` (defined in
a `--use` producer, not this image) matches neither test, so no import is
created for that site; relocation-apply then falls through
`resolve_ref()==0 → if (target==0) continue;` and leaves the displacement `0`.
The load reads whatever bytes happen to sit at `PC+0`, not the symbol's value.

**Confirmed empirically** (`src/vmslink/test/debug_stdout_data_reloc.sh`):
`CPPTEST.EXE`'s global-constructor `std::fputs(msg, stdout)` SIGSEGV'd because
`stdout` (a `FILE *const` defined in `DECC$SHR.EXE`) resolved to garbage — the
cross-image `=DATA` binding chain itself (the producer's ABS64 initializer,
IMGACT's load-time bias, the consumer's `.igot` cell) was proven correct under
`gdb`; the bug was purely the PC32-vs-GOT relocation-class gap above, present
only because `cpptest.o` was compiled `-fPIE` (gcc's default) instead of
`-fPIC`.

| Compile flag | Relocation emitted for `mov stdout(%rip), %reg` | LINK.EXE outcome |
|---|---|---|
| (default) / `-fPIE` | `R_X86_64_PC32` (direct, copy-reloc model) | **unresolved — silent 0, wrong data** |
| `-fPIC` | `R_X86_64_REX_GOTPCRELX` (GOT-indirect) | **resolved — correct data** |

## What this means for a compile driver on OVMX

Any C/C++ toolchain producing an OVMX image — whether the host gcc/g++ used by
the `mk_*.sh` build recipes, or a future on-target compiler — **must default to
`-fPIC` code generation for cross-image data access**, the same way a
distribution's system compiler defaults `-fPIC` for anything meant to become a
shared object. `-fPIE`/plain non-PIC output is a **category error** for an OVMX
image, not a stylistic choice; it silently corrupts any cross-image `=DATA`
reference the way vms-608 found.

**Where this is enforced today:**
- Every `src/vmslink/mk_*.sh` recipe that compiles OVMX's own C/C++ sources
  (`mk_link.sh`, `mk_vmslnm_shr.sh`, `mk_mmk.sh`, `mk_dcl.sh`, `mk_loginout.sh`,
  `mk_tcc.sh`, `mk_libvms_shr.sh`, `mk_vmsrms_shr.sh`, `mk_vmsfs_shr.sh`,
  `mk_as.sh`, `mk_vmssys_shr.sh`, `mk_vmsprocess_shr.sh`) hardcodes `-fPIC` in
  its `CFLAGS` default (`CFLAGS="${CFLAGS:--fPIC ...}"` — override-able for
  local iteration, but the shipped default is `-fPIC`, always).
- `mk_cpptest_ovmx.sh` now does the same for `cpptest.cpp` (vms-608): the host
  `g++` compile step is `g++ -std=c++17 -O2 -Wall -fPIC -c ...`.

**Where this is deliberately NOT (yet) enforced, and why:**
- **`TCC.EXE`** (`third-party/tcc`, OVMX's on-target C compiler, driven via
  `src/vmslink/mk_tcc.sh`) is built `-fPIC` as an *image* like everything else
  above — but that only governs how the *host toolchain* compiled TCC.EXE's own
  sources. It says nothing about what code TCC.EXE **itself generates** when a
  user runs `tcc foo.c` (or a future `CC` DCL verb wrapping it) on-target.
  TCC's x86_64 codegen (`third-party/tcc/src/x86_64-gen.c`) has **no
  gcc-style global `-fPIC`/`-fPIE` mode switch** at all — whether a given
  external-symbol reference goes through the GOT (`gen_gotpcrel`) or direct
  PC32 (`gen_addrpc32`) is decided per call-site by TCC's register-allocation
  state (`TREG_MEM`), not by a compile-wide flag. Whether TCC's current
  codegen already produces GOT-safe cross-image data references in practice,
  or would reproduce the exact vms-608 gap for some access pattern, has **not
  been empirically characterized** — that is a separate investigation (TCC is
  vendored third-party code; changing its codegen model is out of scope for a
  drive-by fix here). Tracked as a **vms-044 known-limitation**: no DCL `CC`
  builtin exists yet either (`src/vmsdcl` has no `CC` verb), so there is
  currently no on-target C compile driver whose default this doc's rule could
  even apply to end-to-end. When one exists, it must either force `-fPIC`-
  equivalent codegen for cross-image data or be proven to already emit it.

## Related, but distinct: the ABS64 producer-data gap (vms-212)

`docs/design-link-native-toolchain.md` §7.7 tracks a different, already-known
limitation: an **ABS64** reference to a symbol defined only in a `--use`
producer (as opposed to the PC32/GOT-cell reference this doc covers) is not yet
bound. Do not conflate the two — vms-608's finding is entirely about the
PC32-direct-vs-GOT-indirect **relocation class** choice at compile time, which
`-fPIC` fixes; vms-212 is about a relocation class (`ABS64`) `-fPIC` never
emits for external symbols in the first place.
