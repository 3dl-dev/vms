# vms-1e58 ci.6 lab fire on the vms-5fa shared-volume bed (2026-11-03): FAIL (C3, C4)

Verdict (`ci6_evac.sh grade ci6evac-031902`, `ci6evac-031902/grade.txt`, re-run from the capture alone and identical):
**C1 PASS, C2 PASS, C3 FAIL, C4 FAIL, C5 PASS.** No bugcheck on any node.

The workload ran on VAX1, moved to OVMXE at seq 137 with no gap, and the cluster stayed up when VAX1 left. Two criteria
fail. C4 is a real exclusion fault: OVMXE was the master of `EVAC$WORKLOAD` and held it EX. VAX2 asked for EX/NOQUEUE.
OVMXE answered with the QUEUED record instead of a NOTQUEUED deny, and VAX2's `$ENQW` came back `SS$_NORMAL`. C3 fails
because the clocks differ between nodes: the records on disk follow the lock, but OVMXE's clock runs behind VAX1's.

Per the fire rules no executive code was changed.

## Bed

- Own pod `lab1e58` (ovmx-lab, image `ovmx-vaxlab:7`, `ASYM_MSCP=1`, request 200m CPU / limit 4, memory 2Gi/3Gi).
  This is the vms-5fa r2-r6 bed. VAX1 (1025) alone owns the RA81 `$2$DUA2:`, and VAX2 (1026) and OVMXE (1030) reach it
  only through VAX1's MSCP server. Read with `F$GETSYI` before the fire: VAX1 VOTES 1, EXPECTED_VOTES 1, LOCKDIRWT 1;
  VAX2 VOTES 0, EXPECTED_VOTES 1, LOCKDIRWT 1. Cluster group 1.
- The artifacts are vms-5fa r6's: build-boot-artifacts run 41566284003, branch `lab/vms-5fa-r6`, SOURCE_COMMIT `5fa8de625`.
  They include **unmerged #2002 and #2003** (head `ff69ca156`). SHA256SUMS were verified.
- OVMXE was booted by `ovmx_fire.sh` from the `cluster-5fa-lab3` tree (`5fa8de625`, the build's own commit and grounding
  table). Tag `r1e58`, `OVMX_FIRE_EXPVOTES=1`, `OVMX_FIRE_GROUP=1`, `LOCKDIRWT=0`, every frame type in the grounding table
  named. The gate binaries were built from that tree. Gate PASS: `frame_conformance: every byte grounded`, all three
  negative controls refused, group 1 read out of the initramfs.
  **One change from r6: `OVMX_FIRE_VOTES=2`.** With it, VAX1 leaving still leaves the survivors quorate. OVMXE read back
  VOTES 2, EXPECTED_VOTES 1, LOCKDIRWT 0, `N=3 Q=2`. The fire's precheck: `Q=2 V=3 N=3, VAX1 VOTES 1: survivors keep 2 >= 2`.
- Driver: `CI6_SHARED=1 CI6_INIT=1 ci6_evac.sh fire lab1e58` from this PR's tree (origin/main `d1bc5b098` + the
  `CI6_SHARED` change). It ran end to end with nothing typed by hand. OVMX's unit was `VAX1$DUA16386:`.

### The `CI6_SHARED=1` fire

The default fire was written for the run16 bed, where the VAXes and OVMX (`VDA100:`) each had their own path to one disk.
On this bed VAX1 is the volume's only server, so the fire mounts the volume the way `share` does and reorders the end:

1. STEP 0: VAX1 `INITIALIZE`s and `MOUNT/SYSTEM`s `$2$DUA2: EVAC1E58`. OVMXE finds its MSCP unit in `SHOW DEVICE D` and
   `MOUNT/SYSTEM`s it. There is no STEP 4 handoff.
2. The standby is `EVACWL STANDBY NL=90 60`, so it writes 60 records and ends.
3. STEP 5a, after the probe: OVMXE dismounts. VAX2 `MOUNT/SYSTEM`s the volume over VAX1's server, `TYPE`s EVAC.DAT and
   dismounts.
4. Only then does VAX1 leave (STEP 6, SHUTDOWN with REMOVE_NODE).

So when VAX1 departs, OVMXE's workload has already finished. On this bed it cannot keep running across the departure,
because the volume leaves with VAX1.

## What the consoles show

