#!/bin/bash
# test_native_acp_arm_defined.sh - vms-656 (V0.5-2 boot blocker regression guard).
#
# THE BUG THIS GUARDS. rms_core.c / rms_io.c / rms_search.c reach SYS$DISK
# through the executive Files-11 ODS-2 ACP under `#if defined(OVMX_HAVE_ACP)`;
# the `#else` arm is a POSIX vmsfs_to_linux_path fallback that CANNOT see a file
# living only on the genuine ODS-2 volume. These arms were keyed on __linux__
# until vms-d5d re-keyed them to OVMX_HAVE_ACP (so the netbsd-vax cross, where
# __linux__ is undefined, gets them too). The native-link recipes build the
# SHIPPED shareables; mk_dcl.sh and mk_libvms_shr.sh were given -DOVMX_HAVE_ACP
# at that point, but mk_vmsrms_shr.sh was MISSED -- so the shipped x86_64
# LIBVMSRMS$SHR.EXE silently fell to the POSIX branch and STARTUP.COM's OPEN of
# SYS$STARTUP:VMS$PHASES.DAT failed %RMS-E-FNF, hanging x86_64 boot before
# Username:. The Debug ctest build (CMake, which sets OVMX_HAVE_ACP
# unconditionally) stayed green throughout -- only the native-link shipped image
# was affected, which is why a unit test could not catch it. This script does.
#
# INVARIANT: every native-link recipe (src/vmslink/mk_*.sh) that COMPILES a
# source file carrying an `#if defined(OVMX_HAVE_ACP)` arm must itself define
# -DOVMX_HAVE_ACP (recipes that only --use a shareable which does are exempt --
# they do not compile the guarded TU). No allowlist: fix the recipe, do not
# annotate around it.

set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC="$ROOT/src"
MKDIR="$SRC/vmslink"

FAIL=0

# 1. Enumerate the source files that carry an OVMX_HAVE_ACP arm.
mapfile -t ACP_FILES < <(grep -rlE '#if[[:space:]]+defined\(OVMX_HAVE_ACP\)|#ifdef[[:space:]]+OVMX_HAVE_ACP' \
    "$SRC" --include=*.c 2>/dev/null | sort -u)

if [ "${#ACP_FILES[@]}" -eq 0 ]; then
    echo "FAIL: no OVMX_HAVE_ACP-guarded .c files found -- did the guard move?"
    exit 1
fi
echo "OVMX_HAVE_ACP-guarded source files:"
for f in "${ACP_FILES[@]}"; do echo "  ${f#$ROOT/}"; done
echo ""

# 2. For each native-link recipe, if it COMPILES (cc/$CC ... file.c) one of
#    those TUs, it must define OVMX_HAVE_ACP somewhere in the recipe.
for mk in "$MKDIR"/mk_*.sh; do
    name=$(basename "$mk")
    compiles_guarded=""
    for f in "${ACP_FILES[@]}"; do
        base=$(basename "$f" .c)
        # The recipe compiles this TU if it names "<base>" in a compile LIST or
        # a direct "$CC ... <base>.c" line (mk_*_shr.sh use a LIST loop). Ignore
        # comment lines -- a recipe that only MENTIONS a TU it --uses from a
        # shareable (e.g. mk_loginout.sh's rms_textfile note) does not compile it.
        if grep -vE '^[[:space:]]*#' "$mk" \
             | grep -qE "(^|[^a-zA-Z0-9_])${base}([^a-zA-Z0-9_]|\$)" 2>/dev/null &&
           grep -qE '\$CC[[:space:]].*-c([[:space:]]|$)' "$mk" 2>/dev/null; then
            compiles_guarded="$compiles_guarded $base"
        fi
    done
    [ -z "$compiles_guarded" ] && continue

    # Look for the actual compile flag on a NON-comment line (a bare mention in
    # an explanatory comment does not put -DOVMX_HAVE_ACP on the cc line).
    if grep -vE '^[[:space:]]*#' "$mk" | grep -qE '(^|[^A-Za-z0-9_])-DOVMX_HAVE_ACP([^A-Za-z0-9_]|$)'; then
        echo "OK: $name compiles [${compiles_guarded# }] and defines OVMX_HAVE_ACP"
    else
        echo "FAIL: $name compiles OVMX_HAVE_ACP-guarded TU(s) [${compiles_guarded# }]"
        echo "      but does NOT define -DOVMX_HAVE_ACP -- the shipped shareable"
        echo "      will take the POSIX #else fallback and cannot read the ODS-2 volume."
        FAIL=1
    fi
done

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "PASS: every native-link recipe that compiles an ACP arm defines OVMX_HAVE_ACP"
    exit 0
fi
echo "FAILED: a native-link recipe compiles an ACP arm without OVMX_HAVE_ACP (vms-656)"
exit 1
