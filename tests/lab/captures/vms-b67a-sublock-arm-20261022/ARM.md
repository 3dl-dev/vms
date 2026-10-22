# rd vms-b67a, the SECOND arrangement: SCORED

Ran 2026-10-22 01:03-01:05Z in pod `lab85f` (image `ovmx-vaxlab:7`, content-attributed
to `origin/main`'s `tests/lab` tree -- see
`../vms-85f-queued-answer-20261021/FIRE.md`). Scored against
[`PLAN.md`](PLAN.md), committed before the arm ran.

**Real VAX to real VAX only.** An OVMX node was a cluster member throughout and
originated nothing about `RMT9`; every frame below is one real V7.3 node talking
to another.

## What ran

| # | node | step | result |
|---|---|---|---|
| 1 | VAX1 | `RMH 100WRMT9` | `RMT9A` in `HIB`; VAX1 takes the root EX first (frame 42, answered `0xf9` by VAX2 -- no directory entry) and masters it |
| 2 | VAX2 | `RMH 123-RMT9` | `RMT9B` in `HIB`: the root at CR plus S0/S1/S2 at CW, under a parent VAX2 does **not** master |
| 3 | VAX1 | SDA | `SHOW RESOURCE/NAME=S0` and `SHOW LOCK` below |

## The frames (`frames.txt`, `sublock.pcap.gz`)

```
f42   02/01 VAX1 -> VAX2  RMT9  mode=05(EX)  own=5a000693          -> f43 82/01 body[34]=f9
f232  02/01 VAX2 -> VAX1  RMT9  mode=01(CR)  own=76000359  parent=00000000 00000000
f234  02/01 VAX2 -> VAX1  S0    mode=02(CW)  own=4d000360  parent=76000359 3c000695
f236  02/01 VAX2 -> VAX1  S1    mode=02(CW)  own=4c000361  parent=76000359 3c000695
f238  02/01 VAX2 -> VAX1  S2    mode=02(CW)  own=50000362  parent=76000359 3c000695
```

**W1' -- THE SUB-LOCKS GO TO THE PARENT'S MASTER. PASS.** All three are
addressed to **VAX1**, the node that masters `RMT9`. There is no directory
lookup for `S0`/`S1`/`S2` anywhere in the capture and no `0xf9` answer to one.
The only directory traffic is for the ROOT name (f42/f43). This is the second
master to show it: the 2026-10-11 capture's seven requests all went to `0104`.

**W2' -- THE PARENT PAIR IS (OWN, MASTER), AND IT IS THE ROOT GRANT'S OWN PAIR.
PASS, and now cross-checked against SDA on the master.** `body[36:44]` is
`76000359 3c000695` on all three:

* `body[36:40]` = `0x76000359` = **VAX2's own root lock id**, which is exactly
  what f232 (its root request) carries at `body[24:28]`;
* `body[40:44]` = `0x3c000695` = **VAX1's master lock id for that root** --
  independently printed by VAX1's SDA as the sub-lock's `Par. id: 3C000695`.

Neither half is zero. A second master, a third sender and a 3-node cluster
read the same rule the first arrangement did.

**W3' -- `body[30]` IS THE SUB-LOCK MODE. PASS.** `02` (CW) on all three, for
`s=2`.

**W4' -- THE HASH AT `body[128:132]` IS **NOT** A FUNCTION OF THE SUB NAME
ALONE. MEASURED, and it is the outcome PLAN.md named as the other branch.**

| name | 2026-10-11 (`ra2-rmt2-remaster.pcap`) | THIS ARM |
|---|---|---|
| S0 | `f7795ee8` | `e3bd7e45` |
| S1 | `f779cc1b` | `e3bdec78` |
| S2 | `f7793a4f` | `e3bd5aac` |

Recorded, not derived (Rule 8): within each capture the three sub names share
the FIRST TWO BYTES on the wire (`f779`, `e3bd` -- the low half of a
little-endian read) and differ after them; across the two arrangements every
value differs. The root's own cell in this capture is
`12543943`, shared by its request and its answer. **What this settles for
rd vms-b67a: OVMX writing ZERO there is the only honest option** -- the value is
not name-determined, so it cannot be learned from the name, and the cluster
never addressed a directory about a sub-resource for OVMX to have learned it
from the wire. The `dlm_enq_req` deviation row says so with both measurements
beside it.

**W5' -- AND THE MASTER REALLY HOLDS THEM. PASS.** VAX1's SDA
(`sda-vax1.txt`):

```
SDA> SHOW RESOURCE/NAME=S0
Address of RSB:  879DF780  GGMODE: CW  Status: VALID
Parent RSB:      879CD840  CGMODE: CW
Lock Count:      1          CSID:   00000000
Granted queue:   07000697  CW 00000000-FFFFFFFF

SDA> SHOW LOCK 07000697
Lock id:  07000697   Par. id:  3C000695
Granted at  CW
Resource: S0   Status: MSTCPY
Master copy of lock 4D000360 on system 00010002
```

`4D000360` is byte-for-byte the handle f234 carried at `body[24:28]`, and
`00010002` is VAX2. So the wire request, the master's LKB and the master's
parent handle all agree -- the exact state the rd vms-c2b fire found MISSING
when OVMX sent its own sub-locks to the directory instead (VAX1's `Lock Count`
stayed 1 and its SDA listed no lock for OVMX).

## What this unblocks, and what it does not

The routing rule rd vms-b67a's fix is built from (W1/W2) now holds in **two
arrangements with two different masters**, which is what
`heldout-proof-must-vary-topology` asked for before the `vms_enq_core_ex`
change. It does NOT settle the hash cell -- it settles that OVMX must write
zero there and record a deviation, which is a different and smaller claim than
the one the first capture suggested.
