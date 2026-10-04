# vms-2570 — Node C V7.3 demo disk build + in-browser proof (2026-09-23)

rd vms-2570 (child of vms-735), decision rd vms-d24. See `tools/lab-vax/build_nodeC_vms73_cluster.sh`
and `tools/lab-vax/PROVENANCE-vms73-nodeC.md`.

## Disk

`vms73-nodeC-cluster.dsk.gz` — SHA-256 in `vms73-nodeC-cluster.dsk.gz.sha256`
(`45355fd2631c59666b0fe2839d17ed743e7d045646d2c179a8b3402915b4fe15`, 29,807,975 bytes). Built
from `/lab/media/openvms073.iso` + `/lab/cluster/data/d0-newinst073.dsk` inside `vaxlab-4`
(ovmx-lab k3s namespace), in an isolated `/lab/cluster-demo-nodeC-v73/` working directory — the
lab's live group-1 reference cluster (`/lab/cluster/data/d0.dsk`) was never touched. Uncompressed
disk SHA-256 recorded in `disk-sha256-precompress.txt`.

Identity (matches `tools/cluster-web-demo/mk_democonfig.py` `DEMO_NODE_C` exactly):
`SCSNODE=VAXC`, `SCSSYSTEMID=1989`, cluster group `257`, `VOTES=1`, `EXPECTED_VOTES=1`,
`ALLOCLASS=0`. Configured via the real VMS install dialog + `@SYS$MANAGER:CLUSTER_CONFIG_LAN`
(full transcript: `build-console.log`), never hand-injected bytes.

## Genesis proof (standalone, isolated SIMH inside vaxlab-4)

`build-console.log` shows the real sequence:
```
%CNXMAN,  proposing formation of a VAXcluster
%CNXMAN,  now a VAXcluster member -- system VAXC
%CNXMAN,  completing VAXcluster state transition
```
followed by an interactive SDA `SHOW CLUSTER` (`ANALYZE/SYSTEM`) confirming a solo CSB:
```
Address   Node    CSID      Votes   State     Status
83A42740  VAXC    00010001    1     local   member,qf_same,qf_noaccess
...
Curr. coord. CSID        00010001
```
and `F$GETSYI("EXPECTED_VOTES"),F$GETSYI("VOTES"),F$GETSYI("NODENAME")` = `1`, `1`, `VAXC`.

## pcjs bootability — CONFIRMED, not a blocker

The packaged `.dsk.gz` boots correctly under pcjs's KA655 browser machine
(`machines/dec/vax/browser/ovmx-cluster.html`, tested against a fresh `~/projects/pcjs`
worktree, commit `8da562328`). No pcjs-side blocker.

## In-browser proof: OVMX Node A (current main, `c8129e08` — carries vms-147/vms-0f8) + Node C

Setup: `tools/cluster-web-demo/build-cluster-demo` bundle built from the `build-boot-artifacts`
GitHub Actions artifact at SHA `c8129e08` (post vms-147 multicast fix + vms-0f8 discovery fix),
Node A config-injected to `SCSNODE=OVMXA`/`1987`/group `257` via the frozen injectors, served
locally (`demo/cluster/e2e/coi-server.js`) alongside a locally-served pcjs machine (this V7.3
disk). Run inside a temporary k3s-worker pod (`ovmx-lab/vms-2570-e2e`, deleted after use), never
served from vax.3dl.network.

**`e2e-run1-clean-transport.log`** (the existing `demo/cluster/e2e/e2e-boot.js` harness, unmodified,
NODE_C=this disk) — clean run, no crash: both nodes reach a running DCL/console state, real
bidirectional SCA (0x6007) traffic climbs continuously and symmetrically for the whole ~280s
observed window (`sca_byPort` VAXC 3→704, OVMXA 0→849 by t=282s; `nicTx` climbing in lockstep),
0 badclass/nobuf implied (no error counters fired). Node A reaches `%CNXMAN, waiting to form or
join an OpenVMS Cluster`; VAXC reaches `%CNXMAN, completing VAXcluster state transition` (its own
genesis) at t=54s. **Membership admission (`SHOW CLUSTER` naming both nodes) was NOT observed
within this window** — the circuit stayed open and busy but `pass`'s `acpOk` gate for VAXC never
fired (see "Known gap" below; diagnostic-only, does not affect this harness's actual `pass`,
which grades Node A's own SCA+ACP, not Node C's).

