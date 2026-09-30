# rd vms-eb3 — a joiner frozen inside its own admission, and what wakes

Stall-rig arm P-3 of rd vms-e88 (build `b2726966`, `/lab/run-e88/runs/P-3`)
took the real OpenVMS VAX V7.3 down with CNXMGRERR. The stall froze the
joining OVMX node (`OVMXB`) 12 ms after the coordinator's barrier GO
(cat-0x01 op-0x0a) and before its own first step (op-0x0b). On waking,
OVMX processed the connection loss before the queued GO. It aborted the
transition, dropped its join back to VC CONNECT, and on the ladder's new
connection sent MODEL and PARAMS with send-msg# 1. The VAX was
re-establishing that connection as a member's inside its reconnect window.
That burst was the VAX's last frame.

This directory is the real-VMS reference for that window, and the fix.

## The oracle — three real V7.3 nodes, the joiner frozen

A private bridge and three SIMH V7.3 nodes (golden 3-node volume) ran in a
disposable pod (`ovmx-lab/eb3lab`); no `vaxlab-*` pod or live cluster was
touched. `fz.sh` boots founder VAX3 (1027) and member VAX1 (1025), then the
joiner VAX2 (1026). `freeze.py` watches VAX2's own tap and SIGSTOPs its SIMH
process the instant VAX2 sends its Phase-1 answer (cat-0x81 op-0x09). It thaws
14 s later. Nothing on the wire is altered, except in F6, where one tc u32
filter drops exactly the cat-0x01 op-0x0a frame to VAX2 during the freeze.

| run | the GO while frozen | what the joiner's console says, in order |
|---|---|---|
| **F5** | delivered, queued | now a member; lost connection to VAX1 / VAX3; re-established both |
| **F6** | **dropped** (the P-3 case: the GO never processed) | lost connection to VAX1 / VAX3; re-established both; **then** now a member |

No node bugchecked in either run.

**F6 on the wire** (`eb3-F6-20260930.pcap`; `cmconn.py`, `ctl.py`):

```
157.670 VAX3->VAX2 cat=01 op=09 smsg=264           Phase-1 OPEN
157.670 VAX2->VAX3 cat=81 op=09 smsg=94  amsg=264  its answer -- frozen here
157.670 VAX3->VAX2 cat=04 op=06 smsg=265           delivered
157.670 VAX3->VAX2 cat=01 op=0a smsg=266           the GO -- dropped
174.523 VAX3 CONNECT_REQ, conndata ack [12:14] = 94
174.524 VAX2 accepts,    conndata ack [12:14] = 265 (09 01)
174.524 VAX3->VAX2 cat=01 op=0a smsg=266 amsg=94   the GO, RE-SENT, same number
174.524 VAX2->VAX3 cat=01 op=0b smsg=95  amsg=266  step 1 -- dialogue CARRIED
...     twelve releases, twelve steps, transition complete
```

The rules the two runs establish:

1. **A system that has answered a transition's Phase 1 treats a lost
   connection to the transition's systems as a re-establishment, not a
   restart.** This happens before Phase 2 (the GO) has made anybody
   SELECTED. The dialogue carries on the new connection in both directions:
   each side advertises its ack in the connect data, and each resumes right
   after the other's.
2. **The coordinator re-sends what the joiner has not acknowledged**
   (F6: the GO at its original send-msg# 266). The joiner's transition is
   still open to receive it.
3. **The joiner re-drives nothing.** It sends no MODEL, no PARAMS and no
   second membership request.

## The fix (rd vms-eb3)

* `cm_phase1_named` (`vms_cluster.h`). The participant's barrier sets it at
  Phase 1 on the coordinator's CSB and on every CSB the proposal's nodemap
  names, and clears it when the transition completes or is abandoned. The
  dialogue-carry predicate (`csb_dialogue_may_continue`) accepts it beside
  SELECTED.
* A path-loss close no longer abandons the participant's transition
  (`cnxman_transition_peer_lost`, `window_over`). A rejection, the window's
  expiry, or the coordinator's CSB being given up does
  (`cnxman_held_transition_check`, which runs each beat before the reclaim).
* The join holds an answered transition across the loss
  (`join_hold_transition_across_loss`: no VC CONNECT). It re-offers nothing
  on a connection re-established inside the transition. Once the transition
  ends without the connection, it takes the old path
  (`join_admit_lost_connection`).

## Tests

* R1: `test_cnxman_csb` (carry at Phase 1, and its control),
  `test_cnxman_barrier` (named at Phase 1, un-named at every end),
  `test_cnxman_join` (held, no re-drive, re-sent GO runs the barrier),
  `test_cnxman_glue` (the close/beat wiring).
* R2: `tests/cluster/sim/scenarios/cnxman_joiner_frozen_across_the_go.c`
  replays F6's own frames (18 manifest-hashed specimens `cm-eb3-f6-*`)
  through the shipping barrier, CSB ladder and reconnect FSM on the virtual
  clock. OVMX's Phase-1 answer is byte-identical to the real joiner's. The
  carried dialogue, the connect-data ack (09 01) and step 1 (95 / 266) all
  match the oracle.
* Host negctl: six defects, each measured to redden exactly its gate.

## The rig arm for this window

`rig/stallpkt.py` arms the same guest stall as `stall.sh` (SIGSTOP of the
node's QEMU tree), but on the wire instead of on a console line.
`pkt:8109` fires on OVMXB's own Phase-1 answer, so the GO lands in a
stopped guest (F6 / P-3). `pkt:0a` fires on the GO leaving the bridge for
OVMXB's tap (F5). `rig/matrix.sh` tokens are `ORDER:SECONDS:MODE`, where
MODE is `vax` (P-3's console trigger), `member` (the vms-1f40 bar's),
`pkt:8109` or `pkt:0a`.

## The pre-fix control (matrix C, build `cbf20152`, main before this item)

| arm | order | trigger | result |
|---|---|---|---|
| C-1 | a-then-b | pkt:8109, 14 s | **FAIL, VAX CNXMGRERR**: OVMXB logged "path lost", "aborting VAXcluster state transition", "lost ... before this node was admitted" -- the P-3 shape |
| C-2 | a-then-b | pkt:0a, 14 s | PASS (the queued GO was processed before the close) |
| C-3 | a-then-b | pkt:8109, 14 s | **FAIL, VAX CNXMGRERR** |
| C-4 | together | pkt:8109, 14 s | PASS |

So the wire-armed stall lands in the P-3 window: two of two a-then-b pkt:8109
arms took the real VAX down on the unfixed build.

(A note on the rig itself: an art directory outside `/lab/run-eb3` escaped
`runarm.sh`'s between-arms kill, and so did two stale matrices whose kill
pattern had the path order reversed. Arms run under those conditions are
discarded. `runarm.sh` now also kills by tap name; see `rig/`.)

## Files

* `eb3-F5-20260930.pcap`, `eb3-F6-20260930.pcap` are trimmed by `trim.py`
  (joiner-side control and CM dialogue); full captures stay at
  `/lab/k8s-labs/eb3lab/F5`, `F6`.
* `consoles/` holds every node's console, plus `freeze.log` (the trigger's
  own timestamps).
* The oracle lab: `fz.sh`, `freeze.py`, `orsetup.sh`, `orboot.sh`,
  `orstop.sh`, `con.sh`. The decoders: `cmconn.py`, `ctl.py`,
  `f6frames.py`. The fixture generator: `mkspec.py`.
* `rig/` holds the stall-rig changes.
