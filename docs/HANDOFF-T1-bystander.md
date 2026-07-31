# Handoff — T1: OVMX survives cluster life as a MEMBER

**Rewritten 2026-07-31, end of session h. Read `docs/HANDOFF-vms-760.md` §0 FIRST
for the orchestrator doctrine and the lab procedure — it still applies verbatim,
and this session is more evidence for it: every substantive finding below came
from a subagent reading a capture, and the orchestrator read none of them.**

> **Start at §0 for state, then §3 for what to do.** §1 and §2 are the reasoning
> and exist so you do not re-derive it; skim them, do not study them.
>
> Companion documents: `docs/analysis-e81-by11.md` (the full field map for
> `cat 0x01 op 0x01`, member vs joiner) and spec **§4(r)** (the CM role slot and
> transition class) + **§4(p)** (barrier scaling and the `body[55]` membership
> bitmap), both written this session.

---

## 0. Status in one paragraph

**T1.1 (`vms-e81`) is DONE. Both bystander cases pass on the real lab.** OVMX
sits as a MEMBER while another node joins *and* while another node fails, answers
the cluster-wide barrier for both, and is still a MEMBER afterwards with zero
aborts and nothing stranded.

```
View of Cluster from system ID 1025  node: VAX1
| VAX1   | VMS V7.3 | MEMBER  |
| VAX2   | VMS V7.3 | MEMBER  |
| OVMXBD | VMX V0.1 | MEMBER  |     <- OVMX, a bystander to VAX3's join
| VAX3   | VMS V7.3 | MEMBER  |
```

| | state | run |
|---|---|---|
| plain join → MEMBER | **verified** | `v1` |
| bystander of a **removal** (class `0x03`) | **PASSES** | `rm1` |
| bystander of an **addition** (class `0x02`) | **PASSES** | `by13` |

`by13`: `transitions=2 xitgo=2 restarts=0`, `aborts=0`. Epoch 4 = OVMX's own
admission, epoch 5 = VAX3's addition — **OVMX's second completed barrier, for a
transition it had nothing to do with.**

## 1. What was settled, and what it cost to learn

### 1.1 `MEMBER` was never broken — the alarm was a measurement artefact

Session g ended flagging that run `by8` reported `PHASE1 transitions=0`. It is
not a regression. `F$GETSYI("CLUSTER_NODES")` **counts a node as soon as its CSB
is added, before the 12-step barrier completes**, so a count-based check can read
3 while `XITDONE` is still 0. Reproduced deliberately in `by10`. Run `v1` on a
pristine 3-node lab reached `| OVMXV1 | VMX V0.1 | MEMBER |` with
`transitions=1 restarts=0`.

`tools/bystander.sh` now waits for `XITDONE` **as well as** the count, so
"PHASE1 OK" means settled, not merely counted.

### 1.2 The op-1 CONNECT-ECHO is correct unconditionally

Grounded across 8 specimens: a member accepting a newcomer's connect and a
**joiner** accepting a member's connect are byte-identical — `op 0` → `op 1` →
`op 2` → `op 3`, with no membership-dependent difference. There is no condition
in code that should distinguish them, and adding one would invent a distinction
VMS does not make. `68f6e9e` stands as written.

Sequence invariants grounded at the same time, worth not relearning:

- **One `send_seq` per peer, shared across all Con.ID pairs**, +1 per emitted
  frame. `accept_ss == echo_ss + 1`, always.
- Peers do **not** key on an absolute `send_seq` (`op 0` was accepted at
  `ss` = 7, 8, 10 and 11 across specimens) but they **do** require
  **contiguity** — zero forward gaps in ~25 000 reference frames.
- **The failure mode for a gap is SILENT DISCARD.** There is no NAK anywhere in
  the corpus. A frame that "should have worked" and drew no reply is the shape
  this bug class always takes.

### 1.3 The bystander root cause: **a member must RECIPROCATE a newcomer's config**

In the reference, a newcomer sends every member it holds a VC with an `op 0x14`
then an `op 0x01`, and the member answers with its **own** `op 0x14` + `op 0x01`
**within one millisecond** (VAX1 +0.9 ms, VAX2 +0.3 ms). The newcomer then emits
its `cat 0x01 op 0x02` add-member request ~110 ms after the **last** member
reciprocates, and its `amsg` literally acknowledges that reciprocal `op 0x01`.

