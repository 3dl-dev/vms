# rd vms-8c54 — the stalled guest

A real visitor's browser, on the live public demo at V0.7-2, crashed the real
OpenVMS VAX V7.3 node with `CNXMGRERR` just after an OVMX member was admitted.
The fault was not on the wire: the visitor's machine was starving the
emulators. This directory is the lab evidence for what that fault really is,
what a real V7.3 node does under it, and what OVMX did instead.

**No pcap is committed** (`docs/clean-room/PROVENANCE.md`). Every file here is
a console log, a script, or a decode of frames printed through offsets already
grounded in `docs/cluster-protocol-spec.md`.

## The distinction the whole item turns on

| | what happens to the frames | what happens to the node |
|---|---|---|
| `tc netem loss 100%` / `SIM_CUT` | **destroyed** | keeps running; its own beat observes the listen deadline pass |
| `SIGSTOP` (a starved guest) | **deferred** — undamaged, in its receive queue | stops; **no timer of that node fires**, and on wake its clock and its queue jump together |

Nine months of blackout rigs could not produce the field failure because a
blackout cannot produce the ordering a stall does: on wake the queued frames
are consumed **before any beat runs**, so every receive path that refreshes the
§4(M) listen deadline gets to refresh one that had already expired.

## `oracle/` — what a real V7.3 member does, stalled the same way

`stall2.sh` SIGSTOPs one of two real OpenVMS VAX V7.3 nodes (a member of a
formed cluster) for 10 s — inside the survivor's 20 s `RECNXINTERVAL`, past the
~8 s port listen timeout — and SIGCONTs it. Nothing on the wire is touched.

* `oracle-stall2.out` — the injection, with epoch timestamps.
* `vax1-survivor.console.log` — the survivor: `lost connection to system VAX2`
  + `%PEA0, Port has Closed Virtual Circuit` at +7.4 s, then
  **`re-established connection to system VAX2`** at +13.9 s. **No removal, no
  state transition, no bugcheck.**
* `vax2-stalled.console.log` — the stalled node: **`lost connection to system
  VAX1` 1.4 s after it woke — it NOTICED** — then `re-established connection`
  0.3 s later.
* `analysis/oracle-vc-reformation.txt`, `analysis/oracle-vc-fields.txt` — the
  re-formation on the wire: **both** sides send their own 0x41 START (each
  carrying its running `send_seq`), both STACK, both ACK. Six frames, one
  millisecond.

## `rig/` — what OVMX did (arm N-1, the pre-fix build)

Three nodes on an isolated bridge: a real V7.3 founder (`VAXC`) and two OVMX
guests under KVM. `stall.sh` SIGSTOPs `OVMXB`'s QEMU process tree 2 s after
**its own** console says `this node is now a VAXcluster member`, for 10 s.

* `fault.out` — the injection.
* `VAXC.console.log` — `lost connection to system OVMXB` +
  `%PEA0, Port has Closed Virtual Circuit` at +8.2 s, then **`timed-out lost
  connection`** and `removed from VAXcluster system OVMXB` 20 s later.
* `OVMXB.console.log` — on wake it declares no loss on that channel at all,
  and 20 s later proposes the removal of the **real VAX** from its own view.
* `analysis/rig-N1-wake-burst.txt` — on wake OVMXB puts `0x5b`/`0x4b`/`0x48`
  sequenced traffic on a circuit the VAX closed 8.2 s earlier, then retransmits
  it as `0x7b` twice.
* `analysis/rig-N1-vc-census.txt` — the VAX re-verifies the channel and sends a
  fresh START; **OVMXB answers STACK and never sends a START of its own**. The
  VAX never acknowledges it: it re-STARTs every 5 s while OVMXB re-sends that
  STACK eight times a beat for the rest of the run.
* `analysis/rig-N1-accepted-stack.txt` — the STACK the VAX ACCEPTED at initial
  formation and the STACK it IGNORED after the stall are **byte-identical
  except `abs 112` (the frame-composition time)**. The STACK was never the
  problem; the missing START was.

## `repro/` — arm N-6: the CNXMGRERR itself, reproduced

A **14 s** stall of the just-admitted member bugchecked the real VAX. Its
console is the shape of Baron's live paste, line for line:

```
%CNXMAN,  completing VAXcluster state transition      <- OVMXB admitted
%CNXMAN,  lost connection to system OVMXB
%PEA0, Port has Closed Virtual Circuit - REMOTE NODE OVMXB
  
**** Fatal BUG CHECK, version = V7.3     CNXMGRERR
```

— with **no `timed-out lost connection`** before it: the VAX died inside its own
reconnect window, exactly as the visitor saw.

`crash-window.txt` and `wake-to-crash.txt` are the last second on the wire. The
VAX last-gasped **0.88 s after the guest woke**:

