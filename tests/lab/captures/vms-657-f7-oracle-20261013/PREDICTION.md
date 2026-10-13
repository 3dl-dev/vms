# vms-657: a former master's 0xf7 answer, provoked on real VAXes

Registered 2026-10-13, before any run of this arm.

## Why

OVMX has to answer a request for a resource it no longer masters, instead of staying silent
(rd vms-e09 part c). The only real answer seen so far is 0xf7: 5 frames, all in L2, all
during a remaster of CACHE$cmSYSDSK1, with 4 of the 5 matching the echo shape (R4). That is
not enough to send it to a VAX. This arm makes three real V7.3 VAXes produce it repeatedly.

## Arm (VAX only; golden 3-node volume; pod NODES="vax1 vax2", vax3 staged on root SYS2)

Programs: `RMPOKE.MAR` (this directory) and `RMHOLD.MAR`, the vms-380 remaster arm
(`/lab/k8s-labs/rm380/RMHOLD.MAR`, copied here). Both go to SYS$LOGIN by FTP (`vput.py`).

1. VAX1 runs `RMHOLD 110-F7ROOT`: it takes the root F7ROOT at CR first, so VAX1 masters
   the tree.
2. VAX3 runs `RMPOKE 9F7ROOT`: 9000 rounds of $ENQW CR on F7ROOT, then $DEQ. Each round's
   first request is addressed by a fresh directory lookup.
3. While RMPOKE runs, VAX2 runs `RMHOLD 114AF7ROOT`: the root at CR, 4 sub-locks, then 300
   rounds of NL<->CR converts. This is the vms-380 activity that moves the tree's mastery to
   VAX2.

The whole run is captured on br0.

## Predictions

- **F1.** At least one 82/01 with body[34] = 0xf7 goes from VAX1 to VAX3, answering a
  request for F7ROOT, after VAX2's activity starts.
- **F2 (R4).** Each 0xf7 answer is its request echoed, except body[0:4] (the envelope),
  body[8] = 0x82, and body[28:39], where body[34] = 0xf7 and body[28:32] = 0. Pass: 100% of
  the 0xf7 answers to an op-0x01.
- **F3, the follow-up, recorded and not predicted.** After an 0xf7, what VAX3's next F7ROOT
  request is (to whom, which opcode) and how long it comes after.

**Pass:** at least 20 0xf7 answers over at least 2 runs, with F2 holding on every one. The
first run is the derivation set and later runs are held out. OVMX does not send 0xf7 until
this file's result section says the held-out runs passed.

## Run 1 (pod labf7, 2026-10-13 13:31–13:38): derivation run, F1 NOT observed

Done as registered: VAX1 ran RMHOLD 110-F7ROOT, VAX3 ran RMPOKE 9F7ROOT (9000 rounds), and
VAX2 ran RMHOLD 114AF7ROOT.
- **F1 failed: no 0xf7 in the capture.** Every one of the 9000 lookups for F7ROOT got
  0xf8 from the directory node, naming VAX1, and no remaster frame (cat-0x02 op
  0x0e..0x14) named F7ROOT. The tree never moved, so there was no former master to ask.
- Remastering did happen for other system trees during the run. Those frames are in the
  capture (`/lab/run-oracle/f7r1.pcap` on the lab PVC), and the remaster sequence is the
  one L2 shows.
- Station identities are ambiguous here: each VAX's MAC changes when DECnet starts (four
  SCA source MACs for three nodes). The next run maps MAC to node from the cat-0x01 MODEL
  frames before counting.

**Amendment for run 2, registered before it runs.** The poker was the most active node on
F7ROOT, which plausibly kept the tree from moving to VAX2. VAX3's RMPOKE now runs one batch
of 1000 rounds per node-wide DCL loop iteration, with `WAIT 0:0:2` between batches. VAX2's
activity grows to 3000 rounds (RMHOLD with a third digit). F1, F2 and F3 stand.

**Run 2 as fired, and step 4 (registered 2026-10-13, before run 2).**
- Implementation of the amendment: VAX3 runs `$ L: RMPOKE 1F7ROOT` / `$ WAIT 0:0:2` /
  `$ GOTO L`. VAX2 runs `RMHOLD 114BF7ROOT`. `B` is a new activity mode in `RMHOLD.MAR`:
  `A`'s loop with 3000 rounds instead of 300.
- Step 4, added. rd vms-380 RA2/RA3 (`../vms-380-remaster-arms-20261011/README.md`) found
  that a master holding its own locks does not give the tree up to an equal-LOCKDIRWT
  node's activity, and that the tree moved VAX1 -> VAX2 only after VAX1 stopped its holder.
  Run 1 kept VAX1's RMHOLD running throughout. So, if no 0xf7 has appeared 120 s into VAX2's
  activity, VAX1's RMHOLD is stopped (Ctrl-Y, STOP) while VAX2 and VAX3 continue.
  Steps 1–3 alone are recorded as the registered amendment's result.
