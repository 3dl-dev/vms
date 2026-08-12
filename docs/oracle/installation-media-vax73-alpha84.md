# Oracle: what the OpenVMS VAX 7.3 distribution media actually is

**Item:** vms-61d. **Method:** booted the real distribution ISO
(`/data/training/vax/media/openvms073.iso`, volume `VAXVMS073`, DECFILE11B)
on a scratch MicroVAX 3900 in SIMH with a blank RA92 as the target, 2026-08-07.
Console capture: `/data/training/vax/run-install-oracle/console-standalone-backup-2026-08-07.log`.
The lab's own cluster disks were not touched — the scratch node has its own
`vax.ini` and its own blank disk. Behaviour observed from the console only; no
disassembly, no reading of VSI command procedures (CLAUDE.md Rule 8).

**Why this was measured.** A design round for vms-61d (moving installation out
of PID 1) produced three successive wrong architectures — a "kit device" that
was a directory in a ramdisk wearing a made-up device name, a QEMU build stage
to populate a volume, and a userspace vmsfs file writer — all of them invented
to solve a problem that does not exist. The operator's correction was that this
is not new ground and the lab can answer it. It can, and it does.

---

## 1. The media boots into Stand-alone BACKUP

`B DUA2` on the distribution CD boots a real, minimal OpenVMS instance:

```
>>>B DUA2

(BOOT/R5:0 DUA2

-DUA2
  1..0..

%SYSBOOT-I-SYSBOOT Mapping the SYSDUMP.DMP on the System Disk
%SYSBOOT-W-SYSBOOT Can not map SYSDUMP.DMP on the System Disk
%SYSBOOT-W-SYSBOOT Can not map PAGEFILE.SYS on the System Disk
   OpenVMS (TM) VAX Version X7G7 Major version id = 1 Minor version id = 0
%WBM-I-WBMINFO Write Bitmap has successfully completed initialization.
PLEASE ENTER DATE AND TIME
 (DD-MMM-YYYY  HH:MM)  07-AUG-2026 15:00

Configuring devices . . .
Now configuring HSC, RF, and MSCP-served devices . . .
...
Available device  DUA0:                            device type RA92
Available device  DUA2:                            device type RRD40
...
Enter "YES" when all needed devices are available: YES
%BACKUP-I-IDENT, Stand-alone BACKUP T7.2; the date is  7-AUG-2026 15:02:43.07
$
```

Four things to read off that, each of which kills one of the wrong designs:

- **The media is a bootable instance of the OS.** It runs SYSBOOT, maps (or
  fails to map) SYSDUMP/PAGEFILE, prints a version banner, asks for the date,
  and configures devices — the ordinary boot chain. It is not a special
  "installer environment" and not a bootstrap with an OS smuggled inside it.
- **It comes up as a system whose whole job is BACKUP.** The banner is
  `Stand-alone BACKUP T7.2` and the prompt is `$`. There is no installer
  program: there is an OS instance, and you drive BACKUP at it.
- **It degrades honestly.** No SYSDUMP.DMP and no PAGEFILE.SYS on a read-only
  CD, and it says so in `%SYSBOOT-W-` rather than pretending.
- **The version differs from the installed system's.** `X7G7` is the
  standalone-BACKUP kernel, not `V7.3`. The media's instance is its own thing.

## 2. The distribution is an image save set, and it carries the volume

```
$ BACKUP/LIST DUA2:VMS073.B/SAVE_SET
Listing of save set(s)

Save set:          VMS073.B
Written by:        VAXBUILDER
UIC:               [000001,000004]
Date:               2-APR-2001 16:08:50.86
Command:           BACKUP/NOALIAS $11$DUA930: $11$DUA931:[000000]VMS073.B/SAVE_SET
                   /IMAGE/INTERCHANGE/NOINIT/NOREWIND/NOASSIST/IGNORE=LABEL_PROCESSING
Operating system:  VAX/VMS version V7.2
BACKUP version:    VAX72R001
Node name:         _HELENA::
Block size:        32256

Image save of volume set
Number of volumes: 1

Volume attributes
Structure level:   2
Label:             VAXVMSV73
Owner UIC:         [000001,000001]
Creation date:      2-APR-2001 15:56:35.72
Serial number:     04201400005
Total blocks:      8378028
```

