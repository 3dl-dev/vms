# vms-64b: OVMX DLM messages carry the cluster's DLM rebuild sequence (lab proof)

Pod `lab64a` and pod `lab64b` (image `ovmx-vaxlab:7`, `ASYM_MSCP=1`). Each holds VAX1 and VAX2 (OpenVMS VAX V7.3) plus OVMXE.

OVMXE was booted through `tests/lab/tools/ovmx_fire.sh`, which runs the frame-conformance gate first. Artifacts and checks per run:

| run | artifacts | gate tree |
|---|---|---|
| order 1, order 2 | commit ebda48b04 (branch `evac/int-64b` = this PR + vms-239) | that commit |
| control | commit 41d4c1fae (vms-239 without this PR) | that commit |

`tools`: `/tmp/evac/vcg/ncheck.py`-style check. For every DLM frame (cat 0x02/0x82), it compares OVMX's body[12:16] with the value the real VAXes last asserted.

## Order 1: the VAX pair forms, OVMXE joins (`order1-vax-first.pcap`)

The VAX pair formed with N = 2. The coordinator's 0x09 opens to VAX1 and to OVMXE carry [92] = [100:102] = [102:104] = 2 and rebuild 1 (MERGE: OVMXE weighs 0).

OVMXE joined, and VAX1 reports `F$GETSYI("CLUSTER_NODES")` = 3. OVMXE then mounted `VAX1$DUA16386:` (a volume VAX1 serves over MSCP) and typed a file from it.

Results:
- Every DLM frame OVMXE sent (7) carries 0x0001 + N = 2. This equals all 9,804 VAX DLM headers in the run, with 0 mismatches.
- The MERGE did not step N, as the corpus predicts.
- 0 bugchecks on either VAX.

Before this change, the same checker flags all 9 of OVMXE's DLM frames in lab run mscp-239 r20 (header 0x00000000).

## Order 2: OVMXE founds first, then the VAXes boot (`order2-ovmx-first.pcap`)

OVMXE founded alone (EXPECTED_VOTES 1) at 44 s, holding N = 1. VAX1 did not ask OVMXE for admission. At 356 s it sent a FORM open (op 0x07) and formed a cluster of its own, and VAX2 stayed in `waiting to form or join`.

`order2-control-pre64b.pcap` shows that the control (no vms-64b) behaves the same way at 354 s. This failure therefore predates this change: it is the OVMX-founder-admits-a-VAX path, and it is tracked separately. 0 bugchecks.