**The trigger is data-driven, not a timer** — which is exactly why every patience
experiment this item ran came back negative.

OVMX never reciprocated. VAX3 sent us its config and got one stray `cat 0x04`
ack 6.6 s later. Result: **zero `op 0x02` from VAX3 for 678 s**, and it was never
proposed for addition by anyone.

**The proof is a natural experiment inside the same capture, and it is worth more
than the fix.** OVMX's process exited at +702 s; VAX1/VAX2 ran the removal
transition at +730 and each re-pushed their `op 0x01` to VAX3; at **+733.5 —
three seconds after OVMX left the membership, with nothing else changed — VAX3
sent its `op 0x02` and joined normally.** The only variable was our presence.

> **This is the THIRD time on this item the premise has inverted the same way.**
> The newcomer looked like it was ignoring us and was **blocked on us** — first
> the missing op-1 CONNECT-ECHO, now the missing reciprocal config. Both are
> things the reference does immediately and unconditionally. Both read from
> outside as *the peer's* silence. When a peer goes quiet, suspect an obligation
> we owe it before suspecting anything about the peer.

Fixed in `e4568c8`, scoped to `appeared_after_join`. **Shipped alone, on purpose**,
so `by11` attributes cleanly.

### 1.4 The `vms-e4b` hypothesis was refuted — and the real P0 was next door

The item proposed keying the CM allowlist on a role slot read as
`<generation><role>`. A 26-capture census says:

- `body[16]` **is** a stable role slot (`0x10` relay, `0x20` commit, `0x30` the
  `op 0x0f` step, `0x40` transition-open, `0x60` barrier go), zero residuals.
- `body[17]` is **not** a generation — it is the **transition class**: `0x02` add,
  `0x03` remove-a-failed-node, `0x04` self-departure. One capture settles it: six
  successive transitions with the epoch monotone at 3, 4, 6, 7, 9, 11 while
  `body[17]` runs `0x04, 0x04, 0x02, 0x04, 0x02, 0x02` — it goes *down*.
- So the transition-open opcode does **not** vary by generation. The
  `0x09`/`0x08`/`0x0d` triple is the three **classes**, which merely look like
  consecutive small integers.
- And **role-keying would have been a bug**: role `0x20` alone spans the echo
  opcodes *and* the 7882-frame `op 0x06` burst that must never be echoed, and
  role tags do not exist at all on cat `0x02`/`0x06` — the two categories that
  have already bugchecked real VAXes. **The key stays `(SYSAP, category, opcode)`.**

**The real P0 the hypothesis was pointing at:** we armed the barrier only on tag
`0x0260`. The tag is `(class << 8) | role`, so **`0x0360` — the barrier of a
class-`0x03` REMOVE — fell through to silence.** A node *failing* is the likeliest
thing to happen around a sitting member.

**Proved live, run `rm1`:** VAX3's SIMH process killed outright while OVMX sat as
a member. VAX1's own console:

```
%CNXMAN, lost connection to system VAX3
%CNXMAN, timed-out lost connection to system VAX3
%CNXMAN, proposing reconfiguration of the VAXcluster
%CNXMAN, removed from VAXcluster system VAX3
%CNXMAN, completing VAXcluster state transition
```

`completing`, not `aborting` — the coordinator gates every step on *every* member
answering, so it could only complete because OVMX answered the class-`0x03`
barrier it was previously blind to. `transitions=2`, `aborts=0`, cluster settled
at VAX1+VAX2+OVMX. Detection latency ~37 s.

### 1.5 The 12-step barrier does not scale with membership — the FRAME COUNT does

Measured, not assumed (41 captures, 40 transitions, 30 completed barriers):

- **Every** completed barrier tops out at exactly step 12. M=2 (16 barriers),
  M=3 (11), M=4 (3): **zero variance**, and the same for a class-`0x03` removal.
- What scales is `#0x0b = #0x0c = 12 × (M−1)`, exact in 30/30, in a **star** around
  the coordinator, **one lock-stepped cluster-wide barrier** (`0x0c#N` never
  precedes the last `0x0b#N` — 0 violations in every transition).
- **An ordinary member's cost is flat**: always 12 out, 12 back, whatever the
  cluster size. The fan-out is the *coordinator's* obligation, inherited only at T3.
- The one member-side consequence is **latency** — the coordinator holds each
  release until the slowest member reports. **Do not time out on a slow step.**
