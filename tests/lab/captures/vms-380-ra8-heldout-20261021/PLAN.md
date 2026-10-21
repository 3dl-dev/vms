# vms-380 RA8: the fresh held-out fire for the remaster codec

**Registered 2026-10-21, BEFORE the pod exists and before any byte of this capture
is taken.** Scored by running the COMMITTED codec
(`src/kernel-core/vms_cluster_codec_dlm.c` at the branch head below) over the fresh
frames — not by eye, and not by a script written after the capture.

Branch: `work/vms-380-remaster-codec`, off `origin/main` `c9dd5e1fe`.
Codec rules under test: `vms_cluster_codec_dlm.h`, "LOCK-TREE REMASTERING ON THE WIRE".

## Why a fresh capture at all

The derivation set is RA5 (`vms-50d-ra5-14cells-20261013`) and the wider corpus; the
held-out set used in `test_codec_remaster.c` is RA6, **a capture this lane had already
read**. RA6 is what caught `body[32:34]` — so it earned its keep — but it is not a fire
taken after the predictions were written down. `heldout-proof-must-vary-topology` wants
both: a prediction committed first, and a topology that is not the derivation's.

## The arm

VAX ONLY. **No OVMX node, so OVMX emits nothing at a real VAX in this fire** — this is
an oracle read, not a conformance fire, and the `ovmx_fire.sh` gate does not apply.

Own pod in `ovmx-lab` (`requests == limits`), image `ovmx-vaxlab:7`, deleted after the
run. Three real V7.3 nodes on the pod's own `br0`: vax1 (root SYS0) and vax2 (SYS1) from
the entrypoint, vax3 (SYS2) staged with the committed
`tests/lab/tools/oracle_stage.sh node vax3`. Holder program: `RMHOLD.MAR` as committed
in `vms-50d-ra5-14cells-20261013/`.

**TOPOLOGY VARIED FROM BOTH PRIOR ARMS.** RA5 was O=VAX1 N=VAX2 H=VAX3; RA6 was
O=VAX2 N=VAX3 H=VAX1. This arm drives **O=VAX3, N=VAX1, H=VAX2** — the one rotation
neither arm has taken — by putting the tree's first holder on VAX3 (so VAX3 masters it)
and the activity on VAX1.

Tree name: `RMT8`. Activity remaster (RA1/RA1b measured that the SHUTDOWN path remasters
only file-system trees), triggered as RA2/RA4/RA5 did: the master's own holder stops
while a remote holder is active.

## Predictions, scored PASS/FAIL individually

**P1 — the sequence.** For the tree that moves, the capture contains, in this order:
O→N `0x0e`, `0x0f`, `0x10`; O→H `0x0f` and `0x11`; one `0x12` per lock from each holder
(root first, then its sub-resources) to N; a `0x13` from each sender to N; then N→O
`0x14` and N→H `0x14`.

**P2 — who is answered.** Every `0x0e`, `0x0f`, `0x10` and `0x14` carries a non-zero
transaction at body[4:8] and is answered cat `0x82` op `0x15`. Every `0x11` and `0x13`
carries body[4:8] == 0 and is answered by nothing.

**P3 — RULE 1, byte-exact, by the committed builder.** For every answered step,
`vms_dlm_rm_ack_build(request)` equals the real answer over body[4:132], with
`new_master = 1` only for the op-0x0f addressed to N. **Expected: 100%.** Any mismatch
is reported offset by offset.

**P4 — RULE 2, byte-exact, by the committed builder.** For every `0x12`,
`vms_dlm_rm_lock_answer_build(request, {master_lkid, valblk, valblk_seq read out of the
real answer})` equals the real answer over body[4:132] **except** body[30:32] and
body[32:34], the two cells the codec does not name. **Expected: 100% outside those four
bytes.**

**P5 — the root/sub split.** Every `0x12` whose body[36:44] is zero is answered with
body[30] in {0xf0, 0xf1}, body[31] == 0x01, and a NON-ZERO 16-byte block at body[36:52]
plus a non-zero sequence at body[52:56]. Every `0x12` whose body[36:44] is non-zero is
answered with body[30:32] == 00 00 and body[36:56] all zero.

**P6 — the unnamed cell, falsifiable.** body[32:34] of the `82/01` answers is the SAME
value across every answer of this one remaster, and it equals the low 16 bits of this
remaster's N→O `0x14` body[20:24] to within 16. (RMT6: 0x0365 vs 0x0364. RMT7: 0x036e
vs 0x036e. RA2: 0x07b8 vs 0x07b8. RA5: 0x0001 for three answers and 0x019e for the
fourth, vs 0x019e — so RA5 already fails the "same across every answer" half, and this
prediction is scored in two parts.)

**P7 — RULE 3, the residue.** Every `0x14` carries body[8:10] == `02 14` and
body[40:44] == 0. At least one `0x14` in this capture carries body[34] == 0xf9 **or**
body[28] != 0x10 — i.e. the RA6 role rule ("0x10/0xfa to the old master, 0x80/0xf9 to
the holder") does not hold here either. Each `0x14`'s body[20:40] is classified by
`tools/cluster/remaster_census.py` into stale-grant / stale-directory / neither.

**P8 — the op-0x12 record cells against SDA.** For the ROOT record each holder sends:
body[24:28] is a lock id `SDA SHOW RESOURCE/NAME=RMT8` listed on THAT holder before the
move, and body[20:24] is an EPID of that holder's `RMHOLD` process (`SHOW SYSTEM`).
For each sub record: body[36:40] is the parent's holder lock id and body[40:44] is the
`82/01` answer's body[20:24] for the parent record.

## What gets committed

`README.md` with every prediction scored honestly (including any that fail), the trimmed
pcap, the SDA truth before and after on all three nodes, the console logs, and the
scoring program. If a prediction fails, the codec is corrected or the cell is
re-classified **before** this item closes — a failed prediction is the result, not a
reason to re-cut the prediction.
