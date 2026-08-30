# What OVMX Can Do at V0.6

A concise, honest read for a human evaluator: what actually works in OVMX as of the
V0.6 release, and what does not yet. Every claim below is grounded in the source and in
the per-surface register. For the exhaustive, per-surface inventory (408 surfaces across
9 domains, each with a status and an authenticity rating), see
[`compatibility-surface.md`](compatibility-surface.md). This page is the prose overview;
that register is the ground truth.

**One rule governs everything here (INV-6):** a facility either does the real thing
against the executive, or it fails honestly with a VMS status code. OVMX does not
fabricate a success. So "works" below always means *really works against a real
`/dev/vms` executive* — not a per-process stand-in that reports success while sharing
nothing.

---

## Cluster participation

OVMX is a genuine VMScluster participant, not a simulation of one.

- **Distributed lock manager — complete (ENQ class).** A cross-node `$ENQ` grants,
  blocks on contention, and grants-on-release on the mastering node; the blocking AST
  fires over the wire on the real holder; the lock value block replicates both ways;
  resources dynamically remaster to a survivor on graceful departure (directory *and*
  lock state rebuilt from the survivor's real origin records); a node refuses to master
  a resource it is not the directory for; and cross-node deadlocks are detected by a
  distributed edge-chasing search that aborts a single deterministic victim with
  `SS$_DEADLOCK`. Proven on a real `/dev/vms` executive across the H0–H11 QEMU
  harnesses. (Register: `cluster-dlm`.)
- **RMS behind the DLM.** File-sharing and record-level locking go through the real
  distributed lock manager — no `flock` fallback.
- **Cluster membership in the executive.** `SHOW CLUSTER` and `$GETSYI` read the real
  executive member block; the old userspace file-facade is fully excised. Membership
  join / rejoin is a real 3-party member-driven FSM, byte-exact against mined wire
  captures. (Register: `connection-manager`.)
- **The wire is real.** The LAN transport under SCS uses genuine raw-Ethernet frames on
  the LAVC/SCA ethertype (0x6007), not a UDP/IP tunnel. (Register: `nisca`, `scs`.)

**What's not there yet:** a **voting cluster under a real quorum algorithm** (quorum
recompute exists, but forming/holding a cluster on votes is post-0.6);
**MSCP-served volumes** as the cluster storage path (MSCP disk serving does real
`pread`/`pwrite`, but serving into a cluster is tracked separately);
**cluster-wide logical names and global sections** (absent — cluster-wide scope degrades
to system-wide); and an **application-process cross-node lock acquisition** (an app
`$ENQ` of a remotely-mastered resource returns the honest `SS$_UNSUPPORTED` stub today;
cross-node locks are currently daemon-choreographed — tracked as `vms-d1f`, post-1.0).

## Files-11 ODS-2 and login

This is the 0.5 authenticity flip, carried into 0.6 as the storage foundation.

- **SYS$DISK is genuine Files-11 ODS-2 over the executive ACP.** RMS reads *and* writes
  real ODS-2 volumes via `$ASSIGN` + `IO$_ACCESS`/`READVBLK`/`WRITEVBLK` — the same path
  a real VMS system uses. The old `/vms` passthrough is retired on the runtime path.
  (Register: domain B, File System & Storage.)
- **Authentic binary login.** `SYSTEM`/`MANAGER` authenticate by **Purdy** against a
  genuine binary `$UAFDEF` SYSUAF read over the ACP — no ASCII shortcut, no SHA-256 —
  with `$RDBDEF` RIGHTSLIST and per-file protection.
- **MOUNT / DISMOUNT / INITIALIZE are real** end to end: privilege-checked,
  executive-resolved, real ODS-2 home block / bitmap / INDEXF / MFD. (Register:
  `devices`.)

**What's honest-but-partial:** `$GETDVI` implements 14 of 48 `DVI$_` items; the rest
return longword 0 (documented "not known", not a fabricated value) — so software reading,
e.g., free space via `$GETDVI` sees 0 rather than a lie. FDL is not implemented (the one
surface, `CONVERT/FDL`, is honestly stubbed).

## The three release architectures

OVMX co-releases across all three release architectures on every cut.

- **x86_64 (Linux substrate)** — the primary architecture; boots to login, runs RMS
  over the executive ACP, and is the reference for the acceptance battery.
- **Alpha LP64** — the 64-bit oracle. Independently proven: the ODS-2 executive ACP,
  authentic binary-SYSUAF login, and the DLM ladder all run on Alpha LP64
  (`qemu-system-alpha`), with a standing green-by-SHA CI gate. Proving a facility across
  two widths is the evidence it is real, not a single-target special case.
- **VAX ILP32 (NetBSD substrate)** — a first-class platform via the NetBSD system
  kernel: the executive, ODS-2 storage, and DCL boot on real VAX emulation, with the VAX
  DCL/SHOW acceptance battery fully green (101/101).

The DCL/SHOW **UX-fidelity gate** holds user-visible output to byte-exact real-VMS
captures continuously, structure-tolerant and with no masked gaps (INV-6); the SHOW
family is proven fabrication-free, with real structural gaps tracked as a gated backlog.

## TCP/IP networking

TCP/IP is treated as a VMS-faithful **layered product**, and at 0.6 only part of it is
present.

- **Configuration/management plane — built.** OVMX IP can be configured the VMS way:
  `TCPIP$CONFIG` plus `TCPIP SET INTERFACE` / `SHOW CONFIGURATION` record the host name,
  domain, and interface address in the VMS-faithful `TCPIP$INET_HOST`/`DOMAIN`/`HOSTADDR`
  SYSTEM logicals (executive-resident, shared cross-process over `/dev/vms`) and apply the
  address to the interface. With no executive it fails `SS$_NOSUCHDEV`, never a
  per-process fake. (Register: `tcpip-services`.)
- **The DCL `TCPIP` verb** answers `SHOW INTERFACE`/`ROUTE` from substrate introspection
  and drives the durable config path authentically.

**What's not there yet:** the **data plane** — a NIC exposed as a VMS device
(`EWA0:`/`BGn:`), a UCX QIO network path, and a usable socket API — is absent (the VM
currently boots with `-nic none`). **SSH** login is consequently not reachable yet; it is
gated entirely on the TCP/IP stack and arrives with it. **DECnet Phase IV** is greenfield
(`SET HOST` honestly reports unavailability; only the `NODE"acc"::` filespec *syntax*
parses, with nothing downstream acting on it). TCP/IP data plane and DECnet are both
1.0-line work.

---

## Reading the register

If you want to verify any claim above at the surface level,
[`compatibility-surface.md`](compatibility-surface.md) lists each surface with:

- a **status** (verified / implemented / partial / stub / designed / absent),
- an **authenticity** rating (real / advisory / facade-risk / n/a) — the facade-risk flag
  is exactly how OVMX hunts for anything that looks done but does not do the real thing,
- the **source file** it lives in, and the evidence or the honest limitation.

The register is an **inventory, not a percentage** — the total VMS surface has no known
denominator, so no "% compatible" is claimed. Cataloguing more of VMS makes the picture
look *less* complete, never more; that is the point.
