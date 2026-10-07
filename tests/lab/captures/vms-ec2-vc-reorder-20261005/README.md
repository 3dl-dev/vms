# vms-ec2: a real V7.3 port accepts sequenced frames that arrive out of order (2026-10-05)

Raw evidence plus the scripts that reduce it. This grounds one transport behaviour that
design section 3.2.5 ruled on without an oracle ("Receive window = 1 ... a selective-repeat
window would be an optimization with no oracle").

## Why this was measured

In the cluster lane's stall rig (real VAXC plus OVMXA/OVMXB), OVMXB's tap carries
`tc netem delay 80ms 40ms distribution normal`, the jitter the rd vms-e88 rig inherited.
OVMXA's tap carries none. In every archived arm (39 of them, `retx.py` in the lab), the
real VAX retransmitted about 30 % of its sequenced frames to OVMXB and **0** to OVMXA.

`ovmx-B3-7-s8.pcap` is one of those arms (b-then-a, 13 s stall). `vcdump.py` on it shows
what happens:
- The VAX sends seq 33 and seq 34 one millisecond apart.
- netem delivers 34 first.
- OVMXB scores 34 as a GAP, discards it and re-acks 32 (`h_vc_rx_gap`, go-back-N), then
  takes 33.
- 34 is recovered only by the VAX's own retransmit timer about 3 s later.

`pairre.py ovmx-B3-7-s8.pcap b18e df0b`: 94 back-to-back pairs, **44** with the second
frame later retransmitted. Toward OVMXA (no jitter): 68 pairs, 0.

Each reorder costs OVMXB about 3 s. This shows up in two places:
- **The join drags:** the 0x01/op-06 membership records are retransmitted in pairs every
  3 s.
- **The post-stall DLM rebuild drags:** the op-0d stream runs at about one record per round
  trip.

That is the time in which the VAX takes no action on OVMXA's membership requests (rd vms-ec2).

## The oracle: the same jitter in front of a REAL VAX

The dlmlab real-VMS lab (three V7.3 SIMH nodes on a private bridge `brdl`; `r2.sh`):
1. vax1 founds.
2. vax2 joins clean.
3. `tc qdisc replace dev tapd3 root netem delay 80ms 40ms distribution normal` puts the
   jitter in front of vax3.
4. vax3 boots and joins.

There are two captures:
- `R2.pcap`, on the bridge, in the order the senders transmitted.
- `R2-rx3.pcap`, on `tapd3`, which is after netem: the order **vax3 actually received** them.

### Results

| measure | real VAX3 behind the jitter | OVMXB behind the same jitter |
|---|---|---|
| back-to-back pairs whose 2nd frame was later retransmitted (`pairre.py`) | **0 / 147** (from VAX2), **0 / 16** (from VAX1) | **44 / 94** |
| sequenced frames that arrived ahead of an earlier one (`reord.py R2-rx3.pcap 28a1`) | 67 | n/a (OVMX captures are bridge-side) |

### What vax3 does with an out-of-order arrival (`R2-rx3.pcap`)

| arrived at vax3 (in this order) | vax3's next acknowledgement to that sender |
|---|---|
| VAX1 seq 3, then seq 2 | 0x48 ack=3 |
| VAX1 seq 5, 4, 6 | 0x48 ack=6 |
| VAX1 seq 8, then 7 | 0x48 ack=8 |
| VAX2 seq 4, then 3 | 0x48 ack=4 |
| VAX1 seq 13, 14, 12 | 0x48 ack=14 |
| VAX1 seq **17, 16, 15** (three, fully reversed) | 0x48 ack=17 |

A real V7.3 port **keeps** a sequenced frame that arrives ahead of the next expected one,
up to at least two ahead. When the hole fills it acknowledges cumulatively through the
highest contiguous sequence number, and the sender never has to retransmit the early frame.
OVMX's receive window of 1 discards it.

The capture cannot show whether vax3 also sends an immediate duplicate ack when the early
frame arrives. Its acknowledgements arrive batched about 1 s later on this slow (noasynch)
SIMH configuration.

### Not established here
- **The size of the real receive window** beyond "at least 3 in flight, 2 ahead".
- **Whether vax3's join completes under the jitter.** It did not complete within the
  script's 500 s in either R1 or R2. vax2's clean join on this noasynch lab took 7 minutes,
  so that is not attributed to the jitter.

## Files

- `R2.pcap`: bridge capture (ethertype 0x6007), whole run.
- `R2-rx3.pcap`: `tapd3` capture, the receive order at vax3. Its tail after netem was
  removed is ordinary traffic.
- `vax1.log`, `vax2.log`, `vax3.log`: the three consoles.
- `ovmx-B3-7-s8.pcap`: the OVMX stall-rig arm (bridge capture).
- `r2.sh`: the scenario.
- `vcdump.py`, `pairre.py`, `reord.py`: the reductions quoted above.
