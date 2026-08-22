#!/bin/sh
# build-ovmx-images-vax-cmake.sh - Rung B of the unified cross-platform build
# (rd vms-64a, epic vms-509, docs/design-unified-cross-build.md §3-B/§8).
# Proves the TOP-LEVEL CMake project's `ovmx-images` aggregate target --
# the single authoritative "what ships" list -- configures and builds the
# FULL shipped userspace image set (not just STARTUP.EXE, rung A's scope)
# under tools/cross-vax/toolchain-vax-netbsd.cmake, and that every one of
# rd vms-e1d's 10 x86_64<->vax parity-drift images (HELP/AUTHORIZE/MAIL/
# MONITOR/INITIALIZE/INSTALL/SYSGEN/PRODUCT/PARTS/SCSD) is now a real
# elf32-vax dynamic executable produced by `cmake --build --target
# ovmx-images`, closing the drift to zero.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), same
# as every other tools/cross-vax/*.sh -- nothing installed on the build host
# (Rule 9). Originally built ALONGSIDE every per-image tools/cross-vax/
# build-*.sh CI job; rd vms-08cb (epic vms-509 Rung E,
# docs/design-unified-cross-build.md §5/§6/§8) collapsed those redundant
# per-lib/per-image gates into this one, migrating every teeth/assertion they
# carried that this aggregate did not already prove (see "MIGRATED TEETH"
# below) rather than dropping them (CLAUDE.md Rule 7/9, INV-6).
#
# MIGRATED TEETH (vms-08cb, Rung E) -- each ported from a retired per-job
# script, run AFTER the main build below so the ovmx-images artifacts already
# exist to check:
#   * libvmssys_ilp32_negctl_vax / librarian_negctl_vax (ctest build-negatives
#     registered in tests/netbsd/guest/CMakeLists.txt) -- was
#     build-libvmssys-vax.sh's / build-librarian-vax.sh's CROSSCOMPILE_NEGCTL=1
#     branch.
#   * ovmx_init.c INV-DRIFT (no substrate #ifdef fork) -- was
#     build-ovmx-init-vax.sh proof 1.
#   * the NetBSD boot backend defines every ovmx_boot.h op, and
#     ovmx_boot_open_executive() opens the real /dev/vms -- was
#     build-ovmx-init-vax.sh proof 2 (Rule 9 / INV-6).
#   * DCL.EXE carries no readline symbols (the sysroot has none; DCL's own
#     line editor is used) -- was build-vmsdcl-vax.sh's readline check.
#   * the Decision-A activation contract genuinely rejects a non-ld.elf_so
#     image -- was build-activation-vax.sh's CROSSCOMPILE_NEGCTL=1 branch.
#
# SCSD.EXE (scsd_exe) was the tenth, previously-excluded drift image: it
# opened a Linux-only AF_PACKET raw socket with no NetBSD equivalent
# (NetBSD's raw-link facility is bpf(4), a materially different API). rd
# vms-838 closed that gap with a thin raw-L2 datalink abstraction
# (src/vmsscs/scs_datalink.h/.c) -- an unchanged Linux AF_PACKET backend and
# a new NetBSD bpf(4) backend behind one header -- so scsd.c no longer opens
# a socket directly and SCSD.EXE now builds and ships in `ovmx-images` on
# every substrate, same as the other nine.
#
# LINK.EXE (vmslink) is also excluded on NetBSD -- it is the OVMX-native
# ELF64 linker that produces x86_64/aarch64 Mode-2 shareable images, a role
# with no meaning on vax (Decision-A images are ordinary NetBSD ld.elf_so
# dynamic executables, no LINK.EXE/IMGACT.EXE involved) -- the same
# "no VAX role by design" status IMGACT.EXE already carries in
# tools/parity/image-parity-allowlist.json. It was never in
# tools/cut-release.sh's VAX_ARTIFACT_ORDER either.
#
# Asserts the Decision-A activation contract (rd vms-42d) against every
# produced image:
#   * ELF32, Digital VAX, dynamically linked
#   * PT_INTERP == /usr/libexec/ld.elf_so (NOT an OVMX IMGACT.EXE path)
#   * DT_NEEDED subset of {libc, libpthread, libm, libatomic}
#
# Exit 0 = all proofs pass. Any failure is fatal (set -e).

set -eu

SRC="$(pwd)"
TARGET="${TARGET:-vax--netbsdelf}"
SYSROOT="${SYSROOT:-/opt/cross/sysroot}"
CC="${TARGET}-gcc"
BUILD_DIR="${BUILD_DIR:-/tmp/build-vax-images-cmake}"
TOOLCHAIN_FILE="$SRC/tools/cross-vax/toolchain-vax-netbsd.cmake"
test -f "$TOOLCHAIN_FILE" || { echo "FAIL: toolchain file missing: $TOOLCHAIN_FILE"; exit 1; }

rm -rf "$BUILD_DIR"

echo "=== configure: top-level project under the vax toolchain ==="
cmake -S "$SRC" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
echo

