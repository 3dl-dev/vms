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

# Re-fire, 2026-11-02 (pod lab1b68r): PASS -- P1, P2, P3, P4 all hold; no bugcheck

Build: SOURCE_COMMIT 5849a3396 (lab/vms-1b68 = #1961 head 65e34db74 + plan). Pod lab1b68r, CPU request
200m / limit 4 (cpu.max 400000/100000, nr_throttled 0); 296 CPU-s over ~42 min (~0.12 CPU avg); kubectl
top 65m after the arms. Votes (addendum 5): VAX1 1, VAX2 0, OVMXE 2; CLUSTER_VOTES 3, QUORUM 2.
Arms and the pre-departure SDA: addendum 6. Wire decode: refire-wire-WKL3-EVAC.txt (refire_dump.py over
refire-wire.pcap.gz); consoles: refire-console-{vax1,vax2,ovmxe}.log.

- 17:12:00Z VAX1 `@SYS$SYSTEM:SHUTDOWN`, option REMOVE_NODE. VAX1: "proposing modification of quorum",
  "SYSTEM SHUTDOWN COMPLETE". VAX2 17:12:27.93: "removed from VAXcluster system VAX1", "completed
  VAXcluster state transition". No "quorum lost" on any console.
- P1 PASS. In the departure rebuild OVMXE sent exactly one op-0x08 holder record per queued lock to
  VAX2 (the new master), read off its LKBs:
  f16694 EVAC$WORKLOAD converter: [28]=02 [30]=05 [31]=00 [108]=04 [110]=00 -> answered 82/01, VAX2 lkid 0200059A
  f16696 WKL3 waiter:            [28]=11 [30]=05 [31]=00 [108]=04 [110]=ff -> answered 82/01, VAX2 lkid 2200000D
- P2 PASS. SDA on VAX2 after the transition (CSID 0, VAX2 masters both): WKL3 granted 060003BF PR,
  waiting 2200000D EX (OVMXE); EVAC$WORKLOAD granted 020003C4 EX, conversion 0200059A NL->EX (OVMXE).
  Neither request ended SS$_UNSUPPORTED; neither was granted early.
- P3 PASS. 17:14:01Z STOP SYSTEM_1 on VAX2 (RMH 300-WKL3): SDA on VAX2 shows WKL3 granted 2200000D EX,
  queues empty; OVMXE's RMHOLD's $ENQW returned and it went on to "holding -- hibernating". 17:15:26Z
  STOP SYSTEM_2 on VAX2 (EVACWL EX): OVMXE's EVACWL printed "holding EVAC$WORKLOAD"; VAX2's RSB then
  names OVMXE (CSID 00010003) as master (VAX2 remastered both trees to the sole holder, op 0e/0f/10/13/14,
  17:15:00Z and 17:15:56Z).
- P4 PASS. 0 matches for Fatal|bugcheck|CNXMGRERR|LOCKMGRERR|INVLOCKID|quorum lost on all three consoles.

## Observations filed separately
- OVMXE's RMHOLD: `$GETLKIW` on its own granted lock 01000411 returned %X00000014 (SS$_BADPARAM) after
  the remote grant, so RMHOLD's self-report was "(not answered)". The grant itself is shown by VAX2's SDA.
- OVMX DCL: SPAWN rejects /INPUT; SPAWN/OUTPUT is ignored (output reaches OPA0); TYPE a,b types only a;
  ANALYZE/SYSTEM is absent (NOIMG).
