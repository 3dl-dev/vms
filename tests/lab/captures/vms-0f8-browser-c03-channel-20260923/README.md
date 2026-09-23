# OVMX opens a NISCA channel to a real OpenVMS VAX **V5.5-2H4**

**rd vms-0f8. In-browser cluster demo, headless chromium on `k3s-worker`
(`ovmx-lab/cluster-demo-proof`), 2026-09-23. One variable: the executive now
speaks the class-`0x03` discovery revision.**

## The rig

Identical to the `vms-147` capture next door, with Node A rebuilt from this
item's fix:

| | |
|---|---|
| **Node A** | OpenVMX x86_64 under qemu-wasm (real Linux kernel + `vms.ko`), `SCSNODE=OVMXA`, `SCSSYSTEMID=1987`, `VOTES=1`, `EXPECTED_VOTES=2`, **group 257**. Boot artifacts built by `build-boot-artifacts.yml` from commit `1e2d50b9` (the codec fix), config-injected by `tools/cluster-web-demo/inject-cluster-config.sh` + `inject-ods2-config.sh`. |
| **Node C** | a **real OpenVMS VAX V5.5-2H4** volume on pcjs (KA655), `SCSNODE=VAXC`, `SCSSYSTEMID=1989`, **group 257**, already a one-member VMScluster. Not OVMX, not modified. |
| **Wire** | the demo page's in-page JS L2 hub — a dumb repeater, no relay server. |

## What changed, in one number

`SHOW CLUSTER/LOCAL_PORTS` on Node A, **before** (rd vms-147, V0.7 + the
multicast fix, same rig, same Node C):

    frames tx 208 (errors 0), rx 553 (dropped: nobuf 0, badclass 553)

**553 of 553 received frames unclassifiable.** After (this item, 15-minute run,
`nodeA-run-15min.log` / `result-15min.json`):

    frames tx 2113 (errors 0), rx 1838 (dropped: nobuf 0, badclass 402)

**402 of 1838 — and every one of those 402 is the SAME single frame shape**,
which is not a discovery frame at all. See the census below.

## The channel formed — `b2` → `b3` → `b4`, bidirectionally, in the V5.5 revision

`hubshapes.json` is a census over the whole run of every distinct
`(port, wire length, abs-30/31 word, abs-36 class)` tuple, with the **first and
last** full frame of each shape kept verbatim. The 7-minute run:

| shape | n | what it is |
|---|---|---|
| `OVMXA/120/w30=4113/b36=01` | 594 | OVMX's own §4(g) phase-2 `0x41` START, format `0x13` |
| `VAXC/128/w30=a000/b36=03` | 519 | Node C's multicast HELLO — **the frame that used to be `badclass`** |
| `OVMXA/128/w30=b300/b36=03` | 186 | **OVMX's directed `b3` channel-verify REQUEST, in Node C's revision** |
| `OVMXA/128/w30=a000/b36=03` | 184 | OVMX's own multicast HELLO, now in Node C's revision |
| `VAXC/128/w30=b400/b36=03` | 184 | **Node C's `b4` CONFIRM — the channel is verified, steadily** |
| `VAXC/104/w30=0103/b36=01` | 184 | Node C's SCS envelope — **format byte `0x03`, not `0x13`. The next blocker.** |
| `VAXC/128/w30=b200/b36=03` | **1** | **Node C's `b2` channel-verify REQUEST.** Exactly one, as §4(a).1 grounds |

§4(a).1: a correct exchange is **one** `b2` → `b3` → `b4` and then steady
`b3`/`b4` keepalives. That is precisely the shape of this capture — against a
real 1993 OpenVMS VAX V5.5-2H4 that OVMX had never been able to address at all.

Node A's own report agrees: `MTU 1500, channels 1, circuits 1`
(`nodeA-local-ports.png`).

## Every asserted byte of OVMX's reply is real executive state (INV-6)

