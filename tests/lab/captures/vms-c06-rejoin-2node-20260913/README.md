# rd vms-c06 — live 2-node REJOIN payoff proof + BARRIER-RELEASE hard-assertion attempt, 2026-09-13

Run on branch `work/vms-c06-coord-barrier`, tip `96f08600` (rebased onto
vms-175-inclusive `main`, no upstream). Image built from
`tests/qemu/Dockerfile.cluster-genesis-2node` at this tip, pushed to
`192.168.2.43:30500/ovmx-genesis-rig:c06-1`. All runs on **k3s-worker**
(`/dev/kvm`), own-lab, via `tests/qemu/k8s-cluster-genesis-rig.yaml`-shaped
pods in `ovmx-lab`.

## 1. THE PAYOFF: RIG_MODE=rejoin — PASSED, live, k3s-worker

`rejoin-run-k3sworker-full.log` (`nodeA.*`, `nodeB-r1.*`, `nodeB-r2.*`,
`nodeB.pcap`/`nodeA.pcap`, `relay.log` alongside it).

**`RIG-B-FINAL role=joiner member=1 state=MEMBER csid=0x00010003 cn=2 quorum=1
cevotes=1 qlost=0 epoch=3 projections=agree`** — node B, evacuated (`kill -9`,
no graceful shutdown) while node A held its CSB inside RECNXINTERVAL, and
relaunched with the SAME SYSGEN identity, **reaches member=1/cn=2**. Node A's
own final line agrees: `RIG-A-FINAL role=founder member=1 state=MEMBER
csid=0x00010001 cn=2 ... projections=agree`. This is the vms-c06 fix's payoff:
the coordinator barrier releases and the rejoiner is admitted, where the prior
run (`vms-4838-rejoin-2node-20260913`) stalled at `NOT REACHED (2)`.

Rig verdict: `REJOIN-AS-TARGET PROOF PASSED (rd vms-4838)`, exit 0. No
panic/BUG/Oops/general-protection/bugcheck string in any of A, B-r1, B-r2
consoles.

## 2. THE WIRE, now falsifiable (vms-4838 claim corrected, not vacuous this time)

Rig's own SYSAP-filtered, transmit-capable census of node B's REJOIN-round
capture (`nodeB.pcap`, `scan_connect_wire.py`):

```
sca_frames=506 connect_req_from_self=4 connect_req_from_peer=4 connect_req_from_other=0
vaxcluster_connect_req_from_self=0 vaxcluster_connect_req_from_peer=1 vaxcluster_connect_req_from_other=0
  SYSAP=SCS$DIRECTORY   from_self=4 from_peer=3 from_other=0
  SYSAP=VMS$VAXcluster  from_self=0 from_peer=1 from_other=0
```

**Why this is no longer vacuous** (unlike the 2026-09-13 vms-4838 retraction):
`connect_req_from_self=4` is nonzero — node B's own capture DOES see its own
outbound `SCS$DIRECTORY` CONNECT_REQ frames (the vms-175 ETH_P_ALL
transmit-capture fix works: self-TX is observable on this rig). Against that
working self-TX capability, `vaxcluster_connect_req_from_self=0` is a real,
falsifiable zero: node B genuinely opened **no** `VMS$VAXcluster` CONNECT_REQ
of its own on the rejoin boot; the one `VMS$VAXcluster` CONNECT_REQ node B's
capture saw came `from_peer` (node A). `join_cm_take_held()`'s own log fired
on both round 1 and round 2 (`cm_connect_suppressed-fired=1`/`1`). **This
retroactively CONFIRMS** (does not merely fail to refute) the vms-4838
connection-selection claim: op-02 rode the member-initiated
`VMS$VAXcluster` connection, not a self-opened one.

## 3. BARRIER-RELEASE hard-assertion: NOT FLIPPED — a new, real, intermittent regression found instead

Task asked to flip the vms-c06 `BARRIER-RELEASE` diagnostic
(`run_cluster_genesis_2node.sh`, `RIG_MODE=proof` default path) to a hard
assertion. **Did not flip it — the live evidence says it would be flaky, not
green.** Three independent `RIG_MODE=proof` runs on this same image/tip,
same rig defaults (`RECNX=8s stagger=30s`) as the byte-identical pre-vms-c06
control (`vms-4838-rejoin-2node-20260913/control-proof-mode-run.log`, which
PASSED cleanly: `A: role=founder csid=0x00010001` / `B: role=joiner
csid=0x00010002`):

