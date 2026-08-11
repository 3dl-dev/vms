#!/bin/bash
# test_gen_release_notes.sh - ground-source gate for tools/gen_release_notes.py's
# previous-release-tag detection (vms-644, epic vms-a84 RELEASE ENGINEERING).
#
# THE BUG THIS PINS: previous_release_tag() finds the highest release-shaped
# tag that is an ANCESTOR of --ref. A commit is its own ancestor, so when
# --ref IS a release tag (the .github/workflows/release.yml tag-push publish
# path checks out the tag and runs cut-release.sh --ref <tag>; a retroactive
# cut of an existing tag does the same), the tag itself was selected as the
# "previous" release -> an empty <tag>..<tag> range -> release notes with ZERO
# commits. That is "release notes aren't present" shipping straight through the
# machinery. The fix excludes any candidate tag pointing at --ref's own commit,
# so the previous release is the highest STRICT ancestor.
#
# This test builds a throwaway git repo with two release tags and asserts:
#   1. Cutting the NEWER tag by name (--ref <tag>) reports the OLDER tag as the
#      previous release and lists exactly the commits between them (non-empty).
#   2. The negative control that makes the gate non-vacuous: the pre-fix
#      behaviour -- picking the ref's own tag and emitting "No commits since
#      the previous release tag" -- must NOT occur.
# No GitHub, no network, no docker.
#
# Usage: test_gen_release_notes.sh [SRC_ROOT]
#   SRC_ROOT defaults to the repo this script lives in; only
#   tools/gen_release_notes.py is read from it.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_ROOT="${1:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
GEN="$SRC_ROOT/tools/gen_release_notes.py"

fail() { echo "FAIL: $*" >&2; exit 1; }
command -v git >/dev/null 2>&1 || fail "git required"
command -v python3 >/dev/null 2>&1 || fail "python3 required"
[ -f "$GEN" ] || fail "tools/gen_release_notes.py not found at $GEN"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ovmx-test-gennotes.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
REPO="$WORK/repo"
mkdir -p "$REPO/src/libvms/include"
cd "$REPO" || fail "cannot cd $REPO"

git init -q
git config user.email test@ovmx.invalid
git config user.name "OVMX Test"
# Deterministic history: fixed dates so the test never depends on wall clock.
export GIT_AUTHOR_DATE="2026-01-01T00:00:00 +0000"
export GIT_COMMITTER_DATE="2026-01-01T00:00:00 +0000"

id_header() { # $1 = version string, e.g. V0.2
    cat > src/libvms/include/ovmx_identity.h <<EOF
#define OVMX_PRODUCT_NAME "OVMX"
#define OVMX_PRODUCT_VERSION "$1"
EOF
}

# --- Build history: v0.1 <- (feature commit) <- 0.2 -------------------------
id_header "V0.1"
git add -A && git commit -q -m "seed: initial identity"
git tag v0.1.0

echo a > a.txt && git add -A && git commit -q -m "feat: alpha feature between releases"
echo b > b.txt && git add -A && git commit -q -m "fix: a bug fixed between releases"
id_header "V0.2"
git add -A && git commit -q -m "release: cut 0.2"
git tag 0.2

# --- 1. Cutting the newer tag by name must span v0.1.0..0.2 (non-empty) ------
OUT="$WORK/notes-0.2.md"
python3 "$GEN" --repo-root "$REPO" --ref 0.2 --out "$OUT" 2>"$WORK/gen.err" \
    || { cat "$WORK/gen.err" >&2; fail "gen_release_notes.py exited nonzero for --ref 0.2"; }

grep -q "since \`v0.1.0\`" "$OUT" \
    || fail "expected previous-release tag v0.1.0 in the notes, got:\n$(cat "$OUT")"
grep -qF "feat: alpha feature between releases" "$OUT" \
    || fail "notes for 0.2 do not list the feature commit between v0.1.0 and 0.2"
grep -qF "fix: a bug fixed between releases" "$OUT" \
    || fail "notes for 0.2 do not list the fix commit between v0.1.0 and 0.2"

# --- 2. Negative control: the empty-range regression must NOT reappear -------
if grep -q "No commits since the previous release tag" "$OUT"; then
    fail "REGRESSION: --ref 0.2 picked its own tag as 'previous' -> empty notes"
fi
if grep -q "since \`0.2\`" "$OUT"; then
    fail "REGRESSION: previous-release detection selected the ref's own tag (0.2)"
fi

echo "PASS: gen_release_notes.py cuts <prev-tag>..<tag> when --ref is a release tag (non-empty notes)"
exit 0
