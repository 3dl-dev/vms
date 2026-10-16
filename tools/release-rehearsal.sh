#!/bin/bash
# release-rehearsal.sh - run release.yml's OWN path against a throwaway
# annotated tag before the real tag is pushed (docs/releasing.md, cut
# checklist step 2).
#
# Why: the pre-tag legs (Kernel Executive, Alpha, VAX, Release E2E) cut a
# BRANCH. Only release.yml cuts a TAG, so a release-only path used to run for
# the first time on the real, public tag. V0.7-11's first publish failed that
# way: cut-release.sh resolved the annotated tag to its tag object, not its
# commit (release.yml run 38845187331).
#
# What it does:
#   1. makes an annotated tag rehearse/<version>-<sha7> at --ref's commit and
#      pushes it (no release workflow or demo build triggers on rehearse/);
#   2. dispatches release.yml from --ref with rehearsal=true: cut-aarch64,
#      cut-release.sh --ref <tag>, the notes check and a publish --dry-run
#      all run, and nothing is created on GitHub;
#   3. waits for the run, then ALWAYS deletes the tag, remote and local.
# Exit 0 only if the run succeeded.
#
# usage: tools/release-rehearsal.sh --ref <remote branch> [--repo OWNER/NAME]
set -euo pipefail
REF=""
REPO="3dl-dev/vms"
while [ $# -gt 0 ]; do
    case "$1" in
        --ref) REF=${2:?}; shift 2 ;;
        --repo) REPO=${2:?}; shift 2 ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "usage: $0 --ref <remote branch> [--repo OWNER/NAME]" >&2; exit 2 ;;
    esac
done
[ -n "$REF" ] || { echo "usage: $0 --ref <remote branch> [--repo OWNER/NAME]" >&2; exit 2; }

ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
git -C "$ROOT" fetch --quiet origin "$REF"
SHA=$(git -C "$ROOT" rev-parse --verify "FETCH_HEAD^{commit}")
VERSION=$(git -C "$ROOT" show "$SHA:src/libvms/include/ovmx_identity.h" \
          | sed -n 's/^#define OVMX_PRODUCT_VERSION[[:space:]]*"\([^"]*\)".*/\1/p')
[ -n "$VERSION" ] || { echo "release-rehearsal: no OVMX_PRODUCT_VERSION at $SHA" >&2; exit 1; }
TAG="rehearse/$VERSION-${SHA:0:7}"

cleanup() {
    git -C "$ROOT" push --quiet origin ":refs/tags/$TAG" 2>/dev/null || true
    git -C "$ROOT" tag -d "$TAG" >/dev/null 2>&1 || true
    echo "release-rehearsal: deleted $TAG"
}
trap cleanup EXIT

git -C "$ROOT" tag -a "$TAG" "$SHA" -m "release rehearsal for $VERSION at $SHA (throwaway)"
git -C "$ROOT" push --quiet origin "refs/tags/$TAG"
echo "release-rehearsal: $TAG -> $SHA ($VERSION)"

BEFORE=$(date -u +%Y-%m-%dT%H:%M:%SZ)
gh workflow run release.yml --repo "$REPO" --ref "$REF" -f tag="$TAG" -f rehearsal=true -f draft=true
RUN=""
for _ in $(seq 1 30); do
    RUN=$(gh run list --repo "$REPO" --workflow release.yml --event workflow_dispatch --branch "$REF" \
            --json databaseId,createdAt --jq "[.[] | select(.createdAt >= \"$BEFORE\")][0].databaseId // empty")
    [ -n "$RUN" ] && break
    sleep 10
done
[ -n "$RUN" ] || { echo "release-rehearsal: the dispatched run never appeared" >&2; exit 1; }
echo "release-rehearsal: release.yml run $RUN"
if gh run watch "$RUN" --repo "$REPO" --exit-status --interval 60 >/dev/null; then
    echo "release-rehearsal: PASS -- release.yml cut $TAG end to end (run $RUN)"
else
    echo "release-rehearsal: FAIL -- release.yml run $RUN failed on an annotated tag; fix it before the real tag" >&2
    exit 1
fi