The header records the command that BUILT the distribution — `/IMAGE` from a
mounted system volume to a save set — and the save set is an **"Image save of
volume set"** carrying the source volume's own attributes: structure level,
label, owner UIC, creation date, serial number, size.

**Therefore INITIALIZE is not the first step of a VMS system-disk install.**
An image restore creates the target volume from the attributes recorded in the
save set. This corrects the assumption the vms-61d design round was running on
("INITIALIZE the target, then copy"); the volume is a property of the save set,
not something the installer composes beforehand.

## 3. The whole chain

```
1. Boot the distribution media        -> a minimal OpenVMS instance (standalone BACKUP)
2. $ BACKUP/IMAGE <saveset> <target>: -> creates AND populates the target volume
3. Boot the target
4. First boot finishes configuration  (STARTUP.COM and friends)
```

Nothing in it is a program that installs an operating system. It is an OS
instance, a save set, and one BACKUP command.

---

## 3a. ALPHA IS DIFFERENT — and Alpha is the one OVMX should follow

**Everything in §1–§3 above is VAX behaviour and does not generalise.** Measured
on **OpenVMS Alpha V8.4** (`/data/training/vax/media/ALPHA084.ISO`, AXPbox
AlphaServer ES40, scratch node, 2026-08-07; capture at
`/data/training/vax/alpha/captures/alpha84-install-procedure-2026-08-07.log`).
This is exactly the guardrail in `tests/lab-alpha/README.md`: asking a VAX and
concluding "VMS does X" reads an *architecture* limit as an *OS* rule. OVMX is
64-bit, so **Alpha is the relevant oracle and VAX is the historical one.**

**The media boots the full operating system, not standalone BACKUP:**

```
P00>>>boot dqa1
    OpenVMS (TM) Alpha Operating System, Version V8.4
Please enter date and time (DD-MMM-YYYY  HH:MM)  07-AUG-2026 15:00
    Installing required known files...
    Configuring devices...
```

**and then runs a menu procedure:**

```
    You can install or upgrade the OpenVMS ALPHA operating system
    or you can install or upgrade layered products ...
    You can also execute DCL commands and procedures to perform
    "standalone" tasks, such as backing up the system disk.

        1)  Upgrade, install or reconfigure OpenVMS ALPHA Version V8.4
        2)  Display layered products that this procedure can install
        3)  Install or upgrade layered products
        4)  Show installed products
        5)  Reconfigure installed products
        6)  Remove installed products
        7)  Find, Install or Undo patches; Show or Delete Recovery Data
        8)  Execute DCL commands and procedures
        9)  Shut down this system
```

Note option 8: the whole of VAX's standalone-BACKUP environment has become
**one menu item** on a full OS.

**INITIALIZE is a choice inside the procedure, not a precondition:**

```
Do you want to INITIALIZE or to PRESERVE? [PRESERVE] INITIALIZE
Enter device name for target disk: (? for choices) DQA0
Enter volume label for target system disk: [ALPHASYS] ALPHASYS
Do you want to initialize with ODS-2 or ODS-5? (2/5/?) 5
Do you want to enable hard links? (Yes/No/?) YES
Is this OK? (Yes/No) YES

    Initializing and mounting target....
    Creating page and swap files....
```

**Then it configures the system, then installs the OS as a PCSI product:**

