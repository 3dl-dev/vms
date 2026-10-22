# vms-3ac fire: the completeness fix, on a real 3-node cluster. Seven of seven.

Pod `lab3ac` in `ovmx-lab` (this lane's own pod, `requests == limits`,
**deleted after the run**), 2026-10-21 20:42-20:52 UTC. Two real OpenVMS VAX
V7.3 nodes + one booted OVMX node, in the **general configuration**: OVMX
`LOCKDIRWT 0`, both real VAXes `LOCKDIRWT 1` (OVMX's own console prints the
weight vector it received, `console-ovmxe.log`).

Build under test: `lab/vms-3ac-promote` at `e1cb3fd7e` = `work/vms-3ac-complete`
(PR #1843, the adoption converts this node's own locks out of proxy state) +
the lab-only RMHOLD staging commit. Artifacts from `build-boot-artifacts.yml`
run `39667248755`, whose `SOURCE_COMMIT` is exactly that commit;
`tests/lab/tools/ovmx_fire.sh` was the only way in and it refuses a mismatch or
a dirty tree. The frame-conformance gate passed for `dlm_enq_req dlm_rm_ack
dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14` before the node booted.

Scored against [`PLAN.md`](PLAN.md), committed in `30985e010` **before the pod
existed**. The predictions were not re-cut.

## The sequence, as it ran

| # | node | step | result |
|---|---|---|---|
| 1 | VAX1 | `RMHOLD 123WRA7T` | VAX1 masters `RA7T`: SDA `CSID 00000000`, granted `0400064F CR`, Valblk `RMLVB-0123456789` Seqnum 1, Sub-RSB count 3 |
| 2 | OVMXE | `RMHOLD 100-RA7T` | granted at CR. VAX1's SDA grows a second lock (`Lock Count 2`), and the grant is frame 8 below: `0104 -> e501 82/01`, master handle `0x0400064F`, OVMX's own `0x01000349` |
| 3 | VAX1 | `STOP HOLD1` | **the tree remasters onto OVMXE** (frames 9-24) |
| 4 | VAX2 | `RMHOLD 500-RA7T` (EX) | **not granted.** `SHOW SYSTEM/PROCESS=HOLD2` -> `RWSCS`; VAX2's SDA puts its own `5600023E EX` on the **waiting** queue, directory entry still `CSID 00010004` (OVMXE) |
| 5 | VAX2 | `STOP HOLD2` | **did not take effect** -- the process stayed in `RWSCS` (see "what this fire also found") |
| 6 | OVMXE | `^Y` | the holder ends; `%DCL-F-ABORT, image SYS$SYSTEM:RMHOLD.EXE terminated abnormally (signal 2)` |

## Scoring

**P1 -- the hand-over still happens. PASS.** [`frames.txt`](frames.txt),
frames 9-24:

```
VAX1 -> OVMXE  02/0e RA7T  -> 82/15 by OVMXE   (9, 10)
VAX1 -> OVMXE  02/0f RA7T  -> 82/15 by OVMXE   (11, 14)
VAX1 -> VAX2   02/0f RA7T  -> 82/15 by VAX2    (12, 13)
VAX1 -> OVMXE  02/1a       (refused: rd vms-f68)      (15)
VAX1 -> OVMXE  02/10 RA7T  -> 82/15 by OVMXE   (16, 17)
VAX1 -> VAX2   02/11 RA7T                             (18)
VAX1 -> OVMXE  02/13 RA7T                             (19)
VAX2 -> OVMXE  02/13 RA7T                             (20)
OVMXE -> VAX2  02/14 RA7T  -> 82/15 by VAX2    (21, 23)
OVMXE -> VAX1  02/14 RA7T  -> 82/15 by VAX1    (22, 24)
```

Rules W1 and W2 held again (the op-0x13 senders are exactly the systems the
op-0x14s went to, and every 0x14 follows every 0x13), and the conversion -- which
runs at the end of the adoption -- did not disturb the adoption.

**P2 -- THE CRASH FRAME IS NOT EMITTED. PASS, counted over the UNTRIMMED
capture** ([`absence.txt`](absence.txt)): of the **130** SCS-MSG frames the OVMX
node originated in the whole run, **ZERO** are `cat 0x02 op 0x03`. The crashing
fire had three, the third naming `0x370004A7`.

**P3 -- AND NOTHING HAD TO BE REFUSED. PASS.** `console-ovmxe.log` carries
**zero** `%DLM, not telling system` lines. The guard-only build printed exactly
one in this same sequence
([`../vms-3ac-invlockid-20261021/REFIRE.md`](../vms-3ac-invlockid-20261021/REFIRE.md),
R3: `%DLM, not telling system 65537 about lock 16778057 on 'RA7T': THIS node
masters that resource`). Two builds, one sequence, one variable: with the
adoption converting the lock there is no stale handle for the fail-safe to
catch. The only `%DLM` line this run is the op-0x1a refusal (rd vms-f68).

**P4 -- THE CONVERTED LOCK REALLY BLOCKS A REAL VAX. PASS on its letter, and
read the caveat below.** VAX2's EX request reached OVMXE as the master (frame
27: `53b9 -> e501 02/01 RA7T`, `body[30] = 0x05` = EX) and was **not granted**:
VAX2's process sat in `RWSCS` and its own lock stayed on VAX2's **waiting**
queue. The FAIL/STOP condition -- a real VAX granted EX while OVMX itself holds
the root at CR -- did **not** occur. That is the double grant the host test
(`test_dlm_deq_reachable` section 9) produces with the conversion disabled, and
it is what an unpromoted proxy would have caused here: a proxy LKB is on
`res->proxies`, which `lock_compatible()` does not read.

