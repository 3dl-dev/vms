# OVMX 0.6

**The 0.6 milestone: OVMX is a genuine VMScluster participant. A real distributed lock manager runs the full H0–H11 ladder on the executive over the SCS wire, RMS file- and record-locking reaches that real arbitrator, and cluster membership lives in the executive — not in a userspace facade. Plus an oracle-driven UX-fidelity gate that holds DCL/SHOW output to byte-exact real-VMS captures.**

0.5 was the authenticity flip — RMS reading and writing genuine Files-11 ODS-2 over
the executive ACP. 0.6 is **cluster correctness**: the same discipline applied to
clustering. A lock granted on one OVMX node is honored on another over real SCS; the
blocking AST fires over the wire on the real holder; resources master, remaster, and
detect distributed deadlock. Every bit of that state is real executive state — no
per-process fake ever answers (INV-6), and where a facility is not built the executive
returns an honest error rather than a fabricated success.

## Cluster correctness (the 0.6 hook)

- **The distributed lock manager runs the full H0–H11 ladder on the real executive
  over the SCS wire.** Cross-node `$ENQ` grant, contention and block-then-grant,
  blocking-AST (BLKAST) delivery on the real holder, resource mastering and dynamic
  remastering to a survivor on graceful departure, lock-value-block replication both
  ways, directory-ownership enforcement, and distributed deadlock detection with a
  globally deterministic single victim (`SS$_DEADLOCK`). Every rung is proven on a
  real `/dev/vms` executive across the DLM QEMU harnesses (H0..H11), on x86_64 and
  Alpha LP64.
- **RMS behind the DLM.** RMS file-sharing and record-level locking reach the real
  DLM arbitrator on a real `/dev/vms` — no `flock` fallback (INV-6). The lock a file
  operation takes is a real executive lock, not a host primitive standing in for one.
- **Cluster membership crosses into the executive.** `SHOW CLUSTER` and `$GETSYI`
  read the real executive member block; the userspace file-facade that used to shadow
  membership is **fully excised**, so there is no drift path. Rejoin is closed
  (member-driven join FSM, no distinct rejoin state — matching VMS's own model), and
  SYSGEN parameter adoption is proven.

## The oracle-driven UX-fidelity gate

- **A continuous, structure-tolerant golden-diff of DCL/SHOW output against byte-exact
  real-VMS captures.** The gate compares OVMX's user-visible output to goldens captured
  from live VMS, tolerating benign structural variation but never masking a real gap
  (INV-6). The SHOW family is proven **fabrication-free** — where a field cannot be
  filled from real data it is honestly omitted, and any real structural gap is tracked
  as an explicit, gated backlog rather than papered over.
- **SHOW fidelity fixes.** `SHOW QUOTA` reports the honest `%SYSTEM-F-QFNOTACT` rather
  than inventing quota figures; `SHOW DEVICE/FULL` carries the volume Owner; and
  `SHOW PROCESS` renders the UIC (`[SYSTEM]`) faithfully.

## How 0.6 proves it is real (not just green)

Every DLM rung and every membership surface is exercised against a real `/dev/vms`
executive on a live multi-node QEMU rail — cross-node grant/block/release, BLKAST over
the wire, H10b lock-state rebuild on a new master from the survivor's real origin
records, the e84 directory-ownership refusal, and the H11 deadlock search over the real
distributed wait-for graph. All DLM state is real executive state (resource blocks,
granted/waiting queues, origin records); a dropped or TTL-expired probe reports
no-deadlock, never a fabricated cycle. Absence is always an honest status code, never a
per-process fake. The version-only bump commit was cut green-by-SHA across Build & Test,
the DCL/SHOW acceptance battery, the console boot gate, cross-arch image parity, and the
tcc self-host stage.

## Honest scope — what is not in 0.6

We name these deliberately rather than bury them. 0.6 is cluster **correctness** — the
lock manager and membership are real — but a full voting cluster is not yet here:

- **Quorum and votes are post-0.6.** The connection manager's quorum recompute exists,
  but a voting member forming and holding a cluster under a real quorum algorithm is not
  the 0.6 claim (targeted at the 0.7–0.9 cluster line).
- **MSCP-served volumes are post-0.6.** MSCP disk serving does real `pread`/`pwrite`
  against a served volume, but serving a volume into a cluster as the storage path is
  its own tracked work, not shipped here.
- **Cluster-wide logical names and global sections are absent.** Cluster-wide
  logical-name scope degrades to system-wide; cluster-wide global sections are not built.
- **An application-process cross-node lock acquisition path is post-1.0.** Today
  cross-node locks are daemon-choreographed and CSID-keyed; an application `$ENQ` of a
  remotely-mastered resource still returns the honest `SS$_UNSUPPORTED` stub (tracked as
  `vms-d1f`).
- **DECnet and TCP/IP are not part of this milestone.** DECnet Phase IV is greenfield
  (`SET HOST` honestly reports unavailability); TCP/IP Services has its config/management
  plane built but no NIC device path yet. Both are 1.0-line work.

For the exhaustive, per-surface inventory of what is built and at what authenticity
level, see `docs/compatibility-surface.md`.
