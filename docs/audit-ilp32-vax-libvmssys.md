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
