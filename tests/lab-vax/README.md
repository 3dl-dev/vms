# lab-vax -- NetBSD/vax on SIMH, containerized (`vms-0041`, epic `vms-8e8`)

The proving ground for the **OVMX/NetBSD SYSKRNL**. OVMX ships one runtime
*model* -- a real host kernel that exposes the VMS executive through `/dev/vms`
(Rule 9, `docs/runtime-target.md`). Today the only such kernel is Linux
(`vms.ko`), reached under `qemu-system-*`. **Linux has no VAX port and never
will**, so to make VAX a first-class runtime the design adds a second SYSKRNL,
**OVMX/NetBSD**, because NetBSD/vax is a current, actively-maintained port. See
`docs/design-ovmx-netbsd-syskrnl.md`.

There is **no `qemu-system-vax`** (§6 of that design). The VAX emulator of
record is **SIMH** -- the same emulator the OpenVMS VAX labs (`tests/lab/`,
`tests/lab-alpha/` is Alpha) already use. This directory is the NetBSD/vax
analogue of `tests/qemu/`: a containerized harness that boots NetBSD/vax under
SIMH non-interactively, runs `uname -srm`, and asserts the output is
`NetBSD <version> vax`. It is the base that P3 (libvmssys VAX backend) and P4
(OVMX/NetBSD-vax boots + executive test) build on.

> **This is BUILD/TEST tooling, not a runtime.** It builds and drives an
> emulator to *prove* the runtime model on VAX; it is never itself a runtime,
> and it does not weaken Rule 9. Nothing is installed on the host: SIMH is built
> in the container, anita + pexpect are pip-installed into a container venv, and
> NetBSD is downloaded into a mounted cache.

## What is pinned

| Thing | Pin | Why |
|---|---|---|
| NetBSD/vax | **10.1**, ISO `NetBSD-10.1-vax.iso` | current release; ships a bootable VAX ISO with sets. Same version the OVMX/NetBSD-vax **cross toolchain + syssrc** are pinned to (`tools/cross-vax/`, `tests/netbsd/netbsd_version.env`), so a cross-built elf32-vax kernel module is ABI-matched to this running kernel (P4-B, `vms-f78bb`) |
| ISO checksum | SHA512 `aa763aa2…9ac0f225` | from the release's own `images/SHA512`; verified before every use |
| SIMH | open-simh commit `2e0d51e9…` | **identical** to `tests/lab`'s `SIMH_REF` -- same emulator source as the OpenVMS/vax lab |
| Machine | SIMH **MicroVAX 3900 (KA655)** | the NetBSD/vax + OpenVMS/vax reference machine |
| anita | tag `v2_18` = commit `79c0a3f1…` | NetBSD's own install/boot driver; ISC-licensed |

`NETBSD_VERSION` is overridable, but **if you bump it you must update
`ISO_SHA512` in `entrypoint.sh`** from that release's `images/SHA512`. There is
no "latest".

## Reused verbatim, not reinvented

- **No DEC ROM is provisioned.** The task was to reuse whatever `tests/lab/`
  does to source `ka655x.bin`. `tests/lab/entrypoint.sh`'s `vax.ini` loads **no
  external ROM** -- the SIMH MicroVAX 3900 model carries its KA655 boot ROM
  compiled into the simulator binary. anita's generated `netbsd.ini` is the
  same: `set cpu <mem>m` / `set rq0 ra92` / `boot cpu`, no ROM load. So the
  faithful reuse is: **handle no DEC-copyrighted ROM binary at all.** There is
  none to handle for the VAX target.
- **SIMH build** is a byte-for-byte copy of `tests/lab/Dockerfile`'s builder
  stage (same `SIMH_REF`, same `make vax`, same provenance string).
- **Console automation** is anita, which "screen-scrapes sysinst over an
  emulated serial console" -- exactly the pexpect pattern the design doc §6 and
  `tests/lab/nodedrv.py` establish, but maintained by the NetBSD project for the
  vax/simh combination specifically. This is the deliberate answer to §6's
  flagged risk ("fully scripting sysinst on VAX is too fragile"): we do not
  hand-script the curses installer, we use the tool NetBSD's own CI uses.

## Console-automation choice (and why)

