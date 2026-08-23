# alpha-dec-vms cross toolchain (build / oracle tooling)

A containerized from-source build of the **`alpha-dec-vms`** (OpenVMS/Alpha,
VMS ABI) cross toolchain — GNU **binutils** (assembler/linker/objdump for EVAX
objects) plus **GCC `cc1`** (the compiler proper). It emits genuine
alpha-dec-vms EVAX objects, which is what makes it useful: it lets us **gap-probe
OVMX's `LINK.EXE` against real GCC-port output** instead of reasoning our way to
gaps (which produces false ones — see the note below).

## Why it's here / Rule-9 posture

This is **build/oracle tooling, not an OVMX runtime target.** The compiler runs
on the build host; it never runs inside the OVMX guest and has no `/dev/vms`
dependency, so it is Rule-9-clean (the same category as
`tools/cross-alpha/`, `distro/Dockerfile.bootable`, etc.).

Note the distinction from the OVMX **Alpha runtime lane**, which is
**OVMX/Linux-Alpha** — the *Linux* ABI on Alpha. That is a different target and
its toolchain (`gcc-alpha-linux-gnu`, packaged by Debian; see
`tools/cross-alpha/`) does **not** emit VMS-ABI objects. The GCC *port* OVMX
aims to build compiles for `alpha-dec-vms` (the VMS ABI), which no distribution
packages — hence this from-source build.

## Use

```sh
tools/cross-alpha-vms/build.sh [OUTDIR]     # default OUTDIR = tools/cross-alpha-vms/out
# then, e.g.:
OUTDIR/bin/alpha-dec-vms-gcc -S -mpointer-size=64 hello.c -o hello.s
OUTDIR/bin/alpha-dec-vms-as  -o hello.obj hello.s        # a real EVAX object
# -> feed hello.obj to OVMX LINK.EXE's EVAX path (--transfer / --use DECC$SHR)
```

`cc1`-only (`make all-gcc`) is built: the compiler proper emits `.s` and needs
no target libc/headers, so the image stays small. The build **smoke-tests**
itself — a trivial `int main(void){return 0;}` must emit `__gcc_main_flags = 3`
(the port's main-flags globalvalue) and VMS procedure descriptors (`.ent` /
`.pdesc`).

## Pinned versions and the two GCC-14 cross wrinkles

- binutils **2.43**, GCC **14.2.0** (override with `--build-arg BINUTILS_VER=` /
  `GCC_VER=`).
- `build-toolchain.sh` carries the two fixes a GCC-14 cross `make all-gcc` needs,
  documented inline:
  1. **`--disable-fixincludes`** — else `stmp-fixinc` wants the build-side
     `fixincludes/fixinc.sh` a cross build never produces.
  2. **pre-create `gcc/{d,rust,...}`** — `all-gcc` generates per-frontend
     target-hooks headers for every frontend even with `--enable-languages=c`,
     and the `mv` fails if the subdir is absent.

## Checked-in port patches (`patches/`)

We **patch the fetched GCC source, never vendor the whole tree.** Each file in
`patches/` is a plain `patch -p1` unified diff; `build-toolchain.sh` applies them
right after `tar xf` and before `configure`, and the `Dockerfile` `COPY`s the
directory into the build context. They must stay minimal, targeted, and
clean-room (Rule 8: derived from public GCC source + observed cc1 output only).

- **`0001-vms-f97-alpha-en-label-decorated-name.patch`** (vms-f97) — codegen
  consistency: derive the procedure **entry label** (`..en`) from the same
  resolved (transparent-alias-decorated) name that `.ent`/`.pdesc` already use.
  Without it, a **definition** of a recognized OpenVMS C-RTL name (musl's
  `strlen`, `malloc`, `memcpy`, `vsnprintf`, …) emitted `.pdesc decc$strlen..en`
  pointing at a nonexistent `strlen..en` label, so GAS rejected ~52% of musl.
  The fix does **not** change the decoration itself — only that the entry label
  matches `.ent`/`.pdesc`. It is the operator-ruled path (A) to the Alpha
  DECC$SHR: musl-as-DECC$SHR then defines the `decc$`-prefixed CRTL names.

## Why real objects, not reasoning

Reasoning about "what the port needs" from source alone produced a **false gap**
once (`__gcc_main_flags` was assumed unlinkable but the linker already folds it,
because its `$ABS$` psect is alloc-0 and lands at base 0). The rule banked from
that: **verify a gap is real against the current `LINK.EXE` before building a
fix** — and the best way to surface real gaps is to link the compiler's actual
output. That's what this toolchain is for.
