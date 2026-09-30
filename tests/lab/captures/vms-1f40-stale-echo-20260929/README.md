# rd vms-1f40 — the stalled node that never re-formed, and the three defects behind it

After rd vms-8c54 closed the CNXMGRERR down to one surviving shape, the stall rig
(`/lab/run-s8`, pod `ovmx-lab/s8lab`: a real OpenVMS VAX V7.3 `VAXC` founds, OVMX
`OVMXA` joins, OVMX `OVMXB` joins and is SIGSTOPped for N seconds the moment it
prints "this node is now a VAXcluster member") still failed about half its arms
without any bugcheck: the stalled node woke, and each side eventually removed
the other. rd vms-18a carried that as "a stalled node does not always re-form
its circuit". This directory is the root cause, measured.

**No pcap is committed** (`docs/clean-room/PROVENANCE.md`). Every file here is a
decode printed through offsets already grounded in
`docs/cluster-protocol-spec.md`: abs 30 message type, abs 36 the §4(i).B
incarnation echo (§4(h)(4c)), abs 58 config round, abs 92 the directed-discovery
advertisement (§4(i).B).

## The two frames that separate a failing arm from a passing one

`M2-19-fail-wake.txt` (13 s stall, art/mB) and `M2-7-pass-wake.txt` (13 s
stall, the same build), OVMXB (`df:0b`) ↔ VAXC (`91:86`), seconds after wake:

```
FAIL  +1.320 VAX  b2  adv=2            the VAX now advertises 2 for OVMXB
      +1.417 VAX  b4, 0x41 START       its b4 and its re-formation START
      +1.489 B    0x41 START  a36=1    <- stamped with the DEAD generation's 1
      +1.489 B    0x41 STACK  a36=1
      ... B re-sends the STACK every 0.64 s, the VAX re-STARTs every 5 s,
          never ACKs, and ~20 s later each side removes the other.

PASS  +1.437 VAX  b2  adv=2
      +1.544 VAX  b4, 0x41 START
      +1.675 B    0x41 START  a36=2    <- the number the VAX advertises now
      +1.720 VAX  0x41 STACK / B 0x41 ACK -- open in 55 ms
```

## Why

§4(h)(4c): the echo is a property of the CIRCUIT, read at formation from the
peer's directed discovery frame and stamped on every frame of that circuit.
OVMX took it in exactly one place, `h_vc_channel_up()` — our channel reaching
b4. But a formation also starts when the **peer's START** reaches the circuit,
and `h_vc_rx_start()` never re-read it. Which of the two gets there first after
a stall is a race nothing controls: when the b4 wins the circuit is stamped
fresh (M2-7), when the START wins it keeps the number its previous generation
was formed with (M2-19). The VAX discards 0x41 frames that carry an echo it did
not advertise — the same discard `E66` measured for sequenced frames.

**Oracle.** `../vms-8c54-stalled-guest-20260928/analysis/oracle-vc-fields.txt`
is two real V7.3 nodes re-forming after the same SIGSTOP: both sides stamp
`echo=2` — the new generation — on every START, STACK and ACK.

## The census: every failing arm, and only failing arms

