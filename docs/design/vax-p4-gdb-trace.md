# vms-d4a option (a): observing the VAX cross-shareable activation

The P4 gate (`tools/cross-vax/run-shr-activation-vax.sh`) boots OVMX/VAX under
SIMH and activates `CONSUMER.EXE` (PT_INTERP=`IMGACT.EXE`); CONSUMER's only
`.vms$imp` import is `purdy_s_hash` from the shipped `LIBVMS$SHR.EXE`. Across
blind runs it exits not-golden and the harness cannot see **why**: resolve
failed, the CALLS mis-transferred, or purdy returned a wrong value.

## Why not literal SIMH gdb/breakpoints

The import cell, the linkage path and `LIBVMS$SHR`'s `purdy` entry are all P0
**user virtual addresses**, valid only while CONSUMER is current. The `vax`
simulator's `BREAK`/`EXAMINE` match the virtual PC in the current addressing
context with no NetBSD process/ASN awareness, and the addresses are unknown
before boot (IMGACT `mmap`s at kernel-chosen bases) — so a breakpoint can't be
pre-seeded and would spuriously fire in the ~4 other images that activate first
(STARTUP/PROVISION/LOGINOUT/DCL). SIMH instruction history is likewise
process-blind and needs a timed console interrupt the unattended anita/pexpect
flow can't do. `SET CPU HISTORY` + `EXAMINE` remain a **manual** last resort.

So the observation is done **higher-level**, emitted on `/dev/console` (which any
process can open, and which the boot transcript already captures — a
`fork()+execve()` child's fd 1/2 are not console-wired on NetBSD).

## The instrumentation (diagnostic, gate-private, anti-LARP-clean)

- **(a) resolve + cell fill** — `src/imgact/imgact.c` `bind_imports` (elf32-vax
  `#else` branch), right after the import-cell store, gated by
  `#ifdef OVMX_IMGACT_BIND_TRACE` **and** the running image being CONSUMER
  (basename of `g_argv0`). Emits `OVMX-IMGACT-BIND: prod=<soname> base=0x.. cell=0x.. val=0x..`
  — the resolved producer base, the importing cell VA, and the value written
  (== `LIBVMS$SHR` base + purdy's static `.vms$sv` value). Absent line +
  `%IMGACT-F-GSMATCH` == resolve failed.
- **(c) return value** — CONSUMER already prints `purdy=0x…` on 3 channels and
  returns 0 iff golden (→ `$STATUS`). Unchanged.

The flag is a **default-OFF** CMake option (`src/imgact/CMakeLists.txt`) enabled
**only** by `build-shr-activation-vax.sh`'s gate build — never in a shipped
`IMGACT.EXE`. It prints **addresses only, never the hash**, so it cannot
manufacture a golden pass; `assert_shr_activation`'s value-sensitive predicate is
unchanged and stays the sole PASS gate. `run-shr-activation-vax.sh` surfaces the
bind + value + fault lines as a **non-gating** readout.

## Verdict table (read from the console transcript)

| `OVMX-IMGACT-BIND` | `val` vs `LIBVMS$SHR_base + purdy_off` | fault | `purdy=` | Verdict |
|---|---|---|---|---|
| absent, `%IMGACT-F-GSMATCH` | — | — | — | **resolve-failed** (sv/GSMATCH/index) |
| present | `val` ≠ base+off (or 0) | — | wrong/abort | **cell-fill / .vms$sv value bug** |
| present | `val` == base+off | ACCVIO/SIG* | none | **call mistransferred** (CALLS/linkage) |
| present | `val` == base+off | clean | wrong 64-bit | **wrong-return** (arg/ABI, not resolve) |
| present | `val` == base+off | clean | `0x716cbdc03c071c59` | **PASS** |

`purdy_off` comes from `vax--netbsdelf-readelf` on the shipped `LIBVMS$SHR.EXE`.

## Deferred / corroborating

Printing CONSUMER's own `&purdy_s_hash` was considered but is only corroborating:
whether it yields the resolved entry or a LINKVAX linkage thunk depends on
`src/vmslink/link.c` `.vms$imp` codegen. The load-bearing call-side split comes
from crash-vs-clean-wrong-value once (a) confirms the cell is correct.
