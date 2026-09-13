# rd vms-c06 — the GENESIS ROLE SWAP, root-caused and fixed; BARRIER-RELEASE flipped to a HARD assertion

Live on **k3s-worker** (`/dev/kvm`), own-lab, image
`192.168.2.43:30500/ovmx-genesis-rig:c06-2` built from this branch's working
tree (the gate + the flipped assertion), pods in `ovmx-lab`, one at a time so
run-to-run timing is not perturbed.

## 1. THE DEFECT (from the previous run's evidence, `../vms-c06-rejoin-2node-20260913/`)

`RIG_MODE=proof` went 2/3 clean and 1/3 **coordinator ROLE SWAP**: node A —
VOTES=1, the only node that can hold quorum — founded generation 1 at t=8 s
(`role=founder csid=0x00010001 epoch=1`), admitted node B as its coordinator,
and was then read back at `role=joiner csid=0x00010003 coord=0x00010002
epoch=3`. The founder had re-joined **under** the voteless node it had just
admitted.

**The edge, traced in the run's own consoles** (`proof-run1-roleswap-nodeAB/`):

| t (node A) | node A console | |
|---|---|---|
| 12.5 | `this node has quorum by its own votes: forming an OpenVMS Cluster` | A founds gen 1 |
| 35.57 | `proposing addition of a system to the cluster` | A coordinates B's admission |
| 35.67 | `system 0000000000000402 was added to the cluster` | B admitted, gen 2 |
| 36.18 | `completed VAXcluster state transition` | A's barrier releases (the vms-c06 fix) |
| 36.23 | `the executive already holds this pair's VMS$VAXcluster connection…` | A's join adopts B's connection |
| **36.2x** | *(no log)* | **A sends B an op-0x02 — a JOIN CLUSTER request** |
| 36.32 | `the cluster assigned this node a cluster system id` | A adopts CSID 0x00010003 |
| 36.38 | `this node is now a VAXcluster member` | A is now B's joiner |

and on node B, 0.05 s after A's op-0x02: `proposing addition of a system to the
cluster`. In the two runs that PASSED, the **same op-0x02 went out** and B
happened to be mid-transition and answered `another system is coordinating a
state transition; deferring` (`proof-run3-nodeAB/nodeB.console.log` t=5.94).

So the swap was never a timing bug: **a founder was asking to be admitted on
every run**, and a coin toss decided whether the peer took the job. What
vms-c06's barrier fix changed was the odds — before it, node B's coordinator
never finished its transition, so it was permanently BUSY and always deferred.

`cnxman_join_drive()` starting a join on an existing member is deliberate
(rd vms-f6b: the VMS$VAXcluster connection to a newly-appeared system is opened
by the same machinery from either side). What was missing is that a member
**asks nobody to admit it**.

## 2. THE FIX

`src/kernel-core/vms_cnxman_join_fsm.c`, two edges, both gated on ONE predicate
read from executive state (INV-6) — `cl->state == VMS_CLUSTER_MEMBER` (only
`cnxman_phase2_commit()` writes it) **and** `club.local_csid_valid` (only a real
assignment or a real founding writes it):

1. **`join_send_config()` / `join_walk_complete()`** — the op-0x02 choke point.
   A committed member builds and sends none, counts `admission_withheld`, says
   it once, and its join parks in `[ADVERTISE]` (a server row: identity, echoes,
   membership bursts, the close poll) instead of entering `[ADMIT]`, where a
   silence clock would run over a request that was never made.
2. **`join_h_csid_learned()`** — a committed member that already holds a CSID
   does not take a **different** one from a peer (p. 7-25's re-adoption is for a
   system that is *being admitted*; a member's slot is what every nodemap
   addresses it by). Counted as `csid_reassign_refused`, said once, refused.
   `membrecs_adopted` is now read back from the CLUB rather than incremented on
   intent, so it cannot report an adoption that did not happen.

The vms-c06 **barrier-step routing is untouched** — that is what fixed the
rejoin, and the member's join parking in `[ADVERTISE]` (whose transition cells
are empty) keeps it working.

## 3. THE PROOF — `RIG_MODE=proof`, 6/6 CLEAN, with the assertion HARD

`proof-run{1..6}-k3sworker-full.log`. Every run, without exception:

```
  node A: role=founder member=1 cn=2 csid=0x00010001
  node B: role=joiner  member=1 cn=2 csid=0x00010002
  BARRIER-RELEASE: REACHED (node A's own transition=0)
  GENESIS 2-NODE PROOF PASSED
```

| run | A ever `role=joiner` | B ever `role=founder` | A final epoch | gate fired on A | barrier |
|---|---|---|---|---|---|
| 1–6 | **0** | **0** | 2 | 1 (`asks nobody to admit it`) | REACHED |

Two things make this more than "the race did not happen":

* the gate's own line is on node A's console in **all six** runs, at t≈35.2 s —
  *before* it proposes B's addition. The path was exercised every time; the
  op-0x02 was withheld every time.
* the `BARRIER-RELEASE` check is now a **hard assertion** in
  `tests/qemu/run_cluster_genesis_2node.sh` (it exits 1), so all six runs are
  passes of the flipped gate, not of the old diagnostic.

## 4. THE REJOIN STILL PASSES — `RIG_MODE=rejoin`

`rejoin-run-k3sworker-full.log`:

```
RIG-A-FINAL role=founder member=1 state=MEMBER csid=0x00010001 cn=2 epoch=3 projections=agree
RIG-B-FINAL role=joiner  member=1 state=MEMBER csid=0x00010003 cn=2 epoch=3 projections=agree
REJOIN-AS-TARGET PROOF PASSED (rd vms-4838)
```

Node B evacuated (`kill -9`) and relaunched with the same SYSGEN identity:
`cm_connect_suppressed` fires on its rejoin boot, its own capture shows
`vaxcluster_connect_req_from_self=0` against a working self-TX capability
(`connect_req_from_self=4`), and it takes a **new** CSID 0x00010003 — the gate
does not touch a system that is genuinely being admitted. Node A is never read
back as anything but `founder csid=0x00010001`, and its transition is 0 at
t=200 s.

(Honest note, printed by the rig itself: on THIS run round 1 also showed
`cm_connect_suppressed-fired=0` — which side wins the connect race varies. The
property under test is that the same mechanism holds across a real crash+reboot.)

## 5. WHAT WAS NOT PROVEN, AND IS TRACKED

A node whose join legitimately reaches `CNXMAN_JOIN_MEMBER` forwards barrier
frames to the barrier FSM and answers CONSUMED, so if that same node is ALSO
the coordinator of a transition, the op-0x0b steps stop one FSM short of it —
the exact shape vms-c06 fixed for the founder, still latent for a joined member
that later coordinates. It cannot arise on the 2-node rig now that the founder
never joins, and the routing repair for it is an architecture question (the glue
routes join → barrier → coordinator; a frame cannot be CONSUMED by the barrier
and still reach the coordinator), so it is escalated rather than improvised.

## Files

- `proof-run{1..6}-k3sworker-full.log` — the six `RIG_MODE=proof` runs.
- `rejoin-run-k3sworker-full.log` — the `RIG_MODE=rejoin` re-pass.
- Per-node consoles/pcaps for each run live on the lab PVC under
  `/lab/genesis-c06-p{1..6}` and `/lab/genesis-c06-rj`; the logs here are the
  rig's own complete transcript, which embeds both guests' dmesg.