**P5 -- NO BUGCHECK ANYWHERE. PASS, counted.** Zero
`bug check|bugcheck|CPUSPINWAIT|fatal|VAXPORT|panic` lines on all three
consoles. VAX1 did not reboot (its console runs straight through the arm).

**P6 -- the cluster stays whole. PASS.** `F$GETSYI("CLUSTER_NODES")` reads 3 on
both VAXes after step 6. The only quorum events in `console-vax1.log` are at
20:11 and 20:22 -- the deliberate teardown of an EARLIER node and its
replacement, both **before** this fire's first step at 20:42.

**P7 -- the lock really went away. PASS.** The `$DEQ` completed locally (the
image aborted cleanly and emitted nothing, P2), VAX1 holds no resource block for
`RA7T` at all (`%SDA-E-NORSBNAM`), and VAX2's directory entry still reads
`CSID 00010004` -- OVMXE still masters the tree it was handed.

## What this fire ALSO found, and it is not this item's

**A QUEUED cross-node op-0x01 is never answered, and the real VAX wedges.**
OVMX received VAX2's EX request at 20:48:37 and sent **no DLM frame at all**
afterwards -- not a queued reply, and not a grant when its own CR lock went away
at step 6 ([`absence.txt`](absence.txt): the only two OVMX frames after that
moment are `cat-0x86 op-0x00` close answers four minutes later). So VAX2's
process stayed in `RWSCS`, and `STOP HOLD2` **could not complete it** -- the
exact symptom `src/kernel-core/vms_dlm_pending.h`'s own header records from
2026-10-09 ("even STOP could not complete it").

Two distinct gaps, both filed, neither caused by the change under test:

* **rd vms-28bd** (p0, blocks ci.6) -- a LOCAL release that flips a queued
  cross-node waiter to granted tells nobody. Filed WITH this fix from its own
  host test; this fire is the same thing measured on real hardware.
* **the queued ANSWER itself** -- OVMX did not answer the request when it
  queued it either, so the real VAX had nothing to wait on but silence.

Both are hang sources for the ci.6 evacuation, and both are a step BEYOND the
crash this item is about: the frame that bugchecked VAX1 is gone, and what is
left is a node that says too little rather than too much.

| file | what it is |
|---|---|
| `PLAN.md` | the predictions, committed before the pod existed |
| `trimmed.pcap.gz` | 27 of the capture's 13,555 frames by `tools/cluster/trim_lab_pcap.py`'s committed rule, payloads unaltered |
| `frames.txt` | those 27 decoded, in order |
| `absence.txt` | the absences, counted over the UNTRIMMED capture (a trimmed file cannot show an absence) |
| `console-*.log` | all three consoles for the whole run |
