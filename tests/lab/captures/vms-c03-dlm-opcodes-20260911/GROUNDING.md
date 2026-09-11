# vms-c03 / FC-P5.2 — cross-node DLM $DEQ / BLKAST / LVB wire opcodes, grounded

**Captured 2026-09-11 on a private 2-node real-OpenVMS VAX 7.3 VMScluster**
(own-lab: `vaxlab-4`, scaled from `sts/vaxlab`; shared oracle untouched — conductor
ruling Option B, `rd vms-c03`). Executive-backed: every field read from the real
wire and correlated to real lock state, never templated
([[executive-backed-not-wire-plumbing]]).

These captures ground the three cat-0x02 DLM wire opcodes the arm previously
refused-with-a-counter (`releases_no_wire_op` / `blkasts_no_wire_op` /
`lvb_write_no_wire_field` in `src/kernel-core/vms_dlm_scs*.c`). They feed the
codec field-map items: DEQ→`rd vms-fa7`, BLKAST→`rd vms-002`, LVB→`rd vms-858`.

## The opcode findings (all real-captured, master_lkid-correlated)

| op | meaning | evidence (pcap / dlm_body frame) | correlation |
|----|---------|----------------------------------|-------------|
| **0x03** | **$DEQ** (lock release) | `dlm-deq-20260911.pcap` f14, VAX1→vax2 | master_lkid `0x3a0004eb` == my ENQ f12 (res `OVMXDEQ1`) |
| **0x06** | **convert-down carrying the LVB** (value-block write) | `dlm-lvb3-20260911.pcap` f14, VAX1→vax2 | master_lkid `0x2b000489` == my ENQ f12 (res `OVMXLVB3`); the 16-byte pattern `WROTEBYVAX1XXXXX` appears verbatim on the wire (pkt offset 108) |
| **0x04** | **BLKAST** (blocking AST, master→remote holder) | `dlm-blk2-20260911.pcap` f58, vax2→VAX1 | master_lkid `0x590004e3` == my EX holder f18 (res `OVMXBLK2`) |

### Guards for the field-map items (conductor, 2026-09-11)
- **vms-fa7 (DEQ):** 0x03 is a **collision to RESOLVE**, not just a new opcode. The
  codec had a *provisional* "commit 0x03"; the real wire uses 0x03 for **DEQ**
  (captured). Either re-ground commit's real opcode from the wire, or confirm the
  provisional "commit 0x03" was a phantom and 0x03 is really DEQ. One opcode = one
  real meaning.
- **vms-858 (LVB):** distinguish **op=06 (convert-DOWN that writes the LVB)** from
  **op=07 (up-convert)** — same convert family, different semantics. The value
  block is at **LKSB+8** (the LKSB is 24 bytes; `$ENQ` has *no* VALBLK keyword),
  and the write path requires `LCK$M_VALBLK` on the value-block-aware `$ENQ` — this
  is part of the field-map, not just the opcode.
- **BLKAST (vms-002):** the meaningful value is *which lock/CSID* the master is
  blocking-AST-ing — read it from the real frame + SDA-correlate (holder ≠ master),
  not from the master's local state alone.

## Capture method (reproducible; the ac4 one-variable-diff + SDA discipline)

1. Private 2-node cluster: `kubectl -n ovmx-lab scale sts/vaxlab --replicas=N+1`,
   use ordinal `vaxlab-N`; both nodes `F$GETSYI("CLUSTER_NODES")==2`.
2. Build the MACRO-32 drivers (in `dlm-drivers/`) on vax1 into
   `SYS$COMMON:[SYSEXE]` (shared dual-ported system disk → both nodes see them).
3. **Master on vax2:** `SPAWN/NOWAIT/INPUT=NL: ENQ 0<2><resname>` places a NL
   holder on vax2 so vax2 masters the resource → vax1's ops cross the wire.
4. Drive the scenario from vax1 under
   `tcpdump -i br0 -w <pcap> -U -s 0 'ether proto 0x6007'`:
   - **DEQ:** `ENQ 4<0><res>` (PW, variant 0 = ENQ then $DEQ).
   - **LVB write:** `LVB3 5<res>` (EX ENQ w/ `LCK$M_VALBLK`, populate LKSB+8,
     convert-down EX→NL w/ `LCK$M_VALBLK`).
   - **BLKAST:** vax1 `BLK 5<res>` (EX holder w/ a BLKAST AST routine, hibernates),
     then vax2 `ENQ 5<2><res>` (queuing EX contender) → master vax2 sends the
     blocking AST to the remote holder vax1.
5. Decode with `tools/cluster/dlm_body.py <pcap> --all`; correlate each frame's
   `master_lkid` to the driving `$ENQ`, and to `SDA> SHOW LOCKS`.

## Driver source (`dlm-drivers/`)
Recovered from the ac4 session console tail (the source was **lost from git**,
`rd vms-f7a`) and extended for this item. `DLMENQ`/`DLMCVT` are the recovered
originals; `DLMLVB3` (value-block write via convert-down) and `DLMBLK` (EX holder
with a BLKAST AST routine) are the new extensions. Committed here so they are
never lost again.

Raw pcaps in this directory are the grounding evidence. Derived spec-composed
fixtures for the codec are produced by vms-fa7 / vms-002 / vms-858.
