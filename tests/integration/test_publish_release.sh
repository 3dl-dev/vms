#!/bin/bash
# test_publish_release.sh - ground-source gate for tools/publish-release.sh
# (vms-644, epic vms-a84 RELEASE ENGINEERING).
#
# tools/publish-release.sh is the publish half of the release machinery: it
# takes a cut bundle (tools/cut-release.sh output) and pushes its artifacts +
# generated notes to a GitHub Release. This test proves that publisher does
# the right thing WITHOUT touching GitHub or the real repo, by running it
# inside a throwaway git repo against a stub `gh` on PATH that records exactly
# how it was invoked. It asserts BOTH that a good bundle publishes correctly
# (assets attached, generated notes used as the body, in-tree record staged)
# AND -- the part that makes the gate non-vacuous -- that a corrupt bundle or
# a tag that disagrees with the bundle is REFUSED and never reaches gh.
#
# Usage: test_publish_release.sh [SRC_ROOT]
#   SRC_ROOT defaults to the repo this script lives in; only tools/publish-release.sh
#   is read from it.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_ROOT="${1:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
PUBLISH="$SRC_ROOT/tools/publish-release.sh"

fail() { echo "FAIL: $*" >&2; exit 1; }
[ -f "$PUBLISH" ] || fail "tools/publish-release.sh not found at $PUBLISH"

command -v git >/dev/null 2>&1 || fail "git required"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ovmx-test-publish.XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

PASS=0
ok() { echo "  ok: $*"; PASS=$((PASS + 1)); }

# --- A stub `gh` that records its calls and simulates release state ---------
# GH_LOG accumulates one line per invocation. GH_RELEASE_EXISTS controls what
# `gh release view` reports, so a single stub drives both the create path and
# the idempotent re-publish path.
BIN="$WORK/bin"
mkdir -p "$BIN"
GH_LOG="$WORK/gh.log"
cat > "$BIN/gh" <<'STUB'
#!/bin/bash
echo "gh $*" >> "$GH_LOG"
case "$1 $2" in
    "auth status") exit 0 ;;
    "release view")
        [ "${GH_RELEASE_EXISTS:-0}" = "1" ] && exit 0 || exit 1 ;;
    "release create"|"release upload"|"release edit") exit 0 ;;
    *) exit 0 ;;
esac
STUB
chmod +x "$BIN/gh"
export GH_LOG
export PATH="$BIN:$PATH"

# --- Build a throwaway repo with a fake cut bundle --------------------------
# A fake product version well outside anything real (V9.9-test) so the in-tree
# notes record this writes can never collide with a shipped release's notes,
# and stays entirely inside $WORK.
REPO="$WORK/repo"
mkdir -p "$REPO/tools"
cp "$PUBLISH" "$REPO/tools/publish-release.sh"
git -C "$REPO" init -q
git -C "$REPO" config user.email t@t && git -C "$REPO" config user.name t
git -C "$REPO" add -A && git -C "$REPO" commit -qm init

VERSION="V9.9-test"
TAG_CORE="9.9-test"
NOTES="RELEASE-NOTES-$VERSION.md"

# make_bundle DIR -- write a complete, self-consistent cut bundle into DIR.
make_bundle() {
    local d="$1"
    mkdir -p "$d"
    # Stand-in artifacts (the publisher never inspects their content, only
    # their checksums) plus the OS-kit internal manifest and generated notes.
    printf 'fake-kernel\n'   > "$d/vmlinuz"
    printf 'fake-initrd\n'   > "$d/initramfs-ovmx-slim.cpio.gz"
    printf 'fake-distrib\n'  > "$d/ovmx-distrib.img"
    printf 'fake-kit\n'      > "$d/ovmx-os.kit"
    printf 'DCL.EXE\nLOGINOUT.EXE\nSTARTUP.COM\n' > "$d/ovmx-os.kit.manifest.txt"
    printf '# OpenVMX %s Release Notes\n\n## Features\n- something (vms-000)\n' "$VERSION" > "$d/$NOTES"
    ( cd "$d" && sha256sum vmlinuz initramfs-ovmx-slim.cpio.gz ovmx-distrib.img \
        ovmx-os.kit ovmx-os.kit.manifest.txt "$NOTES" > SHA256SUMS )
    cat > "$d/release-manifest.json" <<EOF
{
  "_format": "ovmx-release-manifest",
  "product_name": "OpenVMX",
  "product_version": "$VERSION",
  "release_notes": {
    "file": "$NOTES"
  }
}
EOF
}

