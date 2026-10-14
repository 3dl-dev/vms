# vms-9045: the 0b/05 database request names transfer connection 0

**⚠ THE V0.7-9 BUGCHECK'S ROOT CAUSE IS rd vms-f4e (PR #1702), NOT THIS.** That lane got
there first and fixed it: OVMX pulled the clusterwide-LNM database from a member that was
not yet READY (its PARAMS said `body[82]` = 0x2a, not 0x2b), and a real VAX still early in
its own startup died on it. They reproduced it in their own lab with a register dump
matching the demo's and grounded the readiness rule on 54 real pulls.

What is left here, and what this directory is for, is a **second, separate defect in the
same frame** that the readiness gate does not touch: **OVMX names transfer connection 0
where 19 of 19 real requests name a connection.** A ready member will still be asked to
block-transfer onto connection 0. That is rd **vms-9045**.

The crash narrative below is kept because it is the measured context, and because the
ruled-out table and the frame log are the evidence a future arm starts from.

## The crash

The V0.7-9 demo visitor gate (openvmx-site `track-cluster-demo`) failed all eight cases;
three — `c-after-a-up`, `c-last-after-a-up`, `page-order-throttled4` — end in the demo's
real OpenVMS VAX V7.3 taking

```
**** Fatal BUG CHECK, version = V7.3   INVEXCEPTN, Exception while above ASTDEL or on interrupt stack
    Current process = NULL        PC = 8044E27F   PSL= 04080009
```

All three at the **same PC and PSL**: IPL 8, interrupt stack, no current process. PC is
inside `EXCEPTION.EXE` (the dispatcher). The interrupt stack carries `8081CD97`,
`8081D9D5`, `80A02201`, `80A0A26A`, `80A0A283`, `80A0A4D2` — all **above the highest loaded
executive image** (`80556200`), i.e. nonpaged pool: a loaded **driver**. Plus `804CD743` /
`804CD9A1` in `PROCESS_MANAGEMENT.EXE`. So the fault is in the **SCS/port driver receive
path**, not in CNXMAN's own image.

In every crashing case the VAX is the **coordinator**: it formed alone, admitted an OVMX
node, printed `completing VAXcluster state transition`, and died inside a second.

It is a **V0.7-9 regression**: V0.7-8's gate (run 38333892182) failed 2 of 6 cases with
**zero** bugchecks. And it is **intermittent** — the same tag crashes on one run and reaches
CN=3 on the next.

## The frame

`frames-c-after-a-up-BUGCHECK.jsonl` is every 0x6007 frame the demo's in-page L2 switch
carried in a run that **did** bugcheck (the capture option added to the gate on the
openvmx-site branch `work/demo-frame-capture`). Its last frames:

```
100.462  OVMXA  0b/05     <-- the clusterwide logical-name DATABASE REQUEST
100.472  VAXC   02/0d  x5
100.481  OVMXA  82/0d  x5
         ...VAXC never speaks again...
121.820  OVMXB  01/14     <-- 21 s later, Node B boots into a dead cluster
```

In a run that did **not** crash, the same `0b/05` is followed 20 ms later by VAXC's
`8b/05` answer. In the crashing run the VAX **dies instead of answering it**.

## The field

OVMX's `0b/05`, byte for byte:

```
[  0] 81 00 30 01 01 00 06 00 0b 05 00 00 01 00 02 00
[ 16] 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[ 32] e8 91 00 00 ...all zero...
```

`body[20:24]` is this codec's own **`VMS_OFB_CM_XFER_CONID`** — the connection the member is
asked to **BLOCK-TRANSFER** its 0x91e8-byte database onto. `vms_cm_lnm_desc_parse` reads that
same offset as `conid` and **REFUSES a zero one on receive**. OVMX **sends** zero.

`dbreq-conid-census.txt` (from `dbreq_conid_census.py`, over all 120 corpus pcaps): **27 real
`0b/05` requests. 19 carry a NON-ZERO transfer Con.ID; the 8 zeros are OVMX's own.** Every
real-VMS sender names a connection; this executive names connection 0. `body[24:32]` carries
a further non-zero per-request value in the same requests, and `body[16:20]` is zero in all
27.

The frame-grounding table says:

```
cm_lnm_dbreq * 88 16 const const:0   body[16:32]: leftover buffer contents in real
                                     requests (varies request to request), sent as zero
```

**That row is the defect.** A live transfer descriptor varies request to request *because
each request names a different connection*; the census read "it changes" as "it is residue",
and the conformance gate then passed a zero because the rule says `const:0`. This is the
**third** crash of exactly this class — the MSCP buffer name (vms-239 r17), the MSCP
descriptor Con.ID (r19), and now this — and the first two are already written down in
`ovmx-never-crashes-a-peer`: *"values OVMX mints need a grounded form... an invented form is
an ungrounded value, and a real VAX may dereference it."*

## What was ruled out, with evidence

| suspect | ruled out by |
|---|---|
| op-0x14 model count `0x0b` (#1683) | a real VAX1 received it in the vms-6e3 arm (`../vms-6e3-merge-barrier-20261013/`) and proceeded, 0 bugchecks |
| member PARAMS state/quorum/QDSKVOTES (#1667) | 89 real-VAX member PARAMS vs OVMX's: every value OVMX asserts is INSIDE the envelope; the differences are all zeros where a real VAX writes something (`cmparams.py`) |
| the op-0x04 directory removal (#1682) | the vms-e09 arm ran the same topology with it, 0 bugchecks |
| a flood from OVMX | VAXC's own post-commit membership burst is 254/509 frames in the real corpus too — that is real-VAX behaviour, not an OVMX storm |

## The decision (conductor) — now rd vms-9045, no longer the P0

OVMX cannot fill `body[20:24]`: its FORM is not derived — it is **neither** Con.ID the
frame's own SCS envelope carries, and the nineteen real values have no relation this lane
could ground. So the only honest emissions are:

1. **Withhold the `0b/05` entirely** until the field is grounded (INV-6: hold nothing, send
   nothing, count it) — removes the crash, and **costs the clusterwide logical-name database
   pull**, a V0.7-9 headline feature, because that pull's only real use is OVMX asking a real
   VAX. Recommended: a peer crash is release-gating and a feature that worked by fabricating
   a descriptor was never working.
2. Ground the field first (a new arm over the nineteen real requests) and send the real
   value — keeps the feature, but the crash stays live until that arm lands.

Either way: `vms_cm_lnm_dbreq_build` must **refuse a zero transfer Con.ID**, the same refusal
its own parser already applies on receive — the two ends of one codec may not disagree about
what is a usable connection — and the `cm_lnm_dbreq` grounding row must stop calling
`body[20:32]` residue.

## Files

- `frames-c-after-a-up-BUGCHECK.jsonl` — the L2 frame log of a crashing run (1498 frames).
- `VAXC-BUGCHECK.console.log` / `VAXC-nocrash.console.log` — the same case, both outcomes.
- `dbreq_conid_census.py` + `dbreq-conid-census.txt` — the 27-request census.
- `framescan.py` — decode a frames.jsonl by cat/op.
- `cmop.py`, `cmparams.py`, `envelope.py` — the per-byte census tools used above
  (`envelope.py` compares a demo port's emitted bytes against the real-VAX value set for the
  same cat/op, which is how the ruled-out list was built).
