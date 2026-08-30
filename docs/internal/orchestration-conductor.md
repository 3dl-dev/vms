# OVMX Multi-Agent Conductor — Runbook (BOOT FROM HERE)

> **Internal orchestration process — not product documentation.** This runbook describes
> the OVMX agent-swarm operating model, not the OVMX product. It lives under
> `docs/internal/`; do not cite it from user-facing docs.

This file is **static** (the operating model + invariants). Live state is re-derived from ground
truth every tick, never trusted from a stored field. Read this to boot as the **main conductor**,
to onboard as a **new peer lane**, or to resume a lane that was restarted/RL-killed/compacted.

## The model — one conductor, N peer sub-conductors, ephemeral sub-agents

```
                Operator (Baron) — reserved decisions only
                          │
        ┌─────────────────┴──────────────────┐
        │   MAIN / ACP CONDUCTOR (long-lived) │  release train · roadmap · cross-lane
        │   compaction-surviving, stays LEAN  │  coordination · reaps · tags · cascade
        └───────┬───────────┬───────────┬─────┘
                │           │           │            (peers coordinate THROUGH the conductor)
        ┌───────┴──┐  ┌─────┴────┐  ┌───┴──────┐   … as many peer lanes as the operator musters
        │ PEER LANE │  │ PEER LANE│  │ PEER LANE│   each = a long-lived SUB-CONDUCTOR for one domain
        │  (Alpha)  │  │  (VAX)   │  │  (GCC)   │
        └─────┬─────┘  └────┬─────┘  └────┬─────┘
              │             │             │
         ephemeral     ephemeral     ephemeral     ← CLEAN sub-agents do the tool-heavy work
         sub-agents     sub-agents    sub-agents      (builds, qemu/SIMH runs, multi-file edits,
                                                       investigations) and report a ONE-LINE VERDICT
```

**Why this shape (the token/durability rationale):** development is tool-call-heavy. A long-lived
session that runs 40-min builds and multi-file edits in its own context bloats, churns compaction,
and burns tokens. So the tool-heavy work goes to **clean ephemeral sub-agents** that return a
one-line verdict + a PR#; the **long-lived conductors** (main + each peer) stay lean, survive
compaction, and just coordinate/decide/report. A conductor that is doing direct implementation has
drifted — pull it back to delegation.

## Boot / onboard / resume sequence (IDENTICAL for all three)

This is waking from sleep, not being born. The first question is "what was I executing?" — answer
it by RE-DERIVING from ground truth, never by trusting a remembered or stored status field.

1. Read this runbook, then `docs/internal/lane-ownership.md` (which lane is yours / claim one), then
   `docs/internal/conductor-state.md` (the live execution pointer). AGENTS.md is the contributor-surface
   truth; rd is the private work ledger.
