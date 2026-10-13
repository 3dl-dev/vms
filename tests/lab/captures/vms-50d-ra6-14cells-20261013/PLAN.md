# vms-50d / vms-14fd RA6: naming op-0x14's last two cells, with SDA truth taken WITHIN SECONDS

Registered 2026-10-13, **before the arm runs**. VAX only (no OVMX node): this measures what
real V7.3 VAXes put in the frame that ends a remaster, before OVMX is asked to send one.

## What is open, and why RA5 could not close it

`../vms-50d-ra5-14cells-20261013/` named 11, 12, 13 and the 0e/0f/10 handles against SDA.
It left **op-0x14 `body[20:24]` and `body[28:40]`** open, for one reason: SDA ran about two
minutes after the 02/14 and the lock slot the frame named had already been reused (the two
0x14s of that remaster carried the same VAX2 lock-table index `0x4c8` at two different
sequence numbers, `04` and `06`).

A 4,658-frame census of real 0x14s (repo captures + `/lab/run-oracle` + rm380 + dlmlab) says
these cells are **not** constant and so cannot be shipped as constants: `[20]` takes 255
distinct values, `[28]` 69, `[32]` 150, and `[30]` is one of `f1/f0/d0/00`. Only `[22]=00`
(90%), `[26]=00` (98%) and `[34]=fa` (89%) dominate.

## What RA6 changes

1. **The SDA truth is a LADDER, not a single reading.** Every node runs a snapshot loop
   across the whole trigger window, so a complete lock-database reading exists within a few
   seconds either side of the 02/14. There is then no race between decoding the capture and
   asking SDA: the id is looked up in a snapshot that was already taken.
2. **The roles are ROTATED** (`heldout-proof-must-vary-topology`). RA5 was
   O=VAX1, N=VAX2, H=VAX3. RA6 is **O=VAX2, N=VAX3, H=VAX1** — and VAX3 is the lab's
   DISKLESS SATELLITE, so if any cell were a property of a disk-served node, or of join
   order, this arm says so.
3. **Processes are read too**, because `[20:24]` has the shape of an EPID on the sender
   (RA4's 0e/0f/10 `[20:24]` = `13022020`, RA5's = `20200213`, both EPID-shaped; RA4 already
   named the op-0x12 `[20:24]` as the HOLDER's EPID). Each snapshot carries the node's
   process list.

## Rig

Pod `labrm6` (own pod, deleted after the run), image `:7`, `NODES="vax1 vax2 vax3"`,
golden 3-node volume, requests == limits. `SET CLOCK NOCATCHUP` present in the generated
`vax.ini` (lab rule, rd vms-549c). Capture: `tcpdump -i br0` inside the pod.
Holder program: `RMHOLD.MAR` from `../vms-50d-ra5-14cells-20261013/`, unchanged.

## Sequence

1. VAX2 (**O**): `RMH 001WRMT6` — takes the root EX first, writes the known value block, and
   settles at NL with sub-lock S0. VAX2 masters RMT6 because it named it first.
2. VAX1 (**H**): `RMH 123-RMT6` — root CR plus S0..S2 at CW.
3. **SDA truth BEFORE**, on every node: `SHOW RESOURCE/NAME=RMT6` and the locks it lists,
   plus the process list.
4. Start the **snapshot ladder** on all three nodes and `tcpdump` in the pod.
5. VAX3 (**N**): `RMH 312BRMT6` activity processes (RA5 needed four before the tree moved).
6. Stop VAX2's holder, which is RA2's trigger. The tree moves VAX2 -> VAX3.
7. Stop the ladder. Find the 02/14 in the capture, and resolve every id it carries in the
   snapshot nearest in time on the node whose table the id indexes.

## PREDICTIONS (registered before the fire; each one falsifiable)

| # | prediction | falsified if |
|---|---|---|
| P1 | `0x14 [24:28]` is a lock id **on the new master (VAX3)**, present in VAX3's nearest snapshot | no VAX3 snapshot within 10 s of the 02/14 contains it |
| P2 | ...and that lock is on a resource **of the remastered tree** (RMT6 or one of its subs) | the snapshot resolves it to some other resource |
| P3 | `0x14 [20:24]` is an **EPID of a process on the new master** — the owner of the lock in `[24:28]` | no VAX3 process has that EPID in the nearest snapshot |
| P4 | the two 0x14s (to O and to H) carry **different `[24:28]` values with the SAME low-16 index** — one slot, two sequence numbers | the indices differ, or the values are equal |
| P5 | `[34] == 0xfa` in every 0x14 of this run | any 0x14 carries something else there |
| P6 | `[28] == 0x10` in every 0x14 of this run | any 0x14 carries something else there |
| P7 | every 0x14 is answered `82/15` as a **verbatim echo** | any answer differs from the request in any byte |

## What this arm will NOT conclude

A cell the ladder does not resolve stays **OPEN**, and OVMX continues to emit **no op-0x14**
(rd vms-14fd's engine already adopts the tree and counts every adoption it could not report).
No cell is accepted on shape alone: "looks like an EPID" is a prediction here, not a finding.
A finding requires the value to appear in a snapshot, named, on the node whose table it
indexes.
