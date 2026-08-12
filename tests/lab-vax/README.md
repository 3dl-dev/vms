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
| NetBSD/vax | **9.4**, ISO `NetBSD-9.4-vax.iso` | current release; ships a bootable VAX ISO with sets |
| ISO checksum | SHA512 `735a8e8a…597010bf` | from the release's own `images/SHA512`; verified before every use |
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

## The assertion, and the negative control

`smoke` passes only when **both** hold: anita's `--run` (`uname -srm | grep -qx
'NetBSD 9.4 vax'`) returns 0 in-guest **and** the console transcript actually
contains a real `NetBSD 9.4 vax` line. Resting on anita's exit code alone would
trust one plumbing path; resting on the transcript alone would miss a boot that
never reached a shell.

`negctl` runs the identical path but demands the **wrong** arch
(`NetBSD 9.4 hppa`). The in-guest `grep -qx` fails, anita exits non-zero, the
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
