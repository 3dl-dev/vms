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
5. **System processes.** The startup phases create the detached processes OVMX
   *legitimately has* — JOB_CONTROL first (`vms-8d2`, **done**: created by
   `SYSTARTUP_VMS.COM` via `SYS$STARTUP:JOB_CONTROL_STARTUP.COM` and
   `RUN/DETACHED/PROCESS_NAME=JOB_CONTROL`) — under real VMS process names,
   visible in SHOW SYSTEM because they exist. Nothing is faked into the
   roster (Rule 10); §3.6 shows the population is whatever startup actually
   started. The SWAPPER-row question stays with the authenticity board
   (`vms-898` / executive-gap).
6. **Boot conformance test.** A tests/qemu transcript test diffing OVMX's boot
   console against the pinned expected sequence, facility by facility, with
   the §3.5 ordering as the reference.

## 5. Out of scope here

Page/swap files (`SYPAGSWPFILES`) and security-server startup — OVMX lacks the
facilities; faking their messages would be the §2.5 defect in reverse.
AUTOGEN. VAX-era standalone behaviors. The install flow (epic `vms-718`).
