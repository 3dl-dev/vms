#!/bin/sh
#
# test_corpus_scoreboard.sh - the published corpus scoreboard is current and every
# non-running corpus program carries a written reason (rd vms-44a, roadmap R2).
#
# docs/corpus-scoreboard.md is GENERATED (tools/corpus/render_scoreboard.py) from
# the committed measurements -- tests/conformance/corpus_baseline.json (host
# column) and corpus_runtime_baseline.json (runtime column) -- and the written
# ledgers tests/corpus/{expected_exit,unreachable,tiers}.txt. This gate fails when:
#   * the committed document differs from what the sources render (a baseline was
#     raised without regenerating, or the page was hand-edited);
#   * a tier-1 program that is not running has neither the harness's own
#     compile/link diagnostic nor a line in unreachable.txt;
#   * a tests/corpus/tier* directory is missing from tiers.txt, or is marked
#     not-running without a reason.
# Negative control: test_corpus_scoreboard_negctl.sh.
#
# Usage: test_corpus_scoreboard.sh [SRC_ROOT]
set -eu
SRC=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
exec python3 "$SRC/tools/corpus/render_scoreboard.py" --check --root="$SRC"
