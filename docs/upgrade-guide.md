# OVMX Upgrade Guide

This guide upgrades an **already-installed** OVMX system in place — `PRODUCT
INSTALL`ing a newer `ovmx-os.kit` onto a volume that already carries a real
install, real site customizations, and real user data — and verifies both
survive.

## What this guide is tied to

The command block below is **not hand-maintained prose** — it is
mechanically checked, in CI, against
[`tests/qemu/test_upgrade_e2e.sh`](../tests/qemu/test_upgrade_e2e.sh), the
gate that actually boots OVMX under QEMU, runs these exact commands against
two real `tools/cut-release.sh` bundles (never hand-faked version numbers),
and asserts that upgrading preserves what the site put there. That test's
header carries the finding this gate exists because of: an earlier
`PRODUCT INSTALL` unconditionally overwrote every kit-listed file, including
`SYS$MANAGER:SYSTARTUP_VMS.COM` — a file sites are expected to customize —
silently discarding site configuration on every upgrade. Fixed by kit-entry
"seed once, never replace" metadata (`OVMX_KIT_ENTRY_FLAG_SEED_ONCE`).

If this guide's steps and the gate's steps ever disagree,
[`tools/check_guide_drift.py`](../tools/check_guide_drift.py) fails the
build. Do not hand-edit the fenced block below without also updating the
gate script (or vice versa) — see that script's header and
[`tests/integration/test_guide_drift.sh`](../tests/integration/test_guide_drift.sh).

## Prerequisites

- A running OVMX system with an existing install already on its target
  volume (e.g. `VDA100:`, unmounted at the start of this procedure).
- The **upgrade** product kit (a newer `ovmx-os.kit`, cut by
  `tools/cut-release.sh` from a later commit) staged on a kit-carrier
  volume, in `[SYSUPD]`, as `OVMX-OS-UPGRADE.KIT`. `tests/qemu/
  run_upgrade_e2e.sh` masters such a carrier volume with `vmsfs_master` as
  part of preparing this gate; a real site would receive the kit the same
  way it received the original install kit.
- Logged in as `SYSTEM`.

## Upgrade procedure

1. **Mount the kit carrier** volume holding the upgrade kit.
2. **Mount the target volume** — the already-installed system being
   upgraded. It is *not* reformatted; the upgrade installs on top of it.
3. **Run `PRODUCT INSTALL`** against the upgrade kit, with the same
   destination as the existing install. `%PCSI-I-DONE` means the upgrade's
   files landed; seed-once files (like `SYSTARTUP_VMS.COM`) are skipped if a
   site-customized copy already exists.
4. **Verify with `PRODUCT SHOW PRODUCT`** that the destination now reports
   the *new* version, not the version it shipped with.
5. **Dismount both volumes** so they flush cleanly.

<!-- ovmx:guide-steps:begin -->
```dcl
$ MOUNT VDA200: KITS
$ MOUNT VDA100: WORK
$ PRODUCT INSTALL VMS /SOURCE=VDA200:[SYSUPD]OVMX-OS-UPGRADE.KIT /DESTINATION=VDA100:
$ PRODUCT SHOW PRODUCT /DESTINATION=VDA100:
$ DISMOUNT VDA100:
$ DISMOUNT VDA200:
```
<!-- ovmx:guide-steps:end -->

Expected output at step 3 includes `%PCSI-I-DONE` and no `%PCSI-E-`/`%PCSI-F-`
line. Expected output at step 4 shows the *upgraded* version — not the
version the volume reported before this procedure.

## What the gate proves that a plain install cannot

`test_upgrade_e2e.sh` writes real user state (a file under `[USER]`) and a
real site customization (an appended marker line in
`SYS$MANAGER:SYSTARTUP_VMS.COM`) onto the target volume *before* running the
upgrade steps above, then asserts after the upgrade — and again after a full
QEMU restart — that:

- the user's data file survives byte-identical,
- the site customization marker survives,
- `PRODUCT SHOW PRODUCT` reports the upgraded version, not the baseline one,
- the machine still boots to a login prompt.

## Note on identity

OVMX badges itself honestly: `PRODUCT SHOW PRODUCT` and every other
human-facing surface answer as **OVMX**, **OpenVMS-compatible** — never as
"OpenVMS" itself (see `src/libvms/include/ovmx_identity.h`, INV-0). This is
a compatibility layer, not a redistribution of VSI's product.
