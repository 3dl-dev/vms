# RA7 result: a real VAX does not read the op-0x14 residue span

Three fires on 2026-11-01 and 2026-11-02, pod `lab6ad1` (this lane's own pod,
reset before each fire, deleted after). Each fire had two real OpenVMS VAX V7.3
nodes and OVMXE. The build is `lab/vms-6ad1-ra7` `ef0b7e3fc`: the tree of
PR #1900 as merged (`85b8a3375`) plus the lab-only RMHOLD staging. Artifacts
come from `build-boot-artifacts.yml` run `41352991717`, and
`tests/lab/tools/ovmx_fire.sh` checked their `SOURCE_COMMIT` and the
frame-conformance gate before every boot ([`PRE.md`](PRE.md)).

Predictions: [`PLAN.md`](PLAN.md) (2026-10-21) and [`PLAN-C.md`](PLAN-C.md).
PLAN-C.md was registered after fires A and B, before C booted, and was pushed in
`0f70bf7d8`. Neither plan was re-cut.

| fire | `OVMX_RM_RESIDUE_FILL` | old master | directory for `RA7T` | other VAX |
|---|---|---|---|---|
| A | absent (zeros) | VAX1 | VAX2 | (VAX2) |
| B | `0x5A` (`SYSGEN-I-SETPARAM ... changed from 0 to 90`) | VAX2 | VAX2 | VAX1, holds nothing |
| C | `0x5A` | VAX1 | VAX2 | (VAX2) |

The directory node is fixed by the name's hash, and it is VAX2 every time.

## Verdict

**The residue span is not read.** A VAX whose op-0x14 span was `5a` ended
with exactly the lock and resource state of a VAX whose span was zero, and did
the same things next. This held for all three roles that received a pattern:
old master only (VAX1, fire C), directory only (VAX2, fire C), and old master
plus directory (VAX2, fire B). No bugcheck on any console in any fire.
`CLUSTER_NODES = 3` on both VAXes throughout each fire.

**Not exercised:** the holder role. In all three fires the bystander VAX held
nothing when the tree moved. That was necessary to make OVMXE the chosen new
master (PLAN.md P0), so no op-0x12 records were sent and no VAX received a 0x14
as a holder. This is the same limitation as `vms-c2b`'s fire.

## Scoring

**P0. A real VAX master lists a lock held for OVMXE before anything moves. Held
in all three fires.**

| fire | master | SDA on the master |
|---|---|---|
| A | VAX1 | `Lock id: 0700065F ... Granted at CR ... RA7T ... Master copy of lock 01000361 on system 00010003` |
| B | VAX2 | `Lock id: 0200038F ... Granted at CR ... RA7T ... Master copy of lock 01000364 on system 00010003` |
| C | VAX1 | `Master copy of lock 01000367 on system 00010003` |

`01000361`, `01000364` and `01000367` are the lkids OVMXE's own RMHOLD printed.

**P1. The hand-over reaches OVMX's engine. PASS for every step that occurred.**

| fire | RA7T frames (from `trimmed.pcap.gz`) |
|---|---|
| A | VAX1→OVMXE 0e, 0f, 10 (each answered 82/15); VAX1→VAX2 0f, 11; VAX1 and VAX2 → OVMXE 13; OVMXE→VAX1 and OVMXE→VAX2 14, each answered 82/15 |
| B | VAX2→OVMXE 0e, 0f, 10, 13; OVMXE→VAX2 one 14, answered 82/15. VAX1 sent no 13 and got no 14: it never touched the tree (rule W1) |
| C | as A |

The op-0x12 leg did not occur (see "not exercised").

**P2. The answers are the real ones. PASS.** `remaster_census.py --pairs |
score_remaster` over each capture:

- A: 6 of 6 pairs byte-exact.
- B: 44 of 44.
- C: 38 of 38.

B and C also contain VAX1↔VAX2 remasters of `CACHE$cmSYSDSK1`, which are
real-VAX pairs and byte-exact too. Output is in `fire*/score.txt`.

**P3. Not applicable.** No op-0x12 records came in, so the adoption installed
no record handles. The handle OVMXE granted from is the one P8 shows.

**P4. The residue is not read. PASS** for the three roles above:

- **a.** `SHOW RESOURCE/NAME=RA7T` after the move, zero fire vs pattern fires:
  - Old master: no RSB (A: VAX1; C: VAX1).
  - Directory: `DIRENTR VALID`, `NL`, `Lock Count 0`, `CSID 00010003`
    (A: VAX2; C: VAX2; B: VAX2, after a transient `RM_PEND` read during the
    move).
- **b.** The VAXes' own locks in the tree: none left after the move, in every
  fire.
- **c.** The `82/15` echo of OVMXE's 0x14 differs from the request only at
  body[0:4] (the CM envelope), [8], [9] and [34], every time. The span comes
  back verbatim: `00` in A, `5a` in B and C. The decoded rows are in
  `fire*/pairs.txt`.
- **d.** What each VAX did next was the same in all fires. A CR request on
  `RA7T` from the other VAX went to OVMXE and was granted (P8). DCL steps
  returned. `CLUSTER_NODES` stayed 3.

**P5. No bugcheck, no hang. PASS.** All three consoles in every fire show zero
lines matching `bugcheck|bug check|fatal|CPUSPINWAIT|VAXPORT`. Every DCL command
returned.

**P6. The frame is what the grounding table says. PASS.** `run_gate.sh` passed
before each boot (`dlm_rm_14` for A, `dlm_rm_14_ra7` for B and C). Its
`DEVIATION` rows for `dlm_rm_14_ra7` are the three pattern rows the table
declares.

**P7. The pattern did not reach a named field. PASS** (B and C). body[8:10] =
`02 14`, body[40:44] = 0, body[44:52] = `00 00 03 04 'RA7T'`, body[128:132] =
`3c256feb`, the same hash fire A sent.

**P8. OVMX serves the tree afterwards. PASS** in all three fires. The other
VAX's `RMHOLD 100-RA7T` went to `HIB` holding, and its SDA reads master
`CSID 00010003` with its own lock granted at CR (A: `0B0003BF`; B: VAX1
`080006A6`; C: `17000390`).

**PLAN-C C1-C4: PASS**, by the rows above.

## Observed, not predicted (filed)

- In every fire, the requesting VAX's SDA after OVMXE's CR grant shows
  `Valblk: 00000000 ... Seqnum: 00000000`. The tree's value block was
  `RMLVB-0123456789` at Seqnum 1 on the old master. Whether a non-master RSB
  holds the value block at all needs a VAX-to-VAX control: **rd vms-26e**.
- On OVMXE, `F$GETSYI("CLUSTER_NODES")` reads 0 while it is a member, and
  RMHOLD's `$GETLKIW` on its own root lock returns `%X00000014`: **rd vms-b6f**.

## A void attempt, recorded

The first fire C attempt (02:0x UTC) ran none of its steps. The lane's
wait-for-login loop hung on a stalled `kubectl exec` for about 3.5 hours. The
OVMX node's 3-hour run limit (`DUR=10800`) then stopped QEMU, and both VAXes
logged the departure (`timed-out lost connection to system OVMXE`, quorum
recomputed). Nobody touched `RA7T` and no frame of this arm was sent. Its
capture is not used. The pod was reset and fire C ran from scratch.

## Files

`fire{A,B,C}/`:

- `trimmed.pcap.gz`: RA7T and its transactions, via
  `tools/cluster/trim_lab_pcap.py`.
- `pairs.txt` and `score.txt`.
- All three consoles. The VAX logs are gzipped; fire A's VAX1 log carries a
  full `SHOW LOCK/ALL`.
