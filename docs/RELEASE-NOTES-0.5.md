# OVMX 0.5

**The 0.5 milestone: OVMX authenticates and reads its own system files as genuine Files-11 ODS-2 over the executive ACP — the `/vms` passthrough is retired from the runtime path — and builds its own userland in-guest with zero bash in the build path. Proven on two architectures, with the third done and landing next.**

0.5 is the authenticity flip. Earlier milestones reached files through a `/vms`
passthrough that borrowed the Linux filesystem; 0.5 makes RMS read and write
genuine **Files-11 ODS-2** volumes through the **executive ACP** — the same
`$ASSIGN` + `IO$_ACCESS`/`READVBLK`/`WRITEVBLK` path a real VMS system uses. The
fabrication is excised, not hidden behind a flag. And the userland that runs on
top now builds itself in-guest through the OVMX-native toolchain.

## The flip (the 0.5 hook)

- **SYS$DISK is genuine Files-11 ODS-2 over the executive ACP.** Read *and* write
  via `$ASSIGN` + `IO$_ACCESS`/`READVBLK`/`WRITEVBLK`; RMS resolves over the ACP;
  the `/vms` passthrough is retired on the runtime path. The ODS-2 FH2 map handles
  >256-block files (multi-format-1 pointers).
- **Authentic binary login.** `SYSTEM`/`MANAGER` authenticate by **Purdy** against
  a **genuine binary `$UAFDEF` SYSUAF** (World-protected) read over the ACP — no
  ASCII shortcut, no SHA-256, no `/vms`. `$RDBDEF` RIGHTSLIST (World:R) and
  per-file protection via `ods2_class_fileprot`.
- **Proven multi-arch, not x86_64-only.** The flip authenticates over genuine
  ODS-2 on **x86_64 (Linux substrate)** and is independently proven on
  **Alpha LP64** (`qemu-system-alpha`: two consecutive clean login-to-`$` boots).
  Proving the ACP read across two widths is the evidence that it is real, not a
  single-target special case.

## Self-host userland

- **The shipped MMK.EXE drives the whole OVMX-native build chain in-guest** —
  TCC compile → LIBRARIAN archive → LINK to a runnable image → IMGACT activation —
  of real OVMX runtime components, through a persistent mailbox-driven DCL
  subprocess against a real `/dev/vms`. The output is **byte-identical across two
  in-guest builds**, with **zero bash in the build path**. OVMX now builds its own
  userland.

## How 0.5 proves it is real (not just green)

Every executive facility carries a **negative control** that must turn CI red
against a real `/dev/vms` — inject the fabrication a facility could regress into,
and the harness must redden exactly the assertions the manifest names. The flip's
facilities (ODS-2 ACP mount/read/write, binary SYSUAF/RIGHTSLIST, in-process image
activation, the self-host chain) all carry teeth. The suite was run to a clean,
uncontended verdict under KVM — positive control all-green against a real
executive, every negative control firing — so a facility cannot report success
while sharing nothing.

## Honest scope — the three substrates

**0.5 ships the flip proven on the Linux substrate across two architectures
(x86_64 and Alpha LP64). The NetBSD/VAX substrate does NOT yet run the flip: its
runtime boots via the Files-11 VFS/POSIX path, not the executive ACP.**

We name this deliberately rather than bury it. On NetBSD/VAX the executive ACP
codec is built and unit-proven, but it is **not yet wired into the VAX runtime** —
the VAX image set builds with `OVMX_HAVE_ACP` undefined and boots through the
Files-11 VFS/POSIX path. Converting the VAX runtime onto the executive ACP (so
`vms.kmod` mounts over the ACP with the vmsfs mount compiled out, PROVISION
demand-pages off the ODS-2 volume, and an on-disk hash-diff confirms real ACP read
*and* write) is tracked as `vms-d5d`/`vms-049`, targeting V0.5-2+. So the
authenticity flip is real on the two Linux architectures (x86_64 and Alpha LP64)
today; the NetBSD/VAX runtime conversion is named as open work, not hidden behind a
job exclusion.
