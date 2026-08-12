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
deviation).

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
