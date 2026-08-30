# Device-native, boot-discovered system disk (vms-9f5)

**Status:** implemented. Parent `vms-47d` (device-native naming), subsumes `vms-482`
(the DKA0:→DUA0: VAX rename). Operator ruling 2026-08-30: DUA0: for VAX ("fine"),
VDA0: for virtio.

## What changed

The system disk is no longer hardcoded `DKA0:`. It is named by its **native kernel
identity, per substrate**, and the boot device is **discovered at runtime** rather
than frozen at compile time.

| Substrate | Backing | VMS unit | Why |
|-----------|---------|----------|-----|
| virtio (x86_64, Alpha) | `/dev/vd[a-z]` | `VDA0:`, `VDA100:`, … | `VD` = OVMX's device code for a virtio "Virtual Disk" — an OVMX design choice, **labelled** as such (Rule 8); virtio is not VMS hardware and has no VMS code. |
| VAX (NetBSD/SIMH) | MSCP/RQDX3 `/dev/ra*` | `DUA0:`, `DUA100:` | `DU` is the **authentic** VMS device code for an MSCP/UDA disk. This is what a real VMS-VAX calls an RQDX3 disk; `DK` (== SCSI/RK) was always a placeholder. |

The parallel native codes for other Linux substrates — SATA/SCSI `sd*` → `SDA`,
NVMe `nvme*` → `NVME` — are the **documented scheme** for when such a substrate
exists. They are deliberately **not** shipped as probe branches: OVMX has no
SATA/NVMe runtime to ground-test them, and a probe that names a device it cannot
see would be the exact unmeasured-claim anti-pattern INV-6 forbids. Such hardware
is named by its own probe when it is actually present.

## Naming sites (the source of the name)

Two independent executive naming sites, one per substrate family:

- **`src/kernel-core/vms_devtab.c`** `vms_devtab_probe_disks()` — the shared probe
  walks the virtio-blk name space and names the Nth disk `VDA(N*100):`. This is
  **shared kernel-core**, compiled into `vms.ko` (x86_64) and the NetBSD `vms`
  module, and exercised on Alpha through the per-PR N=3 activation gate — so it is
  the leg that proves the LP64 executive names its disk `VDA0:` too. On NetBSD/VAX
  this probe is inert (no `/dev/vd*`).
- **`src/kernel-netbsd/vms_blockdev_netbsd.c`** `ovmx_acp_unitmap[]` — the VAX
  substrate enters its MSCP units. The map already carried both spellings; vms-9f5
  moves the `primary` (entered-into-the-device-table) flag from the `DKA*` rows to
  the authentic `DUA*` rows. `DKA*` remains a **resolver-only alias** so a stale
  reference still resolves across the cutover (a follow-on retires the alias).

## Discovery + the OVMX_SYSDEVICE contract

The boot chain now **discovers** which unit is the system disk instead of assuming
`DKA0:`. Precedence (Linux, `src/ovmx_init/ovmx_boot_linux.c`):

1. `OVMX_SYSDEVICE` environment variable;
2. an `ovmx.sysdev=<unit>` token on the kernel command line (`/proc/cmdline`);
3. the substrate compile-time default `SYSDISK_DEVICE ":"` (`VDA0:` / `DUA0:`).

PID 1 mounts the selected unit via the Files-11 ACP (the mount is **by unit name**;
the executive resolves the backing), and then **publishes** the discovered unit as
`OVMX_SYSDEVICE` (`setenv`). This closes a loop that was previously *unfulfilled*:
`imgact.c` and `lnm_defaults.c` already read `OVMX_SYSDEVICE` and documented that
"the boot chain publishes it", but nothing did — so they fell back to a frozen
`DKA0:`. Now the whole userland agrees on the disk PID 1 actually mounted.

This is what makes booting a system on the **second** disk possible: pass
`ovmx.sysdev=VDA100:` and PID 1 mounts `vdb`, publishes it, and every child resolves
`SYS$SYSDEVICE` there. `src/libvms/include/ovmx_layout.h`'s `SYSDISK_DEVICE` is now
substrate-conditional (`__vax__` → `"DUA0"`, else `"VDA0"`); every `VMS_SYS*`
filespec and `SYSDISK_DEVICE`-keyed seed follows it automatically.

## Proof (ground-source, no mock)

- **`tests/qemu/test_boot_alternate_disk.sh`** (NEW, wired into the CI boot-smoke
  job): Case A default single-disk boot reaches login (zero-regression); **Case B**
  boots the real system as the *second* disk (`VDA100:`, selected by
  `ovmx.sysdev=`) to login — **impossible on pristine main**, which ignores the
  selector and mounts the blank `vda`; Case C is an anti-LARP control (same disks,
  no selector, must **not** log in — proving the decoy is on `vda`, so B's success
  is the selector and not QEMU disk ordering).
- Existing boot/resolver/e2e goldens updated in the same commit to the native
  names: `test_kmod_disk`, `test_syssvc_initialize`, `test_kmod_errcnt`,
  `test_syssvc_sysuaf_uic_base`/`_dirlogical_acp`/`_mmk_build` (the `VDA300:`
  fixture disk `vdd`), `test_distrib_boot`, `test_persistent_boot`,
  `test_boot_conformance`, `test_install_boot_e2e`, `test_show_device_rows`
  (row regex `^VDA[0-9]*:`), `facility_defects`, and the VAX lab drivers
  (`DUA0:`/`DUA100:`).

## Scope boundaries

- NIC naming (`ETH0:`) is `vms-9d2`, separate.
- The unit-number scheme is unchanged (positional `*100`: `VDA0:`/`VDA100:`,
  `DUA0:`/`DUA100:`). Whether MSCP units should use plain sequential numbering
  (`DUA0:`/`DUA1:`) is a separable authenticity question, deliberately **not**
  folded into this boot-critical atomic flip.
- Filespec-*parse* fixtures (`tests/libvms/test_filescan.c`, etc.) keep `DKA0:` as
  arbitrary, syntactically-valid device-name test data — they exercise RMS/LNM
  string handling on the host and resolve no device, so the name is immaterial.
