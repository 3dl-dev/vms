# vms-50d / vms-14fd RA6 result — op-0x14's open cells stay OPEN, and the reason is now named

Pod `labrm6b`, 2026-10-13 21:00–21:18 UTC, deleted after the run. Three real V7.3 VAXes,
**two complete remasters captured** (RMT6 and RMT7), both with SDA truth before and a
tree-dump ladder on the new master across and after the move.

Ran as registered in `PLAN.md`, with one deviation, stated: run 1's tree moved **without**
the step-6 trigger (the activity alone sufficed), so the ladder was not yet running when it
moved; run 2 was fired with the ladder armed, and the move still landed 29 s before the
ladder's first tick (the activity remaster fires as soon as the activity starts, not after a
settling delay). Both remasters are complete in the capture.

## The topology, and how it differs from RA5

| | RA5 | **RA6** |
|---|---|---|
| old master O | VAX1 | **VAX2** (CSID `00010003`) |
| new master N | VAX2 | **VAX3** (CSID `00010002`), the node booted on root SYS2 |
| holder H | VAX3 | **VAX1** (CSID `00010001`) |
| LOCKDIRWT | per `vms-50d` notes, 0/0/0 and 0/0/1 arms | **1/1/1** on all three |
| trees | one (RMT5) | **two** (RMT6, RMT7), independent fires |

CSIDs are read from SDA `SHOW CLUSTER` on VAX1 (`console-vax1.log`): VAX1 `00010001`,
VAX2 `00010003`, VAX3 `00010002` — note they are **not** in node-number order, which is
itself a variable RA5 did not vary.

## Files

- `rm6-remasters.pcap` — every 0x6007 frame in the two 250 ms windows around the two
  remasters (565 packets). Trimmed from the full 22 MB capture with `trimpcap.py`.
- `decode-RMT6.txt` / `decode-RMT7.txt` — `rmhandles.py` (RA5's, unchanged) over each tree.
- `bodies-RMT6.txt` / `bodies-RMT7.txt` — the **full 132-byte body** of every remaster frame,
  from `rm6dump.py`. Written because this run's finding is about bytes a summary hides.
- `console-vax{1,2,3}.log` — every console line: the logins, the SDA `SHOW RESOURCE` truth
  before each move, `SHOW CLUSTER`, `SHOW SYSTEM` / SDA `SHOW SUMMARY` process lists (the
  EPID tables), and the ladder.
- `ladder-vax3.times` — the host clock at each ladder tick, for correlating the console log
  with the capture's absolute timestamps.

## The two remasters

```
RMT6  2026-10-13 21:01:41  VAX2 -> VAX3     RMT7  2026-10-13 21:12:12  VAX2 -> VAX3
  O>N 0e,0f,10 ; O>H 0f,11 ; O>N 12 ; H>N 12 ; O>N 13 ; H>N 13 ; N>O 14 ; N>H 14
```
The sequence matches RA2/RA4/RA5 exactly. The op-0x12 cells RA4 named hold again, in this
third topology, against this run's own SDA:

| cell | RMT6 | RMT7 | named by |
|---|---|---|---|
| `12 O>N [20:24]` | `20600217` | `20600218` | VAX2's **HOLD2 / HOLD2B EPID** (`SHOW SYSTEM` on VAX2) |
| `12 O>N [24:28]` | `13000192` | `0A000363` | the **same holder lock SDA listed** on VAX2 before the move (`13000192 NL`) |
| `12 H>N [20:24]` | `2020021C` | `2020021E` | a VAX1 EPID (VAX1's EPIDs are `2020xxxx`) |
| `12 H>N [24:28]` | `09000971` | `15000989` | **VAX1's own root lock**, SDA-listed before the move (`09000971 CR`) |

## The two op-0x14s are NOT the same frame

```
RMT6  N>O [20:24] 01000364  [24:28] 090003AC  [28:40] 10 00 00 00 17 00 fa 00 00 00 00 00
RMT6  N>H [20:24] 20400217  [24:28] 060003AB  [28:40] 80 2e 98 87 00 00 f9 20 1d 9f 87 00
RMT7  N>O [20:24] 0100036E  [24:28] 060003C7  [28:40] 10 00 00 00 22 00 fa 00 00 00 00 00
RMT7  N>H [20:24] 20400222  [24:28] 030003C8  [28:40] 80 0e a0 87 00 00 f9 a0 04 a1 87 00
```

**ONE CELL IS NOW NAMED.** `0x14 (N -> H) [20:24]` is an **EPID of a process on the NEW
MASTER**, matched exactly against that node's own process table in both runs:

- RMT6: `20400217` = **ACT2** on VAX3 (SDA `SHOW SUMMARY`, `console-vax3.log`)
- RMT7: `20400222` = **BCT4** on VAX3 (same)

It is not an EPID on the sender in general — the frame to the OLD master carries
`01000364` / `0100036E` in the same cell, and VAX3's EPIDs are all `2040xxxx`.

## WHY THE REST OF op-0x14 CANNOT BE READ AS FIELDS

`bodies-RMT7.txt`, the `N>H` 0x14, verbatim:

```
[ 48] 52 4d 54 37 24 61 53 59 53 44 53 4b 31 20 20 20    RMT7$aSYSDSK1
[ 80] 00 00 00 00 20 6e 00 00 65 20 56 41 58 32 20 20    .... n..e VAX2
[ 96] 20 61 74 20 31 33 2d 4f 43 54 2d 32 30 32 36 01     at 13-OCT-2026.
```

The name span after `RMT7` holds **another resource's name** (`$aSYSDSK1`, the file-system
tree) and then a fragment of an **OPCOM message** (`…e VAX2   at 13-OCT-2026`). The 0x14's
body beyond the handful of cells it sets is the **message buffer as the sender found it** —
the same stale-buffer behaviour already measured for a real VAX's op-0x07
(`docs/design-dlm-name-hash.md` §4a).

That reframes `[28:40]`. In the `N>H` frames it reads as two VAX **S0 kernel addresses** —
`0x87982E80` / `0x00879F1D` (RMT6) and `0x87A00E80` / `0x0087A104` (RMT7), in the same
`87xxxxxx` range as this system's own RSBs (`879A6A00`, `87A26380`) and LKBs (`87974240`,
`8798CE40`) — while in the `N>O` frames the same span reads `10 00 00 00 NN 00 fa 00` and
twelve bytes of zero. **A span that is a pair of pointers to one recipient and a small
record to the other, inside a body whose tail is demonstrably residue, is not a span this
census can call a field.** Both answers (82/15) echo it back unchanged except for `[34]`.

