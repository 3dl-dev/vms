# rd vms-2ef — the cat-0x86 close response's body[24], pre-registered prediction

Committed BEFORE the held-out run (the commit timestamp is the proof).

## What the derivation showed

Every real close pair in the capture library plus the PVC lab captures
(VAX responders only), tabulated by `close4.py` / the in-pod `close5.py`:

| request body[26] | response body[24] | n | note |
|---|---|---|---|
| 5 | 4 | 264 | the routine close; also answered **3** 165 times -- NOT a function of the request |
| 6 | 5 | 5 + 9 | every MOUNT/CLUSTER and DISMOUNT/CLUSTER close |
| 10 | 5 / 3 | 3 / 5 | not a function of the request |

The derivation run (`derivation-mount-dismount.pcap`, marks in
`derivation-marks.txt`, two-VAX V7.3 cluster in pod vaxlab-3, no OVMX) drove
MOUNT/CLUSTER and DISMOUNT/CLUSTER from each node, with the responder having the
volume mounted and not: all 9 closes carried request body[26] = 6 and were
answered 5. The 2026-10-09 evacuation hang (run ci6-evac-14) was exactly this
close (request body[26] = 6, body[28:30] = 88 00) sent to OVMXE and never answered.

## The prediction

For a cat-0x06 op-0x00 close whose request body[26] is **6**, the responding
member answers cat-0x86 op-0x00 with body[24] = **5**, whoever initiates, whatever
the device, whether or not the responder has it mounted.

Nothing is predicted for body[26] = 5 or 10: they stay unanswered by OVMX.

## Held-out run (to come, different variables from the derivation)

MOUNT/CLUSTER and DISMOUNT/CLUSTER of a DIFFERENT device -- the read-only CD
`$2$DUA2:` (VAXVMS073) -- from VAX2 and from VAX1, three times each, plus one
MOUNT/CLUSTER/NOWRITE of VAX1DATA. Any close with body[26] = 6 answered with
anything but 5 retires the prediction.
