# ILP32 + Endianness Audit — the ODS-2 core (vmsfs) on elf32-vax (rd vms-bb8, epic vms-8e8)

> **Scope:** the P4-VFS **V3** width audit — the VAX-width step of the ODS-2
> portability tributary. It audits the substrate-neutral ODS-2 core
> (`src/kernel-core/vmsfs/*.c` + the on-disk format `vmsfs_ondisk.h`) for the
> **VAX (32-bit, little-endian, ILP32)** width class, the same class the
> libvmssys audit (`docs/audit-ilp32-vax-libvmssys.md`) established. It is the
> ODS-2 analog of that item: where libvmssys proved the freestanding syscall
> layer cross-compiles for netbsd-vax, this proves the ODS-2 **kernel filesystem
> core** does. It does **not** build the NetBSD vnode backend — that is V4
> (rd vms-308); V3 is the width-proof of the CORE.
>
> **Verdict up front: the ODS-2 core is width-clean for VAX.** The entire core
> compiles `-Werror` freestanding for `elf32-vax`, every on-disk-format and
> in-core `_Static_assert` passes under ILP32, and every emitted object is
> verified `elf32-vax` / `architecture: vax`. No blocking ILP32 or endianness
> defect was found. **One width CONTRACT is made explicit and pinned** (the wall
> clock must be 64-bit — §4.1); it is already satisfied by the Linux backend and
> is a checklist item for the NetBSD backend. Two properties are recorded as
> **backend obligations / non-defects** (§4.2, §4.3).

## 1. The width class (identical to the libvmssys audit)

VAX is **32-bit, little-endian**, model **ILP32**: `int`=32, `long`=**32**,
pointer=**32**, `long long`=64. Every other OVMX target is **LP64**. So, as with
libvmssys, the delta VAX introduces is **almost entirely width (LP64→ILP32), not
byte order**: VAX shares the little-endian byte order of x86_64 / aarch64 / the
Alpha oracle, so nothing here turns on a big-endian split. The proof asserts the
class so it cannot pass on an LP64 host by accident
(the width TU generated at runtime by `tools/cross-vax/build-vmsfs-core-vax.sh`):

```c
_Static_assert(sizeof(long)   == 4, "VAX long must be 32-bit (ILP32)");
_Static_assert(sizeof(void *) == 4, "VAX pointer must be 32-bit (ILP32)");
_Static_assert(sizeof(size_t) == sizeof(void *), "size_t tracks pointer width");
```

The systemic ILP32 hazard is any place that assumes `sizeof(long)==8` /
`sizeof(void*)==8`, or hardcodes a 64-bit width where ODS-2 means a **longword**
(32-bit), or lets a host-width type leak into on-disk / cross-boundary math. The
audit hunted exactly those across the allocator, the directory scanner, the
file-header codec and the retrieval-map math.

## 2. The on-disk format is fixed-width by construction — proven under ILP32

`vmsfs_ondisk.h` builds every on-disk struct (`vmsfs_home_block`,
`vmsfs_file_header`, `vmsfs_dir_entry`, `vmsfs_retrieval_ptr`) from **only**
`uint8/16/32/64_t` + `char[]` + `__attribute__((packed))`. There is **no**
`long`, pointer, or `size_t` field anywhere on disk. Its own `_Static_assert`s
(512-byte home block, 512-byte file header, 88-byte dir entry, 8-byte retrieval
pointer) are re-evaluated by the VAX compiler on every core TU that includes it,
and the generated width TU adds explicit **offset** checks (`fh_size`@8,
`fh_created`@16, `fh_map`@152, `fh_checksum`@508, `hb_modified`@16, `de_name`@8).
All pass on `elf32-vax`. Because VAX is little-endian, the on-disk byte order the
format already commits to ("all multi-byte fields are little-endian") matches the
VAX native order, so decode/encode need no byte-swap — the `vmsfs_le*_to_cpu` /
`vmsfs_cpu_to_le*` ops are width-exact identities on this target. **Clean.**

## 3. The algorithms carry no host-width assumption

A full read + a compile-`-Werror` pass of the six core sources found every
block-addressing quantity — LBN, VBN, cluster/`rp_count`, FID, bitmap bit index —
typed as **`uint32_t`** (or `uint16_t` for `map_count`/version), never `long` and
never pointer-width. Confirmed per file:

| core file | width-sensitive surface | result on ILP32 |
|---|---|---|
| `vmsfs_map.c` | VBN↔LBN walk, `rp_count` accumulation, map extend — all `uint32_t`/`uint16_t` | **clean** (pure fixed-width arithmetic) |
| `vmsfs_alloc.c` | bitmap block index, LBN math, free-count, FID scan — `uint32_t`; `(char*)vol->bitmap + block_idx*512` pointer math | **clean** (`block_idx` is `uint32_t`; overflow only past a multi-TB bitmap, unreachable) |
| `vmsfs_dirscan.c` | per-block slot offsets (`unsigned int`), FID/version compares (`uint32_t`/`uint16_t`) | **clean** |
| `vmsfs_header.c` | decode/encode every field through `vmsfs_le*` at its exact on-disk width | **clean** |
| `vmsfs_version.c` | version int range-checked to `VMSFS_MAX_VERSION` (32767) | **clean** |
| `vmsfs_name.c` | `size_t nlen = dot - fullname` pointer difference | **clean** (`size_t`/`ptrdiff` is the correct width on ILP32) |

