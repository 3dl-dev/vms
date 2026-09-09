#!/bin/sh
# build-os-kit-vax.sh - package the OVMX/NetBSD-vax OS payload into
# OVMX-OS-VAX.KIT (rd vms-c2c, VAX installer Rung E, docs/design-vax-
# installer.md §4).
#
# NOT a new packer. The kit container format is architecture-independent
# (docs/design-ovmx-kit-format.md) and tools/ovmx_kit_pack.c is a portable
# host tool -- this script is ONLY staging glue: it assembles the vax kit
# payload directory (mirroring distro/Dockerfile.bootable's /kit-stage step,
# lines ~647-669) from a caller-provided vax cross-build images directory
# plus the existing distro/rootfs (arch-neutral) and distro/rootfs-vax
# (Decision-A) trees, then drives the SAME `ovmx_kit_pack pack / list /
# extract` sequence that step already uses.
#
# STANDALONE / not wired into any release path: this script is not called
# by tools/cut-release.sh, distro/Dockerfile.bootable, or any CI workflow.
# It is opt-in tooling for the VAX installer lane (vms-f10) to run once the
# four utility-image vax cross-build scripts (§3 gap: PRODUCT/AUTHORIZE/
# INITIALIZE/SYSGEN) exist; wiring it into a release flow is a SEPARATE,
# later decision, not this rung's scope.
#
# Payload (docs/design-vax-installer.md §4 -- "the five boot images + the
# four new utility images [...] + SYSUAF.DAT/RIGHTSLIST.DAT/OVMXVMSSYS.PAR
# [...] + STARTUP.COM + the Decision-A SYSTARTUP_VMS.COM"):
#   SYSEXE/  STARTUP.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE LOGINOUT.EXE
#            PRODUCT.EXE AUTHORIZE.EXE INITIALIZE.EXE SYSGEN.EXE
#            SYSUAF.DAT RIGHTSLIST.DAT OVMXVMSSYS.PAR   (binary, reused
#            verbatim from distro/rootfs -- arch-neutral, see
#            tests/lab-vax/stage_sysvol.sh header)
#   SYSMGR/  STARTUP.COM (reused verbatim)
#            SYSTARTUP_VMS.COM (distro/rootfs-vax Decision-A variant)
#   SYSLIB/  (OPTIONAL, rd vms-c7f7/vms-404 P3b) the genuine elf32-vax .vms$sv
#            shareable graph (LIBVMS$SHR.EXE etc, built by
#            tools/cross-vax/build-vax-shareable-graph.sh), staged when a
#            shareables dir is supplied (see --shareables-dir below).
#
# Product identity: "OVMX VAXVMS VMS" -- the same "vendor + arch-code + VMS"
# shape the Alpha oracle showed (DEC AXPVMS VMS) and the x86_64 kit already
# uses (OVMX X86VMS VMS), per docs/design-vax-installer.md line ~245
# ("OVMX-OS-VAX.KIT, distinct product identity string, e.g. OVMX VAXVMS VMS
# Vx.y mirroring the existing OVMX X86VMS VMS V0.1 shape").
#
# vms-c7f7 (vms-404 P3b): the Decision-A guard that used to REJECT any
# "INSTALL ADD SYS$SHARE" line in the staged SYSTARTUP_VMS.COM is LIFTED --
# a genuine elf32-vax `.vms$sv` shareable graph now exists (LINKVAX.EXE,
# vms-19b/P3a) and can be staged into SYSLIB (below). What this script does
# NOT yet do: the boot/utility images this kit packages are still the
# ORDINARY Decision-A dynamic NetBSD ELF32 executables (PT_INTERP =
# /usr/libexec/ld.elf_so, built by build-boot-images-vax.sh /
# build-ovmx-images-vax-cmake.sh), because LINKVAX.EXE's elf32-vax
# `--executable` emit (the mode that would produce a genuine IMGACT.EXE-
# activated consumer of this shareable) is not implemented yet -- it fails
# honestly with "%LINK-F-ERROR, elf32-vax --executable emit is vms-404
# P3b/P4, not P3a" (src/vmslink/link.c, unmodified here). So a kit built
# today carries a real shareable in SYSLIB but no consumer that imports it
# by symbol vector; PT_INTERP=IMGACT.EXE on the boot chain is a follow-on
# (see the P3b PR notes) that needs that link.c gap closed first.
#
# Usage:
#   build-os-kit-vax.sh <vax-images-dir> <repo-root> <kit-output-file> \
#                        [ovmx_kit_pack-binary] [--shareables-dir DIR]
#
#   <vax-images-dir>     must contain the nine ELF32-vax images by name:
#                         STARTUP.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE
#                         LOGINOUT.EXE PRODUCT.EXE AUTHORIZE.EXE
#                         INITIALIZE.EXE SYSGEN.EXE
#                         (today only the first five exist --
#                         tools/cross-vax/build-boot-images-vax.sh output
#                         dir; the four utility images are the §3 gap this
#                         script does not itself close)
#   <repo-root>           checkout root (for distro/rootfs, distro/rootfs-vax)
#   <kit-output-file>     where to write OVMX-OS-VAX.KIT
#   [ovmx_kit_pack-binary] defaults to <repo-root>/build/bin/ovmx_kit_pack
#                          (ovmx_kit_pack is a HOST tool -- build it with
#                          `cmake -B build -DBUILD_TOOLS=ON && cmake --build
#                          build --target ovmx_kit_pack`, no vax toolchain
#                          needed for packing itself)
#
# Fails loudly (set -eu) on any missing input component or a pack/list/
# extract round-trip that does not reproduce the staged tree byte-exact.
set -eu

