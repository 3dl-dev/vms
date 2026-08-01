# Handoff — `vms-2f3`: OVMX cannot rejoin a cluster it was removed from

**Written 2026-08-01. Read `docs/HANDOFF-vms-760.md` §0 first for the orchestrator
doctrine — it held again this session: every finding below came from agents or
from OVMX's own logs, and the orchestrator read no packet bytes at all.**

> **Start at §0. §5 is what to do next.** §1–§4 exist so you do not re-derive
> them — in particular §3, which is a list of things that look like the answer
> and are not.

---

## 0. Status in one paragraph

**The rejoin is still refused — but the session ended somewhere better than a
narrower version of the same question. Read §4b first: OVMX's join has been
limping all along, and the rejoin is just the case where the limp stops
working.** A returning OVMX gets the full START handshake, the directory phase,
the VC bind, the config exchange, the coordinator's **`op 0x03` COMMIT** and its
**`op 0x05` lock-rebuild** — and then the coordinator never opens the barrier,
for as long as we care to wait (measured to 420 s), and all three peers log
`timed-out lost connection`. The decisive finding is that **the coordinator
stalls on OVMX in successful joins too**, and what rescues those is OVMX
retransmitting its own `op 0x02` at ~+8 s — something no real node ever does. A
real transition is over in 42–132 ms. Two genuinely frozen wire fields were
found and fixed on the way (`c302b7d`, verified live, no regression), and nine
hypotheses were killed. **A four-minute reproducer now exists that needs no lab
reset.**

| | state | run |
|---|---|---|
| fresh identity joins | **works, ~20 s** | `fresh1`, `fresh2` |
| rejoin, same identity | **REFUSED** | `ctl1`, `inc1`, `inc2`, `rej2`, `rej3` |
| quarantine is time-bounded | **REFUTED** — survives 15 h | `ctl1` |
| frozen incarnation is the cause | **REFUTED** — fixed, still refused | `inc1` |
| frozen message-time is the cause | **REFUTED** — fixed, still refused | `inc2` |
| `cat 0x01 op 0x04` is the cause | **REFUTED** — present in successes | log census |
| our window was too short | **REFUTED** — a real transition takes 42–132 ms | `rej3`, af2 |
| the bug is in the commit→barrier window | **REFUTED** — a real node sends *nothing* there | af2 |
| **OVMX's successful joins are healthy** | **REFUTED — they are rescued by our own retry** | `fresh1` vs af2 |
| the coordinator stalls before commit | **REFUTED — it acks in 0.4-0.7 ms. The silence is OURS** | `g1A`/`g1B`, §4c.1 |
| `Ref. time` (`own_admission`) is the cause | **REFUTED — real joiners send the same sentinel** | census, §4c.4 |
| the `amsg=0` / bundled `op 0x02` is the cause | **REFUTED BY EXPERIMENT — the correct shape draws NO reply** | `p1A`, §4c.2 |
| which peer completes START first | **REFUTED — both orderings in successes and failures** | §4c.5 |

## 1. The reproducer — four minutes, no reset

This is the single most useful thing this session produced. Earlier work needed
`reset3.sh` (~6.5 min) plus a full `cycle.sh` run; this needs neither.

```bash
cd /home/baron/projects/vms
CL=/home/baron/vax/cluster
ONE=$CL/tools/oneshot.sh

# 1. a brand-new identity joins in ~20 s  (positive control — ALWAYS run it)
$CL/tools/mk_sysgen $CL/work/sysgen-X.dat OVMXX1 1230
bash $ONE ctlA $CL/work/sysgen-X.dat 130     # expect: CLUSTER_NODES=4, XITDONE=1

# 2. the SAME store, ~45 s later, is refused
bash $ONE ctlB $CL/work/sysgen-X.dat 130     # expect: waitnodes FAILED, XITDONE=0
```

`oneshot.sh` runs one join against the lab **as it stands**, captures to
`work/d94-<tag>.pcap`, logs to `work/scsd-<tag>.log`, and finishes with an SDA
`SHOW CLUSTER` dump into `work/<tag>.status`. Every fresh identity still joins
no matter how many dead CSBs have piled up, so the lab does not saturate the way
the 2026-07-30 session feared — but do reset before a run you intend to publish.

