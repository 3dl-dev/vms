# ILP32 + Endianness Audit — the executive `vms` module (core + NetBSD backends) on elf32-vax (rd vms-20b9, epic vms-8e8)

> **Scope:** the P4-**B** width audit — the VAX-width step for the OVMX
> **executive**. It audits the substrate-neutral executive core
> (`src/kernel-core/vms_{eflag,ast,access,mbx,proctab,lock}.c`) **and** all its
> NetBSD backends (`src/kernel-netbsd/vms_netbsd.c`,
> `exec_{list,hash,rbtree}_netbsd.{c,h}`, `exec_kbackend_netbsd.h`) for the
> **VAX (32-bit, little-endian, ILP32)** width class — the same class the
> libvmssys (`docs/audit-ilp32-vax-libvmssys.md`) and ODS-2 core
> (`docs/audit-ilp32-vax-vmsfs.md`) audits established. It is the executive analog
> of those items: where V3 proved the ODS-2 **kernel filesystem** core
> cross-compiles for netbsd-vax, this proves the executive **`vms` kernel module**
> — the shared core **and** its real NetBSD backends — does. It does **not** LOAD
> the module on NetBSD/vax under SIMH against a live `/dev/vms` — that is **B2**
> (rd vms-f78bb); **B1 is the width-clean COMPILE.**
>
> **Verdict up front: the executive module is width-clean for VAX.** All ten
> module translation units compile `-Werror` for `elf32-vax` against real
> NetBSD/vax kernel headers, they relocatable-link load-coherently, every ILP32
> `_Static_assert` (width class, the packed RB parent+colour word, container node
> sizes/offsets) passes, and every emitted object is verified `elf32-vax` /
> `architecture: vax`. **No ILP32 or endianness defect was found, and no source
> change was required** — neither to the shared `src/kernel-core/*.c` (INV-DRIFT:
> untouched) nor to any NetBSD backend. The one item the scope flagged as a risk —
> the packed parent+colour pointer in the red-black tree — is proven portable
> (§3). The proof is `tools/cross-vax/build-vms-module-vax.sh`, gated per-PR as
> **"NetBSD/vax vms module cross-compiles"**.

## 1. The width class

VAX is **32-bit, little-endian**, model **ILP32**: `int`=32, `long`=**32**,
pointer=**32**, `long long`=64. Every other OVMX target is **LP64**. As with the
libvmssys and ODS-2 audits, the delta VAX introduces is **almost entirely width
(LP64→ILP32), not byte order** — VAX shares the little-endian byte order of
x86_64 / aarch64 / the Alpha oracle, so nothing here turns on a big-endian split.
The proof asserts the class so it cannot pass on an LP64 host by accident (the
width TU generated at runtime by `build-vms-module-vax.sh`):

```c
_Static_assert(sizeof(long)      == 4, "VAX long must be 32-bit (ILP32)");
_Static_assert(sizeof(void *)    == 4, "VAX pointer must be 32-bit (ILP32)");
_Static_assert(sizeof(int)       == 4, "int 32-bit");
_Static_assert(sizeof(long long) == 8, "long long 64-bit");
_Static_assert(sizeof(size_t)    == sizeof(void *), "size_t tracks pointer width");
```

The systemic ILP32 hazard for the executive is any place that assumes
`sizeof(long)==8` / `sizeof(void*)==8`, packs data into a word on the assumption
of a particular width, or lets a host-width type leak into a cross-boundary
(user↔kernel) datum. The audit hunted exactly those across the six shared
facilities and the three intrusive containers.

## 2. Why the executive is a REAL NetBSD kernel compile (not a freestanding audit)

The ODS-2 core (V3) is substrate-neutral kernel filesystem code that reaches its
world through extern block-I/O ops, so its VAX width proof compiles it against a
synthetic **freestanding** backend. The executive cannot be audited that way: its
module glue `vms_netbsd.c` is inherently a NetBSD kernel TU — it names
`sys/device.h`, `sys/module.h`, `sys/ioccom.h`, `kauth(9)`, `kmem(9)`,
`kmutex(9)`. This audit therefore compiles the **real** module — the same SRCS as
`src/kernel-netbsd/Makefile`, with `-DOVMX_KBACKEND_NETBSD`, against the **real**
NetBSD/vax kernel headers (the pinned NetBSD 10.1 `syssrc`, machine→`arch/vax`) —
under `vax--netbsdelf-gcc`. It is the exact elf32-vax twin of the amd64
`tests/netbsd/crosscompile.sh` gate, only under the 32-bit VAX compiler. That the
whole NetBSD backend set (kauth/kmem/kmutex/cv/pserialize/membar twins) compiles
unmodified for VAX is itself a result: the backends carry no amd64/LP64 width
assumption.

## 3. The packed RB parent+colour word — the flagged risk, proven portable

`exec_rbtree_netbsd.c` (OVMX's own intrusive red-black tree, the sole backend the
scope flagged) packs each node's **parent pointer and colour bit into one word**:

