# rd vms-af4 — a member killed and rebooted inside the reconnect window is readmitted

**Result.** Each arm SIGKILLs a member and boots it again at once, still inside the
survivors' reconnect window. **12 of 12 arms readmitted the member, with 0
bugchecks.** The standing stall matrix on the same build is in `stall-matrix.txt`.

The defect is from `tests/lab/captures/vms-af4-unclean-return-20261001/`. There, main
`ff4a7160` never readmitted the returning node in 300 s, and the surviving OVMX node
kept the old incarnation as a MEMBER row. A real OpenVMS VAX V7.3 survivor behaves
differently (that capture's `oracle/`). It accepts the new incarnation's connect,
removes the old incarnation 4 ms later, and admits the new one about 6 s after first
hearing it.

## The change

| | before | after |
|---|---|---|
| A circuit to a known system advertises a different incarnation | the connect is REACCEPTed into the old block, which stays a MEMBER | p. 7-24 DEAD: the old block drops its connection and its removal is proposed at once, or deferred to a transition already running. p. 7-25: a fresh block is built, and lookups by SCSSYSTEMID find the fresh block from then on |
| The `op 0x08` REMOVE open | read as carrying no nodemap, so a participant's phase 2 left membership alone | `body[55]` is read as the members the removal keeps. GROUNDED: spec §4(p).R, nine real specimens from `../vms-af4-op08-remove-20261001/`. Phase 2 deselects the member it omits |
| OVMX as coordinator of a removal | `op 0x08` sent with no nodemap | `op 0x08` carries the kept members' nodemap. OVMX still never originates one toward a non-OVMX connection manager |
| `SHOW CLUSTER` member rows | the walk stopped at the first free CSB slot | holes are skipped. Without this, the new incarnation's block, which sits past the reclaimed old block, was missing from the display |

## The arms (`rig/kr.sh`, `kr-KR.log`)

The founder is VAXC, a real VAX V7.3 under SIMH. OVMXA and OVMXB join it; both run
under QEMU TCG with this branch's artifacts (`7faf24a6`, `cluster_auth_group=257`).
After 45 s as a member, OVMXB's QEMU is SIGKILLed: no departure message and no last
gasp. The same SCSNODE and SCSSYSTEMID is booted again 3 s later.

An arm passes only if all of these hold:
- the returning node prints `this node is now a VAXcluster member`;
- the real VAX logs a new "proposing/proposed addition of OVMXB";
- the final `SHOW CLUSTER` on both OVMXA and OVMXB shows three MEMBER rows;
- no console shows a bugcheck.

| arms | PASS | FAIL | kill → member | VAX bugchecks |
|---|---|---|---|---|
| 12 | **12** | 0 | 26.1–28.1 s (about 20 s of that is the reboot) | **0** |

`KR-1/` has one arm in full. OVMXA's console shows the oracle's sequence:

```
[118.636] %PEA0, channel verified                         <- the new incarnation's circuit
[118.721] %CNXMAN, new incarnation seen, old CSB is dead
[118.726] %CNXMAN, ... the removal this node can build is not grounded for it: the removal is not proposed
[119.944] %CNXMAN, completed VAXcluster state transition  <- the real VAX's removal (op 0x08, nodemap 0x06)
[121.995] %CNXMAN, system 00000000000007c4 was added to the cluster
| 1988   | 00010004 |                 | MEMBER           |  <- readmitted at a NEW CSID, as the b36 oracle does
```

## How the op-0x08 specimens were made (`rig/s4.sh`, `specimen-runs/`)

Four members join in a fixed order: VAXC, then OVMXA, OVMXB and OVMXC. Their CSIDs
are `00010001`..`00010004`, read back from VAXC's own OPCOM lines. One member is
SIGKILLed per run: OVMXC, OVMXA or OVMXB. Each time, VAXC coordinates the removal and
sends `op 0x08` to the two survivors. The frames are trimmed into
`../vms-af4-op08-remove-20261001/` and are the codec specimens
`tests/cluster/host/fixtures/cm-open-remove-s4*.spec`.

## Honest scope

- Run under QEMU TCG, not KVM. The pod is unprivileged, so `/dev/kvm` is not openable.
- A removal of CSID slot 1 was not captured (see spec §4(p).R).
- Only `body[55]`, the class/role/epoch fields, and the foundation time and founder
  are pinned. The other varying bytes of `op 0x08` are listed in §4(p).R as not
  pinned. OVMX reads none of them and asserts none of them toward a real VAX.
