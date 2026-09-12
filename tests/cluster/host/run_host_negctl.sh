#!/bin/bash
#
# run_host_negctl.sh - the HOST-NATIVE (cmake build + run, NO /dev/vms/QEMU)
# negative-control driver for tests/cluster/host (vms-8e7).
#
# WHAT THIS PROVES
#
# host_defects.sh (this directory) is entirely STATIC: it never compiles or
# runs anything, so a PASS from its own `selftest` proves only that the
# manifest's declarations agree with the tree's current text. THIS script is
# the one thing in the pair that actually executes something:
#
#   1. builds test_cnxman_genesis_negctl from the UNMODIFIED tree and runs it
#      as a POSITIVE CONTROL -- refuses to go any further unless it is
#      completely green, so a harness that already fails indiscriminately
#      cannot pass this script;
#   2. copies the tree, injects host_defects.sh's one defect
#      (coord-genesis-refusal-uncounted) into the copy, rebuilds the SAME
#      target from the mutated copy, and runs it;
#   3. captures the complete set of "  FAIL " assertion labels the mutated
#      run produced and asserts it EQUALS the defect's require_fail set
#      EXACTLY -- no missing member, no extra member (the same exact-red-set
#      discipline as tests/qemu/run_facility_negctl.sh, ported host-side);
#   4. discards the working copy (the real tree is never touched by this
#      script -- host_defects.sh apply always targets the copy).
#
# Usage:
#   tests/cluster/host/run_host_negctl.sh
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

TARGET="test_cnxman_genesis_negctl"
DEFECT="coord-genesis-refusal-uncounted"

BUILD_DIR="${OVMX_HOST_NEGCTL_BUILD_DIR:-$REPO_ROOT/build}"

WORK=""
cleanup() { [ -n "$WORK" ] && rm -rf "$WORK"; }
trap cleanup EXIT

step() { printf '\n=== %s ===\n' "$1"; }

# configure_and_build <src-root> <build-dir>
#
# Reuses an already-configured build dir (CMakeCache.txt present) rather than
# reconfiguring, so this can run right after CI's own "Configure"/"Build"
# steps against the real checkout without doing that work twice.
configure_and_build() {
    _root="$1"; _build="$2"
    if [ ! -f "$_build/CMakeCache.txt" ]; then
        if ! cmake -S "$_root" -B "$_build" \
                -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
                >"$_build.cfg.log" 2>&1; then
            echo "FATAL: cmake configure of $_root failed:" >&2
            cat "$_build.cfg.log" >&2
            return 1
        fi
    fi
    if ! cmake --build "$_build" --target "$TARGET" -j"$(nproc)" \
            >"$_build.build.log" 2>&1; then
        echo "FATAL: cmake build of $TARGET (from $_root) failed:" >&2
        cat "$_build.build.log" >&2
        return 1
    fi
}

# fail_set - reads the suite's stdout on stdin, prints one "what" label per
# "  FAIL " line, with any ": got <n> (...), want <m> (...)" tail stripped --
# exactly the extraction rule cluster_test.h's own ct_check_eq_u32 format
# needs (docs/plan-faithful-cluster-executive.md's driver spec for this item).
fail_set() {
    grep '^  FAIL ' | sed -E 's/^  FAIL //; s/: got .*$//'
}

rc=0

# ---------------------------------------------------------------------------
# 1. POSITIVE CONTROL: the suite, unmodified, must be completely green.
# ---------------------------------------------------------------------------
step "POSITIVE CONTROL: $TARGET against the UNMODIFIED tree"
if ! configure_and_build "$REPO_ROOT" "$BUILD_DIR"; then
    exit 1
fi
if [ ! -x "$BUILD_DIR/bin/$TARGET" ]; then
    echo "FATAL: $BUILD_DIR/bin/$TARGET was not produced by the build" >&2
    exit 1
fi

pos_out=$("$BUILD_DIR/bin/$TARGET" 2>&1)
pos_rc=$?
printf '%s\n' "$pos_out"
pos_fails=$(printf '%s\n' "$pos_out" | fail_set)

if [ "$pos_rc" -ne 0 ] || [ -n "$pos_fails" ]; then
    echo "FATAL: the positive control is NOT all-green -- refusing to test a mutation" >&2
    echo "       against a suite that is already red (rc=$pos_rc)." >&2
    [ -n "$pos_fails" ] && printf '%s\n' "$pos_fails" | sed 's/^/  unexpected FAIL: /' >&2
    exit 1
