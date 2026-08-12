# A VMS-faithful installation process for OVMX

**Status:** design record, approved direction (operator rulings 2026-08-07).
**Oracle:** `docs/oracle/installation-media-vax73-alpha84.md` — both architectures
measured on real media, console captures preserved.
**Companion:** `docs/design-init-scope.md` (the PID 1 scope review that started
this) and the `vms-61d` item tree.
**Work tree:** see `vms-ins` (epic) — the dispatch-ready decomposition of this
document. Worktree `.claude/worktrees/vms-install`, branch
`worktree-vms-install`.

---

## 1. The rulings this implements

1. **Installation moves out of init entirely. Init is minimum-necessary
   bootstrap.** (Operator, 2026-08-07.)
2. **It should look like VMS in all things** — no invented device names, no
   directories cosplaying as devices, no facades that print success. (Operator,
   same session, rejecting three successive LARP designs.)
3. **The installation media is bootable and is an instance of the OS it
   installs**; it does disk operations, copies the system on, and the target's
   first boot finishes the job. (Operator; confirmed byte-for-byte by both
   oracles.)
4. **OVMX is 64-bit, so Alpha is the model.** The VAX standalone-BACKUP shape
   is history, kept only as the eventual analogue of Alpha's menu option 8.

## 2. The model, as measured on OpenVMS Alpha V8.4

The distribution medium is an ordinary bootable system volume. Booting it
brings up the **full operating system**, which runs a menu procedure:

```
1) Upgrade, install or reconfigure OpenVMS ALPHA Version V8.4
...
8) Execute DCL commands and procedures
9) Shut down this system
```

Choosing (1) asks INITIALIZE-or-PRESERVE, target device, volume label, ODS
level; then `Initializing and mounting target....`, `Creating page and swap
files....`; then gathers system configuration (SYSTEM password, cluster
membership, SCSNODE, SCSSYSTEMID, time zone, TDF, PAKs); then installs the OS
**as a PCSI product kit**:

```
The following product has been selected:
    DEC AXPVMS OPENVMS V8.4                Platform (product suite)
Configuring DEC AXPVMS VMS V8.4: OpenVMS Operating System
```

No save set, no `BACKUP/IMAGE`, no special install kernel. The installer *is*
the OS plus a DCL procedure driving ordinary utilities.

**Mapped to OVMX:**

```
build   ─ produces: bootstrap initramfs (vms.ko, vmsfs.ko, STARTUP.EXE — nothing else)
                    distribution disk image (a normal OVMX system disk whose
                    site startup runs the install menu, carrying the OS kit file)

install ─ qemu -initrd bootstrap -drive distrib.img -drive blank.img
          OVMX boots from the distribution disk EXACTLY as from any system disk
          menu procedure: INITIALIZE target / MOUNT target / PRODUCT INSTALL VMS
          writes system configuration to the target; shut down

run     ─ qemu -initrd bootstrap -drive target.img
          STARTUP.EXE mounts DKA0: or halts; first boot completes configuration
```

The distribution disk differs from an installed system disk **only in its
payload** (the kit file, the install procedure, a site startup that runs the
menu). There is no installer binary, no install boot mode, and STARTUP.EXE
cannot tell the two disks apart. That is the whole point.

## 3. What is wrong today — measured, with dispositions

### 3.1 KILL — facades and structures VMS does not have