```c
typedef struct exec_rbtree_node {
	struct exec_rbtree_node *rb_left;
	struct exec_rbtree_node *rb_right;
	unsigned long            __rb_parent_color;   /* parent | colour-bit */
} exec_rbtree_node_t;

/* recover: (exec_rbtree_node_t *)(n->__rb_parent_color & ~1UL)
 * colour:  n->__rb_parent_color & 1UL   (RB_RED == 0) */
```

The concern the scope raised — "a 32-bit pointer changes the packing" — resolves
to a **non-defect**, because the encoding keys on `unsigned long`, and the packing
is correct **iff an `unsigned long` can hold a whole pointer**:

| model | `long` | `void *` | `unsigned long` holds a pointer? | node size |
|-------|-------:|---------:|:--------------------------------:|----------:|
| ILP32 (VAX)      | 4 | 4 | **yes** | 12 bytes |
| LP64 (amd64/arm) | 8 | 8 | **yes** | 24 bytes |
| LLP64 (Win64)    | 4 | 8 | **no**  | — (not an OVMX target) |

So the same `(unsigned long)p` cast, `& ~1UL` recovery and `& 1UL` colour read are
width-exact on **both** VAX and amd64: the node simply shrinks from a 24-byte
(LP64) to a 12-byte (ILP32) object, and the packed word moves with it. It would
break **only** on LLP64 (`long`=32, pointer=64), which OVMX does not target. The
low colour bit is free on VAX for the same reason as everywhere else — the node
contains pointers, so it is ≥4-byte aligned and the low bit is always zero. The
generated width TU pins all of this at compile time under the VAX compiler:

```c
_Static_assert(sizeof(unsigned long) == sizeof(void *),
	       "packed RB parent+colour word must hold a whole pointer (ILP32/LP64)");
_Static_assert(_Alignof(exec_rbtree_node_t) >= 2,
	       "RB node must be >=2-byte aligned so the low colour bit is free");
_Static_assert(sizeof(exec_rbtree_node_t) == 12, "RB node = 3 words = 12 bytes on ILP32");
_Static_assert(offsetof(exec_rbtree_node_t, __rb_parent_color) == 8, "packed word @8 on ILP32");
```

This is the customary intrusive-tree encoding (the same one `<linux/rbtree.h>`
uses, which is portable across ILP32 and LP64 for exactly this reason); OVMX's
clean-room implementation inherits its portability.

## 4. The other intrusive containers and the backend seam — width-clean

- **`exec_hash_netbsd.{c,h}`** — the intrusive hash node is two native pointers
  (`next`, `pprev`), 8 bytes on ILP32. Keys are `uint32_t` and bucketing is
  `key % nbuckets` (fixed-width in, fixed-width modulo) — no `long`/pointer width
  enters the hash math. The width TU pins `sizeof(exec_hash_node_t)==8` and
  `sizeof(struct exec_hash_head)==4`.
- **`exec_list_netbsd.{c,h}`** — a two-pointer intrusive node (`next`, `prev`),
  8 bytes on ILP32; pinned by the width TU. Pure pointer relinks, no width math.
- **`exec_kbackend_netbsd.h`** — the allocation header
  (`union { size_t size; uint64_t _a64; }`) uses an explicit `uint64_t` alignment
  floor, so its size is width-stable; `kauth(9)` credential reads are cast to
  `uint32_t` (the UIC pack `(gid<<16)|uid` is fixed-width); `dev_t`/`major`/`minor`
  are the NetBSD types. Nothing here assumes a 64-bit `long`.
- The shared **`src/kernel-core/*.c`** facilities were **not modified**
  (INV-DRIFT): they carry no host-width assumption — VMS status codes,
  event-flag masks, lock/AST identifiers and mailbox channel numbers are all
  fixed-width (`uint32_t`/`uint64_t`) or native-pointer, so they compile
  identically for ILP32 and LP64.

## 5. Endianness

VAX is **little-endian**, matching every other OVMX target. This module carries no
on-disk or on-wire format of its own (those live in the ODS-2 core and the SCS/
NISCA wire audits); the executive's cross-boundary data are the ioctl argument
structs copied via `exec_copyin`/`exec_copyout`, whose fields are fixed-width by
construction. No byte-swap is required or present, and no path turns on a
big-endian split. **Clean.**

## 6. What this proves and what it does not

**Proves (B1):** the whole executive `vms` module — the shared core **and** every
NetBSD backend — is ILP32/endian-clean and cross-compiles + relocatable-links for
`elf32-vax`. A relocatable link failing on a duplicate definition is the
load-coherence property asserted; the residual undefined symbols are the NetBSD
KPIs the module binds to the running kernel at load.

**Does not prove (B2, rd vms-f78bb):** that the module LOADS on a real NetBSD/vax
kernel under SIMH and that `/dev/vms` answers ioctls there. That needs a booted
VAX and is the next tributary step. Compile-cleanliness is necessary, not
sufficient, for load — exactly the B1/B2 split the amd64 gate (per-PR compile) vs.
the nightly console harness (runtime load) already draws.