sysinst on a serial console is a curses app; matching its escape sequences by
hand is the fragile part §6 warns about. anita encapsulates it: for the `vax`
arch it auto-selects the `simh` VMM, boots the install ISO (`boot dua3`), drives
sysinst to completion, and leaves the installed system in `wd0.img`. For the
smoke test it boots that disk (`boot cpu` -> the RA92 system disk), logs in as
root, and runs a command via `--run`, exiting with that command's status. We
install a **minimal set list** (`kern-GENERIC,base,etc`) -- everything needed to
boot to a shell and answer `uname`, and nothing else, which cuts the (very slow)
VAX install time and the cached disk size substantially.

## Install-once, cache the disk

Installing NetBSD/vax on an emulated VAX is **slow** (a ~1980s CPU under
emulation -- tens of minutes). So the flow is split:

- **`install`** runs the slow sysinst once and produces `wd0.img` in the anita
  work directory under the mounted cache.
- **`smoke`** / **`negctl`** boot that cached `wd0.img` (minutes) and assert.

The cache lives under the repo's **`.boot-cache/`** convention (gitignored --
multi-MB disk images are never committed). Locally it defaults to
`.boot-cache/lab-vax/`; in the container it is the `/cache` volume.

**Regeneration recipe** (the disk is fully reproducible from pinned inputs):

```sh
rm -rf .boot-cache/lab-vax
tests/lab-vax/run-local.sh install      # re-downloads+verifies ISO, reinstalls
```

In CI the same `wd0.img` is keyed on the pinned NetBSD version + SIMH ref +
anita ref + the install recipe, via `actions/cache`; a cold cache runs the
install job to populate it, and the smoke job only ever boots.

## Run it locally

```sh
tests/lab-vax/run-local.sh build        # build the image
tests/lab-vax/run-local.sh install      # ~tens of minutes, once
tests/lab-vax/run-local.sh smoke        # boots cached disk, asserts uname; exit 0/1
tests/lab-vax/run-local.sh negctl       # negative control: must fail-then-pass
# or, cold: tests/lab-vax/run-local.sh all
```

Or drive the container directly:

```sh
docker build -f tests/lab-vax/Dockerfile -t ovmx-vax-lab tests/lab-vax
docker run --rm -v "$PWD/.boot-cache/lab-vax:/cache" ovmx-vax-lab install
docker run --rm -v "$PWD/.boot-cache/lab-vax:/cache" ovmx-vax-lab smoke
docker run --rm -v "$PWD/.boot-cache/lab-vax:/cache" ovmx-vax-lab negctl
```

## P4-B: `/dev/vms` live on NetBSD/vax (`vms-f78bb`)

The `uname` smoke above proves NetBSD/vax boots; **P4-B** proves the OVMX
executive itself runs on it. `run-devvms.sh` boots the SAME cached disk and
brings the `vms` pseudo-device up so `/dev/vms` opens and a version/ping ioctl
round-trips through the NetBSD transport seam (`kif_transport_netbsd.c`) — the
VAX analogue of the NetBSD/amd64 P2b proof (`tests/netbsd/run_p2b.sh`).

```sh
tests/lab-vax/run-devvms.sh            # build module+kernel, ensure disk+kernel, PING OK
tests/lab-vax/run-devvms.sh negctl     # teeth: skip modload -> PING must go RED
```

**Compile-into-kernel, not plain modload — the decision, and why it was forced.**
`vms-f78bb`'s done-condition (a real in-kernel `/dev/vms`) allows two build paths
(design-p4 §4.2, risk 2): load the driver as a `module(9)`, or compile it into a
custom NetBSD/vax kernel if the VAX port's loadable-module framework is too thin.
We tried modload first, as directed — and found the risk is **real**, in three
concrete layers exposed empirically on real NetBSD/vax under SIMH:

1. **vax GENERIC has no `options MODULAR`.** It is the *only* NetBSD port whose
   GENERIC omits it (amd64/alpha/i386/sparc64/… all enable it). On the stock disk
   `modctl` returns `ENOSYS` ("Function not implemented") — no module can load.
2. **The vax module-loader glue was never wired.** Even adding `options MODULAR`
   to a config fails to *link*: `arch/vax/vax/kobj_machdep.c` (`kobj_reloc`/
   `kobj_machdep`) has existed since 2018 but is not listed in `files.vax`, and
   `module_init_md()` (an empty per-arch hook every MODULAR port defines in
   `machdep.c`) is missing for vax. Both are undefined references at kernel link.
