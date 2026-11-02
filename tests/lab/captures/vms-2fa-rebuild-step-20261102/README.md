# vms-2fa: a survivor's op-0x08 records go out in the step its own LOCKDIRWT selects

`PLAN.md` was committed and pushed before the pod existed (dd989d833). Its two addenda were each
committed before the arm they describe fired (567a173ff, then the next commit). `rebuild_step.py`,
committed with the plan, scores every arm.

## The rule

A surviving holder sends its op-0x08 rebuild records inside the departure's transition:

- after the release (cat-0x01 op-0x0c, step at body[16]) of **step 5 if its own LOCKDIRWT is
  non-zero**;
- after the release of **step 6 if its own LOCKDIRWT is 0**.

It answers every record before its own next barrier frame. As a participant that frame is its next
01/0b step report. As the coordinator it is its next release. When the transition re-registers the
directory, those op-0x0d records sit between releases 4 and 5.

All 1,965 real records whose barrier is in a capture obey this rule: 1,377 from weighted senders
after release 5, and 588 from weight-0 senders after release 6. `rebuild_step.py --selftest` pins
the split, and ctest `cluster_host_dlm_rebuild_step` runs it. The selftest goes red on any constant
release (5, 6 or 12).

What does **not** select the step:

- **Transition type.** Both a clean shutdown and a fault removal occur at 5 and at 6.
- **Directory-vector change.** E5 and arm 1 both change it and are at 6. Arm 2 changes it and is at 5.
- **The coordinator's weight.** In E4 the coordinator has weight 1 and the step is 6.
- **Directory membership.** In E5 every node is a directory node at weight 0, and the step is 6.

## The arms (VAX only; OVMX emitted nothing at a real VAX)

| arm | pod | LOCKDIRWT VAX1/2/3 (read by F$GETSYI) | VAX1 leaves by | VAX3 role | VAX3 records | after release | dir step | closed / held |
|---|---|---|---|---|---|---|---|---|
| 1 | lab2fa #1 | 1/1/0 | SHUTDOWN REMOVE_NODE | coordinator | 281 | **6** | 5 | yes / yes |
| 3 | lab2fa #1 | 0/1/0 | SHUTDOWN REMOVE_NODE | n/a | **0** | n/a | none | n/a |
| 3b | lab2fa #1 | 0/1/0 | simulator stopped (`^E`) | participant | 1 | **6** | none | yes / yes |
| 2 | lab2fa #2 | 1/1/1 | simulator stopped (`^E`) | coordinator | 459 (+337 from VAX2) | **5** (VAX2 also 5) | 5 | yes / yes |

Addresses: VAX3 is `08:00:2b:98:a4:8f` and VAX2 is `08:00:2b:8b:87:27` in both pods. VAX1 is
`aa:00:04:00:01:04`.

Each `armN-departure.pcap.gz` keeps every 0x6007 frame, cut by time alone, from the departure
transition's opening cat-0x01 frame through its release of step 12:

| file | frames | window (UTC, 2026-11-02) |
|---|---|---|
| arm1 | 694 | 10:29:43.878 to 10:29:43.942 |
| arm3b | 54 | 11:08:06.389 to 11:08:06.434 |
| arm2 | 1,770 | 11:40:10.367 to 11:40:10.577 |

The shutdown remaster that precedes arm 1's barrier is not in the trimmed file.

**Arm 3b is lab33a's case exactly.** VAX3 is a weight-0 participant holding a CR root plus one CR
sub, and the master departs. The frames:

| frame | from > to | message |
|---|---|---|
| 19 | VAX3 > VAX2 | 01/0b step 6 |
| 21 | VAX2 > VAX3 | 01/0c release 6 |
| 22 | VAX3 > VAX2 | 02/08 DEPW ROOT, lkid 300000F0 |
| 23 | VAX2 > VAX3 | 82/01 0xf9 |
| 24 | VAX3 > VAX2 | 01/0b step 7 |

These are frame numbers in the trimmed file; the full capture's are 119 to 124.

OVMXE in lab33a sent its record after release 12.

## Predictions

