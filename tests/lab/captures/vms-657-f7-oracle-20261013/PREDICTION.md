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
