# OVMX Install Guide

OVMX is an OpenVMS-compatible environment for Linux: VMS system services, DCL,
RMS, and a real kernel-resident executive (`vms.ko`), packaged as a bootable
distro. This guide installs a real `ovmx-os.kit` product kit onto a blank
target volume, using `PRODUCT INSTALL` — OVMX's PCSI-shaped installer, driven
entirely from DCL, over the real executive at `/dev/vms`.

## What this guide is tied to

The command block below is **not hand-maintained prose** — it is
mechanically checked, in CI, against
[`tests/qemu/test_product_install_e2e.sh`](../tests/qemu/test_product_install_e2e.sh),
the gate that actually boots OVMX under QEMU, runs these exact commands, and
asserts the install really landed real files on the target volume (a
directory listing of the target confirms `HELP.EXE` exists, and the
installed image is executed *from the target device* — see that test's
header for the full "anti-LARP" reasoning).

If this guide's steps and the gate's steps ever disagree,
[`tools/check_guide_drift.py`](../tools/check_guide_drift.py) fails the
build. Do not hand-edit the fenced block below without also updating the
gate script (or vice versa) — see that script's header and
[`tests/integration/test_guide_drift.sh`](../tests/integration/test_guide_drift.sh).

## Prerequisites

- A booted OVMX system (`./boot.sh`, or a bootable image built from
  `distro/Dockerfile.bootable`), logged in as `SYSTEM`.
- A blank target volume, already `INITIALIZE`d and attached as a virtio disk
  (e.g. `VDA100:`). `tools/cut-release.sh`'s output ships everything needed
  to build a bootable image; the target volume for `PRODUCT INSTALL` is a
  second, separate disk you provide.
- The product kit itself, `OVMX-OS.KIT`, staged at `SYS$UPDATE:` on the
  already-booted system disk (the bootable image build stages a
  byte-verified copy there for exactly this purpose).

## Install procedure

1. **Mount the target volume.** `PRODUCT INSTALL` writes through the real
   VMS filesystem, so the destination must already be a mounted VMSFS
   volume.
2. **Run `PRODUCT INSTALL`**, naming the kit at `SYS$UPDATE:` and the
   destination device. `%PCSI-I-DONE` means the kit's files landed.
3. **Verify with `PRODUCT SHOW PRODUCT`.** This reads the real product
   database `PRODUCT INSTALL` just wrote on the target volume — not an echo
   of the command line — and reports the installed product by the name
   baked into the kit itself.
4. **Dismount** so the volume is flushed cleanly.

<!-- ovmx:guide-steps:begin -->
```dcl
$ MOUNT VDA100: WORK
$ PRODUCT INSTALL VMS /SOURCE=SYS$UPDATE:OVMX-OS.KIT /DESTINATION=VDA100:
$ PRODUCT SHOW PRODUCT /DESTINATION=VDA100:
$ DISMOUNT VDA100:
```
<!-- ovmx:guide-steps:end -->

Expected output at step 2 includes `%PCSI-I-DONE` and no `%PCSI-E-`/`%PCSI-F-`
line. Expected output at step 3 lists the kit's product name (e.g.
`X86VMS VMS`) with state `Installed`.

## What just happened

The installed files persist on the target volume across a full restart of
the emulated machine (not a tmpfs artifact of the running QEMU process) —
`test_product_install_e2e.sh` proves this by killing QEMU and booting a
fresh process against the same disk files, then re-running the same
`DIRECTORY` and `PRODUCT SHOW PRODUCT` checks.

## Note on identity

OVMX badges itself honestly: `PRODUCT SHOW PRODUCT` and every other
human-facing surface answer as **OVMX**, **OpenVMS-compatible** — never as
"OpenVMS" itself (see `src/libvms/include/ovmx_identity.h`, INV-0). This is
a compatibility layer, not a redistribution of VSI's product.
