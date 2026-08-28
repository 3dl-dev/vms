#!/usr/bin/env bash
#
# test_ovmx_kit_pack_vax_roundtrip.sh - Prove the VAX OS-kit packaging path
# (rd vms-c2c, VAX installer Rung E) round-trips byte-exact through the
# EXISTING host packer tools/ovmx_kit_pack.c -- no vax-specific packer, no
# vax-specific format (docs/design-ovmx-kit-format.md already establishes
# the kit container as architecture-independent; docs/design-vax-installer.md
# §4 says packaging the vax payload is "same packer, same format, different
# input tree").
#
# This is the SYNTHETIC-payload half of vms-c2c's ground-source proof: it
# does NOT need the vax cross-build toolchain (tools/cross-vax/*, an
# elf32-vax gcc) to prove the packaging PATH. It stages small stub files
# under the exact names docs/design-vax-installer.md §4 lists as the vax
# kit payload -- the five boot images + the four new utility images (§3
# gap) + the arch-neutral data files + STARTUP.COM + the Decision-A
# SYSTARTUP_VMS.COM -- packs them, lists them, extracts them, and
# BYTE-COMPARES every extracted file against its synthetic source. Real
# cross-built vax ELF32 images exercise the identical pack/list/extract
# path via tools/cross-vax/build-os-kit-vax.sh (not run here -- that script
# needs the vax cross-build outputs this test deliberately avoids).
#
# Product identity (docs/design-vax-installer.md line ~245): "OVMX-OS-
# VAX.KIT, distinct product identity string, e.g. OVMX VAXVMS VMS Vx.y
# mirroring the existing OVMX X86VMS VMS V0.1 shape" -- the same "vendor +
# arch-code + VMS" shape the Alpha oracle showed (DEC AXPVMS VMS) and the
# x86_64 kit already uses (OVMX X86VMS VMS, tests/integration/
# test_ovmx_kit_pack_roundtrip.sh). ovmx_kit_pack composes "OVMX" (the
# vendor token, ovmx_identity.h OVMX_VENDOR_TOKEN) + this test's product
# suffix "VAXVMS VMS" -- never a second hardcoded vendor literal.
#
# Usage: test_ovmx_kit_pack_vax_roundtrip.sh <ovmx_kit_pack-binary>
#
set -euo pipefail

PACK="${1:?usage: $0 <ovmx_kit_pack-binary>}"
PRODUCT_SUFFIX="VAXVMS VMS"

if [ ! -x "$PACK" ]; then
    echo "FAIL: packer tool not executable: $PACK" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Stage a synthetic vax kit payload: correct names, stub bytes.
#
# docs/design-vax-installer.md §4 kit payload list:
#   - five boot images (SYSEXE) -- cross-built today (vms-c99/vms-7b1)
#   - four utility images (SYSEXE) -- PRODUCT/AUTHORIZE/INITIALIZE/SYSGEN
#     (§3 gap: no vax cross-build script for these yet; this test only
#     proves the PACKAGING path, not that these images exist for real)
#   - SYSUAF.DAT / RIGHTSLIST.DAT / OVMXVMSSYS.PAR (SYSEXE, arch-neutral)
#   - STARTUP.COM (SYSMGR, reused verbatim)
#   - SYSTARTUP_VMS.COM (SYSMGR, the Decision-A vax variant)
# ---------------------------------------------------------------------------
STAGE="$WORK/stage"
mkdir -p "$STAGE/SYSEXE" "$STAGE/SYSMGR"

BOOT_IMAGES="STARTUP.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE LOGINOUT.EXE"
UTIL_IMAGES="PRODUCT.EXE AUTHORIZE.EXE INITIALIZE.EXE SYSGEN.EXE"
DATA_FILES="SYSUAF.DAT RIGHTSLIST.DAT OVMXVMSSYS.PAR"

# Deterministic-but-distinct synthetic bytes per file, including a binary
# (non-text) blob for the two .DAT files vms-c2c's brief flags as binary
# now (not text) -- the byte-compare below must not silently pass on a
# text-only fixture.
i=0
for img in $BOOT_IMAGES; do
    printf 'synthetic vax boot image stub: %s (#%d)\n' "$img" "$i" > "$STAGE/SYSEXE/$img"
    i=$((i + 1))
