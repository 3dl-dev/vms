# ci.6 rolling evacuation, first lab capture (2026-10-09, run ci6-evac-13)

Lab bed: pod `vaxlab-3` (ovmx-lab). Real OpenVMS VAX V7.3 nodes VAX1 (1025) and
VAX2 (1026) under SIMH, plus booted OVMXE (1030) under QEMU/TCG on the same
bridge. Cluster group 1. VOTES 1/1/1, EXPECTED_VOTES 3. The VAXes run LOCKDIRWT 0
and OVMXE runs LOCKDIRWT 1: the interim configuration, in which OVMXE is the only
lock-directory node.

OVMX build `07b07b8d` (lab-only integration branch `evac/int-8`): origin/main
plus #1578 at `fcf4f9a1` plus #1582, plus a lab-only Dockerfile step that stages
EVACWL.EXE.

Workload: EVACWL (#1571). On the VAX it is the MACRO-32 image
(`EVACWL-as-run.MAR`), assembled and linked on VAX1. On OVMX it is the C image
built by TCC + LINK.EXE. Both use one contract: an EX lock on `EVAC$WORKLOAD`,
and fixed 64-byte records appended to `EVAC$DATA:EVAC.DAT` on the shared volume
`VAX1DATA` (`$2$DUA1:` on the VAXes, `VDA100:` on OVMX).

## What the capture shows (see timeline.txt; frames in wire.pcap.gz)

| step | result |
|---|---|
| OVMXE joins; the vector line on OVMX reads `1 entries over 3 systems, 1 of them this node's` | yes |
| 1. The OVMX standby takes NL first, so OVMXE is directory and master | yes |
| 2. VAX1's `$ENQW` EX is granted by OVMXE: one op-0x01, one cat-0x82 grant | yes; VAX1 writes records 1..160 |
| SDA `SHOW RESOURCE` on VAX1 and on VAX2 | master CSID 00010007 = OVMXE on both: one master cluster-wide |
| 3. OVMX's CONVERT NL->EX queues behind VAX1's EX | yes, and no CPU stall (the ev6/ev11 kernel spin is fixed) |
| 3b. VAX2's standby (NL, then CONVERT EX) queues at the master | yes; it never got EX while another node held it |
| 4. Evacuate: STOP the workload on VAX1, so its EX goes back to the master | **OVMX standby GRANTED EX** ("EVACWL: holding EVAC$WORKLOAD") |
| 5. Volume handoff: `DISMOUNT/CLUSTER $2$DUA1:` on VAX1 | **FAILED**: VAX2 has files open (see below) |
| 6. VAX1 `SHUTDOWN` with REMOVE_NODE | **did not run**: VAX1's DCL hung after the DISMOUNT |
| Bugchecks | none on either VAX; quorum held throughout (Q=2 V=3) |

## The blocker, measured

OVMXE logged `%DLM, refusing a lock message from a system that has not proved
it runs this implementation: this implementation has no grounded answer for that
message shape`.

A VAX process whose request reaches an OVMX master as a message shape that OVMX
does not answer waits in RWSCS indefinitely. That happened to:

- VAX2's standby, after its CONVERT went to the master. The process cannot be
  deleted, and the image it runs (`[EVAC]EVACWL.EXE` on `$2$DUA1:`) holds the
  volume open.
- VAX1's DISMOUNT/SHUTDOWN.

Killing OVMXE released both VAXes through their own lock rebuild, with no
bugcheck.

Earlier runs on 2026-10-09 found, and #1578/#1582 fixed:

- LOCKDIRWT could not be set at SYSBOOT (#1582).
- A VAX op-0x01 / OVMX cat-0x82 request storm: the grant handles were swapped (afc70176).
- OVMX's lock vector left the node itself out at CSV slot 8 or above (1f0469a9).
- An RCU stall in `$ENQW`'s signal path (82a0bb46).
- An RCU stall in the deadlock search through the delivery process (fcf4f9a1).

## Run 14 (14:22Z), after the queued-CONVERT answer (#1578 at 0a933743)

Same bed. VAX2's standby EVACWL ran from VAX2's system disk.

- VAX2's standby waited in **LEF**, a normal queued `$ENQW`, instead of RWSCS.
  The OVMX master answered its CONVERT at once with 0xfb, as a real VAX master
  does.
- STOP on VAX1 → OVMX standby **granted EX** (the takeover), again.
- `DISMOUNT/CLUSTER $2$DUA1:` on VAX1 **never returned**. VAX2's local DISMOUNT
  succeeded. VAX1 returned to DCL the moment OVMXE left the cluster: the
  cluster-wide dismount waits on something every member must do, and OVMX does
  not do it.
- OVMX DLM counters (CNXTRACE): requests received=4, grants=2, declined=3,
  deferred grants owed=0. No bugcheck on either VAX; quorum held.
