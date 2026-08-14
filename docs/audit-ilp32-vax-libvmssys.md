# ILP32 + Endianness Audit — libvmssys on netbsd-vax (rd vms-9dc, epic vms-8e8)

> **Scope:** the P3 width audit that gates the OVMX/NetBSD VAX userspace port.
> Design context: `docs/design-ovmx-netbsd-syskrnl.md` §4 (VAX as a new width
> class) and `docs/design-netbsd-executive-core.md` (which explicitly defers the
> ILP32/endian audit to this item). This audits `libvmssys` — the freestanding
> syscall layer — and the width-sensitive structs it underpins (VMS descriptors,
> RMS FAB/RAB/NAM/XAB, and the `/dev/vms` ioctl payloads).
>
> **Verdict up front: the audited surface is width-clean for VAX.** No blocking
> ILP32 or endianness defect was found in the netbsd-vax `libvmssys` build set or
> in the descriptor/RMS/ioctl layouts it relies on. Three items are **recorded as
> deferred** — none is in the netbsd-vax build set, each is a runtime (boot-phase,
> P4) or RTL-phase concern, and each has a stated fix. They are listed in §5.

## 1. The width class

VAX is **32-bit, little-endian**, model **ILP32**: `int`=32, `long`=**32**,
pointer=**32**, `long long`=64. Every other OVMX target is **LP64** (`long`=64,
pointer=64): x86_64, aarch64, and the Alpha oracle. So the delta VAX introduces
is **almost entirely width (LP64→ILP32), not byte order**: VAX shares x86_64's
and aarch64's little-endian byte order, so nothing in this audit turns on a
big-endian/little-endian split. The compiler agrees and the build asserts it
(`tools/cross-vax/build-libvmssys-vax.sh`):

```c
_Static_assert(sizeof(long)  == 4, "VAX long must be 32-bit (ILP32)");
_Static_assert(sizeof(void*) == 4, "VAX pointer must be 32-bit (ILP32)");
_Static_assert(sizeof(long long) == 8, "long long 64-bit");
```

The single systemic ILP32 hazard is therefore any place that assumes
`sizeof(long)==8`, `sizeof(void*)==8`, or hardcodes a 64-bit width where VMS
means a **longword** (32-bit). The audit hunted exactly those.

## 2. libvmssys — the netbsd-vax build set

The link-libc VAX backend (design §4.1) compiles a **reduced source set** —
`vms_string.c`, `vms_snprintf.c`, `vms_math.c`, `vms_kif.c`,
`kif_transport_netbsd.c` — selected in `src/libvmssys/CMakeLists.txt` under
`VMSSYS_SUBSTRATE=netbsd`. The freestanding-CRT sources (`vms_runtime_init.c`,
`vms_stdio.c`, `vms_futex.c`) and the Linux syscall ABI (`vms_syscall.h`'s number
tables + asm-trampoline wrappers, and the `struct vms_stat`/`vms_sigaction`
kernel-ABI structs in `vms_types.h`) are **not compiled on VAX** — they are the
raw-freestanding Linux path, and NetBSD's csu/libc supersedes them. That is what
makes most of the classic ILP32 kernel-ABI hazard (a Linux `struct stat` whose
`long` fields resize) simply absent on this path.

| Item (file) | Type / width | ILP32 result | Verdict |
|---|---|---|---|
| `vms_size_t` (`vms_types.h`) | `unsigned long` → 32-bit on VAX | equals `size_t` on ILP32 | **clean** |
| `vms_ssize_t` | `long` → 32-bit | equals `ssize_t` on ILP32 | **clean** |
| `vms_off_t` | `int64_t` (fixed) | 64-bit both LP64 & ILP32; matches NetBSD `off_t` (64-bit) | **clean** |
| `vms_mode_t`/`vms_uid_t`/`vms_gid_t`/`vms_pid_t` | `uint32_t`/`int32_t` (fixed) | stable | **clean** |
| `vms_syscall_netbsd.h` wrappers | pass-through to libc | `(size_t)length`, `(off_t)offset` casts are width-exact on ILP32 | **clean** |
| `vms_math.c` | `#else` → `__builtin_sqrt/floor/ceil` fallback | compiles on VAX (no x86/arm asm) | **clean** (see §5.3 for VAX float) |
| `vms_string.c`, `vms_snprintf.c` | index/len via `vms_size_t`/`int` | no `long`/pointer width assumption | **clean** |

### 2.1 `vms_kif.h` / `vms_kif.c` — the `/dev/vms` ABI carries 64-bit fields on purpose

