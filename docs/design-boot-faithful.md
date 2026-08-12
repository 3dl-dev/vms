# A VMS-faithful bootstrap for OVMX

**Status:** design record, oracle-complete (2026-08-07).
**Prior work:** `docs/design-init-scope.md` (boot-chain research, PID 1 scope),
`docs/design-vms-faithful-install.md` + epic `vms-718` (installation factored
out of init). This document covers what remains: making the boot *itself* —
its mechanisms and its console behavior — faithful.
**Oracle:** OpenVMS Alpha V8.4, lab-alpha scratch clone of the golden image,
2026-08-07. Conversational boot (`-flags 0,1` → `SYSBOOT>`), full clean boot to
login, and post-login startup-driver inventory, captured at
`/data/training/vax/alpha/captures/alpha84-clean-boot-conversational-2026-08-07.log`.
Clean-room Rule 8 throughout: behavior, data files, and directory listings
only; **no VSI `.COM`/`.EXE` content was read** (the capture agent enforced
this; `VMS$PHASES.DAT` is a data file — a list of phase names — and was read).
Golden image md5-verified untouched after the run.

---

## 1. The VMS boot chain vs. OVMX today

From `docs/design-init-scope.md` §1, updated with the vms-718 outcome:

| VMS stage | Job | OVMX analogue today | After vms-718 |
|---|---|---|---|
| Console firmware / VMB(APB) | load the secondary bootstrap | QEMU + Linux kernel + initramfs | unchanged (base layer) |
| **SYSBOOT** | read the **parameter file**, size the executive, load it; **conversational boot** at `SYSBOOT>` | `executive_attach()` loads vms.ko — but **no parameter file is read, ever**; no conversational boot | same gap |
| EXEC_INIT | init executive, construct system process | `establish_system_identity()` (PID 1 reads SYSUAF — `vms-a17e` moves this into the executive) | same gap, tracked |
| SYSINIT | mount system disk, open page/swap, create STARTUP process | mount `DKA0:` in PID 1 | mount-or-halt (`vms-2f0`) |
| **STARTUP process** | DCL runs SYS$SYSTEM:STARTUP.COM = **STDRV, a phased startup driver** reading component data files; runs the documented site files; creates the detached system processes; **terminates like a job, with accounting** | flat `STARTUP.COM` defining two logicals and calling SYSTARTUP_VMS.COM | same gap |
| JOB_CONTROL | create login processes on terminals | **DONE (`vms-8d2`)**: `JOB_CONTROL.EXE`, a real DETACHED process created by `SYSTARTUP_VMS.COM` via `RUN/DETACHED`, owns the login loop | closed |
| LOGINOUT | authenticate | `vms_login` | works |

## 2. Measured gaps (desk research, 2026-08-07)

### 2.1 The parameter subsystem is disconnected from boot

- `tools/vms_sysgen.c` implements USE/SHOW/SET/WRITE against
  `/etc/ovmx/sysparams.dat` — a **Linux path**, not a file on the system disk
  named by a VMS filespec.
- **No boot component reads it.** grep: the only consumer is `src/vmsscs/scsd.c`
  (cluster daemon). SYSBOOT's defining job — configure the system from the
  parameter file before anything runs — has no OVMX analogue.
