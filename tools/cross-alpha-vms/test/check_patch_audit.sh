#!/bin/sh
# check_patch_audit.sh -- every GCC/binutils port patch under
# tools/cross-alpha-vms/patches/ has a classified row in
# docs/selfhost/gcc-patch-audit.md (vms-839), and every row names a patch that
# exists. A row is `| \`<patch file>\` | <what> | (a) or (b) | <evidence> |`.
#
# Usage: sh check_patch_audit.sh <repo root>
#        sh check_patch_audit.sh --self-test <repo root>
set -eu

check() {
    pdir=$1/tools/cross-alpha-vms/patches
    doc=$1/docs/selfhost/gcc-patch-audit.md
    [ -f "$doc" ] || { echo "FAIL: $doc missing"; return 1; }
    rc=0
    for p in "$pdir"/*.patch; do
        [ -e "$p" ] || continue
        b=$(basename "$p")
        row=$(grep -F "| \`$b\`" "$doc" || true)
        if [ -z "$row" ]; then
            echo "FAIL: $b has no row in the audit table"; rc=1
        elif ! printf '%s\n' "$row" | grep -Eq '\| \((a|b)\) \|'; then
            echo "FAIL: $b row carries no class (a)/(b)"; rc=1
        fi
    done
    for b in $(grep -Eo '^\| `[0-9]{4}-[^`]*\.patch`' "$doc" | sed 's/^| `//; s/`$//'); do
        [ -f "$pdir/$b" ] || { echo "FAIL: audit row names missing patch $b"; rc=1; }
    done
    [ "$rc" -eq 0 ] && echo "PASS: every port patch has a classified audit row"
    return "$rc"
}

if [ "${1:-}" = "--self-test" ]; then
    root=$2
    W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
    mkdir -p "$W/tools/cross-alpha-vms" "$W/docs/selfhost"
    cp -r "$root/tools/cross-alpha-vms/patches" "$W/tools/cross-alpha-vms/"
    cp "$root/docs/selfhost/gcc-patch-audit.md" "$W/docs/selfhost/"
    check "$W" >/dev/null || { echo "FAIL: self-test baseline is not green"; exit 1; }
    # 1. a new patch with no row must go red
    : > "$W/tools/cross-alpha-vms/patches/9999-unaudited.patch"
    if check "$W" >/dev/null; then echo "FAIL: unaudited patch not caught"; exit 1; fi
    rm "$W/tools/cross-alpha-vms/patches/9999-unaudited.patch"
    # 2. a row for a deleted patch must go red
    first=$(ls "$W/tools/cross-alpha-vms/patches" | head -1)
    rm "$W/tools/cross-alpha-vms/patches/$first"
    if check "$W" >/dev/null; then echo "FAIL: stale row not caught"; exit 1; fi
    echo "PASS: self-test (unaudited patch and stale row both rejected)"
    exit 0
fi
check "${1:?usage: check_patch_audit.sh [--self-test] <repo root>}"
