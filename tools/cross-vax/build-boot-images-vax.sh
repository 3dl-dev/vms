#!/bin/sh
# build-boot-images-vax.sh - cross-build + link + ACTIVATION-ASSERT the FULL
# SET of OVMX boot images for netbsd-vax (rd vms-5d1, epic vms-8e8, P4-C
# boot; docs/design-p4-netbsd-vax-boot.md §4.5).
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here
# touches the host.
#
# THE BOOT IMAGE SET. Reading src/ovmx_init/ovmx_init.c and the images it
# names, the boot path PID 1 actually walks is:
#
#   STARTUP.EXE (ovmx_init, PID 1)
#       -> execs PROVISION.EXE (ovmx_provision, vms-9b7)
#            -> execs DCL.EXE in-process on SYS$MANAGER:STARTUP.COM
#                 -> STARTUP.COM's END phase RUN/DETACHED-creates
#                    JOB_CONTROL.EXE (ovmx_job_control, vms-8d2)
#                      -> forks LOGINOUT.EXE (tools/vms_login.c) on the
#                         console for every session
#                           -> LOGINOUT execs DCL.EXE (interactive, --login)
#
# All five are REQUIRED links in that chain, not optional images -- a missing
# one halts the boot exactly the way a missing SYS$SYSTEM:DCL.EXE halts it
# today (ovmx_init.c's require_installed_system()). This script builds and
# link-proves all five for netbsd-vax:
#
#   STARTUP.EXE      (src/ovmx_init,        build-ovmx-init-vax.sh)
#   PROVISION.EXE    (src/ovmx_provision,   build-provision-vax.sh)
#   DCL.EXE          (src/vmsdcl,           build-vmsdcl-vax.sh)
#   JOB_CONTROL.EXE  (src/ovmx_job_control, build-job-control-vax.sh)
#   LOGINOUT.EXE     (tools/vms_login.c,    build-loginout-vax.sh)
#
# ACTIVATION (vms-42d, Decision A): none of these images build/run under an
# OVMX-native VAX toolchain (there is none, §4.3) -- on netbsd-vax they are
# ORDINARY NetBSD ELF32-vax dynamic executables activated by NetBSD's own
# `/usr/libexec/ld.elf_so`, a LABELLED Rule-8 substrate divergence from the
# Linux/self-hosting path's IMGACT.EXE + `.vms$sv` symbol-vector activation.
# "Link + activatable" therefore means, for each image: a well-formed
# ELF32-vax dynamic executable, PT_INTERP == /usr/libexec/ld.elf_so, and a
# resolvable NEEDED set (libc/libpthread/libm/libatomic, the same set every
# other netbsd-vax OVMX image links) -- exactly what build-activation-vax.sh
# already asserts for a representative image, applied here to the five REAL
# boot images. No qemu-system-vax exists (§6), so this is a static ELF
# proof, never an execution -- the SIMH-booted follow-up is the P4 capstone
# (vms-d59).
#
# INV-DRIFT: no image's C source forks logic on __NetBSD__/__linux__.
# ovmx_init.c is asserted #ifdef-free by build-ovmx-init-vax.sh already; the
# ONE other substrate difference in this image set (PROVISION.EXE's
# fatal-halt power-off, whose NetBSD reboot(2) has a different signature/flag
# set than Linux's) reuses the SAME ovmx_boot.h backend-object seam
# (vms-28f) ovmx_init already uses, selected by CMake
# (src/ovmx_provision/CMakeLists.txt), not a source-level #ifdef.
#
# Exit 0 = all five images build, link, and satisfy the Decision-A activation
# contract for vax--netbsdelf. Any failure is fatal (set -e).

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
READELF="${TARGET}-readelf"
SRC="$(pwd)"
CROSS_VAX="$SRC/tools/cross-vax"
OUT_ROOT="${OUT_ROOT:-/tmp/vax-boot-images-build}"
mkdir -p "$OUT_ROOT"

echo "############################################################"
echo "# boot image 1/5: STARTUP.EXE (ovmx_init)"
echo "############################################################"
OUT="$OUT_ROOT/ovmx-init" "$CROSS_VAX/build-ovmx-init-vax.sh"

echo
echo "############################################################"
echo "# boot image 2/5: PROVISION.EXE (ovmx_provision)"
echo "############################################################"
OUT="$OUT_ROOT/provision" "$CROSS_VAX/build-provision-vax.sh"

echo
echo "############################################################"
echo "# boot image 3/5: DCL.EXE (vmsdcl)"
echo "############################################################"
OUT="$OUT_ROOT/vmsdcl" "$CROSS_VAX/build-vmsdcl-vax.sh"

echo
echo "############################################################"
echo "# boot image 4/5: JOB_CONTROL.EXE (ovmx_job_control)"
echo "############################################################"
OUT="$OUT_ROOT/job-control" "$CROSS_VAX/build-job-control-vax.sh"

echo
echo "############################################################"
echo "# boot image 5/5: LOGINOUT.EXE (tools/vms_login.c)"
echo "############################################################"
OUT="$OUT_ROOT/loginout" "$CROSS_VAX/build-loginout-vax.sh"