**Always run the positive control in the same session as the negative one.** Two
of this session's four hypotheses would have survived without it.

## 2. The one instrument that mattered

`SCSD-T-CMIN` logs **every** inbound CM message (suppressed by `OVMX_CM_QUIET`;
leave it unset). Counting inbound `(cat, op)` from OVMX's own log is cheaper
than any capture analysis and it is what localised the failure:

```
fresh2 JOINED:  op 0x14 x3 | op 0x01 x3 | op 0x03 COMMIT | op 0x05 x5 |
                op 0x0c x12 | cat 0x81 op 0x0b x12 | op 0x06 x253 | cat 0x02 op 0x0d x176
inc2  REFUSED:  op 0x14 x3 | op 0x01 x3 | op 0x03 COMMIT | op 0x05 x5 | then NOTHING
rej2  REFUSED:  op 0x14 x3 | op 0x01 x3 | op 0x03 COMMIT | no op 0x05 | then NOTHING
rej3  REFUSED:  identical to rej2 over a 420 s window
```

```bash
grep -ao 'SCSD-T-CMIN, cat 0x[0-9a-f]* op 0x[0-9a-f]*' work/scsd-<tag>.log \
  | sed 's/SCSD-T-CMIN, //' | sort | uniq -c | sort -rn
```

The peer-side oracle is SDA, not DCL — `ANALYZE/SYSTEM` → `SHOW CLUSTER`. DCL
`SHOW CLUSTER` renders an *empty table* when peers hold stale CSBs, so the
oracle degrades before it errors.

## 3. Killed hypotheses — do not re-chase any of these

1. **Time-bounded quarantine.** `cyc2` waited 420 s; `ctl1` waited **15 hours**
   on a lab left standing overnight. Still refused, with the identical
   `wait`/`long_break` CSB. There is no wait that works.
