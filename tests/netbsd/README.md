# OVMX/NetBSD amd64 harness (Phase 2a)

rd `vms-7f7` · parent `vms-dd8` · epic `vms-8e8` · design `docs/design-ovmx-netbsd-syskrnl.md` §6

A **containerized** test harness that boots **NetBSD/amd64** under
`qemu-system-x86_64`, drives its serial console non-interactively, runs a smoke
command, and propagates the verdict as the process exit status. Concretely: it
boots NetBSD, runs `uname -srm`, asserts the output is exactly
`NetBSD 10.1 amd64`, and exits 0 on success / nonzero on any failure or timeout.

## Why this exists

Epic `vms-8e8` adds an **"OVMX/NetBSD" SYSKRNL** so OVMX can eventually run as a
real runtime on **VAX** — NetBSD/vax is a live port; Linux has none. Because
**there is no `qemu-system-vax`**, the entire NetBSD executive is de-risked first
on a *known* architecture — NetBSD/**amd64** under `qemu-system-x86_64` — before
the VAX arch port (design §6.1). This harness is that foundation. It is the
NetBSD-substrate sibling of `tests/qemu/` (which does the same for the Linux
`vms.ko` executive), and P1's `vms_kif` transport seam
(`src/libvmssys/kif_transport.h`) is what a future NetBSD transport plugs into.

- **P2b** builds on this: a `vms` pseudo-device (`cdevsw` `/dev/vms`) statically
  linked into a custom NetBSD kernel, booted by this same harness.
- **P2c** builds on that: an executive-facility test against the real `/dev/vms`
  on NetBSD — the NetBSD analogue of the `tests/qemu/` executive assertions.

Because P2b/P2c need a place to build and load a driver, the harness produces a
**real, complete, cached NetBSD disk image**, not a throwaway installer ramdisk.

## This is tooling, not a runtime

Booting NetBSD in QEMU to **test** it is exactly what `tests/qemu/` does for
Linux; it does **not** make Docker or QEMU an "OVMX runtime" (CLAUDE.md Rule 9 /
`docs/runtime-target.md`). NetBSD-as-a-real-runtime is the *product goal* of epic
`vms-8e8`. The Rule 9 gate (`tests/integration/test_runtime_target.sh`) only
inspects `src/**` C/H files, so nothing here trips it.

## Approach chosen: anita, and why

Two realistic ways to get a bootable, console-scriptable NetBSD/amd64 under QEMU
were evaluated:

| Option | Verdict |
|--------|---------|
| **anita** (Automated NetBSD Installation and Testing) — the official NetBSD tool that scripts `sysinst` over an emulated serial console and produces a real installed disk image. | **CHOSEN.** |
| **Prebuilt installer image booted to a single-user shell** — faster (no install) but (a) the NetBSD install ramdisk is not guaranteed to carry `uname`, and (b) it is a transient system with nowhere for P2b to build/load the `vms` driver. |  Rejected. |

**anita wins on the two axes that matter here — reproducibility and being the
right *foundation*:**

1. It is the **canonical** NetBSD tool for exactly this (drives `sysinst` over
   the serial console via `pexpect`), so the fragile part — automating a
   curses installer on a serial line — is code that NetBSD itself maintains,
   not something re-invented here.
2. It yields a **real, complete, reusable** NetBSD install — the substrate P2b
   (build/load the `vms` pseudo-device) and P2c (executive test) require.
3. The slow install is paid **once**: the disk image is **cached** and every
   later run just boots it. This is the standard trade the design (§6, "cache
   the resulting disk image") calls for.

anita fetches the **serial-console** boot ISO (`installation/cdrom/boot-com.iso`,
not the VGA `boot.iso`) automatically, which is why the whole flow works over
`-nographic` serial with no VGA console.

### Reproducibility / pinning

Everything is pinned in `netbsd_version.env` — never "latest":

- **NetBSD 10.1 / amd64**, from the official CDN `https://cdn.netbsd.org`.
- **`boot-com.iso` SHA512** is pinned and **verified on every run** (including
  cached ones) against the CDN's published `SHA512` manifest — anita itself does
  no checksum verification, so `drive_netbsd.py` is the integrity gate.
- **anita 2.18** is fetched from gson.org (it is *not* the unrelated PyPI package
  named "anita") and its tarball **SHA256 is verified** in the Docker build.

Only a minimal set list (`kern-GENERIC modules base etc`) is installed — the
smallest system that boots and answers `uname` — to keep both the local proof and
the cold-cache CI install affordable. P2b can widen the sets (e.g. add `comp` +
`syssrc`) when it needs to build a driver.

## Files

| File | Role |
|------|------|
| `Dockerfile` | Carries qemu-system-x86_64 + anita + serial tooling. All deps containerized; nothing on the host. Sibling of `tests/qemu/Dockerfile`. |
| `run_tests.sh` | Container entrypoint. Wraps the driver in a **hard `timeout`** and reports the verdict. Sibling of `tests/qemu/run_tests.sh`. |
| `drive_netbsd.py` | The driver: install (cache-aware) → verify ISO checksum → boot (snapshot overlay) → console login → `uname -srm` assertion. Owns the qemu console like `tests/qemu/inject_and_run.sh` owns its boot. |
| `netbsd_version.env` | The pinned release, checksums, and anita version. |

## Running it locally

```bash
# from the repo root
docker build -f tests/netbsd/Dockerfile -t ovmx-netbsd-ktest .

# --device /dev/kvm is optional; the harness auto-detects KVM vs TCG.
# The cache volume persists the installed image so later runs skip the install.
mkdir -p .netbsd-cache
docker run --rm -v "$PWD/.netbsd-cache:/cache" --device /dev/kvm ovmx-netbsd-ktest
echo "exit: $?"        # 0 = NetBSD booted and uname asserted
```

### Timeouts (a hung boot FAILS, it does not hang)

- `run_tests.sh` wraps the whole run in `timeout` (`HARNESS_TIMEOUT`, default
  3600s) — the hard outer cap on every qemu invocation.
- `drive_netbsd.py` additionally sets a tighter **internal boot deadline**
  (`NETBSD_BOOT_DEADLINE`, default 900s) on the pexpect console, so a boot that
  never reaches `login:` raises a timeout and the harness exits nonzero.
- On timeout the qemu child is reaped (SIGTERM handler + `finally`).

### Negative controls (the harness can actually FAIL)

```bash
# Wrong expected arch -> the uname assertion must go RED (reuses the cached image)
docker run --rm -v "$PWD/.netbsd-cache:/cache" -e EXPECT_ARCH=sparc64 ovmx-netbsd-ktest
echo "exit: $?"        # nonzero; "FAIL: uname -srm did not print 'NetBSD 10.1 sparc64'"

# Forced boot timeout -> the timeout path must go RED
docker run --rm -v "$PWD/.netbsd-cache:/cache" -e FORCE_TIMEOUT=1 ovmx-netbsd-ktest
echo "exit: $?"        # nonzero via the boot-deadline timeout
```

## CI

Job **`NetBSD/amd64 Executive Harness (QEMU)`** in `.github/workflows/ci.yml`,
gated exactly like the expensive Linux executive jobs: it runs on
push/merge_group/schedule and on PRs **only** when `tests/netbsd/**` (or the
workflow) changes — never on an unrelated PR, given the boot cost. The installed
image is cached via `actions/cache` (keyed on the pinned release + harness
sources). The negative control (wrong-arch assertion) is a step in the same job,
reusing the warm cache.

**Runner-capability caveat (honest):** GitHub-hosted runners have no `/dev/kvm`,
so the guest runs under **TCG** (slower, but a *real* boot — never faked). The
install is cached, so only a **cold** cache pays the full TCG install cost, which
is the run at risk of the 120-minute job cap. If that proves too tight on the
stock runner, this job wants a **KVM-capable** (self-hosted or larger) runner;
the harness itself is complete and real either way.

---

# Phase 2b — a real in-kernel `/dev/vms` on NetBSD

rd `vms-bfe` · parent `vms-dd8` · epic `vms-8e8`

P2b builds directly on the P2a foundation above. Where P2a boots NetBSD and
asserts `uname`, **P2b builds and loads a REAL in-kernel `/dev/vms`** and proves
one ioctl end to end — the NetBSD-substrate analogue of what `tests/qemu/` does
for the Linux `vms.ko` executive.

## What it proves

On the installed, cached NetBSD/amd64 guest (a **separate** cache from P2a — see
below), `drive_netbsd_p2b.py`:

1. builds the **OVMX/NetBSD `vms` pseudo-device** (`src/kernel-netbsd/`) in-guest
   with the NetBSD kernel-module toolchain — a `cdevsw` character pseudo-device
   built as a **loadable `module(9)`** (`vms.kmod`, via `bsd.kmodule.mk`);
2. builds the userspace **probe** (`guest/vmsprobe.c`), which reaches `/dev/vms`
   **through the transport seam** `src/libvmssys/kif_transport_netbsd.c` — the
   NetBSD leaf of the P1 `kif_transport.h` contract;
3. **INV-6 negative control:** runs the probe with the module **not loaded** —
   `/dev/vms` does not exist, so the probe must **fail honestly**
   (`SS$_NOSUCHDEV`) and NEVER fake per-process success (CLAUDE.md Rule 9 / INV-6);
4. `modload`s the module, `mknod`s `/dev/vms` with the dynamically-assigned char
   major (read back from `dmesg`), and runs the probe — the **version/ping ioctl**
   (`VMS_IOCTL_PING`, the shared `/dev/vms` contract in `src/kernel-netbsd/vms_ping.h`)
   must round-trip and the probe must print `PROBE: PING OK`;
5. `modunload`s and halts.

The ping wire contract (`vms_ping.h`) is **identical across substrates**: the same
magic space (`'V'`) and, for a struct this small, the same `_IOWR` request number
on BSD and Linux. It lives in its own portable header rather than in the Linux
`src/kernel/vms_ioctl.h` (which is written for the Linux kernel and would not
compile in a NetBSD kernel TU) — so **the Linux build is untouched** by P2b.

## Why a separate cache / set list

The kernel-module build needs the NetBSD toolchain **and** the kernel sources, so
P2b installs two extra sets on top of P2a's minimal list — **`comp`** (compiler +
`/usr/share/mk`) and **`syssrc`** (`/usr/src/sys`, required by an out-of-tree
module's `-isystem $S`). That makes the P2b installed image different from P2a's,
so it uses its own work/cache dir (`/cache/anita-netbsd-p2b-*`, cache path
`.netbsd-cache-p2b`). This is exactly the widening the P2a section anticipated.

## Running it locally

```bash
docker build -f tests/netbsd/Dockerfile -t ovmx-netbsd-ktest .

mkdir -p .netbsd-cache-p2b
docker run --rm -v "$PWD/.netbsd-cache-p2b:/cache" --device /dev/kvm \
    --entrypoint /netbsd/run_p2b.sh ovmx-netbsd-ktest
echo "exit: $?"        # 0 = module built+loaded, ping asserted, INV-6 negctl OK

# NEGATIVE CONTROL: skip the modload -> the PING OK assertion must go RED
docker run --rm -v "$PWD/.netbsd-cache-p2b:/cache" \
    -e P2B_SKIP_LOAD=1 --entrypoint /netbsd/run_p2b.sh ovmx-netbsd-ktest
echo "exit: $?"        # nonzero: "the version/ping ioctl did not round-trip"
```

Same hard-`timeout` discipline as P2a (`run_p2b.sh`, `HARNESS_TIMEOUT`, default
5400s for the larger cold install), plus per-command pexpect deadlines in the
driver.

## Files (P2b)

| File | Role |
|------|------|
| `src/kernel-netbsd/vms_netbsd.c` | The `vms` cdevsw pseudo-device (loadable `module(9)`); answers `VMS_IOCTL_PING`. |
| `src/kernel-netbsd/vms_ping.h` | Shared `/dev/vms` version/ping wire contract; portable to NetBSD kernel + userspace. |
| `src/kernel-netbsd/Makefile` | Out-of-tree `bsd.kmodule.mk` build → `vms.kmod`. |
| `src/libvmssys/kif_transport_netbsd.c` | NetBSD leaf of the P1 transport seam; **not** in the Linux CMake build. |
| `tests/netbsd/guest/vmsprobe.c` | Userspace probe; reaches `/dev/vms` through the seam, asserts the ping. |
| `tests/netbsd/drive_netbsd_p2b.py` | P2b driver: build ISO → boot → build → INV-6 negctl → load → ping. |
| `tests/netbsd/run_p2b.sh` | P2b container entrypoint (hard `timeout`). Select via `--entrypoint`. |

## CI (P2b)

Job **`NetBSD/amd64 vms Pseudo-Device (QEMU)`** in `.github/workflows/ci.yml`,
gated like the P2a job (push/merge_group/schedule always; on PRs only when
`tests/netbsd/**` **or** the P2b src — `src/kernel-netbsd/**`, the transport seam
— changes). It asserts the INV-6 honest-failure line **and** `PROBE: PING OK`, and
a second step runs with `P2B_SKIP_LOAD=1` to prove the PING assertion has teeth
(must go red for the right reason). The same TCG/`/dev/kvm` runner caveat as P2a
applies, more so on a cold cache (it also fetches `comp` + `syssrc`).
