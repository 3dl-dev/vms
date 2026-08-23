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
# ACTIVATION (vms-42d, Decision A; vms-0ab -static): none of these images
# build/run under an OVMX-native VAX toolchain (there is none, §4.3) -- on
# netbsd-vax they are ORDINARY NetBSD ELF32-vax executables activated by the
# substrate, a LABELLED Rule-8 substrate divergence from the Linux/self-hosting
# path's IMGACT.EXE + `.vms$sv` symbol-vector activation.
#
# BOOT-SPEED (vms-0ab): these five boot images are now STATICALLY linked
# (-static). The in-process IMGACT bails SS$_UNSUPPORTED on the foreign
# ld.elf_so PT_INTERP, so every boot activation is a fork+execve; when the
# images were DYNAMIC, ld.elf_so then re-relocated libc/libpthread/libm/libatomic
# from scratch on EACH of the ~4 activations a boot walks -- a large per-boot
# cost native VMS's prelinked activation never pays. A static image carries no
# PT_INTERP and no DT_NEEDED, so ld.elf_so is never invoked and that relocation
# cost is gone. "Link + activatable" therefore now means, for each image: a
# well-formed ELF32-vax STATIC executable, NO PT_INTERP, NO DT_NEEDED, and (for
# the auth-path images) the SYSUAF/RIGHTSLIST weak-seam engines force-anchored so
# nothing silently drops under -static (asserted in each image's own recipe).
# No qemu-system-vax exists (§6), so this is a static ELF proof, never an
# execution -- the SIMH-booted follow-up is the P4 capstone (vms-d59).
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
    # vms-0ab: STATIC now. The boot images no longer name ld.elf_so as their
    # interpreter, so each fork+execve activation stops paying ld.elf_so's
    # from-scratch re-relocation of libc/libpthread/libm/libatomic.
    echo "$FILE_OUT" | grep -qiF 'statically linked' \
        || { echo "FAIL: $name is not statically linked (vms-0ab boot-speed)"; exit 1; }
    echo "$FILE_OUT" | grep -qF '/usr/libexec/ld.elf_so' \
        && { echo "FAIL: $name still names interpreter /usr/libexec/ld.elf_so -- not static"; exit 1; }

    # readelf -h: class/machine, independent of file(1)'s wording.
    HDR="$("$READELF" -h "$path")"
    echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' \
        || { echo "FAIL: $name is not ELFCLASS32"; exit 1; }
    echo "$HDR" | grep -qiF 'Digital VAX' \
        || { echo "FAIL: $name Machine field is not Digital VAX"; exit 1; }

    # readelf -l: NO PT_INTERP -- a static ELF requests no runtime linker.
    PHDRS="$("$READELF" -l "$path")"
    echo "$PHDRS" | grep -qiE 'INTERP' \
        && { echo "FAIL: $name has a PT_INTERP -- not statically linked (vms-0ab)"; exit 1; }
    echo "$PHDRS" | grep -qiF 'IMGACT.EXE' \
        && { echo "FAIL: $name requests IMGACT.EXE -- OVMX-native activation must NOT be used on netbsd-vax"; exit 1; }

    # readelf -d: NO DT_NEEDED / no dynamic section -- a fully static exe carries
    # no shared-object dependency for ld.elf_so to resolve at activation.
    DYN="$("$READELF" -d "$path" 2>/dev/null || true)"
    echo "$DYN" | grep -qiE 'NEEDED' \
        && { echo "FAIL: $name has a DT_NEEDED -- not a genuinely static exe (vms-0ab)"; exit 1; }

    # no OVMX symbol-vector sections -- not an IMGACT image on this substrate.
    SECS="$("$READELF" -S "$path")"
    if echo "$SECS" | grep -qE '\.vms\$(sv|imp)'; then
        echo "FAIL: $name carries OVMX symbol-vector sections (.vms\$sv/.vms\$imp)"
        exit 1
    fi

    echo "  -> $name: ELF32 Digital VAX, STATICALLY linked, no PT_INTERP, no DT_NEEDED, no .vms\$sv/.vms\$imp"
    echo
done

echo "=== ALL PROOFS PASSED: the full OVMX boot image set (STARTUP.EXE, PROVISION.EXE, DCL.EXE, JOB_CONTROL.EXE, LOGINOUT.EXE) builds, links STATICALLY (no ld.elf_so re-relocation per activation), and satisfies the Decision-A static contract for $TARGET ==="
