#!/bin/bash
# cut-release.sh - build a versioned, checksummed OVMX release bundle from a
# CLEAN tree (vms-d73, epic vms-a84 RELEASE ENGINEERING).
#
# THE PROBLEM THIS REPLACES: every OVMX release before this was hand-rolled --
# a conductor ran the container build, copied artifacts out, and hand-wrote
# checksums and release notes. There was no repeatable "cut a release"
# command. This is that command.
#
# WHAT IT DOES:
#   1. git-archives the requested ref to a scratch tree UNDER /tmp and builds
#      THERE -- never the working repo (CLAUDE.md "never rw-mount + build
#      into the repo"; the standing containerized-build rule, Project Rule 9).
#   2. Runs the existing containerized build (distro/Dockerfile.bootable,
#      unchanged pipeline -- this script does not reinvent the build, it
#      drives it) via `docker buildx build --output=local`, extracting
#      vmlinuz / initramfs-ovmx-slim.cpio.gz / ovmx-distrib.img / ovmx-os.kit
#      to the host.
#   3. Reads OVMX_PRODUCT_NAME / OVMX_PRODUCT_VERSION out of the ARCHIVED
#      copy of src/libvms/include/ovmx_identity.h -- the identity SSOT
#      (INV-1) -- so the version is single-sourced from the exact commit
#      being cut, never a literal in this script.
#   4. Emits SHA256SUMS and a machine-readable release-manifest.json (an
#      OVMX-defined format, Rule 8: not a VSI/PCSI artifact, labeled as ours)
#      mapping component -> version -> sha256, including the OS kit's own
#      internal file manifest (tools/ovmx_kit_pack.c) so DCL.EXE/LOGINOUT.EXE/
#      STARTUP.COM's presence in the shipped kit is asserted from the CUT
#      BUNDLE itself, not merely from the build's internal log.
#   5. Generates RELEASE-NOTES-<version>.md via tools/gen_release_notes.py --
#      the merged git history since the previous release tag, never a
#      hand-written changelog (vms-55a, epic vms-a84). Run against the
#      ORIGINAL repo's git history (the archived scratch tree at $SRC_DIR has
#      no .git), scoped to exactly the commit being cut so a cut from an
#      older ref never carries notes for commits after it.
#   6. Writes the bundle to dist/release-<version>/ (or --out-dir).
#   7. Builds the VAX (netbsd-vax/elf32-vax) release artifact set into
#      OUT_DIR/vax/ via tools/cut-release-vax.sh -- the CO-RELEASE GATE
#      (rd vms-ca5, epic vms-f10, docs/design-vax-mainstream-release.md):
#      VAX is a first-class release platform, so for any tree that HAS the
#      VAX tooling this call is UNCONDITIONAL (no --skip-vax escape hatch)
#      -- a broken VAX build fails THIS script, which fails the cut, for
#      every caller. EXCEPTION: a ref that predates VAX co-release support
#      (no tools/cross-vax/Dockerfile in the archived tree -- e.g. a
#      pre-version-bump baseline tests/qemu/run_upgrade_e2e.sh pins as an
#      upgrade-test fixture) is not "VAX broken", it is a tree from before
#      the capability existed -- that case is skipped LOUDLY (a
#      %CUT-VAX-I-PREVAX log line + a "skipped"/"skipped_reason" field in
#      release-manifest.json), never silently.
#
# Byte-reproducibility (the vms-d73 DONE criterion) depends on
# SOURCE_DATE_EPOCH: two cuts of the SAME commit must embed the SAME clock
# into every wall-clock field the build would otherwise stamp (the OS kit's
# kh_build_time, the mastered VMSFS image's per-file timestamps, and every
# packed initramfs's cpio/gzip metadata -- see distro/Dockerfile.bootable and
# tools/ovmx_kit_pack.c / tools/vmsfs_master.c). This script defaults that
# clock to the CUT COMMIT'S timestamp, so two independent cuts of the same
# ref are deterministic without the caller doing anything -- see
# .github/workflows/ci.yml's cut-release-reproducible job, which builds
# twice from a clean tree and diffs the artifact bytes.
#
# Usage:
#   tools/cut-release.sh [options]
#
# Options:
#   --ref REF                Git ref/commit to cut (default: HEAD)
#   --out-dir DIR             Bundle output directory
#                             (default: <repo>/dist/release-<version>)
#   --work-dir DIR            Scratch dir for the git-archive tree
#                             (default: a fresh mktemp -d under /tmp)
#   --keep-work                Do not delete the scratch dir on exit
#   --no-cache                  Pass --no-cache to the container build (a true
#                             from-scratch build; needed to PROVE
#                             reproducibility rather than measure cache reuse)
#   --source-date-epoch N     Override the reproducible-build clock
#                             (default: the commit time of --ref)
#   -h, --help                  Show this help and exit
#
# Exit 0 on a complete bundle; nonzero and a diagnostic on any failure.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"

