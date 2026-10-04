# rd vms-4fb — where the DLM directory hash rides, and what the vector indexes

Private three-node OpenVMS VAX V7.3 cluster (golden 3-node volume, `VAX1` 1025 /
`VAX2` 1026 / `VAX3` 1027) on its own bridge in a disposable pod
(`ovmx-lab/dlmlab`); no `vaxlab-*` pod and no live cluster touched. Full
captures stay on the lab PVC (`/lab/k8s-labs/dlmlab/L1/L1.pcap`,
`.../L2/L2.pcap`); `dirhash-L1.pcap` here keeps the ten frames the R1
specimens are cut from (`pick.py`).

* **L1**: `VAX1` LOCKDIRWT 3 (the only directory node), `VAX2`/`VAX3` 0, plus a
  scripted `$ENQ` scenario (`DLMTA`..`DLMTG`, MACRO-32 drivers from
  `tests/lab/captures/vms-c03-dlm-opcodes-20260911/dlm-drivers/`) and
  `SDA> SHOW RESOURCE/NAME=` on every node.
* **L2**: `VAX1` 1, `VAX2` 2, `VAX3` 0 — vector `[VAX1, VAX2, VAX2]`.

## Findings

1. **The value is `body[128:132]`, LE u32, of a cat-0x02 op-0x01 request.**
   Over L1+L2 (14237 op-0x01 requests), 939 ROOT resources (request
   `body[36:44]` all zero) each carry exactly one value, from every sender;
   all distinct. The same root name `DLMTA` from `VAX2`, `VAX3` and `VAX1`:
   `e3 6f 33 00` every time.
2. **`body[10:12]` is not the hash** (it was INFERRED from the strawman): the
   same `DLMTA` reads `a3 00` from `VAX2` and `c8 00` from `VAX3`.
3. **Sub-resources carry a parent-dependent value** (`F11B$s<fid>` under two
   parents: two values) — so a value is only learned for a ROOT.
4. **The directory index is the HIGH 16 bits mod n.** L2, the weight-0 node
   `VAX3`'s first request per name: 140/140 root names went to
   `vector[(value >> 16) mod 3]` (the 20 others are all `F11B$s` sub-resources,
   which go to the parent's master with no lookup). Full value 97/160, low half
   80/160, single bytes 85-106/160 — chance is ~94/160.
5. `body[46]` is not a constant marker: 0 on kernel-mode names (892 roots), 1
   on executive-mode (27), 3 on user-mode `DLMENQ` names (20) — the access
   mode qualifying the name. Not decoded further here.

Tools: `hashchk2.py`, `rootchk.py`, `idxrule.py`, `pick.py`.
