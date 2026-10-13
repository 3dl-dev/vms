# vms-50d RA5 result (pod labrm5, 2026-10-13 18:51–19:20)

Run as registered in PLAN.md. Deviation: VAX2's single `RMH 312BRMT5` did not move the tree
in 4 min, so three more activity processes (`RMH 012BRMT5` x3) were spawned at 19:08:59, as
RA4 did. Tree RMT5 then moved VAX1 (O, 0104) -> VAX2 (N, 309a) at capture-relative 1110.44 s.
VAX3 (H, 1332) holds the root at CR plus S0..S2 at CW, and is also the directory node for RMT5
(SDA `DIRENTR`; RSB CSID 00010001 before, 00010002 after).

Files:
- `rm5-remaster.pcap`: every 0x6007 frame from 1109.5 s to 1111.5 s. The full capture is
  `/lab/run-oracle/rm5.pcap` on the lab PVC.
- `rmhandles.py`: extracts the frames and handles.
- `sda-before-vax{1,2,3}.txt`: SDA before the move (VAX2 shows HOLD2's locks).

SDA truth before. VAX1, the master, holds RMT5 granted 0400082F CR and 0A00047D NL. Lock
0400082F is VAX1's master copy of H's root lock. H's own root lock is 110000CE (CR, PID
00010016).

## The frames, and what SDA names in them

| frame | [20:24] | [24:28] | SDA |
|---|---|---|---|
| 0e O>N | 20200213 | 22000844 | [24:28] is no lock on any node after the move |
| 0f O>H | 20200213 | 0D00047B | not a current lock |
| 0f O>N | 20200213 | 0E00047B | not a current lock (same O index 0x47b, next sequence) |
| 10 O>N | 20200213 | 0B000837 | not a current lock |
| **11 O>H** | **0400082F** | **110000CE** | **[20:24] = O's master copy of H's root lock; [24:28] = H's own root lkid** (both SDA) |
| **12 H>N** | 20600216 | **110000CE** | [20:24] = H's holder EPID (RA4); **[24:28] = H's root lkid** |
| **13 O>N** | 01000833 | **180000A9** | **[24:28] = H's S2 lkid, the last sub** (SDA on VAX3); [20:24] = O's master copy of it (RA4 pattern) |
| 13 H>N | 0 | 0 | the holder's end-of-records marker |
| **14 N>O** | 3200019E | **060004C8** | **[24:28] = a lock on N (VAX2, PID 0001001A, sub S0, CR)**; [20:24] is no current lock |
| 14 N>H | 2040021A | 040004C8 | [24:28] is N's lock slot 0x4c8 at sequence 04; [20:24] looks like an EPID on VAX2 |

The answer to each frame (82/15) is the request echoed, except that it sets body[34] f9 -> fa.

## Reading

- 0e/0f/10 [20:24] = 20200213 is the same in all three frames, has the form of an EPID on
  VAX1, and matches RA4 (13022020). Their [24:28] are short-lived VAX1-side lock ids that no
  longer exist when SDA runs after the move. They are the old master's own per-message
  handles, so OVMX as N copies them back in its 82/15 echo and never needs to mint them.
- **14's [24:28] is a new-master lock id (index 0x4c8 on VAX2) of a lock N holds in the tree.**
  That confirms vms-380's finding: 14 carries handles minted by the adopting engine. OVMX can
  build 14 only after vms-14fd's adoption gives its engine a lock of its own in the adopted tree.
  [20:24] in 14 (3200019E toward O; 2040021A, which looks like an EPID, toward H) is still open.
- Still open: exactly which N lock 14 names for each recipient (SDA was taken about 2 min
  after the move, and the slot had been reused: sequence 04 toward H, 06 toward O); and
  14 [28:40].
