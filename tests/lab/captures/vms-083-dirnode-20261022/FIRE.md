# vms-083 FIRE: the owed-op-0x13 set is the ROLES, measured one variable at a time

Run 2026-10-22, pod `lab083`, two real OpenVMS VAX V7.3 nodes (VAX1 SCSSYSTEMID
1025, VAX2 1026) and one OVMX node (OVMXE, 1030) on the pod's own `br0`.
`PLAN.md` beside this file was committed and pushed **before** the pod existed
(fire branch `lab/vms-083-dirnode` at `2f9f22718` = `origin/main` +
the LAB-ONLY `RMHOLD.EXE` staging commit + the registration). Boot artifacts:
`build-boot-artifacts.yml` run **39759587179**, `cluster_auth_group=1`
(`etc/ovmx/cluster_authorize.dat` in the initramfs reads `TUAC` v1 **group 1**,
checked before boot -- rd vms-daa). `ovmx_fire.sh` verified `SOURCE_COMMIT ==
2f9f227188aae92b45449bc6da0c136685e0fdac` against a clean gated tree and the
frame-conformance gate passed for `dlm_enq_req dlm_rm_ack dlm_rm_ack_nm
dlm_rm_14 dlm_deq` ("every byte grounded", negative control refused).

## The bed, read and not assumed

| | read |
|---|---|
| VAX1 | `SCSSYSTEMID` 1025, `LOCKDIRWT` **1** |
| VAX2 | `SCSSYSTEMID` 1026, `LOCKDIRWT` **1** |
| OVMXE | `SCSSYSTEMID` 1030, `LOCKDIRWT` **0** (the GENERAL configuration) |
| membership | `F$GETSYI("CLUSTER_NODES")` = 2 before OVMXE booted, **3** on both VAXes after, and **3** on both at the end |

`RMHOLD.MAR` (the committed one, from `../vms-50d-ra5-14cells-20261013/`) was
FTP'd into `SYS$LOGIN` on both VAXes and assembled + linked on each (`MACRO` +
`LINK`); the golden disk carried none. OVMXE ran the staged `RMHOLD.EXE`.

## The one variable, set by reading the real nodes' own lock databases

Candidate roots were taken one at a time by VAX1's `RMHOLD 100-D83An` and
`SHOW RESOURCE/NAME=` read on **both** VAXes' SDA:

| name | VAX1 | VAX2 | the tree's directory node |
|---|---|---|---|
| `D83A0` | `Status: DIRENTR VALID, CSID 00000000` | `%SDA-E-NORSBNAM` | **VAX1** (= its master) |
| `D83A1` | master | `Status: DIRENTR VALID, Lock Count 0, CSID 00010001` | **VAX2** (a third member) |
| `D83B0` | `%SDA-E-NORSBNAM` | master + directory | **VAX2** |
| `D83B1` | `DIRENTR VALID, CSID 00010002` | master | **VAX1** |

## The four runs

Every run: the old master takes the root at CR with `RMHOLD 100-<name>`, OVMXE
takes the same root at CR, then `STOP` ends the old master's own holder -- the
RA2/RA5/RA8/`vms-c2b` trigger. Nothing else differs.

| run | name | old master O | directory | third member | op-0x13 senders | op-0x14s |
|---|---|---|---|---|---|---|
| A | `D83A1` | VAX1 | **VAX2** | — | **2**: VAX1 `f5571`, VAX2 `f5572` | **2** (`f5575`, `f5576`), both answered 82/15 |
| B | `D83A0` | VAX1 | **VAX1** | VAX2 | **1**: VAX1 `f7341` | **0** |
| A' | `D83B1` | VAX2 | **VAX1** | — | **2**: VAX2 `f9069`, VAX1 `f9070` | **2** (`f9073`, `f9074`), both answered 82/15 |
| B' | `D83B0` | VAX2 | **VAX2** | VAX1 | **1**: VAX2 `f9934` | **0** |