echo
echo "############################################################"
echo "# activation contract (Decision A, vms-42d): assert all five"
echo "############################################################"

# name:path pairs -- POSIX sh has no associative arrays, so a flat list.
IMAGES="STARTUP.EXE:$OUT_ROOT/ovmx-init/STARTUP.EXE \
PROVISION.EXE:$OUT_ROOT/provision/PROVISION.EXE \
DCL.EXE:$OUT_ROOT/vmsdcl/DCL.EXE \
JOB_CONTROL.EXE:$OUT_ROOT/job-control/JOB_CONTROL.EXE \
LOGINOUT.EXE:$OUT_ROOT/loginout/LOGINOUT.EXE"

for pair in $IMAGES; do
    name="${pair%%:*}"
    path="${pair#*:}"
    echo "--- $name ($path) ---"
    test -f "$path" || { echo "FAIL: $name was not produced at $path"; exit 1; }

    # file(1): the headline sentence every reader of this gate can check by eye.
    FILE_OUT="$(file "$path")"
    echo "$FILE_OUT"
    echo "$FILE_OUT" | grep -qF 'ELF 32-bit LSB executable' \
        || { echo "FAIL: $name is not an ELF 32-bit LSB executable"; exit 1; }
    echo "$FILE_OUT" | grep -qiF 'Digital VAX' \
        || { echo "FAIL: $name is not Digital VAX"; exit 1; }
    echo "$FILE_OUT" | grep -qiF 'dynamically linked' \
        || { echo "FAIL: $name is not dynamically linked"; exit 1; }
    echo "$FILE_OUT" | grep -qF '/usr/libexec/ld.elf_so' \
        || { echo "FAIL: $name does not name interpreter /usr/libexec/ld.elf_so"; exit 1; }

    # readelf -h: class/machine, independent of file(1)'s wording.
    HDR="$("$READELF" -h "$path")"
    echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' \
        || { echo "FAIL: $name is not ELFCLASS32"; exit 1; }
    echo "$HDR" | grep -qiF 'Digital VAX' \
        || { echo "FAIL: $name Machine field is not Digital VAX"; exit 1; }

    # readelf -l: PT_INTERP requests exactly NetBSD's runtime linker.
    PHDRS="$("$READELF" -l "$path")"
    echo "$PHDRS" | grep -qiE 'INTERP' \
        || { echo "FAIL: $name has no PT_INTERP -- not dynamically activated"; exit 1; }
    INTERP="$("$READELF" -p .interp "$path" 2>/dev/null | grep -oE '/[^ ]*ld\.elf_so' | head -1 || true)"
    [ "$INTERP" = "/usr/libexec/ld.elf_so" ] \
        || { echo "FAIL: $name interpreter is not /usr/libexec/ld.elf_so (got '${INTERP:-none}')"; exit 1; }
    echo "$PHDRS" | grep -qiF 'IMGACT.EXE' \
        && { echo "FAIL: $name requests IMGACT.EXE -- OVMX-native activation must NOT be used on netbsd-vax"; exit 1; }

    # readelf -d: NEEDED shared libs are the expected NetBSD/OVMX set --
    # libc (implicit via ld.elf_so, not always a DT_NEEDED entry itself) plus
    # whatever this image's link line pulled in (pthread, m, atomic).
    DYN="$("$READELF" -d "$path" 2>/dev/null || true)"
    echo "$DYN" | grep -qiE 'NEEDED' \
        || { echo "FAIL: $name has no DT_NEEDED -- not a genuinely dynamic exe"; exit 1; }
    NEEDED="$(echo "$DYN" | grep -oE '\[lib[a-z]+\.so[^]]*\]' | tr -d '[]')"
    echo "  NEEDED: $(echo "$NEEDED" | tr '\n' ' ')"
    echo "$NEEDED" | grep -qE '^libc\.so' \
        || { echo "FAIL: $name does not NEED libc.so"; exit 1; }
    # every NEEDED entry must be an ordinary NetBSD libc-family .so -- no
    # unexpected/OVMX-native shared object leaked into a netbsd-vax link.
    BAD="$(echo "$NEEDED" | grep -vE '^lib(c|pthread|m|atomic)\.so' || true)"
    [ -z "$BAD" ] \
        || { echo "FAIL: $name NEEDs unexpected shared object(s): $BAD"; exit 1; }

    # no OVMX symbol-vector sections -- not an IMGACT image on this substrate.
    SECS="$("$READELF" -S "$path")"
    if echo "$SECS" | grep -qE '\.vms\$(sv|imp)'; then
        echo "FAIL: $name carries OVMX symbol-vector sections (.vms\$sv/.vms\$imp)"
        exit 1
    fi

    echo "  -> $name: ELF32 Digital VAX, dynamically linked, interp=/usr/libexec/ld.elf_so, NEEDED={$(echo "$NEEDED" | tr '\n' ' ')}, no .vms\$sv/.vms\$imp"
    echo
done

echo "=== ALL PROOFS PASSED: the full OVMX boot image set (STARTUP.EXE, PROVISION.EXE, DCL.EXE, JOB_CONTROL.EXE, LOGINOUT.EXE) builds, links, and activates via ld.elf_so for $TARGET ==="
