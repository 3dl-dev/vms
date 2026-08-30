# Getting Started with OVMX

This is the canonical newcomer path: build the bootable image, boot it under
QEMU, log in, and run DCL — one command gets you all the way to a `$` prompt.

OVMX has exactly one runtime: the real-kernel / QEMU path, where `vms.ko`
provides the VMS executive and userspace reaches it through `/dev/vms`
(CLAUDE.md Rule 9). There is no Docker-based way to *run* OVMX; Docker is used
only to *build* the image and, in the blessed path below, to host the QEMU
process so you don't need QEMU installed on your machine.

## What you need

- **Docker** (or Podman) able to run a Linux container. The build compiles the
  whole toolchain — a from-source Linux kernel, `vms.ko`/`vmsfs.ko`, the
  VMS-native `LINK.EXE` image graph, a static-musl userland, and QEMU — all
  inside containers. **No compiler, kernel headers, or QEMU need to be
  installed on your host.**
- A few GB of free disk and ~25-30 minutes for a cold build (cached after
  that).

`docker buildx` is **not** required for the blessed path — it uses plain
`docker build -t`, which works on a stock Docker Engine install.

## Quick start (one command)

```bash
git clone <this-repo-url> vms
cd vms
./boot.sh
```

`boot.sh` does everything:

1. Builds the `ovmx-boot` image if it isn't already built (or if the source
   tree changed since the last build).
2. Creates a persistent 64 MB system disk under `dist/sysdisk.img` on first
   run (the disk is a host-mounted directory, so state survives across runs).
3. Boots the image under QEMU. On a **blank** disk, the first boot installs
   OVMX onto it (INITIALIZE + system seed) and then continues straight into a
   login prompt — one continuous QEMU session. Later runs reuse the installed
   disk and boot straight to login.

QEMU runs *inside* the container, so nothing but Docker is needed on the host.

When it stops at:

```
Username:
```

log in as `SYSTEM` / `MANAGER` for a full-privilege DCL session (or `GUEST` /
`GUEST` for a restricted one). A correct login prints `Welcome to OVMX` and
drops you at a DCL `$` prompt.

**To exit QEMU:** press `Ctrl-A` then `X`.

### Useful `boot.sh` flags

| Flag | Effect |
|------|--------|
| `./boot.sh` | Build if needed, boot the installed disk (installs on first run). |
| `--clean` | Delete the system disk and reinstall from scratch. |
| `--rebuild` | Force a `--no-cache` Docker image rebuild, then boot. |
| `--distrib` | Boot the pre-installed distribution disk (`dist/ovmx-distrib.img`) — seeded from the mastered image, no self-install. |
| `--disk PATH` | Boot a specific disk image instead of `dist/sysdisk.img`. |
| `--flags R5,R6` | Conversational boot: `--flags 0,1` halts at `SYSBOOT>` before the executive attaches. |
| `--help` | Show all flags. |

Guest RAM defaults to 512M; override with `MEMORY=1G ./boot.sh`.

## Logging in

Two accounts can authenticate on a stock install. Every other row in
`SYSUAF.DAT` ships with no password hash and is refused at login by design.

| Username | Password | Privileges |
|----------|----------|------------|
| `SYSTEM` | `MANAGER` | `ALL` |
| `GUEST`  | `GUEST`   | `TMPMBX` |

```
Username: SYSTEM
Password: MANAGER

    Welcome to OVMX

$
```

## Running DCL

You now have a real DCL session over the live executive. A few commands to
confirm things work (see [`docs/dcl-commands.md`](dcl-commands.md) for the full
reference):

```dcl
$ SHOW TIME
$ SHOW SYSTEM
$ SHOW DEFAULT
$ DIRECTORY
$ SHOW USERS
$ HELP
```

`SHOW TIME` returns the actual current date and time, proving `DCL.EXE` and its
shareable images loaded and ran from the system disk's `SYS$LIBRARY`.

## The lower-level path: `run-qemu.sh` (and how it differs from `boot.sh`)

There are two ways to build and boot, and they are **not** interchangeable —
they consume different build outputs and run QEMU in different places. This is
the single most common point of confusion, so it is spelled out here.

| | `boot.sh` (blessed) | `run-qemu.sh` (lower-level) |
|---|---|---|
| Build command | `docker build -f distro/Dockerfile.bootable -t ovmx-boot .` (done for you by `boot.sh`) | `docker build -f distro/Dockerfile.bootable -o dist .` (**needs `buildx`**) |
| Build output | A tagged image with QEMU + boot artifacts *inside* it | The kernel + initramfs *extracted* onto the host under `dist/` |
| Where QEMU runs | **Inside the container** — no host QEMU needed | **On the host** — needs `qemu-system-x86` (or `-aarch64`) installed |
| System disk | Persistent `dist/sysdisk.img`, self-installed on first boot | None by default (diskless initramfs boot); set `DISK=...` to attach one |
| Best for | Getting a working, persistent system fast | CI, custom disks, and explicit networking control |

The lower-level path, as shown in the top-level `README.md`:

```bash
# Needs docker buildx (extracts /boot artifacts into dist/)
docker build -f distro/Dockerfile.bootable -o dist .

# Needs a host QEMU install
./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz
```

`run-qemu.sh` is what CI uses and is the right entry point when you need to
attach a specific disk (`DISK=path ./distro/boot/run-qemu.sh ...`) or control
guest networking (`OVMX_NET_MODE`, `OVMX_NET_HOSTFWD`, and friends — see the
header of `distro/boot/run-qemu.sh`). It expects a kernel and initramfs already
extracted onto the host; if you built with plain `docker build -t` (as
`boot.sh` does), those files are inside the tagged image, not on your host, so
use `boot.sh` or a direct `docker run` instead.

> If you built with `-t` and try to run `run-qemu.sh dist/vmlinuz ...`, it will
> fail with `kernel not found` — that just means you used the `boot.sh` build
> output with the `run-qemu.sh` boot path. Pick one column of the table above
> and stay in it.

## Next steps

- **Install a product kit** (`PRODUCT INSTALL`) onto a target volume:
  [`docs/install-guide.md`](install-guide.md).
- **Upgrade an installed system** in place, preserving site config and user
  data: [`docs/upgrade-guide.md`](upgrade-guide.md).
- **Build the libraries, DCL, and tests** natively (not the bootable image):
  [`docs/building.md`](building.md).
- **Build and boot the Alpha and VAX images:**
  [`docs/building-multiarch.md`](building-multiarch.md).
- **Configure TCP/IP:** [`docs/tcpip-configuration-guide.md`](tcpip-configuration-guide.md).
- **System architecture and boot sequence:**
  [`docs/architecture.md`](architecture.md).
