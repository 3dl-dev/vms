# vms-c350 lab arm — PLAN (written before the pod exists, 2026-11-02)
Build: work/vms-c350-quorum-adjust 882558798 (PR #1919), cluster_auth_group=1.
Config: VAX1 VOTES 1 EV 1, VAX2 VOTES 0 EV 1 (golden), OVMXE VOTES 3 EXPECTED_VOTES 6.
After OVMXE joins: CEVOTES 6, QUORUM 4, votes present 4.
A. VAX1 `SET CLUSTER/EXPECTED_VOTES=4` (clamp [4,7] -> 4).
   P-A1 the open body[22:24] reads 3 (QUORUM), not 4 (CEVOTES).
   P-A2 VAX2 F$GETSYI CLUSTER_QUORUM=3; OVMXE quorum 3; no quorum-lost line.
B. VAX1 `@SYS$SYSTEM:SHUTDOWN` with REMOVE_NODE (4 - 1 = 3, clamp [3,5] -> 3).
   P-B1 the class-0x04 op 0x0d to OVMXE carries body[22:24] = 2.
   P-B2 VAX2 CLUSTER_QUORUM=2 CLUSTER_VOTES=3; OVMXE logs NO 'quorum lost'
        (unfixed code would derive (6+2)/2 = 4 > 3 present and block).
   P-B3 no bugcheck on any console; DCL on OVMXE and VAX2 answers.
Also recorded, not predicted: F$GETSYI EXPECTED_VOTES on VAX2 after A and B.

## Observed after OVMXE joined (05:0xZ) -- the plan's baseline was WRONG
VAX2: CN=3 Q=3 V=4 EV=1, not Q=4. OVMXE's PARAMS advertise EXPECTED_VOTES 0 at body[76:80]
(seen in the 93fa capture too), so the VAXes' max{EV} never includes OVMXE's 6.
OVMXE's own F$GETSYI reads CN=0 Q=0 V=0 EV=0 (rd vms-b6f), so OVMXE's quorum is read
only from its console lines.
Amended arm A (before running it): SET CLUSTER/EXPECTED_VOTES=7 (clamp [4,7] -> 7, Q 4).
  P-A1' the open's body[22:24] = 4 (QUORUM), not 7 (CEVOTES). P-A2' VAX2 Q=4.
Amended arm B: REMOVE_NODE: 7-1 = 6, clamp [3,5] -> 5, Q 3.
  P-B1' op 0x0d body[22:24] = 3. P-B2' VAX2 Q=3 V=3; OVMXE no 'quorum lost'
  (OVMXE's own p. 7-6 over its EV 6 would give 4 > 3 present and block).
