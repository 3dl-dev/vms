# rd vms-e88 — whom a joiner asks for admission, and when

The stall rig's NOFAULT arms (M2-21, Q-1, S-1, S-11, S-23, T-15) and the V0.7-3
browser all-at-once run share one shape: the joining OVMX node (`OVMXB`) put its
membership request (cat-0x01 op-0x02) where no coordinator would take it, and was
never admitted. This directory is the real-VMS reference for the joiner's side of
that decision, and the decode of the two failing shapes against it.

**The captures here are TRIMMED.** Each `e88-*.pcap` keeps only the frames the
finding reads — every 0x6007 frame to or from the joiner that is channel/VC
control, SCA connection control, or a `VMS$VAXcluster` cat-0x01/0x81/0x04 body,
plus every op-0x02/op-0x12 between any two nodes (`trim.py`). The full captures
(MSCP and DLM bulk included) stay on the lab PVC at `/lab/k8s-labs/e88lab/`.
Hashes are in `docs/clean-room/reference-captures.sha256`.

## The lab

Three real OpenVMS VAX V7.3 nodes under SIMH (the golden 3-node volume:
`VAX1` 1025, `VAX2` 1026, `VAX3` 1027, each booted from its own root of the shared
disk), on a private bridge inside a disposable pod (`ovmx-lab/e88lab`). No
`vaxlab-*` pod and no live cluster was touched. The founder boots conversational
with `VOTES 1`, `EXPECTED_VOTES 1`; the others boot normally. `orsetup.sh`,
`seq.sh` and `expC.sh` are the drivers; `consoles/` holds every node's console.

The real joiner prints whom it asked: `%CNXMAN,  sending VAXcluster membership
request to system VAXn`.

| trio | founder | then | joiner | discovery order at the joiner | the joiner asked |
|---|---|---|---|---|---|
| **A** | VAX3 1027 | VAX1 1025 joins | VAX2 1026 | 1027, then 1025 (last) | **VAX3 1027** |
| **A2** | VAX1 1025 | VAX3 1027 joins | VAX2 1026 | 1027, then 1025 (last) | **VAX3 1027** |
| **B** | VAX1 1025 | VAX2 and VAX3 boot **together** | both | — | VAX3 asked VAX1 (the only member); VAX2 asked **VAX3**, 1.25 s after VAX3's admission |
| **C3** | VAX3 1027 | VAX1 1025 joins | VAX2 1026, with every frame between it and VAX3 dropped at the bridge for 5 min | 1025 only, then 1027 | **nobody for 5 minutes**, then **VAX3 1027**, 9.8 s after VAX3 came into reach |

What the four trios establish:

1. **The member asked is the highest-SCSSYSTEMID member** in every case — the
   founder in A, the most recently joined in A2. Not the one nearest the end of
   the joiner's CLUB (discovery order: 1025 was discovered last in both A and A2,
   and was not asked), not the founder, not the last to join. This is the same
   predicate the receiver side already uses to decide who coordinates (rd vms-1ac,
   107/113 over the reference trees).
2. **A joiner asks only a system that says it is a member, and only once it has
   connectivity with as many members as they say there are** (Davis pp. 7-37/7-38).
   In C3 VAX2 held a connection to VAX1 alone (`%CNXMAN,  have connection to system
   VAX1`), VAX1's PARAMS said the cluster had two members, and VAX2 sent no
   membership request to anybody for five minutes; the moment VAX3 was reachable
   (`discovered system VAX3`, `established connection to system VAX3`) it asked VAX3.
