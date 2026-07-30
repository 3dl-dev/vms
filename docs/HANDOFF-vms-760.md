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

## 1. Where it stands (rewritten 2026-07-30g)

**The fan-out anomaly is SOLVED, the relay works, and the failure has moved.**

The `op 0x02` must go to **the coordinator**. A non-coordinator peer *silently
discards it* — byte-verified: in `d94-e15` all three members received a
byte-identical `op 0x02` within 400 ms, VAX1 and VAX2 only acked and did nothing
(VAX1 had a **383 ms head start**), and only VAX3 relayed. The reference joiner
picks the coordinator too (VAX2, not VAX1). Our "first eligible" rule picked
VAX1 — the wrong node — which is the whole reason `d94-e14` was acked and then
silent. `cm_pick_coordinator()` now picks by highest DECnet node number.
**Fan-out never started competing transitions** — that §4(p) claim is withdrawn;
fan-out only worked because it happened to include the coordinator.

Our `op 0x02`, our `0x81`/`0x09` echo and our config frames are **byte-correct**.
That line of investigation is closed.

**Progress this session, measured on a pristine lab:** barrier `1 → 3 → 5` of 12,
`INCONSTATE` eliminated, and VMS logs `proposed addition of node OVMXC3`.

### Two crashes, both instructive, both fixed or named

Getting the relay working exposed a whole phase we had never reached: once the
coordinator relays, **the non-coordinator members open their own transactions**
carrying ops absent from the pre-relay dialogue.

1. **`INCONSTATE` on VAX3 and `INVEXCEPTN` on VAX1 — FIXED.** The killer was our
   cat-`0x06` **close**, built from the `op 0x01` PARAMS template, which made
   `body[10:132]` byte-identical to our own PARAMS body. The real joiner's close
   differs from its own PARAMS at **17 offsets**, and `body[64:72]` must be a
   **live VMS time** — we shipped a replayed constant ~26 years off the era.
   Now built fresh, with two variants (membership-close vs lock-resource), and
   two regression tests. VAX2, which never received one, survived — a clean
   control.
2. **`LOCKMGRERR` on VAX1 and VAX3 — the CURRENT FRONTIER, see §2.**

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

### 2a. LIVE: barrier stalls at step 1, and it is NOT the DLM

**The cat-`0x02` `op 0x0d` shape is SOLVED and implemented** (§2b below — verbatim
echo + `body[34]=0xf9`, 1367/1367 byte-exact, unit-tested). But the run that was
meant to verify it never reached the DLM at all:

| run | build | barrier | cat-`0x02` seen | cluster |
|---|---|---|---|---|
| `coord3` | echo + cat-01 mutations | 5 | 8 (mangled) | **VAX1+VAX3 dead, LOCKMGRERR** |
| `coord4` | refuse | **5** | 15 (7 distinct → retransmits) | healthy |
| `coord6` | grounded DLM | 1 | **0** | healthy *(lab not reset — void)* |
| `coord7` | grounded DLM | **1** | **0** | healthy *(lab verified `CN_3`)* |

`coord7` completed the whole dialogue — `op 0x03` COMMIT, five `op 0x05` lock
rebuilds, the `op 0x06` burst, `op 0x09`, barrier step 1 — and then the
coordinator **never released step 1**. Response inventory is *identical* to
`coord4` apart from the absent cat-`0x02`. **So the missing DLM traffic is a
CONSEQUENCE of stalling at step 1, not its cause** — in `coord4` the DLM storm
only began once step 5 was reached.

> **Do not assume this is a regression from the DLM change** — it should not
> affect step 1 — **and do not assume it is variance either.** Both readings are
> open; an agent is diffing `coord4` vs `coord7`. The single most useful thing to
> establish: **did an `op 0x0c` release for step 1 ever arrive in `coord7`**, in
> any form or on any VC, that OVMX failed to recognise? Our log prints nothing
> for unhandled ops — that instrumentation gap is worth closing.

Specimens: `ovmx-760-barrier1-stall-20260730.pcap` (coord7) and
`ovmx-760-barrier5-dlmrefused-20260730.pcap` (coord4), logs
`~/vax/cluster/work/scsd-coord{4,7}.log`.

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
