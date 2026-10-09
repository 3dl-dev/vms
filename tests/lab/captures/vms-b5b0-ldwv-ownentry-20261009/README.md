# vms-b5b0 ev7 — the node that left itself out of its own lock directory

What the console showed, and what it means. Every line quoted below is in
`OVMXE-boot1.console.log` in this directory.

## The run

| | |
|---|---|
| build | `evac/int-6` (`c89f6e25` = int-5 + `7750c881`; the int-5..int-6 source diff is only `vms_dlm_echo_guard.{c,h}`) |
| cluster | VAX1 + VAX2 (OpenVMS VAX V7.3, LOCKDIRWT 0, confirmed by `F$GETSYI`) + OVMXE (`SYSBOOT SET LOCKDIRWT 1`) |
| this node's CSID | `0x00010008` — its **ninth** incarnation in this cluster today |

## The lines that matter

```
%CNXMAN, membership bitmap is wider than the grounded byte; recording, not decoding
%CNXMAN, this node is now a VAXcluster member
%DLM, a directory lookup arrived for a resource this node's own weight vector does not direct here
%DLM, lock directory weight vector: 2 entries over 2 systems, 0 of them this node's
%CNXTRACE-I-DLM, ... lockdirwt=1 dirvec_own=0 gen=2
CNXTRACE ... ev=CSID_LEARNED ... aux=0x00010008
```

## Root cause — a cliff, not a corner case

A rejoining system gets a **new** CSID (Davis p. 7-25), so a node's CSV slot
climbs with every rejoin. The ninth incarnation landed on **slot 8**, one past
the **eight** slots this executive has grounded of the transition nodemap (spec
§4(p): the field's true width is undetermined, "do not assume 8 slots").

Each step was individually correct, and together they split the directory:

1. `phase2_csb_in_nodemap()` answered **UNKNOWN** for slot 8 and left `SELECTED`
   alone — silence is not a refusal.
2. The cluster admitted the node anyway, off a real op-0x0c transition-done, and
   the join FSM set its own CSB's **MEMBER** flag.
3. Both VAXes (LOCKDIRWT 0) built vectors consisting of **the OVMX node alone**
   and directed every lookup at it.
4. The OVMX node's own vector required `SELECTED` to count a member, so it
   **omitted itself**: 2 entries, 2 systems, none of them its own.

Lookups arrived at a node whose own vector said it was not the directory.

## The fix, and what is now visible

For **the local CSB** the committed `MEMBER` flag counts as well as `SELECTED`
(`ldwv_csb_counts`). Scoped to the local node deliberately: for a remote system
the selection set is what the transition said. Both flags are real executive
state; no weight, entry or identity is invented.

Every rebuild now prints **one line per member**, so the comparison against SDA
is a diff and not an inference:

```
%CNXMAN, lock directory weight vector, by member (compare with SDA SHOW CLUSTER on a VMS member):
%CNXMAN,   csid=00010001 slot=1 lockdirwt=0 entries=0 [not this implementation]
%CNXMAN,   csid=00010002 slot=2 lockdirwt=0 entries=0 [not this implementation]
%CNXMAN,   csid=00010008 slot=8 lockdirwt=1 entries=1 (this node)
```

and a committed member entitled to entries whose vector gives it none is
announced, diagnosis first, and counted (`club->ldwv_own_entry_missing`):

```
%CNXMAN, DIRECTORY SPLIT: this node is a cluster member and its LOCKDIRWT
entitles it to directory entries, but its own lock directory weight vector
gives it none -- the other members are directing directory lookups at a node
whose own vector says it is not the directory (member readout above)
```

The watchdog is deliberately **not** "own entries == 0": a node at LOCKDIRWT 0
beside a weighted peer holds none, and that is p. 6-32's own answer.

## int-7 (slot 10): the fix had to go deeper than the vector

Re-fired with the first fix in (`evac/int-7`, `fab7e93d`), this node was assigned
**slot 10** and the vector line was unchanged: `2 entries over 2 systems, 0 of them this
node's`. Counting our own committed membership in the vector was necessary and **not
sufficient** — at a slot the nodemap cannot express, **nothing set the local CSB's MEMBER flag
at all**:

- Phase 2 reads the map, finds silence about us, and correctly leaves the flags alone.
- The join FSM's promotion fires on the **completion** (§4(q)) — and used to set only its own
  FSM state.

So the console said `this node is now a VAXcluster member` while the node's own CSB said
otherwise, and `cl->state` — what `SHOW CLUSTER` and `$GETSYI` project, and what
`join_node_already_member()` reads — never reached MEMBER either. That last part means a
high-slot node also keeps asking to be admitted.

The promotion now hands Phase 2 the fact it was missing
(`cnxman_phase2_local_committed`), and **Phase 2 stays the owner of all four of its p. 7-42
tasks**: the local CSB's MEMBER/SELECTED flags, the CLUSTER flag, and a **re-fill of the weight
vector** (the commit had built it before this node's own membership was known). Counted as
`club->local_committed_off_map`, so a diagnostic says the map was SILENT rather than implying it
agreed; idempotent when the map did name us. Proven at **every slot 8..15** plus a control at
slot 3.

## Still open (not papered over)

The nodemap's **true width**. Decoding past the grounded byte needs a capture of
a real transition whose membership spans slots above 7 — bit order, offset and
length — and inventing it is the fabrication class INV-6 names. Until then
phase2 still records rather than decodes such a map, now counting the case where
the slot it cannot express is **this node's**
(`cnxman_phase2_stats.local_slot_unexpressible`). A **remote** member above slot
7 still cannot be selected by the nodemap; it is admitted by the membership
records that name it, and this fix does not reach it.

## What the lab must show next

A high-slot OVMX node (slot ≥ 8) that shows, in this order:

1. `%CNXMAN, this node is a member of the cluster` **and** `SHOW CLUSTER` agreeing (not just the
   join FSM's own line);
2. its member readout naming itself with `entries>=1`;
3. no `DIRECTORY SPLIT` line;
4. SDA `SHOW CLUSTER` on **both** VAXes agreeing with that readout member for member;
5. the evacuation arm's lookups answered by the node they were sent to.
