#!/bin/bash
#
# run_host_negctl.sh - the HOST-NATIVE (cmake build + run, NO /dev/vms/QEMU)
# negative-control driver for tests/cluster/host (vms-8e7, grown vms-55e).
#
# WHAT THIS PROVES
#
# host_defects.sh (this directory) is entirely STATIC: it never compiles or
# runs anything, so a PASS from its own `selftest` proves only that the
# manifest's declarations agree with the tree's current text. THIS script is
# the one thing in the pair that actually executes something, for EVERY
# defect the manifest lists (mirroring tests/qemu/run_facility_negctl.sh's
# loop-over-the-manifest idiom, ported host-side):
#
#   1. for each DISTINCT suite named by some defect's suites_red, builds it
#      from the UNMODIFIED tree and runs it as a POSITIVE CONTROL exactly
#      ONCE -- refuses to go any further with that suite unless it is
#      completely green, so a harness that already fails indiscriminately
#      cannot pass this script. Distinct suites are de-duplicated: two
#      defects that redden the same suite (e.g. two properties of the same
#      TU family) do not pay for the positive control twice.
#   2. for each defect, copies the tree, injects host_defects.sh's mutation
#      into the copy, rebuilds the SAME target from the mutated copy, and
#      runs it;
#   3. captures the complete SET of "  FAIL " assertion labels the mutated
#      run produced and asserts it EQUALS the defect's require_fail set
#      EXACTLY -- no missing member, no extra member (the same exact-red-set
#      discipline as tests/qemu/run_facility_negctl.sh, ported host-side).
#      The comparison is over SETS (duplicate identical labels from a defect
#      that reddens the same assertion text across a test's own loop collapse
#      to one member -- see host_defects.sh's codec-blk-no-trailer-not-honest
#      for why that matters);
#   4. discards the working copy (the real tree is never touched by this
#      script -- host_defects.sh apply always targets the copy) and confirms
#      the defect's own `targets` files are unchanged in the real checkout.
#
# A single defect in the manifest still works exactly as it did before this
# item grew the loop (vms-8e7's own one-defect shape is preserved -- the loop
# body below is what that script's body used to be, unrolled once).
#
# Usage:
#   tests/cluster/host/run_host_negctl.sh [defect ...]   (default: all, from
#                                                          `host_defects.sh list`)
# Env:
#   OVMX_HOST_NEGCTL_BUILD_DIR   build dir for the POSITIVE CONTROL build
#                                (default: <repo-root>/build; reused if
#                                already configured, so this can piggyback on
#                                a build a CI step already produced).
#
set -u

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SELF_DIR/../../.." && pwd)
DEFECTS_SH="$SELF_DIR/host_defects.sh"

BUILD_DIR="${OVMX_HOST_NEGCTL_BUILD_DIR:-$REPO_ROOT/build}"

step() { printf '\n=== %s ===\n' "$1"; }

# fail_set - reads a suite's stdout on stdin, prints one "what" label per
# "  FAIL " line, with any ": got <n> (...), want <m> (...)" tail stripped --
# exactly the extraction rule cluster_test.h's own ct_check_eq_u32 format
# needs (docs/plan-faithful-cluster-executive.md's driver spec for this item).
fail_set() {
    grep '^  FAIL ' | sed -E 's/^  FAIL //; s/: got .*$//'
}

# configure_and_build <src-root> <build-dir> <cmake-target>
#
# Reuses an already-configured build dir (CMakeCache.txt present) rather than
# reconfiguring, so this can run right after CI's own "Configure"/"Build"
# steps against the real checkout without doing that work twice.
configure_and_build() {
    _root="$1"; _build="$2"; _target="$3"
    if [ ! -f "$_build/CMakeCache.txt" ]; then
        if ! cmake -S "$_root" -B "$_build" \
                -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
                >"$_build.cfg.log" 2>&1; then
            echo "FATAL: cmake configure of $_root failed:" >&2
            cat "$_build.cfg.log" >&2
            return 1
        fi
    fi
    if ! cmake --build "$_build" --target "$_target" -j"$(nproc)" \
            >"$_build.build.log" 2>&1; then
        echo "FATAL: cmake build of $_target (from $_root) failed:" >&2
        cat "$_build.build.log" >&2
        return 1
    fi
}