2. **The frozen incarnation timestamp.** Real, fixed, *not* the gate — §4.
3. **The frozen message timestamp.** Same: real, fixed, not the gate.
4. **`cat 0x01 op 0x04` is a rejoin-only probe we fail to answer.** A capture
   agent proposed this as the root cause and it is wrong. `cat 0x01 op 0x04
   txn=0 csum=0 smsg=11 amsg=9` arrives **identically in the successful join**
   (`scsd-fresh1.log` 11:35:37.767) and the failed one (`scsd-inc1.log`
   11:25:14.525) — same sender `08:00:2b:11:22:33`, same 190-byte frame — and
   also in `cyc1-c1`, `rm2`, `dep1/2/7/10`. The "mid-dialogue re-send of `op
   0x01`" is likewise present in successes (4 in `fresh1`, 4 in `inc1`). Both go
   unanswered in runs that reach MEMBER. **Do not write a handler for `op 0x04`
   on this evidence.**
5. **The `[22:24]` incarnation counter echo.** Correct in every failed run: the
   peers advertised 3, 4, 5, 6 across attempts and OVMX echoed each exactly.
6. **SCS sequence numbers.** Both sides restart at 1 on every attempt; the
   member's stale round-0 `send_seq` is receive-tolerated as spec §4(i).A
   requires. Not the failure.
7. **Our `op 0x02` retry.** Required and working — it is what triggers the
   coordinator's second round in a successful join. Keep it.
8. **Our test window being too short.** `rej3` ran 420 s: same census, nothing
   after the lock-rebuild.
9. **Changing one half of the identity pair.** `OVMXNU`/1222 (new name, dead
   sysid) and `OVMXC2`/1225 (dead name, new sysid) are both refused — but VSI
   *OpenVMS Cluster Systems* App. A documents that `SCSSYSTEMID` and `SCSNODE`
   may not be changed independently without rebooting the whole cluster, and
   both fail in a **different place** (never reach `STARTDONE`; 80 `STARTRX`).
   These are documented-illegal configurations, not discriminators. Do not cite
   them as evidence about the rejoin.

## 4. What DID land — two frozen fields, and the spec was wrong about one

Commit `c302b7d`. Neither fixes the rejoin; both are real bugs, both are now
honest, and one of them was one step from crashing a real VAX.

**`[66:74]` of the `0x41` START is THIS SYSTEM'S INCARNATION** — a full VMS
absolute-time quadword, not the three fields the spec described. OVMX shipped
the captured template's `bb 8e 67 7a 94 00 bc 00` = **26-JUL-2026 14:35:33.59**
on every boot, forever. Grounded four independent ways:

- SDA on VAX1 rendered OVMX's CSB as `Incarnation 26-JUL-2026 14:35:33` — the
  template bytes to the second.
- The same dump gives real peers their own boot times: VAX3 `1-AUG-2026
  00:02:21`, VAX1 `30-JUL 08:54:26` (it had not rebooted).
- After the fix VAX1 read back `Incarnation 1-AUG-2026 15:25:12`, matching the
  `0x00bc05526906b4a1` we emitted (the VAX clock runs 4 h ahead of the host).
- **Public doc**, VSI *System Management Utilities Ref. Vol. II*, SHOW CLUSTER
  SYSTEMS class: *"INCARNATION: Unique 16-digit hexadecimal number established
  when the system is booted."* Sixteen hex digits is this quadword.

> **Spec correction owed:** `cluster-protocol-spec.md` splits this into a 5-byte
> token `[66:71]`, a flag `[71]`, and a "constant `0x00bc`" `[72:74]`. It is one
> quadword. Those upper bytes only look constant because every 2026-era VMS
> timestamp ends `bc 00`.

**`[98:106]` is a second quadword — when the frame was composed.** Real peers
carry two or three distinct values inside a single capture, and one of VAX3's
matches to the second the OPCOM line it printed as it built the frame. Ours was
frozen at 26-JUL-2026 14:48:50, so every START OVMX ever sent claimed to have
been written six days earlier. Now live per frame. **Its role is not grounded
and the code does not claim one**; what is grounded is the negative — no real
node sends a stale one.

**Why this mattered beyond tidiness:** VSI *Cluster Systems* App. C.7.1
documents that a connection reestablished after `RECNXINTERVAL` without the node
having rebooted earns a **CLUEXIT bugcheck** on the surviving side. A node whose
incarnation never changes is exactly that node.

**Still frozen, deliberately not touched this session (one variable at a time):**
`Ref. time` — SYSAP body `[64:72]` of the 190-byte cat-`0x01`/op-`0x01` message
(`own_admission`, `scs_member.c`). OVMX ships the `1-JAN-2001` sentinel because
the live value is gated behind `is_member`. Real nodes carry their admission
time (VAX3 `1-AUG 00:04:40`, matching its *"is now a VAXcluster member"* console
line to 50 ms). **This is the next honesty fix and it is a candidate cause** —
see §5.

Kill-switches so the failing case stays reproducible:
`OVMX_INCARNATION_FROZEN=1`, `OVMX_MSGTIME_FROZEN=1`; `OVMX_INCARNATION_TIME`
pins the quadword. 41/41 tests green, with byte-exact assertions on both fields.

## 4b. THE REFRAME — OVMX's join has been limping all along

This landed last and it changes the shape of the problem. The af2 rejoin
specimens were decoded end to end (real identity `VX3`/1050, first join at SCA
2558, rejoins at 20170 and 33591), and compared against OVMX's own captures.

**A real transition is over in milliseconds.** From add-member request to the
last finalize frame: first join **42 ms**, rejoin-1 **132 ms**, rejoin-2
**88 ms**. From first HELLO to full barrier completion, all three are under
**2 seconds**. There is no "minutes" regime anywhere in the reference — our
130–420 s windows are 100–1000× longer than a transition ever needs.

**A real node sends `op 0x02` exactly once**, in all three transactions, and is
answered in **1–20 ms**. There is no retry cycle in the reference at all, and no
retransmission in either direction anywhere between commit and barrier-open —
every `op 0x06` in the coordinator's 257-frame lock-directory burst has a unique,
strictly increasing `smsg`, with zero gaps or repeats.

**What the returning node sends in the commit→barrier window: nothing.** All 341
frames in that window are coordinator→node, 204 bytes, `cat 0x01 op 0x06`. Flow
control is carried in-band in `smsg`/`amsg`; there is no separate ACK frame. The
returning node's only job there is to receive. So the §0 framing — "what does a
returning node owe the coordinator in that window" — has an answer, and the
answer is *nothing*. **The bug is not in that window.**

**And here is the reframe.** OVMX *always* stalls, including on joins that
succeed:

- `d94-fresh1` (**succeeded**): OVMX sends `op 0x02` once at +1.46 s → commit +
  lock-rebuild → coordinator sends `op 0x04` and a re-sent `op 0x01` → **goes
  silent for ~6.5 s**. OVMX answers neither. At **+8.0 s OVMX retransmits its
  own `op 0x02`** — the coordinator answers *that* in 2.2 ms, redoes commit and
  lock-rebuild, and this time proceeds into the 253-frame burst → barrier →
  MEMBER.
- `d94-inc1` (**failed rejoin**): identical through commit, lock-rebuild,
  `op 0x04`, re-sent `op 0x01`, silence. OVMX retries `op 0x02` at +7.76 s —
  **the coordinator never answers.**
- `d94-rej2` (**failed rejoin**): fails earlier still — commit arrives, the
  lock-rebuild round never does, retry at +7.18 s unanswered.

So OVMX's successful join is **rescued by its own retransmit** — a step no real
node ever performs. The rejoin does not introduce a new defect; it removes the
accident that was papering over an existing one. **Something OVMX does at or
before commit makes the coordinator stall, and a fresh identity forgives it
while a returning identity does not.**

This also re-frames `op 0x04` a third time. It is **not** rejoin-specific — a
full sweep of all 103 pcaps found 89 occurrences, all in 3-node captures, all
coordinator-to-all-peers broadcasts, zero in any 2-node capture. But it and the
re-sent `op 0x01` are exactly what the coordinator emits **immediately before it
stalls on us**, and a real coordinator never sends either to a real joiner. They
are plausibly the coordinator *asking us something because we already confused
it*. Grounding them is now interesting again — as a symptom, not as the gate.

## 4c. SESSION j (2026-08-01 pm) — §4b's reframe was measuring the wrong window

**Read this before §5. It supersedes §5 items 1 and 2 and corrects §4b.**

Two agents (a pre-commit byte-diff, and a grounding census of `Ref. time`) plus
four lab runs. Net: **three more hypotheses dead, including the one §5 named as
the next fix, and one of them died to a four-minute experiment after 197k tokens
of byte analysis argued for it.**

### 4c.1 The coordinator does NOT stall before commit. The silence is OURS.

§4b said "the coordinator stalls on us". Measured, in both a success and a
failure, from OVMX's own `SCSD-T-CMIN` timestamps:

| | OVMX `op 0x02` → coordinator's `cat 0x04` ack | → `op 0x03` COMMIT |
|---|---|---|
| `g1A` (**joined**) | **0.4 ms** | **1.1 ms** |
| `g1B` (**refused**) | **0.7 ms** | **2.0 ms** |

The coordinator answers instantly in *both* cases and then runs the lock
rebuild. What comes next is `cat 0x01 op 0x04` + a **re-sent `op 0x01`** — and
*then* the ~6.5 s of quiet. That quiet is **OVMX's**: the coordinator has
re-advertised and is waiting for a reply we owe it, and we produce one only when
a retry timer fires. Our own instrumentation has been saying so all along —
`SCSD-W-STRAYACK, ... arrived 6907 ms ago -- the reference acks within ~1 ms`.

**Do not describe this as a coordinator stall again.** The window to explain is
`op 0x04` + re-sent `op 0x01` → our reply, and we are the one who is late.

### 4c.2 REFUTED BY EXPERIMENT: the `amsg=0` / bundled-burst ordering

The pre-commit diff agent produced a well-evidenced root cause: OVMX bundles
`op 0x02` into the opening burst (three frames in one microsecond, leaving the
wire **0.3 ms before the coordinator's own `op 0x14` arrives**), so its
`body[2:4]` ack-msg is necessarily 0. `(smsg=3, amsg=0)` occurs **104 times,
exclusively from OVMX, and zero times in 196 real specimens**. A real joiner
sends two frames, waits, then sends `op 0x02` as `(smsg=3, amsg=2)` to the
coordinator alone. All of that is **true and grounded**.

It is also **not the gate.** `OVMX_PURE_SERVER=1` already implements exactly that
shape — 2-frame burst, `op 0x02` deferred, coordinator-only, correct `amsg`. Run
`p1A`, fresh identity, no code change:

```
12:26:00.269  CMCONFIG, add-member burst (2 frames)      smsg 1,2  amsg 0
12:26:07.144  CMCONFIG2, DEFERRED op 0x02  send_msg=4 ack_msg=2   <- reference shape
              ... and the coordinator NEVER ANSWERS IT. No ack, no COMMIT, nothing.
              census: op 0x14 x3, op 0x01 x3. That is all. XITDONE=0.