| Artifact | Where | Why it dies |
|---|---|---|
| `cmd_mount` facade | `src/vmsdcl/dcl_cmd_misc.c:1597` | Never calls `mount(2)`. Writes a per-process userspace device table, uses `getcwd()` as the "mount path", prints `%MOUNT-I-MOUNTED`. No `sys/mount.h` anywhere in DCL. The Rule 9 defect class exactly: per-process success sharing nothing. **Blocks everything else** — an install cannot be a procedure until DCL can mount a volume. |
| `cmd_dismount` facade | same file, after `cmd_mount` | Flips `dev->mounted = 0`; never calls `umount(2)`. Same class. |
| PID 1 install machinery | `src/ovmx_init/ovmx_init.c` — `install_system`, `provision_dirs`, `provision_seed_files`, `provision_ownership`, `provision_sysuaf_users`, `is_system_installed`, the INITIALIZE fork at :417, `copy_recursive`, `copy_seed_file` | ~430 lines of VMSINSTAL executed by the booting kernel on every boot. Full analysis in `docs/design-init-scope.md` §2. |
| Overlay mode | `ovmx_init.c:449` | On mount failure, boots an ephemeral tmpfs copy and prints a warning. A VMS system whose system disk does not mount does not come up. Second runtime shape; Rule 9. |
| `is_system_installed()` marker | `ovmx_init.c:543` | Presence of `DCL.EXE` decides install-vs-boot. VMS asks the volume (home block), not one file. Dies with the install branch itself. |
| Fat initramfs | `distro/Dockerfile.bootable` | A complete OVMX smuggled into the bootstrap so PID 1 can install from it. The initramfs is VMB/SYSBOOT — it loads the executive and mounts the system disk. The slim initramfs is the only initramfs; NOEXEC/NODEV negative controls get rebuilt on that baseline. |
| Three PID 1 SYSUAF reads / two local parsers | `ovmx_init.c` `sysuaf_split`, `sysuaf_field` | Leave PID 1 with the install machinery (already `vms-a17e`: identity comes from the executive; coordinate with `vms-9b7`). |

Also rejected during design, recorded so nobody rebuilds them: the "kit device"
(`DKA400:` naming a directory in a ramdisk), the standalone C installer image
(`ovmx_install.c`), a QEMU-boot build stage to populate volumes, and treating
the fat initramfs as installation media. Each died on the same rule: it made
OVMX *look* installed instead of *being* installed.

### 3.2 CHANGE

| Artifact | Change |
|---|---|
| `STARTUP.EXE` | Reduce to: base-layer mounts → `executive_attach` → `vmsfs.ko` → mount `DKA0:` **or halt honestly** (OVMX facility; no oracle-pinned VMS status exists for it) → identity → `%STDRV-I-STARTUP` begun → `run_startup()` → completed → wait. The STDRV pair brackets the procedure (today both lines print back-to-back *after* it — `main():1214/1217`). Login loop leaves for JOB_CONTROL (`vms-8d2`). |
| `boot.sh`, `distro/boot/run-qemu.sh`, `docs/install-0.1.md` | New flow: build → distribution image; install = boot distrib + blank target; run = boot target with bootstrap initramfs. `run-qemu.sh` already grew `cache=writethrough` (other session, vms-9b7) — keep. |
| `tests/qemu/test_persistent_boot.sh`, `test_executive_integral.sh` | Rewritten against the new flow. `test_docker_persistent_disk.sh` is another session's live work (vms-9b7) — coordinate, don't clobber. |
| `distro/rootfs/.../SYSTARTUP_VMS.COM` | On the *distribution* disk only, it invokes the install menu procedure. The installed target gets the normal one. |

### 3.3 BUILD

