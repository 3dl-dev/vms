#!/bin/bash
# fast.sh -- the local loop: prove a change BEFORE the merge queue does.
#
# The queue used to be the first place a change met QEMU: a red found there
# costs a whole PR-check + queue cycle (~1.5 h), and 7 of 25 merged PRs
# (#2023-#2051) were kicked and re-queued at least once. This runs, on this
# host, the same proofs the queue would run for this change:
#
#   1. what the queue will owe: tools/ci/ci_tiers.py over this branch's diff
#      (the heavy tiers and the per-facility negctl selection);
#   2. host: configure + build + ctest (the Build & Test leg);
#   3. QEMU: the kernel-executive harness image, booted against a real
#      /dev/vms, every suite, split into the CI's 6 shards (run in parallel)
#      plus the devtab_terminal VM -- exactly `docker run -e SHARD_INDEX -e
#      SHARD_TOTAL <image>` as ci-kernel-executive.yml does;
#   4. --negctl: the per-facility negative controls the queue selects for this
#      diff (tests/qemu/run_facility_negctl.sh); with --negctl-all, every one.
#
# usage: tools/ci/fast.sh [--base REF] [--no-host] [--no-qemu] [--negctl]
#                         [--negctl-all] [--jobs N] [--keep-image]
#   --base REF   diff base (default: merge-base with origin/main)
#   --jobs N     QEMU shards booted at once (default 3; each VM ~1.5 GB)
# Exit status: 0 only if every step that ran passed. Logs: $OUT (printed).
#
# It removes the image it built (unless --keep-image); the host build dir is
# build-fast/ inside the worktree (dockerignored), removed with the worktree.
# Docker hygiene: only `docker rmi` of the one tag it created -- never prune.
set -u

ROOT=$(git rev-parse --show-toplevel) || exit 2
cd "$ROOT" || exit 2
BASE="" HOST=1 QEMU=1 NEGCTL=0 NEGCTL_ALL=0 JOBS=3 KEEP=0
while [ $# -gt 0 ]; do
    case "$1" in
    --base) BASE=$2; shift ;;
    --no-host) HOST=0 ;;
    --no-qemu) QEMU=0 ;;
    --negctl) NEGCTL=1 ;;
    --negctl-all) NEGCTL=1; NEGCTL_ALL=1 ;;
    --jobs) JOBS=$2; shift ;;
    --keep-image) KEEP=1 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "fast.sh: unknown argument $1" >&2; exit 2 ;;
    esac
    shift
done
case "$JOBS" in ''|*[!0-9]*|0) echo "fast.sh: --jobs wants a positive integer" >&2; exit 2 ;; esac

OUT=$(mktemp -d "${TMPDIR:-/tmp}/ovmx-fast.XXXXXX")
TAG="ovmx-ktest:fast-$(basename "$OUT" | tr 'A-Z.' 'a-z-')"
FAILED=""
fail() { FAILED="$FAILED $1"; echo "FAIL: $1" >&2; }
cleanup() {
    if [ "$KEEP" = 0 ] && docker image inspect "$TAG" >/dev/null 2>&1; then
        docker rmi "$TAG" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

# ---- 1. what the queue will owe -------------------------------------------
[ -n "$BASE" ] || BASE=$(git merge-base origin/main HEAD) || exit 2
{ git diff --name-only "$BASE"; git ls-files --others --exclude-standard; } | sort -u > "$OUT/files"
echo "== fast.sh: $(wc -l < "$OUT/files") changed path(s) vs ${BASE:0:12}; logs in $OUT"
python3 tools/ci/ci_tiers.py --event merge_group --ref local --files "$OUT/files" > "$OUT/tiers" || fail "ci_tiers"
SEL=$(sed -n 's/^negctl_select=//p' "$OUT/tiers")
echo "-- the queue will run these heavy tiers for this diff:"
grep '=true$' "$OUT/tiers" | sed 's/=true$//; s/^/     /'
case "$SEL" in
"") echo "     negctl: none" ;;
ALL) echo "     negctl: ALL $(sh tests/qemu/facility_defects.sh list | wc -l) defects (an unmapped path; fails closed)" ;;
*) echo "     negctl: $(echo "$SEL" | wc -w) defect(s): $SEL" ;;
esac

# ---- 2. host build + ctest ----------------------------------------------------
if [ "$HOST" = 1 ]; then
    echo "== host: build + ctest (build-fast/)"
    if cmake -B build-fast -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DBUILD_TOOLS=ON > "$OUT/host-configure.log" 2>&1 \
       && cmake --build build-fast -j"$(nproc)" > "$OUT/host-build.log" 2>&1; then
        (cd build-fast && ctest -j"$(nproc)" --output-on-failure) > "$OUT/host-ctest.log" 2>&1 \
            || fail "host ctest (see $OUT/host-ctest.log)"
        grep -E "tests passed|tests failed" "$OUT/host-ctest.log" | sed 's/^/     /'
    else
        fail "host build (see $OUT/host-build.log)"
    fi