The **last** `OVMXA` `b3` of the run, decoded out of `hubshapes.json`:

| abs | value | where OVMX got it |
|---|---|---|
| 22 | `01 01` | **learned** off Node C's own frame (the C03 revision's marker; OVMX's own is `01 00`) |
| 36 | `03` | the revision's format marker |
| 47–67 | `…10 03 00 00 00` | **learned** off Node C's own frame (`0x10` at abs 63; OVMX's V7.3 corpus has `0x18`) |
| 68–71 | **`77 11 7a 7d`** | the **cluster join nonce**, learned live off Node C's `b2`. Never computed from (group#, password) — that hash is unpublished (Rule 8) — and never replayed from a stored capture |
| 92–93 | `01 00` | the incarnation |
| 94 | `90 05` | learned (OVMX's own is `92 05`) |
| 126 | `21 00` | learned (OVMX's own is `26 00`) |
| 24–29 | `aa:00:04:00:c3:07` | **OVMX's own** SCSSYSTEMID 1987 — never the peer's |
| 41–46 | `OVMXA` | **OVMX's own** SCSNODE |
| 120–125 | `52:54:00:00:00:0a` | **OVMX's own** hardware MAC |
| 96–101 | live | OVMX's own monotonic clock |

Every value that *identifies this node* is this node's. Every value that is a
*format marker of the cluster's protocol revision* was read off a frame the
real V5.5 machine actually sent. Node C accepted it 184 times.

## What did NOT happen, and why

**CN=2 did not form.** Both nodes still report one member — Node A lists
`OVMXA` only, Node C lists `VAXC` only (`result.json`, both console tails).

The reason is now a single, precisely-located frame shape:
`VAXC/104/w30=0103/b36=01`, n=184, and it is **all** of the residual
`badclass`. It is Node C's SCS envelope, and it is a **second revision of the
SCS layer** exactly as the HELLO had a second revision:

```
OVMXA START  abs30=41 abs31=13   SCA content 106   ... 12 00 | <16 bytes> | 3e 00 00 00 | c3 07 ...
VAXC  START  abs30=01 abs31=03   SCA content  90   ... 03 00 |            | 3e 00 00 00 | c5 07 ...
```

Byte for byte after that point the two bodies are the **same structure** —
`3e 00 00 00`, the sender's SCSSYSTEMID, `40 02`, an ASCII software-version
string (`"VMS V5.5"` vs `"VMX V0.7"`), a live tick, an ASCII hardware string
(`"VAX "` vs `"X86 "`), `06 00 00 0a`, and the sender's SCSNODE — offset by
exactly the 16 bytes the V5.5 form does not carry. OVMX's whole SCS/CM/DLM
stack is keyed on the format constant `0x13` at abs 31 (`is_scs_envelope()`),
on the Con.ID pair at abs 64/68, and on the §4(h) inner-length identity; all of
those shift in this revision, and this capture holds **exactly one specimen** of
it — Node C's round-0 START, retransmitted 184 times because nothing answered.

That is the next item, and it is a different-sized problem from this one: the
discovery revision could be closed from a passive capture, whereas the SCS
revision needs an iterative live-oracle campaign (answer the START, capture the
reply, decode, answer again) before a connection manager can run over it.

## Files

| file | what |
|---|---|
| `hubshapes.json` | the shape census, first + last full frame per shape — the primary evidence, hashed in `docs/clean-room/reference-captures.sha256` |
| `nodeA-run.log` / `result.json` | the 7-minute census run |
| `nodeA-run-15min.log` / `result-15min.json` | the earlier 15-minute run (`rx 1838, badclass 402`) |
| `nodeA-local-ports.png` | the demo page at the end of the run — `channels 1, circuits 1` |

The directed `b2` in `hubshapes.json` is also the host-test fixture
`tests/cluster/host/fixtures/hello-c3-vaxc-directed-b2.spec`.
