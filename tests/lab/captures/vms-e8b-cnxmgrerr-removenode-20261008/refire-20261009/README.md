# vms-e8b re-fire, 2026-10-09 02:15Z (evacuation lane, pod vaxlab-3)

Same bed as m3/m4: real VAX1 (1025) + VAX2 (1026) V7.3, OVMXE (1030), VOTES
1/1/1, EXPECTED_VOTES 3, group 1; VAX LOCKDIRWT 0, OVMXE LOCKDIRWT 1.
OVMX build: integration branch evac/int-1 46cd329d = origin/main + #1572 (this
fix) + #1568 + #1564 + #1571 (+ a lab-only Dockerfile step staging EVACWL.EXE).

VAX1: `@SYS$SYSTEM:SHUTDOWN`, option REMOVE_NODE, no reboot.

* VAX1: "proposing modification of quorum or quorum disk membership", then
  "SYSTEM SHUTDOWN COMPLETE".
* VAX2: **no bugcheck** (m3/m4: CNXMGRERR both times). It logged VAX1 removed
  and "completed VAXcluster state transition"; afterwards Q=2 V=2 N=2,
  SHOW CLUSTER = VAX2 MEMBER, VAX1 BRK_NON, OVMXE MEMBER.
* OVMXE: "system ...401 was removed", "completed VAXcluster state transition",
  SHOW CLUSTER = OVMXE + 1026 MEMBER.
* No quorum loss on any survivor.

Files: wire.pcap.gz (all 0x6007 frames), vax1.log, vax2.log, OVMXE.console.log.