fi

# ---- 3. QEMU kernel-executive suites ------------------------------------------
if [ "$QEMU" = 1 ] && [ ! -e /dev/kvm ]; then
    fail "QEMU: no /dev/kvm on this host"; QEMU=0
fi
if [ "$QEMU" = 1 ]; then
    AVAIL_G=$(df -BG --output=avail / | tail -1 | tr -dc 0-9)
    if [ "${AVAIL_G:-0}" -lt 8 ]; then
        fail "QEMU: only ${AVAIL_G}G free on / (need 8G for the harness image); free space age-scoped first"
        QEMU=0
    fi
fi
if [ "$QEMU" = 1 ]; then
    echo "== QEMU: building $TAG (tests/qemu/Dockerfile)"
    if ! docker build -f tests/qemu/Dockerfile -t "$TAG" . > "$OUT/qemu-image.log" 2>&1; then
        fail "QEMU image build (see $OUT/qemu-image.log)"
    else
        SHARDS=6
        run_leg() { # name, docker-run args...
            local name=$1; shift
            if docker run --rm "$@" "$TAG" > "$OUT/qemu-$name.log" 2>&1; then
                echo "     PASS $name"
            else
                echo "     FAIL $name (see $OUT/qemu-$name.log)"
                echo "$name" >> "$OUT/qemu-failed"
            fi
        }
        echo "== QEMU: $SHARDS shards + devtab_terminal, $JOBS at a time"
        i=0
        for leg in $(seq 0 $((SHARDS - 1))) devtab; do
            if [ "$leg" = devtab ]; then
                run_leg devtab_terminal -e OVMX_KTEST_ONLY=test_kmod_devtab_terminal -e KE_WALL_TIMEOUT=1800 &
            else
                run_leg "shard$leg" -e SHARD_INDEX="$leg" -e SHARD_TOTAL="$SHARDS" &
            fi
            i=$((i + 1))
            [ $((i % JOBS)) -eq 0 ] && wait
        done
        wait
        [ -s "$OUT/qemu-failed" ] && fail "QEMU legs: $(tr '\n' ' ' < "$OUT/qemu-failed")"
    fi
fi

# ---- 4. per-facility negative controls ----------------------------------------
if [ "$NEGCTL" = 1 ]; then
    if [ "$NEGCTL_ALL" = 1 ]; then
        DEFECTS=""   # run_facility_negctl.sh with no argument = every defect
    elif [ "$SEL" = ALL ]; then
        DEFECTS=SKIP
        echo "== negctl: this diff selects ALL defects (~13 h serial on one host): NOT run locally;"
        echo "   the queue runs them on 64 shards. Use --negctl-all to run them here anyway."
    else
        DEFECTS=${SEL:-SKIP}
        [ -n "$SEL" ] || echo "== negctl: the diff selects no defect"
    fi
    if [ "$DEFECTS" != SKIP ] && [ "$QEMU" = 1 ] && docker image inspect "$TAG" >/dev/null 2>&1; then
        # run_facility_negctl.sh REUSES an existing ovmx-ktest-negctl-base
        # tag rather than rebuilding it (CI builds it first). Locally that tag
        # is whatever tree last built it, and a stale one fails the pristine
        # positive control with the newer suites MISSING. Point it at THIS
        # tree's image, as CI's build step does.
        docker tag "$TAG" ovmx-ktest-negctl-base:latest
    elif [ "$DEFECTS" != SKIP ]; then
        docker rmi ovmx-ktest-negctl-base:latest >/dev/null 2>&1 || true   # rebuilt from this tree
    fi
    if [ "$DEFECTS" != SKIP ]; then
        echo "== negctl: tests/qemu/run_facility_negctl.sh ${DEFECTS:-<every defect>}"
        # shellcheck disable=SC2086
        CONTAINER_ENGINE=docker tests/qemu/run_facility_negctl.sh $DEFECTS > "$OUT/negctl.log" 2>&1 \
            || fail "negctl (see $OUT/negctl.log)"
        grep -E "^  PASS: '|^  FAIL" "$OUT/negctl.log" | sed 's/^/   /'
    fi
fi

echo
if [ -n "$FAILED" ]; then
    echo "fast.sh: RED --$FAILED"
    exit 1
fi
echo "fast.sh: GREEN (logs: $OUT)"
