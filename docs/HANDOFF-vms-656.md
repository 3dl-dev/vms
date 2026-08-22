# HANDOFF — vms-656 diagnosis (STOOD DOWN, no code landed)

Status: **diagnosis only.** Per the conductor's stand-down, the resolver/DCL fix
is owned by the peer ACP/vmsfs lane. This branch (`work/vms-656`) carries only
this note. The resolver commit that was briefly on the branch tip
(`edf849cf`, since reverted) is preserved in the branch history / reflog as a
reference implementation; `src/vmsfs/vmsfs_translate.c`, `src/vmsdcl/dcl_cmd_io.c`,
and `tests/vmsrms/test_dirlogical_compose.c` are back to origin/main at the tip.

## What this session actually did (veracity)

- **No boot repro was run.** The task's "confirmed 3 ways — runtime KVM+TCG"
  was the task author's claim, not something reproduced here. There is **no
  console trace** from this session. Evidence below is **source analysis + a
  host unit test** only.
- A bootable image (`ovmx-boot-vms656`, `distro/Dockerfile.bootable`) was built
  to completion but the boot harness was **not** run against it.

## Mechanism (confirmed at the compose layer, host unit test)

`compose_ods2_r()` in `src/vmsfs/vmsfs_translate.c` (~L819–835) only fans out
search-list members inside the `if (rooted)` branch (concealed-rooted devices
like `SYS$SYSROOT`). `SYS$STARTUP` is defined by STARTUP.COM as a **plain,
non-concealed, non-rooted 2-element search list**:

```
DEFINE/SYSTEM SYS$STARTUP SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYS$STARTUP],SYS$MANAGER
```

It is not rooted, so it falls through to the single-equivalence directory-bearing
branch (`strchr(equiv,'[')`), which follows only `lnm_translate`'s **first**
value. Member 1 (`SYS$MANAGER`) is dropped.

Measured directly (rebuilt `test_dirlogical_compose` after defining SYS$STARTUP
as the real 2-member list via `lnm_create_multi`):

- **origin/main resolver:** `SYS$STARTUP:VMS$PHASES.DAT` → **1 candidate**
  `DKA0:[SYS0.SYSCOMMON.SYS$STARTUP]VMS$PHASES.DAT` (member 1 lost).
- **with the fan-out fix:** → **3 candidates**
  `DKA0:[SYS0.SYSCOMMON.SYS$STARTUP]VMS$PHASES.DAT`,
  `DKA0:[SYS0.SYSMGR]VMS$PHASES.DAT`,
  `DKA0:[SYS0.SYSCOMMON.SYSMGR]VMS$PHASES.DAT` — member 1 recovered, search order
  preserved (member 0 first).

## ⚠ CRITICAL for the peer — the fan-out fix likely does NOT explain the FNF of VMS$PHASES.DAT

`VMS$PHASES.DAT` is staged in the rootfs at **member 0's** directory:

```
distro/rootfs/vms/SYS0/SYSCOMMON/SYS$STARTUP/VMS$PHASES.DAT
  == [SYS0.SYSCOMMON.SYS$STARTUP]VMS$PHASES.DAT
```

That is exactly the **one candidate origin/main already composes**. The fan-out
fix only *adds* member-1 candidates (`SYS$MANAGER`→`[SYSMGR]`), tried **after**
member 0; it does **not** change member 0. So for `VMS$PHASES.DAT` specifically,
the resolver on origin/main already emits the correct candidate — a pure
search-list fan-out cannot be why that file FNFs.

If `VMS$PHASES.DAT` still returns `%RMS-E-FNF` on the mastered disk, the gap is
**downstream of composition**:

- the ACP directory walk over `[SYS0.SYSCOMMON.SYS$STARTUP]` — note the literal
  `$` in the `SYS$STARTUP.DIR` directory-component name; verify the ACP walks a
  `$`-bearing directory component, and/or
- `vmsfs_master` / the disk-mastering layout — verify `SYS$STARTUP.DIR` and
  `VMS$PHASES.DAT` are actually emitted onto the mastered ODS-2 volume in
  `[SYS0.SYSCOMMON.SYS$STARTUP]` and are openable via `$ASSIGN`+`IO$_ACCESS`.

The fan-out fix is still correct and worth landing (files that live only in
`SYS$MANAGER` via SYS$STARTUP *are* lost today) — but it should be validated by
booting to `Username:`, not assumed to resolve the VMS$PHASES.DAT FNF.

## Which disk the RED legs boot (corrects the "installer-written target" framing)

Both named RED legs boot the **same** build-mastered image, not an
installer-written disk:

- `tests/qemu/test_distrib_boot.sh` → `/boot/ovmx-distrib.img`
- `tests/qemu/test_startup_phase_driver.sh` → `/boot/ovmx-distrib.img`

`/boot/ovmx-distrib.img` is produced by `vmsfs_master` at build time from
`distro/rootfs/vms`. So in these two legs the axis is **mastered image vs the
operator's qemu-wasm demo image**, not installed-target vs pre-built. If the
demo image boots and these do not, compare how each disk's
`[SYS0.SYSCOMMON.SYS$STARTUP]` tree was written.
