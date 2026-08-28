# Alpha C-RTL archive — the DECC$SHR backing for `alpha-dec-vms` (GAP2-archive)

**Status:** DECIDED — **Option A** (port musl's `arch/alpha-dec-vms` layer),
operator ruling (Baron, 2026-08-23). Implementation routing in progress: the
musl-arch-port couples to the OVMX Alpha executive (GAP3 / Alpha boot lane), so
it sequences with GAP3; libgcc + the decoration-bridge layer + the DECC$SHR
whole-archive (via the vms-c65 shareable emit) are the toolchain/LINK side that
follow. **Owner routing:** conductor coordinates; the Alpha lane owns the
executive-facing syscall backend.
**Bead:** vms-da2c. **Blocks:** a runnable Alpha crt0 activation (the last
non-Alpha-boot rung of the crt0 join).

## What's already done (so this is the *only* remaining LINK/toolchain blocker)

The crt0-join **LINK ladder is complete on main** — real `alpha-dec-vms`
cc1-compiled port objects link, activate (PT_INTERP → IMGACT), bind cross-image
imports by vector index, fold globalvalues, re-bias data pointers (`.vms$rel`,
real-IMGACT-confirmed), and LINK.EXE now emits a real Alpha `.vms$sv` **shareable**
from `alpha-dec-vms` objects (vms-c65 / #742). The `alpha-dec-vms` cross-cc1 is
built and reproducible (`tools/cross-alpha-vms/`).

So a **real Alpha `DECC$SHR`** is now purely: *feed a set of `alpha-dec-vms`
C-RTL objects through LINK.EXE's shareable path.* The only missing input is those
objects — the **Alpha C-RTL archive**. That is what this doc scopes.

## The requirement (from the working x86_64 model)

`mk_decc_shr.sh` builds the x86_64/aarch64 `DECC$SHR` by whole-archiving a
**musl `libc.a` (1345 members) + libgcc.a** (soft-float/long-double/complex
builtins) and adding the OVMX `decc$`-decoration + the malloc/tprintf bridges.
`e_machine` is derived from the archive members. So the Alpha equivalent needs:

1. a `libc.a` of C-standard-library implementations **as `alpha-dec-vms` objects**, whose
2. system-call / arch layer routes to the **OVMX Alpha executive** (the same way
   the x86_64 musl syscalls reach the OVMX executive), plus
3. an `alpha-dec-vms` `libgcc.a` (buildable from the same cross-GCC with a full
   target build — `make all-target-libgcc`; the current `--without-headers`
   `all-gcc` build produced cc1 only).

**The blocker:** musl has **no Alpha port** at all (upstream deliberately —
Alpha is a dead arch). There is no drop-in `alpha-dec-vms` `libc.a`.

**Key enabling fact (measured):** the cross-cc1 compiles musl's **portable C**
to `alpha-dec-vms` cleanly (verified on strlen/memcpy/strcmp). musl's C source is
overwhelmingly arch-neutral; only `arch/<arch>/` is arch-specific (syscall stubs,
`atomic.h`, `setjmp`/`longjmp`, a handful of string asm, `bits/`). So the gap is
**not** "write a libc" — it's "write musl's `arch/alpha-dec-vms/` layer."

## Options

| # | Option | Effort | Parity w/ x86_64 | Notes |
|---|--------|--------|------------------|-------|
| A | **Port musl's arch layer to `alpha-dec-vms`** (a forked `arch/alpha-dec-vms/`: syscalls→OVMX Alpha executive, atomics, setjmp, minimal string asm; reuse all portable C) | **Medium** | **Full (one libc)** | Cross-cc1 already compiles the portable C. Bounded, well-understood arch-port. Couples the syscall layer to the OVMX **Alpha executive** (= the Alpha boot lane / GAP3 / vms-8954). |
| B | **OVMX-native minimal Alpha C-RTL** (implement only the `decc$` surface real programs import, portable C via cross-cc1 + a small arch/syscall layer; grow gap-driven like the `decc_crtl_map` extension) | **Low → grows** | Partial → breaks | Fastest to a first runnable demo; incomplete for arbitrary programs; accrues its own maintenance. Good **labeled bootstrap** if a demo is needed before A lands. |
| C | **newlib / PicoLibc for alpha** | Medium | **Breaks (two libcs)** | More portable/BSD-licensed, but no `alpha-dec-vms` config exists (embedded-oriented), and x86_64=musl / Alpha=newlib means divergent libc behavior to reconcile. |
| D | **glibc-alpha (Linux ABI) + ABI bridge** | High | N/A | The Alpha *runtime* lane uses `gcc-alpha-linux-gnu` (glibc, **Linux** ABI). Bridging Linux-ABI glibc into a **VMS-ABI** DECC$SHR is an ABI mismatch — wrong direction, heavy. Reject. |
| E | **Vendor DEC/VSI DECC$SHR** | — | — | Proprietary + clean-room forbidden (Rule 8). Reject. |

## Recommendation

> **DECIDED (operator, 2026-08-23): Option A.** The rationale below stands as the
> ruling's basis; B remains available only as a labeled bootstrap if a pre-GAP3
> demo is ever wanted.

**Option A (port musl's `arch/alpha-dec-vms/` layer), with Option B as a labeled
bootstrap** if a runnable demo is wanted before A completes.

Why A:
- **Preserves the one-libc-everywhere model** — x86_64/aarch64/Alpha all musl, one
  behavior, one maintenance surface. Consistent with do-it-like-VMS + how the
  x86_64 DECC$SHR already works.
- **The hard 90% (portable C) is already proven** to compile via the cross-cc1;
  the remaining work is the bounded `arch/` layer, not a libc.
- The whole-archive → LINK.EXE-shareable path (vms-c65) already exists, so once
  the `alpha-dec-vms` `libc.a` + `libgcc.a` exist, `mk_decc_shr.sh` produces the
  real Alpha DECC$SHR with **no new LINK work**.

The real operator call is scope/sequencing, not a wild card:
1. **Is A's musl-arch-port acceptable as the direction** (vs a native minimal
   C-RTL), given it maintains an upstream-dead arch in an OVMX musl fork?
2. **A's syscall layer targets the OVMX Alpha executive**, which is the **Alpha
   boot lane (GAP3 / vms-8954)** — so A and GAP3 are coupled and should be
   sequenced together (the C-RTL's `write`/`open`/... land on the executive the
   boot lane brings up). B (minimal, host-syscall or stubbed) could produce a
   *link-and-partially-run* demo sooner but not a real activation.

**Proposed sequencing:** land A's `arch/alpha-dec-vms/` musl layer alongside the
Alpha executive (GAP3) so the C-RTL's syscalls have a real backend; use B only if
a pre-GAP3 demo is explicitly wanted. Either way the LINK side (mine) is done —
the archive feeds straight through vms-c65's shareable emit.

## What I can do next (LINK/toolchain side, all unblocked)
- Build the `alpha-dec-vms` `libgcc.a` from the existing cross-GCC (extend
  `tools/cross-alpha-vms/` to `make all-target-libgcc`) — needed regardless of
  A/B, and purely mine.
- Once the arch layer (A or B) exists, run its objects through `mk_decc_shr.sh` +
  LINK.EXE and hand the conductor a real Alpha DECC$SHR for the joint-e2e re-drive.
