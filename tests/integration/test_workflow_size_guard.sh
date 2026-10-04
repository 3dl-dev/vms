#!/bin/sh
#
# test_workflow_size_guard.sh - every GitHub Actions workflow file stays well
# under GitHub's silent workflow-file size ceiling (rd vms-1af).
#
# WHAT THIS GUARDS AGAINST. GitHub silently refuses to run a workflow file
# once it crosses a size ceiling somewhere around 511,986-512,030 raw bytes:
# the run is created and immediately ends conclusion=startup_failure with
# ZERO jobs, and a pull_request event produces NO RUN AT ALL -- no
# annotation, no error anywhere in the UI or API (measured by bisection
# against the real Actions API, rd vms-1af / vms-e18e). ci.yml hit this at
# 511,343 bytes and had to be split (rd vms-1af) into the ci-*.yml siblings.
# Without a standing gate, any one of those siblings (or any future workflow
# file) can silently grow back into the same ceiling and nobody would notice
# until a PR's checks list came back quietly missing an entire suite.
#
# THE MECHANISM: tools/ci/check_workflow_sizes.py checks each workflow
# file's RAW size and fails if any file exceeds a threshold (default
# 450000 bytes, comfortably under the measured 511,986-512,030 ceiling). Raw
# bytes are a strict upper bound on whatever GitHub actually measures (see
# that script's header for why this deliberately does not try to be clever
# about excluding comments). Pure source/text-scan gate -- no docker, no
# QEMU, cheap enough to run on every build.
#
# See test_workflow_size_guard_negctl.sh for the negative control that
# proves this checker can actually go red on an oversized file, not just
# report OK no matter what it is handed.
#
# Usage: test_workflow_size_guard.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
CHECK="$SRC_ROOT/tools/ci/check_workflow_sizes.py"
WORKFLOWS_DIR="$SRC_ROOT/.github/workflows"

command -v python3 >/dev/null 2>&1 || { echo "FAIL: python3 not available"; exit 1; }
[ -f "$CHECK" ] || { echo "FAIL: $CHECK missing"; exit 1; }
[ -d "$WORKFLOWS_DIR" ] || { echo "FAIL: $WORKFLOWS_DIR missing"; exit 1; }

echo "Workflow-size gate: every .github/workflows/*.yml under the guard threshold"

python3 "$CHECK" --dir "$WORKFLOWS_DIR"
rc=$?

if [ "$rc" -eq 0 ]; then
    echo "PASS: all workflow files under the guard threshold"
else
    echo "FAIL: a workflow file is at risk of GitHub's silent ~512000-byte ceiling (rd vms-1af) -- split it further"
fi

exit "$rc"