The **in-core POD carriers** the core passes across the seam are fixed-width too:
`vmsfs_fh_info`/`vmsfs_fh_meta` use `uint64_t` for `size`/timestamps and
`uint32_t`/`uint16_t` for the rest (no `long` value field); `struct vmsfs_volume`
holds only `uint32_t` geometry plus two native pointers (`void *host`,
`unsigned long *bitmap`) that are the correct 4 bytes on VAX. The generated width TU
pins the 64-bit fields (`fh_info.size`/`.created`, `fh_meta.size` == 8 bytes) so a
future narrowing would fail the build. **Clean.**

## 4. Recorded items

### 4.1 CONTRACT (made explicit + pinned): `vmsfs_now_seconds()` must be 64-bit
The core writes the wall clock straight into the **64-bit** on-disk timestamps:
`fh->fh_modified = vmsfs_cpu_to_le64(vmsfs_now_seconds())` (and the home-block
`hb_modified`). If a backend declared `vmsfs_now_seconds()` returning a **32-bit**
`time_t` — as some 32-bit hosts still do — the value would **truncate on ILP32
before** the `le64` store: a Y2038 defect invisible on LP64. This is the ODS-2
analog of the libvmssys `vms_time_t → int64_t` finding (that audit's §5.1). The
width-audit backend pins the return type to `int64_t` and
the generated width TU asserts `sizeof(vmsfs_now_seconds()) == 8`, so the whole
core is type-checked against a 64-bit clock. **Already satisfied on Linux**
(`ktime_get_real_seconds()` is `time64_t` = `s64`). **NetBSD-backend obligation
(V4, rd vms-308):** declare its `vmsfs_now_seconds()` 64-bit (NetBSD `time_t` is
64-bit on all ports, so this is natural) — never a 32-bit intermediate.

### 4.2 Backend obligation (non-defect): the in-memory bitmap word width differs
`struct vmsfs_volume.bitmap` is `unsigned long *` — **32-bit words on VAX**,
64-bit on LP64. This is a purely **in-memory** representation the backend owns:
the core passes only `uint32_t` bit indices to `vmsfs_bit_*` and never indexes a
word itself, so the word width is invisible to the ODS-2 logic. The on-disk
bitmap is a byte/bit array; on a **little-endian** host the bit-`i` ↔
byte/word mapping is identical whether the words are read 32- or 64-bit-wide, so
VAX (LE) needs no special handling. **Non-defect**; noted so the NetBSD backend
sizes its cached bitmap in its own `unsigned long` and does not assume 64-bit
words. (A hypothetical big-endian ODS-2 host would have to reconcile the on-disk
bit order with its word width — not a VAX concern.)

### 4.3 Deferred to a runtime oracle (V5/SIMH): on-disk round-trip validation
This is a **static** width proof: it compiles the core for `elf32-vax` and checks
sizes/offsets/`-Werror`, but it does not **run** — there is no VAX system target
here (QEMU has none; the NetBSD/vax SIMH lab is the `netbsd-vax-simh` job, and no
NetBSD vnode backend exists yet). So it cannot yet prove a volume **written** by
a 32-bit VAX build and **read** by a 64-bit build (or a real VAX) round-trips
byte-identically. The format's fixed-width + LE construction makes that a strong
expectation, not a proof. **Deferred to V5** (a booted NetBSD/vax or SIMH volume
round-trip), gated behind V4 (the vnode backend that can actually mount).

## 5. How this was validated (the gate)

`tools/cross-vax/build-vmsfs-core-vax.sh` runs inside the pinned
`ovmx-cross-vax` image (`tools/cross-vax/Dockerfile`; the same `vax--netbsdelf`
GCC 13.3.0 / binutils 2.42 toolchain the libvmssys audit uses — the CI job shares
its gha layer cache, no gcc rebuild). It compiles each core TU with
`-ffreestanding -fno-builtin -Werror` against the **width-audit backend**
(`tools/cross-vax/vmsfs_backend_audit.h`, selected by `-DOVMX_KBACKEND_AUDIT`),
archives `libvmsfs_core.a`, and asserts every object is `file format elf32-vax` /
`architecture: vax`.

The audit backend is **BUILD/TEST TOOLING, never a runtime** (Rule 9): it binds
no VFS, mounts nothing, and declares the block-buffer / bitmap ops as **extern
prototypes with no bodies** — the core compiles to **relocatable objects**
(`gcc -c`), never a runnable image, so leaving them unresolved is correct.
Supplying fake success bodies would be exactly the silent-userspace-fallback
LARP the authenticity invariants forbid; the real ops live in the shipping
backends (`vmsfs_backend_linux.h` today, the NetBSD vnode backend at V4). A new
selection branch (`OVMX_KBACKEND_AUDIT`) was added to
`src/kernel-core/vmsfs/vmsfs_backend.h`; it is inert for the Linux `ko` (only
taken when its macro is defined by the cross build), so `vmsfs.ko` is unaffected.

The CI job **`vmsfs-core cross-compiles for elf32-vax`** runs the whole thing
containerized, gated per-PR on `src/kernel-core/vmsfs/**` + `tools/cross-vax/**`
(+ `vmsfs_ondisk.h` + this doc), and always on push/merge_group/schedule. There
is **no** emulation — build-only (§4.3). This unblocks V4 (the NetBSD vnode
backend, rd vms-308).