- No peer announces the step total, so 12 stays a constant. **The honest bound is
  FOUR members**, and OVMX is one of the four.

Also corrected a standing misreading: `body[55]` is not the "third mutation" of
the `op 0x09` echo — it is the coordinator's **membership bitmap** (popcount ==
member count, 54/54), and the responder zeroes it because it is not the
responder's to assert. Spec §4(p) and the new §4(r) carry all of this.

## 2. How the addition case was finally solved

`by11` shipped the reciprocal-config fix. It was **correct, necessary, and not
sufficient** — the frames were right, at +0.1 ms against a reference 0.3–0.9 ms,
with a byte-exact SYSAP header. Two independent capture analyses then converged
on the real blocker:

> **OVMX was advertising the JOINER form of `cat 0x01 op 0x01` while sitting in
> the cluster as a MEMBER** — 131 of 132 body bytes identical to what a node
> emits when it is in no cluster at all.

A newcomer asks every member *"what cluster are you in?"*. VAX1 and VAX2 answered
"3 members, formed 02:02:11.14, last transition 02:05:31.79" — that transition
being OVMX's own admission. OVMX answered **"no cluster, 0 members, admitted
1-JAN-2001"**, i.e. 25 years before the cluster it was in was formed. The newcomer
could not close its view of the membership and never issued its add-member
request, to anyone, for 678 s.

It worked during OVMX's own join *because that is what OVMX was*. The encoding
was simply never updated when OVMX became a member.

Seven fields, each grounded by a controlled variation **inside a single capture**
— full map and provenance in `docs/analysis-e81-by11.md`. Fixed in `88c98a7`,
with `cluster_formed` / `last_transition` **copied verbatim from a member's own
`op 0x01` and never computed** (Rule 10 — they are facts about the cluster, not
about us). `SCSD-I-CLUSTATE` logs the copy live. Also removed: OVMX was sending
the **joiner's** `op 0x02` to the newcomer — an established member asking a
non-member to admit *it*.

**The discriminator, now answered.** The analysis left two readings open. In
`by13`, VAX3 sent **no `cat 0x01 op 0x02` to OVMX** — it aimed its add-member
request at a real VAX. So the gate was **cross-peer consistency**, not
last-reciprocator selection: a newcomer requires *every* peer it holds a VC with
to advertise a coherent member-form cluster state, but does not necessarily then
choose that peer as its coordinator.

**Third time the premise inverted the same way on this item** — the newcomer
looked like it was ignoring us and was blocked on us. Missing op-1 CONNECT-ECHO,
then missing reciprocal config, then a reciprocal that said the wrong thing.

## 3. ⚠ WHERE TO START NEXT SESSION

