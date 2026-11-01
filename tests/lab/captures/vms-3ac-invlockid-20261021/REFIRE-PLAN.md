# vms-3ac re-fire: the same sequence, with the guard, must not crash VAX1

**Registered before the re-fire, and before the build it needs exists on the
pod.** This is a REGRESSION re-fire, not a new held-out arm, so the topology is
*deliberately the one that crashed* -- rotating it would prove something else.
Held-out variation belongs to RA7 fire B (rd vms-6ad1), which is blocked behind
this item.

Build under test: `lab/vms-c2b-rmhold` at `d1f6efad1` -- `work/vms-c2b-arm`'s
`fbeaa5e6d` (the guard) plus the lab-only RMHOLD staging. Artifacts from
`build-boot-artifacts.yml` run `39649856427`; `ovmx_fire.sh` refuses unless
`SOURCE_COMMIT` is that commit and the tree is clean.

## The sequence, step for step the one that crashed

1. VAX1: `RMHOLD 123WRA7T` -- VAX1 takes the tree first and therefore MASTERS
   it (root CR, S0..S2 CW, value block `RMLVB-0123456789`).
2. OVMXE: `RMHOLD 100-RA7T` -- the root at CR, no sub-locks (rd vms-b67a is
   why: a sub-lock under a proxy parent is looked up in the directory and
   mastered locally, so this arm holds the root only).
3. VAX1: `STOP HOLD1` -- the old master stops holding its own, and the tree
   remasters onto OVMXE.
4. OVMXE: end the holder (`^Y`). **This is the step that bugchecked VAX1.**

## Predictions, each scored on its own

**R1 -- the hand-over still happens.** The capture shows `0e/0f/10` from VAX1 to
OVMXE each answered `82/15`, a `0x13` from VAX1 and from VAX2, and an `op-0x14`
from OVMXE to each of them, each answered `82/15`. (The guard must not have
broken the adoption; it touches only what leaves the node.)

**R2 -- THE CRASH FRAME IS NOT EMITTED.** After step 4, the pcap contains **no**
`cat 0x02 op 0x03` from OVMXE to VAX1 naming the master handle VAX1 minted
before the move. This is the whole point.

**R3 -- the refusal is said and counted.** OVMXE's console carries
`%DLM, not telling system <csid> about lock <id> on 'RA7T': THIS node masters
that resource` exactly once.

**R4 -- NO BUGCHECK ANYWHERE.** Zero
`bugcheck|CPUSPINWAIT|fatal|VAXPORT|panic` lines on all three consoles for the
whole run, and VAX1 does not reboot.

**R5 -- the cluster stays whole.** `F$GETSYI("CLUSTER_NODES")` reads 3 on both
VAXes after step 4, and neither reports `quorum lost`.

**R6 -- the lock really went away.** OVMXE's own `$DEQ` returned success, and
`SDA> SHOW RESOURCE/NAME=RA7T` on VAX2 no longer lists a lock for OVMXE's CSID
(the release was local, and it was a release).

## FAIL / STOP

* If **R2** fails the fix does not work where it matters and the item stays
  open with the frame attached.
* If **R4** fails -- any bugcheck, from any cause -- that is the result: capture
  it, reset the pod, and report before anything else moves.
* A prediction that fails is the result, not a reason to re-cut the prediction.