**`probe-run3-vaxc-login-sda.log`** + **`vaxc-fulldump-run3.txt`** — a one-off, throwaway probe
script (not added to the repo) that logs into VAXC (`SYSTEM`/`OVMXCLUSTER1`) and runs SDA's
`SHOW CLUSTER` directly (plain `SHOW CLUSTER` needs a terminal-table entry this minimally-
tailored disk doesn't carry — `%SMG-F-UNDTERNOS`, matched the interactive build session).
Taken ~15s after Node A's own CNXMAN first appeared: VAXC's CSB list still shows **`Nodes 1`**
(itself only) — i.e. **CN=2 had not converged that early**; consistent with run1's frame counts
still climbing at that point.

**`probe-run4-nodeA-kernel-panic.log`** and **`probe-run5-nodeA-boot-stall.log`** — two follow-up
attempts to let the circuit run longer before re-checking VAXC's SDA `SHOW CLUSTER`. Both were
confounded by **real host contention** on k3s-worker (a concurrent, unrelated pod — `cn2-stage1`,
another in-flight lane's warm pod — was actively consuming ~0.9–1.5 CPU at the same time; the
node's CPU **requests** were measured at 99% (7995m/8000m) during these runs, `kubectl describe
node k3s-worker`): run4's Node A guest kernel hit a real `Kernel panic - not syncing: IO-APIC +
timer doesn't work!` at t=12s (a known class of wasm/TCG-under-contention timing failure, not a
Node C or genesis-code issue); run5's Node A guest never printed anything past "Booting from
ROM..." for 180+s (starved, not crashed). Both rounds correctly report `Nodes 1` (Node A never
actually reached a state where it could join).

## run6 (2026-09-24) — Node A rebuilt from current main (carries #1293's console fix), 20-minute uncontended watch (rd vms-2570 follow-up)

Rebuilt Node A from `origin/main` at `32f743e9` (`build-boot-artifacts` run
[36009596171](https://github.com/3dl-dev/vms/actions/runs/36009596171), `SOURCE_COMMIT`
verified) — includes #1293 (the console fix so `%CNXMAN` membership lines are actually printed),
#1291 (Node B injection, unused here), #1290/#1289 (NISCA discovery + multicast fixes already in
run1). Config-injected via `tools/cluster-web-demo/build-cluster-demo main-32f743e9` (the same
generator, `--x86-vmlinuz/--x86-initramfs/--x86-sysdisk` from that run + `--site-dir` a fresh
`openvmx-site` checkout at `f1fe677`) — `SCSNODE=OVMXA`, `SCSSYSTEMID=1987`, group `257`,
`VAXCLUSTER=2`, read back off the injected ODS-2 volume by the injector's own verifier. Node C =
this V7.3 disk, served locally (no vax.3dl.network) via a minimal same-repo-tree pcjs static
server (`machines/dec/vax/browser/` + the shared `machines/modules/`, ~5.7MB, traced by import
closure from `vaxworker.js` — no unrelated pcjs disk images shipped). Both servers ran inside one
temporary k3s-worker pod (`ovmx-lab/vms-2570-cn2-run`, deleted after use), CPU-sized
`requests=2/limits=4` (of 8 total) to avoid starving other lanes.

**Host was NOT contended this time**: `kubectl top nodes`/`describe node k3s-worker` checked
before and during the run — 53–79% CPU utilization throughout, no concurrent heavy pod in
`ovmx-lab` (the prior confounding `cn2-stage1` pod had already completed). This rules out the
host-contention explanation runs 4–5 hit.