```

**The reference-correct `op 0x02` draws *less* response than the malformed one.**
A fresh identity that joins reliably in ~20 s does not join at all in pure mode.
So fixing `amsg`/ordering as specified does not unstick the dialogue, and the
checklist built on it (hold `op 0x02`, gate on `peer_max_smsg ≥ 2`, coordinator
only) is **already implemented and already tested and does not work**.

Caveat kept honest: pure mode changes several things at once, so this refutes
"the pure-server shape fixes the stall", not the narrower claim "`amsg` matters".
But the narrower claim now has no path to a fix that we have not already run.

### 4c.3 The refusal is coordinator-side state — proven, not inferred

The agent byte-diffed OVMX's retry `op 0x02` in the run that **succeeded**
against the one that was **refused**: they are **byte-identical** (`0b 00 0c 00`
envelope, `01 02`, `body[16:18]=0x0200`, rest zero). Same frame, opposite
outcome. **Whatever decides the rejoin is not in anything we transmit at that
moment.** Combined with §3.1 (no wait works, 15 h tested), that closes the
"find the wrong byte" family of hypotheses for this step.

### 4c.4 `Ref. time` is GROUNDED and is NOT the bug — §5 item 2 is dead

§5 named this as the next fix. It is wrong, and the evidence is clean.

`op 0x01` body `[64:72]` = `00 80 4a 3f 0e 57 9f 00` = `0x009f570e3f4a8000` =
exactly **1-JAN-2001 00:00:00**, and **that is what a real VMS node sends while
it is being admitted.** Census over every `op 0x01` in the library, split by
*role, not vendor*:

- real VAX3 `08:00:2b:11:22:33` — the sentinel **69×** (as a joiner), live 2026
  values as an established member
- real VAX2/VX3 `08:00:2b:78:56:b9` — sentinel **20×**
- VAX1 (never a joiner in this library) — **never** the sentinel, always live
- `VX3`/1050 sends the sentinel on its first join **and both rejoins**, and is
  admitted every time

The flip is exactly simultaneous with `body[12]` `0x00→0x21`. No specimen ever
shows a member-form frame with the sentinel or a joiner-form frame with a live
value. Independently corroborated by SDA (`sda-scs-extract-vax1.txt`: VAX1's
`Ref. time` = the CLUB's `Founding Time`; VAX2's = the CLUB's `Last time stamp`).

So `Ref. time` is the reporting node's **own admission** timestamp, OVMX's
`scsd.c:2818-2822` already computes it correctly, and the sentinel it ships as a
joiner is right. **Do not "fix" this field.** A full 8-byte-window scan of the
132-byte body found **no other** timestamp-shaped value: only `[28:36]`,
`[36:44]`, `[64:72]`, all three already named and handled. That closes the
`op 0x01` half of the `vms-70c` honesty debt.

Same sweep on OVMX's `op 0x01` vs a real joiner's: **byte-identical except two
bytes**, `body[44]` (0 vs 3 — "transitions seen since boot"; VX3 sends 0 on its
2nd incarnation and joins) and `body[84]` (already grounded as per-boot noise in
§4(j)). There is nothing left to find in this message.

### 4c.5 Also dead: "which peer completes START first"

Both orderings appear in successes *and* failures — `fresh2` succeeded with
`78:56:b9` first, `g1B` failed with it first; `g1A` succeeded with `11:22:33`
first, `inc2` failed with it first. Free census over the archived logs.

### 4c.6 New peer-side oracles (cheaper than any capture)

**The VMS console counts the rounds.** In a **success**, VAX3 logs
`proposed addition of node OVMX` **twice, 6.7 s apart** — the second driven by
our retry. In a **refusal**, **exactly once**, then
`aborted VAXcluster state transition`. One `grep` of `/tmp/clean-vax1-test/vax*.log`
tells you which regime you are in, with no pcap at all.

**SDA polled *during* the stall** (§5 item 3, now executed — `g1B`, polls at
+15/+35/+60/+95/+140 s):

```
87A1F540  OVMX    00000000    0     open    <NO STATUS FLAGS>
Flags: 31080001 cluster,init,qf_failed_node,quorum,transition
Last transaction code 03 | Last trans. number 7 | Last coordinator CSID 00010001
```

**Nothing moves.** Identical at +60, +95 and +140 s. A transition is open for our
admission, numbered, coordinated by VAX1, and it simply never advances — and
nothing on the peer side times it out. (The `lost connection` at the end of every
run is *us* being killed, not the peers giving up.)

**Peers never retire dead OVMX CSBs.** `OVMXC2`, `OVMXFR` and `OVMXG2` CSBs all
coexist on VAX1 simultaneously, all `state 09 wait`, most `long_break`. The
per-identity residue accumulates without bound.

### 4c.7 Confirmed working: the incarnation fix (`c302b7d`)

VAX1's CSB for a run started at host 12:25:57 reads `Incarnation 1-AUG-2026
16:25:59` (the VAX clock runs 4 h ahead). Live, to the second, no regression.

### 4c.8 Grounded but unexplored: OVMX skips the joiner's directory + MSCP run

The 1.4–4.4 s a real joiner spends between `op 0x01` and `op 0x02` is **not idle**
(§4(o) currently implies it is). `af2-firsttimer` frames 20212–20243: the joiner
opens **its own** `SCS$DIRECTORY` (it does not reuse the member's), looks up
`MSCP$TAPE` and `MSCP$DISK`, opens `MSCP$DISK`, runs 2× SET CONTROLLER
CHARACTERISTICS and the full GET-UNIT-STATUS NEXT-UNIT walk, and tears the
directory connection down. **OVMX does none of it, ever** — its only outbound
CONN-REQ to the coordinator is the `VMS$VAXcluster` one.

Not proven to be a gate (`vax3-2to3#285` has an all-zero topology body and is
acked in 0.3 ms, so the MSCP walk is not *encoded* into `op 0x02`). But it is the
largest single behavioural difference left between OVMX and a real joiner, and it
is the one thing a real node does in the window we are late in. **This is the
strongest remaining lead — see §5.**