done
for img in $UTIL_IMAGES; do
    printf 'synthetic vax utility image stub: %s (#%d)\n' "$img" "$i" > "$STAGE/SYSEXE/$img"
    i=$((i + 1))
done
for f in $DATA_FILES; do
    : > "$STAGE/SYSEXE/$f"
    for b in $(seq 0 255); do
        printf "\\x$(printf '%02x' "$b")" >> "$STAGE/SYSEXE/$f"
    done
    # A trailing marker so the two .DAT stubs are not byte-identical to
    # each other (would mask a filespec/path mixup in extraction).
    printf '%s' "$f" >> "$STAGE/SYSEXE/$f"
done

printf '$ ! synthetic vax STARTUP.COM\n$ EXIT\n' > "$STAGE/SYSMGR/STARTUP.COM"
printf '$ ! synthetic vax Decision-A SYSTARTUP_VMS.COM (no INSTALL ADD SYS$SHARE)\n$ EXIT\n' \
    > "$STAGE/SYSMGR/SYSTARTUP_VMS.COM"

TOTAL_FILES=$(find "$STAGE" -type f | wc -l)
[ "$TOTAL_FILES" -eq 14 ] || fail "expected 14 synthetic payload files staged, got $TOTAL_FILES"

KIT="$WORK/OVMX-OS-VAX.KIT"
OUT="$WORK/out"

# ---------------------------------------------------------------------------
# pack: the EXISTING host packer, unmodified -- vms-c2c reuses it 100%.
# ---------------------------------------------------------------------------
"$PACK" pack "$KIT" "$STAGE" "$PRODUCT_SUFFIX" >/dev/null \
    || fail "pack exited non-zero"
[ -f "$KIT" ] || fail "kit file was not produced: $KIT"
echo "PASS: OVMX-OS-VAX.KIT produced by ovmx_kit_pack pack"

# ---------------------------------------------------------------------------
# list: manifest must name every staged component and carry the vax
# product identity, mirroring the Dockerfile.bootable x86_64 gate
# (DCL.EXE/LOGINOUT.EXE/STARTUP.COM presence check).
# ---------------------------------------------------------------------------
MANIFEST="$WORK/manifest.txt"
"$PACK" list "$KIT" > "$MANIFEST" || fail "list exited non-zero"

grep -q "^Product:.*OVMX VAXVMS VMS" "$MANIFEST" \
    || fail "manifest product identity is not 'OVMX VAXVMS VMS' (got: $(grep '^Product:' "$MANIFEST"))"
echo "PASS: manifest product identity is OVMX VAXVMS VMS"

for name in $BOOT_IMAGES $UTIL_IMAGES $DATA_FILES STARTUP.COM SYSTARTUP_VMS.COM; do
    grep -q "$name" "$MANIFEST" || fail "manifest missing expected vax kit member: $name"
done
echo "PASS: manifest names all $(echo $BOOT_IMAGES $UTIL_IMAGES $DATA_FILES STARTUP.COM SYSTARTUP_VMS.COM | wc -w) expected vax kit members"

# ---------------------------------------------------------------------------
# extract + BYTE-COMPARE -- the load-bearing assertion. Every extracted
# file must be byte-identical to the synthetic source; this is the proof
# the kit carries bytes, not a manifest of promises (same discipline as
# test_ovmx_kit_pack_roundtrip.sh and Dockerfile.bootable's cmp -s loop).
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

[ "$COUNT" -eq "$TOTAL_FILES" ] || fail "compared $COUNT files, expected $TOTAL_FILES"
echo "PASS: all $COUNT vax payload files round-tripped byte-exact (pack -> extract == synthetic source)"

EXTRACTED_COUNT=$(find "$OUT" -type f | wc -l)
[ "$EXTRACTED_COUNT" -eq "$COUNT" ] || fail "extracted $EXTRACTED_COUNT files, expected exactly $COUNT (extra/missing members)"
echo "PASS: extracted file count matches staged file count ($COUNT)"

echo "ALL PASS: OVMX-OS-VAX.KIT round-trip (synthetic payload, real ovmx_kit_pack)"
exit 0
