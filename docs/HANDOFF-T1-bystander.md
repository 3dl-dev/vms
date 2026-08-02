# Handoff — T1: OVMX survives cluster life as a MEMBER

> ## ⇒ `vms-2f3` HAS ITS OWN HANDOFF NOW — read `docs/HANDOFF-vms-2f3.md`
>
> Item 1 of §5 below ("OVMX cannot rejoin") was worked on 2026-08-01 and moved a
> long way: a **four-minute reproducer that needs no lab reset**, two frozen wire
> fields found and fixed (`c302b7d`), and the failure narrowed to a single step —
> the coordinator commits and rebuilds locks for a returning node and then never
> opens the barrier. Nine hypotheses are killed there; check that list before
> spending anything. The rest of §5 (`vms-c7d`, `vms-2d6`, `vms-416`, `vms-ae5`)
> still stands as written.
>
> **⇒ LAB CAPACITY IS NO LONGER ONE (2026-08-02, `vms-a5c`).** The rest of §5 no
> longer has to queue behind `vms-2f3` for the lab. `tests/lab/` on main runs
> isolated 2-node VMSclusters on k3s — one pod each, three replicas up,
> `kubectl -n ovmx-lab scale sts/vaxlab --replicas=N` for more. Read
> `tests/lab/README.md` first. Build a lab-2 line of attack's controls on lab-2
> from the start; never carry a lab-1 control into a lab-2 comparison (different
> SIMH binary — same source commit, different arch).

**Rewritten 2026-07-31, end of session i. Read `docs/HANDOFF-vms-760.md` §0 FIRST
for the orchestrator doctrine — it still applies verbatim, and this session is
more evidence for it: four capture agents produced every grounded finding below
and the orchestrator read no packet bytes at all.**

> **Start at §0 for state, then §5 for what to do.** §1–§4 are the reasoning and
> exist so you do not re-derive it.
>
> Companion documents: spec **§4(s)** (where a member's advertised cluster state
> comes from) · **§4(t)** (Con.ID allocation) · **§4(u)** (ack cadence) ·
> **§4(v)** (a retired finding) — all written this session. Session h's
> `docs/analysis-e81-by11.md`, spec §4(p) and §4(r) still stand.

---

## 0. Status in one paragraph

**T1.1 is closed; T1's remaining work moved forward on four fronts and one
front is still open.** Three grounded code changes landed and are verified live
(commit `7254056`); four spec sections landed (`e20c1e2`); `vms-ae5`'s *fails*
half was re-verified on today's binary; and the *leaves* half — a class-`0x04`
graceful departure with OVMX as a bystander — is **still not demonstrated**, for
reasons that turned out to be entirely about the lab harness and the lab itself
rather than the protocol. Two new defects were found and filed: **`vms-416`** (no
node in this lab completes an orderly shutdown, which blocks the class-`0x04`
evidence) and **`vms-2f3`** (OVMX cannot rejoin a cluster it was removed from).
Nothing is left running.

| | state | run |
|---|---|---|
| plain join → MEMBER | verified | `v1`, and every run since |
| bystander of an **addition** (class `0x02`) | **PASSES** | `by13`, re-confirmed `dep6`/`dep8`/`dep9` |
| bystander of a **removal** (class `0x03`) | **PASSES on today's binary** | `rm2` |
| bystander of a **departure** (class `0x04`) | **NOT YET PRODUCED** | `dep1`–`dep10` |
| barrier when the removed node is *our* barrier peer | **FAILS — new finding** | `dep6` |
| REJOIN after removal, same identity | **FAILS — mechanism found, `vms-2f3`** | `cyc1`, `cyc2` |
| quorum loss (kill the only voter) | **ANSWERED — no reconfiguration at all, `vms-2d6`** | `q1` |

## 1. What landed in the code (commit `7254056`, all 41 tests green)

### 1.1 Re-learn the cluster from the TRANSITION-OPEN — the fix, and a corrected model

The known defect was "our learned `member_count` can go stale". The census
corrected something bigger: **`op 0x01` was the wrong source in the first place.**

It is a point-in-time *reply* to a newcomer's query, sent once per VC, and sent
to a newcomer **before that newcomer is counted** — VAX1 answered OVMX 6.7 s
early, VAX3 4.1 s early, and **never advertised 4 at all** anywhere in `by13`.

The transition-open (`op 0x09`/`0x08`/`0x0d`) is the bundle every node sees on
every transition of every class: `body[40:48]` is the transition-time quadword
(matches a member's later `last_transition` to the millisecond against OPCOM,
and is present 20 ms *before* OPCOM logs "completed"), and `popcount(body[55])`
is the post-transition member count (54/54 opens, zero residuals).

