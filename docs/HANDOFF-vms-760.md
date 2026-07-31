# Handoff — vms-760 (OVMX → `SHOW CLUSTER` MEMBER)

**Rewritten 2026-07-30 (session g). Read this, then `rd show vms-01c` and `rd show
vms-760`, then `docs/cluster-protocol-spec.md` §4(m), §4(n), §4(o), §4(p), §5(z).**

> **`vms-760` can no longer publish rd notes** (its event exceeds the relay size
> limit — see §0b). Live notes go on **`vms-01c`**; `vms-760`'s local history is
> still readable with `rd show vms-760`.

The protocol knowledge is in the spec. This file is *state, procedure, and how to
run the session so you don't die of context exhaustion halfway through.*

---

## 0. READ THIS FIRST — how to run this work

The previous two sessions both ended the same way: not blocked, just **out of
context**, mid-investigation. That is a process failure, not a research failure,
and it is avoidable. The work is 90% *reading captures* and 10% *editing C and
running the lab*. Reading captures is enormously expensive in tokens and is
perfectly delegable. Editing and running the lab is cheap.

**So: you are an ORCHESTRATOR. Your context is for decisions and code, not for
bytes.**

### The rule

> **Never read a capture yourself. Dispatch an agent, and keep only its
> conclusion.**

A single `cm.py` dump of a 17 000-frame capture can cost more context than an
entire fix. Three of this session's four breakthroughs came from subagents that
read hundreds of thousands of tokens of packet bytes and handed back a page:
the malformed op-7 length words, the `sm=3 ∧ am=2` admission gate, and the whole
§4(p) barrier decode. That is the pattern. Repeat it.

### What to delegate (always)

- Any question of the form *"what does the reference do at X?"*
- Any byte-level diff of OVMX vs the reference.
- Any *"why did the peer stop responding after frame N?"*
- Any survey across the capture library (`~/vax/cluster/captures/`,
  `~/vax/clean-cluster/captures/`) to separate per-join variation from constants.
- Any decode of a body whose fields you don't know.

Dispatch these **in parallel** — two or three at once, in one message. They take
5–15 minutes each and you can run lab experiments while they work.

### What to keep for yourself

- Editing `src/vmsscs/*.c` (you hold the design).
- Running `try.sh` / `reset3.sh` and reading their **summary** lines.
- Deciding what to test next.
- Committing, and updating the spec / rd.

### Agent prompt template that works

