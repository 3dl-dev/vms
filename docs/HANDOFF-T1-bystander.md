# Handoff — T1: OVMX survives cluster life as a MEMBER

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
reasons that turned out to be entirely about the lab harness rather than the
protocol. A run chasing it (`dep10`) was left **running** at handoff.

| | state | run |
|---|---|---|
| plain join → MEMBER | verified | `v1`, and every run since |
| bystander of an **addition** (class `0x02`) | **PASSES** | `by13`, re-confirmed `dep6`/`dep8`/`dep9` |
| bystander of a **removal** (class `0x03`) | **PASSES on today's binary** | `rm2` |
| bystander of a **departure** (class `0x04`) | **NOT YET PRODUCED** | `dep1`–`dep10` |
| barrier when the removed node is *our* barrier peer | **FAILS — new finding** | `dep6` |

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

## 5. ⚠ WHERE TO START NEXT SESSION

**A run is live at handoff.** `dep10` — graceful departure of **VAX2**, OVMX
bystanding, 15-minute grace, tag `dep10`, node `OVMXDX`, sysid `1219`.

```bash
tail -40 ~/vax/cluster/work/dep10.status          # the verdict
L=~/vax/cluster/work/scsd-dep10.log
grep -ac 'class=0x04' $L                          # >0 == the first bystander specimen
grep -a 'XITION,\|CLUSTATE\|STRAYACK\|UNGROUNDED' $L | tail
```

**Read the status file before believing anything else.** If it says
`PHASE2 WARNING ... UNATTRIBUTED`, VAX2 did not finish shutting down and the run
says nothing about class `0x04` — it does *not* mean OVMX failed.

Then, in the order I would take them:

1. **Finish `vms-ae5`.** Its *fails* half is verified (`rm2`). If `dep10`
   produced a class-`0x04` open with OVMX still MEMBER and no abort, the item
   closes. If VAX2 also hangs at `STOPUSER`, then **no node in this lab can shut
   down gracefully**, and that — not OVMX — is the blocker; file it and close
   `vms-ae5` on the *fails* half with the *leaves* half moved to its own item.
2. **`vms-c7d`, now with a live reproducer** (§3). Removing OVMX's own barrier
   peer stalls it at step 6/12. This is a genuine T1 hole and it is cheap to
   reproduce.
3. **The quorum experiment** (`tools/quorum.sh`, written and unrun). Grounded
   pre-flight: **quorum is 1 and VAX1 is the only voter**, so killing it
   *guarantees* quorum loss — the survivors are documented to block all process
   and I/O activity and wait, recoverable without a reboot. The script therefore
   reads OVMX's log and VAX2/VAX3's consoles rather than VAX1's, treats a hung
   DCL as a *result*, and trips its watchdog only on bugcheck evidence from a
   real VAX. **It depends on §4.1 being fixed**, which it now is.
4. **`vms-584` item 5, join/exit cycling** (`tools/cycle.sh`, written and unrun).
   Now unblocked by the per-boot Con.ID change — running it *before* that change
   would have tested a defect we already knew about.

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