echo "=== build: cmake --build --target ovmx-images ==="
cmake --build "$BUILD_DIR" --target ovmx-images -- -j"$(nproc)"
echo

# The full shipped-image set ovmx-images builds on this substrate (LINK.EXE
# is deliberately excluded on NetBSD -- see header; it has no vax role by
# design). Boot set + LIBRARIAN.EXE are rung A/C's existing scope, carried
# here too so one job proves the whole aggregate; the ten names marked
# DRIFT are rd vms-e1d's full parity-drift image set, now all closed.
IMAGES="STARTUP.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE LOGINOUT.EXE LIBRARIAN.EXE OVMXDUMP HELP.EXE AUTHORIZE.EXE MAIL.EXE MONITOR.EXE INITIALIZE.EXE INSTALL.EXE SYSGEN.EXE PRODUCT.EXE PARTS.EXE SCSD.EXE"
DRIFT_IMAGES="HELP.EXE AUTHORIZE.EXE MAIL.EXE MONITOR.EXE INITIALIZE.EXE INSTALL.EXE SYSGEN.EXE PRODUCT.EXE PARTS.EXE SCSD.EXE"

FAIL=0
for img in $IMAGES; do
    BIN="$BUILD_DIR/bin/$img"
    if [ ! -f "$BIN" ]; then
        echo "FAIL: $img was not produced at $BIN"
        FAIL=1
        continue
    fi

    HDR="$("$TARGET-readelf" -h "$BIN")"
    OK=1
    echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' || OK=0
    echo "$HDR" | grep -qiF 'Digital VAX' || OK=0
    echo "$HDR" | grep -qiE 'Type:[[:space:]]+EXEC' || OK=0

    INTERP="$("$TARGET-readelf" -p .interp "$BIN" 2>/dev/null || true)"
    echo "$INTERP" | grep -qF '/usr/libexec/ld.elf_so' || OK=0

    NEEDED="$("$TARGET-readelf" -d "$BIN" | grep -i NEEDED || true)"
    if echo "$NEEDED" | grep -viE 'libc\.so|libpthread\.so|libm\.so|libatomic\.so' | grep -q .; then
        OK=0
    fi

    if [ "$OK" -eq 1 ]; then
        echo "-> $img: ELF32 Digital VAX, dynamically linked, interp=/usr/libexec/ld.elf_so"
    else
        echo "FAIL: $img did not pass the Decision-A activation contract"
        echo "$HDR"
        echo "interp: $INTERP"
        echo "needed: $NEEDED"
        FAIL=1
    fi
done

if [ "$FAIL" -ne 0 ]; then
    echo "FAIL: one or more ovmx-images images failed the activation contract"
    exit 1
fi

echo
echo "=== drift closure (rd vms-e1d) ==="
for img in $DRIFT_IMAGES; do
    echo "DRIFT-CLOSED: $img"
done

# =============================================================================
# MIGRATED TEETH (rd vms-08cb, epic vms-509 Rung E) -- assertions the retired
# per-job scripts carried that this aggregate did not already prove. Each is
# labeled with the rd item / job it was migrated from. Any failure is fatal.
# =============================================================================

echo
echo "=== migrated teeth: ctest build-negatives (libvmssys ilp32, librarian) ==="
# enable_testing()+add_test() for these two live in
# tests/netbsd/guest/CMakeLists.txt (added unconditionally on the NetBSD
# substrate, independent of BUILD_TESTS), so the top-level configure above
# already registered them in $BUILD_DIR/tests/netbsd/guest. Was
# build-libvmssys-vax.sh / build-librarian-vax.sh's CROSSCOMPILE_NEGCTL=1.
# Selected by exact name (not the facility jobs' '_width_negctl_vax$'
# pattern): libvmssys_ilp32_negctl_vax is deliberately NOT named
# "*_width_negctl_vax" so it does not also match
# build-facility-tools-vax-cmake.sh's selector and inflate that job's
# expected count of 4.
NEGCTL_BUILD_DIR="$BUILD_DIR/tests/netbsd/guest"
test -f "$NEGCTL_BUILD_DIR/CTestTestfile.cmake" \
    || { echo "FAIL: $NEGCTL_BUILD_DIR/CTestTestfile.cmake missing -- negctl tests were not registered"; exit 1; }
if ! (cd "$NEGCTL_BUILD_DIR" && ctest -R '^(libvmssys_ilp32|librarian)_negctl_vax$' --output-on-failure); then
    echo "FAIL: the libvmssys ilp32 negctl or the librarian negctl did not fire"
    exit 1
fi

echo
echo "=== migrated teeth: ovmx_init.c INV-DRIFT (no substrate #ifdef fork) (vms-f2e) ==="
# Was build-ovmx-init-vax.sh proof 1.
if grep -nE '#[[:space:]]*if(def)?[[:space:]].*(__NetBSD__|__linux__)' \
        "$SRC/src/ovmx_init/ovmx_init.c"; then
    echo "FAIL: ovmx_init.c carries a substrate #ifdef -- the boot sequence must"
    echo "      stay ONE source; the substrate split lives ONLY in the ovmx_boot"
    echo "      backend files (ovmx_boot_linux.c / ovmx_boot_netbsd.c)."
    exit 1
