# Design: CI throughput — same coverage, a fraction of the wall-clock

2026-08-08. Owner lane: `.github/workflows/**` + CI helper scripts. Companion to
`docs/release-plan-0.2-to-0.5.md` §1–2 (why last week's waves broke main and why
per-PR CI latency is the scaling bottleneck). Tracks as rd CI-throughput item.

## Framing (read this first)

This is **not** "cheap testing." It is **implementation-expensive,
operationally-efficient, uncompromised-quality testing.** The cost is paid
up-front, in the CI *infrastructure*; **zero product coverage is traded for
speed.** Every check that runs today still runs — the change is *when* and *for
which PRs* it runs, chosen so the verdict is provably identical:

- **Path-impact selection** — a `src/vmsscs/**`-only PR genuinely cannot change
  the outcome of the kernel-executive or LINK.EXE toolchain jobs. Skipping them
  on that PR is *quality-preserving* (same inputs → same verdict), not a coverage
  cut. Every job still runs unconditionally on push-to-`main`, in the merge
  queue, and nightly, so the union of what gates `main` is unchanged.
- **The ONLY tier demotion** is of checks that are *inherently slow AND not a
  product check*: the clean-rebuild-from-scratch **negative controls** that exist
  to prove the harness can go red. Those are a nightly/post-merge property by
  nature; keeping them off the per-PR path removes zero product coverage.
- Real product checks that happen to be slow are **made fast and kept pre-merge**
  (content-hash build caching, incremental rebuild, cached QEMU image), never
  demoted.

Headline: **same coverage, a fraction of the wall-clock, via real infrastructure
investment.**

## 1. Profile (measured, run 31218140101 on `main`, 2026-08-07)

~40 jobs, all `runs-on: ubuntu-latest`, no cross-job `needs` except the
facility-negctl aggregate. They run in parallel, so **per-PR latency = the
slowest required job**, and **cost = the sum of all job-minutes** (which also
drives queue contention: under concurrent PRs, runners saturate and everything
slows — the facility-negctl shards measured 27m→50m→>60m under load, rd vms-86a).

Long poles (wall-clock, sorted):

| Job | min | What it does / why it's long |
|---|---|---|
| **Build & Test** | **25.4** | Full tree build + `ctest`. Dominated by 3 `slow`-labelled per-control **negative-control** loops (`kif_caller_census_negctl` T/O 1800s, `userspace_service_register_negctl` 1500s, `terminal_identity_negctl` 600s) that recompile the tree per control (vms-819d). |
| **Persistent Boot Smoke Test** | 13.9 | Builds bootable image (gha layer cache) + 3 QEMU boots. |
| Facility neg-controls (6 shards) | ~5–6.5 ea | Clean per-facility rebuild + QEMU boot per injected defect. **Harness self-test.** |
| Static Analysis | 6.0 | Full build + clang-tidy + cppcheck (informational, always exit 0). |
| ~29 VMS-native toolchain jobs | 0.2–5.8 ea | Each independently rebuilds LINK.EXE + the shareable graph (DECC$SHR + 5 libs) in an Alpine container, most under **arm64 emulation**, then links/activates one specimen. Huge *redundant rebuild* across jobs. |
| Conformance / Corpus | 0.5–0.9 | Disposable ubuntu container build + run. |
| Kernel Executive (real /dev/vms, QEMU) | 1.3 | The ONE job that insmods `vms.ko` and asserts against `/dev/vms`. **Real product check.** |
| Kernel Executive — Negative Control | 1.3 | Proves the executive gate can fail. **Harness self-test.** |

Total job-minutes ≈ **160+**; wall-clock ≈ **25 min** (gated by Build & Test),
and materially worse under concurrent load.

Redundant rebuild inventory: the 29 toolchain jobs each rebuild the *same*
LINK.EXE + DECC$SHR + 5-library graph from source. Build & Test + Static Analysis
each build the whole tree. Conformance + Corpus each build the whole tree in a
container. The bootable image is built independently by persistent-boot and
uat-session (gha layer cache shared).

## 2. Design

### 2a. Two triggers, one gate contract
- **Pre-merge tier (PR):** path-impact-selected — only the jobs a PR's file set
  can actually affect, minus the harness negative controls. Target: the common
  seat-1 (`src/vmsscs/**`) and seat-2 (`src/kernel/**`) PRs drop from ~40 jobs to
  ~4–6.
- **Post-merge (`push: [main]`) + merge queue (`merge_group`) + nightly
  (`schedule`) + manual (`workflow_dispatch`):** **every job runs
  unconditionally** (all path outputs forced `true`), so the union gating `main`
  is exactly today's suite. Enabling the **merge queue** additionally runs the
  full suite on the batched result *before* it lands — full-coverage-before-merge
  with zero per-PR latency. (Branch protection / merge-queue enablement is a repo
  setting, called out in §4 as the operator follow-up; `main` is currently
  unprotected — verified 2026-08-08 — so path filtering cannot deadlock a required
  check today.)

