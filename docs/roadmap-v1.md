# OVMX — course to a V1.0 release

> Written 2026-08-04 against the live board (`rd list`), `tracking/roadmap.md`,
> `docs/product-vision.md`, `docs/roadmap-waves.md`, and the repo. Progress claims
> below are measured from item status, not recalled. Re-derive before acting on any
> of them — this document is an execution pointer, not stored truth.
>
> **Scope ruled by the operator 2026-08-04, in two rulings:**
> 1. **Rail A is not deferred.** V1.0 ships both rails; the release gate is the
>    rolling evacuation (`vms-ci.6`). An earlier draft recommended shipping Rail B
>    standalone; that recommendation is withdrawn.
> 2. **V1.0 also means: builds and runs every VMS app we can find.** R2 is therefore
>    not three milestones but an open-ended corpus program with a published scoreboard.
>
> **RE-AFFIRMED AND SHARPENED 2026-08-05**, when the `vms-d5b` gate put the same
> question again (the rd item still carried the withdrawn recommendation — see the
> note at the end of this block). Operator, verbatim:
>
> > *"both. 1.0 is 1.0, not 0.5. we can cut any number of releases until 1.0, but
> > 1.0 means fully compat and clustering, and slides into a real VMS cluster and
> > takes over. no shitting me."*
>
> Two things this adds that ruling 1 did not say:
> 3. **Pre-1.0 releases are unlimited and expected.** "We can cut any number of
>    releases until 1.0." Shipping Rail B on its own is fine and encouraged — **as
>    0.x**. What is forbidden is calling it 1.0. This is a real authorization, not a
>    caveat: it removes any argument that holding the 1.0 bar high means shipping
>    nothing until the cluster works.
> 4. **The clustering bar is participation, not admission.** "Slides into a real VMS
>    cluster and takes over" — joining is necessary and not sufficient. §2's "voting
>    member … serves and consumes MSCP disks … participates in the DLM … proven by
>    moving a live workload off a VMS node" is that bar written out, and it stands.
>
> **Consequence, stated because it moves the critical path:** `vms-187` (build the SCA
> layer) and `vms-2f3` (rejoin under the same SCSNODE/SCSSYSTEMID) are on the critical
> path *to 1.0*, not a parallel pillar 1.0 could ship without. `vms-2f3` especially —
> it reproduces on a **virgin** cluster, so it would hit the first restart of the first
> OVMX node anywhere, and "slides in and takes over" is not claimable while it stands.
>
> **A note on where this document sits in the hierarchy, because it just mattered.**
> Until 2026-08-05 this file was **untracked** — not committed, on one machine, invisible
> to every other session — while the `vms-d5b` rd item still carried the *withdrawn*
> "Rail B standalone, cluster becomes 2.0" recommendation and cited "reasons in
> docs/roadmap-v1.md sec 2" for it. §2 says the opposite. An agent reading the board
> rather than the doc reported that this roadmap needed correcting; the roadmap was
> already right and the item was stale. Both are now fixed. **The doc is the artifact;
> the item points at it.**
>
> **Measured while writing this (§1c): the corpus is at 43/229 = 19% run-pass, and the
> CI regression gate that should have caught the drift is vacuous.** That is the single
> most important number in this document and it is not the number the board claims.

---

## 1. Where the project actually is

**Board:** 289 open items — 194 inbox, 85 blocked, 9 active, 1 waiting. 191 closed.
Main CI green as of 2026-08-04 17:40. 46 registered tests.

### Rail B — "run their software": strong, near an endpoint

The VMS-native toolchain spine is **complete**, which is the single biggest fact on
the board and is not reflected in `tracking/roadmap.md`:

| Landed | Meaning |
|---|---|
| `vms-61f.1/.2` | musl packaged as `DECC$SHR.EXE`; IMGACT drives musl runtime init |
| `vms-b65.1`–`.5` | all five OVMX libraries migrated to `$SHR` shareable images |
| `vms-b65.6` | `DCL.EXE` builds and activates VMS-native through IMGACT |
| `vms-be5` | x86_64 `OVMX_IMGACT` build: DCL/STARTUP/LOGINOUT via cross toolchain |
| `vms-913.5` | INSTALL known-image database |
| `vms-4ba.1`–`.7` | **self-host S2**: tcc runs as an OVMX image and compiles its own source inside OVMX; gen2 == gen3 byte-identical |
| `vms-801.4` | 80%+ of the Eight-Cubed corpus compiles and runs |