**`e2e-run6-cn2-20min-nonconverged.log`** + **`e2e-run6-cn2-result.json`** +
**`e2e-run6-final-t1195s.png`** — a fresh harness (adapted from the prior lane's `cluster-proof2.js`,
not committed — same "throwaway probe, not the repo" precedent as `probe-run3`), run for the full
`DEADLINE_MS=1200000` (20 minutes; exceeds this item's >=15-minute bar), with one methodology fix:
plain DCL `SHOW CLUSTER` on VAXC fails with `%SMG-F-UNDTERNOS` on this minimally-tailored disk
(confirmed again — `SET TERMINAL/DEVICE_TYPE=VT100` does **not** fix it, SMG$ needs a compiled
TERMTABLE entry this volume doesn't carry), so this run used SDA's `SHOW CLUSTER`
(`ANALYZE/SYSTEM` -> `SDA> SHOW CLUSTER`) on Node C instead — the same product-native command
`probe-run3` used interactively, now driven automatically every ~45s for the whole window.

**Result — transport proven at far greater scale than run1, membership genuinely did NOT
converge, zero bugchecks either side:**

- **Traffic**: `hub.sca` = `{"VAXC": 3230, "OVMXA": 4055}` (7,305 real 0x6007 frames total),
  Node A's own executive counters (`SHOW CLUSTER/LOCAL_PORTS`) confirm `frames tx 4026 (errors 0),
  rx 3178 (dropped: nobuf 0, badclass 0)` at t=1195s — sustained, symmetric, growing continuously
  for the entire 20-minute window, zero drops/errors throughout.
- **Node A's own `SHOW CLUSTER`** (plain DCL, works fine on this side) at t=1195s:
  ```
  View of Cluster from system ID 1987 node: OVMXA    24-SEP-2026 14:37:35
  +---------------------------------------------------------------+
  |                 SYSTEMS                |      MEMBERS         |
  |----------------------------------------+----------------------|
  | NODE   | CSID     | SOFTWARE        | STATUS           |
  |--------+----------+-----------------+------------------|
  | OVMXA  |          | VMX V0.7        | LOCAL            |
  +---------------------------------------------------------------+
  ```
  — lists **only itself**, for the entire run (every periodic re-check from t~102s to t=1195s
  showed the identical single-row table).
- **Node C's own SDA `SHOW CLUSTER`** at t=1195s: CSB list still shows a single row (`VAXC`,
  `Nodes 1`, `Curr. coord. CSID 00010001`) — byte-identical in shape to the solo genesis dump in
  `vaxc-fulldump-run3.txt`, i.e. **VAXC never admitted OVMXA either**.
- **`%CNXMAN` lines, whole 20-minute console history (grepped, not sampled)**: exactly two —
  Node C's own genesis completion (`%CNXMAN, completing VAXcluster state transition`, its
  standalone 1-node formation) and Node A's single **`%CNXMAN, waiting to form or join an OpenVMS
  Cluster`** at t=50s. **Node A never printed a second `CNXMAN` line for the rest of the run** —
  no proposing, no completing, no `%CNXMAN, this node is now a VAXcluster member` — despite
  #1293's console fix (confirmed working: this exact string IS what the harness greps for and
  the fix is why the t=50s line appears at all; run1 predates it) and despite the PEDRIVER-level
  transport running continuously and correctly the whole time.
- **Bugchecks**: zero on both sides (scanned for `***FATAL BUGCHECK***`, `BUGCHECK CODE`, `Kernel
  panic`, `Oops:` on every console-text update across the whole run; none matched).

## Diagnosis (evidence-bounded; join-FSM code NOT touched here per this item's scope)

The gap is **not** transport, **not** host contention, and **not** this V7.3 disk's genesis (which
independently forms its own correct 1-node CSB both standalone and here). The SCA/PEDRIVER wire
carried 7,305 real, error-free, bidirectional frames over 20 minutes — proof the executive-resident
datalink, multicast HELLO addressing (group 257, rd vms-147), and frame RX/TX paths on both OVMX
and real VAX/VMS are correct and healthy. The break is **above** that layer: Node A's own CNXMAN
join state machine entered `waiting to form or join` once at t=50s and never advanced again for the
remaining ~19 minutes — no further CNXMAN state transition, no membership admission, even though
it kept transmitting/receiving real cluster-group frames the whole time. This points at the
OVMX-side CNXMAN/connection-manager logic that is supposed to *interpret* an incoming peer's HELLO
(or PROPOSE/formation) traffic and drive its own join forward — not the frame path itself, which
this run's counters clear. Reported as an **open gap** for the vms-151/genesis-join-FSM lane
(explicitly out of this item's scope) with the fullest evidence bundle to date: a real, sustained,
massive, error-free two-way SCA exchange between OVMX and a genuine VAX/VMS V7.3 peer that still
does not converge to CN=2.

## Honest status — CN=2 (Node A + Node C V7.3) in-browser: NOT PROVEN (transport proven, membership does not converge)

What IS proven: the V7.3 disk is real, byte-correct, forms genesis both standalone and in-browser;
the transport carries real, sustained, bidirectional, error-free 0x6007 traffic between OVMX Node A
and it, now demonstrated for a full uncontended 20-minute window (7,305 frames, 0 errors) — ruling
out both host contention and insufficient wall-clock time as explanations. What is **conclusively
NOT achieved**: cluster membership convergence — `SHOW CLUSTER` on Node A and SDA `SHOW CLUSTER` on
Node C both name only themselves for the entire window, and Node A's CNXMAN state machine visibly
stalls after its first "waiting to form or join" message. This is reported as an **open gap**
requiring join-FSM work outside this item's scope (see Diagnosis above), not fabricated as done.

## Known gap (diagnostic only, does not block this item)

`~/projects/pcjs` `ovmx-cluster.html`'s real-VMS authenticity tell
(`ACP_TELL_VMS_BANNER = /VAX\/VMS\s+Version\s+V\d+\.\d+-\w+/i`) was tuned for V5.5's exact banner
text ("VAX/VMS Version V5.5-2H4") and does not match V7.3's actual banner ("Welcome to OpenVMS
(TM) VAX Operating System, Version V7.3") — so the page's own `acp-ok` UI tell / `bar()` message
never fires for a V7.3 Node C, though this does not affect `e2e-boot.js`'s actual `pass` gate
(which only grades Node A). Filed as a follow-on, not fixed here (out of this item's scope —
pcjs page authenticity-tell logic, not the disk or the OVMX executive).

## Retry recommendation (ACTIONED — see run6 above)

The longer-duration, uncontended re-run this section asked for was done (run6, 2026-09-24,
20 minutes, verified-idle host). It answers the open question definitively: membership does
**not** converge given ample real time on an uncontended host — the gap is a real join-FSM
issue on the OVMX side, not a resourcing artifact. Next step is join-FSM work (CNXMAN's handling
of a peer's HELLO/formation traffic after its own "waiting to form or join" state), tracked
outside this item.