```
Password for SYSTEM account: / Re-enter ...
Will this system be a member of an OpenVMS Cluster? (Yes/No) NO
Will this system be an instance in an OpenVMS Galaxy? (Yes/No) NO
Enter SCSNODE: OVMXOR
Do you plan to use DECnet? (Yes/No) [Yes] NO
Enter SCSSYSTEMID: [65534] 65534
  Configuring the Local Time Zone  ... TIME ZONE SPECIFICATION -- MAIN Time Zone Menu
  Configuring the Time Differential Factor (TDF)
Do you want to register any Product Authorization Keys? (Yes/No) [Yes] NO

    The following products are part of the the OpenVMS installation;
    they will be installed along with the OpenVMS operating system:
        o Availability Manager (base) ... CDSA ... KERBEROS ... SSL ...
        o Performance Data Collector (base) ... HP Binary Checker
```

```
The system was booted from a device containing the OpenVMS Alpha distribution.
Validation of signed kits is not supported in this restricted environment.

The following product has been selected:
    DEC AXPVMS OPENVMS V8.4                Platform (product suite)

Configuration phase starting ...
Configuring DEC AXPVMS OPENVMS V8.4: OPENVMS and related products Platform
Configuring DEC AXPVMS VMS V8.4: OpenVMS Operating System
      DECdtm Distributed Transaction Manager [YES]
```

**The operating system is a PCSI kit** — `DEC AXPVMS VMS V8.4`, a member of a
platform product suite — installed by the POLYCENTER Software Installation
utility, the same mechanism as every layered product. There is no image save
set and no `BACKUP/IMAGE` anywhere on this path.

### VAX vs Alpha, side by side

| | VAX 7.3 | Alpha 8.4 |
|---|---|---|
| what the media boots | Stand-alone BACKUP (`X7G7`), `$` prompt | the full OS, V8.4, running a menu procedure |
| how you drive it | you type a BACKUP command | you answer a questionnaire |
| the distribution is | a BACKUP **image save set** carrying volume attributes | a **PCSI product kit** |
| INITIALIZE | not a step — `/IMAGE` creates the volume | an explicit choice; `Initializing and mounting target....` |
| page/swap files | come with the image | `Creating page and swap files....`, done by the installer |
| system config | after first boot | **during** the install (SYSTEM password, SCSNODE, SCSSYSTEMID, TZ, TDF, PAKs) |
| standalone BACKUP | *is* the environment | menu option 8 |

---

## 4. What this means for OVMX (vms-61d)

**Follow Alpha, not VAX.** OVMX is 64-bit; §3a is the model, §1–§3 is history.

On both architectures the distribution is a **file** — a save set on VAX, a
PCSI kit on Alpha — never a pre-populated filesystem. So every design that
tried to write a populated vmsfs volume at build time was solving a
non-problem:

- **No userspace vmsfs file writer is needed.** The build produces a *kit*.
- **No QEMU populate stage is needed.** Same reason.
- **No "kit device" and no fat initramfs.** The media is an OVMX system disk
  like any other, that happens to boot into an installation procedure.
- **`INITIALIZE.EXE` is mkfs and stays mkfs** — and on the Alpha model it *is*
  on the install path, invoked by the procedure (`Initializing and mounting
  target....`) after the operator chooses INITIALIZE over PRESERVE. This
  reverses the VAX-only reading in §2.
- **`STARTUP.EXE` keeps none of this.** It mounts `DKA0:` or halts.

The Alpha shape maps onto OVMX with almost no invention, because the procedure
is a *procedure* — DCL asking questions and driving utilities:

```
boot the distribution disk
  -> OVMX comes up normally (STARTUP.EXE mounts DKA0:, runs STARTUP.COM)
  -> SYSTARTUP_VMS.COM on THAT disk runs the installation procedure:
       INITIALIZE or PRESERVE?  target device?  volume label?
       INITIALIZE <target>          <- SYS$SYSTEM:INITIALIZE.EXE, exists
       MOUNT <target>               <- must become real (see below)
       PRODUCT INSTALL VMS          <- the OS as a kit
       ask SCSNODE / SYSTEM password / TZ, write them to the target
  -> shut down; boot the target; its first boot finishes configuration
```

