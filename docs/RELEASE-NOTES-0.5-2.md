# OVMX 0.5-2

**x86_64 boots to login again, and the OpenVMS GCC-port crt0 surface is built up the do-it-like-VMS ladder.**

0.5-1 completed C++ first-light and the Alpha authentic-login gate. 0.5-2 leads with the
x86_64 boot-to-login fix (vms-656), then packs the crt0 surface the real OpenVMS GCC port
lands on, an honest-failure pass over the identity/executive boundary, and the groundwork
carrying the NetBSD/VAX runtime toward the executive ACP.

## What landed

- **x86_64 boots to login again (vms-656) — the headline fix.** A native-link
  build-flag drift had dropped the shipped RMS's ODS-2 ACP arm, so `STARTUP.COM` could
  not resolve `SYS$STARTUP:VMS$PHASES.DAT` over the executive ACP and the boot spun before
  the `Username:` prompt. The fix restores genuine Files-11 ACP resolution of plain
  (non-rooted) search-list logicals — **removing a POSIX fallback rather than papering
  over it** — and adds a drift-catching guard so the ACP arm cannot silently fall out of
  the shipped image again. x86_64 boots to the `Username:` login prompt.
- **The OpenVMS GCC-port crt0 surface, built up the ladder.** OVMX now presents the
  image-activation context the `alpha-dec-vms` port's crt0 expects rather than adapting
  the port down to a Linux surface:
  - **IMGACT presents a genuine VMS image-activation context** — the Alpha standard
    call at entry (#720).
  - **`decc$main` produces `argc`/`argv`/`envp`** as the DEC C RTL image-startup does,
    plus a `_malloc32` sub-4GB fix for the low-memory arena (#719).
  - **`C$_EXIT1` is a C-RTL globalvalue** — an absolute link-time constant that folds
    at LINK, not a `.vms$imp` import (#718).
  - **LINK.EXE reads the port's native EVAX object** — the Alpha/VMS object's psects
    and symbols parse and link with no ELF force-down, including cross-image `SYMG`
    import binding and the canonical `dsc$descriptor_s` binding (#721, #728, #729, #730).
  - Design of record: IMGACT VMS image-activation-context for the GCC-port crt0
    (vms-f60d, #716).
- **vmssshd fail-honest on executive identity refusal** — an INV-6 auth-bypass fix:
  when the executive refuses to vouch for an identity, `vmssshd` fails honestly rather
  than falling through to a userspace success (#727).
- **The vms-040 executive-boundary audit (Phase A)** — a full inventory of the places
  musl images bypass the executive by issuing raw Linux syscalls (#726).
- **Genuine `$ALLOC`/`$DALLOC` + a NetBSD executive device table** — real device
  allocation/deallocation over the executive device table, an installer target-mount
  prerequisite (#713, #722).
- **The vms-329 VAX-runtime ACP cutover work.** The NetBSD/VAX executive masters the
  lab volumes as genuine ODS-2, answers `VMS_IOCTL_REGISTER`/`VMS_IOCTL_DASSGN`, stages
  `[USERS]`/`[SYSTMP]` on the VAX volume, and re-keys the DCL and LIBVMS ACP arms from
  `__linux__` to `OVMX_HAVE_ACP` — advancing the VAX runtime toward the executive ACP
  (vms-d5d/vms-049; the temporary ACP block trace was reverted).
- **Alpha userspace RUN-half under `qemu-system-alpha`** — the syssvc/imgact suite runs
  on the Alpha userspace half (#700).
- **The do-it-like-VMS ladder reconcile** — roadmap and docs reconciled to the ladder
  rulings (#710), and the **cc1-specific LINK.EXE IE/TPOFF32 relaxation reverted** per
  the operator ruling, keeping the generic IMGACT musl-TLS path (#708).
- **RMS ACP read-ahead = a VMS multiblock window** (`RMS_DFMBC`), not one block at a
  time (#705).
- **SPAWN'd subprocesses visible in `SHOW USERS`/`SHOW SYSTEM`** with correct
  interactive/subprocess classification (#702).
- **Roadmap release feed** — reconcile.py now emits an `atom.xml` release feed (#707).

## Known limitations

- **NetBSD/VAX runtime is not yet on the executive ACP.** The VAX ACP codec is built and
  unit-proven, and the vms-329 cutover work re-keys the ACP arms and masters genuine
  ODS-2, but the VAX runtime still boots via the Files-11 VFS/POSIX path. Converting the
  VAX runtime fully onto the executive ACP is tracked as **vms-d5d / vms-049** (the
  `vms-d9c` VAX-boot gate is green to `PROVISION.EXE` via the current path).
- **The in-guest OpenVMS GCC port does not yet build on OVMX.** The crt0 surface is
  built rung by rung (activation context, `decc$main`, `C$_EXIT1`, EVAX link) so the real
  port's crt0 links and starts; the full in-guest GCC port building and running remains
  the lane's deliverable, not this cut's claim.
