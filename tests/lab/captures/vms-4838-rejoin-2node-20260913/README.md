# rd vms-4838 (REJOIN-AS-TARGET) — live EVACUATE->REJOIN proof, 2026-09-13

> ## ⚠ CORRECTION / RETRACTION (2026-09-13, supersedes the WIRE claims below)
>
> The vms-c06 determination found this run's **live-WIRE evidence is DEFECTIVE and is
> RETRACTED** — do NOT read the "zero own outbound VMS$VAXcluster connect" claim as proof:
> - **`connect_req_from_self=0` is VACUOUS.** The rig captures RECEIVE-ONLY
>   (`tests/qemu/init_cluster_genesis.sh` `sca_l2probe recv eth0`), so node B's own
>   capture can never contain B's own transmit — a self-sourced frame is unobservable
>   by construction, not absent. And `nodeA.pcap` (which *does* see B's TX) shows B
>   **DID** open its own `VMS$VAXcluster CONNECT_REQ` on its rejoin boot (t=37.913);
>   `cm_connect_suppressed` fired but adopted B's OWN earlier connection, not A's.
> - **`connect_req_from_peer=5` is unfiltered.** `scan_connect_wire.py` does not filter
>   by SYSAP; 4 of the 5 were `SCS$DIRECTORY`, only 1 was `VMS$VAXcluster`.
>
> **WHAT STILL STANDS** (unaffected): the `join_cm_take_held()` fix's HOST
> (`test_cnxman_join`, 612 checks) + SIM (44 checks) correctness proofs, that it is
> non-regressing (first-join bit-identical), and that it is **never-crash** on a real
> crash+reboot cycle (no panic/bugcheck on either node — that part of this run holds).
>
> **WHAT IS UNVALIDATED:** the connection-adoption WIRE behavior — deferred to a
> CORRECTED-RIG proof (transmit-capture + SYSAP filter; rig-fix filed separately) bundled
> with the real end-to-end blocker.
>
> **END-TO-END member=1/cn2** is blocked not by "member non-reciprocation (vms-694)"
> (vms-694 is a DONE epic) but by **vms-fc7**: node A's coordinator is stuck in
> `CNXMAN_COORD_BARRIER` (`cnxman_coord_participant_lost()` unwired — the barrier is
> never released when a participant dies), a latent stall present even on the passing
> genesis proof. Tracked as vms-c06 → re-pointed to vms-fc7.

Run on the 2-node OVMX genesis rig (`tests/qemu/`), branch
`work/vms-4838-rejoin-target`, new `RIG_MODE=rejoin` instrument, own-lab QEMU
under KVM. Image built from `tests/qemu/Dockerfile.cluster-genesis-2node` at
this branch tip. **Run TWICE, independently**: once on `workshop` while
building the instrument (files below, no suffix), and once as the official
run on **k3s-worker** via the `ovmx-genesis-rig` pod
(`tests/qemu/k8s-cluster-genesis-rig.yaml`, image
`192.168.2.43:30500/ovmx-genesis-rig:4838-1`) -- `rejoin-run-k3sworker-full.log`.
Both reproduce the SAME result (cm_connect_suppressed fires on the rejoin,
zero self-CONNECT_REQ, admission stalls, never a crash) -- not a
workshop-only artifact.

## Scenario

1. Node A founds (VOTES=1), node B joins (VOTES=0) -- ordinary genesis,
   `nodeB-r1.*`. Host script polled `RIG-B-CLUB` for `state=MEMBER`; reached
   at ~t=8s of B's own boot.
2. Node B **evacuated**: its own QEMU process `kill -9`'d (no graceful
   shutdown) while node A still holds its CSB inside RECNXINTERVAL (45s) and
   keeps dialing the member-initiated VMS$VAXcluster CONNECT_REQ (A's own
   once-a-second beat, unscripted -- executive-native). Dwell 25s (> the 20s
   channel-listen timeout, < RECNXINTERVAL).
3. Node B **relaunched** with the identical SYSGEN identity (OVMXB/1026),
   `nodeB-r2.*` -- a real reboot, not a network blackout.

Node B's two boots ride a reconnect-tolerant segment relay
(`segment_relay.py --reconnect-b`, new): node A's one TCP leg is dialled once
and persists the whole run; node B's leg drops and reconnects across the
evacuation.

## Result: never-crash held live; the WIRE claim is RETRACTED (see the correction block above); end-to-end blocked by vms-fc7

Off round 2 (the REJOIN boot):

- **Executive's own log** (`join_cm_take_held()`'s `join_log`, reaching
  console/dmesg in real time): `nodeB-r2.console.log` t=59.2s —
  `%CNXMAN, the executive already holds this pair's VMS$VAXcluster
  connection: this node opens none of its own and drives its admission on
  that one`. This shows `cm_connect_suppressed` firing — but note it adopted
  B's OWN earlier connection (conid `0x8ad90005`, `nodeA.pcap` t=37.913), not
  A's, so it does NOT establish that op-02 rode a member-initiated connection.
- **The wire claim is RETRACTED** (see the correction block at the top): the
  `connect_req_from_self=0` reading is vacuous — the capture is receive-only,
  so B's own transmit is unobservable, and `nodeA.pcap` shows B DID open its
  own `VMS$VAXcluster CONNECT_REQ` on rejoin. `connect_req_from_peer=5` is
  unfiltered by SYSAP (4 were `SCS$DIRECTORY`). This run does NOT prove B
  opened none of its own connects; a corrected-rig proof (transmit-capture +
  SYSAP filter) is required and is deferred.

