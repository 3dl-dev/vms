#!/usr/bin/env bash
#
# test_ovmx_kit_pack_roundtrip.sh - Prove the OVMX kit packer round-trips.
#
# tools/ovmx_kit_pack.c packs a staged SYSEXE/SYSLIB/SYSMGR/SYSHLP tree into
# an OVMX kit file (vms-0b6, docs/design-ovmx-kit-format.md). A packer that
# writes a kit nothing can read back is not done, so this test packs a tree
# and extracts every payload file BYTE-EXACT against the staged source (the
# anti-LARP proof: the kit carries the bytes, not a manifest of promises).
#
# Comparison is FILE-BY-FILE, not `diff -r` on the whole tree: the kit
# format carries files, not empty directories (there is nothing to check an
# empty directory's bytes against), so an empty staged directory with no
# payload is expected to have no counterpart on extract. Every file that
# exists must still be byte-identical.
#
# Cases:
#   1. A synthetic tree exercising: nested subdirectories, an empty
#      directory, an empty (0-byte) file, a binary file (all 256 byte
#      values), a file with no VMS type component.
#   2. The `list` manifest of case 1 names every packed filespec.
#
# Usage: test_ovmx_kit_pack_roundtrip.sh <ovmx_kit_pack-binary>
#
set -euo pipefail

PACK="${1:?usage: $0 <ovmx_kit_pack-binary>}"

if [ ! -x "$PACK" ]; then
    echo "FAIL: packer tool not executable: $PACK" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Stage a synthetic tree covering the awkward cases.
# ---------------------------------------------------------------------------
STAGE="$WORK/stage"
mkdir -p "$STAGE/SYSEXE" "$STAGE/SYSLIB" "$STAGE/SYSMGR" "$STAGE/SYSHLP" \
         "$STAGE/SYSEXE/SUBDIR"

printf 'fake DCL image\n'      > "$STAGE/SYSEXE/DCL.EXE"
printf 'fake LOGINOUT image\n' > "$STAGE/SYSEXE/LOGINOUT.EXE"
printf '$ WRITE SYS$OUTPUT "hi"\n' > "$STAGE/SYSMGR/STARTUP.COM"
printf 'nested lib\n'          > "$STAGE/SYSEXE/SUBDIR/HELPER.EXE"
printf 'README, no extension\n' > "$STAGE/SYSLIB/README"
: > "$STAGE/SYSLIB/EMPTY.EXE"                    # 0-byte file

# Binary file: every byte value 0..255.
: > "$STAGE/SYSEXE/BINARY.DAT"
for i in $(seq 0 255); do
    printf "\\x$(printf '%02x' "$i")" >> "$STAGE/SYSEXE/BINARY.DAT"
done

# SYSHLP is deliberately left empty (no help files staged) — the kit format
# carries files, not empty directories; extract should simply produce none.

KIT="$WORK/os.kit"
OUT="$WORK/out"

"$PACK" pack "$KIT" "$STAGE" "X86VMS VMS" >/dev/null \
    || fail "pack exited non-zero"

# ---------------------------------------------------------------------------
# `list` manifest sanity: every staged file must be named.
# ---------------------------------------------------------------------------
MANIFEST="$WORK/manifest.txt"
"$PACK" list "$KIT" > "$MANIFEST" || fail "list exited non-zero"

for name in DCL.EXE LOGINOUT.EXE HELPER.EXE README EMPTY.EXE BINARY.DAT; do
    grep -q "$name" "$MANIFEST" || fail "manifest missing $name"
done
grep -q "STARTUP.COM" "$MANIFEST" || fail "manifest missing STARTUP.COM"
grep -q "^Product:" "$MANIFEST" || fail "manifest missing product identification"
echo "PASS: manifest names every staged file"

# ---------------------------------------------------------------------------
# Extract and byte-compare EVERY payload file against the staged tree.
# ---------------------------------------------------------------------------
"$PACK" extract "$KIT" "$OUT" >/dev/null || fail "extract exited non-zero"

COUNT=0
while IFS= read -r -d '' f; do
    rel="${f#"$STAGE"/}"
    got="$OUT/$rel"
    [ -f "$got" ] || fail "extracted tree missing $rel"
    cmp -s "$f" "$got" || fail "$rel did not round-trip byte-exact"
    COUNT=$((COUNT + 1))
done < <(find "$STAGE" -type f -print0)

[ "$COUNT" -ge 6 ] || fail "expected >=6 payload files compared, got $COUNT"
echo "PASS: all $COUNT payload files round-tripped byte-exact"

# No extra files should appear on extract beyond what was staged.
EXTRA=$(find "$OUT" -type f | wc -l)
[ "$EXTRA" -eq "$COUNT" ] || fail "extracted $EXTRA files, expected exactly $COUNT"
echo "PASS: extracted file count matches staged file count ($COUNT)"

echo "ALL PASS: ovmx_kit_pack round-trip"
exit 0