---

## 5. ⚠ WHERE TO START NEXT SESSION

> **§5 items 1 and 2 below are SUPERSEDED by §4c.** Item 2 (`Ref. time`) is dead
> — do not spend a commit on it. Item 1's framing ("why does the coordinator
> stall") is wrong — the silence is ours. Item 3 has been executed; its result is
> §4c.6. **Start from §4c.8, then item 4.**
>
> **The live question: what does OVMX owe the coordinator in reply to `op 0x04` +
> the re-sent `op 0x01`, that a real joiner has already done by then?** The
> leading candidate is the directory + MSCP client run (§4c.8) — the only real
> joiner behaviour we have grounded and never implemented. Implementing it is a
> real piece of work, not a byte fix, and it is testable in four minutes.
>
> **And keep the two outcomes separate.** Byte-identical frames get opposite
> answers (§4c.3), so making our join *healthy* and making the *rejoin* work may
> be two different fixes. Fix the join first — it is the one we can still observe
> going right.

**Historical framing, kept for the reasoning only:**

1. **Find what OVMX does at or before `op 0x02` that a real joiner does not.**
   Diff OVMX's pre-commit exchange against `VX3`'s in af2, frame by frame, in
   both directions — the two paths already differ in *coordinator behaviour* by
   the time commit lands, so the cause is upstream of it. Concretely: what does
   the coordinator have from `VX3` at the moment it answers `op 0x02` in 1–20 ms
   that it does not have from OVMX, given it instead emits `op 0x04` + a re-sent
   `op 0x01` and waits? A re-sent `op 0x01` reads like *"answer the cluster-
   parameters question again"* — which points at our `op 0x01` reply being
   unsatisfying, and note that `Ref. time` in that very reply is `1-JAN-2001`.
