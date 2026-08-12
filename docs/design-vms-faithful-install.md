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
| **First-boot completion** | **MEASURED (vms-490) → justified no-op for OVMX (vms-649).** The Alpha 8.4 first boot was captured end to end (`docs/oracle/installation-media-vax73-alpha84.md` §5). Every one-time step it performs names a facility OVMX does not implement — AUTOGEN parameter feedback + page/swap sizing (§5b), the boot-1-defers / boot-2-starts gating of the **security server, ACME server and audit server** (§5b/§5c/§5d), and the four rights identifiers `AUTHORIZE.EXE` adds on boot 2 (§5c: `SYS$NODE_OVMXOR`, `DECW$WS_QUOTA`, `IMGDMP$READALL`, `IMGDMP$PROTECT` — DECnet, DECwindows, image-dump). OVMX has none of these, so there is no honest first-boot work to reproduce and no VMS status to pin; faking any of it is the exact INV-6 / Rule-10 LARP the authenticity invariants forbid. Full analysis and citations: **§3.5** below. Ground-source proof: `tests/qemu/test_persistent_boot.sh` (a CI gate) boots the pre-installed system disk **twice** and asserts both boots reach login and are identical with no AUTOGEN/first-boot phase on either. |
| **Release e2e test** | The ground-source gate: master media → boot media → drive the menu over the console → boot the target → log in → `PRODUCT SHOW PRODUCT` lists the OS. Must be shown failing on the unfixed tree first (the vms-9b7 standing rule). |

### 3.4 KEEP

`INITIALIZE.EXE` (userspace mkfs, `tools/vms_initialize.c`, shares
`vmsfs_ondisk.h`); `executive_attach()` and the fail-stop executive gate;
`dcl_backup.c`'s saveset create/restore/list (not on the install path;
`BACKUP/IMAGE` and an option-8/standalone analogue are future work, filed but
not in this epic's critical path).

### 3.5 First-boot completion, measured — why OVMX's is a justified no-op (vms-649)

The `vms-490` oracle run finished the Alpha install that §3a had stopped at the
PCSI configuration phase and captured the installed target's first boot end to
end (`docs/oracle/installation-media-vax73-alpha84.md` §5). This section maps
what that capture shows first boot *doing* onto OVMX's facilities, one step at a
time, and records the disposition of each. The scope of `vms-649` was
explicitly provisional by the oracle: implement whatever OVMX needs to match it,
**or** close as a justified no-op with the citation if the oracle shows first
boot doing nothing OVMX has. The oracle shows the latter.

**What the oracle measured (§5e, verbatim conclusion): "first boot is not one
event, it is two", gated by AUTOGEN, and the gate is *what subsystems are
allowed to start*.** Boot 1 (post-install, pre-AUTOGEN) is a structurally
reduced startup that explicitly *defers* the security subsystem; it runs AUTOGEN
(GETDATA, GENPARAMS, GENFILES, SETPARAMS, REBOOT) and drives its own automatic
reboot. Boot 2 (post-AUTOGEN) is where the security server, ACME server and
audit-server database actually come up and where `AUTHORIZE.EXE` first populates
the rights database.

Mapping each first-boot action to OVMX (oracle line references in parentheses):

