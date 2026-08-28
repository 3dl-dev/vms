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
#
# vms-10c4: the SAME drift is possible "one directory over" -- the hand-rolled
# netbsd-vax cross recipes (tools/cross-vax/build-*-vax.sh) also compile some
# of these guarded TUs directly with $CC ... -c, and a recipe that dropped its
# -DOVMX_HAVE_ACP would silently take the same POSIX fallback cross-compiled
# for VAX. So the second loop below applies the identical check to
# tools/cross-vax/build-*-vax.sh. NOT in scope: the CMake-driven cross-vax
# builds (e.g. build-vmsdcl-vax.sh drives `cmake --build`) -- those inherit
# OVMX_HAVE_ACP from the CMakeLists' unconditional target_compile_definitions,
# so a hand-rolled -D can't be dropped there; they have no `$CC ... -c` line
# of their own and so are naturally skipped by the same detection below.

set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC="$ROOT/src"
MKDIR="$SRC/vmslink"
CROSSVAXDIR="$ROOT/tools/cross-vax"

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

# 2. For each recipe in a given directory, if it COMPILES (cc/$CC ... file.c)
#    one of those TUs, it must define OVMX_HAVE_ACP somewhere in the recipe.
#    Shared by both loops below (native-link mk_*.sh and cross-vax build-*.sh)
#    so the detection idiom -- including the process-substitution fix -- stays
#    in exactly one place.
check_recipe() {
    local mk="$1" label="$2"
    local name compiles_guarded f base
    name=$(basename "$mk")
    compiles_guarded=""
    for f in "${ACP_FILES[@]}"; do
        base=$(basename "$f" .c)
        # The recipe compiles this TU if it names "<base>" in a compile LIST or
        # a direct "$CC ... <base>.c" line (mk_*_shr.sh / cross-vax scripts use
        # a LIST loop). Ignore comment lines -- a recipe that only MENTIONS a TU
        # it --uses from a shareable (e.g. mk_loginout.sh's rms_textfile note,
        # or build-boot-images-vax.sh invoking another script) does not compile
        # it.
        #
        # Feed the comment-stripped text via process substitution, NOT a pipe:
        # under `set -o pipefail` a `strip | grep -q` pipeline reports the whole
        # pipeline failed when the downstream `grep -q` matches early and closes
        # the pipe, killing the upstream `grep -v` with SIGPIPE (141). That is
        # buffering/grep-implementation dependent -- it passes with a
        # block-buffered GNU grep on the dev host but bites with the line-buffered
        # busybox/musl grep in the CI container, a false positive on the largest
        # recipe (mk_libvms_shr.sh). Process substitution keeps the guard's
        # PASS/FAIL keyed only on the matcher's own exit status (vms-f60d).
        # \$CC"?[[:space:]] matches both the mk_*.sh unquoted invocation
        # ($CC $CFLAGS ... -c ...) and the cross-vax quoted invocation
        # ("$CC" $CFLAGS ... -c ...).
        if grep -qE "(^|[^a-zA-Z0-9_])${base}([^a-zA-Z0-9_]|\$)" \
             <(grep -vE '^[[:space:]]*#' "$mk") 2>/dev/null &&
           grep -qE '\$CC"?[[:space:]].*-c([[:space:]]|$)' "$mk" 2>/dev/null; then
            compiles_guarded="$compiles_guarded $base"
        fi
    done
    [ -z "$compiles_guarded" ] && return 0

    # Look for the actual compile flag on a NON-comment line (a bare mention in
    # an explanatory comment does not put -DOVMX_HAVE_ACP on the cc line).
    # Process substitution, not a pipe -- see the SIGPIPE/pipefail note above.
    if grep -qE '(^|[^A-Za-z0-9_])-DOVMX_HAVE_ACP([^A-Za-z0-9_]|$)' \
         <(grep -vE '^[[:space:]]*#' "$mk"); then
        echo "OK: $name compiles [${compiles_guarded# }] and defines OVMX_HAVE_ACP"
        return 0
    fi

    echo "FAIL: $label $name compiles OVMX_HAVE_ACP-guarded TU(s) [${compiles_guarded# }]"
    echo "      but does NOT define -DOVMX_HAVE_ACP -- the shipped image"
    echo "      will take the POSIX #else fallback and cannot read the ODS-2 volume."
    return 1
}

for mk in "$MKDIR"/mk_*.sh; do
    check_recipe "$mk" "native-link recipe" || FAIL=1
done

# 3. Same invariant, same check, over the hand-rolled netbsd-vax cross recipes
#    (tools/cross-vax/build-*-vax.sh) -- vms-10c4. These are a separate build
#    path from src/vmslink/mk_*.sh and were not covered by loop 2 above, so a
#    dropped -DOVMX_HAVE_ACP here would recur unguarded "one directory over".
for mk in "$CROSSVAXDIR"/build-*-vax.sh; do
    check_recipe "$mk" "cross-vax recipe" || FAIL=1
done

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "PASS: every native-link and cross-vax recipe that compiles an ACP arm defines OVMX_HAVE_ACP"
    exit 0
fi
echo "FAILED: a native-link or cross-vax recipe compiles an ACP arm without OVMX_HAVE_ACP (vms-656 / vms-10c4)"
exit 1
