# vms-50d RA5: naming the remaster handles with SDA truth

Registered 2026-10-13, before the arm runs. VAX only (no OVMX node): this measures what real
V7.3 VAXes put in a remaster before OVMX is asked to take part in one (rd vms-380 -> vms-14fd).

## What RA4 left open (`../vms-380-remaster-arms-20261011/`, `/tmp/evac/rm380/ra4.pcap`)

Tree RMT4 moved from VAX1 (old master, O) to VAX2 (new master, N); VAX3 is a holder (H).
The op-0x12 and 82/01 cells were named against SDA in RA2/RA4. Still unnamed: the handles at
body[20:28] of 0e, 0f, 10, 11, 13, 14 and their 82/15 answers. In RA4:

| frame | [20:24] | [24:28] |
|---|---|---|
| 0e O>N | 13022020 | da01000a |
| 0f O>N | 13022020 | 0f090002 |
| 0f O>H | be040010 | 10090001 |
| 10 O>N | 13022020 | 78000008 |
| 13 O>N | 2e000006 | b104001b |
| 14 N>H | be04001a | b2030005 |
| 14 N>O | 1c090001 | b2030006 |

The low words index the lock tables: H-side 0x4be appears in both 0f O>H and 14 N>H, and
O-side 0x90f/0x910/0x91c appear in the 0f and 14 frames. None of these is a lock in the RMT4
tree on the SDA list RA4 took. Hypothesis to test: they are each node's own lock ids for the
remaster transaction.

## Arm

Pod `labrm5`, image `:7`, golden 3-node volume, VAX3 on root SYS2 (oracle_stage.sh). Run
`RMHOLD.MAR` from this directory.

1. VAX1: `RMH 001WRMT5`. Root NL with the value block written, plus S0 at NL. VAX1 masters.
2. VAX3: `RMH 123-RMT5`. Root CR, plus subs S0..S2 at CW.
3. SDA before, on every node: `SHOW RESOURCE/NAME=RMT5` and the locks it lists.
4. VAX2: `RMH 312BRMT5`. Then stop VAX1's holder, which is RA2's trigger for the tree to move.
5. When the 02/14 naming RMT5 has been sent, on every node: `ANALYZE/SYSTEM` then
   `SHOW LOCK <id>` for every handle in that tree's 0e/0f/10/11/13/14 frames and their 82/15
   answers. Every id is tried on every node, so a "no such lock" is recorded too. Then
   `SHOW RESOURCE/NAME=RMT5` again.

## Recorded, not predicted

For each handle: which node's lock it is, its resource name, its mode, and its owner process.
A cell is **named** only when SDA shows the same value on the node the index belongs to. Cells
that SDA does not resolve stay "open" and are not written by OVMX (vms-14fd builds 14 from
its engine's own state, never from an echo).
