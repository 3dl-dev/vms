# vms-c350: a REMOVE_NODE quorum adjustment, as a member sees it

`c350-removenode-cm.pcap` is every cat-0x01/0x81 connection-manager frame
(341) of the vms-93fa arm-1 run (pod `lab93fa`, 2026-11-02, build `ef0b7e3fc`).
The full capture and consoles are in
`tests/lab/captures/vms-93fa-depart-remaster-20261102/`. Trim rule: a 204-byte
0x6007 SCS-MSG frame whose SYSAP body[8] is 0x01 or 0x81. Payloads unaltered.

Members, from their own PARAMS (op 0x01, body[22:24] VOTES):

| node | SCSSYSTEMID | VOTES | EXPECTED_VOTES |
|---|---|---|---|
| VAX1 | 1025 | 1 | 1 |
| VAX2 | 1026 | **0** | 1 |
| OVMXE | 1030 | 1 | 3 |

So max{EXPECTED_VOTES} = 3 and p. 7-6 gives QUORUM 2.

| frame | what | body[22:24] |
|---|---|---|
| 292, 293 | VAX1 `SHUTDOWN`/`REMOVE_NODE`: class-0x04 open, cat 0x01 op 0x0d, to VAX2 and to OVMXE | **1** |
| 302 | VAX2's class-0x03 removal of VAX1, op 0x08, to OVMXE | **1** |

Afterwards VAX2's `F$GETSYI`: `CLUSTER_NODES=2 CLUSTER_QUORUM=1
CLUSTER_VOTES=1` -- VAX2's own 0 plus OVMXE's 1, so VAX2 did count OVMX's vote.
OVMXE, which re-derived QUORUM 2 from its own table, logged
`%CNXMAN, quorum lost, blocking activity` when VAX1's vote left.

The same cell agrees with the survivors in every other op 0x0d in the library:
2 in `vms-ci6-evac-20261010` runs 15 and 16 (survivors read Q=2), 1 in
`vms-cab-op0f-20261010` heldout1/heldout2 (2-node VAX-only). The rest of the
op-0x0d body is NOT the f297 open layout: its body[96] equals body[20] - 1 in
all of them (9 in run 15, where CEVOTES was 3), so OVMX reads only body[22:24].

Fixtures: `tests/cluster/host/fixtures/cm-depart-quorum-open-to-ovmx.spec`
(frame 293), `cm-remove-after-adjust-open-to-ovmx.spec` (frame 302).

## labc350: the lab proof on the fixed build (2026-11-02, 04:50-04:59 UTC)

Own pod `labc350` (deleted after). Build `882558798` (PR #1919), artifacts run
`41389435049`, cluster group 1. OVMXE booted with VOTES 3, EXPECTED_VOTES 6;
VAX1 1/1, VAX2 0/1. `labc350/PLAN.md` was written before the pod existed. Its
baseline was wrong: it predicted Q=4 on join. OVMXE's PARAMS advertise
EXPECTED_VOTES 0, so the VAXes never count its 6. The plan was amended in that
file, after the baseline was read and before either arm ran.
`labc350/labc350-cm.pcap` uses the same trim rule (351 frames).

| step | VAX2 `F$GETSYI` | open body[22:24] (to VAX2 / to OVMXE) | OVMXE |
|---|---|---|---|
| OVMXE joins (ADD, op 0x09) | CN=3 Q=3 V=4 | 3 / 3 | member |
| A. VAX1 `SET CLUSTER/EXPECTED_VOTES=7`; VAX1: *"yields quorum of 4"* | CN=3 **Q=4** V=4 | **4 / 4** (op 0x0d) | no quorum line |
| B. VAX1 `SHUTDOWN`/`REMOVE_NODE` | CN=2 **Q=3** V=3 | **3 / 3** (op 0x0d); VAX2's op 0x08: 3 | **no quorum line**, DCL answers, `SHOW CLUSTER` OVMXE+VAX2 MEMBER |

- In step A the cell reads 4, the QUORUM, not 7, the expected votes. That
  settles the cell's meaning with values that tell the two readings apart.
  Held-out fixture: `cm-setcluster-quorum-open-to-ovmx.spec`.
- In step B, p. 7-6 over OVMXE's own table (max{EV} = its own 6) gives
  QUORUM 4. With 3 votes present, the pre-fix code would block exactly as it
  did in the vms-93fa run. The fixed node holds the coordinator's 3.
- Also observed, not used: in op 0x0d, body[91] is the new EXPECTED_VOTES
  (7, then 5 = clamp(7-1, [3,5])), and it was the same for both recipients.
  VAX2's own `EXPECTED_VOTES` SYSGEN value stayed 1.
- No bugcheck on any console. VAX1 halted at the end of its own shutdown,
  as SIMH does, and was not rebooted.
- OVMXE's `F$GETSYI("CLUSTER_*")` reads 0 (rd vms-b6f), so OVMXE's own
  quorum is evidenced by its console alone.
