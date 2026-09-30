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

## Matrix F on the first fix build (`d6644177`), and what it added

F-1..F-15 (stopped there, to move the proof onto the final build): **13 PASS,
0 VAX bugchecks** in 15 injected arms, including 8 in the op-0a window
(pkt:8109; the arm that crashed the unfixed build 2 of 2 passes 7 of 7 at
<= 25 s). Two FAILs, neither a bugcheck:

* **F-6** (together, VAX-console trigger): every node reached MEMBER and the
  VAX admitted OVMXB, but the harness's blind-typed login on OVMXB raced a
  Password: prompt that came 14 s late while the node was joining. The session
  never existed, so the grader read no view of OVMXB. `rig/b36node.sh` now
  waits for each prompt and proves the login (`B36-LOGIN-OK`), retrying on
  failure.
* **F-13** (a-then-b, **30 s**, pkt:8109): past RECNXINTERVAL the VAX correctly
  timed OVMXB out, removed it and refused its reconnect. OVMXB took CLUEXIT
  and re-incarnated, as a real node does (vms-b36 oracle). The other member,
  OVMXA (also OVMX), still held its old block in the reconnect window. It
  re-established the new incarnation's connection as the **old**
  conversation -- counters and the "already introduced" mask carried -- and
  never sent its MODEL/PARAMS, so OVMXB waited for connectivity for the rest
  of the arm. p. 7-24 DEAD / p. 7-25: a new incarnation is a new
  conversation. `cnxman_csb_set_incarnation()` now records the change
  (`cm_new_incarnation`) and the carry predicate refuses it -- the contract
  the predicate's own comment already stated ("whose incarnation has not
  changed").

## Matrix H on `37afccda`, and oracle F7 (a stall past RECNXINTERVAL)

H-1..H-13 (stopped there, to move to the next build): 11 PASS, **0 VAX
bugchecks**. Two FAILs:

* **H-6** (together, VAX-console trigger, 14 s) is not this item's window.
  OVMXB was frozen before it asked, right after its VMS$VAXcluster
  CONNECT_REQ to OVMXA was answered. The OVMX<->OVMX connect never completed
  (OVMXA held OVMXB at ACCEPT for 400 s), and the VAX timed out the idle
  OVMXA while its DLM requests went unanswered. Filed as **rd vms-04b**.
* **H-13** (a-then-b, 30 s, pkt:8109), and F-13 before it: past RECNXINTERVAL
  the VAX removed OVMXB. OVMXA was still inside the addition's barrier (step
  1 sent, never released), and it refused the VAX's removal: "a new
  transition opened while one is committed; the running one stands" (a rule
  the code marked INFERRED, "no capture shows one").

**Oracle F7** is the capture. The same freeze, 30 s, on three real V7.3
nodes (`consoles/F7-*`, `/lab/k8s-labs/eb3lab/F7/F7.pcap`):

```
158.149 VAX3->VAX1 cat=01 op=0a              the addition's GO
158.150 VAX1->VAX3 cat=01 op=0b smsg=12009   step 1 -- never released
185.547 VAX1->VAX3 cat=01 op=03/op=08/op=0a  VAX1 opens the REMOVAL
185.548 VAX3->VAX1 cat=81 op=08              VAX3, inside the stalled
185.549 VAX3->VAX1 cat=01 op=0b ...          barrier, answers it and runs it
```

The real members removed the joiner and completed. The thawed joiner found
itself removed and took CLUEXIT. (What followed is lab artifact: the
CLUEXIT reboot came up on VAX1's root, a duplicate VAX1, and the real VAX1
then bugchecked CNXMGRERR. Nothing here is drawn from after that point.)

So a new transition supersedes a committed barrier that has stalled
(`barrier_supersede_committed`: its open or a bare removal GO). Phase 2's
count stands (p. 7-42); the stalled barrier's DLM rebuild and Phase-1
record end.

## Matrix J on `980bf79a` (the supersede fix)

J-1..J-11 (stopped there, to run the bar matrix L): 9 PASS, 0 VAX bugchecks.

* **J-9** (a-then-b, 30 s, pkt:8109): the past-RECNXINTERVAL case now goes
  right as far as the survivors go. The VAX removed OVMXB. OVMXA superseded
  its stalled barrier, completed the removal and stayed MEMBER. OVMXB took
  CLUEXIT and re-incarnated, and OVMXA re-introduced itself to the new
  incarnation (fresh MODEL/PARAMS both ways). What does not yet work is
  OVMXB's in-place rejoin beside an OVMX member: its CSB for OVMXA stayed
  RECONNECT, so its connectivity hold never cleared. Filed as **rd vms-833**.
  (The real reference, F7, reboots its joiner, so it has no in-place rejoin
  to compare.)
* **J-8** (b-then-a, member trigger, 16 s): OVMXA, the second joiner, was
  not yet admitted when the arm was graded. Its earlier requests had landed
  while the VAX was admitting OVMXB, and the stall hit as it asked again. The
  matrix waits for OVMXB's member line plus 60 s, which here was about 30 s
  after the VAX recovered OVMXB. Re-run with a 300 s settle as matrix K.

This item's bar is therefore held to stalls inside the reconnect window
(<= 25 s), the P-3 window: matrix L.

## The bar, on the final build `980bf79a`

**Stall rig, matrix L** (`rig/matrices-C-F-H-J-L-K.log`): **20 of 20
consecutive PASS, 0 VAX bugchecks, all three nodes MEMBER.** The 20 arms are
12 in the op-0a window (pkt:8109 -- the arm that took the real VAX down 2 of
2 on the unfixed build), 3 pkt:0a, 2 on the P-3 VAX-console trigger and 3 on
the member trigger. They cover all three join orders and stalls of 6 to 25 s.
**Matrix K** (the J-8 shape again, b-then-a, member trigger, 16 s, with a
300 s settle): 3 of 3 PASS. That makes 23 consecutive.

**Browser**, visitor gate `CASE=all-at-once` x10 (`browser-gate-980bf79a.log`)
on a `build-cluster-demo` bundle from this branch's artifacts (group 257) and
a Node B built at the same SHA: **10 of 10 CN=3**. The real VAX admitted both
OVMX nodes every time, with zero bugchecks.

Every matrix on every fixed build (F, H, J, L, K; 80 injected arms) had
**zero VAX bugchecks**. Each non-PASS arm is accounted for above: the
harness login (fixed), H-6 (rd vms-04b), J-8 (grading window), and the 30 s
arms past RECNXINTERVAL (fixed in part; the rest is rd vms-833).

## Files

* `eb3-F5-20260930.pcap`, `eb3-F6-20260930.pcap` are trimmed by `trim.py`
  (joiner-side control and CM dialogue); full captures stay at
  `/lab/k8s-labs/eb3lab/F5`, `F6`.
* `consoles/` holds every node's console, plus `freeze.log` (the trigger's
  own timestamps).
* The oracle lab: `fz.sh`, `freeze.py`, `orsetup.sh`, `orboot.sh`,
  `orstop.sh`, `con.sh`. The decoders: `cmconn.py`, `ctl.py`,
  `f6frames.py`. The fixture generator: `mkspec.py`.
* `rig/` holds the stall-rig changes, and every matrix's per-arm grades.
* `browser-gate-980bf79a.log` holds the final browser proof.
