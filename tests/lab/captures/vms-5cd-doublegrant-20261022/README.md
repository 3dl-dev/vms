# rd vms-5cd: OVMX granted EX on a resource its own process held at CR

Measured 2026-10-22 02:07-02:11Z in pod `lab85f`, the GENERAL configuration
(OVMXE `LOCKDIRWT 0`, both real V7.3 VAXes `LOCKDIRWT 1`, three members). Build
`lab/vms-85f-fire3` head `ece51fb30`, artifacts `build-boot-artifacts.yml` run
`39706710360`, with the `SOURCE_COMMIT` verified **inside the pod** after staging
(see the same commit's fix to `tests/lab/tools/ovmx_fire.sh`).

**This was not the fire this run set out to do.** It is the rd vms-85f /
vms-28bd sequence, which could not get past step 4: the request that must be
QUEUED was **GRANTED**.

## The sequence, as it ran

| time (UTC) | what |
|---|---|
| 02:07:16 | VAX1 `RMH 123WRA7V` -- takes a FRESH tree and masters it (`H1F` in `HIB`) |
| 02:07:46 | OVMXE `RMHOLD 100-RA7V` -- **granted the root at CR, `root lkid 01000356`**, and hibernates holding it |
| 02:08:18 | VAX1 `STOP H1F` -- the tree remasters onto OVMXE, the only other holder |
| 02:08:5x | `f194/f196` `02/0e` -> `82/15`, `f197/f199` `02/0f` -> `82/15`, `f201/f204` `02/10` -> `82/15`, then `f205` `02/13` naming `master_lkid 0x01000356` -- **OVMX's own lock** |
| 02:09:00 | VAX2 `RMH 500-RA7V` (EX): `f355` to VAX1, `f356` redirect, `f357` to OVMXE |
| 02:09:00 | **`f359`: OVMXE -> VAX2 `cat-0x82 op-0x01`, `body[34]=0xfa` -- GRANTED** |
| 02:09:18 | VAX2's `H2F` is in `HIB`: it believes it holds EX. OVMX's own holder was still hibernating on its CR until 02:10:56 |

Two incompatible modes granted on one resource, and the second grant is a frame
OVMX put on the wire.

## The mechanism, out of OVMX's own counters

`vms_cnxtrace` on OVMXE at 02:10:56 (`console-ovmxe.log`):

```
%CNXTRACE-I-DLM, lock-manager wire arm: connected=1 lockdirwt=0 gen=3 proxy_lkbs=2
%CNXTRACE-I-DLMREQ, requests sent=9 received=1 grants sent=1 received=2 declined=3
%CNXTRACE-I-DLMRM, trees opened=1 refused=0  records adopted=0 refused=0  ends=1
%CNXTRACE-I-DLMRMHELD, trees held=0  op-0x14 sent=0 unsent=0  completions unreported=0  other-master notices=0
```

The tree **opened** and **ended** with **zero records adopted** and **never
reached HELD**, so no `op-0x14` was ever originated -- and OVMX's own lock is
**still a PROXY** (`proxy_lkbs=2`). A proxy LKB is not on the resource's granted
queue that the mode-compatibility check walks, so from that check's point of
view the resource was EMPTY and EX was compatible. The grant is correct for the
state the engine was in and wrong for the state the cluster was in.

## Why this is not rd vms-3ac's completeness fix, which IS in this build

That fix converts this node's own locks out of proxy state **at adoption**.
Here the adoption never completed, so the conversion never ran -- and nothing
refused service on the tree in the meantime. The SAME arrangement 100 minutes
earlier, on the previous build (`../vms-85f-queued-answer-20261021/FIRE.md`,
fire q85f1) read `trees opened=1 records adopted=0 trees held=1 op-0x14 sent=2`
and answered VAX2's EX `0xfb` QUEUED, correctly. The difference is whether the
adoption reached HELD, not the compatibility check.

## The two questions, both measurable, neither guessed here

1. **Why did the adoption end without holding?** In q85f1 TWO members sent an
   `op-0x13` (both VAXes held locks in the tree); here only ONE did, because
   VAX2 held nothing yet. Whether the remaster FSM requires something it did not
   get, and whether `ends=1` with `trees held=0` is a state it should ever be
   in, is the first thing to read.
2. **What should a master do while a tree it has been handed is NOT YET HELD?**
   Serving a conflicting request out of a partially built resource is how this
   grant happened. That is a design question for the architect.

## Never-crash held

Zero `bug check|bugcheck|CPUSPINWAIT|fatal|VAXPORT|panic|CNXMGRERR` over all
three whole consoles (900 + 680 + 225 lines). The defect is a wrong GRANT, not a
peer crash.

## Files

| file | what |
|---|---|
| `trimmed.pcap.gz` | the 12 frames the committed trim rule kept (payloads unaltered) |
| `frames.txt` | those frames decoded, the request/answer pairs, and every frame OVMXE originated |
| `console-*.log` | all three consoles, whole |
