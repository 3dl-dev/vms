# STARTUP.EXE: faithfulness review and scope reduction to bootstrap

**Status:** §6 RULED by operator 2026-08-10 — **STRIP ALL OF IT** (see §6 ruling block; grounded in §1: the VMS boot chain never installs).
**Target:** `src/ovmx_init/ovmx_init.c` (1414 lines at review time).
**Method:** read the file against the publicly documented OpenVMS boot chain
(VMS Internals & Data Structures; *OpenVMS System Manager's Manual*, startup
and AUTHORIZE chapters) plus the in-tree oracle capture
`docs/oracle/vax73-show-system-process.md` (VAX1, V7.3, 30-JUL-2026). No
disassembly, no VSI source (Rule 8).

---

## 1. What VMS actually does, and in which component

The VAX boot chain, by stage, with the thing each stage is *not* allowed to do:

| # | Component | What it does | Notably does **not** do |
|---|-----------|--------------|-------------------------|
| 1 | **VMB.EXE** (primary bootstrap) | loaded from the boot block by console firmware; finds and loads SYSBOOT | touch the file system |
| 2 | **SYSBOOT.EXE** (secondary bootstrap) | reads the parameter file, sizes the executive's data structures, loads SYS.EXE and the loadable executive images, builds initial page tables, enters EXEC_INIT | create anything on disk |
| 3 | **EXEC_INIT** | initializes executive databases, turns on memory management, **constructs the system's processes** (SWAPPER, then SYSINIT) | read SYSUAF |
| 4 | **SYSINIT.EXE** | first real process: mounts the system disk, opens page/swap files, creates the STARTUP process | create directories, install files |
| 5 | **STARTUP.COM** (DCL, run by the STARTUP process) | runs the startup phases; creates the detached system processes — JOB_CONTROL, OPCOM, ERRFMT, AUDIT_SERVER, CONFIGURE, CACHE_SERVER … (all 20 visible in the VAX1 `SHOW SYSTEM` capture); ends by calling SYSTARTUP_VMS.COM | authenticate anybody |
| 6 | **JOB_CONTROL** | creates an interactive process running LOGINOUT.EXE in response to unsolicited terminal input | run the boot |
| 7 | **LOGINOUT.EXE** | reads SYSUAF, authenticates, establishes the user's UIC and privileges | anything at boot |

The load-bearing observation is **what is absent from that table entirely**:

- **Nothing in the boot chain creates the system directory tree.** `[SYS0]`,
  `[SYSEXE]`, `[SYSLIB]`, `[SYSMGR]` exist because `INITIALIZE` made a volume
  and the distribution kit wrote a tree onto it. A booting VMS system finds
  them or does not boot.
- **Nothing in the boot chain copies system files onto the system disk.** That
  is VMSINSTAL / standalone BACKUP — an operator command, run once, on a
  system that is *not* the one booting.
- **Nothing in the boot chain creates user home directories.** The System
  Manager's Manual add-user procedure is `AUTHORIZE ADD` followed by
  `CREATE/DIRECTORY .../OWNER=[g,m]` — a human, once per account. (OVMX's own
  `provision_ownership()` comment already cites this correctly, then does it at
  boot anyway.)
- **Nothing in the boot chain re-owns the system tree.** File ownership is a
  property of the on-disk volume, written by the kit.
- **Nothing in the boot chain reads SYSUAF.** EXEC_INIT *constructs* the system
  process; it does not authenticate it. SYSUAF's first reader on a running VMS
  system is LOGINOUT.
- **Nothing in the boot chain initializes a blank system disk.** A system disk
  you are booting is initialized by definition.

Everything in that list is something OVMX's PID 1 currently does.

---

## 2. Mapping: OVMX PID 1 against the chain

