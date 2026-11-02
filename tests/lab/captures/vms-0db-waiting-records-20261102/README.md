# vms-0db: a real holder sends its WAITING and CONVERTING locks, and says which they are

`PLAN.md` and `RMWAIT.MAR` were committed and pushed before the pod existed (dce2781fa). Each of
the four addenda was committed before the firing it describes (0da4a10bd, f624a53f4, 01efe556a,
3ff1fac6c). Pod `lab0db` (`ovmx-vaxlab:7`, 1500m/2Gi, requests == limits), three real OpenVMS
VAX V7.3 nodes, no OVMX node: **OVMX emitted nothing at a real VAX.** The pod and its lab
directory were deleted after the run.

Addresses: VAX1 `08:00:2b:..:01:04` (CSID 00010001), VAX2 `..:1d:3b` (00010002), VAX3 `..:3e:93`
(00010003). LOCKDIRWT 1/1/1, read with `F$GETSYI` before the arms.

## The rule

A holder sends **one record per LKB it owns in the tree: granted, converting and waiting
alike**, in an op-0x12 remaster record and in an op-0x08 departure-rebuild record. Every cell
below was checked against SDA on the holder before the move and on the new master after it.

| cell | meaning | granted | converting (CR -> EX) | waiting (new EX) |
|---|---|---|---|---|
| body[30] | requested mode | the mode (or an earlier request, see below) | 5 | 5 |
| body[31] | granted mode | the mode | 1 | **0** |
| body[110] | **queue state** | **0x01** | **0x00** | **0xff** |
| body[108] | (unnamed) bit 0x04 | 0x00 (0x04/0x08 on a few system locks) | 0x04 | 0x04 |
| body[28] | the lock's flags as SDA names them: VALBLK 0x01, CONVERT 0x02, SYSTEM 0x10 | | | |
| body[24:28], [20:24], [36:44] | own lkid, owner EPID, parent pair | as for a granted lock | same | same |

- **body[110] is the only cell that marks the state.** The modes do not. A granted lock that
  was converted keeps its earlier request in body[30]: VAX1's own WQRE root (RMHOLD takes EX,
  then converts down to CR, and SDA on the new master lists it granted at CR) is sent as (5,1),
  state 0x01, flags 0x13, which is byte for byte the converting root's modes and flags. Over the
  corpus, 2,398 granted records have body[30] != body[31]. They include every op-0x12 ROOT
  record with body[30]=0 and body[31]!=0, which is how rd vms-0db's question arose (327 of
  them, all (0,3) and all at state 0x01, so none is waiting).
- **Order.** Per sender and resource, granted records go first, then converting, then waiting.
  This holds for WQRC's W0, WQRE's root, and WQRB's root and W0. Those are the only four
  sender/resource groups in the corpus that mix states. The corpus's earlier non-granted
  records each name a distinct resource.
- **The answer** is an 82/01 like any other. It carries status 0xfa, or 0xf9 when the sender
  becomes the master, and the new master's lkid at body[20:24]. **body[28] is 0x10 | the record's
  body[108]**, which holds on all 5,694 real 0xfa answers. So a non-granted record is answered
  0x14. The answer to a non-granted record echoes the record's (body[30], body[31]) where a
  granted one carries f0 01 / f1 01 (ROOT) or 00 00 (SUB).
- **The new master puts each lock back on the queue it came from**, still blocked (PA7, PB3).

The corpus already held 25 non-granted records, all of them system locks: CACHE$ converting EX
over CW, AUDRSV$ NL -> PW, and a waiting UCX$INETACP_. That explains rd vms-380's "measured
oddities" (05 02 and 04 00 in ROOT answers), the 0x14 answers in rd vms-874f's and vms-2fa's
departures, and the body[110]=0 cells that rec12's census listed but did not name.

## The firings