| criterion | evidence |
|---|---|
| C1 | OVMXE `EVACWL: standby holds EVAC$WORKLOAD NL for 90 s` (`OVMXE.console.log` 26). VAX1 wrote seq 1..136 (`vax2.log` TYPE, 20-155) |
| C2 | VAX1 `$ STOP EVACVAX1` (`vax1.log` 60), `@SYS$SYSTEM:SHUTDOWN` (61), `Shutdown options [NONE]: REMOVE_NODE` (80). VAX2: `removed from VAXcluster system VAX1` (`vax2.log` 302), `Node VAX1 (csid 00010001) has been removed` (306) |
| C3 FAIL | `EVACWL: OVMXE took EVAC$WORKLOAD EX at seq 137` (`OVMXE.console.log` 28). EVAC.DAT: VAX1 seq 136 is stamped `03:25:07.81` and OVMXE seq 137 `03:25:05.36` (`vax2.log` 155, 156). So seq is +1 with no gap, but the timestamp goes backwards by 2.45 s |
| C4 FAIL | `EVPROBE:  3-NOV-2026 03:25:32.15 $ENQW EX NOQUEUE EVAC$WORKLOAD R0=00000001 %SYSTEM-S-NORMAL` (`vax2.log` 16). That is inside OVMXE's hold: its records run 137..196, 03:25:05.36..03:26:05.74 (`vax2.log` 156-215) |
| C5 | Bugcheck grep (fatal, bugcheck, CNXMGRERR, LOCKMGRERR, INVLOCKID, quorum lost) over the fire window and over the pod's whole console life (VAX1 548, VAX2 657, OVMXE 89 lines): 0/0/0. VAX2 after VAX1 left: `Q=2 V=2 N=2` (`vax2.log` 313). EVAC.DAT seq 1..196 contiguous |

## C4 on the wire: OVMXE, the master, answered QUEUED to an EX/NOQUEUE request

The capture is `/lab/run-fire/r1e58/wire.pcap` (ether 0x6007, host clock). It is not committed. `ci6evac-031902/ci6-frames.txt`
has the full hex of the frames named here, and `evac-workload-frames.txt` lists every frame that carries the name. The MACs
are VAX1 `aa0004000104`, VAX2 `08002b2cf427` and OVMXE `52540000e501`. Offsets are into the Ethernet payload.

| frame (host time) | | what it is |
|---|---|---|
| 5168 / 5169 (03:22:46.79) | OVMXE -> VAX1 -> OVMXE | the standby's NL. OVMXE asks VAX1, the directory, and becomes the master |
| 5212 / 5214 (03:22:51.53) | VAX1 -> OVMXE -> VAX1 | VAX1's EX request goes to the master OVMXE: `+0x54` `00 02 00 00 05 00`. OVMXE grants it: `+0x58` `10 00 00 00 01 00 fa 00`, a grant record (`frame-grounding.tsv` row 353: `01 00 fa 00` is a grant) |
| 6244 / 6247 (03:25:06.96) | VAX1 -> OVMXE | VAX1 grants OVMXE the EVAC.DAT `F11B$` locks: OVMXE's first write after the takeover. Its seq 137 is stamped 03:25:05.36, so OVMXE's clock runs about 1.6 s behind the pod host's |
| 7307 / 7309 (03:25:32.33) | VAX2 -> VAX1 -> VAX2 | VAX2's probe asks the directory. VAX1's answer sends it to OVMXE |
| **7310** (03:25:32.328092) | VAX2 -> OVMXE | the probe's request: `+0x54` `00 1b 04 00 05 00`, mode EX, **flags `04` = LCK$M_NOQUEUE**. VAX1's EX in frame 5212 has flags `00` in the same position |
| **7312** (03:25:32.329468) | OVMXE -> VAX2 | OVMXE's only answer: `+0x58` **`14 00 05 00 00 00 fb 00`**. That is the **QUEUED** record (`frame-grounding.tsv` row 353: body[34] 0xfb is QUEUED), not a deny. The echoed name reads `EVAC 00 00 ORKLOAD` (`+0x6a`): bytes 4-5 of the name are zero |
| 7313 (03:25:32.44) | VAX2 -> OVMXE | the SCS ack. After it, no other DLM frame passes between VAX2 and OVMXE with this name (`evac-workload-frames.txt`) |

So OVMXE held EX as the master and was asked by a real node for EX with NOQUEUE. Its executive answered QUEUED
instead of going through the deny-at-master path (`h_deny_master`, `SS$_NOTQUEUED`). VAX2's `$ENQW` then completed
`SS$_NORMAL` with LKSB `SS$_NORMAL`, and EVPROBE `$DEQ`ed it. On VAX2 the probe looked granted while OVMXE kept writing
until seq 196. The frame alone does not settle whether the outcome byte is wrong or the request's NOQUEUE flag was never
read. The `14` in place of the request's `04`, and the two zeroed name bytes, are the leads.

## C3: the order on the disk follows the lock, not the clocks

The records go seq 136 (VAX1) -> 137 (OVMXE) with no gap or duplicate, and OVMXE's first write follows VAX1's last one on
the wire (frame 6244). But the grader compares timestamps across nodes, and OVMXE's clock is behind VAX1's. r6 measured
3.4 s for the same thing ("OVMXE's record timestamps run about 3.4 s behind VAX1's"). The C3 check `nxt[2] <= last[2]`
therefore fails whenever OVMX's clock is behind by more than the gap between VAX1's last record and the takeover. It is
recorded here as a failure. The criterion was not changed.

References: `OVMXE.console.log` 17 (mount), 26 (NL), 28 (EX at 137), 30 (dismount); `vax1.log` 19 (mount), 60/61/80
(STOP, SHUTDOWN, REMOVE_NODE); `vax2.log` 3 (precheck), 16 (EVPROBE), 18/19 (mount, TYPE), 153-215 (seam and OVMXE's run),
302/306 (VAX1 removed), 313 (Q=2 V=2 N=2); `ci6-frames.txt` frames 5168/5169, 5212/5214, 6244/6247, 7305-7314.