IMAGES_DIR="${1:?usage: $0 <vax-images-dir> <repo-root> <kit-output-file> [ovmx_kit_pack-binary] [--shareables-dir DIR]}"
REPO="${2:?usage: $0 <vax-images-dir> <repo-root> <kit-output-file> [ovmx_kit_pack-binary] [--shareables-dir DIR]}"
KIT_OUT="${3:?usage: $0 <vax-images-dir> <repo-root> <kit-output-file> [ovmx_kit_pack-binary] [--shareables-dir DIR]}"
shift 3

# Remaining args: an optional bare PACK-binary-path token, and/or
# --shareables-dir DIR (rd vms-c7f7) -- a directory holding the genuine
# elf32-vax .vms$sv shareable graph (e.g. tools/cross-vax/
# build-vax-shareable-graph.sh's $OUT_DIR, carrying `LIBVMS$SHR.EXE`). When
# given, every file in it is staged into the kit's SYSLIB/ member set.
# Optional: the kit still packs without it (SYSLIB stays absent, exactly
# today's behavior) -- callers that have not built the shareable graph yet
# are unaffected.
PACK="$REPO/build/bin/ovmx_kit_pack"
SHAREABLES_DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --shareables-dir)
            SHAREABLES_DIR="${2:?--shareables-dir requires a DIR argument}"
            shift 2
            ;;
        *)
            PACK="$1"
            shift
            ;;
    esac
done

PRODUCT_SUFFIX="VAXVMS VMS"

BOOT_IMAGES="STARTUP.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE LOGINOUT.EXE"
UTIL_IMAGES="PRODUCT.EXE AUTHORIZE.EXE INITIALIZE.EXE SYSGEN.EXE"
DATA_FILES="SYSUAF.DAT RIGHTSLIST.DAT OVMXVMSSYS.PAR"

ROOTFS="$REPO/distro/rootfs/vms"
ROOTFS_SYSEXE="$ROOTFS/SYS0/SYSCOMMON/SYSEXE"
ROOTFS_SYSMGR="$ROOTFS/SYS0/SYSCOMMON/SYSMGR"
VAX_SYSTARTUP="$REPO/distro/rootfs-vax/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM"

die() { echo "%%BUILD-OS-KIT-VAX-F, $*" >&2; exit 1; }

echo "=== build-os-kit-vax: stage the OVMX/NetBSD-vax OS-kit payload ==="

[ -d "$IMAGES_DIR" ]    || die "images dir does not exist: $IMAGES_DIR"
[ -d "$ROOTFS_SYSEXE" ] || die "arch-neutral SYSEXE tree missing: $ROOTFS_SYSEXE"
[ -d "$ROOTFS_SYSMGR" ] || die "arch-neutral SYSMGR tree missing: $ROOTFS_SYSMGR"
[ -f "$VAX_SYSTARTUP" ] || die "vax Decision-A SYSTARTUP_VMS.COM missing: $VAX_SYSTARTUP"
[ -x "$PACK" ]          || die "ovmx_kit_pack binary not executable: $PACK (build it: cmake -B build -DBUILD_TOOLS=ON && cmake --build build --target ovmx_kit_pack)"
if [ -n "$SHAREABLES_DIR" ]; then
    [ -d "$SHAREABLES_DIR" ] || die "--shareables-dir does not exist: $SHAREABLES_DIR"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

STAGE="$WORK/stage"
mkdir -p "$STAGE/SYSEXE" "$STAGE/SYSMGR"

# --- 1. the nine SYSEXE images, all cross-built for vax --------------------
for img in $BOOT_IMAGES $UTIL_IMAGES; do
    src="$IMAGES_DIR/$img"
    [ -f "$src" ] || die "vax image missing from images dir: $src"
    cp "$src" "$STAGE/SYSEXE/$img"
    echo "OK: staged boot/utility image $img"
done

# --- 2. arch-neutral data files, reused verbatim (binary, not text) --------
for f in $DATA_FILES; do
    src="$ROOTFS_SYSEXE/$f"
    [ -f "$src" ] || die "arch-neutral data file missing: $src"
    cp "$src" "$STAGE/SYSEXE/$f"
    echo "OK: staged data file $f"
done

# --- 3. STARTUP.COM, reused verbatim ---------------------------------------
[ -f "$ROOTFS_SYSMGR/STARTUP.COM" ] || die "STARTUP.COM missing: $ROOTFS_SYSMGR/STARTUP.COM"
cp "$ROOTFS_SYSMGR/STARTUP.COM" "$STAGE/SYSMGR/STARTUP.COM"
echo "OK: staged STARTUP.COM"

