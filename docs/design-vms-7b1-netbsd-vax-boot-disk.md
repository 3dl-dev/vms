# vms-7b1 — Bootable OVMX/NetBSD-vax system disk, ovmx_init as PID 1

*Parent `vms-c99`; epic `vms-8e8` (OVMX/NetBSD SYSKRNL). Builds on `vms-f2e`
(ovmx_init NetBSD boot seam), `vms-5d1` (STARTUP/LOGINOUT/DCL link+activate on
netbsd-vax), `vms-544d` (ODS-2 mount+read on vax), `vms-f78bb` (/dev/vms on vax).
The capstone (boot to a DCL prompt) is `vms-d59`, out of scope here.*

## What this delivers

A nightly SIMH job (`netbsd-vax-boot`, `tests/lab-vax/run-boot.sh`) that
**assembles** a bootable OVMX/NetBSD-vax system disk from the OVMX build and
boots it **unattended** under SIMH with `ovmx_init` (STARTUP.EXE) as **PID 1
(init)**, asserting the boot milestone:

```
kernel up
  -> vms.kmod loaded, /dev/vms live      (%OVMX-I-EXEC, VMS executive attached on /dev/vms)
  -> OpenVMX product banner emitted        (OpenVMX V0.x - OpenVMS-compatible)
  -> vmsfs.kmod loaded, OVMX ODS-2 system disk (DKA0:) mounted
                                           (%OVMX-I-MOUNTED, system disk DKA0: mounted)
```

It wires together what the two sibling proofs establish in isolation —
`run-devvms.sh` (/dev/vms PING) and `run-vmsfs.sh` (ODS-2 mount+read) — but under
`ovmx_init` running as real PID 1 and driving its own NetBSD boot seam
(`src/ovmx_init/ovmx_boot_netbsd.c`). No `#ifdef` fork of the boot logic:
`ovmx_init.c` is the same source the Linux runtime boots (INV-DRIFT).

## The assembly

`run-boot.sh` reuses the entire proven vax substrate and adds STARTUP.EXE:

1. **Cross-build** (ovmx-cross-vax): `STARTUP.EXE` (ovmx_init, a dynamic
   elf32-vax image activated by NetBSD `ld.elf_so` per Decision A, vms-42d),
   plus the loadable `vms.kmod` and `vmsfs.kmod`.
2. **MODULAR kernel** (`netbsd-OVMX`) — reused from the sibling artifact/cache;
   vax GENERIC omits `options MODULAR`.
3. **ODS-2 volume** — `tests/qemu/mkimage_vmsfs.c` (the same mastered volume the
   vmsfs proof reads).
4. **Isolated disk copy.** The devvms/vmsfs proofs boot the *shared* `wd0.img`
   and rely on NetBSD's own `init`. This proof **replaces** `/sbin/init` with
   `ovmx_init`, which would break them — so it works on a **private clone**
   (`boot-work/wd0.img`); the shared disk is only read (and, on a cold cache,
   kernel-swapped the same way the siblings swap it, via the shared marker).
