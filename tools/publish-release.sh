#!/bin/bash
# publish-release.sh - publish a cut OVMX release bundle to GitHub Releases and
# record its generated release notes in-tree (vms-644, epic vms-a84 RELEASE
# ENGINEERING).
#
# THE GAP THIS CLOSES: tools/cut-release.sh builds a versioned, checksummed
# bundle (artifacts + SHA256SUMS + release-manifest.json + generated
# RELEASE-NOTES-<version>.md) into dist/release-<version>/ -- and then stops.
# Getting those artifacts in front of a user, and getting the release notes
# published, was still hand-work: someone drove `gh release create` by hand,
# picked which files to attach, and pasted notes into the web UI. That is
# exactly the per-cut manual chore the release-engineering pillar exists to
# replace (release-engineering-pillar; every release ships THROUGH the
# machinery). This is the publish half of that machinery.
#
# WHAT IT DOES:
#   1. Reads a bundle dir produced by tools/cut-release.sh (default
#      dist/release-<version>). The bundle's release-manifest.json is the
#      single source of truth for product name/version -- this script never
#      re-derives the version from source; it publishes exactly what was cut.
#   2. VERIFIES the bundle before publishing anything: `sha256sum -c
#      SHA256SUMS` must pass, so bytes whose checksums don't match are never
#      uploaded, and the requested tag must agree with the bundle's own
#      product_version (numeric core) -- publishing tag "0.4" from a bundle
#      that says it is V0.3 is a mismatch and a hard failure, not a silent
#      relabel.
#   3. PUBLISHES via `gh release create` (or, if the release/tag already
#      exists, `gh release upload --clobber` + `gh release edit`), attaching
#      every bundle file (the four artifacts, the OS-kit internal manifest,
#      SHA256SUMS, release-manifest.json) and using the bundle's generated
#      RELEASE-NOTES-<version>.md VERBATIM as the release body. The notes are
#      never re-written here -- they are gen_release_notes.py's output, and
#      publishing them unedited is the point (no hand-drift, vms-55a).
#   4. TRACKS the notes: copies the generated RELEASE-NOTES-<version>.md into
#      docs/release-notes/ (a version-controlled record) and `git add`s it,
#      so every published release leaves a reviewable in-tree note. It does
#      NOT commit or push -- committing the record and pushing the tag are
#      the operator's deliberate, externally-visible actions, not a side
#      effect of this script.
#
# WHAT IT DELIBERATELY DOES NOT DO: build anything (that is
# tools/cut-release.sh), create or push the git tag (the tag push is the
# gated, human-authorized release trigger -- see .github/workflows/release.yml,
# which fires ON a pushed release tag), or invent release notes.
#
# Usage:
#   tools/publish-release.sh [options]
#
# Options:
#   --bundle-dir DIR   Cut bundle to publish
#                      (default: <repo>/dist/release-<version>, where <version>
#                      is read from the newest dist/release-*/release-manifest.json;
#                      pass this explicitly if more than one bundle exists)
#   --tag TAG          Release tag to publish under (default: the bundle's
#                      product_version with a leading V/v stripped, e.g.
#                      V0.3 -> "0.3", matching this repo's existing tags)
#   --title TITLE      Release title (default: "<product_name> <product_version>")
#   --draft            Create the release as a draft (not visible to the public
#                      until published) -- recommended for a first cut
#   --prerelease       Mark the release as a pre-release
#   --repo OWNER/NAME  Target GitHub repo (default: gh's default for this
#                      checkout, i.e. the origin remote)
#   --no-record-notes  Do NOT write/stage the docs/release-notes/ tracked copy
#   --dry-run          Verify the bundle and PRINT the exact gh commands that
#                      would run, but make no GitHub or git mutation
#   -h, --help         Show this help and exit
#
# Exit 0 once the release is published (or, under --dry-run, once the plan is
# printed); nonzero with a diagnostic on any verification or publish failure.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"

BUNDLE_DIR=""
TAG=""
TITLE=""
DRAFT=0
PRERELEASE=0
GH_REPO=""
RECORD_NOTES=1
DRY_RUN=0

usage() {
    sed -n '2,64p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bundle-dir) BUNDLE_DIR="$2"; shift 2 ;;
        --bundle-dir=*) BUNDLE_DIR="${1#--bundle-dir=}"; shift ;;
        --tag) TAG="$2"; shift 2 ;;
        --tag=*) TAG="${1#--tag=}"; shift ;;
        --title) TITLE="$2"; shift 2 ;;
        --title=*) TITLE="${1#--title=}"; shift ;;
        --draft) DRAFT=1; shift ;;
        --prerelease) PRERELEASE=1; shift ;;
        --repo) GH_REPO="$2"; shift 2 ;;
        --repo=*) GH_REPO="${1#--repo=}"; shift ;;
        --no-record-notes) RECORD_NOTES=0; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "publish-release.sh: unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

