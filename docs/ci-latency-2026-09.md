# CI latency: measured, 2026-09-25

rd vms-6bf. Question: why does going from PR to mergeable take hours, and what gets a
typical PR's required signal under 45 minutes? This page records what was measured,
what changed, and the one decision that belongs to the operator (self-hosted runners).

**Data.** All 400 workflow runs from 2026-09-24 17:27Z to 2026-09-25 14:22Z, fetched
with `gh api`: 6,107 jobs that actually ran (skipped jobs excluded). `ci.yml` was split
into the `ci-*.yml` siblings at about 22:40Z on 09-24, so the split-workflow numbers
cover roughly 15 hours. On top of that: the pull_request critical path of the last 30
merged PRs, and a scratch probe job to see whether `/dev/kvm` is usable.

## Headline

| Signal | Before | After | Change |
|---|---|---|---|
| **PR checks, typical PR** (first PR run created → last PR job finished; last 30 merged PRs) | median ~20 min, range 0.5–26 min | same | Already under 45 min. The split + `ci-changes.yml` path filter did this before this work started |
| **PR checks, the 5 named PRs** | #1306 20.3 · #1307 20.7 · #1308 3.7 · #1310 10.4 · #1311 20.6 min | same | |
| **Heavy validation of a kernel/executive PR** (`CI / Kernel Executive` via workflow_dispatch) | median **121 min**, max **252 min** (n=11); push to main median 159 min | negctl matrix **44.9 min** end to end (#1313). KE wall is now set by the NetBSD/amd64 TCG jobs (25–75 min) and negctl shard 21 (~40 min) | This is where the "hours" came from. Still over 45 min: vms-8a8, vms-fa5, vms-4e72 |
| **Heavy validation of a release-path PR** (`CI / Release / E2E` via workflow_dispatch) | 37–89 min (n=5) | 44.8 min on the KVM branch | Bounded by `cut-release is byte-reproducible`, which is build-bound |

**Where the "hours" came from.** A PR's own checks finish in about 20 minutes. The
PRs that took hours to merge were the ones where someone also dispatched the full heavy
suite on the branch:

| PR | open → merge | PR checks | Heavy dispatch runs on the same SHA |
|---|---|---|---|
| #1305 | 614 min | 21.1 min | KE 115, 117, 122, 249 min |
| #1304 | 512 min | 11.1 min | KE 121, 238 min |
| #1296 | 492 min | 24.9 min | KE 121 min |
| #1301 | 94 min | 11.1 min | KE 127 min |
| #1307 | 117 min | 20.7 min | Release/E2E 37, 53, 89 min |

## Top time sinks

| # | Sink | Where it bites | Measured | Status |
|---|---|---|---|---|
| 1 | **negctl `max-parallel: 3`**: 22 shards run in ~8 sequential waves | KE push, dispatch, schedule | Shard queue wait (created → started) median **32.6 min**, max **87.7 min** (n=374 jobs). KE wall median 121 min (dispatch), 159 min (push). | **Fixed, #1313.** All 22 shards start within 4 s; the matrix finishes in 44.9 min (run 36149894658, 22/22 green). |
| 2 | **NetBSD/amd64 guests under TCG** (Event-Flag, Cross-Process, Pseudo-Device, Harness) | KE push/dispatch | Event-Flag execution median 32.6 min, max 52.7 min, **5 of 10 runs failed**. On 09-25 the same jobs took 23–75 min. | KVM tried: the NetBSD 10.1 install drops into ddb (`--db_more--`) under KVM, so these jobs were left on TCG. Tracked as vms-8a8 (KVM for NetBSD) and vms-4e72 (existing Event-Flag flake). |
| 3 | **One slow defect**: `lock-deq-status-wrong` runs to the 1800 s inner wall | negctl shard 21 | Shard 21 median **40 min**; the other 21 shards take 11–18 min | Filed as vms-fa5. Now that the cap is gone, this is the floor of the negctl matrix. |
| 4 | **`cut-release is byte-reproducible`**: two sequential `--no-cache` cuts | Release/E2E push/dispatch | Cut A 21.0 min + cut B 21.5 min; job median 44 min | Unchanged. Running the cuts as two parallel jobs would save ~20 min, but both need the same ephemeral signing key, and moving a private key between jobs is a security-posture choice. Not done. |
| 5 | **"Build bootable image" on the PR path**: three PR jobs each build `distro/Dockerfile.bootable` | Release/E2E pull_request (DCL/SHOW, Console boot, Alternate-disk) | 13–15 min median per job. A PR changes source, so the compile layers rebuild; the cache works as designed. | Unchanged; the PR path is already ~20 min. Next lever: a ccache mount in the Dockerfile. |
| 6 | **x86_64 QEMU under TCG** | KE shards, negctl, release e2e boots | negctl test step median **10.9 → 7.2 min** with KVM. Pristine all-suite boot 44 s → 17 s. 21 of 22 shards finish in 20.8 min. | #1314 enables KVM. Under KVM, the pristine `test_syssvc_procnam` hung in P13 (vms-d90). The cause was a race in the test's own ptrace tracer, which lost a held child. `$CREPRC` was not at fault. The fix is #1317. About 35 release e2e boot scripts also hardcode `MACHINE=""` (TCG) and need the guard: vms-5486. |
| 7 | **qemu-system-alpha** (TCG only on x86 hosts) | Alpha PR jobs | 10–20 min per job | Can't be accelerated on hosted x86 runners. Already runs in parallel, so it doesn't set the PR critical path. |

## Things checked that are *not* the problem

- **Runner queue.** Every job outside the capped negctl matrix queued a median of 0.0–0.1 min (max 5.0 min). At peak, **303 jobs ran at once** account-wide (223 ubuntu-latest, 80 ubuntu-24.04-arm). No runner-concurrency limit is being hit.
- **Superseded PR runs.** Every `ci-*.yml` already uses `concurrency: ci-<workflow>-<ref>` with `cancel-in-progress` for `pull_request`. Push and workflow_dispatch runs *don't* cancel; they queue in the same ref group. A second dispatch on the same branch waits for the first one to finish, so cancel the superseded one yourself.
- **Path filters.** `ci-changes.yml` works. A docs-only PR (#1308) had a 3.7 min critical path.
- **Docker layer cache.** On push to main, build steps restore from GHA cache (negctl image build median 0.7 min, KE image 3.3 min). Longer builds on a PR are the source layers that PR changed, not cache misses.

## KVM on GitHub-hosted runners (probe, run 36148722291)

| Runner | `/dev/kvm` | Usable as `runner`? | After udev rule | `docker run --device /dev/kvm` |
|---|---|---|---|---|
| ubuntu-latest (4 vCPU, 16 GB) | `crw-rw---- root kvm` | no (runner isn't in group kvm) | `crw-rw-rw-`, writable | works |
| ubuntu-24.04-arm | absent | n/a | absent | n/a |

The workflow comments that say "GitHub runners have no /dev/kvm" are out of date. The
earlier vms-fb8 work added `[ -w /dev/kvm ]` guards to a few harnesses but never made the
device usable. The one job that passed `--device /dev/kvm` itself (2-node
CLUSTER_CONFIG_LAN) was already running `accel=kvm` on main. `.github/actions/enable-kvm`
(#1314) applies the udev rule and adds a `docker run` shim that inserts `--device
/dev/kvm`. It's wired into the x86 QEMU jobs in Core Gates, Kernel Executive and
Release/E2E, but not into the NetBSD/amd64 jobs (sink #2). #1314 waited on vms-d90. Under KVM,
the pristine suite hung in P13 (1 hit in about 55 KVM runs of that suite, 0 in 374 TCG shard
runs). The cause was P13's ptrace tracer: it detached a held fork child before the child
had stopped, got ESRCH, and left the child stopped forever. It was not a `$CREPRC` race.
Fixed in #1317. Before the fix, 55 of 90 KVM runs hung with P13 at 60 calls. After it,
0 of 90 hung.

## Self-hosted runners on the k3s rail (vms-101): operator decision

**This is not done.** A public repo that runs `pull_request` workflows on our own
hardware would let a fork PR execute code on our network. That's a security-posture call,
so it's Baron's. The operator trail on vms-101 also conflicts: 2026-08-20 and 08-28 say
"no self-hosted GitHub actions" and "no GitHub Actions on the mainframe"; the conductor's
2026-09-12 note says GO.

**Expected speedup, from the numbers above:**

| Lever self-hosting would give | Is it the bottleneck? | Expected gain |
|---|---|---|
| Less queue wait | No. Queue is ~0.1 min with 300 jobs running at once | none |
| KVM | Hosted x86 runners expose it (#1314) | none extra (except NetBSD, sink #2, which is a guest/CPU-model issue and would show up on our KVM hosts too) |
| Warm persistent Docker/ccache | Partly (sink #5, 13–15 min per PR-path build) | maybe 5–10 min off the PR path, and ccache on hosted runners gets most of that |
| Bigger boxes | k3s-worker is 8 CPU / 53 GB, versus 4 vCPU / 16 GB per hosted job | faster per job, but **one worker node gives far less fan-out**: a KE dispatch is ~36 jobs, which fit in about 4–6 concurrent QEMU jobs on one node, so ~6–9 waves. That's **slower** than hosted, where all of them start at once |

**If Baron wants it anyway, the isolation shape is:**
- runners serve only `push` to main, `workflow_dispatch`, `schedule`, and PRs a maintainer labels (`pull_request_target` gated on a label). Never raw fork `pull_request`;
- ephemeral single-job runner pods (ARC `ephemeral: true`) in their own namespace with ResourceQuota, NetworkPolicy egress-only, no cluster credentials and no secrets, and `/dev/kvm` via a device plugin;
- keep hosted runners as the default and the fallback, and route only the heavy KVM jobs with `runs-on: [self-hosted, kvm]` behind the same event gate.

**Recommendation: don't move CI to self-hosted runners for latency.** The measured
bottlenecks were a self-imposed matrix cap and TCG, and both are fixed or being fixed on
hosted runners. Keep vms-101 for what the trail says the rail is for: native k3s Jobs for
heavy lab and proof runs outside GitHub (its rail-egress gate). Revisit only if a
per-PR job needs hardware that hosted runners don't have.

## Changes merged under vms-6bf

| PR | Change | Job set | Before → after |
|---|---|---|---|
| #1313 | Remove negctl `max-parallel: 3` | unchanged (superset diff) | negctl matrix ~100+ min (KE wall median 121) → **44.9 min** |
| #1314 | `.github/actions/enable-kvm` in the x86 QEMU jobs (not NetBSD) | unchanged; one step added per job | negctl test step median 10.9 → 7.2 min; 21/22 shards in 20.8 min |

No test was deleted, skipped, relocated or weakened. No job moved off the PR path.

## Follow-ups filed

- vms-d90 (p1, fixed in #1317): P13 "`sys$creprc` did not return" under KVM. Cause: a race in the test's ptrace tracer (a detach before the held child had stopped). `$CREPRC` was not at fault.
- vms-fa5: negctl shard 21 / `lock-deq-status-wrong` sets a ~40 min floor.
- vms-5486: add the KVM guard to the ~35 x86 boot scripts that hardcode TCG.
- vms-8a8: NetBSD 10.1/amd64 enters ddb under KVM; its jobs stay on TCG (25–75 min).
- vms-d26f (existing): `test_syssvc_mmk_build` pristine flake. 18 of 20 failed negctl shards in the window; also reproduces under KVM.
- vms-4e72 (existing): NetBSD/amd64 Event-Flag flake (5 of 10 runs failed).