## Scoring the registered predictions — 1 confirmed, 1 partial, 5 falsified

| # | prediction | verdict |
|---|---|---|
| P1 | `[24:28]` is a lock id on N, present in the nearest snapshot | **NOT CONFIRMED.** The *index* is a tree lock's slot on N (`3AB`/`3C8` both appear in RMT6/RMT7's granted queue on VAX3 shortly after), but at a different sequence number every time: `060003AB` on the wire vs `080003AB` in SDA; `030003C8` vs `050003C8`. The slot is reused within seconds. |
| P2 | ...and that lock belongs to the remastered tree | **PARTIAL** — true at the index level in both runs, unverified at the 32-bit value. |
| P3 | `[20:24]` is an EPID of a process on N | **CONFIRMED for the frame to the HOLDER** (2/2, named against the process table), **FALSIFIED for the frame to the OLD MASTER**. |
| P4 | the two 0x14s carry the SAME low-16 index at different sequences | **FALSIFIED.** The indices are ADJACENT, never equal: `3AC`/`3AB`, `3C7`/`3C8`. |
| P5 | `[34] == 0xfa` in every 0x14 | **FALSIFIED.** To the old master `fa`; to the holder **`f9`**, answered `fa`. |
| P6 | `[28] == 0x10` in every 0x14 | **FALSIFIED.** To the old master `0x10`; to the holder **`0x80`**. |
| P7 | the 82/15 answer is a verbatim echo | **PARTIAL.** Byte-identical to the old master; to the holder, identical except `[34]` `f9 -> fa`. |

RA5's own tentative reading is also **falsified** by this arm: it recorded `0e/0f/10 [20:24]`
as "the old master's EPID" (RA5 saw `20200213` on VAX1). Here RMT7's `0e` carries
`20600218` — VAX2's HOLD2B EPID, so an EPID — but **RMT6's `0e` carries `3C0001E1`**, which
is no EPID on VAX2 (its EPIDs are all `2060xxxx`). One cell, two shapes, same node, eleven
minutes apart. That is the `heldout-proof-must-vary-topology` lesson landing a second time.

## WHAT THIS MEANS FOR OVMX — the gate stays shut, and now for a stated reason

**OVMX still emits no op-0x14.** rd vms-14fd's engine already adopts a handed-over tree for
real and counts every adoption it could not report
(`vms_lock_dlm_adopt_unreported()`); nothing here changes that, and this arm says the
withholding is not timidity:

1. Five of seven shape-based predictions were wrong. Any 0x14 OVMX had minted from the
   RA4/RA5 pattern would have carried `0xfa` where a real VAX puts `0xf9`, `0x10` where it
   puts `0x80`, and the same index in both frames where a real VAX uses two.
2. The cells that remain are, on this evidence, **not fields at all** but the sender's own
   buffer residue — including its kernel virtual addresses.

That raises a question this lane will not answer by itself and is escalating:

> **What does a faithful OVMX put in a span a real VAX fills with its own stale buffer?**
> Zeros are a value no real sender produces. Its own kernel addresses are real executive
> reads but mean nothing to a peer and are the exact class of field that has bugchecked a
> real VAX twice. Copying another frame's residue is the frame-to-frame plumbing INV-6
> forbids. An honest omission of a whole span inside a fixed-length body is not obviously
> available either.

Until that is ruled on, the honest position is the one shipping today: **OVMX adopts the
tree, serves the holders, and sends no 0x14** — which leaves a real departing VAX waiting
for a frame, and is therefore a gap that must be closed by a ruling, not by a guess.

## Next arm, if one is wanted

The one thing RA6 could not do is read the lock in `[24:28]` before its slot was reused: the
activity processes that *cause* an activity remaster are also what recycle the tree's lock
slots within seconds. A future arm would have to either (a) stop the activity the instant
the tree moves (a holder program that watches for the master change and exits), or (b) drive
the remaster by a path that needs no activity at all — but RA1/RA1b already measured that
the SHUTDOWN path remasters only file-system trees.
