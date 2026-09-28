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