- `ovmx_init.c` hardcodes `sethostname("OVMX", 4)`; on VMS the node name is the
  `SCSNODE` **parameter** (§3.1 shows it live in SYSBOOT's own table).
- The real file, measured (§3.4): `SYS$SYSROOT:[SYSEXE]ALPHAVMSSYS.PAR` — with
  **file versions** (`;2`, `;1`) and an `AUTOGEN.PAR` beside it: parameter
  writes create new versions, and AUTOGEN has its own working file. OVMX's
  must be OVMX-named and labeled (Rule 8): `SYS$SYSTEM:OVMXVMSSYS.PAR`.

### 2.2 Conversational boot does not exist

Measured: `boot -flags 0,1` halts SYSBOOT **before any banner** at a
`SYSBOOT>` prompt; parameters can be inspected and set; `CONTINUE` resumes.
See §3.1 for the verbatim capture including the parameter-table column format.
Two loud facts: **the startup procedure is itself a parameter**
(`SHOW /STARTUP` → `Startup command file = SYS$SYSTEM:STARTUP.COM`), and OVMX's
platform equivalent of the boot-flag register is the kernel command line.

### 2.3 The startup is a flat script, not a startup driver

VMS's STARTUP.COM is STDRV — a phased driver over component data files in
SYS$STARTUP, running the documented site procedures and creating the detached
system processes. Measured (§3.2, §3.3): `SYS$STARTUP` is a **search list**
(`SYS$SYSROOT:[SYS$STARTUP]`, then `SYS$MANAGER`); the phase list is nine
entries; the component database is `VMS$VMS.DAT` + `VMS$LAYERED.DAT`; the
site-file set (`SYCONFIG.COM`, `SYLOGICALS.COM`, `SYSTARTUP_VMS.COM`, …) lives
in SYSMGR. File *names* and the phase *data file* are observed; VSI procedure
*content* was not read — OVMX authors its own (Rule 8).

OVMX today: `STARTUP.COM` defines `SYS$STARTUP` as an alias of SYS$MANAGER
(wrong shape — no search list, no real [SYS$STARTUP] directory), runs
`SHOW TIME`, invokes `SYSTARTUP_VMS.COM`. No phases, no component files, no
site-file set.

### 2.4 Unix configuration files wearing VMS paths

- `SYS$MANAGER:OVMX.CONF` — `KEY=VALUE` shell idiom (VMS_ROOT, NODE_NAME,
  VERSION_LIMIT, DEFAULT_PROTECTION). Every key is a SYSGEN parameter in VMS
  terms (NODE_NAME → SCSNODE) or belongs in a site procedure. The file class
  itself is the LARP.
- `SYS$MANAGER:SYLOGICALS.CONF` — VMS's file is `SYLOGICALS.COM`, a command
  procedure, not a config file.

### 2.5 Console message sequence — including one invented OVMX message

OVMX's boot narrative today (`ovmx_init.c` + `STARTUP.COM`): `%OVMX-I-EXEC` →
`%STARTUP-I-SYSDISK/MOUNTED` → identity line → STARTUP.COM output → product
banner → `%STDRV-I-STARTUP` begun **and completed** → login loop.

The Alpha oracle (§3.5) says:

- The **OS banner prints first**, immediately after SYSBOOT hands over —
  before any startup output. OVMX prints its banner *after* startup; wrong
  placement.
- `%STDRV-I-STARTUP, OpenVMS startup begun at <t>` is printed **once**. There
  is **no "completed" counterpart anywhere in the boot** — OVMX's
  `%STDRV-I-STARTUP, OVMX startup completed` line is an invention (it was
  never oracle-backed; noted on `vms-2f0`, whose fix is now: keep `begun`
  before the procedure, **delete** the completed line).
- The startup announces the site phase with a real line: `The OpenVMS system
  is now executing the site-specific startup commands.`
- The STARTUP process **ends as a job**: `SYSTEM job terminated at <t>` plus a
  full accounting block — then the `Welcome to OpenVMS (TM) ...` banner and
  `Username:`.
- Detached process creation prints `%RUN-S-PROC_ID, identification of created
  process is <pid>`; OPCOM announces itself with `%%%%%%%%%%%  OPCOM` blocks.

## 3. Oracle captures (verbatim excerpts; full log in the lab archive)

### 3.1 SYSBOOT — conversational boot and the parameter table

```
P00>>>boot -flags 0,1 dqa0
(boot dqa0.0.0.15.0 -flags 0,1)
block 0 of dqa0.0.0.15.0 is a valid boot block
reading 1230 blocks from dqa0.0.0.15.0
bootstrap code read in
...
jumping to bootstrap code

SYSBOOT> SHOW /STARTUP
  Startup command file = SYS$SYSTEM:STARTUP.COM
SYSBOOT> SHOW SCSNODE
Parameter Name            Current    Default     Min.       Max.   Unit  Dynamic
--------------            -------    -------   -------    -------  ----  -------
SCSNODE                 "ALPHA1  "    "    "    "    "     "ZZZZ" Ascii
SYSBOOT> SHOW STARTUP_P1
Parameter Name            Current    Default     Min.       Max.   Unit  Dynamic
--------------            -------    -------   -------    -------  ----  -------
STARTUP_P1                  "    "    "    "    "    "     "zzzz" Ascii
SYSBOOT> CONTINUE
```

No banner precedes `SYSBOOT>`. String parameters render quoted and
space-padded; the table columns are as shown.

### 3.2 SYS$STARTUP is a search list

```
$ SHOW LOGICAL SYS$STARTUP
   "SYS$STARTUP" = "SYS$SYSROOT:[SYS$STARTUP]" (LNM$SYSTEM_TABLE)
        = "SYS$MANAGER"
1  "SYS$MANAGER" = "SYS$SYSROOT:[SYSMGR]" (LNM$SYSTEM_TABLE)
```

### 3.3 The startup driver's files

```
$ TYPE SYS$STARTUP:VMS$PHASES.DAT
INITIAL
DEVICES
PRECONFIG
CONFIG
BASEENVIRON
LPBEGIN
LPMAIN
LPBETA
END
```

`DIRECTORY SYS$STARTUP:*.DAT` (trimmed to the VMS core): in
`SYS$COMMON:[SYS$STARTUP]` — `VMS$PHASES.DAT;1` (1 block),
`VMS$VMS.DAT;1` (102 blocks — the component database), `VMS$LAYERED.DAT;1`
(36 blocks — layered-product components). The `.COM` population of
`[SYS$STARTUP]` (names only, 72 files) is dominated by per-phase drivers named
`VMS$INITIAL-*`, `VMS$CONFIG-*`, `VMS$BASEENVIRON-*`, `VMS$LPBEGIN-*`,
`VMS$END-*`, plus per-facility `<facility>$STARTUP.COM`. SYSMGR holds the site
files (`SYCONFIG.COM`, `SYLOGICALS.COM`, `SYSTARTUP_VMS.COM`, `SYSHUTDWN.COM`,
`LOGIN.COM`, `SYLOGIN.COM`, …).

### 3.4 The parameter file

```
$ DIRECTORY/SIZE/DATE SYS$SYSTEM:*.PAR
Directory SYS$SYSROOT:[SYSEXE]
ALPHAVMSSYS.PAR;2         22   5-AUG-2026 19:49:51.37
ALPHAVMSSYS.PAR;1         22   5-AUG-2026 19:38:39.55
AUTOGEN.PAR;1             22   5-AUG-2026 20:01:09.36
```

### 3.5 The clean boot, `CONTINUE` → `Username:` (load-bearing excerpts)

```
SYSBOOT> CONTINUE

    OpenVMS (TM) Alpha Operating System, Version V8.4
    (c) Copyright 1976-2010 Hewlett-Packard Development Company, L.P.

%STDRV-I-STARTUP, OpenVMS startup begun at  7-AUG-2026 18:51:27.24
%RUN-S-PROC_ID, identification of created process is 00000104
%%%%%%%%%%%  OPCOM   7-AUG-2026 18:51:42.61  %%%%%%%%%%%
Operator _ALPHA1$OPA0: has been enabled, username SYSTEM
...
%SET-I-NEWAUDSRV, identification of new audit server process is 0000010A
...
%STARTUP-I-AUDITCONTINUE, audit server initialization complete

The OpenVMS system is now executing the site-specific startup commands.

%SET-I-INTSET, login interactive limit = 64, current interactive value = 0
%RUN-S-PROC_ID, identification of created process is 00000117
...
  SYSTEM       job terminated at  7-AUG-2026 18:51:54.94

  Accounting information:
  Buffered I/O count:               3601      Peak working set size:       7760
  Direct I/O count:                 1504      Peak virtual size:         186192
  Page faults:                      4072      Mounted volumes:                0
  Charged CPU time:        0 00:00:07.35      Elapsed time:       0 00:00:27.78

 Welcome to OpenVMS (TM) Alpha Operating System, Version V8.4

Username:
```

(The elided lines are unconfigured-subsystem noise on this scratch clone —
missing queue manager, proxy DB, licenses — real messages, not part of the
faithful-boot core. Full text in the archived capture.)

### 3.6 The fresh-boot process population (Alpha 8.4)

```
$ SHOW SYSTEM
OpenVMS V8.4  on node ALPHA1    7-AUG-2026 19:26:06.11   Uptime  0 00:34:39
  Pid    Process Name    State  Pri      I/O       CPU       Page flts  Pages
00000101 SWAPPER         HIB     16        0   0 00:00:00.02         0      4
00000105 FASTPATH_SERVER HIB     10        9   0 00:00:00.02        78     95
00000106 IPCACP          HIB     10        9   0 00:00:00.02        37     51
00000107 ERRFMT          HIB      8       87   0 00:00:00.08       115    136
00000109 OPCOM           HIB      8       54   0 00:00:00.03        93     42
0000010A AUDIT_SERVER    HIB     10       62   0 00:00:00.04       143    148
0000010B JOB_CONTROL     HIB     10       39   0 00:00:00.04        49     75
0000010F SECURITY_SERVER HIB     10       68   0 00:00:00.50       423    423
00000110 ACME_SERVER     HIB     10       71   0 00:00:00.24       506    424 M
00000112 TP_SERVER       HIB     10      145   0 00:00:00.41        80    104
00000117 SMHANDLER       HIB      8       57   0 00:00:00.08       160    175
00000119 SYSTEM          CUR   0  4      108   0 00:00:00.21       339    146
```

Twelve processes on a bare Alpha 8.4 (vs. twenty on the VAX 7.3 capture in
`docs/oracle/vax73-show-system-process.md` — the set is version- and
config-dependent, which is itself the finding: the population comes from what
the startup *actually started*, not from a fixed roster).

### 3.7 vms-1fb: banner-first fix + per-message facility audit (2026-08-12)

Implements target-shape item 3 (§4) against §2.5/§3.5. Two code changes in
`src/ovmx_init/ovmx_init.c`, plus a per-message audit of every boot line that
file prints.

**Banner-first.** `display_boot_banner()` used to be called once, at the very
end of `main()`, after `run_startup()` had already run STARTUP.COM to
completion — last, not first, the exact inversion of §3.5. It is now called
(through a new idempotent `print_banner_once()`) from the point in
`bare_metal_init()`'s flagless AND conversational branches that corresponds to
"SYSBOOT just handed over": immediately after `executive_attach()` succeeds,
before the system-disk-mount messages, before the SCSNODE identity line,
before `%STDRV-I-STARTUP`, before any STARTUP.COM output. `print_banner_once()`
is also called as an idempotent fallback right after `main()`'s own
`executive_attach()` gate call, so a substrate that skips `bare_metal_init()`
entirely still shows the banner before STDRV/STARTUP output.

Gating the banner on a *successful* `executive_attach()` — rather than on
literally nothing, which would put it ahead of even a fatal executive-attach
failure — is a deliberate, disclosed divergence from "banner is the literal
first thing, full stop": it is what preserves the executive-is-integral
guarantee (`tests/qemu/test_executive_integral.sh`) that OVMX never shows an
identification banner on a boot that provably never comes up because the
executive itself would not attach. A boot can still halt *after* the banner
if the system-disk mount subsequently fails (SYSINIT-equivalent) —
`tests/qemu/test_persistent_boot.sh`'s negative control was updated to assert
on that honestly (no login prompt, no STDRV begun) rather than on banner
absence, which is no longer the discriminator once the banner moves this
early. This ordering is not a compromise of the oracle shape: SYSBOOT's own
job (load the executive) has already completed by the time `CONTINUE` is
issued in the §3.1/§3.5 captures, so showing OVMX's EXEC_INIT-equivalent
succeed immediately before the banner is consistent with, not a departure
from, the oracle's implied staging.

**STDRV-begun-only.** Re-confirmed, not re-fixed: `%STDRV-I-STARTUP ... begun`
still prints exactly once, before `run_startup()`, and no "completed" line
exists anywhere in `ovmx_init.c` (`grep -i "startup completed"` is empty) —
vms-2f0's deletion holds.

**Timestamp format.** Re-confirmed, not re-fixed: both `print_stdrv_begun()`
and `display_boot_banner()` already used `%2d` for the day field (space-padded
— ` 7`, not zero-padded `07`) and `(int)(ts.tv_nsec / 10000000)` with `%02d`
for the fractional-seconds field (hundredths — `.24`, not milliseconds).
`grep -n "vms_months\|tm_mday\|tv_nsec"` finds no other boot-timestamp call
site in the file. `tests/qemu/test_boot_conformance.sh` asserts the shape
directly (positively, and negatively against the zero-padded form) so a future
regression is caught.

**Per-message facility audit.** Every `printf`/`fprintf` in `ovmx_init.c` that
emits a `%FACILITY-...`-shaped boot message, dispositioned against §3.5:

| Message (facility-severity-ident) | Oracle match? | Disposition | Reasoning |
|---|---|---|---|
| `%STDRV-I-STARTUP, ... startup begun at <t>` | Y (shape; text is brand-substituted `OpenVMX` for `OpenVMS`, INV-0) | pinned | §3.5 verbatim shape; brand substitution is the standing INV-0 policy, not a new deviation |
| `%OVMX-I-EXEC, VMS executive attached on /dev/vms` | N — no VMS analogue | OVMX-labeled | already correctly `%OVMX-`; VMS's EXEC_INIT never narrates itself on the console, OVMX's does (module load + `/dev/vms` open, both Linux-substrate concepts VMS has no counterpart for) |
| `%OVMX-I-SYSDISK, mounting system disk DKA0:` | N | **fixed: was `%STARTUP-I-SYSDISK`** | §3.5 shows no system-disk-mount narration at all (VMS's SYSINIT mounts silently); borrowing the real `%STARTUP-` facility for content the oracle never shows dressed an OVMX-only event as VMS output — the exact defect class this audit was asked to catch. Relabeled `%OVMX-` |
| `%OVMX-I-MOUNTED, system disk DKA0: mounted` | N | **fixed: was `%STARTUP-I-MOUNTED`** | same reasoning as SYSDISK, immediately above |
| `%OVMX-W-MODFAIL, failed to load vmsfs.ko: <errno>` | N | **fixed: was `%STARTUP-W-MODFAIL`** | a Linux kernel-module load failure has no VMS analogue at all (VMS loads no such thing); same borrowed-facility defect, relabeled `%OVMX-` |
| `%OVMX-I-SCSNODE, node name <n> set from SYS$SYSTEM:OVMXVMSSYS.PAR` | N | OVMX-labeled | already correctly `%OVMX-`; no oracle line for this exact SYSBOOT-parameter-applied announcement |
| `%OVMX-W-NOPARAMS` / `-OVMX-I-NOPARAMS` (missing/corrupt parameter file) | N | OVMX-labeled | already correct; no oracle capture of a missing/corrupt `ALPHAVMSSYS.PAR`, so no VMS message is invented for it (Rule 10) |
| `%EXECINIT, error loading system file - <FILE> R0 = <status>` | Y — pinned to a *different*, cited oracle (OpenVMS VAX 7.3, `execinit_halt()`'s own header) | pinned | reproduces the VAX capture byte-exact; not part of the Alpha §3.5 happy path, but its own oracle citation stands |
| `%OVMX-F-EXECINIT` / `%OVMX-I-EXECINIT` (non-ENOENT exec-attach failure) | N | OVMX-labeled | already correct; Rule 10 — a condition VMS is never in gets no VMS-shaped message |
| `%OVMX-F-SYSINIT` / `%OVMX-I-SYSINIT` (system-disk mount-or-halt failure) | N | OVMX-labeled | already correct; same Rule 10 reasoning, and named for the correct VMS *stage* (SYSINIT) without claiming to be a VMS *message* |
| `%OVMX-E-NOIMG, cannot activate SYS$SYSTEM:PROVISION.EXE: <errno>` | N | OVMX-labeled | already correct; an `execve()` failure reported has no VMS analogue |

No message in `ovmx_init.c` was dispositioned **deleted** — the one invented
message this file ever carried (`%STDRV-I-STARTUP ... completed`) was already
removed by vms-2f0; this audit found no second one. Two messages that ARE a
console-visible boot artifact but are emitted by `STARTUP.COM`/
`SYSTARTUP_VMS.COM`, not `ovmx_init.c`, are out of this file's audit scope by
the item's own definition and were left untouched: the free-text (no
`%FACILITY-` shape) line `The OVMX system is now executing the site-specific
startup commands.` currently prints **twice** — once from `STARTUP.COM`'s
`RUN_SITE_STARTUP` before it invokes `SYSTARTUP_VMS.COM`, once more from
`SYSTARTUP_VMS.COM`'s own trailing `WRITE` (added later, for a test grep, per
its header comment) — the oracle (§3.5) shows it once. Flagged here, not
fixed here (out of `ovmx_init.c`), and not load-bearing for
`test_boot_conformance.sh` (its sequence extraction keys on `%FACILITY-`
tokens, which this free-text line has none of).

The full pinned boot-facility sequence `test_boot_conformance.sh` diffs
against (flagless boot, mastered disk, up to the login prompt), measured
against a real QEMU boot rather than assumed, is, in order: `%OVMX-I-EXEC`
(executive attach, `ovmx_init.c`) → banner → `%OVMX-I-SYSDISK` →
`%OVMX-I-MOUNTED` → `%OVMX-I-SCSNODE` → `%STDRV-I-STARTUP` → a *second*
`%OVMX-I-EXEC` (PROVISION.EXE's "system identity ... established by the
executive", `src/ovmx_provision/ovmx_provision.c` — a different message under
the same ident, not a repeat) → seven `%INSTALL-I-ADDED` lines
(`SYSTARTUP_VMS.COM`'s `INSTALL ADD` of every OVMX shareable) →
`%RUN-S-PROC_ID` (JOB_CONTROL's creation — an END-phase STDRV component as
of §3.8/vms-2a9, unchanged in ordering from before that item) → `Username:`.
The PROVISION.EXE and
INSTALL ADD messages are outside `ovmx_init.c`'s own audit table above (they
are not `ovmx_init.c` boot messages), but they are real console output this
test's sequence extraction — which keys on the `%FACILITY-SEVERITY-IDENT`
shape everywhere in the transcript, not just this file's own prints — legitimately
captures. This is still deliberately sparser than the §3.5 oracle (no OPCOM,
no `%SET-I-NEWAUDSRV`/audit server, no `%SET-I-INTSET`, no job
termination/accounting block) — OVMX has none of those facilities yet (§5,
out of scope), and faking their messages to look busier would be the §2.5
defect in reverse (Rule 10).

### 3.8 vms-2a9: JOB_CONTROL becomes a real STDRV component (2026-08-12)

Implements target-shape item 5 (§4) against the OUTCOME `vms-2a9` states:
JOB_CONTROL must be created *by the startup phases themselves* — the STDRV
component-registration mechanism §3.3/§3.4 of `STARTUP.COM`'s own header
already implements (`RUN_COMPONENTS`, driven by `SYS$STARTUP:VMS$VMS.DAT`)
— not by a line hardcoded into a site file that happens to run inside a
phase.

**Before this item:** `SYS$MANAGER:SYSTARTUP_VMS.COM` (the LPMAIN-phase site
file) directly ran `@SYS$STARTUP:JOB_CONTROL_STARTUP.COM`. Functionally that
did create a real detached JOB_CONTROL process (`vms-8d2`'s own e2e proof,
`tests/qemu/test_job_control_console.sh`, still passes unchanged) — but the
component-registration mechanism `VMS$VMS.DAT` exists to drive had ZERO
registered entries, so nothing in the phase driver's own data actually
governed JOB_CONTROL's creation; a manager wanting to disable it would have
had to edit a site `.COM` file's DCL, not a declarative registration.

**After this item:** `SYS$STARTUP:VMS$VMS.DAT` carries one line —
`END SYS$STARTUP:JOB_CONTROL_STARTUP.COM` — and `SYSTARTUP_VMS.COM` no
longer calls it. STARTUP.COM's existing `RUN_COMPONENTS` subroutine (§3.3,
unchanged) now does the only invoking, at the END phase — the LAST of the
nine.