3. **The module needed `-fno-pic`.** A PIC/GOT build yields `R_VAX_GOT32`
   relocations the kernel's (GOT-less) `kobj` loader rejects
   (`Bad relocation … type=7 … unresolved rela`).

So this harness takes the **compile-into-kernel** fallback, in its least-invasive
form: `tools/cross-vax/build-vax-modular-kernel.sh` builds a `GENERIC + options
MODULAR` kernel (patching the two vax wiring gaps — OVMX build glue over open
NetBSD source, not VMS RE), installs it as `/netbsd`, and modloads the
**unchanged** cross-built OVMX module (built `-fno-pic`). The driver is *not*
statically linked in — one module source still serves both substrates; only the
host kernel is customized to carry the module framework the vax port never
shipped. `drive_devvms_vax.py` never fakes a pass: a `modload` failure is
reported with its exact kernel error.

Proven on real NetBSD/vax under SIMH:

```
[  11.25] vms: registered, char major 366
crw-rw-rw-  1 root  wheel  366, 0  /dev/vms
PROBE: /dev/vms unreachable (open rc=-1) -> honest failure, SS$_NOSUCHDEV; NOT faking success   (module absent, INV-6)
PROBE: PING OK -- ack=0x504b4f21 abi=1 substrate=NetBSD status=1
```

**Cross-built, not in-guest-built — the one difference from amd64 P2b.** The
amd64 P2b builds the module *inside* the guest with `bsd.kmodule.mk` (its disk
carries the `comp`+`syssrc` sets). On VAX that is impractical: the `syssrc` set
alone is ~450 MB and does not fit the emulated VAX system disk, and an in-guest
VAX compile is punishingly slow. So the module (and the userspace probe) are
**cross-built on the host** by `tools/cross-vax/build-devvms-vax.sh` against the
pinned NetBSD/vax kernel headers, then delivered into the guest on a **second
CD** (`rq2`). Because the lab and the cross toolchain are pinned to the **same**
NetBSD version (10.1), the cross-built module is ABI-matched to the running
kernel. `build-devvms-vax.sh` builds the module at `-O2` and asserts every OVMX
symbol is resolved (only real NetBSD KPIs left undefined), so `modload` has a
loadable object — unlike B1's per-PR `-r` *compile* gate, which permits
unresolved symbols.

**Single-user boot — securelevel, not incidental.** The proof boots the guest
**single-user** (`>>> B/R5:2 DUA0`, R5 = `RB_SINGLE`). This is required: on a
multiuser NetBSD, `init(8)` raises the kernel securelevel to 1, and
`secmodel_securelevel(9)` then denies `KAUTH_SYSTEM_MODULE`, so `modload` of an
out-of-tree module returns `EPERM` ("Operation not permitted"). At securelevel 0
(single-user, before init raises it) loading a module is permitted for root —
the standard way to load a test module on a secure BSD. It does not weaken the
proof: the module still serves a **real in-kernel** `/dev/vms`, and INV-6 holds
(the module-absent probe still fails honestly with `SS$_NOSUCHDEV`). anita only
boots multiuser, so the driver reuses anita's `start_simh()` (netbsd.ini + SIMH
spawn) and drives the `>>>` ROM itself to boot single-user.

**Reuses the cached disk; never reinstalls.** `run-devvms.sh` installs NetBSD/vax
only if the cache is cold (design-p4 §5). A one-time `install-kernel` session then
boots the GENERIC disk single-user and swaps in the MODULAR kernel as `/netbsd`
(SIMH cannot inject a kernel like `qemu -kernel`, so it must reach the disk),
keeping the original as `/netbsd.GENERIC`; this is marked and done once. The
`prove` `modload`/`mknod` writes are benign and non-persistent (the module is not
added to `rc.conf`). The custom kernel (4 MB) is a cached artifact so build.sh
runs only on a cold cache.

## P4-E: common event flags proven cross-process on NetBSD/vax (`vms-4e7`)

P4-B proves ONE ioctl round-trips through a live `/dev/vms`; that alone does not
rule out a per-process userspace fake answering it. **P4-E** closes that gap for
event flags: `run-eflag.sh` boots the SAME cached disk, cross-builds the
`vms.kmod` module (carrying `src/kernel-core/vms_eflag.c`, the identical source
the Linux `vms.ko` builds) plus the `vmseflag` guest tool
(`tests/netbsd/guest/vmseflag.c`), and proves a COMMON event flag set by one OS
process is observed by a genuinely DIFFERENT process — and that a blocked
waiter is WOKEN across a process boundary — because the flag lives in the
executive's KERNEL memory, not in either process. This is the VAX analogue of
the NetBSD/amd64 P2c proof (`tests/netbsd/run_p2c.sh`); scope is **event flags
only** — the other boot-required facilities (proctab/lnm/mbx/ast/access) are the
separate `vms-945e`.