- **F1 now reads:** an 0xf7 from VAX1 to VAX3 for F7ROOT, after VAX2's activity starts, in
  steps 1–3 or after step 4. F2 and F3 are unchanged.
- **Node mapping:** all three nodes advertise the same model text, so MODEL frames cannot tell
  them apart. Nodes are mapped from the SCSNODE name in each source MAC's SCS START/STACK
  frames.

## Run 2 (pod labf7b, 2026-10-13 17:05–17:20): F1 NOT observed; the tree moved

Run as registered above: steps 1–3, then step 4. Full capture on the lab PVC at
`/lab/run-oracle/f7r2.pcap`. `f7r2-f7root.pcap` here holds 40 HELLOs plus every F7ROOT frame
from capture-relative 597–602 s. Counts are from `f7count.py`; nodes were mapped from the
HELLO SCSNODE at abs 41.

- **F7ROOT moved VAX1 -> VAX2 at capture-relative 599.24 s, during steps 1–3, with VAX1's
  RMHOLD still holding the root.** VAX1 -> VAX2 sent 02/0e, 0f, 10, 12, 13, then
  VAX2 -> VAX1 sent 02/14. This is the RA2 sequence. With mode B (3000 rounds) the master's
  own lock did not keep the tree. RA3 had seen it kept for 30 s with 300 rounds.
- **0 answers with body[34] = 0xf7.** VAX3 made 12,000 F7ROOT lookups to the directory
  node VAX2 before step 4 (76,292 by the end). 7000 were answered 0xf8 naming VAX1, and VAX3
  then sent each of those 7000 requests to VAX1. VAX3's last request to VAX1 left at
  599.136 s, 103 ms before the remaster. After the move, the directory named the new master,
  and no request went to VAX1. Step 4 (VAX1's holder stopped at 17:16:46) changed nothing:
  the tree had already gone.
- **Why this arm cannot produce 0xf7.** A requester that does a fresh directory lookup every
  round is told the current master. The directory changes in the same exchange as the
  remaster, so a stale master is never named.

**Amendment for run 3, registered before it runs.** The request has to come from a node that
already knows the master and does not look it up again.
- VAX3 holds F7ROOT at NL for the whole run (its RSB keeps master = VAX1).
- In a loop, VAX3 takes and releases a second CR lock on F7ROOT. That request goes straight
  to the master VAX3 has cached, with no lookup.
- The tree has to move repeatedly to give enough races. VAX1 and VAX2 alternate mode-B
  activity (each run once, then the other), so the master moves back and forth.
- F1: at least one 0xf7 from the former master to VAX3 for F7ROOT. F2 and F3 are unchanged.
- Run 3 is the derivation set; runs 4+ are held out. Pass threshold as registered above.

## Run 3 (pod labf7c, 2026-10-13 17:25–17:43): F1 NOT observed; the tree never moved

Capture on the lab PVC: `/lab/run-oracle/f7r3.pcap`. Raw sequence:
- 17:31:25: VAX1 ran `RMH 110-F7ROOT`.
- 17:31:28: VAX3 started `RMC 9F7ROOT` in a loop. Its cached-master requests went straight to
  VAX1 (151,534 F7ROOT 02/01 frames VAX3 -> VAX1 by 17:33, with no lookup), as intended.
- 17:31:32: VAX2 ran `RMH 114BF7ROOT`.
- Deviation, recorded here: after 4 min with no move, VAX3 was re-paced to `RMC 1F7ROOT` plus
  `WAIT 0:0:3`, VAX2's `RMH 114BF7ROOT` was restarted (17:35:59 and 17:41:35), and VAX1's
  holder was stopped (17:39:16).
- **No remaster frame (02/0e..14) named F7ROOT in the whole run, and 0 answers had
  body[34] = 0xf7.** VAX3 sent 526,331 requests to VAX1 over 17 minutes.

Observation, not yet a rule: in run 2 the tree moved 6 s after VAX2's mode-B activity began,
while VAX3 took only short-lived locks. In run 3 VAX3 held an NL lock on the root and drove
heavy activity at VAX1, and the tree stayed with VAX1 for the whole run, even after VAX1
released its own lock. Next arm, to register before firing: begin as run 2 did (fresh-lookup
poker, so the tree moves), and start VAX3's held-NL cached requester only once the 02/14
naming F7ROOT has been seen. Its requests will then go to the cached master VAX2. Then drive
the tree back to VAX1 with VAX1 mode-B activity while VAX2 releases its locks, so that
VAX3's cached requests race the second move.
