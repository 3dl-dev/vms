# vms-6ad1 RA7: prove a real VAX does not read the op-0x14's residue span

**Registered 2026-10-21, BEFORE the pod exists and before any byte of this
capture is taken.** This is the first time an OVMX node puts an op-0x14 in front
of a real OpenVMS VAX, and it is the arm the rd vms-42de ruling authorised.

Branch: `work/vms-ed5-remaster-arm` (PR #1834), on top of
`work/vms-380-remaster-codec` (PR #1825). The exact commit is recorded in
`SOURCE_COMMIT` beside the artifacts; `tests/lab/tools/ovmx_fire.sh` REFUSES to
start if the built tree is not that commit, or is dirty.

## The question, and why it needs a fire

A real V7.3 new master's op-0x14 names six fields and leaves the rest of its
132-byte body **as its own message buffer** — RA6 found another resource's name,
a fragment of an OPCOM message, and two VAX S0 kernel virtual addresses in that
span, and the 68-frame census found it is not constant in any role.

The rd vms-42de ruling (2026-10-13) says OVMX emits that span as **zeros**: not
its own kernel addresses (that leaks executive layout and is the value class
that bugchecked the lab twice, vms-239 r17/r19), and not copied from another
frame (INV-6 plumbing). The ruling is explicit that *"a sender's stale buffer
cannot be something a receiver depends on"* **is a hypothesis**, and that it must
be proven before a real peer gets one.

The proof it names: send the SAME remaster twice, once with the span zeroed and
once with a **second fixed pattern**, and pass only if the real VAX's SDA lock
and resource state and its observable behaviour are identical both times, with
no bugcheck on any node.

## Prerequisite P0 — and the arm STOPS here if it does not hold

**OVMX must be CHOSEN as the new master, and OVMX does not choose.** The 0x0e
arrives addressed to it. Every controlled arm measured so far (RA2, RA4, RA8)
shows the same selection: the new master is **a node that HOLDS a lock in the
tree**, and the move happens when the old master stops holding its own
(RA2: the tree moved ~5 s after VAX1's holder STOPPED; RA3: while the master held
locks of its own at equal LOCKDIRWT, the tree did not move in 30 s). RA1/RA1b
also measured that the SHUTDOWN path remasters only file-system trees, so this
is the ACTIVITY path.

So the arrangement is RA2's with OVMX in VAX2's place, and it requires something
this arm must VERIFY FIRST rather than assume:

> **P0. A real VAX master's SDA lists a lock held FOR the OVMX node on a tree
> that VAX masters.**
>
> Checked by `SDA> SHOW RESOURCE/NAME=RA7T` on the VAX master after OVMX's
> `$ENQ`, before anything is remastered: the resource is mastered on the VAX and
> carries an LKB whose CSID is OVMX's.

If P0 does not hold, **STOP and report**: RA7 cannot run, and what blocks it is
OVMX's requester path at a real VAX master, not the residue question. Do not
substitute an arrangement that makes OVMX the master some other way — a 0x14
sent for a tree nobody handed over proves nothing about the span.

## The two fires

One pod in `ovmx-lab`, `requests == limits`, image `ovmx-vaxlab:7`, **deleted
after the run**. Three nodes: two real V7.3 VAXes and one OVMX node booted by
`tests/lab/tools/ovmx_fire.sh`, which will not start until the frame-conformance
gate passes for every shape this fire sends:

```
tests/lab/tools/ovmx_fire.sh <pod> <artifacts> ra7a <build> \
    dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14
tests/lab/tools/ovmx_fire.sh <pod> <artifacts> ra7b <build> \
    dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14_ra7
```

The two fires differ in **exactly one variable**, and it is a configuration, not
a code change (rd vms-ed5 plumbed it):

| fire | `OVMX_RM_RESIDUE_FILL` in `OVMXVMSSYS.PAR` | op-0x14 body[10:12] and body[16:40] |
|---|---|---|
| **A** | absent (the zero-filled, unconfigured case) | `00` throughout — the ruling's zeros |
| **B** | `0x5A` | `5a` throughout |

`0x5A` is chosen because it is **not an address and not another frame's bytes**:
it is not a plausible VAX S0 pointer (no `0x87xxxxxx` form), it is not `0x10`,
`0x80`, `0xf9` or `0xfa` (the four values the corpus' own residue takes at
body[28] and body[34]), and it is not zero — so a VAX that reads ANY byte of the
span has something visibly different to react to.

**THE TOPOLOGY ROTATES BETWEEN THE TWO FIRES**
(`heldout-proof-must-vary-topology`): a residue cell read only in one role would
otherwise hide behind a fixed arrangement.

| | fire A | fire B |
|---|---|---|
| old master O | VAX1 | VAX2 |
| new master N | OVMXE | OVMXE |
| other member / holder H | VAX2 | VAX1 |
| tree | `RA7T` on VAX1 | `RA7T` on VAX2 |

N is OVMX in both — it has to be; it is the only node that can be made to emit
the two different fills. What rotates is which real VAX is the old master and
which is the bystander-holder, i.e. which role each real node reads the frame
in.

## Pre-registered predictions, each scored PASS/FAIL on its own

**P1 — the hand-over reaches OVMX's engine.** In each fire the capture shows
O→OVMXE `0x0e`, `0x0f`, `0x10`, each answered `82/15` by OVMXE; H→OVMXE one
`0x12` per lock H holds in the tree, each answered `82/01`; a `0x13` from every
other member; then OVMXE→O and OVMXE→H `0x14`.

**P2 — the answers are the real ones.** Every `82/15` and `82/01` OVMXE sends is
byte-identical to what `score_remaster` builds from the same request, outside
body[0:4] (the CM's envelope) and the two cells the codec does not name
(body[30:34] of an `82/01`). Scored by the committed program, not by eye.

**P3 — the handles are the engine's.** Each `82/01`'s body[20:24] appears in
`SDA> SHOW RESOURCE/NAME=RA7T` on the OVMX side as the master copy of that
lock, and in the real VAX holder's own later traffic for the tree. A handle that
appears nowhere in the lock database is an INV-6 failure, not a cosmetic one.

**P4 — THE RESIDUE IS NOT READ. The whole point.** Across fires A and B, on
every real VAX, these are IDENTICAL:

  a. `SDA> SHOW RESOURCE/NAME=RA7T` — the resource's master, its value block and
     sequence, and every LKB on it with its mode, its holder and its lock id
     (modulo the lock-id index values a fresh boot assigns, which are compared
     by SHAPE and by the lock they name, not by value);
  b. `SDA> SHOW LOCK` for each of that node's own locks in the tree — granted
     mode, requested mode, flags, parent;
  c. the node's reply to the `0x14` itself: a `82/15` echo whose body differs
     from the request only at body[8], body[9] and body[34] — **including the
     residue span, echoed back verbatim**, which is `00` in fire A and `5a` in
     fire B;
  d. what the node DOES NEXT: the same set of DLM frames in the same order, the
     same DCL steps completing, the same `SHOW CLUSTER` membership.

**P5 — no bugcheck, no hang.** Zero bugchecks on every console in both fires.
Every DCL command issued returns. Quorum never lost. No VAX DLM or CM request to
OVMXE left unanswered in either pcap.

**P6 — the frame is what the grounding table says.** Running
`tools/cluster/frame_conformance.py` over both captures' OVMX-originated 0x14s
reports every byte as a row of `tests/cluster/conformance/frame-grounding.tsv`,
with `dlm_rm_14` matching fire A and `dlm_rm_14_ra7` matching fire B, and no
unknown shape.

**P7 — the pattern did not reach a named field.** In fire B, body[8:10] is still
`02 14`, body[40:44] is still 0, body[44:48+L] is still the tree's identity and
body[128:132] is still the hash OVMX holds. (The host negative control
`dlm-rm-14-pattern-overruns-a-field` already proves the builder cannot overrun;
this is the same assertion on the real wire.)

**P8 — OVMX really serves the tree afterwards.** After each fire, a real VAX's
next `$ENQ`/`$CVT`/`$DEQ` on a lock in `RA7T` is answered by OVMXE from the LKBs
the adoption installed, and `SDA` on the VAX shows the grant. A tree adopted and
then not served is a hang with extra steps.

## FAIL / STOP conditions, stated before the fire

- **If any 'residue' cell changes what a real VAX holds or does** between fires A
  and B, that cell is **MEANINGFUL**: stop, re-classify it in
  `frame-grounding.tsv`, and report to the lane conductor under the vms-42de
  ruling's clause 4. Do not proceed to rd vms-93fa.
- **If a real VAX bugchecks in either fire**, the fire is a WIN (a crash-vector
  found in the lab, which is where they must be found) and a release-gating
  defect: capture the crash, reset the pod, and take it to its own item before
  anything else in this lane moves.
- **If P0 does not hold**, RA7 does not run. Report what blocked it.
- A prediction that fails is **the result**, not a reason to re-cut the
  prediction.

## What gets committed

`README.md` with every prediction scored honestly, both trimmed pcaps, the SDA
truth before and after on every node for both fires, all three consoles' logs
for both fires, the `score_remaster` output, the frame-conformance report, and
the exact `OVMXVMSSYS.PAR` lines that selected each fill. Then every op-0x14
cell goes into `frame-grounding.tsv` with its class — meaningful, or
residue-proven-by-RA7 — which is the rd vms-42de ruling's own done-condition.

## Lab discipline for this arm

- Reset the pod and crash-check BOTH VAX consoles FIRST, before each fire
  (`cluster-must-be-executive-resident`): a crashed peer wedges everything
  downstream and would be misread as a residue effect.
- Consoles only via `ovmx-lab-console`. Never auto-reboot a non-SYS0 SIMH VAX.
- Delete the pod when the run ends, and remove the worktree and build dirs
  (`agents-clean-up-their-detritus`).