# ============================================================================
# Case 1: a good bundle, no existing release -> `gh release create` with every
# asset attached and the GENERATED notes used verbatim as the release body.
# ============================================================================
: > "$GH_LOG"
BUNDLE="$REPO/dist/release-$VERSION"
make_bundle "$BUNDLE"
GH_RELEASE_EXISTS=0 "$REPO/tools/publish-release.sh" --bundle-dir "$BUNDLE" \
    > "$WORK/out1.txt" 2>&1 \
    || { cat "$WORK/out1.txt"; fail "case1: publisher exited nonzero on a good bundle"; }

grep -q "^gh release create $TAG_CORE " "$GH_LOG" \
    || { cat "$GH_LOG"; fail "case1: did not call 'gh release create $TAG_CORE'"; }
ok "good bundle -> gh release create under the version-derived tag ($TAG_CORE)"

CREATE_LINE="$(grep '^gh release create' "$GH_LOG")"
for asset in vmlinuz initramfs-ovmx-slim.cpio.gz ovmx-distrib.img ovmx-os.kit \
             ovmx-os.kit.manifest.txt SHA256SUMS release-manifest.json; do
    case "$CREATE_LINE" in
        *"$asset"*) ;;
        *) fail "case1: asset '$asset' was not attached to the release" ;;
    esac
done
ok "all four artifacts + kit manifest + SHA256SUMS + release-manifest.json attached"

case "$CREATE_LINE" in
    *"--notes-file"*"$NOTES"*) ok "generated $NOTES used as the release body (--notes-file)" ;;
    *) fail "case1: generated notes were not passed as --notes-file" ;;
esac

# ...and the notes were recorded in-tree, staged but not committed.
RECORD="$REPO/docs/release-notes/$NOTES"
[ -f "$RECORD" ] || fail "case1: notes not recorded at docs/release-notes/$NOTES"
git -C "$REPO" diff --cached --name-only | grep -qx "docs/release-notes/$NOTES" \
    || fail "case1: recorded notes were not git-added (staged)"
[ -z "$(git -C "$REPO" log --oneline 2>/dev/null | grep -i 'release-notes')" ] \
    || fail "case1: publisher must not create a commit"
ok "generated notes recorded + staged in-tree (docs/release-notes/), not committed"

# ============================================================================
# Case 2: release already exists -> idempotent re-publish uses upload --clobber
# and edits the notes, and does NOT call create.
# ============================================================================
: > "$GH_LOG"
git -C "$REPO" reset -q   # unstage case-1's record so this run stages afresh
GH_RELEASE_EXISTS=1 "$REPO/tools/publish-release.sh" --bundle-dir "$BUNDLE" \
    > "$WORK/out2.txt" 2>&1 \
    || { cat "$WORK/out2.txt"; fail "case2: publisher exited nonzero on re-publish"; }
grep -q '^gh release upload .*--clobber' "$GH_LOG" \
    || { cat "$GH_LOG"; fail "case2: existing release did not upload --clobber"; }
grep -q '^gh release edit .*--notes-file' "$GH_LOG" \
    || { cat "$GH_LOG"; fail "case2: existing release did not edit notes"; }
grep -q '^gh release create' "$GH_LOG" \
    && fail "case2: must not create a release that already exists"
ok "existing release -> idempotent upload --clobber + edit, never create"

# ============================================================================
# Case 3 (NEGATIVE): a corrupt artifact -> checksum verify fails, nothing is
# published. This is what makes the gate real: bad bytes never reach gh.
# ============================================================================
: > "$GH_LOG"
BADSUM="$REPO/dist/release-badsum"
make_bundle "$BADSUM"
printf 'tampered\n' > "$BADSUM/ovmx-distrib.img"   # after SHA256SUMS was written
if GH_RELEASE_EXISTS=0 "$REPO/tools/publish-release.sh" --bundle-dir "$BADSUM" \
        --no-record-notes > "$WORK/out3.txt" 2>&1; then
    fail "case3: publisher succeeded on a bundle whose checksums do not match"
fi
grep -q '^gh release' "$GH_LOG" && fail "case3: called gh despite a checksum failure"
ok "corrupt artifact -> refused before any gh call (checksum guard)"

# ============================================================================
# Case 4 (NEGATIVE): a tag that disagrees with the bundle's product_version is
# refused -- publishing "0.4" from a V9.9-test bundle is a mismatch.
# ============================================================================
: > "$GH_LOG"
if GH_RELEASE_EXISTS=0 "$REPO/tools/publish-release.sh" --bundle-dir "$BUNDLE" \
        --tag 0.4 --no-record-notes > "$WORK/out4.txt" 2>&1; then
    fail "case4: publisher accepted a tag that disagrees with product_version"
fi
grep -qi 'mismatch' "$WORK/out4.txt" || fail "case4: expected a tag/version mismatch diagnostic"
grep -q '^gh release' "$GH_LOG" && fail "case4: called gh despite a tag/version mismatch"
ok "tag disagreeing with bundle product_version -> refused before any gh call"

