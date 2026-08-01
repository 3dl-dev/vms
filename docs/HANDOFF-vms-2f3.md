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

## 5. ⚠ WHERE TO START NEXT SESSION

**The question is no longer "why can't we rejoin" — it is "why does the
coordinator stall on us at all, when it never stalls on a real node".** Fix
that and the rejoin should follow, because the rejoin is just the case where our
retry no longer rescues us.

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

Run tags this session: `ctl1`, `inc1`, `inc2`, `fresh1`, `fresh2`, `keyB`,
`keyC`, `rej2`, `rej3`. **Last SCSSYSTEMID used: 1226.**

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