### 2b. Path-impact selection (quality-preserving) — `changes` job
A `dorny/paths-filter@v3` job computes four booleans; on any non-PR event they are
all forced `true`. Every heavy job gains `needs: changes` + an
`if: github.event_name != 'pull_request' || needs.changes.outputs.<f> == 'true'`.
`.github/workflows/**` is in *every* filter, so a CI change re-runs everything.

| Filter | Paths (abridged) | Gates |
|---|---|---|
| `core` | `src/**`, `tests/**`, `third-party/**`, `tools/**`, `CMakeLists.txt` | Build & Test, Static Analysis, Conformance, Corpus |
| `toolchain` | `src/vmslink`, `src/imgact`, `src/libvms*`, `src/vmsprocess`, `src/vmslnm`, `src/vmsfs`, `src/vmsrms`, `src/vmsdcl`, `src/vmsqueue`, `third-party/tcc`, `tools` | the 29 LINK/IMGACT/DECC$SHR/native/tcc jobs |
| `kernel` | `src/kernel`, `src/libvms/syssvc`, `src/libvmssys/vms_kif.*`, `tests/qemu` | Kernel Executive (real `/dev/vms`, QEMU) |
| `boot` | `distro`, `src/ovmx_init`, `tests/qemu/test_{persistent_boot,release_e2e,executive_integral}.sh`, `tests/uat` | Persistent Boot, UAT |

### 2c. The one justified demotion — harness negative controls → non-PR only
Moved off the per-PR path (still run on push-`main` + merge_group + nightly):
- **Kernel Executive — Negative Control** and the **6 Per-Facility Negative
  Control shards + aggregate** — clean-rebuild-from-scratch gates whose sole
  purpose is to prove the executive/attribution harness can go red.
- The **3 `slow`-labelled `_negctl` ctest gates** inside Build & Test — excluded
  on PRs via `ctest -LE slow` (the repo's own documented fast-loop convention,
  vms-819d) and run in full via plain `ctest` on non-PR events.

All are negative controls (harness self-tests), not product checks. This is the
*only* tier move; it removes **zero** product coverage from the per-PR gate.

### 2d. Real product checks stay pre-merge — made fast, not demoted
- **Kernel Executive QEMU proof** stays pre-merge for `kernel`-touching PRs.
  Its `docker build -f tests/qemu/Dockerfile` gains buildx + `type=gha`
  cache-from/to, so the `vms.ko`/`vmsfs.ko`/initramfs image is rebuilt only when
  the kernel/rootfs sources change (content-addressed layer cache), not every run.
- **Build & Test** keeps its content-hash build cache
  (`hashFiles(... src/**/*.c ...)` + `restore-keys` fallback → incremental
  rebuild). The `census`/`register` *positive* product gates stay in the PR tier.
- **Persistent Boot / UAT** keep their `type=gha` bootable-image layer cache.

### 2e. Concurrency
`concurrency: cancel-in-progress` **only for `pull_request`** — a new push to a PR
cancels that PR's superseded run; `main`, `merge_group`, and the nightly schedule
are never cancelled (each needs a complete full-suite run).

## 3. Expected result

| PR shape | Before (jobs / wall) | After (jobs / wall) |
|---|---|---|
| `src/vmsscs/**` (seat 1, weekly) | ~40 / ~25m | changes + Build&Test(`-LE slow`) + static + conformance + corpus ≈ **5 / ~10m** |
| `src/kernel/**` (seat 2) | ~40 / ~25m | + Kernel Executive (cached) ≈ **6 / ~10m** |
| `src/vmslink/**` toolchain | ~40 / ~25m | Build&Test + 29 toolchain (parallel, longest ~6m) ≈ **~32 / ~12m** |
| docs-/tracking-only | ~40 / ~25m | changes only ≈ **1 / ~1m** |
| push-to-`main` / merge queue / nightly | ~40 / ~25m | **~40 / ~25m (unchanged — full union)** |

Cost (job-minutes) drops proportionally on the per-PR path, which also relieves
the runner-saturation slowdown behind the facility-negctl balloon (vms-86a).

## 4. Deferred (follow-up items — need src-lane cooperation)

- **Build-once-share for the 29 toolchain jobs.** Each rebuilds the identical
  LINK.EXE + DECC$SHR + 5-lib graph. A single producer job uploading the graph as
  an artifact (keyed on a `src/vmslink` + `src/imgact` + lib source-content hash),
  consumed by the specimen jobs, would cut the largest *cost* redundancy and let
  the whole toolchain tier stay comfortably pre-merge for toolchain PRs. Blocked
  from this PR because the harness scripts live under `src/imgact/test/**` and
  `src/vmslink/test/**` (src-lane, owned elsewhere this week). File as a joint
  CI + Systems item.
- **Finer fast-tier ctest via labels.** Only some ctests carry `LABELS`; a
  complete `subsystem` labelling in the `CMakeLists.txt` (src-lane) would let the
  PR tier run exactly the affected unit/integration tests, pushing the core
  fast-tier under 5 min for large PRs too.
- **Merge-queue + branch-protection enablement** (operator): require the CI
  workflow green, block direct pushes to `main`, and turn on the merge queue so
  the full suite runs on the batched result before it lands (release-plan §2
  process rule 1). This is the piece that makes "full coverage before merge" and
  "low per-PR latency" both true at once.