| | prediction | verdict |
|---|---|---|
| P1 | arm 1: every VAX3 record after release 6 | **PASS** (281/281, frames 101 to 664 of the trimmed file) |
| P2 | arm 1: an op-0x0d run closed by release 5 | **PASS** (39 records, VAX3 to VAX2) |
| P3 | arm 1: every record answered before release 7, and the next own barrier frame held until the last answer | **PASS** |
| P4 | arm 1: the DEPT ROOT goes to VAX2 and is answered 0xf9; no SUB is sent; VAX3 then masters DEPT | **PASS** (see below) |
| P5 | arm 1: VAX2 puts no op-0x08 on the wire | **PASS** |
| P6 | no bugcheck, and VAX2 and VAX3 stay a 2-node cluster | **PASS**, with a caveat |
| P7 | arm 3: no op-0x0d, and VAX3's records after release 6 | arm 3 **NOT SCORABLE**; arm 3b **PASS** (1/1 after release 6, no op-0x0d, closed and held) |
| P8 | arm 2: every record after release 5 | **PASS** (VAX3 459/459, VAX2 337/337) |

Detail behind the verdicts:

- **P4.** The DEPT ROOT, lkid 30000308 (SDA truth), went to VAX2 and was answered 0xf9. No SUB
  record was sent for S0. SDA on VAX3 afterwards shows DEPT with CSID 00000000, the root CR
  30000308 and one sub-RSB. The value block's status is **INVALID** and its seqnum is 2
  (`console-pod1-vax3.log`).
- **P6 caveat.** No node logged a LOCKMGRERR, CNXMGRERR or INVLOCKID. VAX2 and VAX3 run VOTES 0,
  so they logged "quorum lost, blocking activity" after every VAX1 departure, and regained quorum
  when VAX1 rejoined. The rebuild finished inside the transition, before the quorum block.
- **P7, arm 3.** At LOCKDIRWT 0, VAX1 handed DEPV over to VAX3 during its shutdown remaster
  (op-0x0e/0f/10/11/12/13/14, full-capture frames 3855 to 3873), so nothing was left to rebuild.
  The full arm-3 capture is not committed, because it would move the remaster census for a
  remaster nobody asked about. Arm 3b took the same weights with a stop instead.
- **Rival hypotheses.** H2 ("directory-vector change selects 5") fails twice: arm 1 changes the
  vector and is at 6, and E5 does too. H7 ("5 only when every survivor is weighted") is not
  separated from the rule. With two survivors, a weighted sender facing a weight-0 peer is the only
  directory, and its records are local. Separating H7 needs three or more survivors.

## What OVMX's sender gate tests

Two executive-observable facts, both read and neither guessed:

1. **The executive's own LOCKDIRWT**, the SYSGEN value it already places in its lock-directory
   weight vector. It selects the release `R = (LOCKDIRWT != 0) ? 5 : 6`.
2. **The barrier FSM's step state** in the departure's transition (its epoch at body[12:16]):
   - **Participant.** The rebuild window opens when it receives release R, and the node holds
     its step report R+1 (`barrier_send_step`) until every record is answered.
   - **Coordinator.** The window opens when it sends release R, and the node holds release R+1
     until every record is answered.
   - **Both.** No record goes out with the window closed: not before release R, after release
     R+1, or after the transition. That is rd vms-bcb's closed `rb_window`, unchanged until this
     binding lands.

## Things that happened in the lab, for whoever runs this next

- **A conversational `SET LOCKDIRWT` persisted.** After the arm-3 conversational boot, a normal
  `B DUA0` came back at 0. Read the weight with `F$GETSYI` before every arm, and never assume it.
- **The golden roots differ.** VAX3's stored LOCKDIRWT is 1, and it was 0 only when set at
  SYSBOOT>. VAX2 and VAX3 run VOTES 0.
- **Kill SIMH with `kill -9`.** A SIMH `boot cpu` after "SYSTEM SHUTDOWN COMPLETE" left VAX1 in the
  ROM at about 83M insts/tick, so a fresh simulator was started (README: "only a fresh simulator
  clears it"). A plain `kill` of `nodedrv` orphaned the old SIMH, and two VAX1 instances ran on one
  tap and one system disk. The second VAX1 then failed `SYSINIT-E, error mounting system device`.
  Pod #1 was discarded, its lab directory removed, and arm 2 ran in a fresh pod (#2) from fresh
  golden clones. The orphaned VAX1 instances started after arm 3b had fired, by their process
  start times. The only simulator that could have outlived an earlier kill was one already halted
  at `sim>` after SHUTDOWN, which sends nothing. Each arm's capture carries one VAX1 address, and
  every node read CLUSTER_NODES 3 before it fired.
- **Never `pgrep -f` inside `kubectl exec sh -c`.** The pattern matches that shell's own command
  line, and the exec dies with 137.
