#!/bin/sh
#
# test_corpus_scoreboard_negctl.sh - proves test_corpus_scoreboard.sh can go red.
# Each case mutates a COPY of the inputs and asserts the gate rejects it; the
# unmutated copy must be accepted first (a gate that rejects everything proves nothing).
set -u
SRC=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
rc=0

mk() {
    rm -rf "$TMP/t"
    mkdir -p "$TMP/t/tests/conformance" "$TMP/t/tests/corpus" "$TMP/t/docs"
    cp "$SRC"/tests/conformance/corpus_baseline.json "$SRC"/tests/conformance/corpus_runtime_baseline.json "$TMP/t/tests/conformance/"
    cp "$SRC"/tests/corpus/expected_exit.txt "$SRC"/tests/corpus/unreachable.txt "$SRC"/tests/corpus/tiers.txt "$TMP/t/tests/corpus/"
    for d in "$SRC"/tests/corpus/tier*; do mkdir -p "$TMP/t/tests/corpus/$(basename "$d")"; done
    cp "$SRC"/docs/corpus-scoreboard.md "$TMP/t/docs/"
}
gate() { python3 "$SRC/tools/corpus/render_scoreboard.py" --check --root="$TMP/t" >/dev/null 2>&1; }

mk
gate || { echo "NEGCTL FAIL: the unmutated inputs are rejected -- the control proves nothing"; exit 1; }

expect_red() {
    if gate; then echo "NEGCTL FAIL: $1 was ACCEPTED"; rc=1; else echo "ok: $1 is rejected"; fi
}

mk; echo "<!-- hand edit -->" >> "$TMP/t/docs/corpus-scoreboard.md"
expect_red "a hand-edited scoreboard page"

mk; sed -i '/^sys_cancel /d' "$TMP/t/tests/corpus/unreachable.txt"
expect_red "a non-running program with no written reason"

mk; mkdir "$TMP/t/tests/corpus/tier9-unlisted"
expect_red "a corpus tier directory missing from tiers.txt"

mk; sed -i 's/^tier6-laxdriver .*/tier6-laxdriver     not-running  no/' "$TMP/t/tests/corpus/tiers.txt"
expect_red "a not-running tier with no written reason"

[ "$rc" -eq 0 ] && echo "PASS: the corpus scoreboard gate rejects stale pages, reasonless programs, unlisted tiers"
exit $rc
