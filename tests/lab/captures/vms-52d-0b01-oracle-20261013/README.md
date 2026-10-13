# vms-52d: the cat-0x0b op-0x01 a booting VAX sends every older member

The predictions in `PREDICTION.md` were registered and committed before each arm ran.

## The problem

In every OVMX-founded lab run, each booting real VAX sent OVMXE one cat-0x0b op-0x01, and
OVMX logged it as unroutable. OVMX never answered, and no VAX reached `Username:`. The frame
looked like a role-0x10 form, with body[16:19] = `10 02 01`, that no VAX ever sends to a VAX.

## Arms (real OpenVMS VAX V7.3 only, no OVMX node; image `ovmx-vaxlab:7`, `oracle_stage.sh`)

| arm | pod | founder (SYSBOOT>) | later | result |
|---|---|---|---|---|
| A | lab52da | VAX1 1025: VOTES 1, EXPECTED_VOTES 1, LOCKDIRWT 0 | VAX2 and VAX3 together | H1 (LOCKDIRWT) refuted: no role form to VAX1; 3 members, 0 bugchecks |
| B | lab52db | VAX3 1027 (the highest, like OVMXE 1030): the same parameters | VAX1, then VAX2 | VAX3 answered both with 1, which refutes R-id; 3 members, 0 bugchecks |

`armA-cm.pcap` and `armB-cm.pcap` keep the cat-0x01 and cat-0x0b connection-manager
messages (`trimcm.py`). `rm380-cat0b.pcap` and `rm380-ds3-cat0b.pcap` are the cat-0x0b
messages of the vms-380 three-VAX remaster captures.

## What the frame is

`stale.py`: the request writes only body[8:12], [24:28] (0) and [28:32] (1). The rest of the
buffer is whatever the last message on that connection left there. Toward OVMX, that is OVMX's
own 0x81/0x0b barrier ack, which explains `10 02 01`. Toward VAX1 at the same instant
(db1-r2 302.103), it is VAX1's preceding message. So the frame is the ordinary request each
booting VAX sends every member that is older than it, and OVMX was the only member that
never answered.

`follow.py`: a VAX whose request went unanswered sends nothing more on that connection. In
db1-r1, r2 and r3 the VAX that got VAX1's answer and none from OVMX went silent. In every
all-VAX run the requester goes on to cat-0x0b op-0x04 with each member.

## The answer (`pair0b.py`, 31 real pairs)

Echo the request verbatim, then set body[8] |= 0x80 and write one value to both body[28:32]
and body[36:40]. Nothing else differs in any pair. The value is 1 when the answerer became a
member before the requester (21 pairs), and 0 when it became a member after (7 pairs). In the
other 3 pairs the request already carried the answer's bytes. Arm B separates this from
"lower SCSSYSTEMID answers 1", because the founder held the highest SCSSYSTEMID and still
answered 1.

## Specimens

`tests/cluster/host/fixtures/cm-0b01-{req,resp}-{older,newer}-oracle.spec` (`mkspec0b.py`):
- older: arm B, VAX3 answers VAX1 with 1.
- newer: rm380 ds3, VAX2 answers VAX1 with 0.

## Fire F1 (pod lab52dfa, build lab/int-52d 02f5785c9, artifacts run 38346183373)

The topology was OVMX first: OVMXE (1030, EXPECTED_VOTES 1) founded alone, and VAX1 and VAX2
booted 300 s later. OVMXE admitted both VAXes, giving CN = 3. 0 bugchecks.

| prediction | result |
|---|---|
| F1: each 0b/01 is answered with 1 | **held**. VAX2's 0b/01 was answered in 2 ms with body[28:32] = body[36:40] = 1. Its kind-10 and kind-5 closes were also answered (5 and 4). |
| F2: both VAXes finish startup | **failed** at the point registered as F3 |
| F3: a cat-0x0b op-0x04 to OVMXE goes unanswered | **happened**. VAX2 sent OVMXE one 0b/04 3 s after the 0b/01 answer, which was OVMX's one unroutable frame. VAX2 got as far as DECnet and CSP. VAX1 never reached its own 0b/01. Both stopped at 04:43:57 with no Username:. |

`unans.py` shows that VAX2's 0b/04 to OVMXE was the only request left unanswered in the
whole capture.

### What op 0x04 is (`xfer04.py`, `rec04.py` on the full arm A/B pcaps)

Op 0x04 is a CLUSTERWIDE LOGICAL NAME. The request names a 232-byte buffer ([16:24] the
buffer descriptor, [32:36] the length 0xe8 or 0xf8). Every real answerer first REQUEST
DATAs it (SCA 0x08, then DATA 0x0d) and only then sends 0x8b/0x04. The record is a
logical-name definition for `LNM$CLUSTER_TABLE`. During startup each VAX defines
`CLU$ICC_ORBS_<node>` there, which carries the text `..._INTRUSION_`. So cat 0x0b is the
clusterwide logical-name service. Op 0x01 is the startup query to every older member, and
op 0x04 propagates one definition to every member. OVMX has no clusterwide logical-name
table (compat row cluster-logicals: absent). That is the next rung: rd vms-52d stays open.

Files: `f1-ovmx-first-cm.pcap` (cat 0x01/0x06/0x0b), `f1-{ovmxe,vax1,vax2}-console.log`.