2. **Make `Ref. time` live** (§4, `own_admission`) (§4, `own_admission`). It is the last known frozen
   field, it is what a peer reads as *"when did this node join this cluster"*,
   and a returning node advertising `1-JAN-2001` is claiming to have joined 25
   years ago — plausibly enough for a coordinator to decline to promote it. One
   commit, one reproducer run.
3. **Ask the peer, don't infer it.** Session i's method note earned itself again
   here: SDA answered in one query what two capture analyses could not. During
   a refused rejoin the CLUB carries `transition` (`31080001`) while our CSB
   sits in `wait` with **no CSID and no status flags**, whereas a properly
   removed node reads `long_break,removed`. A transition *is* open for our
   admission and never progresses. Poll SDA **during** the stall, repeatedly,
   and watch which field moves.
4. **`vms-70c` is now partly paid and partly exposed.** Three replayed constants
   turned out to be timestamps; two are fixed. Anything else in
   `scs_start_tmpl` / `member_params_tmpl` that decodes as a plausible VMS
   quadword should be audited the same way — the honesty debt and the rejoin bug
   turned out to be the same defect class.

## 6. Lab tooling added this session

| script | what it does |
|---|---|
| `tools/cycle2.sh` | `cycle.sh` plus an **SDA CSB oracle** before the first join, after every cycle, and at the end; `PER_CYCLE_SYSID=1` for per-cycle identity; `CYCLE<N>_ENV` for per-cycle env; `SKIP_RESET=1`. |
|  `tools/oneshot.sh` | one join against the lab as it stands + SDA dump. **The four-minute loop.** Worth moving into `tools/`. |
|  `tools/probe.sh` | drive any VAX console, capture between markers. |