GIT_REF="HEAD"
OUT_DIR=""
WORK_DIR=""
KEEP_WORK=0
NO_CACHE=0
SOURCE_DATE_EPOCH_OVERRIDE=""

usage() {
    sed -n '2,64p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --ref) GIT_REF="$2"; shift 2 ;;
        --ref=*) GIT_REF="${1#--ref=}"; shift ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --out-dir=*) OUT_DIR="${1#--out-dir=}"; shift ;;
        --work-dir) WORK_DIR="$2"; shift 2 ;;
        --work-dir=*) WORK_DIR="${1#--work-dir=}"; shift ;;
        --keep-work) KEEP_WORK=1; shift ;;
        --no-cache) NO_CACHE=1; shift ;;
        --source-date-epoch) SOURCE_DATE_EPOCH_OVERRIDE="$2"; shift 2 ;;
        --source-date-epoch=*) SOURCE_DATE_EPOCH_OVERRIDE="${1#--source-date-epoch=}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "cut-release.sh: unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

log() { echo "cut-release: $*"; }
fail() { echo "cut-release: FATAL: $*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || fail "docker not found"
docker buildx version >/dev/null 2>&1 || fail "docker buildx not found (needed for --output=local)"

# --- Resolve the commit being cut ------------------------------------------
COMMIT="$(git -C "$REPO_ROOT" rev-parse "$GIT_REF")" \
    || fail "cannot resolve git ref '$GIT_REF' in $REPO_ROOT"

if [ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]; then
    log "NOTE: working tree has uncommitted changes -- git archive only cuts" \
        "committed content at $COMMIT ($GIT_REF); uncommitted edits are NOT in this bundle."
fi

SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH_OVERRIDE:-$(git -C "$REPO_ROOT" log -1 --format=%ct "$COMMIT")}"
case "$SOURCE_DATE_EPOCH" in
    ''|*[!0-9]*) fail "SOURCE_DATE_EPOCH must be a nonnegative integer, got: $SOURCE_DATE_EPOCH" ;;
esac

# --- Scratch tree: git archive the commit, NEVER build in the working repo -
CLEANUP_WORK_DIR=0
if [ -z "$WORK_DIR" ]; then
    WORK_DIR="$(mktemp -d /tmp/ovmx-cut-release.XXXXXX)"
    CLEANUP_WORK_DIR=1
else
    mkdir -p "$WORK_DIR"
fi
SRC_DIR="$WORK_DIR/src"
DOCKER_OUT_DIR="$WORK_DIR/dockerout"
mkdir -p "$SRC_DIR"

