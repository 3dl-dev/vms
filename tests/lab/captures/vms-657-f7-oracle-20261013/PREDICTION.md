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