# --- 4. the vax SYSTARTUP_VMS.COM -------------------------------------------
# vms-c7f7 (vms-404 P3b): the OLD guard here unconditionally DIED on any
# "INSTALL ADD SYS$SHARE" line, because vax shipped no shareable at all and
# Decision A said so explicitly. A genuine elf32-vax `.vms$sv` shareable
# graph now exists (LINKVAX.EXE, vms-19b/P3a) -- so that is no longer a
# reason to hard-fail packaging. It is still INFORMATIONAL: today's boot
# images are still ordinary Decision-A ld.elf_so dynamic executables (see
# the header comment) and cannot yet ACT on an INSTALL ADD SYS$SHARE line
# (that needs LINKVAX.EXE's elf32-vax `--executable` emit, not yet built --
# link.c is out of this script's scope). So we stage whatever
# SYSTARTUP_VMS.COM the caller/repo provides as-is, and only NOTE whether it
# carries that line -- never fail the build over it.
cp "$VAX_SYSTARTUP" "$STAGE/SYSMGR/SYSTARTUP_VMS.COM"
if grep -qiE '^\$[[:space:]]+INSTALL[[:space:]]+ADD[[:space:]]+SYS\$SHARE' "$STAGE/SYSMGR/SYSTARTUP_VMS.COM"; then
    echo "OK: staged SYSTARTUP_VMS.COM carries INSTALL ADD SYS\$SHARE (shareables enabled)"
else
    echo "OK: staged SYSTARTUP_VMS.COM (no INSTALL ADD SYS\$SHARE -- boot images do not yet consume shareables)"
fi

# --- 5. OPTIONAL: the genuine elf32-vax .vms$sv shareable graph (vms-c7f7) --
if [ -n "$SHAREABLES_DIR" ]; then
    mkdir -p "$STAGE/SYSLIB"
    N=0
    for f in "$SHAREABLES_DIR"/*; do
        [ -f "$f" ] || continue
        cp "$f" "$STAGE/SYSLIB/$(basename "$f")"
        echo "OK: staged shareable $(basename "$f") -> SYSLIB"
        N=$((N + 1))
    done
    [ "$N" -gt 0 ] || die "--shareables-dir given but contains no files: $SHAREABLES_DIR"
fi

STAGED_COUNT=$(find "$STAGE" -type f | wc -l)
echo "staged $STAGED_COUNT payload files under $STAGE"
echo

# --- pack / list / extract, via the EXISTING host packer --------------------
echo "=== pack: ovmx_kit_pack pack ==="
"$PACK" pack "$KIT_OUT" "$STAGE" "$PRODUCT_SUFFIX"
[ -f "$KIT_OUT" ] || die "kit file was not produced: $KIT_OUT"
echo

echo "=== list: manifest sanity ==="
LISTING="$("$PACK" list "$KIT_OUT")"
echo "$LISTING"
echo "$LISTING" | grep -q "^Product:.*OVMX VAXVMS VMS" \
    || die "kit manifest product identity is not 'OVMX VAXVMS VMS'"
echo "OK: kit manifest product identity is OVMX VAXVMS VMS"
for name in $BOOT_IMAGES $UTIL_IMAGES $DATA_FILES STARTUP.COM SYSTARTUP_VMS.COM; do
    echo "$LISTING" | grep -q "$name" || die "kit manifest missing expected member: $name"
done
if [ -n "$SHAREABLES_DIR" ]; then
    for f in "$SHAREABLES_DIR"/*; do
        [ -f "$f" ] || continue
        name="$(basename "$f")"
        echo "$LISTING" | grep -qF "$name" || die "kit manifest missing staged shareable: $name"
    done
    echo "OK: kit manifest names every staged member (including the shareable graph)"
else
    echo "OK: kit manifest names every staged member"
fi
echo

echo "=== extract + byte-compare round-trip ==="
VERIFY="$WORK/verify"
"$PACK" extract "$KIT_OUT" "$VERIFY"

COUNT=0
while IFS= read -r f; do
    rel=$(printf '%s\n' "$f" | sed "s#^$STAGE/##")
    got="$VERIFY/$rel"
    [ -f "$got" ] || die "extracted tree missing $rel"
    cmp -s "$f" "$got" || die "$rel did not round-trip byte-exact"
    COUNT=$((COUNT + 1))
done <<EOF
$(find "$STAGE" -type f)
EOF

[ "$COUNT" -eq "$STAGED_COUNT" ] || die "compared $COUNT files, expected $STAGED_COUNT"
echo "OK: all $COUNT payload files round-tripped byte-exact"

EXTRACTED_COUNT=$(find "$VERIFY" -type f | wc -l)
[ "$EXTRACTED_COUNT" -eq "$COUNT" ] || die "extracted $EXTRACTED_COUNT files, expected exactly $COUNT"
echo "OK: extracted file count matches staged file count ($COUNT)"

echo
echo "=== ALL PROOFS PASSED: $KIT_OUT packaged from $IMAGES_DIR, round-trips byte-exact ==="
