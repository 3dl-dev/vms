# rd vms-b98 — the PE listen timeout of a real OpenVMS VAX V7.3, measured

**Question.** How long does a real V7.3 PEDRIVER hear nothing from a peer before
it closes the virtual circuit, and does a SYSGEN parameter set it?

**Answer.** It closes **8.15–9.30 s after the last frame it heard** from that
peer (n = 24, mean 8.80 s, median 8.82 s). That is an 8 s threshold checked on
the port's own periodic tick. No SYSGEN parameter observably governs it.

## The lab

Two real OpenVMS VAX V7.3 nodes (SIMH MicroVAX 3900, `VAX1` SCSSYSTEMID 1025 on
root SYS0, `VAX2` 1026 on SYS1, cluster group 1) formed into one cluster on a
private bridge inside a disposable `ovmx-lab` pod (`b98lab`), restored from the
`.3node-golden.bak` disks. No OVMX node was on the wire. `RECNXINTERVAL` 20,
`TIMVCFAIL` 1600, and `BUGREBOOT` set to 0 on both nodes, so a CLUEXIT halts at
`>>>` and cannot auto-boot the wrong root on the shared disk.

Each console was timestamped line by line with the host clock at 10 ms
resolution (`rig/tstail.py`), and the bridge was captured (`ether proto
0x6007`). Silence is measured from the **last frame on the wire from the peer**
to the instant the survivor's console printed `%PEA0, Port has Closed Virtual
Circuit` / `%CNXMAN, lost connection to system <peer>`.

## Method 1 — both directions dark (`rig/darkm.sh`, 8 trials)

`ip link set tap2 down` for 10 s, then up. Both nodes keep running; each one
hears nothing from the other. Before every trial the script checks VAX1's own
`SHOW CLUSTER` and aborts unless VAX2 is a `MEMBER` (two earlier runs were void
because VAX2 had already left). Every trial re-established (`%CNXMAN,
re-established connection`); nobody was removed.

| trial | VAX1 detects VAX2 | VAX2 detects VAX1 |
|---|---|---|
| T1 | 9.124 s | 8.500 s |
| T2 | 8.152 s | 9.153 s |
| T3 | 8.853 s | 8.959 s |
| T4 | 9.158 s | 8.548 s |
| T5 | 9.163 s | 8.271 s |
| T6 | 8.944 s | 9.221 s |
| T7 | 8.675 s | 8.307 s |
| T8 | 8.615 s | 9.156 s |

Per-trial lines, with the survivor's own directed frames during the silence:
`analysis/darkC.vax1.txt`, `analysis/darkC.vax2.txt`.

The closure lands on the port's periodic tick. In T1, T4, T5 and T7 the survivor
sent a `b3` channel-verify request to the silent peer within 15 ms of closing
(`9.11:b3` / close 9.124, `9.15:b3` / 9.158, `9.16:b3` / 9.163, `8.67:b3` /
8.675): the same timer that probes the channel is the one that gives up on it.

## Method 2 — the peer's emulator SIGSTOPped (2 clean trials)

The archived `vms-8c54` fault. VAX1 closed **8.791 s** (12 s stall) and
**8.646 s** (10 s stall) after VAX2's last frame. SIMH did not survive the
resume in this pod — the stopped VAX hung on wake and was removed — so this
method was not repeated; the survivor-side measurement it gives agrees with
method 1.

## Is it a SYSGEN parameter?

* `SYSGEN SHOW` lists no parameter named for it. The only PE parameters are
  `PE1`..`PE6`, all 0, unit blank (`PE1`..`PE4` dynamic).
* `SCACP SHOW CHANNEL/ALL` counts listen timeouts (`Timeouts: Listen`) but shows
  no value; `SDA SHOW PORTS` shows none for `PEA0`.
* **`PE4` = 20 on VAX1 only** (`USE ACTIVE` / `SET PE4 20` / `WRITE ACTIVE`),
  three 25 s dark trials (`analysis/darkPE4.*`): VAX1 still closed at 8.585,
  8.795, 8.925 s, against VAX2 at 8.285, 9.178, 9.301 s with PE4 0. No effect.
  `PE4` was set back to 0 afterwards.

So the timer is a port constant, not something the operator configures, and OVMX
holds it as one: `PE_LISTEN_TIMEOUT_DEFAULT_MS` 8000.

## What does come from SYSGEN

`TIMVCFAIL`: `SYSGEN SHOW TIMVCFAIL` on VAX1 prints `1600 1600 100 65535 10Ms D`
(current, default, min, max, unit, dynamic). OVMX's executive loaded TIMVCFAIL
but never handed it to the port; it now does, converted out of the 10 ms unit
(`cluster_sysgen_timvcfail_ms`), and the SYSGEN store's minimum is 100 as on V7.3.

## The HELLO cadence (no parameter either)

VAX1's multicast HELLO intervals over the run: n = 328, min 1.539 s, median
2.250 s, max 3.162 s. OVMX's 2000 ms beat is inside that range.

## Files

* `oracle-trials.pcap.gz` — every 0x6007 frame from 5 s before to 15 s after
  each of the 11 trials (`rig/trimpcap.py`).
* `vax1.console.tlog`, `vax2.console.tlog` — the timestamped consoles.
* `rig/darkC.out`, `rig/darkPE4.out` — the injection epochs.
* `rig/measure.py` — the analysis (last frame from the peer before the fault →
  first closure line on the survivor's console).