```sh
tests/lab-vax/run-eflag.sh                # build everything, ensure disk+kernel, PROVE
tests/lab-vax/run-eflag.sh negctl-load     # teeth: skip modload -> whole proof must go RED
tests/lab-vax/run-eflag.sh negctl-set      # teeth: skip process A's SETEF -> cross-process SET assertion must go RED
```

**Why VAX specifically.** vax is ILP32 / non-IEEE-float / ELF32 — a width class
the amd64 (LP64) proof cannot exercise. A struct-layout or width bug in the
shared wire contract (`src/kernel-netbsd/vms_eflag_nb.h`) could compile clean
and pass on every 64-bit OVMX target and only misbehave here.

**Reuses P4-B's decisions unchanged**: compile-into-kernel (not plain modload —
vax GENERIC has no `options MODULAR`), cross-built + CD-delivered artifacts (not
in-guest build — the vax system disk cannot hold `comp`+`syssrc`), single-user
boot (securelevel 0, required for `modload` to be permitted). Own cached
MODULAR-kernel artifact (`eflag-artifacts/netbsd-OVMX`), independent of P4-B's
`devvms-artifacts/netbsd-OVMX` and P4-C's `boot-artifacts/netbsd-OVMX` — same
kernel config, separate cache entries, mirroring how those two already keep
independent caches.

**The cross-process proof (collapsed into single in-guest commands, same
loss-tolerant-transport technique `drive_netbsd_p2c.py` uses for the lossy
serial):** process A `$SETEF`s common flag 64; a DIFFERENT process B `$READEF`s
it and must see SET; a control flag (65) must read CLEAR; process C `$CLREF`s
64 and its previous-state must report `was-set`; process D re-reads 64 and must
see CLEAR. Then the payoff — a waiter process `$WAITFR`s on flag 66 and BLOCKS
in-kernel; a DIFFERENT process `$SETEF`s it; the blocked waiter must WAKE. The
built-in INV-6 negative control (module absent) asserts `vmseflag` fails
honestly with `SS$_NOSUCHDEV`, never fakes success.

**Fast per-PR complement**: `netbsd-vax-eflag-crosscompile` (CI) cross-compiles
+ links `vmseflag` for `vax--netbsdelf` with no SIMH boot — the guest-tool
analogue of the `libvmssys-netbsd-vax` per-PR library gates. The shared facility
source (`vms_eflag.c`) is already width-checked per-PR by B1
(`netbsd-vax-vms-crosscompile`); this SIMH proof is the nightly runtime
complement to both.

## The assertion, and the negative control