| firing | tree | old master's holder | root queues | VAX2 activity | moved? | capture |
|---|---|---|---|---|---|---|
| A0 | WQRA | alive | C, Q queued ~15 s after the activity began | 4x RMH 312B | yes, uncaptured | none (addendum 1) |
| A1 | WQRC | stopped first | C, Q queued | 4x RMW 31A | **no** | `0db-armA` (not committed) |
| A2 | WQRC | stopped | empty | 4x RMW 31A | **no** | same |
| A3 | WQRC | alive (CO2) | C2, Q2 queued | 2x RMW 31A (CP3/CP4 hit SYSTEM's subprocess quota) | **no** | same |
| **A4** | WQRC | alive | empty (W0's conversion and waiting queues full) | 4x RMW 31A | **yes, 13:41:25Z** | `armA4-remaster-WQRC.pcap` |
| **A5** | WQRE | alive (EO1) | conversion EC, waiting EQ | 4x RMW 31A | **yes, 13:48:43Z** | `armA5-remaster-WQRE.pcap` |
| **B** | WQRB | VAX1 itself leaves (SHUTDOWN REMOVE_NODE) | conversion BC, waiting BQ; W0 BV, BU | none | rebuilt 14:01:03Z | `armB-departure-WQRB.pcap.gz` |

Trimmed files keep every 0x6007 frame in a time window, nothing else:

| file | frames | window (UTC 2026-11-02) |
|---|---|---|
| `armA4-remaster-WQRC.pcap` | 42 | 13:41:25.650 to 13:41:25.770 (WQRC's 0x0e through its last 82/15) |
| `armA5-remaster-WQRE.pcap` | 36 | 13:48:43.495 to 13:48:43.700 |
| `armB-departure-WQRB.pcap.gz` | 1,113 | 14:01:03.660 to 14:01:03.752 (the departure transition's 01/12 through release 12) |

VAX1's shutdown remasters before arm B's barrier are not in the trimmed file.

### Arm A4: WQRC, op-0x12 (frame numbers as `remaster_census.py` counts them in the trimmed file)

```
 17 VAX3>VAX2 12 WQRC 070003F0 rq/gr 1/1 st 01            -> fa/10 13000401
 20 VAX3>VAX2 12 WQRC 070003EE rq/gr 1/1 st 01            -> fa/10 480003FD
 22 VAX3>VAX2 12 WQRC 100003DB rq/gr 1/1 st 01            -> fa/10 150003FB
 24 VAX3>VAX2 12 WQRC 060000E0 rq/gr 1/1 st 01            -> fa/10 1F0003E4
 26 VAX3>VAX2 12 W0   28000363 rq/gr 1/1 st 01 par 100003DB/150003FB -> fa/10 0100040B
 28 VAX3>VAX2 12 W0   080003ED rq/gr 5/1 st 00 par 070003EE/480003FD -> fa/14 0100040C  converting
 30 VAX3>VAX2 12 W0   080003EF rq/gr 5/0 st ff par 070003F0/13000401 -> fa/14 0100040D  waiting
 32 VAX3>VAX2 12 S1   050000E8 rq/gr 1/1 st 01 par 060000E0/1F0003E4 -> fa/10 0100040E
 34 VAX3>VAX2 12 S0   060003DC rq/gr 1/1 st 01 par 060000E0/1F0003E4 -> fa/10 0100040F
```

### Arm A5: WQRE, op-0x12

```
 13 VAX1>VAX2 12 WQRE 5D000608 rq/gr 5/1 st 01 flags 13   -> fa/10 20000407  VAX1's own GRANTED CR (EX->CR)
 14 VAX3>VAX2 12 WQRE 4F0003F4 rq/gr 1/1 st 01 flags 11   -> fa/10 0A000415
 19 VAX3>VAX2 12 WQRE 300003F9 rq/gr 5/1 st 00 flags 13   -> fa/14 0A000416  converting
 21 VAX3>VAX2 12 WQRE 070003FC rq/gr 5/0 st ff flags 11   -> fa/14 01000417  waiting
 23 VAX3>VAX2 12 S1   100003FB rq/gr 1/1 st 01 par 4F0003F4/0A000415 -> fa/10 01000418
 25 VAX3>VAX2 12 S0   460003F8 rq/gr 1/1 st 01 par 4F0003F4/0A000415 -> fa/10 01000419
```

### Arm B: WQRB, op-0x08 (VAX3 the coordinator, every record after release 5, `rebuild_step.py`: closed and held)

```
 917 VAX2>VAX3 08 WQRB 16000403 rq/gr 3/3 st 01           -> f9 (VAX2 becomes the master)
1059 VAX3>VAX2 08 WQRB 07000401 rq/gr 1/1 st 01           -> fa/10 020004E6
1063 VAX3>VAX2 08 WQRB 170003F0 rq/gr 1/1 st 01           -> fa/10 020004E8
1065 VAX3>VAX2 08 WQRB 0D0003EE rq/gr 1/1 st 01           -> fa/10 020004EA
1067 VAX3>VAX2 08 WQRB 270000AB rq/gr 1/1 st 01           -> fa/10 020004EC
1069 VAX3>VAX2 08 WQRB 07000403 rq/gr 5/1 st 00           -> fa/14 020004ED  converting
1071 VAX3>VAX2 08 WQRB 07000404 rq/gr 5/0 st ff           -> fa/14 020004EF  waiting
1073 VAX3>VAX2 08 W0   100003FC rq/gr 1/1 st 01 par 0D0003EE/020004EA -> fa/10 020004F0
1075 VAX3>VAX2 08 W0   3D0003F2 rq/gr 5/1 st 00 par 170003F0/020004E8 -> fa/14 020004F2  converting
1077 VAX3>VAX2 08 W0   08000400 rq/gr 5/0 st ff par 07000401/020004E6 -> fa/14 020004F3  waiting
1079 VAX3>VAX2 08 S1   310003FD rq/gr 1/1 st 01 par 270000AB/020004EC -> fa/10 020004F5
1081 VAX3>VAX2 08 S0   300003D3 rq/gr 1/1 st 01 par 270000AB/020004EC -> fa/10 020004F7
```

## Predictions

| | prediction | verdict |
|---|---|---|
| PA1 | the tree moves with its conversion/waiting queues non-empty (against HN) | **PASS** on A5 (root queues full) and A4 (W0's queues full). Arm A's first firing did not move, and addendum 2 records it as a trigger failure, not as HN |
| PA1b | A2 moves | **FAIL**. A2 did not move: with the old master's holder already stopped, nothing moved WQRC |
| PA1c | A3 moves | **FAIL**. Not separated: A3 ran with two activity processes, not four |
| PA1e | HN: A5 does NOT move | **FAIL, so HN is falsified.** WQRE moved with C and Q queued on its root |
| PA2 | one record per holder LKB (HR) | **PASS**. A4 9/9 (PA2b), A5 5/5; every record's body[24:28] is a lkid SDA listed on VAX3. HR0 is rejected |
| PA3 | Hx: body[30] requested, body[31] granted | **PASS**. Converting (5,1), waiting (5,0), granted (m,m). Hy is rejected. Caveat: a granted, converted lock keeps its earlier request: (5,1) at state 0x01 |
| PA4 | HQ: a cell beyond body[30:32] separates the states | **PASS**. body[110]: 01 / 00 / ff. body[108] bit 0x04 is set on every non-granted record but also on 10 granted ones, so it is not a separator |
| PA5 | HO: queue order | **PASS** in all four mixed groups |
| PA6 | the parent pair of the waiting/converting subs | **PASS**. A4's U W0: 070003F0 / 13000401, which is CU's root answer. V W0: 070003EE / 480003FD, which is CV's root answer. Arm B the same |
| PA7 | queue placement on the new master | **PASS**. SDA on VAX2 after A4: W0 conversion queue 0100040C (CR -> EX), waiting queue 0100040D (EX). After A5: WQRE conversion 0A000416, waiting 01000417. Each is the lkid in its record's 82/01 answer. None was granted, and no console logged SS$_DEADLOCK |
| PA8 | no bugcheck | **PASS**. 0 LOCKMGRERR/CNXMGRERR/INVLOCKID/BUGCHECK on the three consoles (see the caveat on VAX1's log below) |
| PB1 | waiting/converting LKBs are rebuilt in op-0x08, after release 5 (VAX3 LDW 1) | **PASS**. 11/11 VAX3 LKBs, release 5, closed and held |
| PB2 | the same cells as op-0x12 | **PASS**, LKB for LKB |
| PB3 | queue placement on the rebuilt master | **PASS**. SDA on VAX2: WQRB conversion 020004ED, waiting 020004EF; W0 conversion 020004F2, waiting 020004F3. None was granted. The W0 RSB reads `Status: INVALID` (its value block) |
| PB4 | no bugcheck; the survivors stay a 2-node cluster | **PASS**, with a caveat: after the departure, SDA on VAX2 and VAX3 hung (quorum lost: VAX2 and VAX3 run VOTES 0, as in vms-2fa). VAX1 was brought back on a fresh simulator (the old one was killed with `kill -9`) and rejoined at about 14:08Z. The queued SDA commands then ran, and they are PB3's truth |

## What the OVMX sender must change (rd vms-0db, vms_lock.c)

1. `dlm_handover_fill` stops refusing `lock->waiting`. Any LKB the node owns in the tree,
   whether on the granted, conversion or waiting queue, is filled. Excluded: a proxy that is
   still pending its first answer from the old master, because it has no master copy to name.
   That is the existing `master_lkid != UNSET` check.
2. body[30] = `requested_mode`, body[31] = `granted_mode`, or **0** for a lock never granted.
   Both are read off the LKB and never derived from each other.
3. body[110] = the LKB's queue: **0x01** granted, **0x00** converting, **0xff** waiting. Read it
   from which queue the lock is on, never from a mode comparison.
4. body[108] = **0x04** on a converting or waiting record (measured 42/42) and 0x00 on a
   granted one. Ten real granted system locks carry 0x04 or 0x08 there, for a reason this
   capture does not name. OVMX has no source for that, so it writes 0. The answerer's body[28]
   is then 0x10 | body[108].
5. body[28] carries the lock's flags (CONVERT 0x02 once the lock has been converted), as it does
   today for VALBLK/SYSTEM.
6. Order: per resource, granted, then converting, then waiting. A sub of a waiting parent cannot
   exist, so parent-before-child still holds.
7. The op-0x08 rebuild path takes the same fill.
8. The answer: OVMX as new master puts a received record with body[110] 0x00 on its conversion
   queue (requested body[30] over granted body[31]), and one with 0xff on its waiting queue. Its
   82/01 answer echoes body[30:32] and sets body[28] = 0x10 | body[108].

## Things that happened in the lab, for whoever runs this next

- **Activity moves a tree only while the old master still holds a lock in it.** WQRA moved
  during setup with its holder alive. WQRC did not move in A1 or A2 (holder stopped) or in A3
  (holder back, but two processes). It moved in A4 with four processes.
- **SYSTEM's subprocess quota is 10.** A SPAWN past it fails, and `vcmd` filtering hid the
  error. Stop old holders first.
- **Queue the waiters after every granted request in the tree**, including the activity
  processes' roots: a new request queues behind a waiter. `RMWAIT` kind `A` (root and sub
  granted at once, activity after 120 s) exists for that.
- **VAX1's console log was overwritten** when its fresh simulator started (`nodedrv.py`
  truncates the log). `console-vax1-before-restart-tail.log` is the last 390 lines read at
  13:10Z, which covers run A0. VAX1's later commands (CO1, CO2, EO1, BO, and SHUTDOWN at 14:00Z)
  appear in this README and in the frames, not in a console log.
- **`/NOBROADCAST` hides the CNXMAN and quorum messages** from the console log. Read SHOW
  CLUSTER, or leave broadcasts on for a departure arm.

## Files

- `PLAN.md`, `RMWAIT.MAR`: the registration and the holder program (5 kinds + delayed activity).
- `armA4-remaster-WQRC.pcap`, `armA5-remaster-WQRE.pcap`, `armB-departure-WQRB.pcap.gz`: the
  trimmed captures.
- `console-vax{1,2,3}.log`, `console-vax1-before-restart-tail.log`: the consoles, with the SDA
  truth before and after.
- The rule is pinned by `tools/cluster/dlm_waiting_record_census.py --selftest` (ctest
  `cluster_host_dlm_waiting_record_census`).