What this run DOES establish live: never-crash across a real crash+reboot
cycle (no panic/bugcheck on either node).

**But** node B never reaches MEMBER within its window. `nodeB-r2.console.log`
t=65.4s: `%CNXMAN, membership request to the selected member not answered` /
`no cluster member answered this node's membership request: this node is NOT
a cluster member, and will ask again` -- repeats, never resolves, B powers
off at t=71s still NEW. Node A's own console shows the mirror image
(`membership request to the selected member not answered` at its own t=67.4s
and t=119.6s) and, notably, `a transition message arrived in a state that has
no edge for it` at t=98.0s (right as B's round-2 channel re-verifies) --
recorded here as an observation, not diagnosed further (out of this item's
scope; the fix under test is `join_cm_take_held()`, not the admission
handshake).

This is the exact shape the task briefed as the known relocated frontier
**rd vms-694** (member non-reciprocation) -- op-0x02 now correctly rides the
member-initiated connection (proven above), but the member does not answer
it. NOT a defect in THIS fix. No verdict was forced: `run_cluster_genesis_2node.sh`'s
own `rejoin` gate reports `NOT REACHED (2)` honestly rather than asserting
`member=1`.

**Reproduced independently on k3s-worker** (`rejoin-run-k3sworker-full.log`,
the official run for this item): `cm_connect_suppressed-fired=1` on round 1
AND round 2 this time (a different race outcome than the workshop run --
consistent with "either side may open it"), `CONNECT_REQ from itself=0 from
peer=4` on the rejoin capture, `NOT REACHED (2)`, no panics. Node A's console
shows the SAME `a transition message arrived in a state that has no edge for
it` line (t=97.3s on k3s-worker vs t=98.0s on workshop) right as node B's
round-2 channel re-verifies -- recorded on both runs, same trigger point,
strengthening that this is a real, reproducible property of the rejoin path
rather than a one-off timing artifact of either host.

**Never-crash-a-peer held**: no panic/BUG/Oops/general-protection/bugcheck
string in any of A, B-r1, or B-r2's console logs, across the whole run
(including well after B round 2 powered off, while A kept retrying).

## Control: RIG_MODE=proof (plain first-join), unchanged

`control-proof-mode-run.log` / `control-nodeA.console.log` /
`control-nodeB.console.log`: unmodified `RIG_MODE=proof` on this SAME build
still reaches `GENESIS 2-NODE PROOF PASSED`, member=1/cn=2 both nodes,
byte-identical outcome to pre-existing behaviour.

**Honest finding, not smoothed over**: in THIS rig's topology, node A's
redial reliably wins the connection race even on an ORDINARY first join (node
A boots first and has been dialling since t=0) -- so the control's own
console (`control-nodeB.console.log`) ALSO logs `cm_connect_suppressed`
firing once. Round 1 of the rejoin run above shows the opposite (B's own
dial won that particular race, `cm_connect_suppressed-fired=0`). This
confirms the fix's own documented design ("either side may open it") and
means a plain first join is not usable as a *zero-suppression* control on
this specific 2-node segment; it is reported here rather than papered over.
The property this run actually establishes is that the SAME correct
mechanism fires across a REAL crash+reboot cycle, on the real executive.

## Files

- `nodeA.*` -- node A, the whole run (never restarted).
- `nodeB-r1.*` -- node B, first boot (killed mid-window at evacuation).
- `nodeB-r2.*` -- node B, REJOIN boot (the round under test).
- `nodeB-round2.pcap` -- node B's own passive capture of round 2, reconstructed
  from `nodeB-r2.pcap.b64`/`nodeB.pcap.b64` (copied to the conventional name
  by the rig script for the generic verdict-input helpers).
- `nodeA.pcap` -- node A's own passive capture (spans the whole run).
- `relay.log` -- the reconnect-tolerant segment relay's own account of when
  node B's leg dropped and reconnected (a fact about the wire, not a claim
  about any executive).
- `rejoin-run-full.log` -- the rig's full host-side stdout, including the
  `RIG_MODE=rejoin` verdict block.
- `control-*` -- the `RIG_MODE=proof` control run, same image build.

## Rig instrument changed for this item

- `tests/qemu/run_cluster_genesis_2node.sh` -- new `RIG_MODE=rejoin` (schedule,
  evacuate/relaunch orchestration, verdict block).
- `tests/qemu/segment_relay.py` -- new `--reconnect-b` mode; fixed a real
  deadlock found while building this (a persistent A-side reader now runs
  independently of each node-B leg, so accept()ing node B's reconnection
  never waits on node A noticing anything).
- `tests/qemu/scan_connect_wire.py` -- new; SCS CONNECT_REQ wire census
  (published offsets only, Rule 8).
- `tests/qemu/cluster_node.c` -- periodic CDT dump (every 5s) alongside the
  existing CLUB report, for finer-grained executive readback across an
  evacuation window.
- `tests/qemu/Dockerfile.cluster-genesis-2node`, `k8s-cluster-genesis-rig.yaml`
  -- wire the new files in / select the new mode.