**Phase choice, and a real regression this item measured and reversed
before landing (Rule 6/7).** VSI never publishes which phase internally
starts the job controller — no capture in this project's oracle archive
shows it, and none was sought (Rule 8 forbids reading VSI `.COM` content to
find out) — so any phase choice here is a labeled OVMX design choice, not
an oracle capture. CONFIG (an early phase) was tried FIRST, on the
reasoning that JOB_CONTROL is a core OpenVMS system service rather than a
layered product or a site choice, and therefore belongs before the
LPBEGIN/LPMAIN/LPBETA boundary the VSI OpenVMS System Manager's Manual's
"Customizing Startup with Site-Specific Files" reserves for those — plus
§3.6's Alpha 8.4 `SHOW SYSTEM` capture ordering JOB_CONTROL's PID
(`0000010B`) well ahead of the interactive SYSTEM session's (`00000119`).

That reasoning turned out to be untestable-as-stated and, worse, WRONG in
its practical effect. Running `tests/qemu/test_boot_conformance.sh` against
a real QEMU boot with JOB_CONTROL registered at CONFIG (baseline-before-
changing, Rule 6) showed the actual console transcript reading `Username:
The OVMX system is now executing the site-specific startup commands.` on
one line — JOB_CONTROL's own LOGINOUT child had already begun prompting
for a username on the shared console WHILE STARTUP.COM's phase driver was
still writing to that same console (`SYCONFIG.COM`, `SYLOGICALS.COM`,
`SYSTARTUP_VMS.COM`'s `INSTALL ADD` lines and site announcement) at later
phases. Two processes racing on one physical console interleaved their
output — a real defect, not a documentation mismatch, and exactly the kind
of thing ground-source testing (CLAUDE.md Rule 7) exists to catch before it
ships.

**END is the phase actually used, and it is not a compromise — it
reproduces this project's own already-tested-safe prior behavior.** Before
this item, JOB_CONTROL was created by a hardcoded call that was, in
practice, the LAST real action of the LPMAIN phase (after
`SYSTARTUP_VMS.COM`'s own `INSTALL ADD` block, with only a duplicate
`WRITE` and an `EXIT` after it) — so nothing else ever wrote to the console
after JOB_CONTROL's LOGINOUT child began prompting. Registering the
component at END, STDRV's last phase, reproduces that exact ordering: by
construction, RUN_PHASES has already run every other phase's components
and every site file by the time END's `RUN_COMPONENTS` runs. This item's
OUTCOME was to route JOB_CONTROL's creation through the declarative
component mechanism, not to change *when* it starts — END is what
"unchanged" means once that ordering is independently expressed as a
registration rather than left implicit in a hardcoded call's position.

**Console ordering, therefore, is UNCHANGED from before this item, and
`test_boot_conformance.sh`'s pinned sequence needed no reordering** — the
seven `%INSTALL-I-ADDED` lines still precede `%RUN-S-PROC_ID`, exactly as
they did on `main` before `vms-2a9`. `RUN/DETACHED`'s `%RUN-S-PROC_ID`
announcement itself (`src/vmsdcl/dcl_cmd_process.c run_detached()`) was
already present before this item — the OUTCOME's "pin RUN's message to the
§3.5 capture — add it if missing" condition needed no code change, only
verification.

**Ground-source proof, both directions.** Positive: `vms-8d2`'s existing
`tests/qemu/test_job_control_console.sh` boots the real mastered image,
logs in, and reads `SHOW SYSTEM` from a *different* process than the one
that created JOB_CONTROL — unchanged in behavior and in console ordering,
now exercising the component-driven path instead of the hardcoded call
(re-run against the END-phase registration: 15/15 checks pass). Negative
(`tests/qemu/test_job_control_negctl.sh`, new): a second mastered disk
image, built from the same staged system tree with the `VMS$VMS.DAT` END-
phase line removed and nothing else changed, boots to the same `STDRV
begun` / site-specific-startup landmarks but never prints `%RUN-S-PROC_ID`
and never reaches `Username:` — because on OVMX JOB_CONTROL is the *only*
thing that creates the console login loop (`vms-8d2`), so its absence is
observable as the boot silently running out the clock rather than as a
missing row in a `SHOW SYSTEM` nobody could ever run. This is the
strongest available proof that the roster is driven by the component file
and not hardcoded anywhere else in the tree.

**Desk research on a second detached process, and why none was added.** The
OUTCOME asks for JOB_CONTROL "first" and leaves the door open to more. A
survey of every standalone executable in the tree (`add_executable` targets
under `src/*/CMakeLists.txt`) found exactly one other real, fully
implemented, not-yet-started facility: `VMSSSHD.EXE` (`src/vmsssh/`), a real
SSH server with SYSUAF authentication. It is not started anywhere — deleted
from PID 1 by name in `ovmx_init.c`'s own "NOTE ON SERVICES" (the same
comment block this item's mechanism follows) — and a Phase 3 security review
(`vms-cb5`, `tests/integration/test_env_identity_census.sh`) reasoned about
four of its environment-variable writes as safe specifically *because*
"vmssshd ... is not in the runtime image at all." Starting it now would be a
security-posture change this item did not go looking for and is not
authorized to make unilaterally (CLAUDE.md's reserved list: "Security and
confidentiality posture"); it is named here, not silently skipped, so the
choice is visible rather than assumed. Every other §3.6 oracle-roster name
(OPCOM, ERRFMT, AUDIT_SERVER, SECURITY_SERVER, ACME_SERVER, TP_SERVER,
SMHANDLER, FASTPATH_SERVER, IPCACP) has no OVMX implementation at all —
registering a component for any of them would be inventing a process, the
exact §2.5/Rule 10 defect this whole design record exists to kill.

## 4. Target shape

1. **Parameters live on the system disk and the boot reads them.**
   `SYS$SYSTEM:OVMXVMSSYS.PAR` (OVMX-labeled format), versioned by vmsfs like
   the oracle's `;2`/`;1`; SYSGEN's USE/WRITE CURRENT point there via
   filespec; STARTUP.EXE's SYSBOOT role loads it before the executive attaches;
   `sethostname` comes from `SCSNODE`; `scsd` reads the same file. The Linux
   path `/etc/ovmx/sysparams.dat` dies.
2. **Conversational boot.** A kernel-cmdline boot flag (the platform's R5)
   halts STARTUP.EXE at `SYSBOOT>` before the executive attaches:
   SHOW/SET/USE/CONTINUE against the parameter file, `SHOW /STARTUP`
   included, table format byte-shaped to §3.1.
3. **Banner and message order per §3.5.** Banner first; `%STDRV-I-STARTUP ...
   begun` once, **no completed line** (delete the invented one — correction
   filed on `vms-2f0`); the site-specific announcement line where site files
   begin; process creation visible as `%RUN-S-PROC_ID` when RUN/DETACHED runs.
   The STARTUP-process-terminates-with-accounting shape belongs to the STDRV
   item's end state, not to PID 1.
4. **STDRV.** A real `[SYS$STARTUP]` directory; `SYS$STARTUP` as the measured
   search list; STARTUP.COM as a phased driver over OVMX-authored
   `VMS$PHASES.DAT` (same nine phase names — observed data) and an
   OVMX-defined component file (labeled; we never read VSI's `VMS$VMS.DAT`
   internals, only its name/size); the site-file set as `.COM` procedures
   (`SYCONFIG`, `SYLOGICALS`, `SYSTARTUP_VMS`; `SYPAGSWPFILES`/`SYSECURITY`
   only when OVMX grows the facilities they configure — honest omission);
   `OVMX.CONF` and `SYLOGICALS.CONF` eliminated into parameters and
   procedures.
5. **System processes.** The startup phases *themselves* create the detached
   processes OVMX *legitimately has* — JOB_CONTROL first (`vms-8d2` +
   `vms-2a9`, **done**: registered as an END-phase STDRV component in
   `SYS$STARTUP:VMS$VMS.DAT`, run by STARTUP.COM's own `RUN_COMPONENTS`
   subroutine, via `SYS$STARTUP:JOB_CONTROL_STARTUP.COM` and
   `RUN/DETACHED/PROCESS_NAME=JOB_CONTROL` — not a hardcoded call from a site
   file, §3.8) — under real VMS process names, visible in SHOW SYSTEM because
   they exist. Nothing is faked into the roster (Rule 10); §3.6 shows the
   population is whatever startup actually started, and removing a
   component's registration line is sufficient, and necessary, to stop it
   from starting (§3.8's negative control). The SWAPPER-row question stays
   with the authenticity board (`vms-898` / executive-gap). No second real
   detached-process facility exists on OVMX today (desk research for
   `vms-2a9`, 2026-08-12): OPCOM is a library call with no process behind it
   (`src/libvms/syssvc/sys_operator.c`, deferred to `vms-042` Phase 3);
   ERRFMT/AUDIT_SERVER/SECURITY_SERVER/ACME_SERVER/TP_SERVER/SMHANDLER/
   FASTPATH_SERVER/IPCACP have no OVMX implementation at all; VMSSSHD.EXE is
   real but was deliberately left unlaunched by a security review
   (`vms-cb5`) that reasoned about it as unreachable — enabling it is a
   posture change outside this item's scope, flagged for the operator rather
   than decided here.
6. **Boot conformance test.** A tests/qemu transcript test diffing OVMX's boot
   console against the pinned expected sequence, facility by facility, with
   the §3.5 ordering as the reference.

## 5. Out of scope here

Page/swap files (`SYPAGSWPFILES`) and security-server startup — OVMX lacks the
facilities; faking their messages would be the §2.5 defect in reverse.
AUTOGEN. VAX-era standalone behaviors. The install flow (epic `vms-718`).
