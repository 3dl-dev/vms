# vms-1b68 fire, 2026-11-02 (pod lab1b68): the arm FAILED as configured; no bugcheck

Build: artifacts run 41472795957, SOURCE_COMMIT 8e3692e8c (#1961 head 7eb359853 + LAB-ONLY staging
+ plan). ovmx_fire.sh gate over 25 frame types PASSED, including the new waiting-record swap and queue
negative controls. OVMXE joined at 15:51:19Z (VAX2 proposed the addition).

CPU: pod requests 200m, limit 4 (cgroup cpu.max 400000/100000, nr_throttled 0). It used 260.4 CPU-s in
2,301 s, an average of 0.11 CPU. kubectl top read 90m during OVMXE's boot and 49m after.

## What happened (raw)
- Arm A (WKL2) and arm B (EVAC$WORKLOAD) set as addendum 3 records; SDA on VAX1 confirmed both of
  OVMXE's locks queued at VAX1 (waiting 1A00067F EX; conversion 7C000679 NL->EX).
- 16:10:49Z VAX1 halted at sim> (^E). VAX2: "lost connection to system VAX1", "quorum lost, blocking
  activity", then "removed from VAXcluster system VAX1", "completing VAXcluster state transition".
  OVMXE: "quorum lost, blocking activity". Then EVACWL "$ENQW failed, status 3658" and RMHOLD "$ENQW
  root 'WKL2' failed, status %X00000E4A": both queued requests ended SS$_UNSUPPORTED.
- Wire: OVMXE sent NO op-0x08 record after the halt. No rebuild ran.
- No Fatal or bugcheck on any console.

## Why the arm could not pass
The vote choice in addendum 2 was wrong. Once OVMXE joined, the cluster held 2 votes (VAX1 1 + OVMXE
1), so quorum was 2 whatever EXPECTED_VOTES said. Halting VAX1 left 1 vote, so the survivors lost
quorum and never ran the departure transition. P1a, P1b, P2 and P3 were not reached. P4 FAILED (quorum
loss on both survivors).

## Observations filed separately
- OVMXE ended a survivor's queued $ENQW with SS$_UNSUPPORTED on quorum loss and departure. Real VMS
  blocks lock activity while quorum is lost; the requests stay queued.
- A stale lock-directory entry gave two masters of WKLD during setup (addendum 3).
- On OVMXE, SPAWN/NOWAIT does not pass DCL symbols to the subprocess (IVVERB on a foreign-command
  symbol), and `STOP <process-name>` returned no error but did not stop the subprocess. `STOP/ID` did.

## Re-fire recipe
Use a fresh pod. Choose votes so that the survivors stay quorate when the master is HALTED, for
example OVMXE VOTES 2 (ovmx_fire_node.sh hard-codes VOTES=1 and would need a knob). The cluster total
is then 3 and quorum 2, and VAX2 + OVMXE keep 2. On VAX1, never ^Y a console whose subprocesses hold
the arm. Do not run SHOW SYSTEM/CLUSTER: it hung both VAX consoles with OVMXE a member.
