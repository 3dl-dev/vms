# vms-f297: what a real V7.3 coordinator puts in a transition open (2026-10-08)

rd vms-f297. Before this change, an OVMX member could not coordinate an admission that a
real VAX takes part in. That is the case where OVMX holds the highest SCSSYSTEMID.

The reason was the transition open (cat 0x01 op 0x09). A real coordinator fills it with
Phase 1 data: Davis p. 7-40 lists the proposed quorum, CEVOTES, quorum-disk votes,
foundation time, founder's SCSSYSTEMID and rebuild type. OVMX had no derivation for any
of these fields. Sending zeros there bugchecked a real VAX (rd vms-1ac).

This directory places every one of those fields. It uses **one-variable controlled
reconfiguration** on a private three-node OpenVMS VAX V7.3 cluster (dlmlab L-lab, SIMH,
the shared lab disk). Each node is booted conversationally with
`SYSBOOT> SET <param> <n>`, then the opens are read off the wire. The rig2 runs
(VAXC on the demo disk + two OVMX nodes) give the reverse proof for the subject's
QDSKVOTES.

All captures are trimmed to the cat-0x01 messages this grounding reads: 0x01, 0x02, 0x05,
0x07, 0x08, 0x09 and 0x14 (`trimops.py`). Specimens are cut with `mkspec.py`. No real
VAX bugchecked in any run.

## Runs

| tag | configuration (vax1 founds; then vax2, vax3 join) | what it decides |
|---|---|---|
| XA | V1/E1; V1/E2; V1/E3 | baseline; vax2 coordinates vax3 (a non-founder coordinating) |
| XB | V2/E2; V1/E3 | quorum, CEVOTES |
| XC | V1/E1; V0/E1 | a non-voting joiner |
| XD | V3/E3; V1/E3 | votes 4 > EV 3, so quorum 3 |
| XE | V1/E1; V2/E3; V1/E3 LOCKDIRWT 2 | a non-founder coordinator with 2 votes; subject LOCKDIRWT |
| XF | V1/E1; V1/E2 QDSKVOTES 2; V1/E3 QDSKVOTES 0 | **subject QDSKVOTES** |
| XG | V1/E1 RECNXINTERVAL 300; vax2 SIGKILLed and rebooted at once | rejoin inside the window |
| XH | same, rebooted only after vax1 removed it | rejoin after the removal (pcap not kept; read in place) |
| XI | V1/E1 LDW 1; V1/E3 LDW 0; V1/E1 LDW 1 | **MERGE rebuild**; CEVOTES > votes |
| FC | rig2, unmodified build | control: op-09 [26:28] = 0000 to OVMX |
| FX1/FX2 | rig2, lab-only builds putting VAX op-0x02 bytes in OVMX's op 0x02 | do not move [26:28] |
| FX3 | rig2, lab-only build putting 1 at OVMX's PARAMS body[24:26] | **flips [26:28] to 0100** |

The FX builds were lab-only branches (`lab/f297-x1..x3`) and are never merged.

## The op-0x09 body

Offsets are SYSAP-body offsets; the frame-absolute offset is body + 72.

| body | field | evidence |
|---|---|---|
| [12:16] | epoch | (already grounded) |
| [16:18] | role/class tag 0x0240 | (already grounded) |
| [20:22] | the next CSV slot: the subject's new slot + 1 | XA 3/4; KR-1 readmission slot 4 gives 5; removals leave it alone |
| [22:24] | proposed quorum = (CEVOTES + 2) / 2 | XD: 4 votes, EV 3, reads 3; XI: EV 3, 2 votes, reads 2 |
| [24] | rebuild type (p. 7-40): 4 full, 3 directory, 1 merge | op 07 = 4; every ADD/REMOVE = 3; XI (zero-weight joiner) = 1 |
| [25] | **stale** | 00 vs 3a to two recipients of the same open |
| [26:28] | the **subject's** QDSKVOTES, as its PARAMS body[24:26] advertised it | XF: 2 gives 2, 0 gives 0; FX3 reverse proof |
| [28] [29] | members, votes the last FORMATION or REMOVAL left | popcount of every op 07/08 nodemap (16/16); 0 0 from a coordinator that saw neither |
| [32:40] | formation time | equals the founder's PARAMS body[28:36] in every capture holding both |
| [40:48] | the coordinator's VMS time at the open | |
| [49:51] | the founder's SCSSYSTEMID (low 16 bits) | XA/XE/XF ep4: vax2 coordinating carries 1025 |
| [55] | nodemap | (already grounded) |
| [87:89] | the subject's own op-0x02 body[36:40] | 2, 3, 4 from VAX joiners; 0 from every OVMX joiner |
| [91] | **stale** | 00 vs 7f to two recipients of the same open |
| [92], [100:102], [102:104] | CSV high-water: the subject's slot on a rejoin, one below it on a first admission | XG, XH, KR-1, fault-f1, oracle-return vs XA..XF |
| [96:98] | proposed CEVOTES = max(EXPECTED_VOTES, votes, old CEVOTES) | XI: 3 from EV 3 against 2 votes |
| [98:100] | the lowest slot in the vector | 1 in every specimen |
| [104:106] | the highest slot in the post-transition nodemap | 7/7 |
| [106:114] | LE64 0xffffffde78ee6000, a -900 s VMS delta time | identical in every op 07/08/09 from every VAX installation |
| [114:132] | **stale** | one specimen carries OPCOM text here |