5. **install-boot session** (single-user, NetBSD init still present): install
   `STARTUP.EXE` as `/sbin/init` (keeping NetBSD's as `/sbin/init.netbsd`),
   place `vms.kmod`/`vmsfs.kmod` in the kernel `module_path` so `ovmx_init`'s
   bare-name `modctl(MODCTL_LOAD, "vms"/"vmsfs")` resolves them the authentic
   way, pre-create `/dev/vms` (major captured from a real load) + the `ra1`
   (DKA0:) node + the boot mount points.
6. **prove session** (single-user, so kernel securelevel is 0 and `ovmx_init`'s
   `modctl` loads are permitted — `ovmx_init`, being init, never raises it): the
   kernel execs `/sbin/init` = `ovmx_init` as PID 1, with the ODS-2 volume on
   SIMH `rq1 -> ra1 -> DKA0:`. Assert the milestone console lines.
7. **negctl**: boot the assembled disk **without** the ODS-2 volume — the mount
   cannot happen, so the `%OVMX-I-MOUNTED` line MUST NOT appear (Rule 7 teeth).

## Backend changes (`ovmx_boot_netbsd.c`), the pins this task was reserved to make

The seam header (vms-f2e) explicitly deferred the disk device/label to "when the
bootable disk is ASSEMBLED (vms-7b1)". Pinned here:

- **System-disk device (DKA0:).** `#if defined(__vax__)` → `/dev/ra1c` (the OVMX
  ODS-2 volume is the *second* MSCP disk, whole-disk partition `c`; the NetBSD
  root is `ra0`). Mirrors the Linux runtime, where the NetBSD root is the
  initramfs and DKA0: is a separate `/dev/vda`.
- **/dev/vms node creation.** NetBSD has no devfs, so a freshly-`modctl`-loaded
  cdevsw driver has a dynamic char major but no `/dev` entry. `ensure_exec_node()`
  is the NetBSD analogue of what Linux devtmpfs does automatically after
  `finit_module`: it looks the major up from the kernel by driver name
  (`getdevmajor(3)`, libc) and `mknod`s `/dev/vms`. Fail-honest (INV-6): if the
  driver is not really registered `getdevmajor` returns `NODEVMAJOR` and no node
  is made, so `ovmx_boot_open_executive()` fails and PID 1 halts. Idempotent and
  read-only-root-safe (the assembly pre-creates the node; a present node is
  reused).
- **PID 1 console wiring.** The Linux kernel opens `/dev/console` as fd 0/1/2
  for the init process; the NetBSD kernel does **not** — NetBSD `init(8)` opens
  it itself. Empirically, without this every line PID 1 printed (banner,
  `%OVMX-I-*`, halt diagnostics) went to a closed descriptor and never reached
  the console, even though the modules loaded and the boot ran to its honest
  halt. An ELF constructor in the NetBSD backend (runs before `main`, so even
  `main`'s first `printf` is captured) opens `/dev/console` onto 0/1/2 when it
  is PID 1 and stdout is not already a tty. Backend-local, so no `#ifdef` in
  `ovmx_init.c` (INV-DRIFT); the Linux backend has none because the Linux kernel
  already wired it.
- **System-disk mount is `MNT_RDONLY`.** The OVMX ODS-2 vnode backend on this
  substrate is read-only today (it registers the read VOPs; the vmsfs proof
  mounts `MNT_RDONLY`). A read-write mount would be refused, so read-only is the
  honest flag for what the backend can do — a real capability difference from the
  Linux backend's RW mount, not a silent drop. A blank/unformatted volume still
  fails to mount and PID 1 halts honestly.

Plus one authenticity fix in `src/libvms/include/ovmx_identity.h`: the SYSKRNL
banner was hardcoded `OVMX/Linux`. On NetBSD that is a false statement about the
running kernel (INV-6). It now selects `OVMX/NetBSD` under `__NetBSD__` at compile
time, so a binary can only ever name the kernel it was built to run on.

## Where the boot stops, and what's missing for DCL (vms-d59)

After `%OVMX-I-MOUNTED`, `ovmx_init` runs `require_installed_system()`, which
`stat`s `SYS$SYSTEM:DCL.EXE` on the mounted volume. The mastered ODS-2 volume
here carries only `HELLO.TXT` (not a system tree), so PID 1 halts **honestly**:

```
%OVMX-F-SYSINIT, system disk DKA0: is not an installed OVMX system volume
-OVMX-I-SYSINIT, SYS$SYSTEM:DCL.EXE is absent; install the system with the OVMX installer before booting
```

That halt is the design-init-scope.md §1 "finds it or does not boot" behaviour,
and it is the concrete list of what boot-to-DCL still needs:

1. **A mastered OVMX *system* ODS-2 volume for netbsd-vax** — the installer spine
   (`vms-791` kit-master → `vms-8ab` mastered disk image → `vms-df9` PCSI) must
   emit an ODS-2 volume carrying `SYS$SYSTEM:` with the cross-linked netbsd-vax
   images and RTL, `SYSUAF.DAT`, `OVMXVMSSYS.PAR`, and `SYS$MANAGER:STARTUP.COM`.
2. **The boot-required userspace images (`vms-c99`), cross-linked for
   netbsd-vax:** `DCL.EXE`, `PROVISION.EXE` (PID 1 execs it), `LOGINOUT.EXE`,
   `JOB_CONTROL.EXE`, and the DECC$SHR/RTL shared images they activate. `vms-5d1`
   proved STARTUP/LOGINOUT/DCL *link+activate*; they must now be *delivered on the
   system volume* and *exec'd from vmsfs*.
3. **The boot-required executive facilities (`vms-945e`):** everything
   `PROVISION.EXE` needs from `/dev/vms` beyond ping — establishing the SYSTEM
   identity from SYSUAF through the executive (`$PERSONA`-equivalent), process
   creation (`$CREPRC`/detached processes for services), and the executive
   logical-name tables. The current `vms.kmod` provides the shared exec core
   (eflag/ast/access/mbx/proctab/lock) — enough for the milestone here, not yet
   for identity + `$CREPRC`.
4. **A read-write ODS-2 vnode backend on netbsd-vax** (or an honest read-only
   boot decision): PROVISION/STARTUP write SYSUAF/logs; today the vax vmsfs mount
   is read-only.
5. **Exec-from-vmsfs on netbsd-vax:** activating the OVMX images that live on the
   mounted ODS-2 volume requires the vmsfs vnode backend to serve the pager
   (VOP_GETPAGES/mmap for exec). To be verified as part of `vms-d59`.
