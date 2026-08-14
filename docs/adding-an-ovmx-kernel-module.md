# Adding an OVMX kernel module

`drivers/ovmx/` is OVMX's in-tree home for kernel modules. It is a **menu**, not
a hard-coded `vms.ko` + `vmsfs.ko` pair: adding a module is a fixed, three-step
motion, after which the new module inherits the properties the owns-kernel work
established — it builds **in-tree** (`modinfo intree=Y`, so loading it does not
set `TAINT_OUT_OF_TREE`, vms-934) and is **signed** with the committed OVMX key
(so `finit_module()` verifies it and does not set `TAINT_UNSIGNED_MODULE`,
vms-ff5) — *for free*. This is the slot the DECnet kernel half (vms-30e,
`docs/design-decnet-ovmx.md`) drops into.

Generalized structure: rd **vms-bae**, epic **vms-19e** ("owns-kernel").

## The model (read this first)

The module **source** is canonical in the OVMX repo (`src/kernel*`, or wherever
your module's sources live). It is **not** forked into the kernel tree. At
kernel-build time `distro/kernel/overlay-ovmx-drivers.sh` copies (flattens) that
source into a from-source Linux tree as `drivers/ovmx/<mod>/`, generates the
`drivers/ovmx/Kconfig` menu wrapper and `drivers/ovmx/Makefile` from whatever
module subdirs it finds, and wires `drivers/ovmx/` into the kernel's own
`drivers/Kconfig` + `drivers/Makefile`. `make modules` then builds each
`<mod>.ko` as part of *our* tree. "In-tree" means **our** tree — no
mainline/Linus acceptance is implied or required.

Each module is a **self-describing subdir** of `distro/kernel/drivers-ovmx/`:

```
distro/kernel/drivers-ovmx/<mod>/
    Kconfig       # declares exactly one `config OVMX_<MOD>` tristate
    Kbuild        # the in-tree object list (obj-$(CONFIG_OVMX_<MOD>) += <mod>.o …)
    sources.conf  # globs (repo-relative) to FLATTEN into the module dir
```

The overlay **discovers** any subdir containing a `Kbuild`. Nothing hard-codes
the module names — not the overlay, not the generated `Kconfig`/`Makefile`, not
the Dockerfile harvest/sign/verify loops. That is what makes a new module free.

## The three steps

### 1. New subdir: `distro/kernel/drivers-ovmx/<mod>/`

Create the three files. Example for a hypothetical `decnet` module:

`drivers-ovmx/decnet/Kconfig` — exactly one `config OVMX_<MOD>` stanza:

```kconfig
config OVMX_DECNET
	tristate "OVMX DECnet Phase IV (forward-ported net/decnet)"
	depends on NET
	default n
	help
	  The OVMX DECnet Phase IV protocol stack ...
```

`drivers-ovmx/decnet/Kbuild` — the object list, same shape as an ordinary
in-tree Kbuild. Sources are **flattened** (no subdirs) into the module dir, so
every object is a plain basename and every local `#include "…"` is basename-only
(resolved by `-I $(src)`):

```make
# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_OVMX_DECNET) += decnet.o
decnet-y := af_decnet.o dn_dev.o dn_route.o dn_nsp_in.o dn_nsp_out.o
ccflags-y := -I $(src)
```

`drivers-ovmx/decnet/sources.conf` — one repo-relative glob per line naming the
canonical source to flatten (`#` comments and blank lines ignored; globs are
non-recursive):

```
src/decnet/*.c
src/decnet/*.h
```

> The overlay **fails** a module that flattens zero source files, and the
> `<mod>-y` object list in `Kbuild` must match the flattened basenames — keep
> `sources.conf` and `Kbuild` in sync exactly as you would for any out-of-tree
> Makefile you also maintain.

### 2. Kconfig stanza

That's the `config OVMX_<MOD>` block from step 1 — it lives in the subdir's own
`Kconfig`. The overlay generates the `menuconfig OVMX` wrapper and adds a
`source "drivers/ovmx/<mod>/Kconfig"` line for you; you never edit a shared
Kconfig. The Makefile `obj-$(CONFIG_OVMX_<MOD>) += <mod>/` line is derived from
the symbol your Kconfig declares, so the two can never disagree.

### 3. Config flag

Turn the module on in the kernel config fragment(s) — `distro/kernel/ovmx-<arch>.config`:

```
CONFIG_OVMX_DECNET=m
```

`CONFIG_OVMX=y` (the menu) is already set. Build modules `=m` (they load at boot
via `finit_module()`); `=y` only if you are statically building an OVMX-only
kernel.

## That's it — what you inherit

Once those three touch-points exist, with **no other edits**:

- The overlay stages your sources in-tree and regenerates the menu + Makefile.
- The Dockerfile kernel-build stage (`distro/Dockerfile.bootable`) verifies
  `CONFIG_OVMX_<MOD>=m` is present, then **discovers** your built
  `drivers/ovmx/*/*.ko`, harvests it, asserts `intree=Y`, and **signs** it with
  the committed OVMX key — the loop names no module, so yours is covered.
- If you ship the module in a boot initramfs, the boot-time taint gate
  (`tests/qemu/test_kernel_taint.sh`, vms-566) discovers every `*.ko` in the
  image and asserts each is `intree=Y` + signed against the real
  `/proc/sys/kernel/tainted` — again, no module named.

## Where it ships (a separate decision)

Building a signed, in-tree `.ko` is automatic. **Which initramfs it ships in**
is a deliberate product choice — the FAT/SLIM initramfs staging in
`distro/Dockerfile.bootable` copies the specific modules a given boot needs. Add
your `cp …/<mod>.ko …/lib/modules/` there if and when the runtime should load
it. (The two executive-critical modules, `vms.ko` and `vmsfs.ko`, are staged and
gated explicitly; a new module is additive.)

## Verifying your change

- **Fast, hermetic (no kernel build):** `tests/integration/test_ovmx_module_home.sh`
  runs the real overlay against a fabricated tree with a demonstrator module and
  proves the discover → generate → flatten → wire path. Run it after editing the
  overlay or the scaffold. It is a standing ctest gate (`ovmx_module_home_gate`).
- **Full proof (real boot):** the `distro/Dockerfile.bootable` build + the
  taint-clean acceptance gate prove `intree=Y` + signed against the real booted
  kernel. These run in CI on any `distro/**` change.

## Invariants (do not break)

- **Overlay, not fork.** `src/…` stays the single source of record; the same
  sources still drive the standalone out-of-tree build used by the QEMU test
  harness. Never edit the flattened copies under `drivers/ovmx/`.
- **No stale top-level Kconfig/Makefile** in `distro/kernel/drivers-ovmx/` — the
  overlay generates them; a checked-in copy would be a second, drifting source.
  The module-home gate fails if one reappears.
- **Signing stays reproducible.** The committed key
  (`distro/kernel/ovmx-module-signing-key.pem`, `CONFIG_MODULE_SIG_KEY`) is what
  keeps signatures byte-identical build-to-build (vms-d73). Do not switch to the
  kernel's build-ephemeral key. `CONFIG_MODULE_SIG_FORCE` stays **off** (vms-ff5)
  so a signing slip can never brick the boot.
- **Rule 9.** The kernel/QEMU path is the one runtime. A kernel module is real
  executive/driver code proven against a real boot — never a userspace fake.
