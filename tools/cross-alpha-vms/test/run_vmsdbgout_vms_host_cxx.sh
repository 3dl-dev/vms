#!/bin/bash
# run_vmsdbgout_vms_host_cxx.sh -- GCC's VMS-host vms_file_stats_name
# (gcc/vmsdbgout.cc, the #ifdef VMS block: FAB/NAM, $PARSE/$SEARCH, $ASSIGN,
# $QIOW IO$_ACCESS with an ATR list) compiles as C++ with the alpha-dec-vms g++
# over the OVMX STARLET surface (vms-fd1, patches/0012). Unpatched, C++ rejects
# its (void *) descriptor initializers and the const char * it hands
# to_vms_file_spec -- cc1 cannot be built VMS-hosted. Run where the stage-2
# toolchain is (the cxx gates); the patched block must compile and the
# unpatched one must not (the control).
# Usage: bash run_vmsdbgout_vms_host_cxx.sh <tools/cross-alpha-vms dir> <g++> [flags...]
set -euo pipefail
TC=$1; GXX=$2; shift 2
W=$(mktemp -d)
tar -C "$W" -xf "$TC/gcc-14.2.0.tar.xz" gcc-14.2.0/gcc/vmsdbgout.cc
mkdir -p "$W/a/gcc" "$W/b/gcc"
cp "$W/gcc-14.2.0/gcc/vmsdbgout.cc" "$W/a/gcc/"
cp "$W/gcc-14.2.0/gcc/vmsdbgout.cc" "$W/b/gcc/"
for p in "$TC"/patches/*.patch; do
    grep -qE '^\+\+\+ b/gcc/vmsdbgout\.cc' "$p" || continue
    patch -s -p1 -d "$W/b" < "$p"
    # the control keeps every vmsdbgout patch except the one under test
    case "$p" in *0012-*) ;; *) patch -s -p1 -d "$W/a" < "$p" ;; esac
done
# The VMS-host block: from the `#ifdef VMS` that opens the STARLET includes
# through the closing brace of vms_file_stats_name.
extract() {
    awk '/^#ifdef VMS$/ { hold=$0; next }
         hold != "" && /^#define __NEW_STARLET 1$/ { on=1; print hold }
         { hold="" }
         on { print }
         on && /^vms_file_stats_name/ { fn=1 }
         on && fn && /^}$/ { exit }' "$1"
}
for v in a b; do
    { printf '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
      printf '#define ATTRIBUTE_UNUSED __attribute__ ((unused))\n'
      extract "$W/$v/gcc/vmsdbgout.cc"; } > "$W/$v.cc"
    grep -q 'SYS\$QIOW' "$W/$v.cc" || { echo "  FAIL: could not extract the VMS-host block"; exit 1; }
done
rc=0
if "$GXX" "$@" -DVMS -fsyntax-only -x c++ "$W/b.cc" 2>"$W/b.err"; then
    echo "  PASS patched vms_file_stats_name compiles as C++ (VMS host, OVMX STARLET surface)"
else
    echo "  FAIL patched vms_file_stats_name:"; sed 's/^/      /' "$W/b.err" | head -20; rc=1
fi
if "$GXX" "$@" -DVMS -fsyntax-only -x c++ "$W/a.cc" 2>/dev/null; then
    echo "  FAIL control: unpatched vms_file_stats_name compiled, so this test proves nothing"; rc=1
else
    echo "  PASS control: unpatched vms_file_stats_name is rejected by C++"
fi
rm -rf "$W"
[ "$rc" -eq 0 ] && echo "ALL VMSDBGOUT VMS-HOST C++ (vms-fd1) CHECKS PASSED"
exit $rc