log() { echo "publish-release: $*"; }
fail() { echo "publish-release: FATAL: $*" >&2; exit 1; }

command -v gh >/dev/null 2>&1 || fail "gh (GitHub CLI) not found -- needed to publish releases"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum not found"

# --- Locate the bundle ------------------------------------------------------
if [ -z "$BUNDLE_DIR" ]; then
    # Any dist/release-*/ that actually carries a manifest. If more than one
    # exists, refuse to guess which one to publish -- make the caller name it
    # (publishing the wrong bundle is exactly the kind of silent mistake this
    # machinery exists to prevent).
    CANDIDATES=()
    for cand in "$REPO_ROOT"/dist/release-*/; do
        [ -f "${cand%/}/release-manifest.json" ] || continue
        CANDIDATES+=("${cand%/}")
    done
    case "${#CANDIDATES[@]}" in
        0) fail "no cut bundle found under $REPO_ROOT/dist/release-*/ -- run tools/cut-release.sh first, or pass --bundle-dir" ;;
        1) BUNDLE_DIR="${CANDIDATES[0]}" ;;
        *) fail "multiple cut bundles under $REPO_ROOT/dist/release-*/ -- pass --bundle-dir to choose: ${CANDIDATES[*]}" ;;
    esac
fi
[ -d "$BUNDLE_DIR" ] || fail "bundle dir does not exist: $BUNDLE_DIR"

MANIFEST="$BUNDLE_DIR/release-manifest.json"
[ -f "$MANIFEST" ] || fail "not a cut bundle (no release-manifest.json): $BUNDLE_DIR"
[ -f "$BUNDLE_DIR/SHA256SUMS" ] || fail "bundle has no SHA256SUMS: $BUNDLE_DIR"

