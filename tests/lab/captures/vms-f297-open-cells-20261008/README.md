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

## Still open

- **body[48]** is 02 in one archived oracle (`vms-af4-unclean-return-20261001/oracle`:
  a VOTES-0 system rejoining inside the window) and 00 everywhere else, including XG,
  which repeats that run with VOTES 1. Lab run XJ (the VOTES-0 replay) decides it.
  Until a capture grounds it, an OVMX coordinator writes 00, which is what every
  other specimen carries.