**Verified live, run `dep6`:**

```
SCSD-I-CLUSTATE, re-learned from the transition-open (our barrier completed):
members 3 -> 4, last_transition 0x00bc046f061936c0 -> 0x00bc046fc60fab80
```

Applied at *our barrier completing*, so a proposal that never completes is never
advertised; class `0x04` has no barrier so it applies at the open. `formed` stays
copy-once — it never changes in any capture. The `op 0x01` path remains the
bootstrap but can no longer walk us backwards: `last_transition` is monotonic, so
a copy carrying an older transition than the one we hold is stale by construction
and is logged and ignored.

**Two limits kept deliberately.** We read the bitmap's *popcount* only — the
bit-to-node mapping is **not** grounded (both observed transitions set a
contiguous run, fitting "bit k = CSID slot" and "bit k = join order" equally) and
the field's extent is undetermined. Counting bits needs no mapping; asserting the
bitmap would, so we never assert it.

**And an explicit absence, which is a finding, not permission:** no peer anywhere
in the library reacts to a wrong `member_count`. `by13` reached four nodes
normally with OVMX advertising a stale `2`. This defect class is invisible on the
wire and will never announce itself.

### 1.2 Con.ID: the high word is now per-boot

Every OVMX Con.ID was a compile-time constant, making OVMX the only node on the
wire whose connection identifiers are identical on every boot. Real nodes use one
monotonic counter shared across *all* service classes per boot
(`0x33590007` dir, `0x33580008` VC, `0x33580009` MSCP) with the high word
**reseeding non-arithmetically per incarnation** (`0x8fd20007 → 0xe9950007 →
0x5b050007`). A real node never repeats a Con.ID across incarnations; OVMX always
did.

Safe to change because the peer **binds whatever is offered and never validates
it** — 30+ CONNECT sequences with unpredictable values, all answered
ECHO → ACCEPT → CONFIRM, no NAK anywhere tied to a value. The *derivation* of our
high word is labelled an OVMX design choice (Rule 8). `OVMX_CONID_BASE` pins it
for a reproducible run.

**This had to land before the cycling test** — that test is exactly the untested
collision case, and no reference capture can answer it because a real allocator
cannot produce a repeat.

### 1.3 A bounds bug the change introduced, found and fixed

The transition-open latch sits under an `n >= 92` guard, but `body[40:48]` needs
`n >= 120` and `body[55]` needs `n >= 128`. Under the outer guard alone, leftover
buffer bytes would have become an advertised member count. Now checked at the
bytes actually touched.

### 1.4 Stray acks: instrumented, deliberately not "fixed"

The reference ack is prompt (0.30/0.53/0.39/0.45 ms), opportunistic (no timer, no
fixed N — idle captures carry none), cumulative, and never keyed to an opcode. An
`op 0x01` is never acked (0/4). OVMX emits an ack naming an `op 0x01` ~7.0 s late
and a genuine ack-of-ack ~4 s late — **both provably inert**, no peer reacted in
any run.

Not silenced, on purpose: the emission rule carries its own grounded claim (only
the `op 0x06` burst is acked; no ack is attributable to the `op 0x0a`/`0x0c`
notifications), and widening the trigger would contradict it **on the one path
that currently works**. Guard 8 — a guard that hides a bug is worse than no
guard. `SCSD-W-STRAYACK` now names which frame each stray acked and how stale it
was; 13 fired in `dep6`.

### 1.5 Connect-back timing: finding retired, no code change

"~11 minutes early" was measured against the wrong anchor. The connect-back is
sub-2 s in every reference specimen (9 events, 6 captures: 0.054–1.925 s) and
OVMX measures 0.046–1.150 s — **inside the range**. The frame cited as evidence
(`by11` 2980) is an `mt=0x4b` MSCP sub-channel renegotiation on a ~10 s cadence,
reproduced byte-identically in `by10`. The ~660 s figures were capture *lengths*.

## 2. `vms-ae5` — the *fails* half is re-verified; the *leaves* half is not