**Nothing is broken and nothing is half-finished.** Tree clean, 10/10 vmsscs
tests, lab healthy (3 VAXes up from `by13`'s `reset2` + `boot3`), every result
above reproducible. `vms-e81` and `vms-e4b` are **closed**.

The honest question for the next session is **which tier to work**, and that is a
prioritisation call rather than a blocked one. Three candidates, in the order I
would rank them:

1. **Finish T1 (`vms-32b`).** Its remaining children are `vms-ae5` (node
   leaves/fails — *largely demonstrated already by `rm1`*, so this may be close to
   free), `vms-b8a` (explicit leave), `vms-c7d` (VC breakage), `vms-405` (cluster
   group + password), `vms-7d4` (undecoded surface). Closing T1 unblocks **T2
   (`vms-d66`)**, which is where OVMX starts contributing resources.
2. **Clear the polish list in §4.** All non-blocking, all grounded, all small.
   Doing them together is one cheap commit and removes several authenticity tells.
3. **`vms-584` item 5** (join/exit cycling to drive the epoch past the node count)
   — ideal unattended work, and `tools/removal.sh` is most of the harness already.
   Item 1 (4–5 nodes) dropped in priority once the barrier-scaling risk was
   retired by census; it is now an *extension* of a retired risk, not a live one.

**If you want a single next experiment rather than a decision:** run
`tools/removal.sh` again but kill **VAX1** (the cluster founder and the only
voting node) instead of VAX3. That is a quorum event, not just a membership one,
and nothing in our evidence covers it. Expect it to be informative whether it
succeeds or hangs.

## 4. Grounded polish — all non-blocking, none shipped

Held back deliberately so each run attributed to one change. None of these
prevents membership.

1. **Our `op 0x14` model string is 17 bytes** (`"OVMX Cluster Node"`) where the
   reference emits 21 (`"VAXserver 3900 Series"`). An **authenticity tell**, not a
   blocker — VAX1 and VAX2 both accepted ours during OVMX's own join. Note the
   tension with INV-0: announcing ourselves honestly is deliberate policy, so this
   is a *decision* to take, not obviously a bug to fix.
2. **Two stray poll-tick `cat 0x04` acks.** The reference member's first cat-`0x04`
   acknowledges the peer's `op 0x02`, not its `op 0x01`, and it never acks an ack.
   Ours come from the poll-loop ack-backlog flush, which is why they land ~6.6 s
   late on a tick — the tick, not the protocol, is choosing when OVMX speaks.
3. **Member-initiated connect-back timing — RE-DERIVE, do not assume.** This was
   filed as "~11 minutes early", but `by11` frame 2980 shows **VAX1 doing the same
   thing**, which contradicts that reading. Ground it before changing it.
4. **The constant local Con.ID** (`vms-298`'s unfixed half). `OVMX_MSCP_SERVER_CONID`,
   `SCS_DIR_OVMX_CONID`, `OVMX_JOINER_CONID`, `OVMX_MSCP_CONID` are all
   compile-time constants. A Con.ID identifies a connection *endpoint* and real
   nodes allocate a fresh pair per connection. Touches the working join path —
   ground it first.
5. **Our learned `member_count` can go stale.** In `by13` we copied `2` from a
   peer before our own admission and nothing re-taught us `3`. It is *grounded*
   that the value should become 3 (a member's `op 0x01` tracks live membership),
   but we do not currently re-learn it. It did not stop VAX3 joining, which is
   itself interesting — the newcomer may key on the member *form* rather than on
   the count agreeing.

## 5. Lab tooling added this session

| script | what it does |
|---|---|
| `tools/bystander.sh` | reset2 → OVMX joins → **verified settled member** → boot VAX3 into the live cluster → verify 4 nodes. The class-`0x02` bystander test. |
| `tools/removal.sh` | reset3 → OVMX joins (4 nodes) → **kill VAX3's SIMH outright** → verify 3 nodes with OVMX among them. The class-`0x03` bystander test. |

Both abort on an unverified precondition. **Both had to be fixed for a bug worth
remembering:** `waitnodes.sh N | tee -a $STAT` returns *tee's* exit status, so
`|| die` never fires — run `by10` printed `waitnodes: FAILED -- wanted 4` and the
very next line claimed `*** FOUR NODES INCLUDING OVMX ***`. There is now a `wn()`
wrapper that preserves the real status. **A pipeline that discards a verdict is
the same failure as not checking at all**, and it is harder to see.

Run naming so far: `v1` (plain join), `by9`–`by13` (bystander addition), `rm1`
(removal). **Last SCSSYSTEMID used: 1209.**

## 6. Guardrails — earned, not assumed

The five from session g still hold verbatim (never answer an ungrounded
`(cat, op)`; being faster than the reference is a compatibility bug; a repair that
fires during normal operation is worse than the fault; one reset one believable
run; copy console logs out before the next reset; generalising from one leg of one
capture is the recurring error). Three more earned this session:

7. **An opcode is not an identifier — and knowing that is not the same as
   applying it.** `op 0x0d` is the class-`0x04` transition-open in category `0x01`
   and the DLM rebuild record in category `0x02`, and a single join carries 216 of
   the latter. I wrote a commit message warning about exactly this collision and
   then, two hundred lines away, matched on the opcode alone. Every lock record
   latched as a transition open; the barrier carried a corrupted epoch into step 6
   and stalled. **Qualify by category at every site.**
8. **A cross-check must LOG, not GATE.** The above was caught in under a minute
   because the role cross-check *warned* 223 times. My first version *gated* the
   latch on `role == 0x40`, which looked more defensive — and would have silently
   skipped the DLM records, made the code correct by accident, and shipped the real
   defect invisibly. **A guard that hides a bug is worse than no guard.**
9. **When a peer goes quiet, suspect what you owe it.** Three times on this item
   the symptom read as "the newcomer ignores us" and three times it was blocked on
   an obligation the reference discharges immediately and unconditionally. Before
   theorising about the peer, diff what a real member emits toward it — as an
   **ordered checklist** — and find your first MISSING row.