`frames.txt` is the whole read (`scan_d83.py`), `d83-arms-trimmed.pcap.gz` the
frames by the rule in `tools/cluster/trim_lab_pcap.py` (84 of 10,783).

**In arm B and B' the third member was not even TOLD**: no `op-0x0f` and no
`op-0x11` for that tree reached it, and it sent nothing in the 180 s the arm
watched (B' was polled to a 0x0e within 5 s of the trigger and then watched for
a further 100 s). The `0x0f`/`0x11`/`0x13` frames to the third member that DO
appear in that window belong to a different tree -- a real-VAX-to-real-VAX
hand-over of `F11B$aSYSDSK1` + a file number -- and are named as such in
`frames.txt`.

## Scored predictions

| | verdict |
|---|---|
| **P1** arm A reproduces the control: exactly two 0x13 senders (old master + directory), tree HELD, one 0x14 each, both answered | **PASS** |
| **P2** arm B: exactly ONE 0x13 sender, the old master; the member with no role sends none | **PASS** |
| **P3** the defect reproduces as its own detector: arm B's tree does not reach HELD, no 0x14 | **PASS** -- OVMXE's own counters after all four runs: `trees opened=4 refused=0 records adopted=0 refused=0 ends=6` / `trees held=2 op-0x14 sent=4 unsent=0 completions unreported=0` / `proxy_lkbs=4`. Six ends for four trees (2+1+2+1) and **two** held: exactly the two control arms. (The `%CNXTRACE-I-DLMOWED` line `PLAN.md` names does not exist in this build -- it is in an unmerged PR -- so `DLMRM`/`DLMRMHELD` carry the same facts instead.) |
| **P4** who is told: arm A sends the directory node an `op-0x11`; arm B tells the roleless member nothing | **PASS**, as recorded above -- so "told" is not a wider set than "owes" either |
| **P5** never crash a peer: zero `BUG`/`CNXMGRERR`/`INVLOCKID`/`INVEXCEPTN`/`HALT`/`FATAL` on all three WHOLE consoles, `CLUSTER_NODES` 3 at the end, every DCL step returned | **PASS** -- 0 matches over 1,322 console lines (`console-vax1.log`, `console-vax2.log`, `console-ovmxe.log`) |
| **P6** the variable really moved, on the wire as well as in SDA | **PASS** -- arm A: OVMXE asked **VAX2** and was answered `0xf8` REDIRECT (`f4618`/`f4619`) before going to VAX1; arm B: OVMXE asked **VAX1** directly, no redirect (`f6881`); A': redirect from VAX1 (`f8558`/`f8559`); B': straight to VAX2 (`f9317`) |
| **P7** the roles-swapped repeats agree with their twins | **PASS** -- A' == A and B' == B, sender for sender |

## The one extra measurement, and why it matters to the implementation

OVMX's own directory lookups carried a **non-zero** hash on every arm name
(`hash=4daf8fd4` for `D83A1`, `cf7c21d4` for `D83A0`, `587afbd4` for `D83B1`),
and each went to the node the real cluster's SDA named as that name's directory
-- **4 of 4**, over both vector entries. So the two reads the fix composes (the
hash this engine holds for the name, resolved through the connection manager's
weight vector) agree with the real cluster's own assignment on every name this
arm used. That is what makes the owed set computable from executive state
instead of guessed.

## What was implemented, exactly

Per `PLAN.md`'s pre-registered mapping for P1 ∧ P2 ∧ P7, and nothing more: the
owed set is seeded with the **op-0x0e sender** and the tree's **directory node**
when this node can name it, a member that really sent an **op-0x12** joins it,
and a 0x13 from a selected member the seed did not name is **admitted and
acked** rather than refused. No count, no majority, no heuristic. The engine
change is in `src/kernel-core/vms_dlm_rm_fsm.c` (`rm_owed_seed`,
`rm_dir_member`, `rm_owed_add`) behind one new injected door, `dir_node`.