### Gaps this exposes in the tree, measured

| Gap | Where | State |
|---|---|---|
| `MOUNT` that actually mounts | `src/vmsdcl/dcl_cmd_misc.c:1597` | **facade** — never calls `mount(2)`; writes a per-process device table, uses `getcwd()` as the mount path, prints `%MOUNT-I-MOUNTED`. No `sys/mount.h` in any DCL source. |
| `INITIALIZE` as a DCL verb | `src/vmsdcl/dcl_builtin.c:83` | absent from `builtin_verbs[]`; needs a `cmd_initialize` dispatching to `SYS$SYSTEM:INITIALIZE.EXE` via the existing `dcl_exec_utility()` pattern (ANALYZE/SYSGEN/INSTALL all use it) |
| `PRODUCT` (PCSI) that installs | `dcl_builtin.c` has a `PRODUCT` verb → `cmd_product` | verb exists; whether it installs a kit is unverified |
| an installation procedure | — | absent |

The MOUNT facade is the Rule 9 defect class (a userspace fake reporting success
while sharing nothing) and it blocks everything else: an install cannot be a
procedure until DCL can mount a volume.

### Gaps this exposes in the tree, measured (BACKUP)

| Gap | Where | State |
|---|---|---|
| `BACKUP/IMAGE` — image save/restore incl. volume attributes | `src/vmsdcl/dcl_backup.c:438` | absent; only `/SAVE_SET` and `/LIST` are handled |
| a standalone-BACKUP-equivalent boot mode | — | absent |

---

## 5. First boot of the installed target (vms-490)

**Item:** vms-490, parent vms-718. **Method:** completed the Alpha install
that §3a stopped at the PCSI configuration phase, then booted the freshly
installed target through AUTOGEN, the reboot it triggers, and first
interactive login — all on a scratch node so the golden reference image
(`disks/alpha1-sys.golden.img`, md5 `2d5eea8e2b60e67806735a9bf5ea0b97`,
unchanged by this run) was never touched. Console captures:
`/data/training/vax/alpha/captures/alpha84-firstboot-installed-target-2026-08-12.log`
(this run) and the earlier
`/data/training/vax/alpha/captures/alpha84-install-procedure-2026-08-07.log`
(§3a's stopped run). Observation only, no disassembly (CLAUDE.md Rule 8).

**A config trap worth recording.** The first attempt reused §3a's disk layout
— the CD-ROM as slave on the system disk's own IDE channel (`disk0.1` →
DQA1). It booted the CD and got deep into the PCSI **execution** phase (past
the point §3a stopped at) before failing:

```
%PCSI-E-WRITEERR, error writing DISK$ALPHASYS:[VMS$COMMON.][SYS$LDR]SYS$GVADRIVER.EXE;1
-SYSTEM-F-CTRLERR, fatal controller error
-SYSTEM-W-NOTINTBLSZ, block size is greater than 2048
%PCSI-E-OPFAILED, operation failed
```

— a concurrent CD-read/disk-write failure on the shared IDE channel, the same
"large file transfers between IDE CD-ROM and hard drive problematic" weak spot
`tests/lab-alpha/cfg/alpha1-install-scsi.cfg` documents. `tests/lab-alpha/entrypoint.sh`
(lines 109–115) already carries the measured fix — the layout the golden image
was actually built with: the CD-ROM as **master on the second IDE channel**
(`disk1.0` → DQB0), system disk alone on the first channel. Re-running the
install with that layout (`boot dqb0` for the installer) completed cleanly.
This is now the documented working scratch-install recipe; the stale
`disk0.1`-based `alpha1-install.cfg` should not be used for a full install.

### 5a. What the installer says first boot will do

Right before AUTOGEN runs, the procedure prints its own description of what is
about to happen — this is the installer's own account of the "first boot
finishes the installation" half that §3a/§4 flagged as unmeasured:

