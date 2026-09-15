# vms-1ee -- DLM rung H10a re-established: the autonomous remaster (2026-09-14)

Own-lab, own-isolated k3s-worker (KVM), `tools/k3s/run-on-rail.sh --dind`,
`RIG_MODE=remaster`. Two runs of the SAME rig, back to back, bracketing the
one-line root-cause fix this item found live.

## What the mode does (`tests/qemu/cluster_node.c` section 6f)

Node B joins normally (real genesis, real op-0x05 admission, CN=2). Node A
(the survivor) discovers a resource name (`OVMXA$M01`) that node B genuinely
masters -- the same GET_RESMASTER read-back discovery the H8/H9 xnode phase
already uses, never computed (Rule 8). Node B's process then simply **exits
on its own** after its (shorter) `--window` -- no last-gasp announcement, no
`kill -9`, no rig-injected event of any kind. Node A is left with nothing but
its own connectivity-loss ladder (RECNXINTERVAL's reconnect hold, then the
coordinator's PROPOSE_TRANSITION removal) to notice the departure. Node A
then polls its own CLUB for `cluster_nodes == 1`, re-`$ENQ`s the SAME name,
and reads GET_RESMASTER back.

## run1 (`remaster-FAIL-run1-before-timerfix.log`) -- honest FAILURE, real diagnosis

Genesis worked (A founder, B joiner, CN=2). Node A's connectivity-loss ladder
genuinely fired (console: "reconnect interval expired, proposing removal" ->
"proposing removal of a system from the cluster" -> "completed VAXcluster
state transition" -> "this node is NOT a cluster member" for B; CLUB
correctly dropped to `cluster_nodes=1`, observed by the rig). But the
re-master check FAILED:

```
RIG-A-REMASTER-AFTER res=OVMXA$M01 enq_status=1 lkid=0x00000004
  master_csid_before=0x00010002 master_csid_after=0x00010002
  is_local_master=0 remastered=0
```

`master_csid_after` never changed -- the resource's cached master (B) was
never invalidated, even though B was genuinely gone.

**Root cause** (read off `src/kernel-core/vms_cnxman.c`, not guessed):
`cnxman_notify_membership_changes()` -- the function the DLM
`member_departed` hook was wired into (this item's first commit) -- is
called from exactly four places, all inside `cnxman_vc_message()`'s
wire-frame dispatch. The recnx-timeout removal path never sends node A a
frame (there is nobody left to send one): `CNXMAN_TIMER_RECNX`'s own case in
`cnxman_work_handler()` runs the reconnect ladder and, when it completes a
removal synchronously (as it does here -- nobody to negotiate a transition
with), goes straight to `cnxman_quorum_apply()` without ever passing through
`cnxman_notify_membership_changes()`. The quorum-apply comment right beside
it already named this exact failure mode for quorum ("a CSB can also change
state from a reconnect ladder that expired here, on this beat, with no
message involved") -- the same reasoning had never been extended to the
membership notifier, because no departure hook hung off it before this item.

## The fix

`cnxman_work_handler()`'s `CNXMAN_TIMER_RECNX` case now takes the same
before/after snapshot `cnxman_vc_message()` takes
(`cnxman_membership_snapshot`) around the recnx sweep, and calls
`cnxman_notify_membership_changes()` with it -- so a membership change driven
purely by RECNXINTERVAL timeout now reaches `$SETCLUEVT` and the DLM arm's
`member_departed` callback exactly as a message-driven one already did.

## run2 (`remaster-PASS-run2.log`) -- same rig, same build otherwise, PASS

```
RIG-A-REMASTER-BEFORE res=OVMXA$M01 master_csid=0x00010002 dir_csid=0x00010002
RIG-A-REMASTER-DEPARTED observed=1 polls=96 nodes=1
RIG-A-REMASTER-AFTER res=OVMXA$M01 enq_status=1 lkid=0x00000004
  master_csid_before=0x00010002 master_csid_after=0x00010001
  is_local_master=1 remastered=1

AUTONOMOUS REMASTER PROOF PASSED (rd vms-1ee, DLM rung H10a re-established,
executive-resident)
```

And the causal marker: `%CNXMAN, system 0000000000000402 was removed from
the cluster` -- `cnxman_log_membership_change()`'s own operator line, which
only `cnxman_notify_membership_changes()` can print -- appears **0 times**
in run1 and **1 time** in run2, at the exact moment the reconnect ladder's
PROPOSE_TRANSITION completes (t=138.68s). No panic/bugcheck string in either
console (`grep -aiE 'Kernel panic|BUG: |Oops: |general protection|%CNXMAN,
bugcheck|CLUEXIT'` == 0 hits, both runs).

## What this proves, and what it does not

Proves: a REAL, unannounced, timing-driven departure (no last-gasp, no
ioctl injected on the departed peer's behalf) is detected AUTONOMOUSLY by
the surviving node's own executive, and the DLM engine's already-correct
departure sweep (`vms_lock_dlm_member_departed`, unchanged by this item)
re-masters a previously peer-held resource onto the survivor on its next
use -- entirely inside vms.ko, no scsd, no ioctl this rig issued on B's
behalf.

Does NOT prove: H10b (lock-STATE rebuild onto the new master, rd vms-dca9)
or H11 (distributed deadlock search, rd vms-ec75) -- out of this item's
scope, deferred to vms-d55. Does not flip any compat register row (still
`implemented`, not `verified` -- no oracle byte-gate here).
