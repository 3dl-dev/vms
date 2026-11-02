# vms-26e lab re-proof: a VAX granted by an OVMX new master gets the tree's value block

Registered 2026-11-02 ~07:30Z, BEFORE the OVMX node boots. Pod `lab33a` (this
lane's own, deleted after). Build `lab/vms-26e-reproof` = origin/main `552028e83`
(includes #1926) + the lab-only RMHOLD staging (`f8f462511`), artifacts from
`build-boot-artifacts.yml` with `cluster_auth_group=1`.

Arrangement = RA7 fire A (tests/lab/captures/vms-6ad1-ra7-residue-20261021/PRE.md):
O = VAX1, N = OVMXE, H = VAX2. LOCKDIRWT 1/1/0.

1. VAX1 `RMHOLD 123WRA7T`: VAX1 masters RA7T, writes `RMLVB-0123456789`, Seqnum 1.
2. OVMXE `RMHOLD 100-RA7T`. P0: VAX1 SDA lists a master copy for OVMXE's CSID.
3. VAX1 `STOP HOLD1`: the tree remasters onto OVMXE (0e/0f/10, 0x1a, 13, 14).
4. VAX2 `RMHOLD 100-RA7T` (root CR, LCK$M_VALBLK): OVMXE grants it.

Predictions, each scored on its own:
- Q1 (the defect's own cell): OVMXE's 82/01 grant to VAX2 for RA7T carries
  body[36:52] = `RMLVB-0123456789` and body[52:56] = 1 (was all zero in fire A).
- Q2: VAX2 `SDA> SHOW RESOURCE/NAME=RA7T` reads Valblk `RMLVB-0123456789`,
  Seqnum 1 (was 00000000 / 0).
- Q3: no bugcheck on any console, CLUSTER_NODES 3 on both VAXes throughout,
  every DCL step returns.
A failed prediction is the result; it is not re-cut.