| `ovmx_init.c` | VMS stage | Verdict |
|---|---|---|
| mount proc/sys/dev/tmp/pts/shm, `sethostname` (`bare_metal_init`:376–386) | VMB/SYSBOOT substrate | **Bootstrap.** Keep. Linux plumbing with no VMS analogue is honest here. |
| `executive_attach()` — load `vms.ko`, open `/dev/vms`, pin it (:262) | SYSBOOT loading the executive | **Bootstrap.** Keep. The strongest-written code in the file. |
| load `vmsfs.ko` (:397) | SYSBOOT loadable image | **Bootstrap.** Keep. |
| mount `/dev/vda` as vmsfs (:415) | SYSINIT mounts the system disk | **Bootstrap.** Keep. |
| **run `INITIALIZE.EXE` on a blank disk** (:417–437) | — | **Scope violation.** VMS never initializes the volume it is booting. |
| **overlay mode**: `copy_recursive` initramfs → `/var/vmsfs`, mount overlay (:449–460) | — | **Scope violation + second runtime shape.** A boot that silently substitutes an ephemeral tmpfs tree for the system disk is not a state VMS has. |
| **`install_system()`** → `provision_dirs` + `copy_recursive(initramfs → disk)` + `provision_sysuaf_users` (:857) | — | **Scope violation.** This is VMSINSTAL, executed by the booting kernel. |
| **`provision_seed_files()`** (:505) — seeds SYSUAF/RIGHTSLIST/STARTUP.COM/SYSTARTUP_VMS.COM **on every boot** | — | **Scope violation.** This is an upgrade kit, executed on every boot. |
| **`provision_ownership()`** (:800) — `chown -R` the system tree from SYSUAF, **every boot** | — | **Scope violation.** VMS ownership comes off the volume. |
| **`provision_sysuaf_users()`** (:727) — mkdir+chown every account's home, **every boot** | — | **Scope violation.** This is `AUTHORIZE ADD` + `CREATE/DIRECTORY`. |
| `establish_system_identity()` (:983) | EXEC_INIT constructing the system process | **Bootstrap, wrong direction.** See §4. |
| `run_startup()` — fork DCL on STARTUP.COM (:1052) | SYSINIT creating the STARTUP process | **Bootstrap.** Keep. |
| `display_boot_banner()` (:1081) | STARTUP.COM / STDRV | **Faithfulness bug.** See §3. |
| **login loop** (:1235–1407) — fork LOGINOUT on the console forever | JOB_CONTROL | **Wrong component.** Real function, wrong owner. |

Roughly **430 of 1414 lines** are install/provisioning that VMS performs in a
different component at a different time; a further **~175** are the login loop,
which VMS performs in a detached process created by STARTUP.COM. The actual
bootstrap — mounts, executive attach, module load, disk mount, identity,
hand-off to STARTUP.COM — is under 300 lines of the file.

---

## 3. Faithfulness defects visible without moving anything

**3.1 The STDRV bracket is printed after startup already finished.**
`main()` calls `run_startup()` at :1214 and `display_boot_banner()` at :1217.
The banner then prints

```
%STDRV-I-STARTUP, OVMX startup begun at <t>
%STDRV-I-STARTUP, OVMX startup completed at <t>
```

back to back, re-reading the clock between two `printf`s with no work in
between. On VMS those two lines *bracket* the startup procedure — STDRV is the
startup driver, and "begun" precedes every phase. OVMX prints an empty bracket
after the fact. The pair belongs on either side of `run_startup()`, or in
STARTUP.COM itself; `STARTUP.COM`'s own comment already says the *product*
banner is deliberately kept in the image so one place knows the version, which
is fine and orthogonal.

**3.2 A silent fallback chain survives in the mount path.**
`bare_metal_init` :444 prints `%STARTUP-W-MOUNTFAIL … using overlay` and
proceeds to bring the system up on an ephemeral tree. That is precisely the
shape Rule 9's own text forbids for executive facilities ("fail honestly, never
fake success"), applied to the system disk instead. The system disk failing to
mount is a condition VMS does not survive.