fi
echo "ok: positive control all-green (exit $pos_rc, 0 FAIL lines)"

# ---------------------------------------------------------------------------
# 2. Inject the defect into a WORKING COPY of the tree and rebuild.
# ---------------------------------------------------------------------------
step "injecting '$DEFECT' into a working copy and rebuilding $TARGET"
WORK=$(mktemp -d) || exit 1

if ! rsync -a \
        --exclude='.git' --exclude='build' --exclude='.worktrees' \
        --exclude='.ready' --exclude='.joint-e2e-*' \
        "$REPO_ROOT/" "$WORK/repo/"; then
    echo "FATAL: could not copy $REPO_ROOT to a working copy" >&2
    exit 1
fi

if ! "$DEFECTS_SH" apply "$DEFECT" "$WORK/repo/src"; then
    echo "FATAL: host_defects.sh could not inject '$DEFECT' -- see its own error above" >&2
    exit 1
fi

MUT_BUILD="$WORK/build"
if ! configure_and_build "$WORK/repo" "$MUT_BUILD"; then
    exit 1
fi
if [ ! -x "$MUT_BUILD/bin/$TARGET" ]; then
    echo "FATAL: $MUT_BUILD/bin/$TARGET was not produced by the mutated build" >&2
    exit 1
fi

neg_out=$("$MUT_BUILD/bin/$TARGET" 2>&1)
neg_rc=$?
printf '%s\n' "$neg_out"
neg_fails=$(printf '%s\n' "$neg_out" | fail_set)

# ---------------------------------------------------------------------------
# 3. The observed red set must EQUAL require_fail EXACTLY.
# ---------------------------------------------------------------------------
step "checking the observed red set against '$DEFECT's require_fail"
want=$("$DEFECTS_SH" field "$DEFECT" require_fail | grep -v '^$' | sort)
got=$(printf '%s\n' "$neg_fails" | grep -v '^$' | sort)

if [ "$neg_rc" -eq 0 ]; then
    echo "FAIL: the mutated build's exit code was 0 -- the defect produced no failure at all." >&2
    rc=1
fi

if [ "$got" = "$want" ]; then
    echo "PASS: the injected defect reddened EXACTLY the require_fail set, no more, no less:"
    printf '%s\n' "$want" | sed 's/^/    /'
else
    echo "FAIL: observed red set does NOT equal require_fail." >&2
    echo "  require_fail (want):" >&2
    printf '%s\n' "$want" | sed 's/^/    /' >&2
    echo "  observed     (got): " >&2
    printf '%s\n' "$got" | sed 's/^/    /' >&2
    _missing=$(comm -23 <(printf '%s\n' "$want") <(printf '%s\n' "$got"))
    _extra=$(comm -13 <(printf '%s\n' "$want") <(printf '%s\n' "$got"))
    [ -n "$_missing" ] && { echo "  MISSING (named but did not go red):" >&2; printf '%s\n' "$_missing" | sed 's/^/    /' >&2; }
    [ -n "$_extra" ] && { echo "  EXTRA (went red but not named):" >&2; printf '%s\n' "$_extra" | sed 's/^/    /' >&2; }
    rc=1
fi

# ---------------------------------------------------------------------------
# 4. Revert: discard the working copy. The real tree was never touched --
#    host_defects.sh apply only ever wrote into $WORK/repo.
# ---------------------------------------------------------------------------
step "revert"
rm -rf "$WORK"
WORK=""
if [ -n "$(cd "$REPO_ROOT" && git status --porcelain -- src/kernel-core/vms_cnxman_coord_fsm.c 2>/dev/null)" ]; then
    echo "FATAL: the real tree's vms_cnxman_coord_fsm.c is dirty after the run -- the" >&2
    echo "       injection leaked into the checkout instead of staying in the working copy." >&2
    rc=1
else
    echo "ok: the real tree is unmodified (defect only ever touched the working copy)"
fi

if [ "$rc" -eq 0 ]; then
    echo
    echo "===================================================================="
    echo "VERDICT: PASS -- $TARGET is a real mutation-tested gate:"
    echo "  positive control all-green, and '$DEFECT' reddens exactly its"
    echo "  require_fail set and nothing else."
    echo "===================================================================="
else
    echo
    echo "===================================================================="
    echo "VERDICT: FAIL -- see the FATAL/FAIL lines above."
    echo "===================================================================="
fi

exit $rc
