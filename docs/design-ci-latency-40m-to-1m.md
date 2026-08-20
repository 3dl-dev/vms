# CI Latency: 40m → sub-minute iteration signal

**Status:** in progress (2026-08-20). Epic **vms-fb8**. **Owner decision points flagged `⚠ OPERATOR`.**

Operator ask: *"40-minute runs are unacceptable for high-throughput iteration. We run
5× wide parallel peers. Take it from 40m to under 1m."*

## The honest target split (read this first)

You cannot boot a full VMS-personality OS, install it, upgrade it, boot the upgrade, and
run a release-acceptance PARTS scenario in under 60 seconds. That is physics, not tuning.
So "under 1 minute" resolves into two different numbers:

| Signal | Today | Target | How |
|---|---|---|---|
| **Per-PR iteration gate** (what a peer blocks on) | **~46m** | **~2–4m now, ~1–2m after follow-ons** | tier-split (this PR) + build-job caching (follow-on) |
| **Full merge gate** (green before landing on `main`) | **~46m** | **~4–6m** | KVM (this PR) + build-once artifact (follow-on) |
| Nightly full suite | 60m | unchanged | coverage moved here stays here |

The heavy 15–46m e2e wall leaves the iteration loop entirely in this PR. The literal
sub-60s per-PR floor is bounded by two non-QEMU jobs (build+test's ctest, static
analysis's clang-tidy) and is chased in the follow-ons — it is not free.

## Measured diagnosis (run 32317162722, a core PR, 46.0m wall)

90 jobs. ~79 finish under 10m and are **not** the problem. Wall = the single slowest job.
Critical path = ~11 heavy QEMU e2e jobs, each = `cut-release` (~19m) + image build + boot/
verify (~25m). Root causes, all confirmed in source:

1. **No hardware acceleration.** All jobs `runs-on: ubuntu-latest`; `run-qemu.sh` x86_64
   sets `MACHINE=""`, `-smp 2` → **TCG software emulation**. `run_tests.sh` comment:
   *"a normal runner does the full 76-suite run in ~90s; GitHub's TCG runner is ~10×
   slower."*
2. **Redundant heavy builds, no artifact sharing.** `cut-release` (~19m) rebuilt in ≥3
   jobs; the bootable image in ~11. Docker layer cache helps the image, not the cut/disk.
3. **Full e2e blocks every core PR.** The 11 heavy jobs ran on the per-PR path.
   **Finding:** `main` has **no branch protection and no merge queue** — so today `merge_group`
   never fires; pre-merge coverage came entirely from the per-PR run, and the conductor
   reaps by verifying green-by-SHA manually.

## What THIS PR does (work/vms-fb8-ci-latency)

**A — KVM acceleration (guarded, safe).** `distro/boot/run-qemu.sh`,
`tests/qemu/run_tests.sh`, `tests/qemu/release_install_inner.sh` now select
`-accel kvm -cpu host` when `/dev/kvm` is writable, else `-accel tcg` (identical to
today's behavior — no regression if a runner lacks KVM). GitHub-hosted Linux runners
expose `/dev/kvm`; the first run measures the real speedup. Accelerates the kernel-executive
shards **on the PR path** and every e2e job on the merge/dispatch path (~10×).

**C — Tier-split.** The 12 heavy release/install/upgrade/boot-scenario/UAT e2e jobs now
carry `if: github.event_name != 'pull_request'` — they run on push/merge_group/
workflow_dispatch/schedule, **skipped on PRs**. Per-PR keeps the fast tier. Documented in
the `ci.yml` header.

**Coverage-before-merge, with no merge queue (the bridge):** the reap gate MUST fire a
`workflow_dispatch` full run on the candidate branch and verify it GREEN before squash-
merge (`gh workflow run ci.yml --ref <branch>`; forces every output true = full suite;
KVM-accelerated ≈ single-digit minutes). The by-SHA proof still exists — fired at reap
time instead of on every push. `⚠ OPERATOR`: the durable fix is to **enable a GitHub merge
queue** on `main` (then `merge_group` runs the full suite automatically before landing).
That is a repo-settings + merge-posture change reserved to you; the `workflow_dispatch`
bridge holds the invariant until then.

## Follow-ons (children of vms-fb8, tracked, dispatchable)

- **D1 — static-analysis on changed files only (PR).** clang-tidy/cppcheck currently sweep
  the whole tree (~9.8m, not incremental). On a PR, lint only files changed vs the base.
  Biggest remaining per-PR ceiling.
- **D2 — ccache / build-split for build-and-test.** Persistent ccache; consider splitting
  compile+fast-unit (PR) from the heavier ctest integration gates (merge/dispatch).
- **B — build-once release artifact.** One `build-release-artifact` job runs `cut-release`
  + image once, `upload-artifact`; the heavy e2e jobs `download-artifact` and only boot.
  Collapses 11×(~19m build) into 1. Primarily cuts the **merge-gate** time.
- **B' — self-hosted KVM pool on `workshop`.** Only if hosted KVM proves absent/throttled.
  `⚠ OPERATOR — SECURITY POSTURE`: self-hosted runners must never execute untrusted
  fork-PR code; gate to same-repo refs (our peers push branches into the repo, not forks).

## Expected end state
- Per-PR blocking verdict: **~2–4m now** (heavy e2e gone; ceiling = static-analysis /
  build-and-test), **~1–2m** after D1/D2.
- Full merge gate: **~4–6m** (KVM + build-once), down from 46m.
- Nightly: same coverage, off everyone's critical path.

## Risks / non-goals
- No test deleted or weakened (Rule 7/9). Coverage moves lanes, never disappears.
- KVM changes the emulated CPU (`-cpu host`): re-baseline any TCG-specific assertions in
  `facility_defects.sh` if the first accelerated run flags them (they read as observations
  about the emulator; verify host-CPU-independence).
- The `workflow_dispatch` pre-merge run is a **process** guarantee until a merge queue is
  enabled — it depends on the reaper actually firing it. Enabling the merge queue makes it
  mechanical.
