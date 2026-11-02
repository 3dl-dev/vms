# vms-26e lab re-proof: a VAX granted by an OVMX new master gets the tree's value block

Pod `lab33a` (this lane's own), 2026-11-02 07:24-07:34 UTC. Build
`lab/vms-26e-reproof` `9487af6e3` = origin/main `552028e83` (with #1926) plus the
lab-only RMHOLD staging (`f8f462511`, never merged). Artifacts from
`build-boot-artifacts.yml` run `41404552438` (`cluster_auth_group=1`).
`ovmx_fire.sh` checked `SOURCE_COMMIT` and passed the frame-conformance gate
(`dlm_enq_req dlm_enq_ans_queued dlm_grant_notify dlm_deq dlm_rm_ack
dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14`: every byte grounded, negative control
refused) before boot. Predictions: [`PLAN.md`](PLAN.md), written before the
OVMX node booted, not re-cut.

The arrangement is RA7 fire A: VAX1 (`aa:00:04:00:01:04`) is the old master,
OVMXE (`52:54:00:00:e5:01`, CSID `00010003`) is the new master, and VAX2
(`08:00:2b:e6:29:7b`) is the later requester.

## Steps and what happened

| # | step | observed |
|---|---|---|
| 1 | VAX1 `RMH 123WRA7T` | VAX1 SDA: RA7T `CSID 00000000`, Valblk `RMLVB-0123456789`, Seqnum 1 |
| 2 | OVMXE `RMH 100-RA7T` | P0 held: VAX1 SDA `060006A3 CR ... RA7T ... MSTCPY ... Master copy of lock 0100038C on system 00010003` (OVMXE's RMHOLD printed `0100038C`) |
| 3 | VAX1 `STOP HOLD1` | 07:28:54 VAX1 -> OVMXE 0e, 0f, **0x1a carrying `RMLVB-0123456789` seq 1**, 10, 13; VAX1 -> VAX2 0f, 11; VAX2 -> OVMXE 13; OVMXE -> VAX1 and VAX2 14, each answered 82/15. VAX1 then has no RA7T RSB (`%SDA-E-NORSBNAM`) |
| 4 | VAX2 `RMH 100-RA7T` | VAX2 02/01 to OVMXE (body[28] = 0x11), OVMXE's 82/01 answer below |

OVMXE's grant to VAX2, 07:33:24.689, body[16:64]:

```
00 00 00 00 8d 03 00 01 7f 03 00 5a 10 00 00 00 01 00 fa 00
52 4d 4c 56 42 2d 30 31 32 33 34 35 36 37 38 39   <- body[36:52] "RMLVB-0123456789"
01 00 00 00                                       <- body[52:56] seq 1
00 00 00 00 00 00 00 00
```

In RA7 fire A (before #1926) the same answer carried zeros in body[36:56].

## Scoring

| prediction | result |
|---|---|
| Q1 the grant carries body[36:52] = `RMLVB-0123456789`, body[52:56] = 1 | **PASS** |
| Q2 VAX2 `SHOW RESOURCE/NAME=RA7T`: Valblk `RMLVB-0123456789`, Seqnum 1 | **PASS**: `CSID 00010003`, `Valblk: 31302D42 564C4D52 39383736 35343332`, `Seqnum: 00000001`, granted `5A00037F CR` |
| Q3 no bugcheck, `CLUSTER_NODES` 3 on both VAXes, every DCL step returns | **PASS**: no bugcheck, CNXMGRERR, LOCKMGRERR or quorum-lost line on any console; CN=3 on both VAXes after step 4 |

## Files

- `console-*.log*`: the three consoles at the end of the run.
- The full br0 capture is `/lab/run-fire/r26e/wire.pcap` on the `ovmx-lab` PVC.
  It is not committed: it carries OVMX-originated frames, which would move the
  real-sender census pins (rd vms-4c0). The grant bytes above are copied from it.
