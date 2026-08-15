# A VMS-faithful installation process for OVMX/NetBSD-vax

**Status:** design record (not yet implemented). Non-blocked prep — `vms-4834`
(R4 of `vms-f10`) is blocked on `vms-065` (R3, runtime parity) for
**co-release**, but the install *model* is designable now because the VAX
runtime capstone (`vms-d59`) is done: OVMX/NetBSD-vax boots under SIMH to a
real DCL `Username:` prompt.

**Template (read first):** `docs/design-vms-faithful-install.md` — the
x86_64/Alpha faithful-install model this document maps onto vax. Nothing here
invents a new install shape; every section below is "how does the landed
x86_64 piece map to netbsd-vax", not a fresh design.

**Companions:** `docs/design-vax-mainstream-release.md` (epic `vms-f10`, R1-R4
table); `docs/design-ovmx-kit-format.md` (the kit container format, already
arch-independent); `tests/lab-vax/README.md` (the SIMH lab protocol); `rd show
vms-718` / `rd show vms-37f` (the x86_64 epic + its R1 capstone e2e).

---

## 1. The model does not change — only the substrate under it

OpenVMS installs the same way on every architecture (`design-vms-faithful-
install.md` §2, measured on Alpha V8.4): distribution media is a bootable
system volume; booting it runs a menu; the menu does `INITIALIZE`/`MOUNT` the
target, then `PRODUCT INSTALL` lays the OS onto it as a kit; the target then
boots standalone to a login. OVMX's x86_64 implementation of that model is
already landed (`vms-718`/`vms-37f`, `vms-8ab`, `vms-df9`/`vms-96ec`). **R4 is
not a new install model — it is the same model, with a NetBSD/vax substrate
under the "media" and "target boots to DCL" steps**, exactly as the epic table
says: "reuse the install machinery with a vax backend."

The one structural fact that makes vax different is **Decision A**
(`vms-42d`): OVMX images on netbsd-vax are ordinary ELF32 dynamic executables
activated by NetBSD's `ld.elf_so`, with the OVMX RTL **statically linked** —
there are **no** `SYS$SHARE:*$SHR.EXE` shareables on vax (`vms-c99`
handoff). That single fact is why the vax `SYSTARTUP_VMS.COM`
(`distro/rootfs-vax/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM`) already
diverges from the Linux one — it drops the `INSTALL ADD SYS$SHARE:*$SHR.EXE`
block — and it is the one place the installer's payload genuinely differs by
architecture, not just by target width.

## 2. Media: what "distribution media boots a full OS" means on vax/SIMH

**x86_64 today:** QEMU boots the slim bootstrap initramfs (`vms.ko`,
`vmsfs.ko`, `STARTUP.EXE` — nothing else) against **two drives**: a mastered
distribution disk (`ovmx-distrib.img`, built by `distro/Dockerfile.bootable`'s
mastering stage, `vms-8ab`) whose `SYSTARTUP_VMS.COM` runs the install menu,
and a blank target disk. `install ─ qemu -initrd bootstrap -drive distrib.img
-drive blank.img`.

