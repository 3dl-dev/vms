# GCC-port host-surface gap map (F2a rung-2, vms-9ed)

> **Status:** MEASURED, 2026-09-11. rung-2 of vms-078 (F2a): intersect rung-1's
> 523-symbol demand ([design-gcc-port-host-surface-demands.md](design-gcc-port-host-surface-demands.md))
> against what OVMX's host surface (musl 1.2.5, the `DECC$SHR` producer) actually
> provides → the ABSENT set = the real gaps. Feeds rung-3 (vms-e52, the gate).

## Method

Ground-source, both sides measured (no assertion): the **provided** set = `nm -g
--defined-only` of musl 1.2.5's `libc.a` (**1937** symbols; musl 1.2.5 = OVMX's
pinned musl, sampled via Alpine 3.20 on the k3s-worker). Intersected against the
523 de-versioned rung-1 demands. `comm -23` → **345 absent**. Raw:
[data/gcc-port-host-surface-absent.txt](data/gcc-port-host-surface-absent.txt).

## The answer: the host CRTL surface essentially SUFFICES

345 sounds large; it is not a CRTL wall. It splits cleanly:

| Bucket | Count | What it is |
|---|---:|---|
| **ISL** (`isl_*`) | 222 | GCC's Graphite polyhedral loop optimizer library — **optional** |
| **MPFR** (`mpfr_*`) | 71 | bignum float — GCC required dependency **library** |
| **MPC** (`mpc_*`) | 21 | complex bignum — GCC required dependency **library** |
| **GMP** (`__gmp*`) | 20 | bignum — GCC required dependency **library** |
| **True host-CRTL/libc gaps** | **11** | the only entries that are the C-runtime surface |

**334 of the 345 are GCC's own dependency LIBRARIES, not the host C-runtime.** They
are provided by *linking libraries into GCC*, not by the OVMX `DECC$SHR` CRTL:
- GMP/MPFR/MPC — required; already addressed by **vms-5b7e** (GMP/MPFR/MPC build on
  OVMX, marked done). Confirm they are on the OVMX host link path for rung-3.
- **ISL (222) — newly surfaced, and OPTIONAL.** ISL backs only `-fgraphite`
  loop-nest optimization. GCC configures `--without-isl` cleanly, dropping only
  Graphite. Faithful scope call for F2a: build the port `--without-isl` (or provide
  ISL on OVMX later). Not a CRTL gap; does not block the host-surface-suffices claim.

## The 11 true host-CRTL/libc gaps

7 of 11 are glibc-naming aliases for functions **musl provides under a different
name** — a mapping, not an absence:

| Demanded (glibc name) | musl reality | Resolution |
|---|---|---|
| `__isoc23_sscanf`, `__isoc23_strtol`, `__isoc23_strtoul`, `__isoc23_strtoull` | glibc-2.38 C23 interposition aliases; musl uses the plain names, which it HAS | alias `decc$__isoc23_X → sscanf/strtol/…` (4) |
| `fopen64`, `fseeko64`, `ftello64` | musl is natively 64-bit `off_t` — no `_64` suffix; `fopen`/`fseeko`/`ftello` ARE in musl | alias `*64 → unsuffixed` (3) |

4 are genuine minor glibc-isms, all stubbable or optional (GCC degrades without them):

| Symbol | glibc feature | Impact / resolution |
|---|---|---|
| `__libc_single_threaded` | 2.32+ single-thread fast-path flag | GCC uses it only as an optimization hint; a `0` stub is correct-and-safe |
| `_dl_find_object` | 2.35+ dynamic-loader unwind helper | unwind-path only; the port carries its own EH — stub/omit |
| `mallinfo2` | glibc malloc introspection | `-fmem-report` only; stub returning zeros |
| `arc4random` | RNG | musl **1.2.3+ provides it** (absent only from this Alpine `libc.a` sample); confirm in OVMX's musl build — trivial stub if not |

## Weak-stub-satisfied demands — link-resolvable, RUNTIME-UNIMPLEMENTED (latent facade)

Per the honest-gaps rule (a weak stub that fails at runtime must be NAMED, not
counted as "provided"): these demands are **not** in the absent set because musl
static `libc.a` defines them — but as stubs that FAIL at runtime under static
linking (OVMX images are static musl):

- **`dlopen` / `dlsym` / `dlclose`** — musl static dynamic-loading stubs (also
  explicitly exported in `src/vmslink/mk_decc_shr.sh` for tcc's `-l` path). They
  **link** on the OVMX host surface (so they are not an F2a link-surface absence),
  but at RUNTIME they return failure — GCC's plugin / `-flto`-via-`dlopen` paths
  would not work. **Classification: link-resolvable weak stub, runtime-unimplemented.**
  Resolution for F2a: build the port `--disable-plugin` (and note LTO-via-dlopen is
  a downstream runtime item). The runtime rung inherits this as a known
  runtime-unimplemented, not a hidden green.

## Verdict for rung-3 (and the claim scope)

The OVMX host CRTL/`DECC$SHR` **link surface suffices** to host the port GCC, modulo:
(a) 7 trivial aliases + 4 stubbable/optional glibc-isms (11 CRTL gaps), (b) provide
GMP/MPFR/MPC (vms-5b7e) on the link path, (c) build `--without-isl` and
`--disable-plugin` (faithful scope calls, dropping only Graphite + runtime plugin
loading). rung-3 (vms-e52) then proves configure + a minimal multi-TU compile → real
target ELF over this surface. **Claim stays link-surface-scoped** — this does NOT
assert the produced GCC's CRTL calls behave faithfully at runtime (downstream,
OOM/alpha-gated), and the weak-stub `dl*` are named so that rung inherits an accurate
list.