Run tags session i: `ctl1`, `inc1`, `inc2`, `fresh1`, `fresh2`, `keyB`, `keyC`,
`rej2`, `rej3`. Session j: `g1A` (joined), `g1B` (refused, SDA-polled), `p1A`
(pure-server, refused). **Last SCSSYSTEMID used: 1232.**

Session j also added `$CLAUDE_JOB_DIR/tmp/neg.sh` — a negative run that polls SDA
at +15/+35/+60/+95/+140 s *during* the stall instead of only at the end. Worth
folding into `tools/` as `stallpoll.sh`; the end-of-run SDA dump that `oneshot.sh`
takes cannot show that nothing moves.

## 7. Guardrails earned here

13. **Verify an agent's wire claim against a cheaper oracle before you build on
    it.** The `op 0x04` root-cause was refuted in one `grep` of our own log,
    after an agent had spent 160k tokens arriving at it. Agents are for reading
    bytes, not for deciding what is true.
14. **Run the positive control in the same session as the negative one.** "It is
    refused" means nothing without "and a fresh identity joined 20 seconds
    later, on this same lab, with this same binary."
15. **A fix that is right can leave the bug unfixed.** Two frozen fields were
    genuine defects, grounded and documented, and neither was the gate. Fix them
    anyway — and do not let the disappointment relabel them as noise.
16. **Before implementing an agent's checklist, check whether it is already
    behind an env var.** The pre-commit agent spent 197k tokens deriving a
    correct, well-evidenced `op 0x02` shape. `OVMX_PURE_SERVER=1` had implemented
    it months ago. One four-minute run refuted the whole checklist — *and* told
    us something new (the correct shape draws no reply at all), which no amount
    of further capture reading would have. **`grep` the codebase for the fix
    before you write it, and run the experiment before you commit it.** This is
    guardrail 13 with the cost inverted: cheap oracles beat expensive analysis
    not only for *refuting* claims but for *acting* on them.
17. **Measure the window you are actually naming.** §4b asserted a coordinator
    stall for a whole session. The coordinator's response latency was 0.4 ms and
    had been in our own logs the entire time; the 6.5 s belonged to the next
    window and to us. Timestamp the specific edge before naming who is late.