# ============================================================================
# Case 5: --dry-run verifies + plans but performs no gh or git mutation.
# ============================================================================
: > "$GH_LOG"
DRYREPO_BEFORE="$(git -C "$REPO" status --porcelain)"
GH_RELEASE_EXISTS=0 "$REPO/tools/publish-release.sh" --bundle-dir "$BUNDLE" \
    --dry-run > "$WORK/out5.txt" 2>&1 \
    || { cat "$WORK/out5.txt"; fail "case5: --dry-run exited nonzero on a good bundle"; }
grep -qi 'would run: gh release create' "$WORK/out5.txt" \
    || fail "case5: --dry-run did not print the gh create plan"
# The only gh a dry-run may touch is the read-only existence probe; it must
# never create/upload/edit.
grep -qE '^gh release (create|upload|edit)' "$GH_LOG" \
    && fail "case5: --dry-run performed a mutating gh call"
[ "$(git -C "$REPO" status --porcelain)" = "$DRYREPO_BEFORE" ] \
    || fail "case5: --dry-run mutated the git tree"
ok "--dry-run verifies + prints plan, no gh mutation, no git mutation"

# ============================================================================
# Case 6 (REGRESSION): a real cut bundle ships the same VMS image basenames
# under more than one per-arch subdir (vax/STARTUP.EXE AND alpha/STARTUP.EXE).
# GitHub names an asset by its basename, so uploading by basename made the
# SECOND upload 422 ("ReleaseAsset.name already exists") and abort the whole
# publish -- the silent V0.5-4..V0.5-8 gap. The publisher must PATH-NAMESPACE
# per-arch assets (vax/STARTUP.EXE -> STARTUP-VAX.EXE) so every uploaded name
# is unique, WITHOUT renaming the actual bundle files (SHA256SUMS still lists
# the real vax/… alpha/… paths).
# ============================================================================
: > "$GH_LOG"
git -C "$REPO" reset -q
MULTI="$REPO/dist/release-multiarch"
make_bundle "$MULTI"                          # the flat top-level artifacts + notes
# Add two per-arch dirs whose basenames deliberately collide across arches.
for a in vax alpha; do
    mkdir -p "$MULTI/$a"
    printf '%s-startup\n'  "$a" > "$MULTI/$a/STARTUP.EXE"
    printf '%s-dcl\n'      "$a" > "$MULTI/$a/DCL.EXE"
    printf '%s-manifest\n' "$a" > "$MULTI/$a/$a-artifact-manifest.txt"
done
# Rebuild SHA256SUMS to include the per-arch (subdir-prefixed) files too.
( cd "$MULTI" && sha256sum \
    vmlinuz initramfs-ovmx-slim.cpio.gz ovmx-distrib.img ovmx-os.kit \
    ovmx-os.kit.manifest.txt "$NOTES" \
    vax/STARTUP.EXE vax/DCL.EXE vax/vax-artifact-manifest.txt \
    alpha/STARTUP.EXE alpha/DCL.EXE alpha/alpha-artifact-manifest.txt \
    > SHA256SUMS )

GH_RELEASE_EXISTS=0 "$REPO/tools/publish-release.sh" --bundle-dir "$MULTI" \
    --no-record-notes > "$WORK/out6.txt" 2>&1 \
    || { cat "$WORK/out6.txt"; fail "case6: publisher failed on a valid multi-arch bundle"; }
ok "multi-arch bundle (colliding per-arch basenames) publishes without error"

CREATE6="$(grep '^gh release create' "$GH_LOG")"
# The colliding basenames must have been namespaced by arch dir.
for want in STARTUP-VAX.EXE STARTUP-ALPHA.EXE DCL-VAX.EXE DCL-ALPHA.EXE; do
    case "$CREATE6" in
        *"$want"*) ;;
        *) { echo "$CREATE6"; fail "case6: expected namespaced asset '$want' not uploaded"; } ;;
    esac
done
ok "per-arch collisions namespaced (STARTUP-VAX.EXE / STARTUP-ALPHA.EXE, DCL-VAX/-ALPHA)"

# The bare colliding basename must NOT appear as an upload arg, and no two
# upload-arg basenames may repeat -- that is the 422 the fix prevents.
BASENAMES="$(printf '%s\n' $CREATE6 | sed -n 's#.*/##p' | grep -E '\.(EXE|txt|img|gz|kit|json)$|SHA256SUMS|^vmlinuz$')"
DUPES="$(printf '%s\n' "$BASENAMES" | sort | uniq -d)"
[ -z "$DUPES" ] || { echo "duplicate asset names: $DUPES"; fail "case6: two upload assets share a name (the 422 bug)"; }
ok "no two uploaded assets share a name (asset-collision 422 cannot recur)"

echo "PASS: test_publish_release.sh ($PASS checks)"