```
    AUTOGEN will now be run to compute the new system parameters.  The system
    will then shut down and reboot, and the installation or upgrade will be
    complete.

    After rebooting you can continue with such system management tasks as:

            Decompressing System Libraries (not necessary on OpenVMS I64)
            Configuring networking software (TCP/IP Services, DECnet, other)
            Using SYS$MANAGER:CLUSTER_CONFIG.COM to create an OpenVMS Cluster
            Creating FIELD, SYSTEST and SYSTEST_CLIG accounts if needed
```

Measured behaviour matched this exactly: AUTOGEN ran, the system shut down and
rebooted on its own, and the reboot completed the install.

### 5b. Boot 1 — the post-install boot that runs AUTOGEN

Booting the freshly INITIALIZEd `DQA0:` (`boot dqa0`) is a **normal OpenVMS
boot**, not a special installer environment — it re-asks for date/time (cold
TOY clock in this scratch node) and configures devices exactly like §3a's
media boot:

```
    OpenVMS (TM) Alpha Operating System, Version V8.4
      Copyright 1976-2010 Hewlett-Packard Development Company, L.P.

Please enter date and time (DD-MMM-YYYY  HH:MM)  12-AUG-2026 14:39

    Installing required known files...

    Configuring devices...
```

But this boot's STARTUP pass is **structurally reduced** — it never prints the
`%STDRV-I-STARTUP` banner boot 2 does, and it explicitly defers the security
subsystem rather than starting it:

```
%SYSTEM-I-BOOTUPGRADE, security auditing disabled
%JBC-E-OPENERR, error opening SYS$COMMON:[SYSEXE]QMAN$MASTER.DAT;
-RMS-E-FNF, file not found
%LICENSE-F-EMTLDB, license database contains no license records
%SYSTEM-I-BOOTUPGRADE, security server not started
%SYSTEM-I-BOOTUPGRADE, ACME server not started
TDF-I-SETTDF TDF set new timezone differential
%LICENSE-E-NOAUTH, DEC OPENVMS-ALPHA use is not authorized on this node
-LICENSE-F-NOLICENSE, no license is active for this software product
Startup processing continuing...
%SYSTEM-I-BOOTUPGRADE, Coordinated Startup not performed

CDSA-I-InitCDSA, Initializing CDSA...
MDS installed successfully.
Module installed successfully.  [x11]
CDSA-I-InitCDSA, CDSA Initialization complete
CDSA-I-InitSecDel, Initializing Secure Delivery...
CDSA-I-InitSecDel, Secure Delivery Initialization complete
```

Every `%SYSTEM-I-BOOTUPGRADE` message names a subsystem this boot is
*skipping* — the `%LICENSE-E-NOAUTH` is the lab's known unlicensed state (see
`tests/lab-alpha/README.md` "Still open"), not a first-boot artifact. Then
AUTOGEN itself, observed end to end through every phase:

```
%AUTOGEN-I-BEGIN, GETDATA phase is beginning.
%AUTOGEN-I-NEWFILE, A new version of SYS$SYSTEM:PARAMS.DAT has been created.
%AUTOGEN-I-END, GETDATA phase has successfully completed.
%AUTOGEN-I-BEGIN, GENPARAMS phase is beginning.
%AUTOGEN-I-NEWFILE, A new version of SYS$MANAGER:VMSIMAGES.DAT has been created.
%AUTOGEN-I-END, GENPARAMS phase has successfully completed.
%AUTOGEN-I-BEGIN, GENFILES phase is beginning.
%SYSGEN-I-CREATED, SYS$SYSROOT:[SYSEXE]SYS$ERRLOG.DMP;2 created
%SYSGEN-I-EXTENDED, SYS$SYSROOT:[SYSEXE]PAGEFILE.SYS;1 extended
%AUTOGEN-I-END, GENFILES phase has successfully completed.
%AUTOGEN-I-BEGIN, SETPARAMS phase is beginning.
%AUTOGEN-I-END, SETPARAMS phase has successfully completed.
%AUTOGEN-I-BEGIN, REBOOT phase is beginning.
%SHUTDOWN-I-BOOTCHECK, performing reboot consistency check...
%SHUTDOWN-I-CHECKOK, basic reboot consistency check completed
%SHUTDOWN-I-OPERATOR, this terminal is now an operator's console
%SHUTDOWN-I-DISLOGINS, interactive logins will now be disabled
%SHUTDOWN-I-STOPQUEUES, the queues on this node will now be stopped
%SHUTDOWN-I-STOPUSER, all user processes will now be stopped
%SHUTDOWN-I-REMOVE, all installed images will now be removed
%SHUTDOWN-I-DISMOUNT, all volumes will now be dismounted
```