**`rm2` (today's binary):** VAX3's SIMH killed outright while OVMX sat as a
member → class-`0x03` open seen, **barrier 12/12, `XITDONE=2`**, cluster settled
at three nodes with OVMX among them, zero aborts. Identical to `rm1`, so today's
three code changes are **not** a regression.

**The *leaves* half is still open**, and the library cannot supply it: a
44-capture census found class `0x04` in only **2 files, 3 instances, every one
strictly 2-node** (VAX1↔VAX2). **There is no third-party-bystander specimen for a
graceful departure anywhere.** Whatever run finally produces one is the first
evidence that exists, which is why it is worth the trouble.

Grounded from those 2-node specimens, for whoever implements the response:
`op 0x12` → reply `0x81/0x12` with `body[18]=1` and **`body[17]` = the
responder's own current class, NOT an echo**; `op 0x03` and `op 0x0d` → standard
full-body echo; `op 0x0a` → **send nothing**; and do not alter HELLO cadence or
tear down any VC — real VMS just stops. OVMX's existing silence on tag `0x0460`
is consistent with all of this.

## 3. NEW FINDING — OVMX cannot barrier through the loss of its own barrier peer

`dep6` removed **VAX2** and OVMX **stalled at barrier step 6/12**. `rm2` removed
**VAX3** on the *same binary* and completed **12/12**. The variable is *which*
node left: OVMX appears unable to complete a transition barrier when the node
being removed is the peer it holds its CM dialogue with.

That is not a regression and it is not the departure question — it is
`vms-c7d` (VC breakage) showing up on its own. **Do not re-derive this from
scratch; it is reproducible by pointing `departure.sh` or a kill at VAX2.**

## 4. THE HARNESS WAS THE ADVERSARY — four real bugs, all now fixed

Runs `dep1`–`dep9` produced no class-`0x04` transition, and **not one failure was
protocol**. Each was a lab-harness defect that reported success or looked like a
peer problem. They are listed because each one is a trap the next session would
otherwise re-enter.

1. **Only VAX1 was ever logged in.** `reset3.sh` catches VAX1's `Username:`
   prompt during boot; VAX2 and VAX3 boot unattended and nobody logs in. Their
   consoles then answer a carriage return with a **bell** and nothing else.
   Every experiment in this lab has silently depended on VAX1 being the only
   oracle. **This also blocks the quorum run**, which kills VAX1 and needs
   VAX2/VAX3 consoles afterwards. `tools/loginN.sh` now logs in any node, and
   the prompt *is* re-offered later, so post-hoc login works.
2. **SYSMAN's remote path is dead in this lab.** `SET ENVIRONMENT/NODE=VAX3`
   succeeds and reports the environment, but `DO SHOW TIME` returns no output and
   `SHUTDOWN NODE` is accepted with `%SYSMAN-I-SHUTDOWN, SHUTDOWN request sent to
   node VAX3` and then **nothing happens** — VAX3's SMISERVER never answers. Do
   not spend time on it again. (`/AUTOMATIC_REBOOT=NO` is also invalid — it is a
   boolean qualifier, `/NOAUTOMATIC_REBOOT`.)
3. **THE CONSOLE ECHOES INPUT, SO A LITERAL PROBE ALWAYS "SUCCEEDS".** Sending
   `WRITE SYS$OUTPUT "V3_DCL_OK"` and grepping for `V3_DCL_OK` matches **our own
   echo**, from any state — including a bare `Username:` prompt. `loginN.sh`
   reported `LOGGED-IN` while VAX3 sat at the login prompt, and
   `@SYS$SYSTEM:SHUTDOWN` was typed in **as the username** (it is in that console
   log). **Every probe is now computed** — `F$STRING(6*7)` → `42` — because only
   DCL can evaluate it. This is the same failure family as the `tee`-swallows-
   exit-status bug from session h: *a check that cannot fail is not a check.*
4. **Two premature kills, both of which replaced the experiment.** `dep6` waited
   180 s for `SYSTEM SHUTDOWN COMPLETE` and then reaped VAX2 mid-shutdown; `dep8`
   "detected" a halt in 4 s because it grepped the **whole** console log for
   `>>>` and matched the node's own **boot** prompt. Both converted a graceful
   departure into a node *failure* — `lost connection` → `timed-out` →
   `proposing reconfiguration` → class `0x03`. Killing the node does not hurry
   the announcement; **it replaces it.** Halt detection is now tail-only and the
   grace period is 15 min, and if the node will not go down the run says
   UNATTRIBUTED rather than manufacturing an ending.

**And one lab fact that is not a harness bug:** an orderly `@SYS$SYSTEM:SHUTDOWN`
on **VAX3 hangs at `%SHUTDOWN-I-STOPUSER`** and never completes (16+ min, twice).
VAX3 is a diskless MOP-booted satellite. Both reference class-`0x04` specimens
depart **VAX2**, a full member — which is why `dep10` targets VAX2.

## 4b. `dep10` and `cyc1` — the two results that closed the session

**`dep10` (class-0x04, VAX2 departing): UNATTRIBUTED, and the lab is the
blocker.** VAX2's orderly shutdown answered all seven prompts and then stalled
right after `N terminals have been notified`, never even reaching
`%SHUTDOWN-I-STOPUSER`, for the full 15-minute grace. VAX3 stalled twice at
`STOPUSER`. **No node in this lab completes an orderly `@SYS$SYSTEM:SHUTDOWN`**,
so a class-`0x04` departure cannot currently be produced here at all. That is a
lab property, not an OVMX one — OVMX joined and settled at four nodes in every
one of these runs. Filed as **`vms-416`**, which now blocks `vms-ae5`. The
harness refused to kill the node and marked the run unattributed rather than
manufacturing a class-`0x03` and calling it a departure.

**`cyc1` (join/exit cycling): a REAL DEFECT, and the best find of the session.**
Cycle 1 joined normally (epoch `0x06`, `XITDONE=1`), OVMX was killed, the cluster
removed it and settled back to three nodes in ~45 s. **Cycle 2 — the same
`SCSNODE` and `SCSSYSTEMID`, ~15 s later — never reached four nodes**, for 240 s:
`XITDONE=0`, `XITGO=0`, and *no transition of any class was opened by anybody*.

OVMX is not silent and not obviously wrong. It established the VC with **both**
peers, sent its own CONNECT-REQUEST, confirmed the VC, sent the full add-member
burst (`op 0x14/0x01/0x02`), and correctly learned `members=3`. **The peers took
the VC and then simply never proposed the addition**, eventually logging
`lost connection` / `timed-out lost connection` for `OVMXCY`.

**The constant Con.ID is RULED OUT as the cause** — this was the first run with
the per-boot change and the log shows `local_conid=0x7C960002`, a fresh value.
Filed as **`vms-2f3`** (P1), which blocks `vms-584`.

*OVMX cannot come back.* "Survive cluster life as a MEMBER" has to include
restarting, so this is a T1-level hole, and it would hit a real deployment on the
first restart.

## 4c. The last two runs — quorum answered, and the rejoin mechanism found

**`q1` — kill the only voting node. ANSWERED, and the answer is "nothing
happens".** Grounded pre-flight: SDA reads `Quorum/Votes 1/1`, VAX2 and VAX3 are
`VOTES=0`, so VAX1 is the only voter and killing it *guarantees* quorum loss.
After the kill: **no class-`0x03` removal of VAX1 was ever proposed by anyone**
(OVMX saw `0x02:1 0x03:0 0x04:0`), and VAX2's and VAX3's consoles carry **no
post-kill CNXMAN traffic at all** — not even a `lost connection`. Compare `rm1`/
`rm2`, where losing a *non-voting* node produces the full
`lost connection → timed-out → proposing → removed → completing` chain in ~37 s.
Zero bugchecks; the watchdog never tripped.

**The consequence that matters: quorum loss gates the RECONFIGURATION ITSELF.** A
member cannot be removed while quorum is lost, so OVMX's silence in that window
is *correct*, not merely lucky — there is no transition to participate in. OVMX
also declined 4 ungrounded requests (`cat 0x02 op 0x01`) rather than inventing a
reply, which is the allowlist doctrine working on the exact path that has
bugchecked real VAXes twice. Filed as **`vms-2d6`**. New surface named there:
`cat 0x02 op 0x01` is a DLM request a member sends that we have never grounded.

**`vms-2f3` — the rejoin. Two hypotheses killed, then SDA answered it.**

- **Incarnation: REFUTED.** OVMX *does* bump the counter (`01 00` → `02 00`
  across all 9 STARTs of each cycle), so the §4(i).B gate is satisfied. A
  byte-diff of the deferred `op 0x02`, the fan-out burst and the CONNECT-REQUEST
  against the *successful* cycle, to the same peer, shows **every byte from
  abs 72 onward — the whole SYSAP body — identical**. OVMX sends correct content.
- **Time-bounded quarantine: REFUTED.** `cyc2` waited 420 s instead of 10 s
  between the kill and the rejoin. Still refused.
- **MECHANISM, directly observed.** `ANALYZE/SYSTEM` → `SHOW CLUSTER` on VAX1,
  taken live while the refusal was happening:

```
87A06040  OVMXC2  00000000    0     wait    long_break
878F2380  VAX1    00010001    1     local   member,qf_same,qf_noaccess
CLUB flags: 11080001 cluster,init,qf_failed_node,quorum
```

  The peers hold a **CSB for the dead incarnation in state `wait`, status
  `long_break`**, and the cluster block carries `qf_failed_node`. And the rejoin
  is **not ignored — it is ABORTED**: VAX3's console logs `timed-out lost
  connection to node OVMXC2` immediately followed by **`aborted VAXcluster state
  transition`**. A transition *is* proposed for our rejoin and then abandoned.

  So this is neither an encoding bug nor a timing bug in anything OVMX sends. The
  question is now specific: **what retires a `long_break` CSB, and what must a
  returning node present so that CSB is reused rather than collided with?** A real
  VAX reboots and rejoins routinely, so a mechanism exists and we do not have it.

> **Method note worth keeping: SDA on a real member answered in one query what
> two capture analyses could not.** Passive capture can show what a peer did; it
> cannot show what a peer *declined* to do. When a peer goes quiet, ask it.

## 5. ⚠ WHERE TO START NEXT SESSION

Nothing is running and nothing is half-finished. Tree clean, 41/41 tests green,
three commits landed. Take these in order:

1. **`vms-2f3` — OVMX cannot rejoin.** Highest value. Both easy hypotheses are
   already dead (§4c); the mechanism is a peer-side `long_break` CSB. Next steps,
   cheapest first:
   - **Rejoin under a DIFFERENT `SCSSYSTEMID`** and confirm it *is* admitted.
     One cycle, and it isolates the collision to the identity rather than to
     anything else about our restart.
   - **Poll the `long_break` CSB** with repeated `SHOW CLUSTER` after a kill: does
     it ever retire on its own, and after how long? `RECNXINTERVAL` is the obvious
     candidate and is a documented SYSGEN parameter we can read and vary.
   - Only then ask what a real rebooting VAX presents that we do not.
   **Do not spend more effort on passive captures for this** — SDA answered in one
   query what two capture analyses could not.
2. **`vms-c7d`, now with a live reproducer** (§3). Removing OVMX's own barrier
   peer stalls it at step 6/12 (`dep6`), while removing a third node completes
   12/12 (`rm2`). Genuine T1 hole, cheap to reproduce.
3. **`vms-2d6` — ground `cat 0x02 op 0x01`**, the DLM request the quorum run
   showed a member sending and OVMX correctly declining. Silence is the right
   default, so this is not urgent; it is named so it is not rediscovered.
4. **`vms-416` — make some node shut down cleanly**, which is the only way the
   class-`0x04` evidence ever gets produced. Ideas are in the item: find what
   SHUTDOWN is waiting on (`SHOW SYSTEM` on the departing node from VAX1), try
   the `REMOVE_NODE` shutdown option, or check whether an absent queue manager /
   audit server makes `SHUTDOWN.COM` wait on a step that never finishes.
5. **Close `vms-ae5` on the *fails* half.** `rm2` verified it on today's binary;
   the *leaves* half is now properly blocked behind `vms-416`.

## 6. Lab tooling added or fixed this session

| script | what it does |
|---|---|
| `tools/departure.sh` | class-`0x04` graceful departure. `DEPART_NODE=n` picks the departing node, `DEPART_GRACE_S` the patience, `SKIP_RESET=1` reuses a verified lab. |
| `tools/quorum.sh` | kills VAX1, the only voter. Oracles moved off VAX1; bugcheck-only watchdog. **Unrun.** |
| `tools/cycle.sh` | join/exit cycles to drive the epoch past the node count. **Unrun.** |
| `tools/loginN.sh` | console login for *any* node. Computed probe, one prompt per pass. |
| `tools/reset3all.sh` | `reset3.sh` plus concurrent logins on VAX2/VAX3. The concurrent watcher still misses the boot window — `loginN.sh` afterwards works, which is what `departure.sh` relies on. Worth fixing. |

Run naming: `dep1`–`dep10` (departure), `rm2` (removal). **Last SCSSYSTEMID used:
1219.**

## 7. Guardrails

The nine from sessions g and h stand verbatim. Three more earned here:

10. **A check that cannot fail is not a check.** A literal console probe matches
    its own echo; `tee` swallows the exit status of the command before it. Both
    printed success over a failed check. Prefer a probe whose *output differs
    from its input* — `F$STRING(6*7)`.
11. **State-detection must consult current state, not history.** Grepping a
    whole console log for `>>>` matches the boot prompt forever. Tail it.
12. **When the experiment will not start, suspect the harness before the
    protocol.** Nine runs, zero protocol failures, four harness bugs. Session h's
    lesson was "when a peer goes quiet, suspect what you owe it"; this session's
    is its sibling — when *nothing happens at all*, suspect the instrument.