| run | verdict | node A | node B | barrier |
|---|---|---|---|---|
| proof run 1 | **GENESIS 2-NODE PROOF FAILED** | role=**joiner** csid=0x00010003 | role=**founder** csid=0x00010002 | transition=1, NOT REACHED |
| proof run 2 | PASSED | role=founder csid=0x00010001 | role=joiner csid=0x00010002 | transition=0, REACHED |
| proof run 3 | PASSED | role=founder csid=0x00010001 | role=joiner csid=0x00010002 | transition=0, REACHED |

**Run 1 is not merely "barrier still not released" — it is a coordinator
ROLE SWAP.** Node A (VOTES=1 EXPECTED_VOTES=1, the only node that can ever
found, and which DID found at t=8s with `role=founder csid=0x00010001
epoch=1`) is later read back at t=31s+ holding `role=joiner csid=0x00010003
coord=0x00010002 epoch=3` — its own executive now names **node B** as
coordinator. `role=` is read straight off `vms_club_view_wire` (`local_csid
== coordinator_csid` ⇒ founder; see `cluster_node.c` `rig_role_name()`,
consulted nowhere from rig config) — not a rig label, a live executive
readback (INV-6).

Node A's own console around the transition (`proof-run1-roleswap-nodeAB/nodeA.console.log`,
t≈35.5–36.4s) shows its **own JOIN FSM actively running and taking actions**
— `vms_cnxman_join_fsm.c`'s `join_h_cm_accepted()`/`join_cm_take_held()` path
(`"a cluster member opened a VMS$VAXcluster connection to this node that is
not the member this join is driving through"`, then `"the executive already
holds this pair's VMS$VAXcluster connection..."`, `"adopting the
VMS$VAXcluster connection the executive holds for this member"`, `"the
cluster assigned this node a cluster system id"`) — on the **founder**, which
should have no live join underway at all once it has founded. This is
consistent with vms-c06's own change (routing frames the join FSM used to
silently eat as `NOT_MINE` instead of `CONSUMED`) reactivating a join
instance on the founder that should stay dormant post-genesis, under a race
this tighter `RECNX=8s/stagger=30s` timing hits and the `RECNX=45s/stagger=55s`
rejoin-mode timing (section 1, clean 3-for-3 across this task's own runs) did
not.

**Verdict: 2/3 clean (barrier releases, roles correct, matches the payoff
expectation), 1/3 a genuine coordinator-role-swap regression exposed by the
fix.** Per the file's own comment (`run_cluster_genesis_2node.sh` above the
diagnostic) and CLAUDE.md ("flaky tests are broken tests"), this is NOT
flipped to a hard assertion: doing so would land an intermittently-red
assertion on main's own long-standing genesis proof for a defect this run
found live but did not root-cause or fix. Diagnostic left as-is
(non-failing). Escalating rather than forcing a verdict.

## Files

- `rejoin-run-k3sworker-full.log`, `nodeA.*`, `nodeB-r1.*`, `nodeB-r2.*`,
  `nodeB.pcap`, `relay.log` — section 1+2, the `RIG_MODE=rejoin` payoff run.
- `proof-run-FAILED-roleswap-k3sworker-full.log` +
  `proof-run1-roleswap-nodeAB/` — the role-swap run (section 3, run 1).
- `proof-run2-PASSED-k3sworker-full.log` — run 2 (clean).
- `proof-run3-PASSED-k3sworker-full.log` + `proof-run3-nodeAB/` — run 3 (clean).

## Rig / image

- Built+pushed `192.168.2.43:30500/ovmx-genesis-rig:c06-1` from this
  worktree's tip (`96f08600`) via
  `docker build -f tests/qemu/Dockerfile.cluster-genesis-2node`.
- No rig-scenario files changed for this run (used the existing
  `RIG_MODE=rejoin`/`proof` instruments already on this branch from
  vms-c06 + vms-175).
