# Kernel-build host-tool inventory (vms-bab F0)

> **Status:** MEASURED, 2026-09-11. Empirical answer to the "unknown until measured"
> host-tool question posed in [design-self-hosting-own-kernel.md](design-self-hosting-own-kernel.md)
> §6.2/§7/§8. Converts the GNV-scale-POSIX-layer scope from hypothesis to a concrete list.
> Parent epic: `vms-df7` (OVMX = distribution). Ladder item: `vms-bab` (F0). Feeds `vms-4fa`.

## Method

Instrumented OVMX's **pinned kernel version** `linux-6.12.103` (sha256
`f143aaad…`, cdn.kernel.org — the exact tree `distro/Dockerfile.bootable` builds),
in a clean `debian:12` container with OVMX's exact kernel-build deps
(`gcc binutils make bc bison flex libssl-dev libelf-dev openssl xz-utils curl
kmod cpio`), on the **k3s-worker** (48 GB — the OOM-safe rail; the workshop is
memory-starved):

```
make ARCH=x86_64 defconfig
strace -f -qq -e trace=execve -o ex.log  make ARCH=x86_64 -j$(nproc) bzImage modules
```

`strace -f -e trace=execve` captures **every** binary exec'd by the whole build
tree (make, sub-makes, `sh -c`, every compiler/assembler/host-tool invocation).
**66,892 execve calls** captured; `bzImage` + modules built clean. The build ran
under **`/bin/sh → dash`** and succeeded — see the shell finding below.

Baseline = upstream `defconfig` (reproducible, per the item). OVMX's real build
adds a few steps on top (see **OVMX-specific addenda**); those add a small,
enumerated delta, not a different tool class.

## The inventory

Three categories. The distinction between the first two (**host-provided**) and
the third (**kernel-self-built**) is the core result: OVMX must *ship* only the
first two; the third is compiled from the kernel's own `scripts/`+`tools/` during
the build and needs nothing but the C toolchain already in category A.

### A. Host-provided toolchain (must exist in the OVMX image)

| tool | execs | role |
|---|---:|---|
| `as` | 12212 | assembler (the workhorse — every .o) |
| `gcc` / `cc1` | 3083 / 3070 | C compiler driver + cc1 proper |
| `collect2` | 19 | gcc's link driver |
| `ld` | 54 | linker |
| `ar` | 1540 | archive (built-in.a, thin archives) |
| `objcopy` | 27 | bzImage/section surgery |
| `strip` | 19 | module/vmlinux strip |
| `objdump` | 19 | build-time inspection |
| `nm` | 9 | symbol extraction |
| `readelf` | 2 | ELF inspection |

I.e. a complete **GCC + GNU binutils** on the host. (`bison`/`flex` are install-time
deps for the kbuild parser generators; they did not exec in *this* defconfig run
but are required deps and belong in the provided set.)

### B. Host-provided shell + POSIX utilities (must exist in the OVMX image)

- **Shell:** `sh` (4719 execs) = **`/bin/sh → dash`**. **The build completed under
  dash → OVMX needs a POSIX `sh`, NOT bash, for the core kernel build.** This is the
  single most scope-relevant finding (it bounds the "GNV-scale POSIX layer" to
  POSIX-sh + coreutils, not a bash port). Kbuild's own shell scripts that ran
  (`link-vmlinux.sh`, `setlocalversion`, `mkcompile_h`, `remove-stale-files`,
  `build-version`) are all dash-compatible.
- **Text / file utilities:** `sed` (1041), `rm` (3803), `mkdir` (807), `cat` (717),
  `wc` (399), `tr` (364), `xargs` (275), `grep` (67), `mv` (32), `diff` (12),
  `awk` (12), `nm`, `sort`, `sha1sum`, `head`, `uniq`, `cut`, `basename`, `expr`,
  `find`, `ls`, `touch`, `install`, `ln`, `mktemp`, `dirname`, `date`, `cmp`,
  `tail`, `whoami`, `uname`, `getconf`, `true`.
- **Compression / misc:** `gzip` (bzImage payload), `bc` (1 — timeconst),
  `perl` (1 — minimal; a single config/version script). `cpio`/`kmod` are deps for
  initramfs + module handling.

`awk` and `perl` appear at very low counts → the POSIX layer needs them present but
the build is not perl/awk-heavy (a mawk + a small perl suffice; not a blocker).

### C. Kernel-self-built host tools (NOT host-provided — built from source in-tree)

These are compiled by the build itself using category-A's C toolchain, then exec'd.
OVMX does **not** ship them; it only needs to be able to *compile* them:

`fixdep` (3008), `objtool` (10), `modpost`, `kallsyms`, `relocs`, `asn1_compiler`,
`extract-cert`, `genheaders`, `vdso2c`, `gen_init_cpio`, `gen_crc32table`,
`conmakehash`, `mkpiggy`, `mkcpustr`, `mk_elfconfig`, `sorttable`,
`arch/x86/boot/tools/build`.

Note `asn1_compiler` + `extract-cert` ran even at defconfig → **module signing
infra is exercised by the baseline** (defconfig enables `CONFIG_MODULE_SIG`), which
is why `openssl`/`libssl-dev` is a required category-A-adjacent dep.

## OVMX-specific addenda (beyond the bare defconfig baseline)

`distro/Dockerfile.bootable` builds the kernel as `defconfig` +
`scripts/kconfig/merge_config.sh -m .config distro/kernel/ovmx-x86_64.config` +
`olddefconfig` + `overlay-ovmx-drivers.sh` + openssl module signing. Folding those
in adds, beyond A/B/C above:

- `merge_config.sh` — a **bash** script (`#!/bin/bash`). This is the one place the
  OVMX build path reaches for bash today; either provide bash for config-merge, or
  (cheaper, faithful to the dash finding) port/confirm it under POSIX sh. Tracked as
  a follow-on decision, not a category change.
- `overlay-ovmx-drivers.sh` — copies OVMX driver sources in; standard sh + cp/ln.
- `openssl` — module signing (already surfaced via `asn1_compiler`/`extract-cert`).

## Implication for the self-host ladder

This is a **bounded, GNV-scale POSIX layer**, not an open-ended one: a GCC+binutils,
a POSIX `sh` (dash-class — **no bash needed for the core build**), ~35 coreutils/text
utilities, `gzip`/`bc`/`perl`(minimal)/`cpio`/`kmod`/`openssl`, plus the ability to
*compile* the ~17 in-tree host tools. That converts §8 of the parent design from a
hypothesis into a concrete provisioning list for the OVMX image (`vms-4fa`).

Caveats: `defconfig` (not the full `ovmx-x86_64.config`) baseline — config changes
which `.c` compile, not which host *tools* run, so the tool SET is stable; the one
delta is the `merge_config.sh`/signing addenda above. Counts are from one build; the
*set* (not the counts) is the deliverable. Full execve log + per-path counts:
reproducible via `tools/…` (the instrumented run recorded in `vms-bab`).