`M2-echo-census.txt` runs `echocheck.py` over all 21 arms of matrix M2 (art/mB,
the build PR #1327 started from). Of 20 injected arms, 12 failed:

* **11 of them stamped the stale echo on 100% of their post-wake 0x41 frames**
  (61–82 frames each);
* the 12th, M2-5, stamped none — it is the one bugcheck, the send-msg# hole the
  PR's own first commit closes;
* **every passing arm stamped 0 stale frames.**

## The fix

`vc_take_echo()` (`src/kernel-core/vms_pe_fsm.c`) runs at every formation start:
CHANNEL_UP, the peer's START, and a circuit a START creates. R1:
`test_a_peer_started_formation_takes_the_new_echo` drives M2-19's exact order
(circuit up at 1, b2 advertising 2, START before b4) and asserts every 0x41 out
carries 2; negctl `pe-peer-start-keeps-dead-echo` disarms the START-side take
and reddens exactly that test.

## The verification matrix

Each build was run on the rig with the same duration mix (13/14/20/10/16/6 s,
repeated). An arm whose injector never fired grades NOFAULT and counts for
nothing either way. `echocheck.py` / `who_dials.py` / `crossing.py` re-derive
every per-arm fact below from `runs/<arm>/` on the rig.

| matrix | build | injected | PASS | FAIL | NOFAULT | VAX bugchecks | stale-echo arms |
|---|---|---|---|---|---|---|---|
| M2 (before) | `25892f62` | 20 | 8 | 12 | 1 | 1 (M2-5) | 11 of 12 FAILs |
| P | `40399422` echo fix | 16 | 14 | 2 (P-9, P-15, both 20 s) | 0 | 0 | 0 |
| S | `ae3bf895` + reconnect + two-connection | 21 | 20 | 1 (S-17) | 3 | 0 | 0 |
| T | `b8fb125c` + accepted-connection attribution | 20 | **20 in a row** (T-1..T-14, T-16..T-21) | 0 | 1 (T-15, rd vms-e88) | 0 | 0 failing (T-9: 2 frames, recovered) |

**THE BAR IS MET on T**: 20 consecutive injected arms across 6/10/13/14/16/20 s,
zero VAX bugchecks, and all three MEMBER -- both OVMX nodes' own SHOW CLUSTER at
3/3 (`grade3.sh`) and the real VAX's console carrying no removal and no bugcheck
(`T-census.txt`, read independently of the grader). T was stopped after 21 graded
arms once the bar was met, to free the node for the browser runs. T-9 stamped the
old echo on 2 frames and still passed: OVMXB took the VAX's START while its
channel still held the pre-stall advertisement, the VAX's fresh advertisement then
reset the channel, and the circuit re-formed at the new number 0.2 s later -- the
reset path doing its job, not the failure this item closed.

(Q and R were started on intermediate builds and stopped after 3 arms each when a
newer fix landed; `loop-Q.log` / `loop-R.log` on the rig say so. S-4 graded FAIL
and was re-graded PASS: the VAX's `%CNXMAN` "proposing addition" line was cut by
console interleaving while its OPCOM line "proposed addition of node OVMXB" was
intact -- `grade3.sh` now accepts either rendering, as `gate-eval.mjs` always
did; no other arm in M2/P/Q/R changes grade under that fix.)

### What each residual failure was, and what closed it

**P-9 (20 s) -- a reconnect that superseded itself.** OVMXB, woken, dialled
OVMXA once a second while OVMXA answered each CONNECT ~1 s late; every ACCEPT
arrived after the CSB had been re-bound to a newer attempt -- six CONNECT_REQs,
five accepted by the peer, none owned -- until the window ran out and OVMXA was
removed (`failing-arms-dials.txt`). Fix `d26c402f`: one reconnect attempt in
flight per CSB; the p. 7-30 once-a-second cadence is for attempts that ENDED.

**Q-2 (14 s) -- two connections for one pair.** Both ends re-dialled; each
accepted the other's; the real VAX then disconnected the one IT had initiated
and kept OVMXB's -- 4 crossings of 4 on the rig resolved this way
(`crossing-census.txt`). OVMXB's CSB was bound to the one the VAX dropped, read
the drop as the peer hanging up, stopped asking, and removed the VAX while the
kept connection stood open. Fix `ae3bf895`: the CSB remembers its own attempt and
the pair's second connection; it runs on the one it initiated and, if the peer
closes either while the other stands, continues on the survivor.

**S-17 (16 s) -- an accepted connection bound to the wrong system.** OVMXB
accepted OVMXA's reconnect, then the VAX's, before OVMXA's ACCEPT_RSP was
processed. The glue named an accepted connection's system from ONE pending-
accept slot -- by then naming the VAX -- so OVMXA's connection was bound to the
VAX's CSB and OVMXA was removed. Fix `b8fb125c`: `scs_conid_peer()` -- the
CDT's own `peer_sysid` -- names the system; the slot no longer decides.

**P-15 (20 s) -- a START handled after the STACK that answered it.** Not
reproduced since; not closed. OVMXB re-formed with the VAX, then ~180 ms later
restarted the open circuit on a START the capture shows only once, before the
STACK it acted on first. No OVMX layer reorders frames (the fork queue and the
packet handler are FIFO). `98c97749` makes the next occurrence name its frame:
"peer re-started the circuit (its START carries send-msg# N)".

**NOFAULT (M2-21, Q-1, S-1, S-11, S-23) -- OVMXB never admitted in the first
place.** The initial join, not the stall: the join picked OVMXA (an OVMX member
does not coordinate an addition beside a real VAX), then the VAX rejected a
later connect and OVMXB never asked it again. Filed as rd vms-e88; it is also
the V0.7-3 browser all-at-once shape.

## Who re-dials after a stall -- the oracle (`oracle-vax1-stall/`)

Two real OpenVMS VAX V7.3 members, each stalled 10 s in turn (SIGSTOP of the
emulator, nothing on the wire touched):

| stalled | original initiator of the pair's connection | who dialled VMS$VAXcluster after the wake |
|---|---|---|
| VAX2 (csid 00010002), `../vms-8c54-stalled-guest-20260928` | VAX2 (`compare-vax2-stall-and-formation.txt`) | VAX1 -- the survivor, csid 00010001 |
| VAX1 (csid 00010001), this capture | VAX2 | VAX2 -- the survivor, csid 00010002 |

"The original initiator re-dials" and "the lower CSID re-dials" are both
refuted; in every observation the node that STALLED dialled nothing and only
accepted. OVMX's stalled node still dials too (its first attempt fires ~1 s after
it notices), which is how the crossings above arise -- the two-connection fix makes
them harmless rather than rare. Whether a stalled VMS node is inhibited from
dialling, or only loses the race, is not visible from the wire; nothing here
encodes a mechanism the oracle did not show.