Give them: the clean-room rule, the exact capture paths and node MAC→name map,
the tooling list with the frame-offset cheat sheet, the spec sections to *read
rather than re-derive*, what is **already known** (so they don't redo it), a
numbered list of questions, and this instruction:

> Return numbered findings with frame indices and byte evidence. Separate
> GROUNDED from INFERRED. State explicitly what you could not account for.

Ask for an **implementation-ready checklist** ("the joiner must send X in
response to Y") when the answer will become code. The §4(p) agent returned
exactly that and it went almost straight into `scsd.c`.

### Budget discipline — hand off *early*

Check your remaining context at every natural break. When you are at roughly
**⅔ used**, stop starting new investigations:

1. Commit everything (never leave work uncommitted — a previous session nearly
   lost the MSCP sources that way).
2. Update `rd progress vms-760` with the *situated* state: what you tried, what
   you observed, where it stopped, and the next bisect.
3. Update this file's §1 and §2.
4. `bash docs/clean-room/retain.sh <session-id>` and commit the manifests.
5. Hand off with the next single experiment named.

**Handing off with budget to spare is success. Running to the wall is not.**
A successor who inherits a clean tree, a green suite, a live lab and a named next
experiment loses nothing. One who inherits a half-finished thought loses a day.

---

## 0b. One operational hazard — `vms-760` is close to un-publishable

`.ready/nostr-rejected.jsonl` recorded `invalid: event too large: 66138`. The
item now carries six long session notes and its serialized event has outgrown the
relay's limit. The notes are all in the local store and `rd show vms-760` renders
them, but **new progress notes may stop publishing**, which silently breaks
cross-machine sync.

Mitigation: keep future `rd progress` notes **short** and put the long-form
narrative **here** and in the spec, which are git-tracked and have no size limit.
If you need the full history, `rd show vms-760` locally. Consider closing
`vms-760` once MEMBER lands and opening a fresh item for the follow-on work
rather than continuing to append.

## 1. MEMBER ACHIEVED — 2026-07-30g

```
View of Cluster from system ID 1025  node: VAX1
|  NODE  | SOFTWARE |  STATUS |
| VAX1   | VMS V7.3 | MEMBER  |
| VAX2   | VMS V7.3 | MEMBER  |
| VAX3   | VMS V7.3 | MEMBER  |
| OVMXCA | VMX V0.1 | MEMBER  |
```

Run `coord10`, pristine 3-node lab, `SHOW CLUSTER` on a real VAX. **All 12/12
barrier steps released** (`XITDONE`), 178 cat-`0x02` DLM records answered,
`op 0x02` correctly aimed at the coordinator, the GO deferral fired once, and
**zero bugchecks on any VAX**. Specimen:
`captures/ovmx-760-MEMBER-achieved-20260730.pcap`.

**Four fixes, in the order they were needed:**

1. **Aim `op 0x02` at the COORDINATOR.** A non-coordinator silently discards it.
2. **Rebuild the cat-`0x06` close** — it was the PARAMS body with a replayed
   timestamp ~26 years off the era. Killed VAX3 (`INCONSTATE`) and VAX1
   (`INVEXCEPTN`).
3. **cat-`0x02` `op 0x0d` = verbatim echo + `body[34]=0xf9`**, and *not* the
   cat-`0x01` mutations, which corrupt the lock resource name. Killed VAX1 and
   VAX3 (`LOCKMGRERR`).
4. **Stop answering the barrier GO faster than the coordinator can fan it out**,
   and **let the candidate set settle before picking** the coordinator.

**Caveats to carry, not to gloss:**

- **REPRODUCIBLE — 3/3.** `coord10`, `coord11`, `coord12` each completed 12/12
  barrier steps with `XITDONE`, each on a freshly reset lab, **zero bugchecks**
  across all three VAXes throughout. `coord10` and `coord12` show `MEMBER` in
  `SHOW CLUSTER`; `coord11`'s console evidence was erased by the *next* run's
  reset (`reset3.sh` truncates `vax1.log`) — its SCSD log still shows 12/12.
  *Harness note:* VMS truncates `SCSNODE` to **6 characters**, so `OVMXR11` and
  `OVMXR12` both appear as `OVMXR1`; a `grep` for the full name silently finds
  nothing and looks like failure. Keep run node-names ≤ 6 chars.
- One `SCSD-W-CMUNGROUNDED` fired — cat `0x02` `op 0x01`, correctly refused,
  and membership was reached anyway. It is still an ungrounded pair: ground it.
- **PERSISTENCE CONFIRMED (soak, `d94-persist`).** OVMX held `MEMBER` across
  **nine consecutive minute-marks** of an ~11 min run, with a real cluster ID
  (`csid 00010004`). On daemon exit the cluster detected the loss, timed it out,
  and logged `Node OVMXP1 (csid 00010004) has been removed from the VAXcluster`
  — a clean departure, no stranded transition, cluster back to 3, **zero
  bugchecks**. Only 2 `CMUNGROUNDED` refusals fired in the whole soak and
  membership held throughout. Specimen:
  `captures/ovmx-760-persist-10min-20260730.pcap`.
  Departure is by peer *timeout*, not an explicit leave handshake — a real node
  presumably announces its departure. Refinement, not a defect.

## 1b. Where it stood before (kept for continuity)

OVMX now runs the real VMScluster add-member protocol. On the live 3-node lab
VMS prints, on its own console:

```
%CNXMAN,  received VAXcluster membership request from system OVMX..
Node VAX3 (csid 00010003) proposed addition of node OVMX..
%CNXMAN,  completing VAXcluster state transition
SDA>  879F7480  OVMX..  00010004    0     open
```

Working end to end: own `SCS$DIRECTORY` / `MSCP$DISK` / `VMS$VAXcluster`
connections all accepted · MSCP unit enumeration byte-identical to a real VAX ·
config exchange with **all three** members · the deferred `op 0x02` · `cat 0x04`
ack · `op 0x03` COMMIT · `op 0x05` lock rebuilds · the `op 0x06` burst · `op
0x09` / `0x12` / cat-`0x06` close · the §4(p) 12-step barrier · and the
interleaved cat-`0x02` DLM rebuild transactions.

**`SHOW CLUSTER` still reads `NEW`.** That is the whole remaining gap.

## 2. The frontier (2026-07-30g, latest first)

### 2a. SOLVED — the step-1 stall was a RACE, and OVMX was the fast one

OVMX answers the coordinator's `op 0x0a` GO **from the receive path in ~20 µs**.
The coordinator's own `0x0a` fan-out takes **20–214 µs per additional member**. A
step-1 `op 0x0b` that lands *while the coordinator is still fanning out* is acked
with tag **`0x0260`** and response marker `0x00` — "received in the go phase,
**not counted**" — instead of `0x0210` / `0x01`. The step is never tallied, the
barrier stays one member short, and the whole cluster's transition freezes. (In
`coord7` the coordinator later ran a wind-down with VAX1 and VAX2 *without* us —
precisely the §4(p) failure mode.)

**GROUNDED 6/6 across every 3-member run:**

| run | `0x0b#1` vs the last `0x0a` | ack tag | releases | reached |
|---|---|---|---|---|
| `coord2` | **after** all 3 GOs | `0x0210` | 2 | step 3 |
| `coord4` | **after** all 3 GOs | `0x0210` | 4 | step 5 |
| `coord1` | before last GO (−20 µs) | `0x0260` | 0 | step 1 |
| `coord6` | before last GO (−30 µs) | `0x0260` | 0 | step 1 |
| `coord7` | before last GO (−214 µs) | `0x0260` | 0 | step 1 |
| `e15` | before last GO (−75 µs) | `0x0260` | 0 | step 1 |

Epoch 5 produced **both** outcomes, and OVMX's step-1 `0x0b` is **byte-identical**
between a good and a bad run — so it is timing, not cluster state and not content.

> ⚠ **CONSEQUENCE: "the barrier reached step N" was NEVER a baseline.** `coord4`'s
> step 5 was luck on this race, and the cat-`0x02` DLM work only became visible
> *because* of that luck. **Every barrier-depth number in the rd history before
> commit `43527ea` is a coin flip, not a measurement.** The handoff already
> warned not to trust "5/12" for a different reason; this is the real mechanism.

**Fix (`43527ea`):** the GO reply is held `JOIN_BARRIER_GO_DELAY_MS` (3 ms). The
sleep is taken in the receive path deliberately — the loop otherwise wakes only
on traffic or a 1 s `SO_RCVTIMEO`, which would make the interval anywhere from
3 ms to 1 s, and controlling it precisely is the whole point.

**This is a general lesson, not a one-off.** OVMX is a userspace program on
modern hardware answering a 1980s minicomputer. *Being faster than the reference
implementation is itself a compatibility bug* wherever a peer's protocol assumes
it can finish a fan-out before anyone replies. Suspect it again at any other
multi-target broadcast point.

### 2b. SOLVED — the cat-`0x02` `op 0x0d` DLM response

GROUNDED and implemented. The response is a **verbatim echo + `body[34]=0xf9`**,
a recipe that reconstructs **1367 of 1367** real responses byte-for-byte across
four responder nodes and two captures. Derived independently by two agents.

**The theory that was wrong, and cost three sessions:** "a joiner holds no locks,
so echoing a rebuild record asserts lock state it does not have." False. The echo
returns the **coordinator's own record** with a result code and claims nothing —
which is why a lock-less joiner answers all 216 of them. What killed VAX1 and
VAX3 was **corrupting** the record: we applied the cat-`0x01` mutations
`body[18]=0x01` and `body[55]=0x00`, and for cat `0x02` those offsets land inside
the L1 region and **the 8th byte of the lock resource name** —
`"CACHE$cmSYSDSK1"` went out as `"CACHE$c\0SYSDSK1"`. The in-capture control is
decisive: across the same milliseconds VAX1 and VAX3 exchanged the same records
with each *other* correctly and neither crashed.

Full rule in spec §4(p); regression test in `tests/vmsscs/test_scs_member.c`.

### The controlled pair, both measured on a pristine 3-node lab

| run | cat `0x02` `op 0x0d` | barrier | cluster after |
|---|---|---|---|
| `coord3` (`OVMX_DLM_ECHO=1`) | full-body echo | **advances past 5** | **VAX1 + VAX3 dead, `LOCKMGRERR`** |
| `coord4` (default) | refused | **pins at 5** | **healthy, 3 MEMBER, zero bugchecks** |

**`coord4` proves the records are REQUIRED, not optional.** 15 requests arrived
carrying only **7 distinct `txn` values** — the coordinator **retransmits each
unanswered record up to 3×** and the barrier never leaves step 5. So:

> The answer is **a response**, not silence — and it must not assert lock state.
> Silence is merely the *safe* failure, which is why it is now the default
> (`OVMX_DLM_ECHO=1` restores the echo for bisecting). Neither is correct.
> Do not "fix" this by picking whichever failure looks tidier — ground the shape.

Specimens: `ovmx-760-lockmgrerr-20260730.pcap` (echo → crash) and
`ovmx-760-dlm-refused-20260730.pcap` (refuse → freeze), with
`~/vax/cluster/work/scsd-coord{3,4}.log` timestamp-aligned.

### Also open, lower priority

- **Do NOT expect `OVMX_NO_OWN_VC=1` to test the selection rule — measured, it
  does not.** Run `coord5`: with it set, OVMX sends **no `op 0x02` at all** (no
  `CMCONFIG2`, no barrier), so admission never starts. It also draws
  `%PEA0, Inappropriate SCA Control Message — FLAGS/OPC/STATUS/PORT 00/22/00/DB`
  and gets rendered in `SHOW CLUSTER` with a **blank** status column rather than
  `NEW`. Cluster stayed healthy (no bugchecks, 3 MEMBER). The flag only
  suppresses our own VC; **wiring the deferred `op 0x02` onto the
  member-opened VC is code work**, not a flag flip.
- **Peer selection is right but for a possibly-wrong reason.** The best-grounded
  rule is "the peer that opened a `VMS$VAXcluster` VC **to** the joiner" (3/3
  established joins). **We destroy that signal ourselves** by preemptively
  opening our own VC to all three peers — so the fallback ("highest
  SCSSYSTEMID", read at abs 60 of each member's 120-byte directed HELLO) is what
  actually carries us. Highest-SCSSYSTEMID is still confounded with
  "most-recently-added"; disambiguating needs a lab where those orderings
  disagree (e.g. SYSGEN VAX3 to sysid 1024, or reboot VAX2 last).
- **`op 0x02` `body[10:12]`.** §4(p) says send zeros (residue). One agent found a
  correlation the other way: all nine *working* requests carry `0x4150` (`"AP"`),
  and the one malformed failing run zeroed it. **Correlation only — do not act,
  but do not forget.**
- `d94-e10/e11/e12/e13` are **VOID** — every VAX in them self-reports
  `name="VAX1" sysid=1025`. Any conclusion resting on those is unsound.

## 2b. The previous frontier — SOLVED, kept for the record

A controlled pair on a **pristine** cluster (`reset3.sh`, zero ghost CSBs,
3 × MEMBER verified before each run):

| run | `op 0x02` sent to | result |
|---|---|---|
| `d94-e14` | **one** peer (what the reference does) | acked, then **nothing**. No commit, no transition. CSID `00000000`. |
| `d94-e15` | **all** peers (`OVMX_CFG2_ALL=1`) | `proposed addition of node OVMX..`, barrier starts |

So on our cluster the **fan-out** gates the transition — which **contradicts** the
reference, where the joiner sends `op 0x02` to exactly one peer and that peer
relays (`op 0x12`) and barriers with everyone.

Two readings, both testable, both kept live behind `OVMX_CFG2_ALL`:

- **(a) the peer-*selection* rule matters.** Our pick is "first eligible". The
  reference picked the *last* peer to finish config — but "last to finish
  config", "last MSCP walk" and "highest SCSSYSTEMID" are mutually confounded in
  the one 3-node specimen. **Delegate**: get an agent to permute the candidate
  rules against every completed join in the library and find one that survives.
- **(b) something in our `op 0x02` (or our `op 0x09` response) stops the chosen
  peer relaying.** **Delegate**: byte-diff our `0x02`/`0x81 0x09` against the
  reference's at *every* offset, including the SCS envelope.

> ⚠ **Do not treat "5/12 barrier steps" as the baseline.** The runs that got
> there were on a cluster that was itself re-forming (their OPCOM carries
> `%CNXMAN, proposing formation of a VAXcluster`). On a pristine cluster the
> single-coordinator form does not open a transition at all. The barrier code is
> correct and grounded; it is simply not the current blocker. Re-establish any
> baseline on a freshly reset lab.

## 3. Lab — repeatable and non-destructive

Live 3-node cluster: VAX1 + VAX2 + VAX3.

> ⚠ **AN UNVERIFIED PRECONDITION MUST ABORT, NEVER FALL THROUGH.** This voided
> three runs before I fixed it. The probe
> `WRITE SYS$OUTPUT "PRE_" + F$STRING(F$GETSYI("CLUSTER_NODES"))` returns an
> **empty string** when the console does not answer (it is often sitting at
> `Username:` after a reset). A caller doing `[ "$N" = "3" ] && break` then just
> ... doesn't break, the loop expires, and the run proceeds against whatever the
> lab happens to be — producing a plausible smaller number rather than an error.
> Use **`bash tools/waitnodes.sh <N> [max-checks]`**, which re-logs-in on an
> empty read and **exits non-zero** if it never sees N; and have the caller
> `|| exit 1` on it. Never hand-roll this check again.

> ⚠ **2026-07-30g: I broke the reset rule and got a junk result — don't repeat
> it.** Runs `coord5` and `coord6` went out back-to-back with no reset between
> them. By `coord6` the transition epoch had drifted to `0x00000007`, the barrier
> stopped at step 1, and **no cat-`0x02` request arrived at all** — so the run
> said nothing about the change it was meant to test. Stale CSBs and a
> half-transitioned cluster degrade the oracle *silently*: you get a plausible
> number, not an error. **One reset, one believable run.**

> ⚠ **2026-07-30g: `reset3.sh` does not always land 3 MEMBER.** After one reset
> `F$GETSYI("CLUSTER_NODES")` read **2** with all three SIMH processes up (VAX3
> booted but had not rejoined; it joined ~1 min later). **Always verify
> `CLUSTER_NODES` = 3 immediately before a run you intend to believe, and
> re-check rather than assuming the reset worked.** Also: the reset can leave the
> VAX1 console at the `Username:` prompt — run `login.sh` before probing.

```bash
T=~/vax/cluster/tools
bash $T/reset3.sh    # ~6.5 min -> pristine 3-node cluster, ZERO ghost CSBs
bash $T/login.sh     # robust console login (drives off actual console state)
bash $T/try.sh <tag> <NODE> <sysid> <duration> [ENV=V ...]
```
**The tooling lives in `~/vax/cluster/tools/`** — scripts, `mk_sysgen`, and the
capture-analysis tools (`pcap.py`, `tl.py`, `cm.py`, `cmp.py`, `body.py`,
`fulldiff.py`, `seqchk.py`). Runs write to `~/vax/cluster/work/`, which also
holds the reference OVMX captures `d94-e12/e14/e15.pcap`.
(They were developed in a session job-tmp dir, which is deleted with the job —
they were copied out deliberately. Do not point at a `.claude/jobs/...` path.)

- **`reset3.sh` restores `data/d{0,1}.dsk.3node-golden.bak`** — a snapshot taken
  *with VAX3 installed*. **Never run the old `golden-reset.sh`**: it restores the
  Jul-26 **2-node** golden and destroys VAX3's `[SYS2]` root.
- **`try.sh` makes VMS narrate.** It sets `REPLY/ENABLE=(CLUSTER)` for CNXMAN
  OPCOM and runs `ANALYZE/SYSTEM` → `SHOW CLUSTER` for the CSB dump. **These two
  oracles are worth far more than DCL `SHOW CLUSTER`** — they name the state VMS
  thinks you are in and why. The pre-session reset script actively *suppressed*
  them with `REPLY/DISABLE=(CLUSTER,…)`.
- **Reset before any run whose result you intend to believe.** Stale `NEW`
  CSBs accumulate; at ~16 of them `SHOW CLUSTER` renders an *empty table* — the
  oracle degrades before it errors.
- Use a **fresh, novel `SCSSYSTEMID`** per run (last used: 1164).
- **Gotcha that has bitten twice:** never `pgrep -f`/`pkill -f` a pattern that
  also matches your own command line. Use `pgrep -x vax` and explicit PIDs.
- `/tmp` is `noexec`: run scripts as `bash script.sh`.

## 4. Code

Branch `worktree-760-active-directory`, tree clean, **10/10 vmsscs tests**.
All joiner work is behind **`OVMX_JOIN_SEQ`**; the default path is untouched
(Rule 9). Env switches: `OVMX_JOIN_SEQ`, `OVMX_CFG2_ALL`, `OVMX_NO_OWN_VC`,
`OVMX_PURE_SERVER` (superseded, kept for comparison).

- `src/vmsscs/scsd.c` — sequencer, CM handler, barrier state machine, ack path.
- `src/vmsscs/scs_member.c/.h` — CM builders: config, `0x81` echo (**three**
  mutations), cat-`0x04` ack, cat-`0x86` token response, `op 0x0b` barrier step.
- `src/vmsscs/scs_hello.c/.h` — `peer_logical` (§4a.0).

**Two invariants worth not relearning the hard way:**

1. **Derive length words from what you emit; never inherit them.** Inheriting
   them from the request is what stalled the entire join for three sessions.
2. **Response shape is per-category and must be grounded per category.**
   Reusing the category-`0x01` full-body echo on a category-`0x06` request
   reflected VAX1's own I/O structures back at it and bugchecked it
   (`INCONSTATE, Inconsistent I/O data base`). Generalising a transform across
   categories is the Rule 10 "plausible handler" failure.

**Known honest limit:** we answer cat-`0x02` DLM rebuild transactions because the
coordinator gates the barrier on them. A joining node holds no locks, so
acknowledging a rebuild record is what a joiner has to say — but grant / deny /
block / remaster are **not** implemented. Revisit when OVMX actually holds locks.

## 5. Reference and clean-room

- **Authority for an established join:** `~/vax/cluster/captures/vax3-2to3-established-join-20260730.pcap`
  (VAX3 `08:00:2b:11:22:33`, VAX1 `aa:00:04:00:01:04`, VAX2 `08:00:2b:78:56:b9`).
  Every *other* join specimen is a 1→2 **formation** and has misled earlier
  sessions.
- Our best runs to diff against it: `~/vax/cluster/work/d94-e14.pcap`
  (single-coordinator, clean cluster) and `d94-e15.pcap` (fan-out). Tools are in
  `~/vax/cluster/tools/`.
- **Clean-room is a HARD invariant (Rule 8).** Wire observation + public docs
  only. Run `bash docs/clean-room/retain.sh <session-id>` at the end of every RE
  session and commit the refreshed `*.sha256`.
- **`cat`/`op` are per-SYSAP namespaces** — resolve the Con.ID to a SYSAP before
  interpreting `body[8:10]`, or you will "find" a membership message that is
  actually `SCA$TRANSPORT`. That mistake is already in the record once.
