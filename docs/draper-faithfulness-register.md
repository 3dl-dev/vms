# Draper Faithfulness Register

> **"Draper faithfulness"** (operator, 2026-08-11) — the Don Draper problem: a persona that
> presents flawlessly and is hollow underneath. In OVMX terms: code that returns a success
> status or prints plausible VMS output while doing nothing real, reports per-process/local
> state as if it were shared/system-wide, or invents an artifact and presents it as
> VMS-authentic. This is INV-6 (executive layer) + INV-DCL (surface layer), applied product-wide.
>
> Produced by a 5-lane read-only audit (2026-08-11) of `vms-054-alpha-port` — all findings
> confirmed identical to `main`. Re-derive status before acting; this is an execution pointer,
> not stored truth. rd IDs are as cited in code/docs (nostr board; some may be stale).

## The headline

**The authenticity program is working where it has been pointed.** The loudest historical
facades are genuinely fixed on main: the getenv-identity family (AUTHORIZE `vms-b2e`, MAIL
`vms-2d39`), the executive IPC spine (event flags, locks, mailboxes, ASTs, `LNM$SYSTEM`), the
`SHOW SYSTEM/USERS/PROCESS` identity surfaces, and the three big CI gate-facades (corpus
`total:0`, reloc golden re-baselining, runtime-target allowlist). Don't re-file these.

**The facade retreated, it didn't die.** It now lives in four pockets: (1) **process-control
and identity by PID** in the executive, (2) the **RMS on-disk representation**, (3) the
**SSH session's credential model** — security-critical, (4) the **DCL surface** (its own pillar,
`vms-b9a`) and the **DCL test suite** that is supposed to guard it.

## Tier 0 — SECURITY-CRITICAL (ships into a customer cluster)

| ID | Finding | Evidence | Status |
|---|---|---|---|
| **NEW** | **SSH logs every user in as root.** `vmssshd` authenticates against SYSUAF and stamps the executive identity, then execs DCL with **no `setuid`/`setgid` drop**. Every SSH session runs euid=0 → System-category access to the whole volume + `CAP_SYS_ADMIN` (ALL privileges) on any child. Console LOGINOUT drops creds (`vms_login.c:342`); SSH does not. | `src/vmsssh/vmssshd.c:434,535` | **Untracked** (code comment only). Filed this session. |
| vms-f15 / vms-36d | **Privileges enforced for reporting, not file access.** The kernel permission hook checks only root/gid0/UIC; SYSPRV/BYPASS/READALL/GRPPRV are never consulted → fails **unsafe** for any euid=0 (root or SSH) session, and wrongly denies a legit-SYSPRV non-root user. | `src/kernel/vmsfs/vmsfs_blkdev.c:1276` | Tracked, blocked (unfinished half). |

## Tier 1 — any VMS user notices immediately / silent data loss / confidently wrong

| ID | Finding | Evidence | Status |
|---|---|---|---|
| vms-9e2 / vms-b5e | **F$GETJPI lexical ignores its pid argument** and answers about the caller; PID item returns Linux `getpid()` dressed as a VMS pid. The *system service* `sys$getjpi` was fixed to read the executive — the DCL lexical every `.COM` uses was left behind. Two identity surfaces disagree in one session. | `dcl_lexical.c:1385,1405,1481` | Tracked, **still live**. |
| vms-pt1 (sharpen) | **Process control by PID signals the wrong Linux process.** `$WAKE/$DELPRC/$FORCEX/$SUSPND` cast the executive-assigned **VMS pid** straight into `kill()` as a **Linux pid** — despite `$CREPRC` documenting VMS pid ≠ Linux pid — then return `SS$_NORMAL`. `vms_kif_wake`/`_hiber` have zero callers. | `sys_process.c:445-449,889-894,944-950,969` | Tracked (labeled "userspace"); wrong-target angle under-tracked. |
| vms-5c6d | **`sys$close` on an indexed file discards records behind a success status.** Raw `free(fab->_rms_state)` instead of `rms_idx_cleanup`/`_flush` (which exist) → returns `RMS$_NORMAL` while dropping every record since the last `%100` periodic save. **Silent data loss.** | `rms_core.c:832-835` | Tracked, **still live**. |
| **NEW** | **XAB dates are a raw Unix `time_t` in VMS's 1858-epoch quadword.** `xab$q_cdt = st.st_ctime` — off by ~112 years *and* wrong units (VMS = 100ns since 17-NOV-1858). The correct converter (`lib_datetime.c`) sits unused three dirs away. Any VMS tool reading the XAB gets garbage. | `rms_core.c:1002-1003` | **Untracked.** |
| vms-2f0 | **Invented boot line `%STDRV-I-STARTUP, OVMX startup completed`.** Real STDRV prints `begun`, never `completed`; and here the begun/completed pair fires *after* `run_startup()` already returned — bracketing nothing. Banner also prints *after* startup output (VMS prints it first). | `ovmx_init.c:829,838,970` | Tracked, **still prints**. |

## Tier 2 — noticed in normal use / structural

