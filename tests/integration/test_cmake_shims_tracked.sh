#!/bin/sh
#
# test_cmake_shims_tracked.sh - vms-4847 standing gate: every *_no_python3.cmake
# file a CMakeLists.txt else() branch references must actually be tracked by git.
#
# WHY THIS GATE EXISTS. The repo's .gitignore has a blanket `*.cmake` rule
# (needed for generated build-tree files like cmake_install.cmake), carved
# back open for the `*_no_python3.cmake` naming convention used by the
# no-Python3 fallback shims registered next to every figures/mutants gate in
# tests/vmsscs/CMakeLists.txt. Before vms-4847 that carve-out was one `!`
# line per path, added by hand each time a new shim was written. Three shims
# (scs_figures_wire_mutants, scs_join_capability_figures,
# scs_join_capability_mutants) were referenced by CMakeLists.txt for months
# and never existed in git -- caught only by vms-371's audit, and only
# because two of the three failed SILENT: `git status` showed nothing to
# commit, ctest ran a gate that silently did not exist. A fourth
# (scs_449_bracket_shape) failed LOUD instead (a bare "cmake -P: file does
# not exist" naming neither the gate nor the fix) and a fifth
# (scs_send_sites) was found only by vms-4847's own audit -- the pattern
# recurs regardless of the .gitignore fix, because the file can also simply
# never be `git add`-ed. The wildcard carve-out in .gitignore fixes the FIRST
# failure mode (re-ignored). This gate fixes the second (never added): it
# re-derives, on every run, that every *_no_python3.cmake path a
# CMakeLists.txt else() branch names resolves to a file `git ls-files` knows
# about -- not by trusting the .gitignore fix to have worked.
#
# Usage: test_cmake_shims_tracked.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd "$SRC_ROOT" || { echo "FAIL: cannot cd to SRC_ROOT $SRC_ROOT"; exit 1; }

if ! command -v git >/dev/null 2>&1; then
    echo "FAIL: git not found -- cannot verify tracked status"
    exit 1
fi

status=0
checked=0

# Every *_no_python3.cmake token referenced anywhere by any CMakeLists.txt in
# the tree, paired with the CMakeLists.txt directory it was referenced from
# (the shim lives alongside the CMakeLists.txt that names it, per convention).
#
# vms-4e31 dispatch (2026-08-06): exclude .claude/worktrees -- those are
# SEPARATE git worktrees (their own branch, their own checkout), not part of
# the current branch's tree. `git ls-files` below always answers against
# THIS checkout's index, so any nested worktree path is unconditionally
# "not tracked" here regardless of whether the shim is actually committed on
# that worktree's own branch -- a guaranteed false positive, not a real gap.
# Same exclusion convention as test_runtime_target.sh.
refs=$(grep -rhEo '[A-Za-z0-9_]+_no_python3\.cmake' --include=CMakeLists.txt \
    --exclude-dir=.claude . 2>/dev/null | sort -u)

if [ -z "$refs" ]; then
    echo "OK: no *_no_python3.cmake references found in any CMakeLists.txt"
    exit 0
fi

# Map each referenced basename back to the CMakeLists.txt file(s) that name
# it, so we can resolve it relative to that file's directory.
for name in $refs; do
    found_any=0
    for cml in $(grep -rlE "$name" --include=CMakeLists.txt --exclude-dir=.claude . 2>/dev/null); do
        dir=$(dirname "$cml")
        candidate="$dir/$name"
        checked=$((checked + 1))
        if [ ! -f "$candidate" ]; then
            echo "FAIL: $cml references $name but $candidate does not exist on disk"
            status=1
            continue
        fi
        if ! git ls-files --error-unmatch "$candidate" >/dev/null 2>&1; then
            echo "FAIL: $candidate exists on disk but is NOT tracked by git" \
                 "(referenced by $cml -- check .gitignore is not swallowing it)"
            status=1
        else
            found_any=1
        fi
    done
    if [ "$found_any" = 0 ] && [ "$status" = 0 ]; then
        : # already reported as FAIL above for every referencing file
    fi
done

if [ "$status" = 0 ]; then
    echo "OK: all $checked *_no_python3.cmake reference(s) resolve to git-tracked files"
fi

exit $status
