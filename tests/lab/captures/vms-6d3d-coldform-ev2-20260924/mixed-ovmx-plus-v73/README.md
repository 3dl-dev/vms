# vms-6d3d, arm 3 — one OVMX node + one real V7.3, both `EXPECTED_VOTES=2`

**2026-09-24, the same disposable pod as the parent capture.** The real VAX
(`VAX1`, SCSSYSTEMID 1025, `VOTES=1 EXPECTED_VOTES=2`) was cold-booted **alone**
and left waiting; a booted OVMX/x86_64 node (`OVMX6D`, SCSSYSTEMID 1993,
`VOTES=1 EXPECTED_VOTES=2`, cluster group 1) was then started under QEMU/TCG
inside the same pod, tap-bridged to the same `br0`. Boot artifacts:
`build-boot-artifacts` on `work/vms-6d3d`, `SOURCE_COMMIT`
`4b5836833ff14d43552cb371c61417ab4a11f194` — i.e. the executive **with** this
item's fix. `CAP_NET_RAW` was dropped from the OVMX node's process subtree
(`verdict: (e) PASS -- CAP_NET_RAW denied`), so every frame it put on the wire
was the executive's.

## What happened — the vote arithmetic agrees across the two implementations

`VAX1.console.log`, the real system reporting on itself:

```
%CNXMAN,  discovered system OVMX6D
%CNXMAN,  established connection to system OVMX6D
%CNXMAN,  received VAXcluster membership request from system OVMX6D
%CNXMAN,  proposing formation of a VAXcluster
```

This is the parent capture's §2 rule firing **with an OVMX node as the other
half of the quorum**: a real V7.3 system whose own single vote cannot meet
quorum 2 proposed a formation as soon as an OVMX node advertised the second
vote over an open circuit. Its own `SHOW CLUSTER` view on the OVMX side agrees
— `OVMX6D` LOCAL, system `1025` OPEN.

OVMX, correctly, did **not** try to form: 1993 > 1025, so the election
(`vms_cnxman_coord_fsm.h` §8b (3), lowest SCSSYSTEMID first) stands it down and
it runs its join drive instead. Its trace shows it emitting the membership
request and then *responding* to the formation's records:

```
seq=11 EMIT     state=ADMIT->ADMIT  cat=0x01 op=0x02      <- membership request
seq=15 EMIT     state=ADMIT->ADMIT  cat=0x81 op=0x03      <- responds to the commit
seq=17 EMIT     state=ADMIT->ADMIT  cat=0x81 op=0x05      <- responds to a membership record
seq=19 EMIT     state=ADMIT->ADMIT  cat=0x81 op=0x05      <- ... and the second one
seq=21 ARRIVAL  state=ADMIT->ADMIT  detail=not-mine
seq=22 DISPATCH state=ADMIT->ADMIT  ev=TIMER_JOIN rep=241
```

## CN=2 was NOT reached, and this is exactly the gap §4 of the parent names

Two membership records (two members: VAX1 and OVMX6D) reached OVMX and were
answered — but OVMX stayed in `ADMIT`, **never adopted a CSID**, and re-armed
its join timer 241 times. `SHOW CLUSTER` on it listed `OVMX6D` with a blank CSID
and `1025` merely `OPEN` for the whole run. When the harness's own window
expired and the QEMU process was torn down, the real VAX logged the departure
and abandoned the transition it had opened:

```
%CNXMAN,  lost connection to system OVMX6D
%CNXMAN,  timed-out lost connection to system OVMX6D
%CNXMAN,  aborting VAXcluster state transition
```

(The `lost connection` is the harness killing the guest at the end of its
window — not a crash and not a peer-initiated departure.)

**This is not the quorum rule.** OVMX never reached its own founding gate in
this run: it deferred on SCSSYSTEMID and spent the whole run inside the join
FSM. What it cannot do is take its place in a **formation-class** transition
somebody else coordinated — it adopts an assigned CSID out of an *admission*
(`ADD`) but not out of a *formation*, which is the multi-participant founding
transition §4 of the parent capture records as a real remaining difference.
Tracked as **rd vms-f29**; nothing here is worked around.

**Zero bugchecks on either side.** The real VAX took OVMX's frames, opened a
transition, timed the peer out and abandoned it cleanly — the never-crash-a-peer
property held through an exchange neither side completed.
