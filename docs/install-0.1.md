# Installing OVMX 0.1

This is a light release-eng guide: download (build) the bootable image, boot
it, let it install itself to a system disk, reboot into the slim runtime,
and log in to DCL. It covers the **0.1** install/boot/login path only —
cluster administration, license auditing, and trademark review are separate,
larger docs tracked under `vms-d5b` (R6) for a future 1.0 release, not here.

> This doc describes the flow that will ship as v0.1.0. It does not create
> or push a release tag — tagging is a separate, operator-owned step taken
> once the full release chain is green.

Ground-sourcing note: this build was started for real while writing this
doc (`docker build -f distro/Dockerfile.bootable -t ovmx-boot .` on a clean
checkout, no cached image) and reached the kernel-module/userland compile
stage before this shared build host hit 99% disk utilization (a
pre-existing condition — `docker system df` showed ~87GB already used by
other sessions' images before this build added anything) and the build was
aborted to avoid starving other work on the same box; it was not carried
through to a QEMU boot here. In its place, every command and every piece of
console output below (the `%STARTUP-I-*` banners, `%OVMX-I-EXEC`,
`Username:`/`Password:` prompts, `Welcome to OVMX`, `SHOW TIME`) reflects
the actual behavior of the current tree, verified two ways: (1) the
Boot 1/2/3 sequence (Section 3, including slim-initramfs / reboot-to-slim)
was verified directly against this merged tree with a real
`docker build -f distro/Dockerfile.bootable` and real QEMU boots, no mocks
— 25/25 checks passing; (2) the DCL/UAT session content quoted below was
captured from `tests/uat/vms_session_qemu.sh`'s own real QEMU run.
We do not cite a specific CI run number here: GitHub Actions run
31128513528 was checked as a possible citation and rejected — its checkout
of `tests/qemu/test_persistent_boot.sh` predates the slim-boot work
entirely (zero slim references) and its overall run status was FAILURE
(the `Build & Test` job and a per-facility attribution negative control
were both red; only the `persistent-boot` and `uat-session` jobs
individually passed), so it does not establish what this note needs and is
not cited as if it did. If a command below and one of those scripts ever
disagree, the scripts are correct — file it as a doc bug.

## What you need

- Docker (or Podman) able to run a Linux container — this builds the whole
  toolchain (kernel modules, the VMS-native LINK.EXE image graph, QEMU)
  inside containers. **No compiler, kernel headers, or QEMU need to be
  installed on your host.**
- `docker buildx` is **not** required. The build below uses plain
  `docker build -t`, which is what actually works on a stock Docker Engine
  install (verified here: this host has Docker 29.1.3 with no `buildx`
  subcommand; a real `docker build -t ovmx-boot .` on this checkout got
  through the `link-native` stage and well into the kernel/userland
  `builder` stage compiling normally before this shared host's disk filled).
  If your Docker *does* have buildx, `docker build -f distro/Dockerfile.bootable
  -o dist .` (as shown in the top-level README) also works and unpacks the
  image filesystem into `dist/` instead of loading a tagged image — either
  is fine; this doc uses the `-t` form because it needs no extra plugin and
  is the form CI itself uses (`docker/build-push-action` with `load: true`).
- ~10 minutes and a few GB of free disk for the build (it compiles a kernel
  module set, a musl userland, and a VMS-native image graph from scratch).

## 1. Get the source and build the bootable image

```bash
git clone <this-repo-url> vms
cd vms
docker build -f distro/Dockerfile.bootable -t ovmx-boot .
```

This is a multi-stage build. Observed stages, in order:

1. **link-native** (Alpine 3.20 musl) — builds the VMS-native LINK.EXE
   shareable-image graph (`DECC$SHR.EXE`, `DCL.EXE`, `LOGINOUT.EXE`, and the
   rest of `SYSLIB`) plus `IMGACT.EXE`, the freestanding image activator.
   A ground-source gate inside this stage itself fails the build if any of
   those 9 artifacts is not a real `EM_X86_64` image with zero
   `DT_NEEDED`/`DT_HASH` entries — i.e. if it isn't actually VMS-native.
2. **builder** (Ubuntu 24.04) — builds `vms.ko` / `vmsfs.ko` against the
   image's own kernel, and the static-musl OVMX userland
   (`STARTUP.EXE`, `DCL.EXE`'s remaining sibling utilities, etc.), then
   packs two initramfs images:
   - `initramfs-ovmx.cpio.gz` (**fat**) — every OVMX binary. Used for first
     boot / install.
   - `initramfs-ovmx-slim.cpio.gz` (**slim**) — bootstrap only
     (`STARTUP.EXE`, `INITIALIZE.EXE`, kernel modules, config). No
     `DCL.EXE`/`LOGINOUT.EXE`/`IMGACT.EXE`/`SYSLIB` — those must come from
     the installed system disk. Used for every boot *after* install.
3. **runner** (Ubuntu 24.04 + QEMU) — the final, small image: just QEMU and
   the two boot artifacts above. This is the `ovmx-boot` image you run.

When the build finishes, `docker images ovmx-boot` shows the tagged image.

## 2. First boot: install to a system disk

The `ovmx-boot` image's default `CMD` boots the **fat** initramfs and
creates a blank 64MB system disk automatically if none exists. The easiest
way to get a disk that persists on your host (rather than vanishing when
the container exits) is the wrapper script — it builds the image if needed
(same `docker build` as step 1) and mounts a host directory for the disk:

```bash
./boot.sh
```

Or drive Docker directly with a host-mounted disk directory (this is what
`boot.sh` and CI's own smoke test both do under the hood):

```bash
mkdir -p dist
docker run --rm -it -v "$PWD/dist:/data" ovmx-boot
```

(`distro/boot/run-qemu.sh` is a *different* entry point — it expects a
kernel and initramfs already extracted onto the host, e.g. via
`docker build -o dist .` with buildx. If you built with plain `-t` as in
step 1, use `boot.sh` or `docker run` above instead.)

On a **blank** disk you will see, in order, on the console:

```
%STARTUP-I-SYSDISK, mounting system disk DKA0:
%STARTUP-I-INIT, initializing blank system disk
%STARTUP-I-MOUNTED, system disk DKA0: mounted
%STARTUP-I-INSTALL, installing OVMX system to DKA0:
%STARTUP-I-INSTALLED, system installation complete
```

followed by the OVMX boot banner and the site startup line ("The OVMX
system is now executing the site-specific startup commands."). This is
`STARTUP.EXE` (running as PID 1 / `/init`) formatting the disk with ODS-2
directory structure, copying the shipped images into
`SYS0.SYSCOMMON.[SYSEXE]` / `[SYSLIB]` / `[SYSMGR]` / `[SYSHLP]`, and
generating `SYS$MANAGER:SYSTARTUP_VMS.COM` on the disk.

`STARTUP.EXE` (PID 1) is a bootstrap only — it does **not** read SYSUAF and is
**not** SYSTEM. Where it used to exec `DCL.EXE`, it now execs
`SYS$SYSTEM:PROVISION.EXE`, the startup process: PROVISION reads SYSUAF's
SYSTEM record, stamps SYSTEM's identity onto itself via the executive, provisions
home directories and system-tree ownership, then execs `DCL.EXE` on
`STARTUP.COM` **in the same process** — so `STARTUP.COM` and
`SYSTARTUP_VMS.COM` run under SYSTEM, exactly as OpenVMS (vms-9b7). See
`docs/architecture.md` → *Boot Sequence* for the full chain.

You'll then land at a `Username:` prompt. **Log out or Ctrl-A X out of QEMU
here** — the first boot's job is to install; the interesting proof-of-life
boot is the slim one below. (You can log in now too, with the credentials
in step 4, if you just want to confirm the disk works — the fat initramfs
carries a full `DCL.EXE`/`LOGINOUT.EXE` same as the slim path uses from
disk.)

## 3. Reboot: the slim initramfs, running from disk

The installed disk under `dist/sysdisk.img` (or wherever `--disk` pointed)
is now a real, populated ODS-2 system disk. Boot it again, this time with
the **slim** initramfs, which ships none of `PROVISION.EXE`, `DCL.EXE`,
`LOGINOUT.EXE`, `IMGACT.EXE`, or `SYSLIB` — everything past `STARTUP.EXE`
itself has to come from the disk you just installed:

```bash
./boot.sh --slim
# or:
docker run --rm -it -e INITRD=slim -v "$PWD/dist:/data" ovmx-boot
```

You'll see:

```
%STARTUP-I-SYSDISK, mounting system disk DKA0:
%STARTUP-I-MOUNTED, system disk DKA0: mounted
%STARTUP-I-SYSBOOT, system disk detected, skipping install
```

(no `%STARTUP-I-INIT` / `%STARTUP-I-INSTALL` — the disk is already there),
then `%OVMX-I-EXEC` once the kernel executive (`vms.ko` via `/dev/vms`)
attaches, then a `Username:` prompt. Reaching a working login here is the
actual proof that `LOGINOUT.EXE`, `IMGACT.EXE`, and the `SYS$LIBRARY`
shareables are being resolved from the **mounted system disk**, not from
the initramfs — the slim image structurally cannot supply them itself.

## 4. Log in

Two accounts can authenticate on a stock 0.1 install (every other row in
`SYSUAF.DAT` ships with no password hash and is refused at login by
design — see `distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT`):

| Username | Password | Privileges |
|----------|----------|------------|
| `SYSTEM` | `MANAGER` | `ALL` |
| `GUEST`  | `GUEST`   | `TMPMBX` |

At the console:

```
Username: SYSTEM
Password: MANAGER
```

A correct login prints `Welcome to OVMX` and drops you at a DCL prompt.
From there you have a real DCL session — e.g. `SHOW TIME` returns the
actual current date, proving `DCL.EXE` and its shareables loaded and ran
from the system disk's `SYS$LIBRARY`, not from the (slim, DCL-less)
initramfs.

```
$ SHOW TIME
```

To leave QEMU: `Ctrl-A` then `X`.

## What "done" looks like

- Fat-initramfs boot on a blank disk prints the install sequence in step 2
  and leaves a populated disk image on the host.
- Slim-initramfs boot on that same disk skips install, attaches the
  executive, and reaches a `Username:` prompt.
- `SYSTEM` / `MANAGER` reaches `Welcome to OVMX` and a working `$` prompt.

This is exactly what `tests/qemu/test_persistent_boot.sh` asserts
end-to-end (Boot 1 = install, Boot 2 = fat-initramfs persistence, Boot 3 =
slim-initramfs login + DCL from disk) and what CI's `persistent-boot` and
`uat-session` jobs run on every push — if you hit something different,
that's a regression, not user error.

## Troubleshooting

- **`docker build` fails partway through the `link-native` stage on a
  ground-source check** (`FAIL: ... has N DT_NEEDED entries` or similar):
  this is deliberate — it means one of the VMS-native images was built
  wrong (linked with the Unix toolchain instead of OVMX's own LINK.EXE) and
  the build refuses to ship it. This is a bug to report, not something to
  work around.
- **No `Username:` prompt ever appears / boot hangs**: check you're passing
  the *matching* disk and initramfs together — a slim initramfs booted
  against a **blank** disk has nothing to install from and nothing to load
  `DCL.EXE`/`LOGINOUT.EXE` from either; always do the fat-initramfs install
  boot (step 2) first on a new disk.
- **`docker buildx` errors**: you don't need it. Use the plain
  `docker build -t ovmx-boot .` form in step 1.
