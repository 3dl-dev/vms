# vms-e09: the master's directory-entry removal (op 0x04) and a former master's answer (0xf7)

Registered 2026-10-13, before any held-out set was read for these rules.

## The defect (DC1b, `../vms-94a-cwlnm-oracle-20261013/`)

1. Each time OVMXA used its system disk, it looked up F11B$sOVMXSYS at VAX1, the directory
   node, and was told 0xf9 ("you master it").
2. When the resource went away, OVMXA sent nothing, so VAX1's directory entry kept naming
   OVMXA.
3. OVMXB looked the name up, got 0xf8 naming OVMXA, and asked OVMXA. OVMXA declined
   silently, because it holds no such resource (SS$_UNSUPPORTED), and OVMXB asked again
   every 2 s.
4. A VAX asking for any name OVMX has mastered and freed meets the same silence.

## Corpora

- **Derivation set (read to write these rules):**
  - L1: `/lab/k8s-labs/dlmlab/L1/L1.pcap`. Three VAX V7.3 nodes, VAX1 the only directory
    node, 4881 named op-0x04.
  - For 0xf7 only, L2: `/lab/k8s-labs/dlmlab/L2/L2.pcap`. The only corpus with a 0xf7: 5
    answers, during a remaster.
- **Held out (not read for op 0x04 until this file is committed):**
  - L2's op-0x04 frames (245).
  - `lnm1.pcap`: the vms-94a oracle, three VAX V7.3 nodes with VAX3 booting late. 2980
    op-0x04.

## Rules

**R1, the shape.** A named op-0x04 from master M to directory D for resource N:
- body[4:8] = 0 (no transaction, so nothing to answer)
- body[8] = 0x02, body[9] = 0x04
- body[12:16], the identity body[44:48+body[47]] (group, mode, length, name) and the hash
  body[128:132] each equal the same bytes of the 0x82/0x01 0xf9 answer D last gave M for N

Nothing else in body[10:12], body[16:44] or body[48+len:128] is constant for one entry. For
the most frequent L1 entry, body[20:24] takes 237 values over 394 removals. So these spans
are not an entry identifier. OVMX writes them as zero.

**R2, the pairing.** Every removal from M to D for N follows an 0xf9 answer from D to M for
N, with no other removal for N from M in between. One removal per mastery, never more.

**R3, never answered.** No 0x82/0x04 appears anywhere.

**R4, the former master's answer (0xf7).** A node asked to serve a resource it does not
master answers with the request echoed, except:
- body[0:4], the envelope
- body[8] = 0x82
- body[28:39], with body[34] = 0xf7 and body[28:32] = 0

These are the same rewrite rules as the directory's 0xf8/0xf9 answer, and OVMX writes zeros
in the stale bytes. The opcode is echoed (82/01 for an ENQ, 82/07 for a CONVERT). In L2 the
requester later asks again.

The pass threshold for R1 to R3 on each held-out corpus: R1 and R3 hold for at least 99% of
removals, and R2 holds for every removal whose f9 falls inside the capture.

## What OVMX will do (rd vms-e09)

- **(b)** When a resource this node mastered on an 0xf9 from a remote directory node is
  deleted, send that node the R1 removal. Expect no answer (R3).
- **(c)** An inbound request for a resource this node does not master, and for which it
  holds no master to redirect to, gets the R4 0xf7 answer instead of silence. An OVMX
  requester that receives 0xf7 asks the directory again.
- **(d)** Lab proof on the DC1 topology:
  - VAX1 receives OVMXA's removal for F11B$sOVMXSYS.
  - OVMXB's lookup then gets 0xf9, or gets 0xf8 to a node that serves it.
  - No request goes unanswered, and there are 0 bugchecks.

## Held-out result (`checkrm.py`, run after this file's first commit, 9cd0a7f16)

| corpus | removals | R1 | R2 | R3 | 0xf7, R4 shape |
|---|---|---|---|---|---|
| L1 (derivation) | 4881 | 4881 (100%) | 4872/4880 | 0 answers | none |
| L2 (held out for 0x04) | 245 | 245 (100%) | 217/244 | 0 answers | 4 of 5 |
| lnm1 (held out) | 2980 | 2980 (100%) | 2954/2957 | 0 answers | none |

- **R1 and R3 held** on both held-out corpora.
- **R2 failed in its strict form.** Each miss is a second removal for the same mastery,
  with no lookup in between: 8 in L1, 27 in L2, 3 in lnm1. In L1 and lnm1 every second
  removal is byte-identical to the first apart from the envelope; in L2, 18 of 27 are. So a
  real master sometimes sends the removal twice, and a directory node must accept a removal
  for an entry it no longer holds.
  - What OVMX does: it sends one removal per mastery, which is the shape of the large
    majority. As a directory node it already ignores a removal for an absent entry
    (`vms_dlm_dir_remove`).
- **R4: 4 of 5 0xf7 answers match.** The fifth answers an op-0x07 (82/07), whose request is
  not matched by `checkrm.py`'s transaction key. The CONVERT is not used. OVMX answers 0xf7
  only to an op-0x01 ENQ, the shape the 4 matched answers ground.
  - No held-out corpus contains a 0xf7, because none has a remaster. The lab run (d) is
    where R4 meets a real VAX.
