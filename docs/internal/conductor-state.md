# Conductor — live pointer (re-derive the rest from rd / gh / git; do not maintain a parallel state layer)

> **Internal orchestration state — not product documentation.** This file is the OVMX
> agent-swarm's live execution pointer, not documentation of the OVMX product. It lives
> under `docs/internal/`; do not cite it from user-facing docs.

**ENGINE RULE (why it once went idle):** keep every lane with a next item queued (never idle-waiting
on the conductor); push-first agents (commit+push before any long build); delegate build-heavy work
to ephemeral sub-agents; short heartbeat. Never narrow to one item + a long wait.

**As-of 2026-08-27.** Re-derive the live world every tick — this pointer is a hint, not truth:
`gh pr list --state open`, `git log --oneline origin/main`, `git tag --sort=-creatordate`,
`rd ready`, `ListAgents`.

**Shipped:** V0.5-5 (tag, `bf034e4c`) — the FIRST release cut through the all-arches gate
{x86_64 + aarch64 + VAX + Alpha green-by-SHA on the cut commit}. Site + demos single-source the
version from `roadmap.json releases[0].tag`; rail-next shows `meta.nextPointRelease`.

**Accreting:** V0.5-6 — reap the open PRs green-by-SHA as they land; cut at ≥7 items.

**Flagship in flight:** vms-f60d Tier-1 — the genuine VMS-standard 6-arg activation proven
end-to-end on qemu-system-alpha (IMGACT.EXE OVMX-SEAM getexit readback of `$STATUS` on real
/dev/vms). The do-it-like-VMS ladder's activation half. Co-lands #799 (stdcall R27 fix) + #793
(instrumentation) + the Alpha seam runner when the OVMX-SEAM line fires.

**Standing programs:** convergence (`vms-1d6`, retire the ~8k-line legacy VFS driver = biggest win),
build-time reduction (`vms-8a4`), release-eng (`vms-a84`), authenticity/executive-boundary (`vms-040`).

**Open operator-reserved:** shared-host RAM pressure (the enterprise-ai tenant vs the k3s rail for
heavy builds); any product-scope / test-weakening / external-exposure call.

**Runbook + invariants:** `docs/internal/orchestration-conductor.md`. Lane registry: `docs/internal/lane-ownership.md`.