| t after wake | |
|---|---|
| +0.021 s | OVMXB puts a `0x4b` 204-byte sequenced CM message and credit returns on the circuit **the VAX closed 8.3 s earlier** |
| +0.31 s → +0.57 s | the VAX re-verifies the channel (`b2`/`b3`/`b4`) and the pair re-forms the circuit (`0x41` START/STACK/ACK) |
| +0.805 s | the VAX dials `VMS$VAXcluster` `CONNECT_REQ` |
| +0.876 s | OVMXB `CONNECT_RSP`, then `ACCEPT_REQ` — it accepts |
| +0.877 s | the VAX `ACCEPT_RSP`s **and last-gasps in the same millisecond** |

### The cell, DERIVED — `analysis/oracle-cm-counters-vs-conndata.txt`

`content[106:108]` is **this node's own connection-manager ack counter for the
peer it is dialling**: the highest `VMS$VAXcluster` CM send-msg# it has TAKEN
from that system, the same number it stamps at `abs 74` of the CM messages it
sends on the connection. Read straight off the oracle:

```
before the loss   VAX2 -> VAX1   CM send=14811 ack=10248
                  VAX1 -> VAX2   CM send=10249 ack=14811
the reconnect     VAX1 -> VAX2   CONNECT_REQ  cd[12:14] = db 39  (14811)
                  VAX2 -> VAX1   ACCEPT_REQ   cd[12:14] = 09 28  (10249)
after it          VAX1 -> VAX2   CM send=10250 ack=14811
                  VAX2 -> VAX1   CM send=14812 ack=10249
```

The conversation did not restart. It **moved** onto the new Con.ID pair, and
each side advertised where its receive stream stood so the other could resume
without a hole.

**The off-by-one is the proof** (`analysis/oracle-vax3-offbyone.txt`): VAX3 had
retransmitted `send=1799` three times unanswered, and VAX1's connect to it
carries **1798**. The cell is "the highest I have TAKEN", not "the last I have
SEEN on the wire" — a distinction only a real receive counter can make.

And `[2]`/`[11]` follow **that cell**, not membership: `0x01`/`0x08` when none
is carried, `0x02`/`0x0a` when one is (`0x08` → `0x0a` is its two bytes).
Eleven samples across two independent clusters, no counterexample — including
the four real-VAX rows already in `test_codec_cm.c`, which are *members*
carrying `0x01`/`0x08` because they had taken nothing from the peer they were
dialling, and one real VAX observed sending both forms minutes apart with its
membership unchanged.

### The earlier reading, recorded because it was WRONG

`conndata-form-vs-oracle.txt`. The VAX opened that reconnect with the **member
form** of the 16-byte SCA connect data (`content[94:110]`): byte 2 `0x02`,
byte 11 `0x0a`, bytes 12–13 a non-zero value. Two real V7.3 members
reconnecting after the same SIGSTOP both carry `02 … 0a`. OVMX — itself a
member at that instant, and carrying the member's real vote/quorum/node counts
— answered with the **joiner form**: byte 2 `0x01`, byte 11 `0x08`, bytes 12–13
`00 00`. OVMX has no member form at all; those two bytes come from the baked
E31 head/tail, not from executive state.

That first reading — "member form vs joiner form" — is **wrong**, and the
experiment that disproved it is worth keeping: a build that sent `0x02`/`0x0a`
whenever this node was a member (branch `exp/vms-8c54-member-conndata`,
artifacts `3ca53b57`, matrix `X`) put the right two bytes on the wire and the
VAX **still bugchecked**, 2 of 3 injected arms. The bytes were never the
point; the **ack cell they announce** was.

## The matrices

`analysis/loop-M.log` — the stall armed on the **real VAX's** commit of the
joiner's addition. Every arm took the join's "lost the connection before this
node was admitted" path, so the MEMBER reconnect ladder was never entered:
6 arms, 0 bugchecks, 3 convergence failures.

`analysis/loop-N.log` — the **live-shaped** stall, armed on the OVMX node's own
membership line (`startL.sh`). The signature is sharp:

| stall | verdict | why |
|---|---|---|
| 6 s | PASS | under the port listen timeout — the circuit never closes |
| 10 s, 13 s, 16 s | **FAIL, 2/3 MEMBER rows** | past the listen timeout, inside `RECNXINTERVAL` — the circuit closes on the peer and is never re-formed |

A note on the harness itself: the first run of the live-shaped matrix graded
**six green arms whose injector had never fired** (`grep -c` prints the count
*and* exits 1 on zero, so `|| echo 0` produced two lines and every integer test
died). `stall.sh` now counts through one helper, and `grade3.sh` grades an arm
with no injected fault `NOFAULT`, never `PASS`.
