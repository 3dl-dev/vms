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
(x86_64 and Alpha LP64). The NetBSD/VAX substrate flip is DONE and PROVEN — it
lands as V0.5-1, the next cut in this release train, not the 0.5 tag itself.**

We name this deliberately rather than bury it. The VAX ACP flip is complete and
proven end-to-end on real NetBSD/VAX under SIMH: `vms.kmod` mounts `DKA0:` over the
**executive ACP** with the vmsfs mount compiled out (`#else`), PROVISION
demand-pages off the ODS-2 volume and runs, and an on-disk **hash-diff confirms
real ACP read *and* write** — no false-pass. It is not in the 0.5 tag only because
it rides the 3-way convergence gate {x86_64 + VAX ILP32 + Alpha LP64} on top of
0.5's merged base first; that cut is V0.5-1. So the authenticity flip is real on
all three substrates (x86_64 Linux, Alpha LP64, NetBSD/VAX); 0.5 tags two of them
and V0.5-1 completes the matrix within days. The gap is named as closing, not
hidden behind a job exclusion.