No `ld` / `ld.so` in the product path.

**Open on this rail:** `vms-034` (DCL end-to-end as a CI gate), `vms-sys` (★ a real
OpenVMS-built image's `SYS$` calls dispatch into OVMX services — the endpoint),
`vms-913.6/.7/.10/.11` (the boot-and-install chain), `vms-801.5/.6/.7` (corpus ladder).

### Rail A — cluster interop: mid-rebuild, and now the schedule

`vms-ci.0/.1/.2/.3/.7/.8` are closed — an OVMX node **does** appear in a real
`SHOW CLUSTER`. But `vms-2f3` (cannot rejoin under the same SCSNODE/SCSSYSTEMID)
survived a session that falsified eleven hypotheses and landed ten real,
separately-fixed defects, none of them causal. The verdict in `docs/HANDOFF-vms-2f3.md`:

> OVMX replays byte-exact captured wire shapes; it does **not** implement SCA.
> Measured over `src/vmsscs/`: `conn_state`=0, `path_block`=0, `system_block`=0,
> `connect_data`=0, `reason_code`=0, `rspid`=0, `credit_wait`=0, `ACCEPT_REQ`=0.
> The rejoin failure is a connection-state-machine failure and there is no
> connection state machine for it to be legibly wrong in.

The bug reproduces on a **virgin** cluster — it would hit the first restart of the
first OVMX node anywhere.

**The response is already planned and already moving.** `vms-187` (implement SCA per
*VAXcluster Principles* ch.2) is a 45-item closure across five deliverable epics, swarm
dispatch started 2026-08-03, and its first wave landed today: PR #68 (CI timeout fix),
then PR #63 (`vms-dd5` — the connection state machine, `scsd.c` wired to the CDL) and
PR #64 (`vms-1d2` — Credit Wait and the p.2-44 special credit message) all merged
between 17:05 and 17:45. The merge blocker that stalled this epic is **cleared**.

```
vms-187 (epic, blocks vms-2f3)
├── vms-6eb  SCA data model + VC state machine        ← frontier (inbox)
├── vms-0ce  SCS connection state machine + services
├── vms-e57  SCS flow control to spec
├── vms-876  Connection-Manager identification via SCA connect data
├── vms-0fb  Directory service + process poller as real SYSAPs
├── 5 sweeps: test coverage · bug · dead code · security · antipattern (vms-14d/176/265/414/728)
└── vms-70e2 A returning OVMX identity is readmitted, proven end to end on the lab
```

Beyond `vms-187` the rail still owes membership behaviour that an evacuation demo
depends on: `vms-ae5` (stay MEMBER while another node leaves or fails), `vms-2d6`
(**quorum loss produces no reconfiguration — survivors go silent**), `vms-7a9` (votes
and quorum), `vms-b8a` (announce our own departure), `vms-63e` (act as coordinator for
another node's join), `vms-591` (symmetric disconnect).

### The executive gap — the root cause under most authenticity work

`vms-6b8`: `vms.ko` is a real but largely unwired executive. Phase 0 (`vms-e4d`,
QEMU CI loads `vms.ko`) and Phase 2 (`vms-71a`, Docker CI migrated) are **done**;
`vms-0ff` (absence must crash, not fake success) is **done**. Phase 1 is in flight —
`vms-ef1` (shared event-flag clusters), `vms-as1` (executive AST delivery),
`vms-pv1` (privileges/access modes). Phase 3 has not started: `vms-pt1` process table,
`vms-d37` logical names, mailboxes, `vms-853` `SHOW SYSTEM`.

Until Phase 3 lands, `DEFINE/SYSTEM` dies with the process, `$CREMBX` cannot support
the canonical VMS IPC pattern, and `SHOW SYSTEM`/`SHOW USERS` can only ever see one
process.

### (c) The corpus scoreboard — measured 2026-08-04, and it is not what the board says

The corpus harness exists and is good: `tests/conformance/run_corpus.sh` (373 lines)
compiles, links and runs each of the 231 tier-1 Eight-Cubed programs and classifies
every one into six buckets. `tests/corpus/` carries `PROVENANCE.md`, `LICENSE-AUDIT.md`
and four tiers (`tier1-examples`, `tier3-mmk`, `tier3-netlib`, `tier4-mx`).

I ran it. **Method:** the harness hardcodes container paths, so I ran a copy with
`/src/` rewritten to the repo root, against `build/lib` (built 2026-08-01 23:48) —
and confirmed **zero commits have touched `src/libvms`, `src/vmsrms`, `src/vmsfs`,
`src/vmsprocess` or `src/libvmssys` since that build**, so the libraries are current
with respect to HEAD for everything the corpus links against.

| Bucket | Count | Share of 229 |
|---|---:|---:|
| compile-fail | 102 | 44.5% |
| link-fail | 67 | 29.3% |
| **run-pass** | **43** | **18.8%** |
| run-fail | 11 | 4.8% |
| run-crash | 6 | 2.6% |

**`vms-801.4` is closed as "Milestone 1: 80%+ Eight-Cubed examples compile and run."**
The measurement above does not reproduce that. Even the loosest reading — "compiled at
all" — is 127/229 = 55%, and actually running correctly is 19%.

**Why the drift was invisible:** `tests/conformance/corpus_baseline.json` is committed
with `total: 0`, an empty program list, all six counters zero, and no timestamp. The
CI job (`corpus-conformance`) is explicitly "a metric job, not a gate — it NEVER fails
the build due to compile/link/run failures" and fails *only* on regression against
that baseline. **With an empty baseline no program can regress, so the job can only
ever exit 0.** The real numbers are computed at runtime, written to an uploaded
artifact, and never asserted or committed back.

Stated fairly: `vms-801.4`'s number may have been measured in the builder container
rather than on the host, and the harness had a real bug fixed during that item (its own
comments record it was "silently producing 100% compile-fail"). I am not calling the
claim dishonest. I am reporting that **nothing in the tree currently backs it**, which
is the whole problem — this is precisely the defect class the authenticity register was
built to catch, sitting on the gate the operator just made central.

The gaps are also already legible and actionable. Missing RTL entry points include
`lib$add_times`, `lib$get_foreign`, `lib$do_command`, `lib$find_file`, `lib$crc`,
`lib$format_date_time`, `lib$get_logical`, `lib$lock_image`, `lib$convert_date_string`;
missing headers include `unixio.h`, `builtins.h`, `smg$routines.h`, `tbk$routines.h`,
`eradef.h`, `sjcdef.h`, `iccdef.h`, `acmemsgdef.h`.

**One more thing this gate must confront (Rule 9):** the corpus job builds with gcc
inside the glibc `ovmx-builder` container and runs the binaries there. That is a
compile-compatibility measurement, not a product measurement. "Builds and runs every
VMS app we can find" ultimately has to mean built with `LINK.EXE` and run under the
QEMU runtime through `IMGACT.EXE` — otherwise the scoreboard measures a path the
product does not ship.

### Two things nobody has ranked as release blockers

**(a) Security defects that are real, not theoretical.**

| Item | Defect |
|---|---|
| `vms-b2e` (P1) | `AUTHORIZE.EXE` grants ALL/SYSPRV from `getenv(USER)` — privilege escalation by environment variable |
| `vms-2d39` | MAIL picks the mailbox from `getenv(VMS_USERNAME)` — one user reads another's mail |
| `vms-f81` | RMS/vmsfs protection bits mismatched across 3 modules |
| `vms-f15`, `vms-36d` | file protection not enforced in the executive |
| `vms-9e2`, `vms-70eb` | `F$GETJPI` / `SHOW PROCESS` are identity facades |
| `vms-888`, `vms-889`, `vms-887` | buffer overflow in `vms_fwrite`, integer overflow in `lib_vm.c`, world-writable `/tmp/OPERATOR.LOG` |

V1.0 ships SSH multi-user login **and now joins a customer's production cluster**.
The second raises the stakes on this table considerably: a node that grants SYSPRV
from an environment variable is a node inside their trust boundary.

**(b) There is zero release engineering on the board.** No packaging item, no artifact
publication, no install/getting-started documentation milestone, no versioning scheme,
no release notes, no trademark review of shipped branding, no license audit of vendored
musl/tcc. A search of all 289 open items returns nothing.

---

## 2. What V1.0 is

> **OVMX 1.0 — join the cluster, run the software, evacuate the node.**
> A bootable, installable x86_64 system that joins a real VMScluster as a voting
> member, serves and consumes MSCP disks, participates in the distributed lock
> manager, and **builds and runs every VMS application we can get our hands on** —
> proven by moving a live workload off a VMS node onto OVMX with the cluster staying up.

This is `docs/product-vision.md` read literally: the two rails converge at
`vms-ci.6`, and the evacuation *is* the product. Both rails are release gates.

**What the rulings cost, stated once so the plan is honest and then not relitigated:**
Rail A is the schedule — `vms-ci.6` sits behind `vms-187`'s 45-item closure, then
membership robustness, then MSCP, then the DLM, and the DLM rung is data-integrity
critical, the one place on this board where "mostly working" is worse than absent.
R2 is now the *volume*: 19% of tier-1 runs today, six more tiers are inventoried but
not ingested, and the gate has no natural finish line. Between them the two rulings
mean V1.0 has both a long pole and an open end.

**What the rulings buy:** one release that makes the actual pitch, and a compatibility
claim measured instead of asserted. A V1.0 that could not join a cluster would have
shipped the foundation and called it the product; a V1.0 whose compatibility number
lived only in a closed item would have shipped a claim nobody could check.

**The two rulings interact, and it is favourable.** R2's gap-closure loop is the most
parallelizable work on the board — hundreds of small, independent, test-driven fixes
that need no cluster lab and no orchestrator attention. Rail A is the opposite: serial,
lab-bound, and expensive in orchestrator context. They contend for very little. Run R2
as continuous swarm background load *underneath* Rail A rather than after it.

---

## 3. The course — six gates

Each gate is a verifiable end state, not a work phase. R1/R2/R3 run in parallel with
R5 (different subsystems, no shared files). R4 runs continuously and blocks only
publication. R5 is the long pole and consumes R1+R2 at its final rung. R6 starts as
soon as R1 produces an artifact.

### R1 — There is a product you can boot and install

```
vms-913.6  fat initramfs with dynamic binaries + IMGACT
   └─▶ vms-913.11  x86_64 IMGACT full boot-to-login   (only remaining blocker)
         └─▶ vms-913.7   first-boot install to a system disk
               └─▶ vms-913.10  slim boot from the installed system disk
```

*Done when:* on a clean x86_64 host, boot the published image under QEMU, install to
a disk, reboot from that disk, and log in to DCL. x86_64 is the primary architecture
(Rule 5) and now the dev seat; `vms-be5` already landed the x86_64 build.

`vms-913.6`'s three blockers — `913.2`, `913.3`, `61f.2` — are **all done**, so it is
dependency-satisfied. It nonetheless does **not** appear in `rd ready` (hygiene item
6); verify before dispatching, because it changes whether this chain starts today.

**This chain is the release artifact, and it is the only gate with no work in flight.**

### R2 — It builds and runs every VMS app we can find *(operator ruling; open-ended)*

This gate is no longer three milestones. It is a **program with a scoreboard**, and
the scoreboard starts at 43/229 = 19%.

**R2.0 — Make the instrument honest (do this first; it is small and everything
downstream depends on it).**
- Commit a **real** `corpus_baseline.json` from an actual run, with timestamp and
  per-program status. The current file is `total: 0` and makes the CI gate vacuous.
- Give the CI job a **floor**, not just a regression check: fail if run-pass drops
  below the committed number. A metric that cannot fail is not a gate.
- Reconcile `vms-801.4`: reproduce its 80% claim in the builder container or correct
  the record. Whichever way it resolves, the tree must end up carrying the number.
- Publish the scoreboard as a build artifact *and* a committed file, so the trend is
  visible without digging through CI runs.

**R2.1 — Acquisition: widen the corpus to "every app we can find."**
`docs/vms-source-code-corpus.md` (2026-02-13) already inventories 100+ programs across
seven tiers with licenses and URLs — that research is done and unexecuted. The tree has
tier1 (231 Eight-Cubed), tier3-mmk, tier3-netlib, tier4-mx. Remaining named targets:
HP `SYS$EXAMPLES`/`DECW$EXAMPLES`/`TCPIP$EXAMPLES` code, vmsbackup (both
implementations), GNV (bash/coreutils/grep/sed), WASD HTTP server, OpenSSL for VMS,
libcurl, plibsys, the John Francis PCSI kits, the VMS Freeware CD collection,
vms-ports, and the GitHub `openvms` topic.
- Every acquisition passes through `PROVENANCE.md` + `LICENSE-AUDIT.md` before it
  enters the tree — non-negotiable, and it feeds R6's license audit directly.
- **Clean-room note:** ingesting *application* source is unrelated to Rule 8, which
  governs VMS-internal formats and protocols. Do not let the two get confused in
  either direction — but also do not ingest anything whose license forbids
  redistribution into a repo that ships.

**R2.2 — Close gaps, driven by the harness rather than by guesswork.**
The 102 compile-fails and 67 link-fails name exactly what to build: the missing `lib$`
entry points and the missing headers listed in §1(c). This is the "grinding the long
tail cheaply" loop the vision doc calls the whole game — and it is the most
parallelizable work on the entire board. It is what swarm dispatch is *for*.

**R2.3 — Point the scoreboard at the real runtime (Rule 9).**
Today's numbers come from gcc-in-a-container. Add a second, authoritative column:
built with `LINK.EXE`, activated by `IMGACT.EXE`, run under QEMU. Depends on R1.
The container column stays as a fast signal; the runtime column is the one that
ships in the release notes.

**R2.4 — The existing named milestones, now rungs rather than the gate.**
`vms-034` (DCL end-to-end as a CI gate) · `vms-801.5` (CRTL shim + header binary
compatibility) · `vms-801.6` (MMK) · `vms-801.7` (NETLIB — QIO + AST networking) ·
`vms-sys` ★ (a real OpenVMS-built image's `SYS$` calls dispatch and execute).

*Done when:* `docs/compatibility-contract.md` is published as a release document whose
every claim is backed by a corpus program running in CI against the real runtime, and
the scoreboard — including the programs that **fail** — ships with the release.

`vms-sys` is the rung `vms-ci.6` depends on: the evacuated workload *is* an activated
VMS image making system calls.

> **A target, offered for a ruling rather than assumed.** "Every app we can find" has
> no natural finish line, so V1.0 needs a number to ship against. I suggest: **100% of
> tier-1 run-pass, and every tier-2/3 program either running or carrying a written,
> published reason it does not.** The second half is what makes an open-ended gate
> shippable — an honest scoreboard with known failures is a release; a promise of
> completeness is not. Absent a different number I will plan against this one.

### R3 — The executive is real, not per-process fiction

```
vms-6b8 Phase 1 (in flight): vms-ef1 event flags · vms-as1 ASTs · vms-pv1 privileges
   └─▶ vms-vx1 veracity: Phase 1 wiring cannot be faked
Phase 3 (not started): vms-pt1 process table · vms-d37 logical names · mailboxes
   └─▶ vms-853 SHOW SYSTEM lists the real process table
   └─▶ vms-vx2 / vms-150 veracity: every replaced facade survives A-writes/B-reads
```

*Done when:* a second process observes what the first one wrote — `DEFINE/SYSTEM`,
`$CREMBX`, event-flag clusters and `SHOW SYSTEM` all cross a process boundary, proven
against a real `/dev/vms`, never a userspace fallback (Rule 9).

Under the Rail A ruling this gate gains a second reason to exist: cluster state is
system-wide by definition. A node whose lock and process state is per-process fiction
cannot hold cluster-wide locks honestly, which is exactly what R5's DLM rung requires.

### R4 — Security truth *(blocks publication, nothing else)*

Fix the identity/privilege set (`vms-b2e`, `vms-2d39`, `vms-9e2`, `vms-70eb`), the
protection set (`vms-f81`, `vms-f15`, `vms-36d`), and the memory-safety set
(`vms-888`, `vms-889`, `vms-887`). Add the SCS security sweep `vms-414` — a network
listener that joins a production cluster is remote attack surface.

*Done when:* no shipped path grants privilege from an environment variable, and the
release notes state the security posture plainly. **Silently shipping the table in
§1(a) into a customer's production cluster is the option that does not exist.**

### R5 — OVMX is a cluster member *(the long pole, the release gate)*

```
A1  vms-187 closure — 5 epics + 5 sweeps
      vms-6eb (frontier) ▶ vms-0ce ▶ vms-e57 ▶ vms-876 ▶ vms-0fb
      wave 1 landed 2026-08-04: PR #68 ▶ #63 (vms-dd5) ▶ #64 (vms-1d2)
        └─▶ vms-70e2  returning identity readmitted, end to end on the lab
              └─▶ vms-2f3 CLOSES  (rejoin works — the acceptance test)
A2  vms-ac4  ground SCS SYSAP message bodies (DLM opcodes + MSCP command blocks)
A3  membership robustness: vms-ae5 · vms-2d6 quorum · vms-7a9 votes · vms-b8a
      departure · vms-63e coordinator · vms-591 symmetric disconnect
A4  vms-ci.4  MSCP-served disk across the OVMX boundary
A5  vms-ci.5  Distributed Lock Manager ($ENQ/$DEQ) — data-integrity critical
      (vms-ci.7 already rewired $ENQ/$DEQ to the kernel lock manager; vms-7fa
       makes OVMX hold real distributed lock state)
A6  vms-ci.6  ★ ROLLING EVACUATION — needs A5 + R1 + R2
```

*Done when:* on the reference lab, a workload running on a VMS node moves to the OVMX
node, the VMS node shuts down, and the cluster stays up with no application error and
no lock-state corruption. That demo **is** V1.0.

Two rungs deserve named caution:
- **A3 `vms-2d6`** — killing the only voting node currently produces no reconfiguration
  and survivors go silent. Quorum is not a nicety in an evacuation demo; removing nodes
  is literally what the demo does.
- **A5 the DLM** — a lock manager that is subtly wrong corrupts the customer's data
  rather than failing visibly. Rule 9's "fail honestly" applies with full force: no
  optimistic lock grants, ever.

### R6 — Release engineering *(entirely missing — must be created)*

- Version + tag scheme; what "1.0" means and what a 1.x promises.
- Published artifact: bootable image + checksums + reproducible build from `git archive`.
- Install guide and getting-started a stranger can follow with no repo access.
- **Cluster admin guide** — SCSNODE/SCSSYSTEMID/ALLOCLASS/votes, how to add an OVMX
  node to an existing cluster, and how to remove it. New under the Rail A ruling and
  non-optional: the product's core operation is a procedure someone performs on a
  production cluster.
- Release notes with an honest **known-not-working** section: the R4 posture, the
  corpus pass rate, which cluster operations are proven and which are untested.
- **Trademark review (INV-0):** brand is OVMX; badge is "OpenVMS-compatible"; OVMX is
  never presented as OpenVMS. Applies to the image, banner, docs, download page (`vms-e8f`).
- License audit of everything vendored and shipped (musl, tcc, busybox) against `LICENSE`.
- A "what OVMX is not" page — the vision doc's honest-limits section, public.

*Done when:* a person who has never seen the repo downloads, boots, installs, joins a
test cluster, and runs one of their own VMS programs using only published documentation.

---

## 4. Explicitly out of V1.0

Not cancelled, not deprioritized as work — just not gating the release:

- **Self-hosting S3–S5** — assembler/LIBRARIAN/MMS/editor as OVMX images,
  OVMX-builds-OVMX, agent-in-OVMX. `vms-116` is a north star, not a release gate.
- **Authenticity meta-apparatus beyond current capability** — see §5.
- **Diskless satellite boot** (`vms-ce7`) — real cluster capability, not needed to
  demonstrate evacuation. Reassess if a design dependency surfaces from `vms-187`.

## 5. One ruling I am taking

Stated so it can be overridden: *freeze the authenticity measurement apparatus at its
current capability until V1.0 ships.*

Roughly 36 of the 289 open items are about the *instrument* — the census, the register,
negative-control gates, citation checks, mutation proofs — rather than the product.
That apparatus is genuinely excellent and it caught real defects. It is also now
generating backlog faster than it retires product risk: each round of hardening
discovers a new way the previous round's proxy could be purchased, which is a true
observation and an infinite one. Recursion in a measurement layer terminates by
decision, not by discovery.

Rule for V1.0: work a meta-layer item only when it gates a claim the release actually
makes. Everything else waits behind the ship date. `vms-38c` states its residual
honestly in the test itself, which is the correct disposition for the rest of them.

The Rail A ruling sharpens this rather than softening it: `vms-187` is a 45-item
closure with five sweeps of its own, and it is now the critical path. Meta-layer work
competing with it for orchestrator attention is competing with the ship date.

**The exemption, stated precisely, because R2.0 lives in it:** "gates a claim the
release actually makes" is a real carve-out, not a formality. The vacuous corpus
baseline in §1(c) is exactly the defect class the register exists to catch, on the
headline compatibility number, discovered by running the harness rather than by
reasoning about it. That is instrument work worth doing immediately. The freeze is on
*recursive* hardening — new proxies for proxies — not on making a specific shipped
claim measurable.

## 6. Board hygiene (do this first — it is cheap)

1. **Nine items are marked `active`.** There is one execution pointer. Collapse to the
   real one and return the rest to ready/blocked.
2. **194 inbox items are untriaged.** Triage against the six gates: label
   `v1-gate-r1`…`r6`, or `post-v1`. Anything that lands in neither is a candidate to close.
3. **`tracking/roadmap.md` is stale** — it stops at the 2026-07-25 pivot and shows the
   whole toolchain spine as unstarted when it is complete.
4. **`docs/roadmap-waves.md` is stale in its invariants** — aarch64-first (the dev seat
   and primary arch are now x86_64) and the lab path `~/vax/cluster` (the lab is at
   `/data/training/vax`). Its DAG structure is still sound.
5. `type=epic` returns nothing on the board; `level=epic` is what is actually set. Any
   tooling filtering on type will silently see no epics.
6. **`rd ready` under-reports.** `vms-913.6` has three blockers and all three are `done`,
   yet it is absent from `rd ready` while other `inbox` items with no blockers (e.g.
   `vms-187`) are present. If ready-ness is computed from the *presence* of a
   `blocked_by` edge rather than the *status* of its target, every item that ever had a
   dependency is invisible to dispatch after that dependency closes — which would
   silently hide much of the unblocked board. Confirm or refute before trusting
   `rd ready` to drive a wave.
7. **`vms-2c6` (QEMU harness exit codes) and `vms-819d` / `vms-86a` (negctl gates
   exceeding CI budgets)** are throughput taxes on every Rail A PR. PR #68 bought
   headroom; these fix the cause. Worth doing early because Rail A is now the schedule.

---

## 7. Critical path, condensed

```
R5 CLUSTER (the long pole) ─────────────────────────────────────────────┐
  vms-187: 6eb ▶ 0ce ▶ e57 ▶ 876 ▶ 0fb ▶ sweeps ▶ 70e2 ▶ [vms-2f3 CLOSES]│
      ▼                                                                 │
  ac4 SYSAP bodies ▶ membership (ae5·2d6·7a9·b8a·63e·591)               │
      ▼                                                                 ▼
  ci.4 MSCP ▶ ci.5 DLM ──────────────────────────────────────▶ ci.6 ★ EVACUATION ─▶ R6 ship ─▶ V1.0
                                                                  ▲   ▲
  R1 artifact  913.6 ▶ 913.11 ▶ 913.7 ▶ 913.10 ───────────────────┘   │
                                    │                                 │
  R2 CORPUS (the open end, continuous swarm load — 43/229 today)      │
     R2.0 honest baseline+floor ▶ R2.1 ingest tiers ▶ R2.2 close gaps │
     R2.3 rescore on the real runtime (needs R1) ▶ R2.4 034/801.x/vms-sys ★
  R3 executive 6b8 Ph1 (in flight) ▶ Ph3          (feeds R5's DLM rung honestly)
  R4 security  ──────────────────────────────────────────── blocks publication only
```

**Three next actions, all dispatchable now, in this order of value:**

1. **R2.0 — commit an honest corpus baseline and give the CI job a floor.** Smallest
   task on this page and it converts the headline compatibility claim from unbacked to
   measured. Everything R2 does afterwards is judged against it. Do this first because
   until it lands, every subsequent corpus number is unfalsifiable too.
2. **`vms-187` wave 2** — `/swarm-dispatch --strategy worktree --workers 4 vms-187`,
   frontier `vms-6eb`. Wave 1 merged today and the CI timeout blocker is cleared; this
   is the critical path and it should not sit idle.
3. **`vms-913.6`** — dependency-satisfied, head of the only gate with nothing in
   flight, and it unblocks `vms-913.11`, which makes the primary architecture bootable.
   Both `vms-ci.6` and R2.3 consume this chain, so it cannot be left until the end.

Then set R2.2 running as continuous background swarm load. It is the one gate that
absorbs unlimited parallelism without contending for the lab or the orchestrator.
