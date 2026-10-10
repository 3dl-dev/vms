# ci.6 rolling evacuation, full run with volume handoff (2026-10-10, runs ci6-evac-15 and -16)

Lab bed: the same as `../vms-ci6-evac-20261009/`.
- Pod `vaxlab-3` (ovmx-lab), cluster group 1.
- Real OpenVMS VAX V7.3 nodes VAX1 (1025) and VAX2 (1026) under SIMH. Both run LOCKDIRWT 0.
- Booted OVMXE (1030) under QEMU/TCG runs LOCKDIRWT 1, so it is the only lock-directory node (the interim configuration).
- VOTES 1/1/1, EXPECTED_VOTES 3.
- The shared volume `VAX1DATA` is `$2$DUA1:` on the VAXes and `VDA100:` on OVMX.

Workload: EVACWL. It takes an EX lock on `EVAC$WORKLOAD` and appends fixed records `<node> <pid> <seq><time>` to `EVAC$DATA:EVAC.DAT`.

Builds (lab-only integration branches; the lab-only commit only stages EVACWL.EXE):

| run | OVMX build | content |
|---|---|---|
| 15 | `fa487d04` (`evac/int-10`) | main + #1582 (SYSGEN factory) + #1613 (vms-2ef close state) |
| 16 | `b8174637` (`evac/int-11`) | run 15 + #1615 (vms-cab op-0x0f answer) |

## Run 16: the whole evacuation (`run16/timeline.txt`, frames in `run16/wire.pcap.gz`)

| step | result |
|---|---|
| 0a. `DISMOUNT/CLUSTER` and 0. `MOUNT/CLUSTER $2$DUA1:` on VAX1, with OVMXE a member (close kind 6 to OVMXE) | **completed** on VAX1+VAX2. OVMXE reported as RMTDSMFAIL/DEVOFFLINE and RMTMNTFAIL/ACCVIO (status field, see gaps) |
| 1. OVMX standby takes NL, so OVMXE is directory + master | yes |
| 2. VAX1 workload EX granted by OVMXE | yes, seq 1..174 |
| SDA `SHOW RESOURCE/NAME=EVAC$WORKLOAD` on VAX1 | master CSID 00010003 = OVMXE; VAX1 EX granted |
| 3. VAX2 falsifier (STANDBY) | LEF for the whole run; never got EX |
| 4. **Evacuate**: `STOP EVACVAX1` | OVMX standby **granted EX** |
| 5. **Volume handoff**: VAX1 `DISMOUNT/CLUSTER $2$DUA1:` | **completed**: Online on VAX1 and VAX2 (it hung in run 13) |
| 6. OVMXE `MOUNT VDA100: VAX1DATA` | **`EVACWL: OVMXE took EVAC$WORKLOAD EX at seq 175`** |
| 7. VAX1 `SHUTDOWN` with REMOVE_NODE | VAX1 SHUTDOWN COMPLETE. VAX2 and OVMXE removed VAX1. Q=2 V=2 N=2 |
| after 7 | VAX2's `SHOW CLUSTER` returns at once (VAX2, OVMXE MEMBER). OVMXE answered 3/3 op-0x0f with 82/15 |
| Seam, read back on real VMS (VAX2 `SEARCH`) | VAX1 seq …174 (17:26:02.21), then OVMXE 175 (17:27:00.85) … 567. Contiguous: 567 records, no gap, no duplicate |
| Bugchecks / OVMX stalls | none on any console; no rcu stall or oops on OVMXE; quorum never lost |

Run 15 got the same result through step 7 (seq 174 → OVMXE 175). After VAX1 left, though, VAX2's DCL hung. Its op-0x0f (`F11B$vSYSDSK1`) to OVMXE was never answered. #1615 is the fix, grounded on 1309/1392 real pairs. Run 16 is the re-fire with that fix.

## Honest notes

- **The seam read-back was not cluster-coherent.** VAX2 mounted `$2$DUA1:` privately while OVMXE still had `VDA100:` mounted. OVMX's first `STOP SYSTEM_1` left EVACWL running, and `DISMOUNT` said DEVNOTDISM. VAX2 only ran `SEARCH` and then dismounted. OVMXE's second STOP ended EVACWL, and its DISMOUNT then succeeded.
- **The run-15 timeline says "DSMCLUFAIL ... DEVOFFLINE" at STEP 5.** That console's final remote status was `-SYSTEM-W-GPTFULL` (see `run15/vax1.log`). Either way it is an ungrounded status field (rd vms-1f64).

## Gaps filed (children of vms-ci.6)

- vms-1f64: OVMX's answer to a kind-6 close carries no grounded status. VMS shows RMTMNTFAIL/RMTDSMFAIL with garbage secondary status.
- vms-5a0: OVMX DCL `TYPE` of a write-shared open file gives RMS-E-FNF.