## PARAMS (op 0x01) words placed on the way

| body | field | evidence |
|---|---|---|
| [20:22] | (EXPECTED_VOTES + 2) / 2 | EV 1, 2, 3 give 1, 2, 2 |
| [24:26] | QDSKVOTES | XF: 2, 0, default 1 |
| [76:80] | EXPECTED_VOTES (was "param_f2, observed const 1") | 21/21 PARAMS across XA..XI |

OVMX's own PARAMS still sends 0 in all three words. Advertising its SYSGEN values
there is rd vms-b338.

## What coordinating for a real VAX needed beyond the cells

These were found on rig2 (`runP.sh`). VAXC (real V7.3, SCSSYSTEMID 1989) founds the
cluster. OVMXA (1995) joins, and OVMXB (1988) then asks; OVMXA, as the
highest-numbered member, coordinates with the real VAX as a participant. Each arm
below was a lab-only build. A VAX bugcheck on them is the reason for the fix
listed, not a shipped result.

| arm | what happened on the real VAX | fix |
|---|---|---|
| PF-1 | step 1 reported, never released: OVMXA's join FSM swallowed every op 0x0b | the join hands a step report on to its coordinator (`join_forward`) |
| PF-2 | CNXMGRERR on OVMXA's step ack | the 0x81/0x0b carries `10 <class> 01` (893 real acks; the builder left 17-18 echoed) |
| PF-3, PK-1 | answered OVMXA's op-0x05 with status 00, then CNXMGRERR on the op 09 | the open waits for every record's 0x81/0x05 (a real coordinator sends op 05, waits for the answer, then sends op 09: XF); a status-00 answer abandons the transition (p. 7-41); the joiner tells every connected member who it is before it asks (XF: vax3, 1.3 s before its request), because in PK-1 OVMXB asked 20 ms after the VAX's connection opened |
| PF-4 | all twelve steps, no bugcheck | -- |

`runP.sh` greps "BUGCHECK"; the console says "BUG CHECK". The graders used for the
stall matrices (`grade3.sh`) and `pfloop.sh` match both spellings.

## A joiner re-dialling a real VAX mid-conversation (rd vms-a35c)

The stall matrices on lab build c8 (arms GM-14, TG-3, HM-11; `rig/matrix.sh`,
graded by `rig/grade3.sh`) bugchecked VAXC with CNXMGRERR. In each one OVMXB was
not yet a member and already had a conversation open with VAXC, because it now
introduces itself to every connected member before it asks (PK-1 above). Its
connection to VAXC re-formed during the stall. VAXC's ACCEPT carried connect
data cd[12:14] = 2: it had taken two of OVMXB's messages and expected the
conversation to continue at send 3. OVMXB reset the conversation, as E77 does
for a system that is not a member, and spoke at send 1, ack 0.

The fix is the ACCEPT half of rd vms-ba4's connect-data resume:

- SCS keeps the ACCEPT's 16 connect-data bytes on the initiator's CDT
  (`scs_conid_accept_conndata`).
- The connection manager reads them when the connection opens. When the peer
  says it has taken N, the block resumes at send N and acks what this node's
  own CONNECT advertised.
- What this node already told the peer about itself stands. A resumed
  conversation does not re-send MODEL or PARAMS (rd vms-8c54 arm F-4).
- A peer that took 0 is a fresh conversation and changes nothing.

| build | runs | result |
|---|---|---|
| c8 (before) | HM, 21 arms | 1 VAX bugcheck (HM-11, together:20) |
| c9 (fix) | IM, the same 21 tokens | 21 × 0 VAX bugchecks; 20 PASS, and IM-2's FAIL is the login harness below (both nodes members, VAX proposed B) |
| c9 (fix) | PFI: `rig/pfloop.sh`, 3 runP + 3 runPK | 6/6 PASS, 0 bugchecks; the VAX reported losing member OVMXB in all three kills |
| c9 (fix) | IMR: IM-2's token plus two more, on the final login harness | 3/3 PASS, 0 bugchecks; every login on the first attempt |

The rig's console login is now prompt-synchronised (rd vms-b64, `rig/b36node.sh`).
It used to log in by timing, and a %CNXMAN line landing between the prompts typed
the poll's SHOW CLUSTER into `Username:` (FM-5, FM-11, HM-8). Now:

- each field is sent only after its own prompt appears, counted past a baseline;
- a DCL prompt is any line that *starts* with `$`, because in IM-2 a kernel line
  was appended to the `$ ` itself;
- a session found back at `Username:` before a poll logs in again.

## Still open

- **body[48]** (rd vms-91d) is 02 in one archived oracle (`vms-af4-unclean-return-20261001/oracle`:
  a VOTES-0 system rejoining inside the window) and 00 everywhere else, including XG,
  which repeats that run with VOTES 1, and XJ, which replays it with VOTES 0. An
  OVMX coordinator writes 00, which is what every other specimen carries.
- **the op-0x08 removal cells** are not yet placed. A removal toward a foreign member
  is still refused, so a real VAX removes a lost member itself.