AUTOGEN's five phases — GETDATA, GENPARAMS, GENFILES, SETPARAMS, REBOOT — ran
in that exact order, each announced with a matching `-BEGIN`/`-END` pair, and
the REBOOT phase drove its own SHUTDOWN sequence rather than handing control
back to a script. **The reboot that follows is fully automatic**: the SRM
console shows `CPU 0 booting` and re-boots `dqa0` on its own — no operator
`boot` command, confirming the console firmware's default boot device was set
during the install (not the CD, and not left unset).

### 5c. Boot 2 — the boot that actually starts the system

Booting again from the AUTOGEN-computed parameters is the mirror image of
boot 1: **no date/time prompt** (the TOY clock is warm now), an explicit
`%STDRV-I-STARTUP` banner, and every subsystem boot 1 deferred now actually
starts:

```
%STDRV-I-STARTUP, OpenVMS startup begun at 12-AUG-2026 14:41:58.27
%RUN-S-PROC_ID, identification of created process is 00000104
...
%AUDSRV-I-NEWSERVERDB, new audit server database created ( PC 000410B8)
%AUDSRV-I-REMENABLED, resource monitoring enabled for journal SECURITY
%AUDSRV-I-NEWOBJECTDB, new object database created ( PC 00044D68)
%JBC-E-OPENERR, error opening SYS$COMMON:[SYSEXE]QMAN$MASTER.DAT;   [still absent — queue manager was never configured]
%SECSRV-E-NOPROXYDB, cannot find proxy database file NET$PROXY.DAT [x2]
%SECSRV-I-CIACRECLUDB, security server created cluster intrusion database
%SECSRV-I-SERVERSTARTINGU, security server starting up
%SECSRV-I-CIASTARTINGUP, breakin detection and evasion processing now starting up
%ACME-I-SERVERSTART, ACME_SERVER starting
TDF-I-SETTDF TDF set new timezone differential
Warning: DECdtm log file not found (SYS$JOURNAL:SYSTEM$OVMXOR.LM$JOURNAL)
%LICENSE-E-NOAUTH, DEC OPENVMS-ALPHA use is not authorized on this node   [still the lab's unlicensed state]
Startup processing continuing...
%STARTUP-I-AUDITCONTINUE, audit server initialization complete

The OpenVMS system is now executing the site-specific startup commands.
```

`SYS$MANAGER:AUTHORIZE.EXE` adds four rights identifiers to the rights
database during this pass (`SYS$NODE_OVMXOR`, `DECW$WS_QUOTA`,
`IMGDMP$READALL`, `IMGDMP$PROTECT`) — first-time database population, not
repeated on later boots. **Boot 1 explicitly said "security server not
started" / "ACME server not started"; boot 2 is where those two servers,
the audit server database, and the rights database actually come up.** That
is the concrete content behind the installer's promise in §5a — AUTOGEN
doesn't just resize parameters, it gates which subsystems boot 1 is even
allowed to start.

STARTUP finishes and — after several minutes of the emulator running with no
new console output while `SYSTARTUP_VMS.COM`'s tail (queue/audit
housekeeping) completed silently — the login banner and prompt were pending
on OPA0: they appeared only once the console received a keystroke:

