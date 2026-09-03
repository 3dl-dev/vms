# E56: channel VERIFIED, circuit never opens (2026-09-03)

Companion note for `e56-scs-vc-stall-20260903.pcap` (225 816 bytes, 1 430 SCA
frames, `ether proto 0x6007` on vaxlab-2's `br0`). Tank name of the same file:
`join-e55refire-1788460304.pcap` — this is the E55 re-fire capture, kept in
tree because it is the *negative* half of the controlled pair that grounds
spec §4(a).2.

sha256 `67cca7442727ff9fbdf7ea16e1885674b0054f308633a130fb644643d7b64b53`.

## Run

- Pod `vaxlab-2` (ns `ovmx-lab`), OVMX joiner `OVMXJ1`, SCSSYSTEMID 1986,
  Ethernet source `52:54:00:00:00:f4`, cluster group 257.
- Live 2-node reference cluster throughout: VAX1 (`aa:00:04:00:01:04`,
  SCSSYSTEMID 1025) and VAX2 (`08:00:2b:1e:85:61`, 1026). Both re-polled
  after the run: MEMBER, CN 2, no OVMXJ1.

## What the wire says

E55 is confirmed fixed: OVMX's directed HELLOs carry the live-learned join
nonce `ee 05 39 5b` byte-exact, and the §4(a).1 verify ladder completes in
BOTH directions with BOTH VAXes —

| SCA | frame |
|---|---|
| 50 | VAX2 → OVMX `b2` INIT |
| 51 | OVMX → VAX2 `b3` |
| 52 | VAX2 → OVMX `b4` CONFIRM |
| 53 | OVMX → VAX2 §4(k) padded 1500-byte `b3` |
| 54 | VAX2 → OVMX `b4` |
| 66–71 | the same ladder with VAX1 |
| 107 / 121 | VAX2 / VAX1 each send their OWN padded 1500-byte `b3`; OVMX answers `b4` (108 / 122) |

Both VAXes then keep the `b3`↔`b4` keepalive oscillation going for the whole
capture. The channels are VERIFIED.

**And then nothing.** Census of `0x41` START/config frames in this capture:

| sender | STARTs sent | to |
|---|---|---|
| OVMX `52:54:00:00:00:f4` | **242** | 122 to VAX2, 120 to VAX1 |
| VAX1 | **0** | — |
| VAX2 | **0** | — |

242 OVMX STARTs, byte-identical, re-sent every ~0.64 s for the length of the
run, and **zero** replies of any SCS class. No virtual circuit, `circuits=0`.
(The four 1500-byte frames the first pass read as "bulk `0xb3` replies" are
not SCS at all — they are the §4(k) padded channel-size HELLOs above; `0xb3`
is the abs-30 channel-verify word, and those frames' abs 31 is `0x00`, not the
`0x13` SCS format byte.)

## Why (the E56 verdict)

The member, not the joiner, opens the circuit. `ovmx-5fe-channel-formed-
20260728.pcap` shows the rule cleanly: VAX1 `b2` → OVMX `b3` → VAX1 `b4`, and
**10 ms after the b4** VAX1 emits its own round-0 `0x41` START, unprompted,
18 times. In this capture the identical ladder completes and no START ever
comes.

The byte-diff of OVMX's directed HELLO between the two runs leaves exactly
three things: the two node identities, the abs 96–101 live tick, and
**abs 47–67**, the discovery-format span. `ovmx-5fe` carries
`00 80 01 ff 83 00 04 00×9 18 03 00 00 00` there; this run carries **zero**.
So does nothing else on the wire that the member reads before it opens a
circuit. Census over 10 captures: 11 403 of 11 575 HELLOs carry that span;
the only 172 residuals are this build's zeros. Grounded and written up in
spec §4(a).2; fixed by `pe_learn_disc_format()` (learn it off a real peer,
never bake it).

## Still open after E56 (next wall, pre-diagnosed)

OVMX's own `0x41` START body carries two zeros a real joiner does not:
abs 72–79 software version (8 zero bytes; the OVMX build that reached MEMBER
sent `"VMX V0.1"`, every VAX sends `"VMS V7.3"`) and abs 95 CLUSTER_CREDITS
(0; every real START carries 10, §4(g) grounds it 28/28). Neither has an
executive source in `kernel-core` today — see the DISCLOSED GAPS block in
`src/kernel-core/vms_pe.c`. Expect these to bite as soon as the member's
round-0 START starts arriving.