cleanup() {
    if [ "$KEEP_WORK" -eq 0 ] && [ "$CLEANUP_WORK_DIR" -eq 1 ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

log "cutting $GIT_REF ($COMMIT) into clean tree: $SRC_DIR"
git -C "$REPO_ROOT" archive --format=tar "$COMMIT" | tar -x -C "$SRC_DIR"

# --- Version: single-sourced from the ARCHIVED ovmx_identity.h, not a literal
# in this script (vms-d73 constraint). Reads the exact commit being cut, not
# the possibly-ahead working tree.
IDENTITY_HEADER="$SRC_DIR/src/libvms/include/ovmx_identity.h"
[ -f "$IDENTITY_HEADER" ] || fail "identity header missing from the cut tree: $IDENTITY_HEADER"

PRODUCT_NAME="$(sed -n 's/^#define[[:space:]]\+OVMX_PRODUCT_NAME[[:space:]]\+"\([^"]*\)".*/\1/p' "$IDENTITY_HEADER" | head -1)"
PRODUCT_VERSION="$(sed -n 's/^#define[[:space:]]\+OVMX_PRODUCT_VERSION[[:space:]]\+"\([^"]*\)".*/\1/p' "$IDENTITY_HEADER" | head -1)"
[ -n "$PRODUCT_NAME" ] || fail "could not read OVMX_PRODUCT_NAME from $IDENTITY_HEADER"
[ -n "$PRODUCT_VERSION" ] || fail "could not read OVMX_PRODUCT_VERSION from $IDENTITY_HEADER"
log "product: $PRODUCT_NAME $PRODUCT_VERSION (single-sourced from ovmx_identity.h)"

OUT_DIR="${OUT_DIR:-$REPO_ROOT/dist/release-$PRODUCT_VERSION}"
mkdir -p "$OUT_DIR"

# --- VAX (netbsd-vax) release artifacts -- CO-RELEASE GATE (vms-ca5) -------
# VAX is a first-class co-release platform (docs/design-vax-mainstream-
# release.md): a release is NOT cuttable unless the VAX build gate passes.
# tools/cut-release-vax.sh reuses the R1 (vms-9172) per-PR VAX cross-compile
# recipe (the same tools/cross-vax/build-*.sh scripts ci.yml's netbsd-vax-*
# gates already call) to build the real elf32-vax artifact set into
# OUT_DIR/vax/. For any tree that IS VAX-capable this call is UNCONDITIONAL
# -- no --skip-vax flag exists -- so a broken VAX build fails this script,
# which fails the cut, for every caller (cut-release-reproducible,
# release-acceptance-e2e, release.yml's tag-triggered publish,
# `make release`, and a conductor cutting by hand) with no separate wiring
# to keep in sync. R2 scope only: build + artifact production, no SIMH boot
# (that is R3 vms-065, gated on the boot-to-DCL capstone vms-d59, still in
# flight).
#
# HISTORICAL-BASELINE EXCEPTION: this script also cuts refs that PREDATE VAX
# co-release support entirely (tests/qemu/run_upgrade_e2e.sh pins a BASELINE
# ref from before the version bump, as an upgrade-test fixture -- never HEAD)
# -- an archived tree that old has no tools/cross-vax/Dockerfile at all, so
# demanding a VAX build of it isn't "the VAX build is broken", it is asking a
# capability of a tree from before the capability existed. The co-release
# invariant applies to cutting the CURRENT release, not to re-cutting a
# pre-VAX commit as a fixture. So: skip LOUDLY and honestly (never silently)
# when the archived tree has no VAX tooling; the gate stays airtight for
# every tree that DOES have it (in particular every current/HEAD cut).
#
# Deliberately runs BEFORE the ~25-30-min x86_64 container build below, not
# after: a broken VAX build should fail the cut FAST (the same "cheap
# front-line gate before the expensive stuff" reasoning R1's header-
# portability gate already uses), not only after paying for a full OS build.
VAX_ARTIFACT_ORDER=()
VAX_SHA_NAMES=()
VAX_SKIP_NOTE=""
if [ ! -f "$SRC_DIR/tools/cross-vax/Dockerfile" ]; then
    log "%CUT-VAX-I-PREVAX, ref $GIT_REF ($COMMIT) predates VAX co-release support (no tools/cross-vax in the archived tree) -- VAX artifacts omitted for this historical cut"
    VAX_SKIP_NOTE="ref $GIT_REF ($COMMIT) predates VAX co-release support (rd vms-ca5) -- no tools/cross-vax/Dockerfile in the archived tree, so no VAX build was attempted or required for this historical cut."
else
    VAX_OUT_DIR="$OUT_DIR/vax"
    mkdir -p "$VAX_OUT_DIR"
    CUT_RELEASE_VAX_ARGS=(--src-dir "$SRC_DIR" --out-dir "$VAX_OUT_DIR")
    [ "$NO_CACHE" -eq 1 ] && CUT_RELEASE_VAX_ARGS+=(--no-cache)
    log "building VAX (netbsd-vax/elf32-vax) release artifacts -- co-release gate, no skip"
    "$SCRIPT_DIR/cut-release-vax.sh" "${CUT_RELEASE_VAX_ARGS[@]}" \
        || fail "VAX release-artifact build FAILED -- co-release invariant (docs/design-vax-mainstream-release.md): a release is not cut unless the VAX build gate passes"

    # VAX_ARTIFACT_ORDER mirrors what tools/cut-release-vax.sh's own header
    # documents it produces; re-verified here against the SHIPPED bundle dir
    # (ground-source check), not just the sub-script's own exit code.
    VAX_ARTIFACT_ORDER=(vms.kmod.o vmsfs.kmod vmsfs_mount STARTUP.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE LOGINOUT.EXE LIBRARIAN.EXE)
    for name in "${VAX_ARTIFACT_ORDER[@]}"; do
        [ -f "$VAX_OUT_DIR/$name" ] || fail "VAX artifact missing from the cut bundle after cut-release-vax.sh claimed success: vax/$name"
    done
    for name in "${VAX_ARTIFACT_ORDER[@]}"; do
        VAX_SHA_NAMES+=("vax/$name")
    done
    log "VAX artifact set present in the cut bundle: ${VAX_ARTIFACT_ORDER[*]}"
fi

# --- The containerized build (unchanged pipeline; this script drives it) ---
docker buildx inspect >/dev/null 2>&1 || \
    docker buildx create --name ovmx-cut-release --use >/dev/null

BUILDX_ARGS=(
    build
    -f "$SRC_DIR/distro/Dockerfile.bootable"
    --build-arg "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"
    --output "type=local,dest=$DOCKER_OUT_DIR"
)
[ "$NO_CACHE" -eq 1 ] && BUILDX_ARGS+=(--no-cache)

# Kernel module signing key (vms-secret-signing-key). Passed as a BuildKit
# secret, NEVER committed. If OVMX_MODULE_SIGNING_KEY points at a PEM (private
# key + cert), feed it so this cut's module signatures/keyring are deterministic;
# two cuts given the SAME key are byte-identical (that is how the reproducibility
# gate stays green -- it mints one ephemeral key and passes it to both cuts).
# Unset -> the kernel build generates an ephemeral key and signatures vary.
if [ -n "${OVMX_MODULE_SIGNING_KEY:-}" ]; then
    [ -s "$OVMX_MODULE_SIGNING_KEY" ] || fail "OVMX_MODULE_SIGNING_KEY=$OVMX_MODULE_SIGNING_KEY is not a non-empty file"
    BUILDX_ARGS+=(--secret "id=ovmx_module_signing_key,src=$OVMX_MODULE_SIGNING_KEY")
    log "module signing: using key from OVMX_MODULE_SIGNING_KEY (deterministic signatures)"
else
    log "module signing: no OVMX_MODULE_SIGNING_KEY set -- kernel will use an ephemeral key (signatures not byte-reproducible)"
fi
BUILDX_ARGS+=("$SRC_DIR")

log "building (SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH, no-cache=$NO_CACHE) -- this runs the full" \
    "kernel + VMS-native toolchain + QEMU build; expect ~25-30 minutes cold"
docker buildx "${BUILDX_ARGS[@]}"

# --- Collect the release artifact set --------------------------------------
# Exactly the four vms-d73 OUTCOME artifacts. Symlinks (docker local export
# can preserve them, e.g. /boot/vmlinuz -> vmlinuz-<kver>) are dereferenced
# with cp -L so the shipped bundle is self-contained real files, not a
# dangling reference into a container that no longer exists.
declare -A ARTIFACT_SRC=(
    [vmlinuz]="$DOCKER_OUT_DIR/boot/vmlinuz"
    [initramfs-ovmx-slim.cpio.gz]="$DOCKER_OUT_DIR/boot/initramfs-ovmx-slim.cpio.gz"
    [ovmx-distrib.img]="$DOCKER_OUT_DIR/boot/ovmx-distrib.img"
    [ovmx-os.kit]="$DOCKER_OUT_DIR/boot/ovmx-os.kit"
)
ARTIFACT_ORDER=(vmlinuz initramfs-ovmx-slim.cpio.gz ovmx-distrib.img ovmx-os.kit)

for name in "${ARTIFACT_ORDER[@]}"; do
    src="${ARTIFACT_SRC[$name]}"
    [ -e "$src" ] || fail "expected build artifact missing: $src"
    cp -L "$src" "$OUT_DIR/$name"
done

# The OS kit's own internal manifest (tools/ovmx_kit_pack.c list output),
# captured at build time from the exact ovmx-os.kit this bundle ships
# (distro/Dockerfile.bootable's kit-packing stage writes it) -- carried
# alongside so the release manifest can name the kit's real contents without
# needing ovmx_kit_pack on this host.
KIT_LISTING_SRC="$DOCKER_OUT_DIR/boot/ovmx-os.kit.manifest.txt"
[ -f "$KIT_LISTING_SRC" ] || fail "OS kit internal manifest missing from build output: $KIT_LISTING_SRC"
cp "$KIT_LISTING_SRC" "$OUT_DIR/ovmx-os.kit.manifest.txt"

# Ground-source check (vms-d73 DONE criterion), re-verified against the
# SHIPPED bundle file, not just the build's internal gate: the OS kit must
# actually name the images the login chain depends on.
for name in "DCL.EXE" "LOGINOUT.EXE" "STARTUP.COM"; do
    grep -qF "$name" "$OUT_DIR/ovmx-os.kit.manifest.txt" || \
        fail "ovmx-os.kit.manifest.txt (shipped kit's own listing) does not name $name"
done
log "OS kit manifest names DCL.EXE, LOGINOUT.EXE, STARTUP.COM (ground-source check on the shipped kit)"

# --- Release notes: GENERATED from merged history, never hand-maintained ---
# Run against REPO_ROOT (full git history), not $SRC_DIR (a bare git-archive
# tree with no .git) -- scoped to exactly $COMMIT so the notes never include
# anything after the commit actually being cut.
RELEASE_NOTES_FILE="RELEASE-NOTES-$PRODUCT_VERSION.md"
python3 "$SCRIPT_DIR/gen_release_notes.py" \
    --repo-root "$REPO_ROOT" --ref "$COMMIT" \
    --out "$OUT_DIR/$RELEASE_NOTES_FILE" \
    || fail "gen_release_notes.py failed"

# --- Checksums ---------------------------------------------------------------
# VAX artifacts (vax/*) are included here too, so publish-release.sh's
# generic "upload every file SHA256SUMS names" loop ships them automatically,
# and its own `sha256sum -c SHA256SUMS` re-verification covers them for free.
(
    cd "$OUT_DIR"
    sha256sum "${ARTIFACT_ORDER[@]}" ovmx-os.kit.manifest.txt "${VAX_SHA_NAMES[@]}" "$RELEASE_NOTES_FILE" > SHA256SUMS
)
log "wrote $OUT_DIR/SHA256SUMS (including ${#VAX_SHA_NAMES[@]} VAX artifacts)"

# --- Machine-readable release manifest --------------------------------------
# OVMX-defined format (Project Rule 8): this is NOT a VSI/PCSI artifact and is
# not presented as one -- OpenVMS/PCSI have no equivalent machine-readable
# release manifest this mirrors. It is OVMX release-engineering tooling
# output, JSON by convenience, labeled as ours right here.
MANIFEST="$OUT_DIR/release-manifest.json"
CUT_AT_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

sha256_of() { sha256sum "$OUT_DIR/$1" | awk '{print $1}'; }
bytes_of() { stat -c%s "$OUT_DIR/$1"; }

{
    printf '{\n'
    printf '  "_format": "ovmx-release-manifest",\n'
    printf '  "_format_note": "OVMX-defined release manifest (Project Rule 8) -- not a VSI/PCSI artifact; no VMS-authentic equivalent is claimed.",\n'
    printf '  "format_version": 1,\n'
    printf '  "product_name": "%s",\n' "$PRODUCT_NAME"
    printf '  "product_version": "%s",\n' "$PRODUCT_VERSION"
    printf '  "git_ref": "%s",\n' "$GIT_REF"
    printf '  "git_commit": "%s",\n' "$COMMIT"
    printf '  "source_date_epoch": %s,\n' "$SOURCE_DATE_EPOCH"
    printf '  "cut_at_utc": "%s",\n' "$CUT_AT_UTC"
    printf '  "artifacts": [\n'
    first=1
    for name in "${ARTIFACT_ORDER[@]}"; do
        [ "$first" -eq 1 ] && first=0 || printf ',\n'
        printf '    {"component": "%s", "product_version": "%s", "sha256": "%s", "bytes": %s}' \
            "$name" "$PRODUCT_VERSION" "$(sha256_of "$name")" "$(bytes_of "$name")"
    done
    printf '\n  ],\n'
    printf '  "vax_release_parity": {\n'
    printf '    "_note": "VAX (netbsd-vax/elf32-vax) co-release artifacts (rd vms-ca5, epic vms-f10, R2: docs/design-vax-mainstream-release.md). BUILD-ONLY (Rule 9 tooling, never a runtime claim): vms.kmod.o is a RELOCATABLE object, not yet a loadable .kmod (loading needs the custom MODULAR NetBSD/vax kernel, R3/R4 territory, tools/cross-vax/build-vax-modular-kernel.sh); vmsfs.kmod IS a genuine loadable module. All files under vax/.",\n'
    printf '    "skipped": %s,\n' "$([ -n "$VAX_SKIP_NOTE" ] && echo true || echo false)"
    if [ -n "$VAX_SKIP_NOTE" ]; then
        printf '    "skipped_reason": "%s",\n' "$VAX_SKIP_NOTE"
    fi
    printf '    "artifacts": [\n'
    vax_first=1
    for name in "${VAX_ARTIFACT_ORDER[@]}"; do
        [ "$vax_first" -eq 1 ] && vax_first=0 || printf ',\n'
        printf '      {"component": "vax/%s", "product_version": "%s", "sha256": "%s", "bytes": %s}' \
            "$name" "$PRODUCT_VERSION" "$(sha256_of "vax/$name")" "$(bytes_of "vax/$name")"
    done
    printf '\n    ]\n'
    printf '  },\n'
    printf '  "ovmx_os_kit_internal_manifest": {\n'
    printf '    "file": "ovmx-os.kit.manifest.txt",\n'
    printf '    "sha256": "%s",\n' "$(sha256_of "ovmx-os.kit.manifest.txt")"
    printf '    "names_verified_present": ["DCL.EXE", "LOGINOUT.EXE", "STARTUP.COM"]\n'
    printf '  },\n'
    printf '  "release_notes": {\n'
    printf '    "file": "%s",\n' "$RELEASE_NOTES_FILE"
    printf '    "sha256": "%s",\n' "$(sha256_of "$RELEASE_NOTES_FILE")"
    printf '    "generated_by": "tools/gen_release_notes.py"\n'
    printf '  }\n'
    printf '}\n'
} > "$MANIFEST"
log "wrote $MANIFEST"

log "release bundle complete: $OUT_DIR"
ls -la "$OUT_DIR"
