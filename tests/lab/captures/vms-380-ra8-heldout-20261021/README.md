# vms-380 RA8 — the fresh held-out fire, scored by the committed codec

Pod `labrm8` in `ovmx-lab` (own pod, `requests == limits` 1500m/2Gi, image
`ovmx-vaxlab:7`), 2026-10-21 14:39–14:58 UTC, **deleted after the run**. Three real
OpenVMS VAX V7.3 nodes, no OVMX node: **OVMX emitted nothing at a real VAX in this
fire**, which is why it is an oracle read and not a conformance fire.

Ran as registered in [`PLAN.md`](PLAN.md), committed in `1bc24712b` **before the pod
existed**. Codec under test: `work/vms-380-remaster-codec` at `3aa543f98`.

## The topology, rotated to the one neither prior arm took

| | RA5 | RA6 | **RA8** |
|---|---|---|---|
| old master O | VAX1 | VAX2 | **VAX3** (`1ca8`, CSID `00010003`) |
| new master N | VAX2 | VAX3 | **VAX1** (`0104`, CSID `00010001`) |
| holder H | VAX3 | VAX1 | **VAX2** (`3f00`, CSID `00010002`) |
| LOCKDIRWT | 0/0/x | 1/1/1 | 1/1/1 |
| tree | RMT5 | RMT6, RMT7 | **RMT8**, plus two `CACHE$cmSYSDSK1` remasters |

VAX1 is also RMT8's **directory node** (`SHOW RESOURCE/NAME=RMT8` on VAX1 before the
move: `Status: DIRENTR`, `CSID 00010003`). The arm: VAX3 took the tree first (so VAX3
mastered it), VAX2 held the root CR plus S0/S1/S2 at CW, then four activity processes
on VAX1 and `STOP HOLD3` on VAX3 moved it — the RA2/RA5 trigger.

## SDA ground truth, before and after (console logs, verbatim)

VAX2 (H), **before** — `SHOW PROCESS/INDEX=17/LOCKS`, process `HOLD2`, EPID `20400217`:

| resource | VAX2's own lock id | mode | master copy on VAX3 |
|---|---|---|---|
| `RMT8` (root, VALBLK+SYSTEM) | `150000AB` | CR | `040004BF` |
| `S0` | `0E00039E` | CW | `010004C2` |
| `S1` | `07000210` | CW | `010004C3` |
| `S2` | `1A0003A7` | CW | `010004C4` |

VAX2 (H), **after** — same locks, new master copies **on VAX1**: root `01000923`,
`S2` `01000924`, `S1` `01000925`, `S0` `01000926`. VAX1 after the move:
`CSID 00000000` (it masters the tree), `Lock Count 5`, value block
`RMLVB-0123456789` preserved at `Seqnum 00000001`.

## Scoring — 8 predictions: 7 PASS, 1 PASS on the letter and NOT CONFIRMED on its
## stronger reading in one of the three remasters

### P1 — the sequence. **PASS.**
```
O(1ca8)->N(0104)  0e, 0f, 10          each answered 82/15
O(1ca8)->H(3f00)  0f (answered), 11   11 is a notification
O(1ca8)->N        13                  notification
H(3f00)->N        12 root, 12 S2, 12 S1, 12 S0   each answered 82/01
H(3f00)->N        13                  notification
N(0104)->O        14                  answered 82/15
N(0104)->H        14                  answered 82/15
```
**One honest deviation from the registered order:** the OLD MASTER's `0x13` arrives
*before* the holder's `0x12` records here (`124.7459` vs `124.7461`), where RA5 had
both `0x13`s after the records. P1 predicted "a `0x13` from each sender to N" without
pinning the interleave, so it is scored PASS — but the interleave is not a rule, and
nothing in the codec depends on one.

### P2 — who is answered. **PASS.** Every `0x0e`, `0x0f`, `0x10` and `0x14` carries a
non-zero transaction and is answered `82/15`. Both `0x13`s and the `0x11` carry
`body[4:8] == 0` and nothing answers them.

### P3 — RULE 1, byte-exact by the committed builder. **PASS, 16/16.**
### P4 — RULE 2, byte-exact by the committed builder. **PASS, 4/4.**

`tools/cluster/remaster_census.py --pairs | tests/cluster/host/score_remaster`
([`score.txt`](score.txt)) — the SHIPPING `vms_dlm_rm_ack_build` and
`vms_dlm_rm_lock_answer_build`, not a re-implementation:

```
score_remaster: 20 pair(s): 20 byte-exact, 0 mismatched, 0 refused
  the two cells rd vms-380 does not name (op-0x12 answer body[30:34]): 6 agreed, 10 differed
  real SUB answers' body[36:56]: 3 all-zero (what this codec writes), 0 carrying residue
```

### P5 — the root/sub split. **PASS.** The ROOT answer carries `body[30:32] = f1 01`
(in {`f0 01`, `f1 01`} as predicted), the value block `RMLVB-0123456789`, and sequence
1. All three SUB answers carry `body[30:32] = 00 00` and `body[36:56]` all zero.