**vax:** there is no separate "bootstrap initramfs" concept — NetBSD/vax boots
its own kernel from `wd0.img` with `ovmx_init` (`STARTUP.EXE`) installed as
`/sbin/init` (`vms-7b1`), and the OVMX **system volume** is a second SIMH disk
unit attached over the MSCP controller (`rq1 → ra1 → DKA0:`,
`tests/lab-vax/drive_boot_vax.py`'s `sysboot` mode). The vax analog of "boot
the distribution disk" is therefore: **attach a distribution-shaped ODS-2
volume at `DKA0:` instead of the existing plain system volume**, and add a
**second** MSCP unit (`rq2 → ra2 → DKA100:`, say) carrying a blank target —
the exact two-drive shape x86_64 already uses, just over SIMH's RQ/RA
controller instead of QEMU's virtio/ATA.

**REUSE, unchanged:** the whole boot substrate — the GENERIC+MODULAR NetBSD
kernel, `vms.kmod`/`vmsfs.kmod`, `ovmx_init` as PID 1, the ELF32 dynamic-exe
activation path — is exactly what `vms-7b1`/`vms-d59` already proved. R4 does
not touch any of it.

**VAX-SPECIFIC new work:** `stage_sysvol.sh` currently stages one shape (a
plain, already-installed-looking system tree). It needs a second staging mode
— "distribution" — that lays the OS kit at `SYS$UPDATE:` and swaps in a
distribution variant of `SYSTARTUP_VMS.COM` that invokes the install menu (see
§3). `run-boot.sh`/`drive_boot_vax.py` need a mode that attaches **two** ODS-2
volumes (distribution + blank target) instead of one, mirroring the existing
`sysboot` mode's single-volume attach.

## 3. PCSI kit: what's shared vs. what Decision A changes

`docs/design-ovmx-kit-format.md` already establishes the kit **container**
format as architecture-independent (a flat, OVMX-invented, Rule-8-labeled
index+payload format — no VMS byte layout to diverge from, because none is
published). `tools/ovmx_kit_pack.c` (the packer) and `src/product/
ovmx_kit_reader.c` + `src/product/product.c` (`PRODUCT.EXE`, the installer)
are portable C with no architecture assumptions in the format itself. **The
kit format needs no vax-specific design work.**

What *does* need vax-specific work is getting the **images that drive
install** onto elf32-vax at all. Checked against `tools/cross-vax/build-*.sh`
(the existing per-image cross-build scripts): `vms-c99`/`vms-7b1` cross-built
exactly the five **boot** images (`STARTUP.EXE`, `PROVISION.EXE`, `DCL.EXE`,
`JOB_CONTROL.EXE`, `LOGINOUT.EXE`). **None of `PRODUCT.EXE`, `AUTHORIZE.EXE`,
`INITIALIZE.EXE`, or `SYSGEN.EXE` — every utility `OVMX$INSTALL.COM`'s menu
runs — has a vax cross-build script yet.** This is the single largest concrete
gap R4 must close, and it is mechanical, not a new design: each follows the
exact same static-link/Decision-A pattern `build-vmsdcl-vax.sh` et al. already
established (link against NetBSD libc, statically link the OVMX RTL, activate
via `ld.elf_so`).

The menu procedure itself (`distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/
OVMX$INSTALL.COM`) is pure DCL with no architecture assumptions — **it is
reused byte-for-byte** (INV-DRIFT: one source of truth for DCL/data, the same
principle `stage_sysvol.sh`'s header already states for everything but the
five boot images and `SYSTARTUP_VMS.COM`). Two pre-existing gaps the x86_64
menu already carries forward unfixed and undisguised (documented in the
procedure's own header) — `INITIALIZE` not resolving VMS device syntax to a
block device (gap (a)), and `SYSGEN.EXE` not yet resolvable as an image on the
target (`vms-597`) — are **inherited by vax as-is**, not R4's to fix; the
existing x86_64 gate proves the `PRESERVE` branch only (never `INITIALIZE`)
for the same reason, and the vax gate should do the same.

The **distribution disk's** `SYSTARTUP_VMS.COM` needs one more edit beyond the
existing Decision-A vax variant: on x86_64, "the distribution disk differs
[...] only in its payload [...] a site startup that runs the menu"
(`design-vms-faithful-install.md` §2) — i.e. the distribution disk's
`SYSTARTUP_VMS.COM` invokes `OVMX$INSTALL.COM` where the installed target's
does not. The vax distribution `SYSTARTUP_VMS.COM` must be **both**
Decision-A (no `INSTALL ADD SYS$SHARE` block) **and** carry that menu
invocation — a small, mechanical variant of the file that already exists at
`distro/rootfs-vax/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM`.

## 4. Laydown: installing onto a fresh ODS-2 disk

`PRODUCT INSTALL /DESTINATION=<dev>` (`src/product/product.c`,
`pd_kit_target_path()`) writes the rooted, concealed `SYS0/SYSCOMMON/<bracket>/…`
layout — the *same* structure `vmsfs_master`/`stage_sysvol.sh` already master
directly (`vms-96ec` made this the shared invariant on x86_64: an
installed target and a directly-mastered disk are boot-structurally
identical). Nothing here is x86_64-specific: `product.c` is portable C and,
once cross-built for vax (§3's gap), lays down the identical structure
`stage_sysvol.sh`'s `sysboot` mode already masters and boots today. **This is
pure reuse once the image exists** — no vax-specific laydown logic is needed.

The kit payload itself, for vax, is: the five boot images + the four new
utility images (§3) + `SYSUAF.DAT`/`RIGHTSLIST.DAT`/`OVMXVMSSYS.PAR`
(arch-neutral data, per `stage_sysvol.sh`'s own header) + `STARTUP.COM` +
the Decision-A `SYSTARTUP_VMS.COM`. Packaging it needs a vax-targeted
invocation of `tools/ovmx_kit_pack.c` pointed at a vax payload staging
directory — mirroring `distro/Dockerfile.bootable`'s `/kit-stage` step
(lines ~617-669) but sourced from the vax cross-build outputs instead of the
Linux `link-native` stage. This is new build-tooling glue, not new design:
same packer, same format, different input tree.

## 5. Boot-to-DCL of the installed disk (the R4 acceptance e2e)

Mirrors `vms-37f` exactly, with SIMH sessions standing in for containers.
`vms-37f`'s proof shape: container 1 boots media, drives the menu, installs
to a **host-mounted** target file; container 2 is a **separate** `docker run`
that boots the target **alone** (no distribution disk attached) against the
same host file, logs in with the install-set SYSTEM password, and runs
`PRODUCT SHOW PRODUCT` / `DIRECTORY SYS$SYSTEM:`; container 3 re-boots to
prove idempotence. The container boundary is the point — it is what caught
the real writeback-cache bug (`vms-9b7`) that a same-process test would have
missed.

vax has the identical class of boundary already, proven by `run-boot.sh`'s
own `ISOLATION FROM THE SHARED DISK` discipline: `BOOT_WORKDIR` is a private
**copy** of the cached NetBSD disk, and the mastered ODS-2 volume file is
attached across **separate SIMH invocations** (`run_session()` launches one
`docker run` + one SIMH process per stage). The R4 capstone reuses that same
shape: session 1 boots the NetBSD/vax runtime with the distribution volume
at `DKA0:` and a blank volume as target, drives `OVMX$INSTALL.COM` over the
console (a new console driver in the `drive_*_vax.py` family, following
`drive_boot_vax.py`'s existing milestone-matching pattern rather than
`vaxharness.py`'s lower-level primitives directly); session 2 is a **separate**
SIMH invocation that attaches **only** the now-installed target volume at
`DKA0:`, boots it alone, and drives login + `PRODUCT SHOW PRODUCT` +
`DIRECTORY SYS$SYSTEM:` over the console exactly as `drive_boot_vax.py`'s
`sysboot` mode already asserts `%STDRV-I-STARTUP`. The `sysboot` mode's
pre/post-boot **sha256 hash-diff** technique (proving real block writes,
not a silently-no-op'd VOP — the INV-6 teeth `vms-e7a` already built) is the
right template for proving the install's writes actually landed, reused
directly rather than re-invented.

This is the item that actually closes `vms-4834` — see §6, rung H — and it is
the one place R3 (`vms-065`) is a **hard** prerequisite: R3 puts the plain
boot-to-DCL SIMH proof into the release-acceptance gate; R4's e2e is strictly
more work (install *then* boot), so the release train should not carry a
"VAX installs and boots" claim before it already carries "VAX boots" as a
release gate. Rungs A-G below (cross-building the utility images, kit
packaging, media mastering) do **not** individually depend on R3 — they only
need the boot substrate `vms-d59` already proved — but the item that closes
the epic, and any claim that VAX installs are part of a release, does.

## 6. REUSE vs. vax-backend — the split, concretely

| Piece | x86_64 today | vax (R4) |
|---|---|---|
| Install model (media→menu→PCSI kit→target→DCL) | `vms-718` | **Same model, no redesign** |
| Kit container format (`ovmx_kit_format.h`) | `vms-0b6` | **100% reused** — arch-independent, already Rule-8-labeled |
| Kit packer (`tools/ovmx_kit_pack.c`) | — | **100% reused**, pointed at a vax payload dir (new glue, not new tool) |
| `PRODUCT.EXE` install logic (`src/product/product.c`) | `vms-df9`/`vms-96ec` | **100% reused C**, needs a **new elf32-vax cross-build** (gap) |
| Install menu DCL (`OVMX$INSTALL.COM`) | `vms-dcf` | **Reused byte-for-byte** (INV-DRIFT) |
| `AUTHORIZE.EXE`, `INITIALIZE.EXE`, `SYSGEN.EXE` | existing x86_64 builds | **New elf32-vax cross-builds needed** (none exist today) |
| `SYSTARTUP_VMS.COM` (installed target) | Linux variant (`INSTALL ADD` block) | Decision-A variant **already exists** (`distro/rootfs-vax/…`) |
| `SYSTARTUP_VMS.COM` (distribution disk) | Linux variant + menu invocation | **New**: Decision-A variant **+** menu invocation (small edit) |
| `PRODUCT INSTALL` laydown (rooted `SYS0/SYSCOMMON`) | `vms-96ec` | **100% reused** once `PRODUCT.EXE` is cross-built |
| Volume mastering (`vmsfs_master`) | `distro/Dockerfile.bootable` | **100% reused tool**; needs a "distribution" staging mode in `stage_sysvol.sh` |
| Two-drive attach (distribution + blank target) | QEMU `-drive` ×2 | **New**: SIMH RQ controller, second unit |
| Boot substrate (kernel, kmods, PID 1, activation) | N/A | **100% reused**, proven by `vms-d59` |
| Container-boundary e2e proof shape | `vms-37f` | **Same shape**, SIMH-session boundary instead of `docker run` boundary (new harness code, same proof design) |

The shape of the work is: **nothing about the install *model* or the kit
*format* is vax-specific — every open gap is "cross-build an existing,
already-portable C program for a fourth target" or "stage a second SIMH
disk unit,"** not new design.

## 7. Constraints honored

- **Rule 8 (clean-room).** No new VMS format research is needed — the kit
  format was already clean-room-derived and Rule-8-labeled for x86_64
  (`design-ovmx-kit-format.md` §2); vax reuses the identical container, so
  there is nothing new to derive or cite. The one place a VMS-authenticity
  question could arise — VAX MSCP disk naming — is called out as an open
  question in §8, not decided here (it was set by `vms-7b1`/`vms-d59`, out
  of this design's scope to relitigate).
- **Rule 9 (SIMH is a dev/test vehicle; real hardware is the target).** None
  of the install logic (`PRODUCT.EXE`, the kit format, `vmsfs_master`) has
  any SIMH- or QEMU-specific assumption baked in — it operates on block
  devices and VMS filespecs. The only SIMH-specific code is the *lab
  harness* (`drive_boot_vax.py`, `run-boot.sh`), which is explicitly test
  tooling, not the installer itself — matching how `distro/Dockerfile.
  bootable`/QEMU relate to the x86_64 installer.
- **Decision A (`vms-42d`) consequence.** Carried through consistently: no
  shareable-image `INSTALL ADD` anywhere on vax, RTL statically linked into
  every cross-built image including the four new utility images.
- **No CI/build work done by this document.** This is a design + decomposed
  plan only; §6/§8 name gaps and rungs, nothing here modifies
  `tools/cross-vax/`, `stage_sysvol.sh`, `.github/workflows/`, or any other
  buildable artifact.

## 8. Open questions / operator-reserved

1. **VAX disk naming: `DKA0:` (as `vms-7b1`/`vms-d59` already chose) vs. the
   historically-authentic VAX MSCP name `DUA0:`.** Real OpenVMS VAX
   commonly names UDA50/RQDX-class MSCP disks `DUx:`; `DKA0:` is the
   Alpha/SCSI-port convention OVMX's boot proof reused for vax (`vms-7b1`
   handoff: `DKA0:=/dev/ra1c`). This predates R4 and is not introduced by
   the installer, but the installer's `Enter device name for target disk:`
   prompt is the first place a human actually *types* a vax device name, so
   it is the first place the naming choice becomes visible to an installer
   experience aimed at VAX authenticity. Same family of question as `vms-9f5`
   (`VDA0:` vs `DSA0:` on Linux) — flagging, not deciding, here.
2. **Does the vax installer media reuse the x86_64 menu (`OVMX$INSTALL.COM`,
   `SYS$UPDATE:OVMX-OS.KIT`) or get a vax-native kit/menu identity?** This
   design assumes reuse of the menu verbatim and a vax-specific kit *file*
   (`OVMX-OS-VAX.KIT`, distinct product identity string, e.g. `OVMX VAXVMS
   VMS Vx.y` mirroring the existing `OVMX X86VMS VMS V0.1` shape) — the same
   "reuse the shape, vax-flavor the identity string" pattern the Alpha
   oracle itself showed (`DEC AXPVMS VMS` vs a VAX product name would also
   differ). Confirming the exact product-name tokens is small and can be
   decided at implementation time, not here.
3. **Standalone-BACKUP / option-8 VAX install path.** `design-vms-faithful-
   install.md` §4 explicitly puts "VAX standalone-BACKUP is history [...]
   kept only as the eventual analogue of Alpha's menu option 8" and rules it
   **out of scope** for the whole faithful-install epic. R4 inherits that
   exclusion — this design does not reopen it.
4. **INITIALIZE's VMS-device-syntax gap and `vms-597` (SYSGEN.EXE
   resolvability)** are pre-existing, cross-architecture gaps the x86_64
   menu already lives with (proving only the `PRESERVE` branch). Whether to
   fix them is an existing backlog call, not new to R4 — noted so nobody
   reads the vax gate's `PRESERVE`-only scope as a new vax-specific
   limitation.

## 9. Decomposed work-item plan

All items below are children of `vms-4834` (epic `vms-f10`). **Gating:**
rungs A-D and E-G are cross-build/packaging/media work that only needs the
already-done `vms-d59` boot substrate; rung H (the acceptance e2e that
actually closes `vms-4834`) is additionally gated on **R3 (`vms-065`)** —
the plain boot-to-DCL SIMH proof must be in the release-acceptance gate
*before* an install→boot proof can be claimed as part of a co-release. rd
dependency edges below encode exactly that: H depends on `vms-065`; A-G do
not.

| Rung | Outcome | Depends on |
|---|---|---|
| **A** | `PRODUCT.EXE` cross-builds and activates for elf32-vax (`tools/cross-vax/build-product-vax.sh`, Decision-A static-link pattern, broken-TU negctl mirroring existing gates) | `vms-d59` (done) |
| **B** | `AUTHORIZE.EXE` cross-builds and activates for elf32-vax | `vms-d59` (done) |
| **C** | `INITIALIZE.EXE` cross-builds and activates for elf32-vax | `vms-d59` (done) |
| **D** | `SYSGEN.EXE` cross-builds and activates for elf32-vax | `vms-d59` (done) |
| **E** | Vax OS kit packaging: a vax payload staging dir (five boot images + A-D's four utility images + SYSUAF/RIGHTSLIST/OVMXVMSSYS.PAR/STARTUP.COM/Decision-A SYSTARTUP_VMS.COM) packed via `tools/ovmx_kit_pack.c` into `OVMX-OS-VAX.KIT` | A, B, C, D |
| **F** | Distribution-volume staging mode: `stage_sysvol.sh` (or a sibling) gains a mode that lays the kit at `SYS$UPDATE:` and swaps in a distribution `SYSTARTUP_VMS.COM` (Decision-A + menu invocation), mastered via `vmsfs_master` into `ovmx-distrib-vax.img` | E |
| **G** | Two-disk SIMH lab harness: `run-boot.sh`/`drive_boot_vax.py` gain a mode that attaches the distribution volume at `DKA0:` and a blank target at a second MSCP unit, and drives `OVMX$INSTALL.COM`'s console menu to completion (PRESERVE branch, mirroring the x86_64 gate's own scope) | F |
| **H (capstone)** | The R4 acceptance e2e: install session (G) → **separate** SIMH session boots the target alone → login → `PRODUCT SHOW PRODUCT` lists the vax kit → `DIRECTORY SYS$SYSTEM:` shows `DCL.EXE` from the target — the vax analog of `vms-37f`, using the sha256 hash-diff technique (§5) as the positive real-write proof. Closes `vms-4834`. | G, **`vms-065`** |

Rungs A-D are independent of each other and can proceed in parallel. E
depends on all of A-D (needs all four images to stage a complete kit). F
depends on E. G depends on F. H depends on G and, additionally, on R3
(`vms-065`) landing in the release-acceptance gate.
