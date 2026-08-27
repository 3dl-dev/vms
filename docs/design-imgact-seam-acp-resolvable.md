# IMGACT VMS-standard activation seam — stage the subject ACP-resolvably (vms-f60d)

Status: IMGACT half DONE (this branch). Harness half OWNED BY THE ALPHA LANE.

## Why this exists

The flagship activation proof: a Linux kernel `execve`s a subject whose `PT_INTERP`
is `IMGACT.EXE`; the kernel maps both, runs `IMGACT.EXE`, which performs the
VMS-standard 6-argument activation and routes the returned condition value to
`$EXIT` (the `OVMX-SEAM: ... $STATUS=0x00000bad` line).

Under **qemu-user** this already passes (`run_vms_std_activation_alpha.sh`): with
no `/dev/vms`, `imgsrc_open()` falls through to the vms-5f0 legacy POSIX open, so
the subject resolves and the genuine re-open path runs.

Under the **real alpha kernel (qemu-system)** the previous seam staged the subject
at a Linux-initramfs path (`/tests/activate_seam`). `/dev/vms` IS present there, so
`imgsrc_open()` resolves over the **executive Files-11 ACP**, which cannot see an
initramfs path → `SS$_NOSUCHFILE` → no POSIX fallthrough → `%IMGACT-F-IMGNOTFND`.

Commit `a9155a46` tried to work around this with an in-memory magic-scan fallback
(`find_mapped_vms_section` / `activate_symbol_vector_in_memory`) that activated the
subject from its **mapped tmpfs copy**. That is a test-accommodation adjacent to the
INV-6 "silent userspace fallback for an executive facility" LARP class, and — worse —
its mere presence in `IMGACT.EXE` regressed the interp's *earliest* execution under
the real kernel (see "finding #1" below). **`a9155a46` has been reverted.**

The authentic fix is to stage the subject where production images live — on the
**ODS-2 volume** — so the REAL `imgsrc_open()` → ACP path resolves it and reads its
`.vms$xfer` from genuine ODS-2 bytes, exactly as production does. This is a harness
change; the Alpha lane owns ODS-2 staging.

## The IMGACT resolution contract (what the harness must satisfy)

From `src/imgact/imgact.c` (`imgsrc_open` + `imgsrc_map_staged`, unchanged):

1. `imgsrc_open(execfn)` first calls `imgsrc_map_staged(execfn)`:
   - It remaps **only** paths under the boot-stage prefix
     `IMGACT_BOOT_STAGE_PREFIX = "/run/ovmx-boot/"`.
   - For such a path it takes the **basename** and prepends
     `IMGACT_SYSEXE_VOLPATH = "/vms/SYS0/SYSCOMMON/SYSEXE/"`, then ACP-opens that
     on `g_acp_sysdevice` (the runtime-discovered SYS$SYSDEVICE).
   - Any path NOT under `/run/ovmx-boot/` passes through **unchanged** and is
     ACP-opened literally.
2. `imgact_acp_open()` result:
   - `SS$_NOSUCHDEV` (executive/`/dev/vms` absent) → legacy POSIX fallthrough
     (qemu-user only).
   - `SS$_NOSUCHFILE` / `SS$_DEVNOTMOUNT` (`/dev/vms` present, file/volume issue)
     → **no** fallthrough → `imgsrc_open` returns -1 → `die_imgnotfnd`.

So for the seam subject to resolve under qemu-system, the ACP must find it on the
mounted ODS-2 volume at the SYSEXE location.

## Harness spec (Alpha lane — build this)

Follow the **production boot-stage pattern** (how `DCL.EXE` / `LOGINOUT.EXE` boot),
in the qemu-system-alpha seam harness (the `boot-vmsko-qemu-alpha.sh`-style script
that assembles the initramfs and the vda ODS-2 volume):

1. **Put the subject's genuine bytes on the ODS-2 volume.** Write the built subject
   image (the flavor `VMS_STD` `.vms$xfer` stub — today `src/imgact/test/vmsstd/img.S`
   built with `-DXFER_FLAVOR=1`, or the real flagship subject) onto the mounted vda
   ODS-2 volume so it resolves at the SYSEXE directory that
   `/vms/SYS0/SYSCOMMON/SYSEXE/<NAME>.EXE` maps to — i.e.
   `SYS$SYSROOT:[SYSCOMMON.SYSEXE]<NAME>.EXE`. Name it uppercase `.EXE`
   (ODS-2 naming). Suggested name: `ACTIVATE_SEAM.EXE`.