## The browser failures were not restarts

The V0.7-3 deploy read its failures as "emulated VAX halts -> reboot -> the cluster
refuses the new incarnation". There were no reboots: the visitor gate's
scrollback stitch re-appended the whole screen on every poll (pcjs paints its
cursor into the screen text), so each copy re-read the power-on banner as a
restart. Fixed in `tools/cluster-web-demo/gate-eval.mjs` (`681cb70d`, unit-tested
on the real shapes); replayed through the fix, every node in all three failing
V0.7-3 runs had ZERO restarts -- see the correction appended to
`../vms-deploy-v073-20260929/README.md`. The quick-reboot oracle the halt reading
called for was therefore not the question; the real all-at-once shape is an
initial-join failure (rd vms-e88).

**Branch bundle vs the live V0.7-3 page, same pod, same gate** (`browser-*/`). The
branch bundle is this PR's own build (`b8fb125c`) for both OVMX nodes -- Node A's
initramfs, sysdisk and kernel from build-boot-artifacts, Node B's slim disk from
`run-boot.sh sysboot-single` at the same SHA -- with the pinned real-V7.3 Node C,
served from two localhost origins exactly as the live page splits
openvmx.3dl.dev / vax.3dl.network, no query parameters, cold profile per run:

| case | branch `b8fb125c` | live V0.7-3 |
|---|---|---|
| all-at-once (x1 CPU) | **5/5 CN=3** (~2 min 15 s each) | 5/5 CN=3 |
| page-order-throttled4 (x4 CPU) | **3/3 CN=3** | 2/3 -- run1: OVMXA put no SCA frame on the hub in 20 min |

No restarts, no bugchecks, no losses, no OVMX founding on any branch run. HONEST
READING: this pod does not reproduce the V0.7-3 all-at-once failure even against
the live page, so these runs show NO REGRESSION under visitor conditions, not an
improvement; the stall rig above is where the improvement is measured.