| Oracle first-boot action | OVMX facility | Disposition |
|---|---|---|
| AUTOGEN GETDATA/GENPARAMS: compute parameters, write `PARAMS.DAT` / `VMSIMAGES.DAT` (§5b) | OVMX SYSGEN is a static ~1-parameter model (SCSNODE); no feedback-driven computation, no `VMSIMAGES.DAT`. `SYS$SYSTEM:OVMXVMSSYS.PAR` ships **in the kit** (`distro/rootfs/…/SYSEXE/OVMXVMSSYS.PAR`) and SCSNODE is set during install by the menu's `SYSGEN` step. | **No analogue.** Nothing to compute; the parameter file already exists and is set at install time. |
| AUTOGEN GENFILES: create `SYS$ERRLOG.DMP`, extend `PAGEFILE.SYS` (§5b) | OVMX has no pagefile / swapfile / error-log-dump facility (`grep` finds only PCB struct fields, no page/swap sizing). | **No facility.** Creating those files at first boot would fake a facility that does not exist. |
| AUTOGEN SETPARAMS + REBOOT: activate computed parameters, auto-reboot (§5b/§5d) | Nothing changes OVMX's parameters between install and boot, so nothing needs activating and there is no reason to reboot. The one parameter that *is* set (SCSNODE) is read by `read_boot_parameters()` on the target's ordinary boot. | **No analogue.** An automatic reboot with nothing changed is theater. |
| Boot 1 defers, boot 2 starts: **security server** (`SECSRV-I-SERVERSTARTINGU`), **ACME server** (`ACME-I-SERVERSTART`) (§5c/§5d) | OVMX runs neither. The only STDRV component registered at boot is JOB_CONTROL (`SYS$STARTUP:VMS$VMS.DAT`). `secsrvmsgdef.h` / `ciadef.h` are message/constant headers, not a running server. | **No facility.** Printing "server not started" then "server starting" for servers OVMX does not have is INV-6 LARP. |
| Boot 2 creates the **audit-server database** (`AUDSRV-I-NEWSERVERDB`) (§5c/§5d) | No audit server process. `SHOW AUDIT` reports a per-process status flag; the `AUDIT_SERVER` name is only a `SHOW PROCESS` gap fixture. | **No facility.** |
| Boot 2 `AUTHORIZE.EXE` adds four rights identifiers — `SYS$NODE_OVMXOR`, `DECW$WS_QUOTA`, `IMGDMP$READALL`, `IMGDMP$PROTECT` (§5c) | OVMX's rights database is real (`RIGHTSLIST.DAT`, `src/libvms/rtl/rightslist.c`), but all four name facilities OVMX lacks: DECnet node proxy, DECwindows, image-dump. `RIGHTSLIST.DAT`'s own header **already** excludes `SYS$NODE_*` (§3 "NO SITE ROWS") and forbids shipping identifiers for facilities OVMX "cannot grant or check" (§2, citing Rule 10). | **Deliberately excluded by existing design.** Adding them at first boot is the precise Rule-10 "invent the plausible-looking middle" that file already rules out. |

**Conclusion.** Every one-time action the Alpha first boot performs is the
first-time bring-up of a facility OVMX does not implement. OVMX's own install
already produces a **fully configured, directly bootable** target — the system
tree, the product database (`VMS$PRODUCT_DATABASE.DAT`, written to the target by
`PRODUCT INSTALL /DESTINATION`), `OVMXVMSSYS.PAR` and the configured SCSNODE are
all in place when `PRODUCT INSTALL` finishes — and that target boots straight to
an ordinary login (already proven by `test_install_menu.sh`'s target boot and
`test_distrib_boot.sh`). There is therefore **no first-boot completion phase,
and no first-boot-vs-subsequent-boot distinction, for OVMX to build**: the
target's first boot is identical to every later boot, which is the honest state
for a system with no deferred one-time configuration. When OVMX later grows a
facility whose bring-up must be gated to a first boot (an AUTOGEN, a security
server), *that* facility's bead builds the mechanism it needs; adding an empty
self-disabling first-boot procedure now would be a speculative abstraction
(Rule 2) doing nothing.

**Scope note / constraint honored:** `vms-649` also ruled that first-boot work,
if any, must be a **command procedure**, never C in `src/ovmx_init/ovmx_init.c`
(PID 1 stays bootstrap-only). The no-op honors this trivially — nothing enters
PID 1, and nothing enters a first-boot `.COM` either, because there is nothing
to run. If a future first-boot facility appears, the mechanism it stages must
still be a `.COM`, not PID 1 C.

