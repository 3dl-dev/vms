# Handoff — vms-760 (OVMX → `SHOW CLUSTER` MEMBER)

**Rewritten 2026-07-30 (session f). Read this, then `rd show vms-760`, then
`docs/cluster-protocol-spec.md` §4(m), §4(n), §4(o), §4(p), §5(z).**

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

## 1. Where it stands

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

## 2. The frontier — one clean bisect, already set up

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

Live 3-node cluster: VAX1 + VAX2 + VAX3, all MEMBER.

```bash
bash $JOBTMP/reset3.sh    # ~6.5 min -> pristine 3-node cluster, ZERO ghost CSBs
bash $JOBTMP/login.sh     # robust console login (drives off actual console state)
bash $JOBTMP/try.sh <tag> <NODE> <sysid> <duration> [ENV=V ...]
```
`$JOBTMP` = `/home/baron/.claude/jobs/678334fd/tmp` (scripts, captures, tools).

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
- Our best runs to diff against it: `$JOBTMP/d94-e14.pcap` (single-coordinator,
  clean cluster) and `d94-e15.pcap` (fan-out). Tools: `tl.py`, `cm.py`, `cmp.py`,
  `body.py`, `fulldiff.py`, `seqchk.py`.
- **Clean-room is a HARD invariant (Rule 8).** Wire observation + public docs
  only. Run `bash docs/clean-room/retain.sh <session-id>` at the end of every RE
  session and commit the refreshed `*.sha256`.
- **`cat`/`op` are per-SYSAP namespaces** — resolve the Con.ID to a SYSAP before
  interpreting `body[8:10]`, or you will "find" a membership message that is
  actually `SCA$TRANSPORT`. That mistake is already in the record once.