```
  SYSTEM       job terminated at 12-AUG-2026 14:42:47.10

  Accounting information:
  Buffered I/O count:               3624      Peak working set size:       7776
  ...

 Welcome to OpenVMS (TM) Alpha Operating System, Version V8.4

Username: SYSTEM
Password:
%LICENSE-I-NOLICENSE, no license is active for this software product
%LOGIN-S-LOGOPRCON, login allowed from OPA0:
   Welcome to OpenVMS (TM) Alpha Operating System, Version V8.4
```

The earlier §3a-era capture
(`alpha84-clean-boot-conversational-2026-08-07.log`) shows the identical
pattern — a carriage return immediately precedes the `Username:` prompt there
too — so this is OPA0's ordinary behavior, not an artifact of this run.

First login confirmed the target is what the install said it would be:

```
$ WRITE SYS$OUTPUT F$GETSYI("ARCH_NAME")+" / "+F$GETSYI("HW_NAME")
Alpha / AlphaServer ES40
$ SHOW SYSTEM/NOPROCESS
OpenVMS V8.4  on node OVMXOR   12-AUG-2026 14:59:13.72   Uptime  0 00:17:15
```

### 5d. Boot 1 vs boot 2, side by side

| | Boot 1 (post-install, pre-AUTOGEN) | Boot 2 (post-AUTOGEN) |
|---|---|---|
| date/time prompt | yes (cold TOY clock) | no (clock already set) |
| startup banner | none (`%SYSTEM-I-BOOTUPGRADE` messages instead) | `%STDRV-I-STARTUP, OpenVMS startup begun` |
| security server | **not started** (`BOOTUPGRADE` skip) | starts (`SECSRV-I-SERVERSTARTINGU`) |
| ACME server | **not started** | starts (`ACME-I-SERVERSTART`) |
| audit server database | not created | created (`AUDSRV-I-NEWSERVERDB`) |
| rights database | untouched | 4 identifiers added (`AUTHORIZE.EXE`) |
| AUTOGEN | runs (all 5 phases) | does not run again |
| ends in | self-triggered shutdown + automatic reboot | interactive login (`Username:`/`Password:`) |
| license state | `LICENSE-E-NOAUTH` (unlicensed lab, both boots) | `LICENSE-E-NOAUTH` (unlicensed lab, both boots) |

### 5e. What this means for OVMX (vms-649, vms-dcf)

The design doc's flag (`docs/design-vms-faithful-install.md` §3.3,
"First-boot completion — Not yet measured") is now measured: **first boot is
not one event, it is two**, gated by AUTOGEN, and the gate is what subsystems
are allowed to start — not merely parameter values:

- **A one-time "run AUTOGEN then reboot" step belongs after `PRODUCT INSTALL
  VMS` and before the target is considered installed.** The install procedure
  in §4's chain needs a fifth line: `AUTOGEN REBOOT` (or equivalent), not a
  plain shutdown.
- **STARTUP needs a first-boot/subsequent-boot distinction**, not just a
  "have I booted before" flag: boot 1 measurably skips the security server,
  ACME server, and audit database creation (`BOOTUPGRADE`-class messages);
  boot 2 brings them up for the first time. An OVMX STARTUP.EXE that always
  runs the full sequence, or that never gates anything, does not match either
  side of this — both are real, distinct executive states.
- **The rights database and audit server database are populated on boot 2,
  not at PCSI install time.** Anything in OVMX that assumes SYSUAF-adjacent
  state exists right after `PRODUCT INSTALL` is checking too early.
- **Not measured here:** what a *second* AUTOGEN-and-reboot cycle looks like
  once the rights/audit databases already exist (i.e., boot 3) — everything
  above compares boot 1 only against boot 2, the one AUTOGEN reboot the
  installer performs. If OVMX's own first-boot design needs boot-N behavior
  for N > 2, that is a further, separate measurement.
