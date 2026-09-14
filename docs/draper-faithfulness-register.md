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
>
> **⚠ STATUS RE-GROUNDED 2026-09-14 against `origin/main` code + the compat register
> (`docs/compat/facilities/*.yaml`).** The Status columns below were reconciled to the actual
> code state — a security register that reports a fixed finding as "still live" is worse than no
> register. Most of the Tier-0/Tier-1 security + data-loss findings the 2026-08-11 pass filed as
> live have since been fixed on `main` and are now marked **RESOLVED** with the landing evidence.
> Findings I could not confirm closed against `origin/main` are marked **still live** or
> **status unverified — re-check** (never a stale "live"). Re-run the per-finding `git grep` on
> `origin/main` before acting; the RESOLVED rows cite the file/service that proves the fix.

## The headline

**The authenticity program is working where it has been pointed.** The loudest historical
facades are genuinely fixed on main: the getenv-identity family (AUTHORIZE `vms-b2e`, MAIL
`vms-2d39`), the executive IPC spine (event flags, locks, mailboxes, ASTs, `LNM$SYSTEM`), the
`SHOW SYSTEM/USERS/PROCESS` identity surfaces, and the three big CI gate-facades (corpus
`total:0`, reloc golden re-baselining, runtime-target allowlist). Don't re-file these.

**The facade retreated, it didn't die** — and as of the 2026-09-14 re-grounding it has retreated
further. Of the four pockets the 2026-08-11 pass named, three are now largely closed on `origin/main`:
(1) **process-control and identity by PID** — `$DELPRC/$FORCEX/$SUSPND/$RESUME/$SETPRI`, `F$GETJPI`,
and `$HIBER` now resolve targets through the executive (vms-904/dff7/9e2); (3) the **SSH credential
model** — the root-session hole is closed by a fail-closed UIC drop (vms-49e); and the **DCL test
suite** was re-armed (vms-fe21 et al.). Still open: (2) the **RMS on-disk representation** in part
(the not-real-ISAM / Prolog-3 completeness question), the **logical-name split-brain** for
`LNM$GROUP`/`LNM$JOB`, and residual `uname()`-sourced `$GETSYI` params. See the tier tables for the
per-finding as-built status.

## Tier 0 — SECURITY-CRITICAL (ships into a customer cluster)

| ID | Finding | Evidence | Status |
|---|---|---|---|
| ~~**NEW**~~ | ~~**SSH logs every user in as root.** `vmssshd` authenticates against SYSUAF and stamps the executive identity, then execs DCL with **no `setuid`/`setgid` drop**.~~ **RESOLVED (vms-49e).** SSH now performs a permanent, fail-closed credential drop to the authenticated UIC just before `execl()` — `ovmx_cred_drop_to_uic()` clears supplementary groups, then `setgid` **before** `setuid` (uid-first would silently keep the gid), refusal DENIES the session. Factored out of `vmssshd.c` and unit-tested. | `src/vmsssh/cred_drop.c`, `src/vmsssh/vmssshd.c:588`, `tests/vmsssh/test_cred_drop.c` | **Resolved.** Mirrors console LOGINOUT (`tools/vms_login.c`). |
| vms-f15 / vms-36d | **Privileges enforced for reporting, not file access.** SYSPRV/BYPASS/READALL/GRPPRV were never consulted by the file-permission hook → failed **unsafe** for any euid=0 session. **RESOLVED on the current runtime path (vms-165).** The cited `vmsfs_blkdev.c` VMFS driver was retired; runtime SYS$DISK is now ODS-2 over the executive ACP, and `acp_check_access` (`vmsfs_acp.c:1053`) consults BYPASS (lifts all control), READALL (grants read), and SYSPRV (confers the SYSTEM protection category). | `src/kernel-core/vmsfs_acp.c:995-1062` | **Resolved on the ACP path.** Re-verify the executive supplies the caller's real privilege mask end-to-end. |

## Tier 1 — any VMS user notices immediately / silent data loss / confidently wrong