| Piece | Notes |
|---|---|
| **Real MOUNT/DISMOUNT** | `MOUNT DKA100:` must end in `mount(2)` of the right block device with vmsfs, and DISMOUNT in `umount(2)`. Requires the device-name→block-device binding to live in the **executive's device table** (vms.ko already creates OPA0: at module init; disk units belong there too — a process asks the executive, never scans `/sys/block` itself). This is a kernel-interface design change → Rule 4 cascade. |
| **INITIALIZE as a DCL verb** | Absent from `builtin_verbs[]` (`dcl_builtin.c:83`). `cmd_initialize` → `dcl_exec_utility("INITIALIZE.EXE", ...)`, the existing pattern (ANALYZE/SYSGEN/INSTALL). `INITIALIZE.EXE` itself is mkfs, exists, works on files and block devices — unchanged. |
| **PCSI: kit format + PRODUCT INSTALL** | Measured: `cmd_product` implements only `PRODUCT SHOW PRODUCT`; everything else is `%PCSI-E-NOTIMPL`; no `PRODUCT.EXE` exists. Needed: a kit container format — **OVMX-defined and labeled as such** (Rule 8: the PCSI kit byte layout is not published; we reproduce the *behavior* — `PRODUCT INSTALL VMS /SOURCE=...`, the product database, `PRODUCT SHOW PRODUCT` listing installed kits — never the bytes). The OS itself becomes a kit (`OVMX AXPVMS VMS V0.1`-shaped naming, honest about being OVMX). |
| **The installation procedure** | DCL. Menu text and question flow pinned to the Alpha capture (INITIALIZE/PRESERVE, target device, label, confirmation gate, SYSTEM password, SCSNODE/SCSSYSTEMID, TZ/TDF). Where OVMX lacks a facility (PAKs, Galaxy), the honest move is omission, not a fake question. |
| **Media mastering build tool** | The build must produce a populated vmsfs volume image on a Linux host. **Decision (mine, build tooling): a userspace mastering tool** — the `xorriso`/`mkfs -d` of vmsfs — extending the same `vmsfs_ondisk.h` that `INITIALIZE.EXE` (userspace mkfs) and `vmsfs.ko` already share, so the format keeps exactly one description. Reasons over a QEMU-populate stage: the build stays plain deterministic tooling; no nested-KVM dependency in CI; and QEMU-populate has a chicken-and-egg of its own (the guest needs the files delivered somehow, which is how the fat initramfs happened). It is factory tooling: never shipped on the media, never given a VMS name, never run at boot. |
| **Kit packaging at build** | Package `SYSEXE`/`SYSLIB`/`SYSMGR`/`SYSHLP` payloads into the OS kit file; master the distribution image = normal system tree + kit + install procedure. |
| **First-boot completion** | **Not yet measured** — the Alpha run was stopped at the PCSI configuration phase, so what the target's first boot does (the AUTOGEN-shaped half) is unestablished. Oracle follow-up: finish the Alpha install on the scratch node, capture the first boot of `DQA0:`, then design OVMX's first-boot step against it. Until then, first-boot behavior is explicitly provisional. |
| **Release e2e test** | The ground-source gate: master media → boot media → drive the menu over the console → boot the target → log in → `PRODUCT SHOW PRODUCT` lists the OS. Must be shown failing on the unfixed tree first (the vms-9b7 standing rule). |

### 3.4 KEEP

`INITIALIZE.EXE` (userspace mkfs, `tools/vms_initialize.c`, shares
`vmsfs_ondisk.h`); `executive_attach()` and the fail-stop executive gate;
`dcl_backup.c`'s saveset create/restore/list (not on the install path;
`BACKUP/IMAGE` and an option-8/standalone analogue are future work, filed but
not in this epic's critical path).

## 4. Decisions taken vs. reserved

**Taken (with reasons, per authority rules):** mastering tool over
QEMU-populate (§3.3); kit format OVMX-defined and labeled (Rule 8 requires it);
VAX standalone-BACKUP path out of scope for the install epic (Alpha is the
model; option 8 is a later faithfulness item). `vms-c8f` (the install-path
decision item) is resolved by the operator's rulings + the oracle and closed
accordingly; `vms-4af`/`vms-b8b` fold into the new tree.

**Reserved / open:** nothing blocks dispatch. The first-boot design lands only
after its oracle run (§3.3), and the kernel device-table change carries the
Rule 4 cascade inside the tree.

## 5. Cold-start pointers

- Oracle captures: `/data/training/vax/run-install-oracle/console-standalone-backup-2026-08-07.log`
  (VAX), `/data/training/vax/alpha/captures/alpha84-install-procedure-2026-08-07.log` (Alpha).
- Alpha lab protocol: `tests/lab-alpha/README.md`; the local AXPbox node driver
  is `/data/training/vax/alpha/node.sh` (persistent console pump; AXPbox dies
  if the console client disconnects).
- The `vms-61d` epic holds the PID-1-scope items (`vms-a17e` identity,
  `vms-8d2` JOB_CONTROL, `vms-f5c` STDRV, `vms-43d` AUTHORIZE-owns-homes);
  the install epic depends on none of them except where stated per-item.
- Concurrent work: `vms-9b7` (SYSUAF parsers; worktree
  `sysuaf-one-format-one-parser`) also edits `ovmx_init.c` — coordinate merges.
