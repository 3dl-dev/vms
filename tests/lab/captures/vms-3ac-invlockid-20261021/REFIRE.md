# vms-3ac re-fire: the same sequence, with the guard. No crash. Six of six.

Pod `labc2c` in `ovmx-lab` (a FRESH pod -- the crashed one's cluster could not
re-form after VAX1 rebooted into a quorum-less VAX2, so it was deleted and this
is a clean 2-node bed), 2026-10-21 18:2x UTC, **deleted after the run**. Two real
OpenVMS VAX V7.3 nodes + one booted OVMX node.

Build under test: `lab/vms-c2b-rmhold` at `d1f6efad1` = `work/vms-c2b-arm`'s
`fbeaa5e6d` (the guard) + the lab-only RMHOLD staging. Artifacts from
`build-boot-artifacts.yml` run `39649856427`, whose `SOURCE_COMMIT` is exactly
that commit; `ovmx_fire.sh` was the only way in, and the frame-conformance gate
passed for `dlm_enq_req dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14`
before the node booted. Scored against [`REFIRE-PLAN.md`](REFIRE-PLAN.md),
committed in `d7a28a53d` before the build existed.

## The sequence -- step for step the one that crashed VAX1

| step | what | result |
|---|---|---|
| 1 | VAX1 `RMHOLD 123WRA7T` | VAX1 masters `RA7T`; SDA: `CSID 00000000`, granted `05000395 CR`, Valblk `RMLVB-0123456789` Seqnum 1 |
| 2 | OVMXE `RMHOLD 100-RA7T` | granted; VAX1's SDA grows a second lock, `45000442 CR`, for OVMX's own `01000349` |
| 3 | VAX1 `STOP HOLD1` | **the tree remasters onto OVMXE** |
| 4 | OVMXE `^Y` -- release the holder | **the step that bugchecked VAX1. This time: nothing on the wire, and no crash.** |

## Scoring

**R1 -- the hand-over still happens. PASS.** [`frames.txt`](frames.txt), trimmed
frames 10-34:

```
VAX1 -> OVMXE  02/0e  RA7T    -> 82/15 by OVMXE  (frames 10, 19)
VAX1 -> OVMXE  02/0f  RA7T    -> 82/15 by OVMXE  (21, 24)
VAX1 -> OVMXE  02/10  RA7T    -> 82/15 by OVMXE  (26, 27)
VAX1 -> VAX2   02/11  RA7T                        (28)
VAX1 -> OVMXE  02/13  RA7T                        (29)
VAX2 -> OVMXE  02/13  RA7T                        (30)
OVMXE -> VAX2  02/14  RA7T    -> 82/15 by VAX2   (31, 33)
OVMXE -> VAX1  02/14  RA7T    -> 82/15 by VAX1   (32, 34)
```

The guard touches only what leaves the node, and it did not disturb the
adoption: rules W1 and W2 held again, and both real VAXes accepted OVMX's
op-0x14.

**R2 -- THE CRASH FRAME IS NOT EMITTED. PASS, counted over the whole untrimmed
capture** ([`deq-absence.txt`](deq-absence.txt)): of the 119 frames the OVMX node
originated, **zero** are `cat 0x02 op 0x03`. The crashing fire had three, the
third naming `0x370004A7`.

**R3 -- the refusal is said and counted. PASS.** OVMXE's console, verbatim:

```
%DLM, not telling system 65537 about lock 16778057 on 'RA7T': THIS node masters that resource
```

65537 is `0x00010001` -- VAX1's CSID -- and 16778057 is `0x01000349`, OVMX's own
lock id. That is precisely the message the crashing frame would have carried.

**R4 -- NO BUGCHECK ANYWHERE. PASS.** Zero
`bug check|bugcheck|CPUSPINWAIT|fatal|VAXPORT|panic` lines on all three
consoles, counted; VAX1 did not reboot (its console runs straight through the
arm into the SDA reads that follow).

**R5 -- the cluster stays whole. PASS.** `F$GETSYI("CLUSTER_NODES")` reads 3 on
both VAXes after step 4, and neither console carries `quorum lost`.

**R6 -- the lock really went away. PASS.** The `$DEQ` returned success, and
afterwards:

* VAX2 (the directory node): `SHOW RESOURCE/NAME=RA7T` -> `Status DIRENTR VALID`,
  `CSID 00010003` (**still mastered on OVMXE**), `Lock Count 0`, granted queue
  empty -- OVMX's lock is gone, released LOCALLY, as a lock on a resource this
  node masters must be;
* VAX1: `%SDA-E-NORSBNAM` -- it has no resource block for `RA7T` at all, which is
  exactly the state that made the old DEQ fatal.

## What this does and does not establish

It establishes that the frame which bugchecked a real VAX is no longer emitted,
in the arrangement that emitted it, on a real cluster -- rd vms-3ac's
done-condition (c).

It does **not** establish the completeness fix. The counter rising at all is the
remaining gap: OVMX's own lock in the adopted tree is still a PROXY pointing at
the old master, and the right end state is that the adoption converts it into a
master-side lock, after which nothing is refused because nothing is addressed
anywhere. That is banked on rd vms-3ac with the design (the engine's own
`vms_lock_dlm_assume_mastery` is the sibling transition), and rd vms-89dc -- the
value block this tree still loses -- closes with it.

| file | what it is |
|---|---|
| `refire-trimmed.pcap.gz` | 34 of the capture's 2,906 frames by `tools/cluster/trim_lab_pcap.py`'s rule, payloads unaltered |
| `frames.txt` | those 34 decoded, in order |
| `deq-absence.txt` | the absence, counted over the UNTRIMMED capture (a trimmed file cannot show an absence) |
| `refire-console-*.log` | all three consoles for the whole run |
