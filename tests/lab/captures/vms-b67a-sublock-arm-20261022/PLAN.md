# rd vms-b67a: the SECOND arrangement for the sub-lock routing rule

**Registered before the arm runs.** rd vms-b67a's grounding (W1-W4, pinned by
`tools/cluster/dlm_sublock_census.py` + ctest `cluster_host_dlm_sublock_census`)
rests on **7 real sub-lock requests from ONE capture with ONE master (`0104`)**,
two senders and two modes. rd vms-e80 widened the corpus 3.5x (21,499 ->
74,894 real `op-0x01` requests) and **the numerator did not move**: no second
arrangement can come from more corpus, only from a driven arm. That is what
`heldout-proof-must-vary-topology` requires before the routing change in
`vms_enq_core_ex` is built, and this is that arm.

**REAL VAX TO REAL VAX ONLY.** OVMX sends nothing in this arm; it is a member of
the cluster (so the topology is 3-node, where the 2026-10-11 capture was 2-node)
but it originates no frame about `RMT9`. Nothing OVMX emits is under test here,
so `ovmx-never-crashes-a-peer`'s new-shape precondition does not apply.

## The arrangement, and why it differs

| | 2026-10-11 capture (`vms-380-remaster-arms`) | THIS ARM |
|---|---|---|
| master of the sub-tree's root | `0104` | the OTHER VAX |
| sub-lock senders | `2ac4`, `36ea` | one, a different node |
| sub-lock mode | CW (`02`), CR (`01`) | CW (`02`) |
| cluster size | 2 real VAXes | 2 real VAXes **+ an OVMX member** |
| the root's own routing | directory -> redirect -> master | measured again here |

## The steps

| # | node | step |
|---|---|---|
| 1 | VAX1 | `RMH 100WRMT9` -- takes the root FIRST and therefore MASTERS `RMT9` (root CR, no sub-locks, the known value block) |
| 2 | VAX2 | `RMH 123-RMT9` -- the root at CR **plus three sub-locks S0/S1/S2 at CW**, under a parent VAX2 does NOT master |
| 3 | both | `SDA> SHOW RESOURCE/NAME=RMT9`, and `SHOW RESOURCE/NAME=S0` on each, so the wire is read against each node's own lock database |

## Predictions, each scored on its own

**W1' -- THE SUB-LOCKS GO TO THE PARENT'S MASTER, AND NEVER TO A DIRECTORY.**
All three `cat-0x02 op-0x01` frames naming `S0`/`S1`/`S2` are addressed to VAX1,
the node that masters `RMT9`. **FAIL if any of them is addressed to a directory
node under its own derived hash**, or if any is answered `0xf9` (no directory
entry) -- that is the OVMX defect this item is about, measured at a real node.

**W2' -- THE PARENT PAIR AT `body[36:44]` IS (OWN, MASTER), AND IT IS THE PAIR
THE ROOT'S GRANT CARRIED.** `body[36:40]` equals VAX2's own root lock id and
`body[40:44]` equals VAX1's master lock id for that root -- byte-for-byte the
pair in the root's `82/01` grant (`body[24:28]` and `body[20:24]` of that
answer). **FAIL if either half is zero**: a zero at `body[40:44]` is what a ROOT
record means, and a master reading one would create a second root.

**W3' -- `body[30]` IS THE SUB-LOCK'S REQUESTED MODE**, `02` (CW) for `s=2`.

**W4' -- THE HASH AT `body[128:132]` IS A FUNCTION OF THE SUB NAME ALONE.** The
2026-10-11 capture measured `f7795ee8` (S0), `f779cc1b` (S1), `f7793a4f` (S2)
from two different senders. If this arm -- a different cluster instance, a
different master, a different sender -- carries the SAME three values, the cell
is name-determined and nothing else, which is a fact about a cell OVMX will
write ZERO in (it has not learned a hash for a name the cluster never addressed
a directory about, and Rule 8 forbids computing one). **If they differ, say so
and record both**: that is a measurement, not a failure, and it would mean the
deviation row needs a different note.

**W5' -- AND THE MASTER REALLY HOLDS THEM.** VAX1's SDA `SHOW
RESOURCE/NAME=S0` lists an LKB held FOR VAX2 (`Status MSTCPY`, naming VAX2's own
lkid and CSID) at CW, and `Parent RSB` is the `RMT9` RSB. This is the state
OVMX's own sub-lock route must produce, and the one the vms-c2b fire found
missing (VAX1's `Lock Count` stayed 1).

## Files this directory must carry afterwards

| file | what it is |
|---|---|
| `ARM.md` | the scored predictions, with the bytes |
| `sublock.pcap.gz` | the frames, payloads unaltered |
| `frames.txt` | the `RMT9`/`S0`/`S1`/`S2` frames decoded, in order |
| `sda-*.txt` | each node's own lock database at step 3 |