# --- Read the identity the bundle asserts about itself ----------------------
# release-manifest.json is emitted by cut-release.sh with one field per line
# (no embedded newlines), so a grep+sed extraction is exact and needs no JSON
# parser -- the same technique ci.yml already relies on for this file.
manifest_field() {
    sed -n "s/.*\"$1\": \"\\([^\"]*\\)\".*/\\1/p" "$MANIFEST" | head -1
}
PRODUCT_NAME="$(manifest_field product_name)"
PRODUCT_VERSION="$(manifest_field product_version)"
NOTES_BASENAME="$(sed -n 's/.*"file": "\(RELEASE-NOTES-[^"]*\.md\)".*/\1/p' "$MANIFEST" | head -1)"
[ -n "$PRODUCT_NAME" ] || fail "release-manifest.json has no product_name"
[ -n "$PRODUCT_VERSION" ] || fail "release-manifest.json has no product_version"
[ -n "$NOTES_BASENAME" ] || fail "release-manifest.json names no release-notes file"

NOTES_FILE="$BUNDLE_DIR/$NOTES_BASENAME"
[ -f "$NOTES_FILE" ] || fail "generated release notes missing from bundle: $NOTES_FILE"

# Default tag: the product version with a leading V/v stripped -- this repo's
# tags are bare ("0.2", "0.3"), while ovmx_identity.h carries "V0.3".
VERSION_CORE="${PRODUCT_VERSION#[Vv]}"
[ -n "$TAG" ] || TAG="$VERSION_CORE"

# The tag must agree with what the bundle says it is. Compare on the numeric
# core so "0.3" / "v0.3" / "V0.3" all match a V0.3 bundle, but "0.4" never
# does -- a tag that disagrees with the cut artifacts is a mismatch, not a
# relabel to be done silently.
TAG_CORE="${TAG#[Vv]}"
[ "$TAG_CORE" = "$VERSION_CORE" ] || \
    fail "tag/version mismatch: --tag '$TAG' (core '$TAG_CORE') != bundle product_version '$PRODUCT_VERSION' (core '$VERSION_CORE'). Refusing to publish a tag that disagrees with the cut bundle."

[ -n "$TITLE" ] || TITLE="$PRODUCT_NAME $PRODUCT_VERSION"

# --- Verify the bundle bytes before publishing anything ---------------------
log "verifying bundle checksums (SHA256SUMS) in $BUNDLE_DIR"
( cd "$BUNDLE_DIR" && sha256sum -c SHA256SUMS ) \
    || fail "SHA256SUMS verification failed -- bundle is corrupt or incomplete; refusing to publish"
log "checksums OK: bundle bytes match their recorded SHA256SUMS"

# Assemble the upload set: every file the bundle ships. SHA256SUMS lists them
# all (artifacts, kit manifest, release notes) -- read it rather than
# hard-coding artifact names, so a bundle that grows a file ships it too.
UPLOAD_FILES=()
while read -r _sum name; do
    [ -n "$name" ] || continue
    [ -f "$BUNDLE_DIR/$name" ] || fail "SHA256SUMS names a file not present in the bundle: $name"
    UPLOAD_FILES+=("$BUNDLE_DIR/$name")
done < "$BUNDLE_DIR/SHA256SUMS"
# SHA256SUMS and the machine-readable manifest are metadata about the cut and
# are not listed inside SHA256SUMS itself (it records the OTHER files); ship
# both alongside the artifacts so a downloader can verify what they pulled.
UPLOAD_FILES+=("$BUNDLE_DIR/SHA256SUMS")
UPLOAD_FILES+=("$MANIFEST")

# --- Give every asset a UNIQUE upload name (asset-name collision fix) ---------
# GitHub derives a release asset's NAME from the basename of the file handed to
# `gh release {create,upload}` -- there is NO per-asset name override flag (the
# `file#label` form only sets a cosmetic display label, not the asset name). A
# cut bundle ships the SAME VMS image basenames under more than one per-arch
# subdir: the whole `ovmx-images` set (STARTUP.EXE, DCL.EXE, LOGINOUT.EXE,
# STARTUP.COM, SYSUAF.DAT, ...) is duplicated under vax/ AND alpha/ (and any
# future x86_64/ arch dir). Uploading them all by basename made the SECOND
# upload of a shared basename fail with
#     HTTP 422 Validation Failed ... ReleaseAsset.name already exists
# and ABORT the whole publish -- the create was rolled back, so V0.5-4 ..
# V0.5-7 all cut cleanly yet SILENTLY published nothing (the gap this fixes).
#
# Fix: PATH-NAMESPACE the asset name. A bundle file at <archdir>/<name> uploads
# as <stem>-<ARCHDIR>.<ext> -- vax/STARTUP.EXE -> STARTUP-VAX.EXE,
# alpha/STARTUP.EXE -> STARTUP-ALPHA.EXE, x86_64/FOO.EXE -> FOO-X86_64.EXE
# (arch dir upper-cased; the token is inserted before the final extension, or
# appended if the basename has none). A TOP-LEVEL bundle file (vmlinuz,
# ovmx-distrib.img, SHA256SUMS, release-manifest.json, the notes, ...) keeps
# its name unchanged. This is a STABLE, documented scheme: the arch dir is the
# namespace, so the mapping is mechanical and reversible.
#
# The actual bundle files are NEVER renamed -- only the upload label is made
# unique -- so SHA256SUMS (which lists the real vax/… alpha/… relative paths)
# still describes the bundle exactly; a verifier re-creates the arch subdirs.
# We stage SYMLINKS under a temp dir with the unique names and upload those
# (symlinks are cheap; several bundle artifacts are hundreds of MB).
asset_name_for_relpath() {
    local rel="$1"
    case "$rel" in
        */*) : ;;                        # has an arch-dir component -> namespace
        *)   printf '%s' "$rel"; return ;;  # top-level file -> unchanged
    esac
    local dir base arch
    dir="${rel%%/*}"                     # first path component == the arch dir
    base="${rel##*/}"                    # the basename
    arch="$(printf '%s' "$dir" | tr '[:lower:]' '[:upper:]')"
    case "$base" in
        *.*) printf '%s-%s.%s' "${base%.*}" "$arch" "${base##*.}" ;;
        *)   printf '%s-%s' "$base" "$arch" ;;
    esac
}

STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ovmx-publish-assets.XXXXXX")"
cleanup_stage() { [ -n "${STAGE_DIR:-}" ] && rm -rf "$STAGE_DIR"; }
trap cleanup_stage EXIT

STAGED_FILES=()
declare -A SEEN_ASSET=()
for f in "${UPLOAD_FILES[@]}"; do
    rel="${f#"$BUNDLE_DIR"/}"
    aname="$(asset_name_for_relpath "$rel")"
    if [ -n "${SEEN_ASSET[$aname]:-}" ]; then
        fail "asset-name collision AFTER namespacing: '$aname' derives from both '${SEEN_ASSET[$aname]}' and '$rel' -- the arch-prefix scheme did not disambiguate these; refusing to publish a colliding set"
    fi
    SEEN_ASSET["$aname"]="$rel"
    ln -s "$f" "$STAGE_DIR/$aname"
    STAGED_FILES+=("$STAGE_DIR/$aname")
