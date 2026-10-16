#!/bin/sh
# tools/ci/fast.sh -- the local fast loop (CI-speed). Build, run the PR-tier
# ctest suite, and run host negctl for the facilities your change touches.
# Target: under 5 minutes on a warm build dir. `make fast` calls this.
#
#   tools/ci/fast.sh [--base REF]      changed files = REF...HEAD plus the
#                                      working tree (default REF: origin/main)
#
# Env: FAST_BUILD_DIR (default build-fast), FAST_JOBS (default nproc).
#
# What it deliberately does NOT run: anything needing QEMU/docker (the
# per-facility executive negctl, boot e2e, release cuts). Those are CI-only;
# the merge queue runs the ones your change touches (tools/ci/ci_tiers.py) and
# the nightly runs all. Run `python3 tools/ci/ci_tiers.py --event merge_group
# --files <list>` to see which heavy legs your change will owe in the queue.
set -eu

BASE=origin/main
while [ $# -gt 0 ]; do
    case "$1" in
        --base) BASE=$2; shift 2 ;;
        *) echo "usage: tools/ci/fast.sh [--base REF]" >&2; exit 2 ;;
    esac
done

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
BUILD=${FAST_BUILD_DIR:-build-fast}
JOBS=${FAST_JOBS:-$(nproc)}
T0=$(date +%s)
stamp() { printf '\n=== %s (%ss) ===\n' "$1" "$(( $(date +%s) - T0 ))"; }

CHANGED=$(mktemp)
trap 'rm -f "$CHANGED"' EXIT
{ git diff --name-only "$(git merge-base HEAD "$BASE" 2>/dev/null || echo "$BASE")" ; git diff --name-only HEAD; git ls-files --others --exclude-standard; } | sort -u | grep . > "$CHANGED" || true
echo "changed files: $(wc -l < "$CHANGED")"

stamp "configure + build ($BUILD, -j$JOBS)"
LAUNCH=""
command -v ccache >/dev/null 2>&1 && LAUNCH="-DCMAKE_C_COMPILER_LAUNCHER=ccache"
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DBUILD_TOOLS=ON $LAUNCH >"$BUILD.cfg.log" 2>&1 \
    || { cat "$BUILD.cfg.log"; exit 1; }
cmake --build "$BUILD" -j"$JOBS"

stamp "ctest (PR tier: -LE slow, parallel)"
( cd "$BUILD" && ctest --output-on-failure --parallel "$JOBS" -LE slow )

# Host negctl (tests/cluster/host): run the defects whose mutation target you
# changed, or ALL of them when the harness, a suite or CMake changed. Same
# fail-closed rule as the queue: any change under tests/cluster/host other than
# a defect target selects everything; a change elsewhere selects nothing.
HOST=tests/cluster/host
SEL=""
ALL=false
while IFS= read -r f; do
    case "$f" in
        "$HOST"/*|CMakeLists.txt) ALL=true ;;
    esac
done < "$CHANGED"
if $ALL; then
    SEL=$("$HOST/host_defects.sh" list)
else
    for d in $("$HOST/host_defects.sh" list); do
        for t in $("$HOST/host_defects.sh" field "$d" targets); do
            if grep -Fxq "src/$t" "$CHANGED"; then SEL="$SEL $d"; break; fi
        done
    done
fi
if [ -n "$(echo "$SEL" | tr -d ' \n')" ]; then
    stamp "host negctl: $(echo "$SEL" | wc -w) defect(s) for the facilities you touched"
    OVMX_HOST_NEGCTL_BUILD_DIR="$ROOT/$BUILD" "$HOST/run_host_negctl.sh" $SEL
else
    stamp "host negctl: no touched cluster facility, skipped"
fi

stamp "fast loop OK"