fi
echo "OK: ovmx_init.c has no __NetBSD__/__linux__ boot-logic fork"

echo
echo "=== migrated teeth: NetBSD boot backend defines every ovmx_boot.h op + opens the real /dev/vms (vms-f2e, Rule 9/INV-6) ==="
# Was build-ovmx-init-vax.sh proof 2. The ovmx_init target's build-tree object
# for ovmx_boot_netbsd.c (Unix Makefiles layout:
# <build>/src/ovmx_init/CMakeFiles/ovmx_init.dir/ovmx_boot_netbsd.c.o) --
# found rather than hardcoded so a generator change does not silently skip
# this check.
NETBSD_BOOT_OBJ="$(find "$BUILD_DIR" -name 'ovmx_boot_netbsd.c.o' | head -1)"
test -n "$NETBSD_BOOT_OBJ" \
    || { echo "FAIL: ovmx_boot_netbsd.c.o not found under $BUILD_DIR -- was ovmx_init built?"; exit 1; }
for sym in ovmx_boot_kernel_filesystems_mounted ovmx_boot_mount_kernel_filesystems \
           ovmx_boot_start_console_log_bridge ovmx_boot_load_module \
           ovmx_boot_open_executive ovmx_boot_system_disk_dev \
           ovmx_boot_system_disk_present \
           ovmx_boot_power_off; do
    if ! "$TARGET-nm" "$NETBSD_BOOT_OBJ" | grep -qE " T $sym\$"; then
        echo "FAIL: ovmx_boot_netbsd.c.o does not define $sym"
        exit 1
    fi
done
echo "OK: every ovmx_boot.h op is defined by the NetBSD backend"
if ! grep -qF 'open("/dev/vms", O_RDWR | O_CLOEXEC)' "$SRC/src/ovmx_init/ovmx_boot_netbsd.c"; then
    echo "FAIL: ovmx_boot_netbsd.c does not open the real /dev/vms executive device"
    exit 1
fi
echo "OK: ovmx_boot_open_executive() opens the real /dev/vms (fail-honest)"

echo
echo "=== migrated teeth: DCL.EXE carries no readline symbols (vms-1cb2) ==="
# Was build-vmsdcl-vax.sh's readline check -- re-homed onto the FINAL linked
# DCL.EXE ovmx-images already produced (stronger than the original, which
# checked the pre-link archive: this also proves no readline symbol survived
# the real link). readline is absent from the NetBSD/vax sysroot, so
# HAVE_READLINE stays undefined and DCL's own non-readline line editor is used.
DCL_BIN="$BUILD_DIR/bin/DCL.EXE"
test -f "$DCL_BIN" || { echo "FAIL: $DCL_BIN not found -- was vmsdcl built?"; exit 1; }
if "$TARGET-nm" "$DCL_BIN" 2>/dev/null | grep -qiE ' U (readline|add_history|rl_redisplay)$'; then
    echo "FAIL: DCL.EXE references GNU readline symbols -- HAVE_READLINE leaked into the netbsd-vax build"
    exit 1
fi
echo "OK: no undefined readline symbols -- DCL's own non-readline line editor is used"

echo
echo "=== migrated teeth (negctl): the Decision-A activation contract must REJECT a non-ld.elf_so image (vms-42d) ==="
# Was build-activation-vax.sh's CROSSCOMPILE_NEGCTL=1 branch: a genuinely
# dynamic vax exe whose PT_INTERP is deliberately NOT NetBSD's runtime linker
# must be rejected by the same interp check the per-image loop above applies
# to every real image. -Wl,--dynamic-linker only writes the .interp string;
# the loader is never consulted at link time, so this always links.
NEGCTL_OUT="$BUILD_DIR/activation-negctl"
mkdir -p "$NEGCTL_OUT"
cat > "$NEGCTL_OUT/negctl.c" <<'EOF'
int main(void){ return 0; }
EOF
BAD="$NEGCTL_OUT/BAD_INTERP.EXE"
"$CC" --sysroot="$SYSROOT" -Wl,--dynamic-linker=/ovmx/negctl/not-ld.elf_so \
    "$NEGCTL_OUT/negctl.c" -o "$BAD"
NEG_INTERP="$("$TARGET-readelf" -p .interp "$BAD" 2>/dev/null | grep -oE '/[^ ]*ld\.elf_so' | head -1 || true)"
if [ "$NEG_INTERP" = "/usr/libexec/ld.elf_so" ]; then
    echo "FAIL (negctl): a bogus-interp image still matched /usr/libexec/ld.elf_so -- the activation assertion has NO teeth"
    exit 1
fi
echo "PASS (negctl): the non-ld.elf_so image is correctly rejected by the Decision-A activation contract"

echo
echo "=== ALL PROOFS PASSED: ovmx-images builds the full shipped image set via 'cmake --build --target ovmx-images' for $TARGET, every image passes the Decision-A activation contract, and every migrated teeth check genuinely fires ==="
