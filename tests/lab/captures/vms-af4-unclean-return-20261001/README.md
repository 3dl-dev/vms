# rd vms-af4 — a member that leaves uncleanly and comes straight back

**Short version.** The browser observation from the V0.7-2 capture (PR #1324) does
not reproduce on V0.7-5. The defect behind it does: if a member leaves uncleanly
and comes back while an OVMX member still holds it, it is never readmitted. A
real OpenVMS VAX V7.3 cluster readmits that member about 6 s after its new
incarnation is first heard. On the rig, current `main` (`ff4a7160`) keeps it
out for the whole 300 s window.

## 1. The browser form: not reproduced (V0.7-5 live demo)

`browser/af4browser.sh` runs one visitor-gate run to CN=3 and kills the browser
with all three nodes members. That kill is the unclean teardown. It then starts
a fresh run.

| run | verdict | refusal / re-incarnation lines (`rejected`, `re-incarnated`, `refuses`) |
|---|---|---|
| r1 | CN=3 | 0 |
| r2 (after the kill) | CN=3 | 0 |

Every visitor-gate run uses a new browser context, so a run cannot inherit
anything from the one before it. PR #1338 graded 12 consecutive runs on V0.7-5
and none of them printed a refusal line either. The V0.7-2 text comes from the
#1320 REJECT → CLUEXIT path ("the cluster rejected this node's connection",
"already re-incarnated and the cluster still refuses it"). That path can only
fire within a single cluster lifetime, when a peer refuses this node's
`VMS$VAXcluster` connect.

## 2. The oracle: a real V7.3 member rebooted uncleanly

Two real OpenVMS VAX V7.3 nodes form one cluster on a private bridge. Consoles
are timestamped at 10 ms. The survivor's `RECNXINTERVAL` is raised to 300 so the
return falls inside its reconnect window. VAX2's emulator is SIGKILLed (no
shutdown, no last gasp) and booted again at once. Files: `oracle/`; times are
`1790874xxx` epoch seconds.

| t | event |
|---|---|
| ≈322.3 | VAX2 killed |
| 327.724 | VAX1: `lost connection to system VAX2`, `%PEA0, Port has Closed Virtual Circuit` |
| 345.969 | VAX2 (new boot): `%SYSINIT, waiting to form or join` |
| 347.981 | new VAX2's first multicast HELLO |
| 349.398–.400 | VAX1 sends b2. b3/b4 follow, then START/STACK/ACK: a new circuit forms with the new incarnation |
| 350.110 | VAX2 sends `VMS$VAXcluster` CONNECT_REQ |
| **350.112** | **VAX1 ACCEPTs it** as a fresh connection |
| **350.116** | **VAX1: `timed-out lost connection to system VAX2` → `proposing reconfiguration` → `removed from VAXcluster system VAX2`**. The old incarnation is removed 4 ms later, 277 s before its `RECNXINTERVAL` would have expired |
| 354.075 | VAX1: `received VAXcluster membership request from system VAX2` → `proposing addition` |
| 355.994 | VAX2: `now a VAXcluster member` |

So real VMS accepts the new incarnation's connection and removes the old
incarnation at once, without waiting out `RECNXINTERVAL`. It then admits the
new incarnation as a joiner.

## 3. The rig: OVMX never readmits it

The setup is `rig/af4arm.sh`. The real V7.3 VAXC founds the cluster. OVMXA and
OVMXB join, under QEMU TCG. OVMXB's QEMU is SIGKILLed and booted again at once
with the same SCSNODE and SCSSYSTEMID.

| build | OVMXA lost the old circuit | new circuit OVMXA↔OVMXB | VAXC removed the old OVMXB | returning OVMXB admitted? | OVMXA's final SHOW CLUSTER |
|---|---|---|---|---|---|
| `af4-m0-1`, main `ff4a7160` | +19.4 s (20 s listen timeout) | +21.5 s | +25.5 s (before its `RECNXINTERVAL` ran out, as in the oracle) | **no, 300 s** | `1988 00010003 MEMBER` |
| `af4-b1-1`, main + vms-b98 + vms-6b1 | +7.6 s | +22 s | +25.3 s | **no, 300 s** | `1988 00010003 MEMBER` |

All times are measured from the kill. The real VAX does what the oracle's VAX1
did. OVMXA does not, in three steps:

1. **The new incarnation's connect is re-accepted into the old member's CSB.**
   That CSB is inside its p. 7-30 window, so `CONNECT_RCVD` takes the
   `h_reaccept` edge and goes REACCEPT → OPEN, and the old incarnation is held as
   MEMBER. The pcap shows OVMXA's ACCEPT_REQ at rel 115.135; the REACCEPT edge
   itself is read from the code. Its effect is measured: a window that had
   expired would have cleared the MEMBER flag (`csb_give_up`), and 1988 still
   reads MEMBER 300 s later. `CNXMAN_CSB_EV_NEW_INCARNATION` has an `h_dead` cell in every row of
   the CSB table (`vms_cnxman_csb.c`), but nothing in the tree dispatches it.
2. **The real VAX's removal transition cannot be applied by OVMXA.** VAXC runs
   op 0x12 / 0x03 / 0x0f / 0x08 / 0x0a with OVMXA (`af4-b1-1/s8.pcap`, rel
   118.098). OVMXA answers every message and prints `completed VAXcluster state
   transition`. But the removal opens with op 0x08, which carries no nodemap
   OVMX can read (`barrier_take_bitmap`: "op 0x08 / cat-01 op 0x0d carry no
   nodemap at all"). Phase 2 therefore leaves membership alone, which is the
   correct INV-6 outcome, and 1988 stays MEMBER with the old CSID.
3. **The returning node waits forever.** OVMXB drives its join through OVMXA,
   prints `waiting for connectivity to every cluster member before asking for
   admission`, and never sends a membership request (CNXTRACE: ADMIT,
   TIMER_JOIN ×147). VAXC's connection to it is logged as `not the member this
   join is driving through`. *Inferred, not yet measured:* the member set it is
   waiting on still contains OVMXA's stale 1988 entry.

rd vms-833 (CLUEXIT-in-place beside an OVMX member: "the member still lists the
old incarnation MEMBER") is the same family.

## What matching the oracle needs (open — design question)

* (a) Dispatch NEW_INCARNATION when a circuit's incarnation differs from the
  CSB's. For a SELECTED member this ends the reconnect window immediately: give
  up, then propose or defer, as `h_last_gasp` does. A connect from a different
  incarnation is never REACCEPTed into the old block. This part is grounded by
  §2.
* (b) A participant has to drop a member removed by a transition it did not
  coordinate, even when the open carries no readable nodemap (op 0x08). There
  are two ways to do that, and choosing between them is an INV-6 question:
  ground op 0x08's 44-byte payload from captures, or apply the removal of a
  block this node has itself declared DEAD when a nodemap-less transition
  commits.

(a) alone does not readmit the node: OVMX cannot originate a removal toward a
real VAX (the coord-removal-open-gate refusal), so the stale SELECTED block
survives VAXC's transition either way.
