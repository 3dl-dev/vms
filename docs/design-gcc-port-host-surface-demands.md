# GCC-port host-surface demand inventory (F2a rung-1, vms-480)

> **Status:** MEASURED, 2026-09-11. rung-1 of vms-078 (F2a): what the alpha-dec-vms
> OpenVMS GCC port's host binaries DEMAND of the host C-runtime. Parent vms-078 →
> vms-df7/vms-da0. Feeds rung-2 (vms-9ed, the provided-vs-absent gap map).

## What this measures (and, honestly, what it does not)

The alpha-dec-vms GCC port's **host binaries** (`cc1`, the `alpha-dec-vms-gcc`
driver, `as`, `ld`, `collect2`) import a set of C-library + bignum symbols from
whatever host runs them. This is that demand set, measured by `nm -u`/`nm -D -u`
across those binaries in the current build (`tools/cross-alpha-vms`,
`build-toolchain.sh`): **523 unique imported symbols** (de-versioned).

**Honest scoping (INV-6, and the conductor's anti-LARP guard):** the current port
build is `--host=linux` **oracle tooling** (`tools/cross-alpha-vms/README.md`: "runs
on the build host; never runs inside the OVMX guest"), so these names are
**glibc-shaped** (`__assert_fail@GLIBC_2.2.5`, `fopen64`, …). This inventory is the
**functional demand SET** — the C-library/bignum *functions* the port needs from any
host, OVMX included — **not** a claim that a glibc surface suffices. OVMX images are
musl-over-`DECC$SHR`, so the mapping of this set onto the real OVMX host surface (the
`decc$`-prefixed universals) is **rung-2's** job, against the existing
[`design-gcc-port-surface-gaps-register.md`](design-gcc-port-surface-gaps-register.md)
(538 `decc$` universals, most PROVEN). The count agreeing to within ~15 of that
register is a strong consistency signal, not a coincidence.

Full de-versioned list: [`data/gcc-port-host-surface-demands.txt`](data/gcc-port-host-surface-demands.txt).

## Categorized demand (523 unique)

| Category | Count | Notes |
|---|---:|---|
| **Core CRTL / libc** (the `DECC$SHR` surface) | 502 | stdio (`fopen`/`fread`/`fprintf`/…), string/mem (`memcpy`/`strlen`/…), `malloc`/`calloc`/`free`, dirent (`opendir`/`readdir`/`closedir`), time (`ctime`/`asctime`), `fcntl`/`access`/`chmod`/`dup` — the bulk, expected to be almost entirely in the 538-universal register |
| **GMP / MPFR / MPC** (bignum) | 20 | `__gmpz_*`, `__gmp_fprintf`, … — GCC's constant-folding bignum deps. Ties to **vms-5b7e** (GMP/MPFR/MPC on OVMX, marked done): must be present on the OVMX host |
| **C++ runtime** | 1 | `__cxa_atexit` only (this build is C-focused; a `cc1plus`/`-flto` build would pull more libstdc++/libgcc-EH — out of scope for the minimal C gate) |
| **glibc `__`-internals** | 14 | glibc-specific (`__ctype_b_loc`, `__errno_location`, `__fsetlocking`, …) — the **musl/`decc$` mapping-risk set**: musl names/mechanisms differ; rung-2 must map or flag each |

## Gap CANDIDATES for rung-2 (the non-trivial demands)

Most of the 502 core-CRTL are trivially in the register. The demands worth
explicit rung-2 attention (likely-absent or mapping-sensitive on musl/`DECC$SHR`):

- **Dynamic loading** — `dlopen`, `dlsym`, `dlclose`, `dl_iterate_phdr`. GCC uses
  these for plugins/LTO and `dl_iterate_phdr` for unwind. If OVMX's host surface
  doesn't provide them, the port must be built **without plugin/LTO** (a documented
  scope call), or they must be backed. **Top rung-2 gap candidate.**
- **`arc4random`** — GCC 14 uses it; musl provides it, glibc-2.36+ too; confirm the
  `decc$` mapping.
- **LFS variants** — `fopen64` (and any `*64`): musl is natively 64-bit-off_t with
  no `_64` suffix, so these map to the unsuffixed name — a rung-2 aliasing note.
- **The 14 glibc `__`-internals** — map to musl equivalents or flag.

## rung-1 method + the DEMAND(B) note

- DEMAND(A) (symbol demand): `nm -D -u` + `nm -u` across the port host binaries in
  the `ovmx-cross-alpha-vms` image, on the **k3s-worker** (OOM-safe). Reproducible.
- DEMAND(B) (host-tool invocations + header reads via a real compile): attempted via
  `strace -f -e execve,openat`, but `strace` is **not** in the build image (rc=127).
  Low-value miss and deliberately **not** re-run for a 30-min toolchain rebuild: the
  host-**tool** set is already the vms-bab F0 inventory (`cc1`/`as`/`ld`/`collect2`/
  driver), and the port's **header** demand (`rtldef`/`starlet_c` include surface) is
  already covered by the register's R5 (vms-714c, landed). The *real compile run*
  that would exercise both belongs to **rung-3's** gate (which must actually RUN the
  compiler and inspect real target ELF — the anti-LARP done-condition on vms-e52),
  not to this measurement rung.

## Claim scope

rung-1 establishes the **demand**. It does NOT assert the OVMX host surface provides
it (rung-2) nor that the produced GCC runs faithfully at runtime (downstream,
OOM/alpha-gated). "The port GCC's host binaries demand these 523 C-library/bignum
functions" — full stop.