`vms_kif.h` declares VMS addresses and privilege masks as **`uint64_t`**
(`vms_kif_dclast(uint64_t astadr, …)`, `vms_kif_p0_map(uint64_t base, uint64_t
limit)`, `vms_kif_setprv(uint64_t mask, …)`, etc.). This is **not** an LP64
assumption — it is the deliberately **fixed-width** `/dev/vms` contract the design
mandates (§4.2: "the ioctl payload structs must be fixed-width `uint32_t`/
`uint64_t`, never `long`/pointer"). A 32-bit VAX address travels in the low 32
bits of a `uint64_t`; because VAX and the kernel side are **both little-endian**,
those significant bytes sit at the same offset on both ends — no byte-swap, no
truncation surprise.

The consumer side is ILP32-correct too. `vms_kif.c` narrows an ABI `uint64_t`
address back to a native pointer via `(void *)(unsigned long)base`
(`sys_p*`/`mprotect` paths). On ILP32 `(unsigned long)` is 32-bit, which is
exactly the width of a VAX pointer, so the high half (always zero for a real VAX
address) is dropped correctly. The same cast is width-correct on LP64. **Clean.**

## 3. VMS descriptors (`descrip.h`)

The descriptor is *the* width-sensitive VMS struct, and OVMX gets it right by
using a **real pointer**, not a hardcoded 64-bit field:

```c
struct dsc$descriptor_s {
    uint16_t dsc$w_length;   /* fixed */
    uint8_t  dsc$b_dtype;    /* fixed */
    uint8_t  dsc$b_class;    /* fixed */
    char    *dsc$a_pointer;  /* NATIVE pointer */
};
```

Consequence on VAX (ILP32): the descriptor is `2+1+1+4 = 8` bytes — **exactly the
authentic VAX VMS descriptor**. On LP64 it is 16 bytes (4 bytes prefix + 4 pad +
8-byte pointer). Because `dsc$a_pointer` is `char *`, the struct is the right size
and shape on *every* arch automatically, and on VAX it collapses to the genuine
8-byte layout with no special-casing. All metadata fields are fixed-width
(`uint16_t`/`uint8_t`/`uint32_t`/`int32_t` incl. the array-descriptor bounds
`dsc$l_m*`/`dsc$l_l*`/`dsc$l_u*`). **Clean.** The only latent risk the design
flagged — "code assuming 64-bit pointers *inside* a descriptor payload" — is a
consumer-code concern, not a struct-layout one; nothing in the netbsd-vax build
set makes that assumption.

## 4. RMS (`fab.h`, `rab.h`, `nam.h`, `xab.h`) and the ioctl payloads

- **RMS control blocks**: a full scan for bare `long`/`unsigned long`/`uint64_t`/
  `int64_t` across FAB/RAB/NAM/XAB found **none**. Address fields use native
  pointer types — `char *fab$l_fna`, `char *fab$l_dna`, `struct NAM *fab$l_nam`,
  `struct XABKEY *fab$l_xab` — which are 4-byte **longwords on VAX** (authentic:
  the `$l_` prefix means longword) and 8 bytes on LP64. Everything else is
  fixed-width `uint8_t`/`uint16_t`/`uint32_t`. **Clean.**
  - *Pre-existing, arch-independent note (not a VAX finding):* `fab$b_bln =
    sizeof(struct FAB)` stores a struct size into a `uint8_t`, and `struct FAB`
    carries OVMX-internal extension fields (`_resolved_path[1024]`), so `bln`
    truncates on **all** arches, not just VAX. Out of scope here; flagged only so
    a later reader does not misattribute it to ILP32.

- **`/dev/vms` ioctl payloads (`src/kernel/vms_ioctl.h`)**: a scan for `long`/
  pointer fields *inside* the `struct vms_*_args` payloads found none — they are
  fixed-width, satisfying the design §4.2 requirement that a 32-bit VAX userspace
  and the (32-bit VAX) kernel agree byte-for-byte. **Clean.**

## 5. Recorded / deferred items (none block the netbsd-vax build)

None of the three is in the netbsd-vax `libvmssys` build set; each is a
later-phase concern with a stated fix, recorded here per Rule 5 (no silent spec
deviation). **All three are now RESOLVED in the RTL phase — see §7 (rd
vms-30a).**

### 5.1 `vms_time_t = long` mismatches NetBSD's 64-bit `time_t`
`vms_types.h` defines `typedef long vms_time_t;` → **32-bit on VAX ILP32**, but
**NetBSD `time_t` is 64-bit on all ports** (including vax). Used only by
`struct vms_timespec`/`vms_timeval` on the freestanding path (buffered-I/O and
futex timeouts in `vms_stdio.c`/`vms_futex.c`), which are **excluded** from the
netbsd build set — so it cannot currently reach a NetBSD time syscall. **Fix
before any netbsd time path is added (P4):** change `vms_time_t` to `int64_t`.

### 5.2 VMS_O_* / VMS_MAP_* / futex constant namespaces are Linux-numeric
The `VMS_O_*`, `VMS_MAP_*`, and `VMS_FUTEX_*` constants in `vms_types.h` hold
**Linux** numeric values (e.g. `VMS_O_CREAT=0x40`; NetBSD's `O_CREAT=0x200`;
`VMS_MAP_ANONYMOUS=0x20` vs NetBSD `MAP_ANON=0x1000`). On the link-libc path these
would be passed to NetBSD libc and mean the wrong thing. This is why
`vms_syscall_netbsd.h` deliberately provides only the **flag-clean** wrappers
(`getpid`/`mprotect`/`mmap`/`munmap`/POSIX `read`/`write`/`close`/`lseek`, whose
`PROT_*` values are identical across Linux and NetBSD) and **omits** `openat`
and `futex`. Neither omitted wrapper is used by the netbsd build set. **Fix when
those facilities are ported (P4):** resolve these constants per-substrate (a
NetBSD-valued block in `vms_types.h` under `__NetBSD__`) or translate in the
wrapper.

### 5.3 VAX floating point is not IEEE-754
GCC/vax defaults to VAX F/D/G float formats, not IEEE-754. `vms_math.c` compiles
via `__builtin_*` fallbacks, so this does not block the build, but any IEEE
bit-pattern assumption in the `MTH$`/`OTS$` float RTL is a latent runtime bug on
VAX. Matches design §4.2's note; **out of scope for the build/audit milestone**,
recorded for the RTL phase.

## 6. How this was validated

`tools/cross-vax/Dockerfile` builds a pinned `vax--netbsdelf` GCC cross toolchain
(binutils 2.42, gcc 13.3.0) over a SHA512-pinned NetBSD 10.1/vax sysroot
(headers + libc + crt objects). `tools/cross-vax/build-libvmssys-vax.sh` then
(1) asserts the ILP32 widths above, (2) builds `libvmssys.a` for netbsd-vax
through the CMake `VMSSYS_SUBSTRATE=netbsd` path, and (3) links a real
`vax--netbsdelf` ELF executable that calls into `libvmssys` against NetBSD libc —
proving the link-libc backend end to end. There is **no** emulation: QEMU has no
VAX system target and this is a build-only gate (SIMH boot is a later phase, P4).
The CI job `libvmssys-netbsd-vax` runs the whole thing containerized.

## 7. RTL-phase resolution of §5's three items (rd vms-30a)

The three §5 items were the width/float concerns the P3 audit deferred to the
RTL phase. This section records their resolution. Each is either **fixed
portably** or (where it genuinely needs a booted NetBSD/vax to validate) fixed
in-principle with the runtime check flagged as a **P4 (SIMH boot)** follow-up.
The netbsd-vax cross gate (`libvmssys-netbsd-vax`) compiles the changed
`libvmssys` surface — including every `#if defined(__NetBSD__)` / non-IEEE branch
below — so all of this is **compile-proven on the real VAX toolchain**; the
Linux x86_64/aarch64 build + `ctest` prove the IEEE path is unregressed.

### 7.1 `vms_time_t` — FIXED
`src/libvmssys/vms_types.h`: `typedef long vms_time_t;` → `typedef int64_t
vms_time_t;`. On LP64 this is a no-op (`long`==64-bit), so `struct
vms_timespec`/`vms_timeval` keep an identical layout on x86_64/aarch64; on VAX
ILP32 it widens 32→64, matching NetBSD's 64-bit `time_t` and killing the Y2038 /
truncation defect. A `_Static_assert(sizeof(vms_time_t) >= 8, …)` guards it and
is compiled by BOTH the LP64 targets and the ILP32 vax gate — so the fix is
proven where a bare `long` would fail (VAX). No silent truncation remains: a
full-tree grep shows `vms_time_t`'s only uses are the two typedef'd time structs.

### 7.2 `VMS_O_*` / `VMS_MAP_*` / futex constants — FIXED (substrate-selected)
`src/libvmssys/vms_types.h`: the file-flag constants are now selected per
substrate. Under `#if defined(__NetBSD__)` the header `#include`s the sysroot
`<fcntl.h>`/`<sys/mman.h>` and **aliases** `VMS_O_*`/`VMS_AT_*`/`VMS_MAP_*` to the
authoritative NetBSD macros (`VMS_O_CREAT → O_CREAT`, `VMS_MAP_ANONYMOUS →
MAP_ANON`, …) — never a transcribed magic number, so it is correct by
construction and can never drift from the platform. The Linux raw-syscall numeric
values are kept under the `#else`. `_Static_assert(VMS_O_CREAT == O_CREAT, …)`
(compiled by the vax gate against the real NetBSD sysroot) proves the select
took the NetBSD value, not a stale Linux `0x40`. The Linux-numeric `VMS_FUTEX_*`
op block is now `#if !defined(__NetBSD__)` — deliberately **undefined** on
NetBSD (no Linux futex ABI there; the netbsd wait primitive is separate, design
§4.2, and `vms_futex.c` is excluded from the build set), so a Linux futex op
number can never reach a NetBSD syscall. Audit evidence: the only consumers of
these constants — `vms_stdio.c`, `vms_futex.c`, `vms_runtime_init.c`,
`kif_transport_linux.c` — are all excluded from the netbsd build set, so nothing
in-set regressed; the fix makes the constants correct for when a NetBSD
open/mmap-flags path *is* added.

### 7.3 VAX F/D/G float (non-IEEE-754) — FIXED in `libvmssys`, in-principle in `libvms` (P4 runtime)
The IEEE-only bit tricks were split by target on `#if defined(__vax__)` (VAX is
the only non-IEEE float target; note `__STDC_IEC_559__` is unusable here because
`-ffreestanding` leaves it undefined even on x86_64):

- **`src/libvmssys/vms_math.c`** (IN the netbsd-vax build set): the hardcoded
  IEEE inf/NaN words (`0x7FF0…`/`0xFFF0…`/`0x7FF8…`) are replaced by
  `vms_math_inf()`/`vms_math_nan()` helpers — bit words on IEEE, `__builtin_huge_val()`
  / `__builtin_nan("")` on VAX (the target-correct value; VAX has no true
  Inf/NaN, so gcc/vax lowers these to its reserved-operand / max-magnitude
  forms — the VMS-authentic outcome). The exponent surgery in `vms_exp()` (the
  `p * 2^n` reconstruction) and `vms_log()` (mantissa/exponent extraction) keeps
  the exact IEEE bit path on IEEE targets — load-bearing because the
  freestanding Linux build is `-ffreestanding -fno-builtin` and must not emit
  libm calls — and on VAX uses `__builtin_ldexp`/`__builtin_frexp`, which the
  compiler lowers to native VAX-float scaling (NetBSD libm backs them on the
  link-libc substrate; the archive tolerates the unresolved symbol and the gate's
  smoke exe does not pull `vms_math.o`). Bounded residual: `vms_exp`'s 709/-745
  overflow *clamp* constants are IEEE thresholds — exact for VAX G_float, loose
  for D_float; because `ldexp` itself signals correctly on VAX overflow this
  affects only extreme-input overflow *signalling*, not normal-range results, and
  precise VAX thresholds are a **P4** runtime item.

- **`src/libvms/rtl/lib_timer.c`** (`lib$wait`, DECC$SHR LIB$; NOT yet in a
  cross-built set — libvms cross-build is the C2 item this work unblocks): its
  VAX F/D/G decoders were already host-independent arithmetic (correct
  everywhere). Its `LIB$K_IEEE_S`/`_IEEE_T` cases, however, `memcpy`'d the IEEE
  bytes into a *native* `float`/`double`, which misreads them on a VAX (non-IEEE
  native) host. Fixed symmetrically: IEEE hosts keep the exact `memcpy`; non-IEEE
  hosts decode the IEEE bit fields **arithmetically** (`ieee_s_to_double` /
  `ieee_t_to_double`), the mirror of how the VAX decoders parse VAX bytes on an
  IEEE host.

- **`src/libvms/rtl/mth_routines.c`, `ots_routines.c`**: audited **clean** —
  `MTH$` forwards to libm on native `double` (no bit assumptions; libm handles
  native VAX float), and `OTS$` conversions are integer/text only.

Static-analysis and runtime coverage: `tests/libvmssys/test_math.c` gains
exp/log/pow range checks, an `exp(log(x))` round-trip, and inf/-inf/NaN sentinel
assertions (run on the IEEE host by Linux `ctest`, pinning the split against
regression). Because there is **no** VAX system emulator in CI, a real VAX-float
`printf`/`strtod` round-trip must be validated on booted NetBSD/vax under SIMH —
that is the **P4** follow-up; the static audit + fixes above are complete and
the non-IEEE branches are compile-proven by the vax cross gate.

## 8. vmsprocess — the next layer up (rd vms-84b, epic vms-8e8; C-track)

The library graph continues `libvmssys → vmsprocess (+pthread) → libvms`
(CLAUDE.md "Library Build Order"). This section extends the width/portability
audit to **vmsprocess** — process control blocks (PCB), ASTs, access modes, and
privileges — now that it cross-compiles + links for netbsd-vax. Build set:
`vms_pcb.c`, `vms_process.c`, `ast.c`, `access_modes.c` (headers under
`src/vmsprocess/include/vms/` plus `ssdef.h`/`prvdef.h` from `src/libvms/include`).

**Verdict: width-clean, and no Linux-only wait primitive.** vmsprocess needed
**no source change** to cross-compile and link for netbsd-vax — only CMake
standalone wiring + a build/link gate. It is a link-libc consumer: NetBSD
libc/libpthread supply pthread, signals, and stdio; no OVMX freestanding CRT and
no raw syscall ABI are pulled on this path.

### 8.1 The wait-primitive question (the anti-stall concern) — no futex dependency
The RTL audit made Linux `futex` deliberately undefined on NetBSD (§7.2), so the
key question for vmsprocess was whether its process-control waits reach a
Linux-only primitive. They do **not**:

- **AST delivery uses POSIX signals, not futex.** `ast.c` arms `SIGUSR1` via
  `sigaction`/`SA_RESTART` and triggers delivery with `raise(SIGUSR1)`; the
  handler sets a `volatile int` in the PCB. All of that is POSIX and present on
  NetBSD/vax — no futex, no `vms_futex.c` (which is excluded from the netbsd
  build set anyway).
- **Event flags are NOT in vmsprocess.** They were removed from the PCB (vms-2a8,
  CLAUDE.md Rule 11) and live in the executive (`src/kernel/vms_eflag.c`, reached
  through `/dev/vms`). So the classic "event-flag wait needs a futex" hazard is
  structurally absent here — there is no wait primitive in vmsprocess to port.
- **io_uring is declared but never called.** The PCB carries `uring_*` fields
  (set to `-1`/`NULL` in `vms_pcb_init`) and a **weak** `extern vms_uring_cleanup`;
  no `<liburing.h>`, no io_uring syscall. The weak symbol resolves to NULL when
  unprovided, so nothing Linux-specific is linked. Clean.

Net: vmsprocess has no hard dependency on a Linux-only wait primitive, so the
anti-stall STOP condition did not fire.

### 8.2 ILP32 width scan — clean
| Item (file) | Type / width | ILP32 result | Verdict |
|---|---|---|---|
| `vms_process_t.linux_pid` (`process.h`) | `pid_t` (NetBSD `int32_t`) | 32-bit both models | **clean** |
| identity/UIC fields (`vms_pid`, `uic`, priorities, flags) | `uint32_t` (fixed) | longword-exact, matches VMS `[group,member]` packing | **clean** |
| PCB privilege masks (`cur_privs`/`perm_privs`) | `uint64_t` (fixed) | 64-bit both models | **clean** |
| `PRV$M_*` (`prvdef.h`) | `((uint64_t)1 << N)`, N up to 38 | cast-to-64 **before** shift → no ILP32 `1<<32` UB | **clean** |
| PCB quotas | `uint32_t[]` (fixed) | stable | **clean** |
| AST `param`/`acmode` (`ast.h`) | `uint32_t`/`uint8_t` (fixed) | stable | **clean** |
| `__thread current_pcb`, `__atomic_compare_exchange_n` | GCC TLS/atomics | supported by gcc-13.3/vax (libgcc emutls if needed) | **clean** |

No bare `long`/pointer-width field, no `sizeof(long)==8` assumption, and the one
systemic ILP32 trap for bitmasks (`1 << N` with N≥31 on a 32-bit `int`) is
avoided in `prvdef.h` by the `(uint64_t)1` cast. VAX and every other target share
little-endian byte order, so nothing here turns on byte order.

### 8.3 Non-blocking note (not a VAX finding)
`vms_process.c` draws two `-Wstringop-truncation` warnings (`strncpy` into
`username[32]`/`prcnam[16]`) — the intentional VMS fixed-field truncation pattern,
identical on every arch. The netbsd branch compiles `-Wall -Wextra` **without**
`-Werror`, so it is non-fatal; recorded so a later reader does not misread it as
an ILP32 defect.

### 8.4 How this was validated
`tools/cross-vax/build-vmsprocess-vax.sh` (CI job `vmsprocess-netbsd-vax`)
(0) builds the elf32-vax `libvmssys.a`, (1) builds `libvmsprocess.a` through the
CMake `VMSPROCESS_STANDALONE` path (asserted `file format elf32-vax`), and
(2) links a real `vax--netbsdelf` ELF32 executable that references one symbol from
each of the four translation units, against NetBSD libc + libpthread **and** the
elf32-vax `libvmssys.a` — proving the process-control layer sits on top of
libvmssys end to end. No emulation (SIMH boot is the separate P4 job). The Linux
`Build & Test` + `ctest` (`vmsprocess_unit`) prove the in-tree
Linux/aarch64/x86_64 build is unregressed.

## 9. libvms — the VMS runtime (rd vms-1b2, epic vms-8e8; C-track)

The library graph continues `vmsprocess → libvms (+pthread, m)` (CLAUDE.md
"Library Build Order"). This section extends the width/portability audit to
**libvms** — the biggest userspace library: system services (`sys$*`) plus the
full RTL (`lib$`/`str$`/`mth$`/`ots$`) — now that it cross-compiles + links for
netbsd-vax. Build set: the whole `src/libvms/CMakeLists.txt` source list (60
translation units; every `syssvc/*.c` and `rtl/*.c`), headers from
`src/libvms/include` plus the peer include dirs for RMS/FS/LNM/process, and
`vms_kif.h` (libvmssys), the vmslink symbol-vector headers, and the vmsscs
membership header as header-only compile deps.

**Verdict: the ENTIRE libvms cross-compiles and links for netbsd-vax.** No
subsystem had to be extracted or excluded. Two portability defects surfaced — a
Linux-only I/O accelerator and a Linux-only mmap flag — both fixed with the
substrate-selected discipline (§7). The ILP32 width surface is clean: libvms's
public structs already use fixed-width types (`uint32_t` longwords, `uint64_t`
quadwords, VMS descriptors carry an explicit `uint16_t` length + pointer), so no
bare `long`/pointer-width assumption or `sizeof(long)==8` reached the VAX build.

### 9.1 `sys_uring.c` — io_uring is Linux-only (FIXED, substrate-guarded)
`syssvc/sys_uring.c` is a **QIO accelerator**, not the QIO facility: it wraps
Linux `io_uring` (`<linux/io_uring.h>`, `syscall(__NR_io_uring_setup/enter)`,
`IORING_*` mmap offsets) — none of which exist on NetBSD. `sys$qio`/`sys$qiow`
(`sys_qio.c`) already fall back to a **real synchronous `read()`/`write()` path**
(`qio_sync`) whenever `uring_available()` reports 0 — the case on any pre-5.1
Linux kernel and on every non-Linux substrate. Fix: the whole io_uring
implementation is guarded by `#if defined(__linux__)`; on other substrates the
five entry points (`vms_uring_init/submit_rw/process_completions/
wait_completion/cleanup`) compile to **"unavailable" stubs** (`init`→-1), so
`uring_available()` returns 0 and the synchronous path is taken. This is a
substrate selection, **not** a LARP fallback (Rule 9 / INV-6): the QIO facility
itself is fully implemented by the synchronous path; only the accelerator is
absent, exactly as on an old Linux kernel. The gate asserts (`nm`) that
`sys_uring.o` carries no Linux io_uring syscall on the VAX build.

### 9.2 `sys_memory.c` — `MAP_FIXED_NOREPLACE` is Linux-only (FIXED, substrate-selected)
`sys$cretva` maps at a requested address **non-destructively** (it must never
clobber an existing mapping; the fallback re-maps at any address). Linux
expresses that atomically with `MAP_FIXED_NOREPLACE` (4.17+); NetBSD has no such
flag. Fix: a `VMS_CRETVA_FIXED_FLAG` macro keyed on **the platform macro's own
presence** (`#if defined(MAP_FIXED_NOREPLACE)`) — never a transcribed number, so
correct by construction (the §7.2 discipline). On Linux it resolves to
`MAP_FIXED_NOREPLACE` (behavior byte-identical); on NetBSD it resolves to `0`, so
the requested address is passed as a plain **hint** (no `MAP_FIXED`), which the
kernel honors when the range is free and otherwise places elsewhere — the same
non-destructive "try the requested range, else any address" contract, with the
actual placement reported in `retadr[]`. (`MAP_ANONYMOUS` needed no change:
NetBSD 10 aliases it to `MAP_ANON`, and it compiles on the VAX sysroot.)

### 9.3 ILP32 width + IEEE-float scan — clean
- **Descriptors / longwords / quadwords.** libvms's system-service and RTL APIs
  pass VMS descriptors (`struct dsc$descriptor`: `uint16_t` length, `uint8_t`
  class/dtype, pointer) and fixed-width status/argument longwords (`uint32_t`)
  and quadwords (`uint64_t`) — no bare `long` or pointer-width field crosses an
  API boundary, so LP64→ILP32 does not change a single struct layout.
- **Float RTL (`mth_routines.c`, `ots_routines.c`).** Confirmed the §7.3 finding:
  `MTH$` forwards to libm on native `double` (no IEEE bit assumptions; NetBSD
  libm backs it on the link-libc substrate) and `OTS$` conversions are
  integer/text only. No unguarded IEEE bit-punning reached the VAX build; the one
  IEEE-decode path that needed a non-IEEE branch (`lib_timer.c`'s
  `LIB$K_IEEE_S/_T`) was already fixed in §7.3. No new `#if __vax__` float split
  was required in libvms.
- **`-Wstringop-truncation` noise.** The same intentional VMS fixed-field
  `strncpy` truncation pattern flagged in §8.3 recurs across libvms
  (`sys_assign.c`, `sys_misc.c`, `ovmx_accounting.c`, the `sysgen_params.h` /
  `scs_membership.h` headers). All are identical on every arch and non-fatal
  (the netbsd branch is `-Wall -Wextra` without `-Werror`); recorded so a later
  reader does not misread them as ILP32 defects.
- **`sys_imgact.c` `-Wint-to-pointer-cast` (6×) — EXPECTED, dead on vax, NOT a
  defect.** `sys_imgact.c` is the OVMX-native in-process image activator
  (`SYS$IMGACT` as a library, the Linux / self-hosting-pillar path). It hardcodes
  the 64-bit ELF structures (`Elf64_Ehdr/Phdr/Dyn/Rela`) and casts an
  `unsigned long base` + 64-bit `p_vaddr`/`r_offset` to pointers, so on ILP32 the
  vax compiler emits "cast to pointer from integer of different size" for those
  casts. This is **not** an ILP32 defect and **cannot** execute on vax: under
  Decision A (rd vms-42d) OVMX images on netbsd-vax are activated by NetBSD
  `/usr/libexec/ld.elf_so`, never by this activator. `imgact_activate()` gates on
  `eh.e_ident[EI_CLASS] != ELFCLASS64` (→ `SS$_BADPARAM`) and `IMGACT_EM == 0`
  (→ `SS$_UNSUPPORTED`) *before* any of the flagged casts is reached; every vax
  image is ELFCLASS32, so it always declines and DCL's `RUN` path
  (`dcl_activate_image`) falls through to its `fork()+execve()` model — the
  correct netbsd-vax activation. The warned-on code is therefore unreachable on
  vax. It is *not* substrate-guarded the way `sys_uring.c`/`sys_memory.c` are
  (§9.1/§9.2); guarding it to a `#if !defined(__linux__)` stub (to make the
  netbsd-vax libvms compile warning-clean, matching that pattern) is filed as a
  follow-up cleanliness item — it touches the activation / self-hosting pillar
  surface, changes no runtime behavior on any substrate, and is not required for
  the boot (the images already build, link, and activate).

### 9.4 How this was validated
`tools/cross-vax/build-libvms-vax.sh` (CI job `libvms-netbsd-vax`)
(0) builds the elf32-vax `libvmssys.a`, (0b) the elf32-vax `libvmsprocess.a`,
(1) builds `libvms.a` through the CMake `LIBVMS_STANDALONE` path (asserted
`file format elf32-vax`, and `nm`-asserted that `sys_uring.o` is the non-Linux
stub), and (2) links a real `vax--netbsdelf` ELF32 executable that calls into
libvms's system-service + RTL surface (status decoding, the arithmetic RTL)
against NetBSD libc + libpthread + libm **and** the elf32-vax `libvmsprocess.a` +
`libvmssys.a` — proving the VMS runtime sits on top of the whole elf32-vax stack
end to end. No emulation (SIMH boot is the separate P4 job). The Linux `Build &
Test` + `ctest` prove the in-tree Linux/aarch64/x86_64 build is unregressed (the
two fixes are byte-identical on Linux: the io_uring path is unchanged inside its
`__linux__` guard, and `VMS_CRETVA_FIXED_FLAG` resolves to `MAP_FIXED_NOREPLACE`
there).

## 10. vmslnm + vmsfs (userspace) — the logical-name and filespec layers (rd vms-271, epic vms-8e8; C-track)

`vmslnm` (Logical Name Manager) and the **userspace** `vmsfs` (filespec/device/
version/protection — NOT the kernel ODS-2 core of §/vms-bb8, and NOT the genuine
ODS-2 volume codec `src/vmsfs/ods2`) are the two layers directly above libvms in
the library graph:
`... libvms -> vmslnm (+pthread) -> vmsfs -> vmsrms -> vmsdcl`.
Both cross-compile to `elf32-vax` through new `*_STANDALONE` CMake branches
(mirroring §8/§9's `vmsprocess`/`libvms` pattern) and link real `vax--netbsdelf`
ELF32 executables on top of the elf32-vax stack.

### 10.1 `PTHREAD_ONCE_INIT` is not a portable *assignment* — FIXED

`src/vmslnm/lnm_client.c`'s `lnm_shutdown()` reset the global manager guard with
`g_manager_once = PTHREAD_ONCE_INIT;`. This compiled on Linux only because glibc
defines `PTHREAD_ONCE_INIT` as the scalar `0`. NetBSD (all arches, not VAX-
specific) defines it as a **braced aggregate** `{ ... }`, which C permits only in
a *declaration initializer*, never in an assignment — so the cross build failed
with `expected expression before '{' token`. This was a latent
Linux/glibc portability assumption, not a width bug.

**Fix:** copy from a real const object initialized with the macro —
`static const pthread_once_t g_manager_once_init = PTHREAD_ONCE_INIT;` and
`g_manager_once = g_manager_once_init;`. Legal on both libcs (scalar copy on
glibc, struct copy on NetBSD) and byte-identical behaviour on Linux.

### 10.2 ILP32 / endianness / IEEE-float scan — clean

No `long`-as-64-bit, no pointer-width packing, no IEEE bit-punning, no Linux-only
header/syscall in either library. Logical-name and filespec strings are byte
buffers; the LNM equivalence-name lengths and attributes are `uint32_t`. `vmsfs`
version scanning uses `dirent`/`fnmatch` (POSIX, NetBSD-provided). The
`-Wstringop-truncation` fixed-field `strncpy` noise of §8.3/§9.3 recurs
(`lnm_translate.c`) and is non-fatal for the same reason.

### 10.3 Executive LNM-arena 8-byte atomic forces libatomic — FIXED in the toolchain

The link proofs exercise `vmsfs_to_linux_path -> lnm_translate ->
vms_kif_lnm_translate`, whose seqlock read `lnm_gen_load()` in libvmssys
`vms_kif.c` does `__atomic_load_n(&arena->generation, __ATOMIC_ACQUIRE)` on the
`uint64_t generation` field of the shared LNM arena (`src/kernel/vms_lnm.h`).
**VAX is 32-bit with no lock-free 8-byte atomic**, so GCC lowers that to a
`__atomic_load_8` libatomic call. The cross toolchain built GCC with
`--disable-libatomic`, so the symbol was an undefined reference — this had been
latent because the §9 libvms proof never pulled the LNM-arena path. It blocks the
link proof of **every** layer above libvmssys that touches the arena
(vmslnm/vmsfs/vmsrms, and next vmsdcl).

**Fix (tools/cross-vax/Dockerfile, vms-271):** build + install `libatomic` for
`vax--netbsdelf` (`--enable-libatomic`, `all-target-libatomic`), and pass
`-latomic` on the userspace-library link proofs (GCC does not auto-link it). The
arena's `generation` is a kernel↔userspace ABI field and stays 64-bit;
libatomic's lock-based 8-byte load is a correct atomic on VAX. Narrowing the field to 32-bit
was rejected: it would change the arena ABI (the Linux executive writes it) and
weaken the seqlock's atomicity — an executive-ABI decision outside this port.

## 11. vmsrms — Record Management Services (rd vms-271, epic vms-8e8; C-track)

`vmsrms` is the top userspace library before DCL. It cross-compiles to
`elf32-vax` through a `VMSRMS_STANDALONE` CMake branch and links a real
`vax--netbsdelf` ELF32 executable over the **whole** elf32-vax stack (libvms +
vmsfs + vmslnm + vmsprocess + libvmssys; `--start-group` resolves the
libvms↔vmsfs archive cycle).

### 11.1 RMS file I/O is ordinary POSIX — no Linux syscall/constant assumption

The specific concern flagged in §4 (RMS + the ioctl payloads) was checked at the
source level: `rms_seq.c`/`rms_rel.c` reach files through `open`/`lseek`/
`ftruncate` with `off_t` and the `O_*`/`SEEK_*` constants from the **NetBSD
sysroot's** `<fcntl.h>`/`<unistd.h>` — never a Linux syscall number and never a
hard-coded Linux-numeric `O_*` value (contrast the `VMS_O_*` remap the RTL needed
in §7.2; RMS uses the platform `<fcntl.h>` directly, so it is already substrate-
correct). `dirent`/`fnmatch` back wildcard search. All POSIX, all NetBSD-
provided.

### 11.2 On-disk record widths + the `off_t` question — clean

RMS record prefixes and VBN/bucket math are byte counts and fixed-width offsets
(`uint16_t`/`uint32_t`), ILP32-clean under the 32-bit VAX compiler (compiled
`-Wall -Wextra`, no width warnings). The RAB's in-memory `off_t _current_offset`
/ `_last_rec_offset` are **host file positions**, not on-disk fields, so NetBSD's
64-bit LFS `off_t` is fine (a 64-bit file cursor on a 32-bit host is expected).
The b-tree node's `off_t offsets[]` in `rms_idx.c` are likewise in-memory data-
file positions. Note (not a cross-*compile* concern): an RMS index/data file
**written by a 64-bit host and read by a 32-bit VAX** — or vice-versa — is a
future cross-*platform file-format* portability question for the SIMH/boot phase,
not this build-only gate; recorded here so it is not forgotten.

### 11.3 How §10/§11 were validated

`tools/cross-vax/build-{vmslnm,vmsfs,vmsrms}-vax.sh` (CI job
`vmslnm-vmsfs-vmsrms-netbsd-vax`, "netbsd-vax userspace libs cross-compile"):
each builds its elf32-vax dependency stack, builds the library through its CMake
`*_STANDALONE` path (asserted `file format elf32-vax`), and links a real
`vax--netbsdelf` ELF32 executable calling into the library (`Machine: Digital
VAX` asserted) against NetBSD libc/libpthread(/libm for RMS). The Linux `Build &
Test` + `ctest` prove the in-tree Linux/aarch64/x86_64 build is unregressed — the
`PTHREAD_ONCE_INIT` fix is byte-identical on glibc, and every standalone branch
is guarded by `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR` so the in-tree
image builds are untouched. This unblocks C4 (vmsdcl cross-build), which hits the
same executive-arena libatomic wall resolved in §10.3.

## 12. vmsdcl — the DCL command interpreter (rd vms-1cb2, epic vms-8e8; C-track)

vmsdcl is the TOP of the userspace library graph and the LAST userspace library
before the init+images integration (`... vmsfs -> vmsrms -> vmsdcl`, plus the
leaf `vmsqueue`). DCL is overwhelmingly string/parse/file work over ordinary
POSIX; the cross-build surfaced no new ILP32 *width* bug, but it did surface a
namespace issue and a genuine Linux-only facility. Both are resolved; the in-tree
Linux build is byte-identical.

### 12.1 Integer symbols are longwords — ILP32 `long` is the exact VMS width

DCL arithmetic symbols are held in `expr_val_t.ival`, typed `long` and formatted
`%ld` throughout (`dcl_exec.c`, `dcl_lexical.c` FAO `!SL`/`!UL`). On a 64-bit host
`long` is 64-bit — WIDER than a VMS longword, so `!SL`/`!UL` need masking to stay
faithful. On **ILP32 VAX `long` is 32-bit == the VMS longword**, so the DCL
integer model is *more* natural there, not less. Self-consistent (cast-to-`long`
printed as `%ld`); no width fix needed. The `.OLB`/LBR container math
(`dcl_library.c`) is fixed-width byte offsets with `uint64_t` decimal fields
(`olb__dec`), ILP32-clean.

### 12.2 `_POSIX_C_SOURCE` hides the BSD surface on NetBSD — FIXED (substrate-selected)

The in-tree build defines `_POSIX_C_SOURCE=200809L` + `_DEFAULT_SOURCE`; on
glibc/musl `_DEFAULT_SOURCE` re-exposes the BSD extensions DCL uses
(`FNM_CASEFOLD`, `flock`/`LOCK_EX`/`LOCK_UN`, `struct ifreq`, `IFNAMSIZ`,
`IFF_UP`, the `SIOC*` ioctls). On NetBSD that same surface is gated behind
`_NETBSD_SOURCE`, and defining `_POSIX_C_SOURCE` *turns it off* — so those tokens
vanished under the cross compiler. Fixed in `src/vmsdcl/CMakeLists.txt` by
substrate-selecting the feature macros: `if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")`
defines `_NETBSD_SOURCE` (the native widest namespace, a POSIX superset), else
the Linux `_POSIX_C_SOURCE`/`_DEFAULT_SOURCE` pair — unchanged for Linux/aarch64/
x86_64. The two vestigial `#include <mntent.h>` lines (glibc-only, absent from
the NetBSD sysroot; the mount table is parsed by hand from `/proc/mounts`) were
removed — no `mntent` symbol was referenced.

### 12.3 TCPIP interface config is Linux-substrate — substrate-guarded (honest degrade)

`ifr_hwaddr`/`ifr_netmask` (Linux `struct ifreq` union members), `SIOCGIFHWADDR`,
and `/sys/class/net` enumeration in `dcl_cmd_misc.c`'s `TCPIP SHOW/SET INTERFACE`
are genuinely Linux-only — NetBSD reads the link-layer address via
`getifaddrs`/`AF_LINK` and has no `ifr_netmask` member. This is the DCL face of
the **AF_INET networking engine** (rd vms-67f), whose engine is deliberately the
Linux substrate. The Linux ioctl bodies are guarded `#if defined(__linux__)`; on
netbsd-vax the netmask readout shows `*` and the `/FULL` hardware-address line is
omitted — **honest degradation, nothing fabricated** (INV-6). Linux/musl keep the
`__linux__` path verbatim. A future NetBSD-substrate networking backend (the
`getifaddrs`/`AF_LINK` path) is the precise follow-up, out of scope for the
build-only gate.

### 12.4 `ovmx_tlsdesc_static` had no non-primary-arch definition — FIXED

Pulling `libvms`'s `sys_imgact.o` into the full DCL link exposed that the resident
TLSDESC resolver `ovmx_tlsdesc_static` (`src/libvms/syssvc/sys_imgact.c`) had asm
bodies only for `__x86_64__`/`__aarch64__` — on VAX the symbol was `.globl`'d and
its address taken but never defined (hidden-symbol link error). The own-PT_TLS
in-process activation path is x86_64/aarch64-only (`IMGACT_EM==0` elsewhere, and
the stack-switch jump is already a no-op `#else`); on netbsd-vax activation is
delegated to `ld.elf_so` (gate `activation-netbsd-vax`), so the descriptor cells
that point here are never built and the resolver is never called. Fixed by adding
an `#else` branch: a defined, hidden C stub that `__builtin_trap()`s if ever
reached — the same "must not happen" contract as the existing
`__builtin_unreachable()`. Strict no-op for the two primary arches (the
`#if defined(__x86_64__) || defined(__aarch64__)` branch is byte-identical to
before), so no Linux/aarch64 regression; `imgact`/`libvms` ctest stays green.

### 12.5 How §12 was validated

`tools/cross-vax/build-vmsdcl-vax.sh` (CI job `vmsdcl-netbsd-vax`, "vmsdcl
cross-compiles for netbsd-vax"): builds the whole elf32-vax stack (libvmssys,
vmsprocess, libvms, vmslnm, vmsfs, vmsrms, vmsqueue), compiles every DCL TU
through the CMake `VMSDCL_STANDALONE` path to `libvmsdcl.a` (asserted `file format
elf32-vax`), asserts no GNU-readline symbol leaked in (readline is optional and
absent from the sysroot → DCL's own non-readline line editor), then links a real
`vax--netbsdelf` **DCL.EXE** with `-Wl,--whole-archive` on `libvmsdcl.a` — every
DCL object must resolve, a complete shell — against the stack + NetBSD libc +
libpthread + libm + libatomic (`Machine: Digital VAX`, `Class: ELF32` asserted).
There is no cross-only smoke `.c`: the proof links the actual shell. The in-tree
Linux `Build & Test` + DCL/imgact/libvms `ctest` confirm no regression — the
feature-macro select, the `__linux__` guards, and the `sys_imgact.c` `#else` are
all no-ops on Linux/aarch64/x86_64, and the `VMSDCL_STANDALONE`/`VMSQUEUE_STANDALONE`
branches are guarded by `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR`.
This unblocks the init+images integration (rd vms-5d1): STARTUP.EXE/ovmx_init and
the LOGINOUT/DCL.EXE link+activate for netbsd-vax, then the bootable-disk assembly.