`smoke` passes only when **both** hold: anita's `--run` (`uname -srm | grep -qx
'NetBSD 10.1 vax'`) returns 0 in-guest **and** the console transcript actually
contains a real `NetBSD 10.1 vax` line. Resting on anita's exit code alone would
trust one plumbing path; resting on the transcript alone would miss a boot that
never reached a shell.

`negctl` runs the identical path but demands the **wrong** arch
(`NetBSD 10.1 hppa`). The in-guest `grep -qx` fails, anita exits non-zero, the
harness reports failure -- and `negctl` exits 0 **only because** the harness
correctly failed. This proves the gate can go red (Rule 7: the harness is a
test; a test that cannot fail is decoration). A second, equivalent negative
control is a forced timeout: `BOOT_TIMEOUT=1 ... smoke` -> the `timeout` fires
-> non-zero.

## Traps

- **Hard timeout on every SIMH run.** `entrypoint.sh` wraps both install and
  boot in `timeout --signal=KILL`. A hang fails; it never spins (a prior
  emulator proof here once ran unbounded for 1h43m).
- **No console-reconnect trap.** Unlike lab-alpha's AXPbox (which powers off if
  a console client disconnects), SIMH-VAX is driven over a single long-lived
  stdio console (anita's pexpect child) for the whole run. The console only
  closes mid-run when our own `timeout` kills anita -- the intended failure
  path. Do **not** add a TCP/port readiness probe; there is no telnet console
  here to probe.
- **Do not commit `wd0.img`.** It is multi-MB and gitignored under
  `.boot-cache/`. If you see it staged, something bypassed the ignore rule.
- **VAX RAM ceiling.** NetBSD/vax panics with too much RAM (a known SIMH-VAX +
  NetBSD issue at 512 MB); anita's default vax memory size stays safely below
  it. Do not raise `--memory-size` without checking.

## Shared harness contract: `vaxharness.py` (rd vms-cf5)

The **port** code on vax lands clean; the **harness** is the time-sink -- it
re-bites the same fragility class once per rung, and three separate agents
have each hand-rolled a fix for a variant of it inside `drive_boot_vax.py` /
`drive_eflag_vax.py`. `vaxharness.py` (+ its bash mirror `negctl_gate.sh`)
kills the class ONCE, mined from `~/projects/pcjs-vax`'s Target Adapter
Protocol (`docs/reference/target-adapter-protocol.md`, `tools/ehkaa-gate/
gate.py`): that gate never scrapes a console -- it runs one adapter command
and parses ONE line of JSON from stdout, target-agnostic. This module adapts
that shape to a SIMH/pexpect console.

**The three bugs it kills** (see the module docstring in `vaxharness.py` for
full detail and exact call-site provenance):

1. **anita's `expect()` crashes on its own TIMEOUT/EOF sentinel.** anita's
   vendored pexpect subclass unconditionally calls `self.match.group(0)`
   after every `expect()`, which raises `AttributeError` when the match IS
   the `pexpect.TIMEOUT`/`pexpect.EOF` sentinel (neither has `.group()`).
   `safe_expect(child, patterns, timeout)` never puts those sentinels in the
   pattern list (so pexpect raises them as exceptions instead, sidestepping
   anita's buggy hook entirely), catches `pexpect.TIMEOUT`/`pexpect.EOF`/any
   other exception, and **never raises** -- it always returns a
   `SafeExpectResult` (`kind` = `match`/`timeout`/`eof`/`error`).
2. **`child.before` can be `None`** after a failed `expect()`, crashing any
   `.count()`/`.strip()` call downstream. Every `SafeExpectResult.before`
   (and `.after`) is unconditionally a `str`, never `None` -- even if the
   attribute access itself raises.
3. **Negative-control exit-code contradictions.** A driver and its wrapper
   disagreeing about what "negctl satisfied" means (`run-eflag.sh` wanted a
   nonzero driver exit; one driver variant exited 0 logging "negctl ok").
   `negctl_gate(driver_exit_code, negctl)` is the ONE inversion rule: a
   driver's exit code is **never** mode-aware (0 = every positive assertion
   held, regardless of negctl); the WRAPPER applies `negctl_gate()` (or its
   bash mirror `negctl_gate.sh`'s `vaxharness_negctl_gate`) to invert that
   meaning for negctl mode. Both implementations are unit-tested against the
   identical table so they cannot silently diverge.

**The structured result contract.** `Proof`/`StepResult` turn a driver's
pass/fail decision into DATA -- one `{step, ok, marker_seen, detail}` per
proof step, reduced by `Proof.emit_result_line()` to exactly one JSON line on
stdout (mirroring the Target Adapter Protocol's "exactly one JSON object"
contract) -- instead of scattered `if seen.get(...)` checks through the
script.

**Self-test:** `test_vaxharness.py` (pytest, no SIMH/anita/container needed)
feeds `safe_expect()` fake children that raise `pexpect.TIMEOUT`,
`pexpect.EOF`, the exact anita `AttributeError`, and one whose `.before`
property itself raises, asserting the honest structured result comes back in
every case and nothing ever raises; plus the full `negctl_gate` truth table
in both Python and bash. Run: `pytest tests/lab-vax/test_vaxharness.py -v`.

**Adoption (follow-up, not this item).** `vaxharness.py` is a new,
standalone module -- it does not yet replace the ad hoc `_console_text()` /
scattered `except (pexpect.TIMEOUT, pexpect.EOF, Exception)` call sites in
the existing `drive_*.py` drivers. That retrofit is separate follow-up work,
sequenced after `vms-84fe` (mid-flight in `drive_boot_vax.py`) merges, and is
also the intended contract for `vms-945e`'s remaining facility drivers
(proctab/mbx/ast/access), so they adopt `safe_expect`/`Proof`/`negctl_gate`
from day one instead of re-deriving them.