| ID | Finding | Evidence | Status |
|---|---|---|---|
| vms-9e2 / vms-b5e | ~~**F$GETJPI lexical ignores its pid argument** and answers about the caller.~~ **RESOLVED.** `lex_getjpi` now branches: the null/self form calls `vms_kif_getjpi_self`, and a supplied pid calls `vms_kif_getjpi_pid((uint32_t)pid, &info)` — the executive resolves the target and applies its authorization, so `F$GETJPI(<other pid>, ...)` reports the resolved row, not the caller. | `src/vmsdcl/dcl_lexical.c:1524,1533` | **Resolved.** No longer disagrees with `sys$getjpi`. |
| vms-pt1 (sharpen) | ~~**Process control by PID signals the wrong Linux process.** `$WAKE/$DELPRC/$FORCEX/$SUSPND` cast the VMS pid straight into `kill()` as a Linux pid.~~ **RESOLVED (vms-904 / vms-dff7).** `$DELPRC/$FORCEX/$SUSPND/$RESUME/$SETPRI` route through `resolve_control_target()` (VMS pid or prcnam → the target's real Linux pid via the executive), signal the **resolved** pid, and return the executive's `SS$_NONEXPR` for an absent target — never a raw `kill()` on a mis-cast pid. `$WAKE/$HIBER` go through `vms_kif_wake`/`vms_kif_hiber`. Register rates all `real`. | `src/libvms/syssvc/sys_process.c:1659,1695-1702`; `sys-process.yaml` | **Resolved.** |
| vms-5c6d | ~~**`sys$close` on an indexed file discards records behind a success status.**~~ **RESOLVED.** `sys$close` now calls `rms_idx_cleanup(fab)` (which runs `btree_save()` before freeing) for any file holding an in-memory B-tree, so records added since the last periodic save reach disk instead of being dropped behind `RMS$_NORMAL`. | `src/vmsrms/rms_core.c:2354-2375` | **Resolved.** Silent-data-loss path closed. |
| ~~**NEW**~~ | ~~**XAB dates are a raw Unix `time_t` in VMS's 1858-epoch quadword.**~~ **RESOLVED (vms-3dd).** `dat->xab$q_cdt = unix_time_to_vms(st.st_ctime)` (and `xab$q_rdt` from `st_mtime`); `unix_time_to_vms()` converts to VMS's 100ns-since-17-NOV-1858 quadword via `lib$cvt_vectim`, not a hand-rolled magic number. | `src/vmsrms/rms_core.c:134-152,2824-2825` | **Resolved.** |
| vms-2f0 | ~~**Invented boot line `%STDRV-I-STARTUP, OVMX startup completed`.**~~ **RESOLVED (vms-1fb).** The invented `completed` line is gone — `print_stdrv_begun()` emits only `%STDRV-I-STARTUP, <product> startup begun` once, matching the Alpha 8.4 / VAX 7.3 clean-boot captures (which show `begun` once and no `completed` anywhere); the banner now prints banner-first, before startup narration. | `src/ovmx_init/ovmx_init.c:1585-1621` | **Resolved.** |

## Tier 2 — noticed in normal use / structural

| ID | Finding | Evidence | Status |
|---|---|---|---|
| **NEW** | **Logical-name split-brain.** `sys$crelnm`/`sys$trnlnm` for `LNM$GROUP`/`LNM$JOB` use a private in-process array; the DCL `DEFINE` path routes the same tables to the executive. A program's `$CRELNM(LNM$GROUP)` and a `DEFINE/GROUP` **cannot see each other.** | `sys_logical.c` (LNM$JOB/GROUP still in `logical_table[]`, executive residency deferred) | **Still live for LNM$GROUP/LNM$JOB.** `LNM$SYSTEM` was made executive-resident (vms-d37); GROUP/JOB remain process-private on `origin/main`. |
| vms-890 (reframe) | **Indexed files were a flat Unix file + `.rms_idx`/`.rms_meta` sidecars ("IDX1"/"RMS1" magic), not ISAM.** The B-tree is real in RAM; the sidecar had no prologue/area/bucket/VBN structure. | `rms_idx.c`; `rms_core.c` | **Status unverified — re-check.** `origin/main` now has `rms_idx_author_p3` authoring "a genuine, EMPTY Files-11 Prolog-3 indexed file" over the ACP, with record `$PUT`s riding `rms_p3_put` (NOT the retired sidecar path). Whether the on-disk Prolog-3/bucket structure is complete enough for a VAX to parse is unconfirmed here — re-verify against the ods2/rms register rows. |
| vms-0f3 (scope) | ~~**"Real ODS-2 volume" overreach.** Genuine byte-exact ODS-2 exists only in the *served/interop* artifact a VAX MOUNTs; the **live** RMS store is POSIX files (or the non-genuine `"VMFS"/"VFH2"` kernel fs).~~ **RESOLVED (vms-165).** The legacy `vmsfs.ko` VFS driver and its VMFS/VFH2 format were retired; runtime SYS$DISK is now a genuine Files-11 ODS-2 volume read and written through the executive ACP (`$ASSIGN` + `IO$_ACCESS/READVBLK/WRITEVBLK`), with per-file SOGW protection enforced in `acp_check_access`. | `src/kernel-core/vmsfs_acp.c`, `src/vmsfs/ods2/` | **Resolved.** vmsfs$runtime_fs = implemented/real; ods2$reader = verified/real (byte-exact vs a real VAX volume). |
| vms-407 | ~~**RMS record locking accepted and ignored.** FAB share bits / RAB lock ROPs never reach a lock manager; `sys$connect` returns success.~~ **RESOLVED (vms-50e / vms-0dd).** File-level share arbitration and per-record locking are now real on the ACP path: `sys$open`/`create` map `fab$b_fac`/`fab$b_shr` to a DLM lock mode `$ENQ`'d on the file's FID (`RMS$_FLK` on a real conflict), and each record takes a per-record `$ENQ` child arbitrated by the executive DLM. The one honest degradation is the non-ACP/POSIX-defer handle (`access_lkid==0`), which holds nothing and passes status through. | `src/vmsrms/rms_core.c`, `src/vmsrms/rms_record.c` | **Resolved.** rms$file_share_locking + rms$record_locking = implemented/real in the register. |
| vms-642 | **$GETSYI reported per-process `uname()` as cluster-wide SYI params.** **Cluster params RESOLVED (vms-5919):** `SYI$_CLUSTER_MEMBER`/`SYI$_CLUSTER_NODES` now read the connection manager's own CLUB via `vms_kif_cluster_getsyi` → `cluster_api_getsyi_project`, so `F$GETSYI` and `SHOW CLUSTER` read the SAME executive state. **Partial:** some non-cluster params (SCSSYSTEMID, node name, …) still answer from `uname()`/`sysconf()`. | `src/libvms/syssvc/sys_misc.c` | **Partly resolved.** Cluster-fabrication path closed; remaining `uname()`-sourced params still live. |
| vms-70eb | ~~**SHOW PROCESS prints a hardcoded `LEF` scheduler state** and self-declared name; `/ALL` emits one row.~~ **RESOLVED (vms-70eb / vms-2b8).** The fabricated `/ALL` process table (self-declared name + hardcoded `LEF`) was removed **whole**; `/ALL` now falls through to the plain per-target display with every field read from the target's executive row via `$GETJPI`, plus the one section OVMX can source faithfully (the two privilege blocks). No literal `LEF`, no `getpid()`, no `ctx->process_name`. | `src/vmsdcl/dcl_cmd_show.c:940-975` | **Resolved.** |
| vms-46c | **STDRV had no phased driver** — "STARTUP phases" were one `.COM` calling another; `SYLOGICALS.CONF` was a Unix config file standing in for `SYLOGICALS.COM`. | `src/ovmx_init/ovmx_init.c`; distro rootfs | **Status changed — re-verify.** `origin/main` now ships `SYS$STARTUP/VMS$PHASES.DAT` + `VMS$VMS.DAT` phase files and a real `SYSMGR/SYLOGICALS.COM` (no longer `.CONF`); the invented `%SYSBOOT`/`%STDRV` lines were re-grounded to oracle captures (see vms-2f0 above). Whether a genuine phase *driver* consumes `VMS$PHASES.DAT` is unconfirmed here — re-verify against the boot register. |
| $HIBER / $SETPRI / SET UIC | **$HIBER RESOLVED** — no longer `pause()`; `sys$hiber` blocks in the executive via `vms_kif_hiber()` until a `$WAKE` is pending or an AST is ready. **$SETPRI RESOLVED (vms-dff7)** — resolves the target through the executive and applies the priority to the *resolved* pid (was: reprioritized the caller, discarded the target). **SET UIC: status unverified — re-check** — it now enforces a privilege check, but whether it writes executive-visible state (vs. the DCL context struct only) is unconfirmed. | `src/libvms/syssvc/sys_process.c`; `src/vmsdcl/dcl_cmd_set.c:1483` | $HIBER/$SETPRI resolved; SET UIC to re-check. |

## The meta-facade — tests that guard the facades

| ID | Finding | Evidence | Status |
|---|---|---|---|
| ~~**NEW**~~ | ~~**`test_no_unix_leaks.sh` cannot fail.** Its only assertion is `contains:LEAK_CHECK_COMPLETE`, a token it always prints last.~~ **RESOLVED (vms-fe21).** Re-armed: the guard `EXPECT_NOT: contains:UNIX_LEAK_DETECTED` is now active (was commented out as `FUTURE_EXPECT_NOT`), and any detected leak now sets `UNIX_LEAK_DETECTED` in the output. | `tests/dcl/test_no_unix_leaks.sh:3-14` | **Resolved.** |
| vms-3bb | ~~**`test_mount.sh` is vacuous** — SHOW DEVICE output never asserted.~~ **RESOLVED.** Now asserts the honest state: `EXPECT_NOT: %MOUNT-I-MOUNTED`, `EXPECT_NOT: DUA0:`, `EXPECT_NOT: TESTDISK`, plus `DCL-ALIVE` + an explicit `$STATUS` — i.e. it fails if MOUNT fabricates a mounted device. | `tests/dcl/test_mount.sh:5-10` | **Resolved.** |
| ~~**NEW**~~ | ~~**Tautology-test family** — grep for the product's own hardcoded banners; `test_vms_messages.sh` returns OK when a command emits no error.~~ **RESOLVED.** Re-armed to assert real behavior: `test_vms_messages.sh` now asserts the specific `%FACILITY-SEV-IDENT` per scenario (a dropped message now FAILS); `test_sysgen.sh` does a real SET→SHOW round-trip assertion instead of grepping a self-token. | `tests/dcl/test_vms_messages.sh:4-44`, `tests/dcl/test_sysgen.sh:6-26` | **Resolved.** Re-verify the remaining `test_help_content.sh`/`test_show_memory.sh`/`test_tcpip_show_version.sh` individually. |

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