RC=0

# ---------------------------------------------------------------------------
# The defect list: every argument given, or the whole manifest.
# ---------------------------------------------------------------------------
if [ $# -gt 0 ]; then
    DEFECT_LIST="$*"
else
    DEFECT_LIST=$("$DEFECTS_SH" list)
fi

# ---------------------------------------------------------------------------
# 1. POSITIVE CONTROLS. One per DISTINCT suite named across the defects this
#    run is about to exercise, built and run against the REAL tree, before any
#    injection: a harness that already fails indiscriminately must not be able
#    to pass this script. POSCTL_DONE de-dupes so two defects that share a
#    suite (e.g. two independent properties of the same FSM) do not redo it.
# ---------------------------------------------------------------------------
declare -A POSCTL_DONE
declare -A POSCTL_OK

positive_control() {
    _suite="$1"

    if [ -n "${POSCTL_DONE[$_suite]:-}" ]; then
        return $([ -n "${POSCTL_OK[$_suite]:-}" ] && echo 0 || echo 1)
    fi
    POSCTL_DONE[$_suite]=1

    step "POSITIVE CONTROL: $_suite against the UNMODIFIED tree"
    if ! configure_and_build "$REPO_ROOT" "$BUILD_DIR" "$_suite"; then
        return 1
    fi
    if [ ! -x "$BUILD_DIR/bin/$_suite" ]; then
        echo "FATAL: $BUILD_DIR/bin/$_suite was not produced by the build" >&2
        return 1
    fi

    _pos_out=$("$BUILD_DIR/bin/$_suite" 2>&1)
    _pos_rc=$?
    printf '%s\n' "$_pos_out"
    _pos_fails=$(printf '%s\n' "$_pos_out" | fail_set)

    if [ "$_pos_rc" -ne 0 ] || [ -n "$_pos_fails" ]; then
        echo "FATAL: the positive control is NOT all-green -- refusing to test a mutation" >&2
        echo "       against a suite that is already red (rc=$_pos_rc)." >&2
        [ -n "$_pos_fails" ] && printf '%s\n' "$_pos_fails" | sed 's/^/  unexpected FAIL: /' >&2
        return 1
    fi
    echo "ok: positive control all-green (exit $_pos_rc, 0 FAIL lines)"
    POSCTL_OK[$_suite]=1
    return 0
}

# ---------------------------------------------------------------------------
# run_defect <defect> - the per-defect negative control: inject into a working
# copy, rebuild the ONE suite the manifest names, and assert the observed red
# set equals require_fail EXACTLY. Mirrors run_facility_negctl.sh's per-defect
# loop body, minus the QEMU boot -- a cmake build + run in its place.
# ---------------------------------------------------------------------------
run_defect() {
    _d="$1"
    _suite=$("$DEFECTS_SH" field "$_d" suites_red)
    _targets=$("$DEFECTS_SH" field "$_d" targets)
    _why=$("$DEFECTS_SH" field "$_d" why)

    echo ""
    echo "--- negative control: $_d ---"
    echo "  suite:  $_suite"
    echo "  defect: $_why"

    if [ -z "$_suite" ]; then
        echo "FAIL: $_d: suites_red is empty -- broken manifest entry" >&2
        return 1
    fi

    if ! positive_control "$_suite"; then
        echo "FAIL: $_d: skipped -- the positive control for '$_suite' did not pass" >&2
        return 1
    fi

    # -----------------------------------------------------------------------
    # Inject the defect into a WORKING COPY of the tree and rebuild.
    # -----------------------------------------------------------------------
    _work=$(mktemp -d) || return 1
    _rc=0

    if ! rsync -a \
            --exclude='.git' --exclude='build' --exclude='.worktrees' \
            --exclude='.ready' --exclude='.joint-e2e-*' \
            "$REPO_ROOT/" "$_work/repo/"; then
        echo "FATAL: could not copy $REPO_ROOT to a working copy" >&2
        rm -rf "$_work"
        return 1
    fi

    if ! "$DEFECTS_SH" apply "$_d" "$_work/repo/src"; then
        echo "FATAL: host_defects.sh could not inject '$_d' -- see its own error above" >&2
        rm -rf "$_work"
        return 1
    fi

    _mut_build="$_work/build"
    if ! configure_and_build "$_work/repo" "$_mut_build" "$_suite"; then
        rm -rf "$_work"
        return 1
    fi
    if [ ! -x "$_mut_build/bin/$_suite" ]; then
        echo "FATAL: $_mut_build/bin/$_suite was not produced by the mutated build" >&2
        rm -rf "$_work"
        return 1
    fi

    _neg_out=$("$_mut_build/bin/$_suite" 2>&1)
    _neg_rc=$?
    printf '%s\n' "$_neg_out"
    _neg_fails=$(printf '%s\n' "$_neg_out" | fail_set)

    # -------------------------------------------------------------------
    # The observed red set must EQUAL require_fail EXACTLY. Compared as a
    # SET (sort -u on both sides): a defect whose mutation reddens the same
    # assertion text more than once inside the suite's own loop (a single
    # property, observed several times) must not be double-counted against
    # a manifest that names it once.
    # -------------------------------------------------------------------
    step "checking the observed red set against '$_d's require_fail"
    _want=$("$DEFECTS_SH" field "$_d" require_fail | grep -v '^$' | sort -u)
    _got=$(printf '%s\n' "$_neg_fails" | grep -v '^$' | sort -u)

    if [ "$_neg_rc" -eq 0 ]; then
        echo "FAIL: the mutated build's exit code was 0 -- the defect produced no failure at all." >&2
        _rc=1
    fi

    if [ "$_got" = "$_want" ]; then
        echo "PASS: the injected defect reddened EXACTLY the require_fail set, no more, no less:"
        printf '%s\n' "$_want" | sed 's/^/    /'
    else
        echo "FAIL: observed red set does NOT equal require_fail." >&2
        echo "  require_fail (want):" >&2
        printf '%s\n' "$_want" | sed 's/^/    /' >&2
        echo "  observed     (got): " >&2
        printf '%s\n' "$_got" | sed 's/^/    /' >&2
        _missing=$(comm -23 <(printf '%s\n' "$_want") <(printf '%s\n' "$_got"))
        _extra=$(comm -13 <(printf '%s\n' "$_want") <(printf '%s\n' "$_got"))
        [ -n "$_missing" ] && { echo "  MISSING (named but did not go red):" >&2; printf '%s\n' "$_missing" | sed 's/^/    /' >&2; }
        [ -n "$_extra" ] && { echo "  EXTRA (went red but not named):" >&2; printf '%s\n' "$_extra" | sed 's/^/    /' >&2; }
        _rc=1
    fi

    # -------------------------------------------------------------------
    # Revert: discard the working copy. The real tree was never touched --
    # host_defects.sh apply only ever wrote into $_work/repo. Confirmed
    # against every file the defect NAMED as a target, not one hardcoded path.
    # -------------------------------------------------------------------
    step "revert"
    rm -rf "$_work"
    _dirty=""
    for _t in $_targets; do
        _f="src/$_t"
        if [ -n "$(cd "$REPO_ROOT" && git status --porcelain -- "$_f" 2>/dev/null)" ]; then
            _dirty="$_dirty $_f"
        fi
    done
    if [ -n "$_dirty" ]; then
        echo "FATAL: the real tree is dirty after the run --$_dirty -- the injection leaked" >&2
        echo "       into the checkout instead of staying in the working copy." >&2
        _rc=1
    else
        echo "ok: the real tree is unmodified (defect only ever touched the working copy)"
    fi

    return $_rc
}

for d in $DEFECT_LIST; do
    if run_defect "$d"; then
        echo ""
        echo "VERDICT: PASS -- $d's suite is a real mutation-tested gate."
    else
        echo ""
        echo "VERDICT: FAIL -- $d: see the FATAL/FAIL lines above."
        RC=1
    fi
done

echo ""
echo "===================================================================="
if [ "$RC" -eq 0 ]; then
    echo "OVERALL: PASS -- every defect in {$DEFECT_LIST} reddens exactly its"
    echo "  require_fail set and nothing else, with the real tree left clean."
else
    echo "OVERALL: FAIL -- see the per-defect VERDICT lines above."
fi
echo "===================================================================="
exit $RC