**3.3 `is_system_installed()` is a marker file, not a volume property.**
Existence of `DCL.EXE` decides whether the system installs itself. VMS asks the
volume — the home block — what it is, not whether one file happens to be
present.

**3.4 The five-parser SYSUAF defect (`vms-9b7`) is a symptom of §2, not a cause.**
PID 1 reads SYSUAF three times through two of its own hand-rolled parsers. Two
of those three reads exist **only to serve provisioning** —
`provision_sysuaf_users()` (every account's UIC and default dir) and
`provision_ownership()` (SYSTEM's UIC). Remove provisioning from PID 1 and PID
1's SYSUAF surface drops from three reads and two parsers to at most one, which
is the whole of that item's `ovmx_init.c` exposure. (Note: the concurrent
in-tree test `tests/qemu/test_docker_persistent_disk.sh` has since ground-sourced
the *reported* boot failure to qemu's writeback cache, not to parser
divergence — the divergence is real but was not what bricked that boot.)

---

## 4. `establish_system_identity()` — right instinct, one layer too low

The current shape: PID 1 reads SYSUAF's SYSTEM record, parses UIC and
privileges, and hands them to the executive via `vms_kif_setident()`, which may
refuse. The comment's argument is Rule 11's and it is correct as far as it
goes — PID 1 no longer *declares* its identity.

But PID 1 still **supplies the answer**. On VMS, EXEC_INIT constructs the
system process; SYSUAF is not consulted, and could not be — the system disk's
file system is not yet available at that point in the chain. The authority for
"who is the system process" is the executive itself.

`vms.ko` already demonstrates the pattern: it creates `OPA0:` at module init,
and PID 1 *looks it up* rather than announcing it (see the `VMS_IOCTL_SETTERM`
comment at :1247). The same move applies here — the executive constructs the
system process's identity at module init, and PID 1 calls
`vms_kif_getjpi_self()` to learn what it is. That deletes the last SYSUAF read
from PID 1 entirely, deletes both of its parsers, and is *more* faithful, not
less: it removes the file-system dependency VMS's own chain does not have.

This is a design change to the kernel-module interface and therefore triggers
the Rule-4 cascade. It is proposed, not assumed.

---

## 5. Target shape

**PID 1 = SYSBOOT + EXEC_INIT + SYSINIT. Nothing else.**

```
STARTUP.EXE (PID 1)
  1. Linux substrate: mount proc/sys/dev/tmp/pts/shm, hostname
  2. executive_attach()          — load vms.ko, open /dev/vms, pin, or halt
  3. load vmsfs.ko
  4. mount the system disk       — or halt (no overlay, no auto-INITIALIZE)
  5. learn its identity from the executive   (§4)
  6. %STDRV-I-STARTUP, begun
  7. run_startup()               — fork DCL on SYS$MANAGER:STARTUP.COM
  8. %STDRV-I-STARTUP, completed
  9. wait
```

Everything else moves to where VMS keeps it:

| Moves out of PID 1 | Goes to | When it runs |
|---|---|---|
| directory tree, file copy, seed files (`install_system`, `provision_seed_files`, `provision_dirs`) | the **image build** (`distro/Dockerfile.bootable` already holds the whole built tree in the fat initramfs) and/or a `VMSINSTAL`-equivalent operator image | once, at kit build / operator command |
| `INITIALIZE.EXE` on a blank disk | operator command, out of band | once |
| home directories + their ownership (`provision_sysuaf_users`) | `AUTHORIZE` (`tools/vms_authorize.c`) at `ADD` time | once per account |
| system-tree ownership (`provision_ownership`) | the kit that writes the tree | once |
| console login loop | a **JOB_CONTROL**-equivalent detached process, created by `SYSTARTUP_VMS.COM` like every other service | at startup, as a service |

Note the last row is the same argument the file already makes for itself in its
own `NOTE ON SERVICES` block (:877–900): *services start where VMS starts them,
never in this file*. The login loop is a service that never got the memo — on
VMS, `JOB_CONTROL` is one of the 20 detached processes in the `SHOW SYSTEM`
capture. OVMX has no job-controller today (grep: the string appears in the
oracle capture only), so this is new work, not a move.

The fat initramfs already contains the complete built VMS tree, so
`install_system()` is doing at boot what the *build* has already done — the
cheapest version of this change is to have the build write the disk image
rather than have PID 1 write it on first boot.

---

## 6. Operator decision (reserved — Rule: product scope)

**Does OVMX still ship a "boots from a blank disk and installs itself" path at
all?**

- **(a) No.** The build produces a populated system-disk image; `./boot.sh`
  boots it. PID 1 mounts or halts. Maximum fidelity, smallest PID 1, and it
  deletes the entire class §3.2/§3.3 lives in. Costs: the release artifact
  becomes a disk image, not just a kernel+initramfs, and `docs/install-0.1.md`'s
  documented flow changes.
- **(b) Yes, but out of PID 1.** Keep first-boot install as an explicit
  `VMSINSTAL.EXE`-equivalent that the fat initramfs runs *instead of* a normal
  boot (a distinct boot mode, as standalone BACKUP is on VMS), not a branch
  inside the normal boot path.
- **(c) Status quo.** PID 1 keeps installing.

**Recommendation: (b).** It preserves the existing "one artifact, first boot
installs" user experience the release docs describe, while making the install a
*different thing the machine can be doing* rather than a branch in the boot
path — which is exactly how VMS models standalone BACKUP and VMSINSTAL. (a) is
more faithful but changes the shipped release artifact mid-0.1, which is a
scope call, not mine.

---

### ►►► OPERATOR RULING — 2026-08-10: "STRIP ALL OF IT" (see ruling 1 / §1)

The operator ruled the maximal strip: **PID 1 does NO install work.** Every line
§2 marks a "scope violation" — `INITIALIZE`-on-blank (:417–437), overlay mode
(:449–460), `install_system` / `provision_dirs` / `provision_seed_files` /
`provision_ownership`, `copy_recursive` / `copy_seed_file`, `is_system_installed`,
the `INITRAMFS_BACKUP` copy dance, and re-ownership — is **deleted**. PID 1 keeps
only the bootstrap set §2 marks "Keep": mount the Linux plumbing, `sethostname`,
`executive_attach()` (load `vms.ko` + pin `/dev/vms`), load `vmsfs.ko`, mount the
system disk (`/dev/vda`), run STARTUP — and if the disk is not a properly
installed system volume, **it halts** (VMS: "finds them or does not boot"). This
is stricter than the doc's own (b) recommendation — the operator took (a)'s
PID-1 shape.

**Consequence (load-bearing):** installation moves ENTIRELY to the separate,
faithful installer path — the 0.4 installer spine (`791` kit-master → `8ab`
bootable image → `df9` PCSI `PRODUCT INSTALL`, `651` MOUNT / `f812` INITIALIZE
as operator commands). A stripped PID 1 can only boot an already-installed disk,
so **`vms-2f0` (the strip) lands with / after `vms-8ab`** (which produces the
installed disk) — never before, or nothing boots. `vms-2f0` scope = the full
delete above; no residual install branch, no overlay, no self-init.

Everything in §3 and §5 except the install path itself is independent of this
ruling and can proceed either way.

---

## 7. Rule-4 cascade

Boot sequence and kernel-module interface are both named design-change
triggers. The cascade for §4 (executive-constructed system identity):

1. **API compatibility check** — Systems Engineer: does adding
   executive-constructed system identity break `vms_kif_setident()`'s existing
   callers, and does anything else depend on PID 1's SYSUAF read?
2. **Test coverage check** — QA: the boot→install→boot e2e that `vms-9b7`
   already identifies as absent; plus a test that PID 1 reads SYSUAF zero times.
3. **Documentation** — Tech Writer: `docs/architecture.md` boot section,
   `docs/install-0.1.md` if §6 lands on (a) or (b).