2. **Stage a boot copy in tmpfs and `execve` THAT path.** Place a copy at
   `/run/ovmx-boot/ACTIVATE_SEAM.EXE` and have the seam init `execve`
   `/run/ovmx-boot/ACTIVATE_SEAM.EXE` (so `AT_EXECFN` carries a boot-stage path).
   `imgsrc_map_staged` then remaps the basename to
   `/vms/SYS0/SYSCOMMON/SYSEXE/ACTIVATE_SEAM.EXE` and the ACP reads the **genuine
   ODS-2 bytes** — never the tmpfs copy (INV-6 satisfied).
   - The subject's `PT_INTERP` must still name the run-time `IMGACT.EXE` path, and
     the ELF must be loadable by the alpha kernel with a **valid, mapped `AT_PHDR`**
     (17a28322's `%IMGACT-F-BADIMGHDR` mincore hedge is retained and will reject a
     header-outside-PT_LOAD image fail-honest).
3. **Ensure SYS$SYSDEVICE is the mounted ODS-2 volume before the exec.**
   `g_acp_sysdevice` is discovered from the environment (`imgact_discover_sysdevice`)
   — the volume must be ACP-mounted (as the executive-proof boot already mounts vda)
   so the open is `SS$_NORMAL`, not `SS$_DEVNOTMOUNT`.

If wiring the boot-stage remap for the seam is inconvenient, the equivalent is to
`execve` the subject by a path the ACP resolves directly to the on-volume SYSEXE
file; the boot-stage tmpfs + SYSEXE remap is preferred because it is byte-for-byte
the production first-hop pattern.

## Acceptance (the re-run must show BOTH)

1. **All five markers fire** (proving finding #1 is gone — no `a9155a46` code in the
   interp):
   `IMGACT-ENTRY reached` / `IMGACT-GENVP ...` / `IMGACT-SYSDEV` / `IMGACT-PHDR` /
   `IMGACT-PRE-ASV`.
2. The genuine ACP re-open path runs (no `%IMGACT-F-IMGNOTFND`), the 6-arg standard
   call is issued and RETURNS to IMGACT, and `$EXIT` records the odd condition:
   `OVMX-SEAM: image=ACTIVATE_SEAM.EXE stdcall_returned=1 has_exited=1 $STATUS=0x00000bad`.

A green run that reached `$STATUS` via any in-memory fallback would NOT satisfy this
— the point is that the **authentic production path** (ACP read of genuine ODS-2
bytes → `.vms$xfer` → standard call) carries the proof.

## finding #1 (why `a9155a46` had to be reverted, not patched in place)

`a9155a46` only ADDED post-marker functions, yet under the real alpha kernel it
suppressed even `IMGACT-ENTRY` — the very first raw `syscall6(SYS_write)` emitted
*before* `self_relocate()`. Post-marker code cannot temporally suppress a pre-marker
print, so the regression is a **code-presence / layout / relocation effect on the
interp's earliest execution**, not a fallback-execution bug:

- `IMGACT.EXE` is a static-PIE `ET_DYN`. `_start` establishes `$gp` PC-relatively
  (`br`/`ldgp`), and `self_relocate()` applies the `R_ALPHA_RELATIVE` slots — but it
  runs AFTER marker (a). Marker (a) is only correct if the compiler addresses its
  string `m_a` in a **load-bias-invariant** form (GP-relative), not via an
  **unrelocated GOT/linkage-table slot** (which still holds the link-time address
  and, under the real kernel's nonzero `ET_DYN` rebase, points at unmapped memory →
  the `SYS_write` `EFAULT`s silently → the marker is lost).
- Adding `a9155a46`'s functions perturbed the `.text`/linkage-table layout enough to
  flip marker (a)'s string reference into the GOT-dependent form (the reloc COUNT can
  stay 43 if the slot already existed and only the addressing instruction changed).
  The later `%IMGACT-F-IMGNOTFND` prints because it runs AFTER `self_relocate()`,
  when the GOT is fixed — hence the paradox "GOT-free marker silent but GOT-using
  IMGNOTFND fires."

Because the side-effect is on the PRODUCTION interp's earliest execution, any fix
that KEEPS `a9155a46`'s code (patch-in-place, or map section headers into a PT_LOAD)
must fully root-cause and neutralize this fragile early-exec dependency or it ships a
latent early-exec corruption a green test would mask. Reverting `a9155a46` removes
the perturbation entirely and restores the marker-firing early execution proven under
qemu-user (all five markers observed on this branch). This is why (c)+revert is the
chosen fix rather than (a) or (b).