| ID | Finding | Evidence | Status |
|---|---|---|---|
| **NEW** | **Logical-name split-brain.** `sys$crelnm`/`sys$trnlnm` for `LNM$GROUP`/`LNM$JOB` use a private in-process array; the DCL `DEFINE` path routes the same tables to the executive. A program's `$CRELNM(LNM$GROUP)` and a `DEFINE/GROUP` **cannot see each other.** | `sys_logical.c:61-68,94` vs `lnm_client.c:336-345` | **Untracked** (vms-e32 covers SYSTEM only, stale). |
| vms-890 (reframe) | **Indexed files are a flat Unix file + `.rms_idx`/`.rms_meta` sidecars ("IDX1"/"RMS1" magic), not ISAM.** The B-tree is real in RAM; on disk there is no prologue/area/bucket/VBN structure — nothing a VAX can parse. RMS attributes live in a companion file, not the file header. | `rms_idx.c:39-41,510-524`; `rms_core.c:101,541-611` | Only sidecar *perms* tracked; the not-real-ISAM framing is new. |
| vms-0f3 (scope) | **"Real ODS-2 volume" overreach.** Genuine byte-exact ODS-2 exists only in the *served/interop* artifact a VAX MOUNTs (`src/vmsfs/ods2/`). The **live** RMS store is POSIX files (or the non-genuine `"VMFS"/"VFH2"` kernel fs). The genuineness is real; the implied claim that the running FS *is* that volume is not. | `vmsfs_ondisk.h:38-39` | Framing note. |
| vms-407 | **RMS record locking accepted and ignored.** FAB share bits / RAB lock ROPs never reach a lock manager; `sys$connect` returns success. `_rms_stream` is never allocated, so the connect/disconnect lifecycle is hollow. | `rms_core.c:19-60,923-928` | Tracked. |
| vms-642 | **$GETSYI reports per-process `uname()` as system/cluster-wide SYI params.** `vms_kif_getsyi` unused; cluster params fabricated. | `sys_misc.c:116-127,208-235` | Tracked. |
| vms-70eb | **SHOW PROCESS prints a hardcoded `LEF` scheduler state** and self-declared name; `/ALL` emits one row. Item's own directive ("do not reintroduce LEF") violated in code. | `dcl_cmd_show.c:733` | Tracked, still live. |
| vms-46c | **STDRV has no phased driver behind it** — "STARTUP phases" are one `.COM` calling another; zero `VMS$*` phase files. `SYLOGICALS.CONF` is a Unix config file standing in for VMS's `SYLOGICALS.COM` procedure. `%STARTUP-I-*`/`%SYSBOOT-I-*` lines wear invented facilities for real mount/install events (VMS says `%MOUNT-I-*`). | `ovmx_init.c:413,444,714`; `ovmx_layout.h:87` | Tracked/new mix. |
| $HIBER / $SETPRI / SET UIC | `$HIBER` = `pause();return SS$_NORMAL` (local, `sys_process.c:429`); `$SETPRI` reprioritizes *itself*, discards target (`:75-77`); `SET UIC` writes the DCL context struct only — a writer whose sole reader is itself (`dcl_cmd_set.c:801`). | — | vms-pt1/vms-012. |

## The meta-facade — tests that guard the facades

| ID | Finding | Evidence | Status |
|---|---|---|---|
| **NEW** | **`test_no_unix_leaks.sh` cannot fail.** Its name is a headline authenticity claim; its only assertion is `contains:LEAK_CHECK_COMPLETE`, a token it always prints last. It *finds* leaks and downgrades them to "WARNING"; the real guard (`UNIX_LEAK_DETECTED`) is commented out and never emitted. | `tests/dcl/test_no_unix_leaks.sh:2,9` | **Untracked.** |
| vms-3bb | **`test_mount.sh` is vacuous** — SHOW DEVICE output never asserted; all device strings come from MOUNT's own hardcoded echo (`dcl_cmd_misc.c:1689`). | `tests/dcl/test_mount.sh` | Tracked, still vacuous. |
| **NEW** | **Tautology-test family** — `test_tcpip_show_version.sh`, `test_help_content.sh`, `test_show_memory.sh`, `test_sysgen.sh` grep for the product's own hardcoded banners / values; `test_vms_messages.sh` returns OK when a command emits **no** error (a dropped message passes clean). All trace to the self-emitted-token framework in commit `f7a0169e`. | `tests/dcl/*` | **Untracked.** |

## Now genuinely wired — DO NOT re-file

Event flags (`vms-afc`), lock manager (`vms-042`), `LNM$SYSTEM` (`vms-d37`), `LNM$GROUP/JOB` DCL
path (`vms-aba`), mailboxes (`vms-d44`), ASTs (`vms-as1`), `sys$getjpi` service (`vms-pt1`),
`SHOW SYSTEM/USERS/PROCESS` scan (`vms-8019`), AUTHORIZE/MAIL identity (`vms-b2e`/`vms-2d39`),
corpus baseline + run-pass floor (`vms-801` R2.0), reloc property-gate (`vms-49bb`),
runtime-target no-allowlist. The negative-control CI shards are exemplary anti-facade gates.

## Remediation shape (proposed)

- **Tier 0 → security lane / R4.** SSH cred-drop + privilege-aware file enforcement are the two
  that must not ship into a cluster. The SSH item is filed this session.
- **Surface facades (F$GETJPI lexical, SHOW PROCESS `LEF`, SET UIC, the DCL test suite) → fold
  under the DCL fidelity pillar `vms-b9a`** — same subsystem, same INV-DCL invariant.
- **Executive process-control-by-PID (the VMS-pid→`kill()` class) → under `vms-6b8`** — needs a
  real vms_kif control path, not a userspace `kill()` shortcut.
- **RMS (close-drops-records, XAB epoch, sidecar-not-ISAM) → RMS fidelity items**, sequenced with
  the RMS/PARTS work; the close-drop and XAB-epoch are the two to do first (data-loss + wrong-by-a-century).
- **Meta: re-arm the DCL test suite** — every `*_OK`/`*_COMPLETE` self-token test re-authored to
  assert real behavior; `test_no_unix_leaks.sh` is the priority (it guards the headline).
