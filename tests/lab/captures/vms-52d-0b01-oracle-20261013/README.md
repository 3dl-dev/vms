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
