#!/bin/bash
# run_gcov_tool_nftw.sh -- an alpha-dec-vms client sees nftw exactly as the
# DEC C RTL provides it (not at all), and GCC's gcov-tool builds on such a host
# (vms-fd1, patches/0011 + musl-arch/filter-headers.pl).
#
# 1. filter-headers.pl hides every prototype DECC$SHR does not export, those
#    with a callback parameter included (nftw, qsort_r, tsearch ...), while a
#    function-pointer-RETURNING declaration (signal) is still read by name.
# 2. gcov-tool.cc's <ftw.h> use, against that filtered header as a VMS client
#    sees it: patched (HAVE_FTW_H but no HAVE_NFTW) compiles; unpatched is
#    rejected for the undeclared nftw. The patched configure checks for nftw
#    and config.in carries HAVE_NFTW.
# Usage: bash run_gcov_tool_nftw.sh <tools/cross-alpha-vms dir>
set -euo pipefail
TC=${1:-$(cd "$(dirname "$0")/.." && pwd)}
CXX=${CXX:-c++}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
rc=0
pass() { echo "  PASS $*"; }
fail() { echo "  FAIL $*"; rc=1; }

# --- 1. the header filter ---------------------------------------------------
tar -C "$W" -xzf "$TC/musl-arch/musl-1.2.5.tar.gz" musl-1.2.5/include
INC="$W/musl-1.2.5/include"
perl "$TC/musl-arch/filter-headers.pl" "$TC/musl-arch/decc-crtl-names.txt" "$INC" > /dev/null
guarded() {   # is the prototype of $2 in header $1 inside a vms-fe03 guard?
    awk -v n="$2" '/vms-fe03: not in DECC/ {g=1; next} /^#endif/ {g=0}
        $0 ~ "[ *]" n "[ ]*\\(" { print (g ? "yes" : "no"); exit }' "$INC/$1"
}
for hn in ftw.h:nftw stdlib.h:qsort_r search.h:tsearch dirent.h:scandir; do
    h=${hn%%:*}; n=${hn##*:}
    [ "$(guarded "$h" "$n")" = yes ] && pass "<$h> $n (callback parameter, not in DECC\$SHR) is hidden from clients" \
        || fail "<$h> $n is still declared to alpha-dec-vms clients"
done
for hn in ftw.h:ftw stdlib.h:qsort stdlib.h:bsearch; do
    h=${hn%%:*}; n=${hn##*:}
    [ "$(guarded "$h" "$n")" = no ] && pass "<$h> $n (a DEC C RTL function) stays declared" \
        || fail "<$h> $n was hidden though DECC\$SHR exports it"
done
grep -qE '^void \(\*signal\(' "$INC/signal.h" && ! grep -B1 -E '^void \(\*signal\(' "$INC/signal.h" | grep -q vms-fe03 \
    && pass "<signal.h> signal (returns a function pointer) is not misread and stays declared" \
    || fail "<signal.h> signal was misread by the filter"

# --- 2. gcov-tool.cc against that header --------------------------------------
tar -C "$W" -xf "$TC/gcc-14.2.0.tar.xz" gcc-14.2.0/gcc/gcov-tool.cc \
    gcc-14.2.0/gcc/configure gcc-14.2.0/gcc/configure.ac gcc-14.2.0/gcc/config.in
G="$W/gcc-14.2.0/gcc"
cp "$G/gcov-tool.cc" "$W/gcov-tool.unpatched.cc"
for p in "$TC"/patches/*.patch; do
    grep -qE '^\+\+\+ b/gcc/(gcov-tool\.cc|configure|config\.in)$' "$p" || continue
    patch -s -p1 -d "$W/gcc-14.2.0" < "$p"
done
grep -qE '^	clearerr_unlocked .* getauxval nftw$' "$G/configure" && pass "configure checks for the nftw function" \
    || fail "configure does not check for nftw"
grep -q '^#undef HAVE_NFTW$' "$G/config.in" && pass "config.in carries HAVE_NFTW" || fail "config.in lacks HAVE_NFTW"
# The <ftw.h> part of gcov-tool.cc (its include through unlink_profile_dir),
# compiled as configure leaves it on a VMS host: HAVE_FTW_H 1, HAVE_NFTW unset.
region() {
    { cat <<'H'
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#define HAVE_FTW_H 1
#define ATTRIBUTE_UNUSED __attribute__((unused))
#define GCOV_DATA_SUFFIX ".gcda"
static int input_location;
static void fatal_error (int, const char *, ...) {}
H
      sed -n '/^#if HAVE_FTW_H/,/^#endif/p' "$1" | head -3
      sed -n '/^static bool verbose;/,/^unlink_profile_dir/p' "$1"
      sed -n '/^unlink_profile_dir/,/^}/p' "$1" | tail -n +2
      echo 'int main () { return unlink_profile_dir (".") == -1 ? 0 : 1; }'
    } > "$2"
}
region "$G/gcov-tool.cc" "$W/patched.cc"
region "$W/gcov-tool.unpatched.cc" "$W/unpatched.cc"
# Only <ftw.h> comes from the filtered OVMX set; the rest is the host's own.
mkdir -p "$W/vmsinc" && cp "$INC/ftw.h" "$W/vmsinc/"
FLAGS="-fsyntax-only -D__VMS -I$W/vmsinc"
if $CXX $FLAGS "$W/patched.cc" 2> "$W/patched.err"; then
    pass "patched gcov-tool.cc <ftw.h> code compiles on a host with ftw but no nftw"
else
    fail "patched gcov-tool.cc:"; sed 's/^/      /' "$W/patched.err" | head -10
fi
if $CXX $FLAGS "$W/unpatched.cc" 2> "$W/unpatched.err"; then
    fail "control: unpatched gcov-tool.cc compiled, so this test proves nothing"
elif grep -q nftw "$W/unpatched.err"; then
    pass "control: unpatched gcov-tool.cc is rejected (nftw not declared)"
else
    fail "control: unpatched rejected for another reason:"; sed 's/^/      /' "$W/unpatched.err" | head -5
fi
[ "$rc" -eq 0 ] && echo "ALL GCOV-TOOL NFTW (vms-fd1) CHECKS PASSED"
exit $rc