3. **The member count is on the wire**: op-0x01 PARAMS `body[18:20]` (abs 90) is
   the sender's member count — 1 from a lone founder, 2 from either member of a
   two-member cluster, **0 from every joiner** — and a joiner's copy moved 0 -> 2 on
   the frame its sender was admitted (B: VAX3's PARAMS to VAX2 carry 0 at t=113.473 and 2 at t=115.207 of the full capture, 60 ms after its commit). Every
   member PARAMS also carries `body[12] = 0x21` and two nonzero VMS time quadwords
   at `body[28:44]`; every joiner PARAMS carries zeros there. Those are NOT decoded
   and NOT used. `p01all.py` prints all of them.
4. **The request follows the member's own PARAMS, never the joiner's identity
   burst.** In every trio the member's op-0x14/op-0x01 reached the joiner first;
   the joiner's op-0x02 went out 0.7–1.9 s after its own PARAMS (A, A2), 3 s in B,
   9.8 s in C3. Real members re-send their PARAMS to a waiting joiner when a
   transition changes the count (B: both members within 1 ms of VAX3's commit).

## The two failing OVMX shapes, decoded against it

`S-1` (rig build `ae3bf895`): OVMXB discovered OVMXA 2 s before the real VAX and
asked it — nearest the CLUB tail. OVMXA is outranked by VAXC (1989 > 1987) and
discards op-0x02 by design (rd vms-1ac). A real joiner in that position (C3) asks
nobody until it can reach VAXC, then asks VAXC.

`T-15` (rig build `b8fb125c`): OVMXB asked the right system, the real VAX — but
(`cmconn.py`) its MODEL, PARAMS and op-0x02 left in ONE burst, on the
VAX-initiated connection (`dbe8000c`/`32520009`), 2.7 s before the VAX had said a
word; the pair's two connects had crossed, and the VAX answered with its own
MODEL/PARAMS on the OTHER connection (`dbea000b`/`32520008`) and never read a
request on the first. Every real joiner's request followed the member's PARAMS
and never shared a burst with its own.

**"The VAX rejected it", read off the wire** (`rej.py`): every REJECT_REQ the real
VAX sent OVMXB in S-1 and T-15 is on **`MSCP$DISK`**, reason words `[58:62] = 00 00
2c 00` — and so does every one of VAXC's 15 `MSCP$DISK` rejects in S-1: 14 to
OVMXA, which it admitted, 1 to OVMXB. VAXC serves no disks; the lab VAXes reject each other's
`MSCP$DISK` connects every 10 s too (with `00 00 01 00`). It is not a
`VMS$VAXcluster` reject and not the cause: OVMX already treats an MSCP$DISK refusal
as a non-event (E68).

## The fix (src/kernel-core/vms_cnxman_join_fsm.c, rd vms-e88)

* op-0x01 `body[18:20]` is parsed into the sender's CSB (`adv_members`) and built
  from `club.cluster_nodes` — to a peer running this implementation only; a foreign
  connection manager keeps getting the 0 OVMX has always sent (the member-only
  bytes beside it are not grounded).
* The joiner ranks by "says it is a member" then highest SCSSYSTEMID
  (`join_outranks`), at selection, at re-issue, and on every beat of ADMIT.
* op-0x02 is held (`join_admission_held`) until the member being asked has sent
  its PARAMS, while the members advertise more members than this node has
  connectivity with, and for one beat after this node's own MODEL/PARAMS; the join
  moves to a higher member once that member's PARAMS says it is one, and onto the
  connection the CSB records for its member after a crossing.
* A system whose own PARAMS says it is in no cluster is never asked (trio B). If
  every system in sight says so, the attempt ends as an exhausted round (the fact
  the founding election reads) with nobody asked.

## The first build of this fix on the stall rig, and what it taught

Rig arm E2-1 (`JOIN_ORDER=together`, build `5e7e4b50`) still PASSED, but its
decode showed the joiner's FIRST request go to the other OVMX node while that node
was itself being admitted: its PARAMS then still said 0, the real VAX had not yet
sent its own, and the rule as first written fell back to asking whatever it was
driving through. It went unanswered for 5 s and the re-issue to the VAX admitted
it. Trio B is exactly this position and the real joiner asked nobody; the no-cluster
rule above is the result.

## Files

* `e88-{A,A2,B,C3}-20260930.pcap` — the trimmed captures.
* `consoles/<trio>-vax<n>.log` — every node's console.
* `orsetup.sh`, `orboot.sh`, `con.sh`, `orstop.sh`, `seq.sh`, `expC.sh` — the lab.
* `op02.py`, `firsts.py`, `joinseq.py`, `p01all.py`, `cmconn.py`, `rej.py`,
  `trim.py` — the decoders (offsets from `docs/cluster-protocol-spec.md` 4(d),
  4(h)(1a), 4(j) only).
