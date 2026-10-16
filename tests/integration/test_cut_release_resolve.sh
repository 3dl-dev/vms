#!/bin/bash
# test_cut_release_resolve.sh - tools/cut-release.sh resolves an ANNOTATED
# release tag to the COMMIT it names, never to the tag object.
#
# Every release tag is annotated. V0.7-11's first publish (release.yml run
# 38845187331) failed because cut-release.sh resolved `--ref V0.7-11` with a
# bare `git rev-parse`, which yields the tag OBJECT, while release.yml's
# cut-aarch64 job peels the tag (^{commit}); the prebuilt aarch64 set was then
# refused as "built from a different commit". No pre-tag leg cut a tag, so the
# release-only path ran for the first time on the real tag.
#
# Builds a throwaway repo holding a copy of cut-release.sh, tags it with an
# annotated tag and a lightweight tag, and checks --resolve-only. Pure shell +
# git; no docker, no network.
#
# usage: test_cut_release_resolve.sh <repo-root>
set -euo pipefail
ROOT=${1:?usage: test_cut_release_resolve.sh <repo-root>}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

g() { git -C "$T/r" -c user.name=t -c user.email=t@t -c tag.gpgSign=false -c commit.gpgSign=false "$@"; }
mkdir -p "$T/r/tools"
git init -q "$T/r"
cp "$ROOT/tools/cut-release.sh" "$T/r/tools/cut-release.sh"
g add -A
GIT_COMMITTER_DATE=@1700000000 g commit -q -m base
g tag -a V9.9-1 -m "annotated release tag"
g tag V9.9-2
C=$(g rev-parse HEAD)
TAGOBJ=$(g rev-parse V9.9-1)
[ "$C" != "$TAGOBJ" ] || fail "fixture: an annotated tag's object must differ from its commit"

for ref in V9.9-1 V9.9-2 HEAD "$C"; do
    out=$("$T/r/tools/cut-release.sh" --ref "$ref" --resolve-only)
    [ "$out" = "commit=$C source_date_epoch=1700000000" ] \
        || fail "--ref $ref resolved to '$out', want commit=$C source_date_epoch=1700000000"
    echo "PASS: --ref $ref -> the commit it names"
done

# Negative control: the tag object itself is what the old resolution returned.
case "$("$T/r/tools/cut-release.sh" --ref V9.9-1 --resolve-only)" in
    *"$TAGOBJ"*) fail "the annotated tag resolved to its tag object $TAGOBJ" ;;
esac
echo "PASS: the annotated tag never resolves to its tag object"

# A ref that names no commit is refused, not passed through.
if "$T/r/tools/cut-release.sh" --ref no-such-ref --resolve-only >/dev/null 2>&1; then
    fail "an unknown ref was accepted"
fi
echo "PASS: an unknown ref is refused"