done
log "asset upload set: ${#STAGED_FILES[@]} files, per-arch basenames namespaced (e.g. vax/STARTUP.EXE -> STARTUP-VAX.EXE, alpha/STARTUP.EXE -> STARTUP-ALPHA.EXE) so no two assets share a name"

log "publishing $PRODUCT_NAME $PRODUCT_VERSION as tag '$TAG' (${#STAGED_FILES[@]} assets)"

GH_COMMON=()
[ -n "$GH_REPO" ] && GH_COMMON+=(--repo "$GH_REPO")

# --- Track the notes in-tree (version-controlled record) --------------------
if [ "$RECORD_NOTES" -eq 1 ]; then
    RECORD_DIR="$REPO_ROOT/docs/release-notes"
    RECORD_PATH="$RECORD_DIR/$NOTES_BASENAME"
    if [ "$DRY_RUN" -eq 1 ]; then
        log "[dry-run] would record notes: cp '$NOTES_FILE' '$RECORD_PATH' && git add '$RECORD_PATH'"
    else
        mkdir -p "$RECORD_DIR"
        cp "$NOTES_FILE" "$RECORD_PATH"
        git -C "$REPO_ROOT" add "$RECORD_PATH" \
            || log "WARNING: could not git-add $RECORD_PATH (not a fatal publish error)"
        log "recorded tracked notes at $RECORD_PATH (staged, NOT committed -- commit it with the tag)"
    fi
fi

# --- Does the release already exist? ----------------------------------------
RELEASE_EXISTS=0
if gh release view "$TAG" "${GH_COMMON[@]}" >/dev/null 2>&1; then
    RELEASE_EXISTS=1
fi

if [ "$DRY_RUN" -eq 1 ]; then
    log "[dry-run] release '$TAG' currently exists: $([ $RELEASE_EXISTS -eq 1 ] && echo yes || echo no)"
    if [ "$RELEASE_EXISTS" -eq 1 ]; then
        log "[dry-run] would run: gh release upload $TAG <${#STAGED_FILES[@]} assets> --clobber ${GH_COMMON[*]}"
        log "[dry-run] would run: gh release edit $TAG --notes-file '$NOTES_FILE' --title '$TITLE' ${GH_COMMON[*]}"
    else
        CREATE_FLAGS=""
        [ "$DRAFT" -eq 1 ] && CREATE_FLAGS+=" --draft"
        [ "$PRERELEASE" -eq 1 ] && CREATE_FLAGS+=" --prerelease"
        log "[dry-run] would run: gh release create $TAG <${#STAGED_FILES[@]} assets> --title '$TITLE' --notes-file '$NOTES_FILE'$CREATE_FLAGS ${GH_COMMON[*]}"
    fi
    log "[dry-run] namespaced asset names:"
    for a in "${STAGED_FILES[@]}"; do log "[dry-run]   $(basename "$a")"; done
    log "[dry-run] no GitHub or git mutation performed"
    exit 0
fi

# gh needs a token in non-interactive/CI use; fail honestly rather than hang.
gh auth status >/dev/null 2>&1 || fail "gh is not authenticated (set GH_TOKEN or run 'gh auth login')"

if [ "$RELEASE_EXISTS" -eq 1 ]; then
    log "release '$TAG' exists -- updating assets and notes (idempotent re-publish)"
    gh release upload "$TAG" "${STAGED_FILES[@]}" --clobber "${GH_COMMON[@]}" \
        || fail "gh release upload failed"
    gh release edit "$TAG" --notes-file "$NOTES_FILE" --title "$TITLE" "${GH_COMMON[@]}" \
        || fail "gh release edit failed"
else
    CREATE_ARGS=("$TAG" "${STAGED_FILES[@]}" --title "$TITLE" --notes-file "$NOTES_FILE")
    [ "$DRAFT" -eq 1 ] && CREATE_ARGS+=(--draft)
    [ "$PRERELEASE" -eq 1 ] && CREATE_ARGS+=(--prerelease)
    gh release create "${CREATE_ARGS[@]}" "${GH_COMMON[@]}" \
        || fail "gh release create failed"
fi

log "published: $PRODUCT_NAME $PRODUCT_VERSION -> release '$TAG' with ${#STAGED_FILES[@]} assets"
gh release view "$TAG" "${GH_COMMON[@]}" | head -20 || true
