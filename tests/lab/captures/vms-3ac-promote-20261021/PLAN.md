# vms-3ac fire: with the COMPLETENESS FIX, the release is local by construction

**Registered before the pod exists.** This is a REGRESSION re-fire of the
sequence that bugchecked VAX1, so the topology is deliberately the one that
crashed -- rotating it would prove something else. Held-out variation belongs to
RA7 fire B (rd vms-6ad1).

It differs from the guard's re-fire
([`../vms-3ac-invlockid-20261021/REFIRE.md`](../vms-3ac-invlockid-20261021/REFIRE.md))
in exactly one thing, and that is the point: with the guard alone, OVMX's own
lock in the adopted tree stayed a PROXY and its release was REFUSED at the post
(the console said so, once). With the completeness fix the adoption CONVERTS
that lock, so there is nothing to refuse. **The guard-only re-fire is the
measured control for this fire's P3.**

Build under test: `lab/vms-3ac-promote` at `8fe4c123e` -- `work/vms-3ac-complete`
(PR #1843: the adoption converts this node's own locks out of proxy state) on top
of `work/vms-c2b-arm` (PR #1841: the guard + the lab-only RMHOLD staging).
Artifacts from `build-boot-artifacts.yml` run `39661199932`, `cluster_auth_group=1`.
`ovmx_fire.sh` refuses unless `SOURCE_COMMIT` is that commit and the tree is
clean, and the frame-conformance gate must pass for `dlm_enq_req dlm_rm_ack
dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14` before the node boots.

## The sequence

| # | node | step |
|---|---|---|
| 1 | VAX1 | `RMHOLD 123WRA7T` -- takes the tree first and therefore MASTERS it (root CR, S0..S2 CW, value block `RMLVB-0123456789`) |
| 2 | OVMXE | `RMHOLD 100-RA7T` -- the root at CR, no sub-locks (rd vms-b67a is why: a sub-lock under a proxy parent is still looked up in the directory, so this arm holds the root only) |
| 3 | VAX1 | `STOP HOLD1` -- the old master stops holding its own, and the tree remasters onto OVMXE |
| 4 | VAX2 | `RMHOLD 500-RA7T` -- an **EX** request on the adopted root, which OVMX now masters *and holds at CR* |
| 5 | VAX2 | `STOP` that process -- cancel the queued request |
| 6 | OVMXE | `^Y` -- end the holder. **This is the step that bugchecked VAX1.** |

Step 5 is **required**, not tidiness: rd vms-28bd (filed with this fix, measured
by its own host test) is that a LOCAL release which flips a queued cross-node
waiter to granted tells nobody. Leaving VAX2's request queued across step 6
would hang VAX2's process on a known, filed gap and would measure that gap
instead of this fix.

## Predictions, each scored on its own

**P1 -- the hand-over still happens.** `0e/0f/10` from VAX1 to OVMXE each
answered `82/15`, a `0x13` from VAX1 and from VAX2, and an `op-0x14` from OVMXE
to each, each answered `82/15`. (The conversion runs at the end of the adoption;
it must not disturb the adoption.)

**P2 -- THE CRASH FRAME IS NOT EMITTED.** Counted over the whole untrimmed
capture: **zero** `cat 0x02 op 0x03` frames originated by the OVMX node. The
crashing fire had three, the third naming `0x370004A7`.

**P3 -- AND NOTHING HAD TO BE REFUSED.** OVMXE's console carries **zero**
`%DLM, not telling system` lines. The guard-only build printed exactly one in
this same sequence (that re-fire's R3, verbatim:
`%DLM, not telling system 65537 about lock 16778057 on 'RA7T': THIS node masters
that resource`). Absence here is the A/B against a measured control: it is what
"the stale handle is gone at its source" looks like from outside.

**P4 -- THE CONVERTED LOCK REALLY BLOCKS A REAL VAX.** VAX2's step-4 `RMHOLD
500-` does **not** report a granted EX while OVMX holds the root at CR: the
process waits (`SHOW SYSTEM` / `SHOW PROCESS` shows it in a wait state, and no
`RMHOLD` completion line appears on its console). **FAIL/STOP if VAX2 is granted
EX**: that is the double grant -- an unpromoted proxy is invisible to the local
granting algorithm, so before this fix OVMX would hand a real VAX an exclusive
lock on a resource OVMX itself holds. Its host twin is
`test_dlm_deq_reachable` section 9 ("a peer's EX on the adopted resource is
QUEUED behind the converted lock"), which fails with the conversion disabled.

**P5 -- NO BUGCHECK ANYWHERE.** Zero
`bug check|bugcheck|CPUSPINWAIT|fatal|VAXPORT|panic` lines on all three consoles
for the whole run, and VAX1 does not reboot.

**P6 -- the cluster stays whole.** `F$GETSYI("CLUSTER_NODES")` reads 3 on both
VAXes after step 6, and neither console reports `quorum lost`.

**P7 -- the lock really went away.** OVMXE's `$DEQ` returned success and
`SDA> SHOW RESOURCE/NAME=RA7T` on VAX2 lists no lock held for OVMX's CSID
afterwards, with the directory entry still naming OVMX as the master.

## FAIL / STOP

* **P5** fails -- any bugcheck, from any cause -- that is the result: capture it,
  reset the pod, report before anything else in this lane moves.
* **P4** fails granted -- stop. A double grant at a real VAX outranks the rest of
  this fire and takes its own item with the SDA pages attached.
* **P2** fails -- the fix does not work where it matters; the item stays open
  with the frame attached.
* A prediction that fails is the result, not a reason to re-cut the prediction.

## Lab hygiene

Own pod in `ovmx-lab`, `requests == limits`, created for this fire and
**deleted after it**. Consoles only through `ovmx-lab-console`. No non-SYS0 SIMH
VAX is ever auto-rebooted.