### P6 — the unnamed cell `body[32:34]`. **PASS, both halves.**
All four `82/01` answers carry `04d3`, **constant across the remaster**; the N→O
`0x14`'s `body[20:24]` is `0100 04d2`, low half `04d2` — **adjacent**, well inside the
predicted ±16. Two of the three `0x14`s in this capture also carry a value from that
same index space at `body[32]` (`0x1c`, `0x1f`). That is as strong as this cell gets:
a node-local allocator index, and the reason this codec writes the grounded `0x0001`
there and records the difference instead of minting a number it cannot source.

### P7 — RULE 3, the residue. **PASS on the letter, and the stronger reading is
CONFIRMED — in the middle remaster, not in RMT8's.**

Every `0x14` carries `body[8:10] = 02 14` and `body[40:44] = 0` (5/5). And the capture
contains **three** remasters, which is what makes it decisive:

| window | old master | `0x14` recipient | `body[28]` | `body[34]` | RA6's role rule wants |
|---|---|---|---|---|---|
| t=0.41 | VAX3 | to OLD MASTER | `10` | `fa` | `10`/`fa` — holds |
| **t=32.51** | **VAX2** | **to OLD MASTER** | **`d0`** | **`f9`** | `10`/`fa` — **broken** |
| **t=32.51** | **VAX2** | **to OTHER** | **`00`** | **`fa`** | `80`/`f9` — **broken** |
| t=115.12 (RMT8) | VAX3 | to OLD MASTER | `10` | `fa` | `10`/`fa` — holds |
| t=115.12 (RMT8) | VAX3 | to OTHER | `80` | `f9` | `80`/`f9` — holds |

The middle remaster's two `0x14`s are **exactly inverted** from RA6's rule, and
`body[28] = 0xd0` is a fourth value for that cell. So RMT8's own `0x14`s *do* follow
RA6's pattern — and one tree over, in the same capture and the same seconds, another
pair does not. **A cell that takes `10`, `80`, `00` and `d0` in five frames, with the
role reversed between two trees, is not a field a receiver can depend on**, which is
what the rd vms-42de ruling's residue test asks. Classified by
`remaster_census.py`: one stale-GRANT-record shape, two stale-DIRECTORY-answer shapes
(one carrying the VAX S0 kernel address `0x008794d0` at `body[36:40]`), two neither.

### P8 — the op-0x12 record cells against SDA. **PASS, every cell.**

```
02/12 3f00>0104 [20:24] 20400217  [24:28] 150000AB  [28] 11  [30:32] 01 01  parent 0 0  'RMT8'
02/12 3f00>0104 [20:24] 20400217  [24:28] 1A0003A7  [28] 00  [30:32] 02 02  [36:40] 150000AB [40:44] 01000923  'S2'
02/12 3f00>0104 [20:24] 20400217  [24:28] 07000210  [28] 00  [30:32] 02 02  [36:40] 150000AB [40:44] 01000923  'S1'
02/12 3f00>0104 [20:24] 20400217  [24:28] 0E00039E  [28] 00  [30:32] 02 02  [36:40] 150000AB [40:44] 01000923  'S0'
```
`body[20:24]` is `HOLD2`'s EPID on VAX2 (`SHOW SUMMARY`). `body[24:28]` is, in all four
records, **the exact lock id SDA listed on VAX2 before the move**. `body[30:32]` is the
granted/requested mode SDA showed (root CR/CR, subs CW/CW). `body[36:40]` is the
parent's holder lock id; `body[40:44]` is `01000923` — the handle the new master minted
and answered in the root record's `82/01` seconds earlier, and the master copy SDA on
VAX2 shows for the root **after** the move.

And the four `82/01` answers' `body[20:24]` are `01000923`, `01000924`, `01000925`,
`01000926` — **the four master copies SDA on VAX2 lists after the move, in order,
4 of 4**.

## Files

- `ra8-remaster.pcap` — every 0x6007 frame in the three ~1 s remaster windows (90
  packets, trimmed from a 16 MB capture).
- `score.txt` — the committed codec scored against this capture.
- `census.txt` — `remaster_census.py` over it.
- `console-vax{1,2,3}.log` — the logins, the SDA truth before and after, the holder
  spawns, `STOP HOLD3`, and the process tables the EPIDs come from.

## What this does and does not establish

It establishes RULE 1 and RULE 2 on a fire whose predictions were committed first, in a
topology neither derivation arm used: 20 of 20 real answers reproduced byte for byte by
the shipping builders, every op-0x12 cell SDA-named, and both unnamed cells behaving
exactly as the codec's refusal to guess them assumes.

It does **not** establish that a real VAX ignores the `0x14` residue span. Nothing in
this fire sent a `0x14`; that is rd vms-6ad1 (RA7), and it is still the only thing that
can let OVMX send one. What RA8 adds for RA7 is that the span's variation is now
measured in a *controlled* arm as well as in the corpus — including one pair of `0x14`s
whose supposedly role-determined cells are inverted.