2. **Re-derive the live world** (do not trust any state file's status):
   - `rd ready` / `rd list --json` — what's claimed / ready, per lane
   - `gh pr list --state open --json number,title,headRefName` — in-flight PRs
   - `git log --oneline -20 origin/main` + `git tag --sort=-creatordate` — what has landed / shipped
   - `ListAgents` — which peers + sub-agents are alive right now
3. Reconcile stored vs ground truth (**ground truth wins**), run ONE tick, then re-arm your wake.
   Continue until the objective is met or a reserved decision blocks you.

A **restarted / RL-killed / compacted** lane resumes the SAME way: its state is externalized (its
rd item carries a resume-note; its WIP is on a pushed branch), so it self-onboards from this
sequence — the conductor should NOT have to re-explain the lane from scratch. (When that fails, it
is a durability bug: fix the externalized state, not the conductor's memory.)

## Tick protocol (each wake): REAP → ASSESS → DISPATCH → ESCALATE → REPORT → PERSIST

1. **REAP.** For each open in-lane PR: read its checks. If the real gate is green **by-SHA**
   (see gates below; treat the vms-898a heavy-TCG flake tier as no-new-vs-baseline, not
   absolute-green), squash-merge; confirm main didn't regress; if it did, `git revert` and kick
   the item back to its lane with the failing check. Close the rd item with a reason.
2. **ASSESS.** Re-derive per-lane state. Is any lane idle-waiting (finished a turn, nothing queued)?
   That is the failure mode — every lane must always have a next item queued.
3. **DISPATCH.** Feed each idle lane its next item. Heavy work → the lane's own sub-agent, never the
   lane's long-lived context. Keep ≥2 things churning; never narrow to one item + a long wait.
4. **ESCALATE.** Reserved decisions (below) → tee up the question + your recommendation + what you'll
   do absent an answer, then proceed on everything that doesn't depend on it.
5. **REPORT.** To the operator: the number/headline, not the narration.
6. **PERSIST.** Rewrite `docs/internal/conductor-state.md` to the new pointer; re-arm the wake.

## Standing directives (invariants — check every tick, never assume)

- **All-arches simultaneous (release gate).** Tag a release only when {x86_64 + aarch64 + VAX + Alpha}
  are ALL green-by-SHA on the exact cut commit (boot + acceptance + co-release), via a frozen-verify
  dispatch — not x86_64 plus a build-only co-release check. No four-implementations-that-drift.
- **Single-source everything.** One source per surface (rd labels / git tags / `docs/compat/*.yaml`);
  all else GENERATED. Version: the site + demos DERIVE from `roadmap.json releases[0].tag`; the
  reconcile emits `meta.nextPointRelease`; PAYLOAD_VER stays build-stamped. New ledgers are forbidden.
- **Max shared code (convergence).** New functional code goes in shared-core, compiled for all four
  arches; per-arch only for genuine ABI/hardware (syscall#, register conv, `.S`, endianness, width,
  PAL). A feature present on one arch is the SAME code on all. Program: `vms-1d6`.
- **Sub-conductor delegation.** Every conductor delegates build/qemu/SIMH/multi-file/investigation
  work to clean ephemeral sub-agents that return a one-line verdict + PR#. Stay lean.
- **Build-time is authentic-fast, not slow-and-thorough.** Layer (don't rebuild from scratch), split
  build-the-world from run-the-proof, KVM not TCG where possible, right-size, use the k3s rail for
  heavy builds off the shared host. NEVER cache a proof RESULT or skip a real gate. Surgical prune
  only (never `docker builder prune -f`; never rm `$REPO/.boot-cache`). Program: `vms-8a4`.
- **Fail honest / INV-6 / measure-first.** Executive facilities fail honestly, never fake success.
  Tests decide on real exit codes, not console substrings. Before fixing, MEASURE the mechanism
  (an item's premise can be wrong — verify against the oracle, not a hypothesis).
- **Release cadence never stops.** Cut a point release at ≥7 items or immediately on a blocker fix;
  run the full post-tag cascade (reconcile → roadmap PR → site PR → verify the live Pages deploy).

## Reserved for the operator (escalate, don't decide)

Product scope (removing/deferring a feature); anything irreversible/externally-visible (public
exposure, spend, external comms); security posture; weakening/skipping a test; host-capacity calls
(killing a tenant, freeing RAM, the k3s rail); facts only the operator holds.

## Mustering a new peer lane

The operator spins up a peer session; it becomes a sub-conductor for a domain. To onboard: it runs
the boot sequence above, claims a lane in `docs/internal/lane-ownership.md` (one owner per hot file — honor
it or two lanes will clobber each other), reads `rd ready` for its domain, and starts ticking. The
main conductor release-gates its shared-core work and reaps its PRs green-by-SHA. Adding an Nth peer
is: claim a lane + start the tick loop — no other setup. Foreign-arch/heavy work still goes to that
peer's own sub-agents.

## Durability checklist (what makes this survive a kill / scale to N)

- Every lane's WIP is on a **pushed branch** before any long build (an RL-kill mid-build loses
  nothing recoverable).
- Every in-flight item has an **rd resume-note** so a restarted lane self-onboards.
- These three docs live **on main** (not untracked in one checkout).
- The conductor holds no lane's tool-output — only verdicts, PR#s, and the pointer.
