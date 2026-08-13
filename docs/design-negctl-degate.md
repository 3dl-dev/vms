# Tearing out the neg-control declaration-gate theatre (vms-49f)

Per the CLAUDE.md Rule 9 amendment:

> The QEMU/`/dev/vms` suite is the whole proof — never build a CI substitute. A
> check over the tree's own self-declarations (census, register, manifest,
> ledger) proves a string relation, not runtime behavior; it may lint but MUST
> NOT be gated or worded as proof. A lane that can't boot `vms.ko` is "unproven
> here", not proven otherwise.
>
> A gameable declaration-gate is a tear-out, not a hardening target. Ask whether
> the QEMU suite already covers the property — it does.

## The distinction that drove this change

The per-facility negative-control harness has two fundamentally different kinds
of check, which had grown tangled together:

1. **Runtime defect-injection proof (KEPT — the real teeth).** Inject a minimal
   defect into `src/kernel` / `src/libvms`, rebuild `vms.ko`, boot the guest in
   QEMU against a **real `/dev/vms`**, and confirm the facility's own test suite
   goes **RED**. This is behavioral: it observed the executive misbehave. It is
   what caught the #177 fake test (`test_syssvc_scratch_writable` passing even
   when provisioning was broken). Lives in `tests/qemu/run_facility_negctl.sh`
   checks 1-4 (and check 8's fatal-guest-crash detection), driven by the CI job
   `kernel-executive-facility-negative-controls-shard`.

2. **Declaration / string-relation bookkeeping (TORN OUT / DE-GATED).** Checks
   that a run's observed red set **exactly equals** the set the *manifest names*,
   that the union of shard observations **exactly equals** a *committed ledger*,
   that the manifest is internally self-consistent, that the ledger *reader* is
   sound against a *synthetic* record. These prove string relations over the
   harness's own self-declarations, not runtime behavior. They also re-broke on
   **every** executive change that shifted which assertions redden — each such
   change forced a byte-exact re-commit of the 61 KB ledger or the aggregate CI
   job went red — and consumed enormous effort for no behavioral guarantee the
   runtime proof does not already give.

## What was torn out or de-gated

| Artifact | Before | After |
|----------|--------|-------|
| `tests/qemu/facility_negctl_observed.tsv` (61 KB execution ledger) | committed, compared row-for-row | **deleted** |
| CI job `kernel-executive-facility-negative-controls` (aggregate `fnr_compare` ledger gate) | gating on push/merge/nightly | **deleted** |
| ctest `facility_negctl_manifest` (`facility_defects.sh selftest`) | gating | de-gated (script kept as manual lint) |
| ctest `facility_negctl_record` (`facility_record_negctl.sh`) | gating | de-gated (script kept as manual lint) |
| ctest `facility_attribution_selftest` / `facility_attribution_negctl` | gating | de-gated (scripts kept as manual lint) |
| ctest `facility_negctl_equality` (`facility_negctl_equality_negctl.sh`) | gating | de-gated (script kept as manual lint) |
| `run_facility_negctl.sh` checks 5, 6, 7 + committed-ledger comparison | `bad()` (gating) | `lint()` (non-gating, printed only) |

The de-gated `.sh` scripts remain in `tests/qemu/` and can be run by hand as
informational lint (e.g. `sh tests/qemu/facility_defects.sh selftest .`). They
are simply no longer registered with `add_test()` and never gate CI.

## What was kept (the runtime proof)

- `tests/qemu/run_facility_negctl.sh` checks 1-4: the harness went red at all;
  `vms.ko` still loaded / `/dev/vms` present (a facility defect, not an absent
  executive); every expected suite produced a verdict (not silently dropped from
  the initramfs); and **at least one suite in the facility's set went RED**.
- Check 8, strengthened: for a `fatal` defect, a guest that **survives** to
  FINAL RESULTS is now a gating failure — the injected defect no longer has the
  runtime effect the control exists to detect. This is a runtime fact about the
  guest, not a string relation over the manifest.
- The CI shard job `kernel-executive-facility-negative-controls-shard` still runs
  every defect against a real `/dev/vms` under QEMU.
- `test_kmod_*` / `test_syssvc_*` executive tests against a real `/dev/vms` are
  untouched — they are the proof.
- ctest `kernel_executive_negctl_crash` is kept: it verifies the CI shell step
  survives a crashing suite. That is harness robustness, not a string relation
  over self-declarations, and it does not re-break on executive changes.

## Consequence for main

Main no longer flakes on the ledger/manifest bookkeeping: an executive change
that shifts which assertions redden no longer requires re-committing a ledger or
re-aligning a manifest equality. The only way the negative controls can now go
red is the way that matters — a facility whose defect the QEMU/`/dev/vms` suite
fails to catch.

## The two mandatory veracity questions (vms-14f / vms-ad5, operator-approved 2026-08-13)

Keeping only *runtime* defect-injection proofs is necessary but not sufficient: a
proof can still be fake if it is constructed wrong. The repeated-failure-class
circuit breaker (`vms-14f`) tripped when the SAME defect class was overturned
three times — culminating in a rework that was told the defect, was required to
"prove it both ways", produced a both-ways proof, and STILL shipped the class,
because it injected its fault by **monkeypatching the function under test**
instead of through the `FA_ATTR_CACHE` file the tool actually reads. Its negative
control proved nothing. The systemic finding (`vms-ad5`) is that the veracity
rubric never asked two questions. They are now MANDATORY for every negative
control / self-consistency / "prove-it-both-ways" claim, in review and in
self-check:

- **Q1 — Independent-oracle sourcing.** Where does the test's EXPECTED VALUE come
  from? It must come from an independent oracle (public VMS documentation, lab
  observation, a source other than the code under test) — never be derived from
  the same code the test validates. A test whose expected value is computed by its
  target is a tautology that cannot fail (this is exactly the `vms-a4d` defect,
  which must not merge until reworked to satisfy Q1).

- **Q2 — Real-injection-path.** Is the fault injected through the REAL input path
  the code actually reads (the file / socket / `/dev/vms` the tool consumes) — not
  by monkeypatching the function under test? An injection that bypasses the real
  read path proves nothing about the real read path.

If either answer is wrong, the both-ways proof is fake — reject it. These two
questions are mirrored in the reviewer veracity checklist so every review applies
them.