**Ground-source proof (vms-649).** `tests/qemu/test_persistent_boot.sh` (a CI
gate — `.github/workflows/ci.yml` "Run persistent boot smoke test") gains a
second boot of the pre-installed system disk and asserts the two boots are
materially identical: both reach `%STDRV-I-STARTUP` begun and a login prompt,
and **neither** runs any AUTOGEN / first-boot / one-time-completion phase, so
there is no boot-1-only phase a later boot skips. That is the measurable form
of "the installed system's first boot completes configuration": it is already
complete, and a second boot changes nothing. (The proof rides
`test_persistent_boot.sh`'s already-bootable mastered `ovmx-distrib.img` rather
than a menu-installed target because `PRODUCT INSTALL /DESTINATION=<dev>`
originally extracted the kit into a **flat** `<dev>:[SYSEXE]` layout instead of
the rooted `[SYS0.SYSCOMMON.SYSEXE]` a booted system disk requires — so a
`/DESTINATION`-installed target was not bootable as a system disk. That
install-path gap in `src/product/product.c pd_kit_target_path()` is **fixed by
vms-96ec** — see §3.6 below, which also adds the missing gate that boots a
`/DESTINATION`-installed target; it was never a first-boot-completion issue.)
### 3.6 FIXED — PRODUCT INSTALL /DESTINATION must lay the rooted concealed layout (vms-96ec)

**Found by vms-649, fixed by vms-96ec.** A booted OpenVMS system disk is
rooted and concealed: the boot root is `[SYS0.]` and shared files live under
`[SYS0.SYSCOMMON.]`, so `SYS$SYSTEM:` is `DEV:[SYS0.SYSCOMMON.SYSEXE]`
(`ovmx_layout.h`: `VMS_SYSEXE`, `VMS_SYSTEM_DIR = /vms/SYS0/SYSCOMMON/SYSEXE`).
`vmsfs_master` masters `ovmx-distrib.img` in exactly this shape
(`distro/Dockerfile.bootable`), and STARTUP resolves `SYS$SYSROOT:[SYSEXE]DCL.EXE`
through it at boot.

`PRODUCT INSTALL /DESTINATION=<dev>` (`src/product/product.c`,
`pd_kit_target_path()`) originally wrote a **flat** `<mount>:[SYSEXE]…`
instead — files landed on the volume, but not under the rooted root. A
`/DESTINATION`-installed target was therefore **not bootable as its own
system disk**: booted as `DKA0:` it halted `%OVMX-F-SYSINIT` because the
boot path looked for `SYS$SYSROOT:[SYSEXE]DCL.EXE` under the rooted structure
and found nothing. This went undetected because no CI e2e booted a
`/DESTINATION`-installed target *as its own system disk* — every booting
gate boots the pre-mastered (already-rooted) `ovmx-distrib.img`.

**The fix:** `pd_kit_target_path()` and `pd_db_path()` now write
`<mount>/SYS0/SYSCOMMON/<bracket>/…` for `/DESTINATION` just as they already
did for the default (currently-running) system — the same rooted, concealed
structure a mastered disk has. The `SYS0/SYSCOMMON` prefix lives on the
volume, so it is mount-point-independent: bytes installed at
`/mnt/dkaNNN/SYS0/SYSCOMMON/…` resolve correctly once the volume is booted as
`DKA0: → /vms`. The OS kit already carries every directory a boot needs
(`SYSEXE`/`SYSLIB`/`SYSMGR`/`SYSHLP`/`SYS$STARTUP`, including `SYSUAF.DAT`,
`STARTUP.COM`, and the `SYS$STARTUP` phase data), so a rooted kit-install is
boot-structurally identical to a mastered disk. `[SYSTMP]`/`[USERS]` are not
in the kit but are non-fatal for boot-to-login (LOGINOUT's `chdir` and
PROVISION's `mkdir` are non-fatal, and SYSTEM's home `[SYSMGR]` is in the
kit); the `%RMS-E-DNF` those absences would cause surfaces only in a
data-writing app, not at login.

**The coverage that was missing** is now `tests/qemu/test_install_boot_e2e.sh`
(runner `run_install_boot_e2e.sh`, CI job `install-boot-e2e`): it PRODUCT
INSTALLs the OS kit onto a blank INITIALIZEd disk, then boots THAT disk as the
sole `DKA0:` system disk with the bootstrap-only slim initramfs and reaches a
SYSTEM login — red on the unfixed (flat) tree (`%OVMX-F-SYSINIT`), green
after. It runs for real in CI (not opt-in cover); a SKIP is a hard failure.

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
